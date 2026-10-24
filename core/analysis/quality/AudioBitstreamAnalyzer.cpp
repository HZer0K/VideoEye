#include "core/analysis/quality/AudioBitstreamAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
}

namespace videoeye {
namespace {

std::string CodecTagText(uint32_t tag) {
    if (tag == 0)
        return std::string();
    char text[5] = {0};
    text[0] = static_cast<char>((tag >> 24) & 0xFF);
    text[1] = static_cast<char>((tag >> 16) & 0xFF);
    text[2] = static_cast<char>((tag >> 8) & 0xFF);
    text[3] = static_cast<char>(tag & 0xFF);
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c > 0x7E)
            return std::string();
    }
    return std::string(text, 4);
}

std::string DescribeChannelLayout(const AVCodecParameters* par) {
    if (par == nullptr)
        return std::string();
    char buffer[128] = {0};
    if (par->ch_layout.nb_channels > 0 && av_channel_layout_describe(&par->ch_layout, buffer, sizeof(buffer)) >= 0) {
        return std::string(buffer);
    }
    return std::string();
}

} // namespace

void AudioBitstreamAnalyzer::Reset(const AudioBitstreamOptions& options) {
    options_ = options;
    result_ = model::AudioBitstreamResult{};
    states_.clear();
    registered_ = false;
    finished_ = false;
}

void AudioBitstreamAnalyzer::RegisterStreams(const AVFormatContext* fmt) {
    if (fmt == nullptr)
        return;
    states_.clear();
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        if (stream == nullptr || stream->codecpar == nullptr)
            continue;
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        StreamState state;
        state.parsed_limit = options_.max_packets_per_stream;
        model::AudioBitstreamStream& info = state.info;
        info.stream_index = static_cast<int>(i);
        const char* codec_name = avcodec_get_name(stream->codecpar->codec_id);
        info.codec_name = codec_name != nullptr ? codec_name : "";
        info.codec_tag = CodecTagText(stream->codecpar->codec_tag);
        info.container_sample_rate = stream->codecpar->sample_rate;
        info.container_channels = stream->codecpar->ch_layout.nb_channels;
        info.container_channel_layout = DescribeChannelLayout(stream->codecpar);

        switch (stream->codecpar->codec_id) {
        case AV_CODEC_ID_AAC: {
            AacAudioSpecificConfig asc;
            if (stream->codecpar->extradata != nullptr && stream->codecpar->extradata_size > 0 &&
                AudioBitstreamParser::ParseAacAudioSpecificConfig(
                    stream->codecpar->extradata, static_cast<size_t>(stream->codecpar->extradata_size), asc)) {
                info.codec = model::AudioBitstreamCodec::AacRaw;
                state.asc = asc;
                state.asc_ready = true;
                ApplyAacConfig(state, asc, model::AudioBitstreamCodec::AacRaw);
            } else {
                info.codec = model::AudioBitstreamCodec::AacAdts;
            }
            break;
        }
        case AV_CODEC_ID_AAC_LATM:
            info.codec = model::AudioBitstreamCodec::AacLatm;
            break;
        case AV_CODEC_ID_AC3:
            info.codec = model::AudioBitstreamCodec::Ac3;
            info.profile = "AC-3";
            break;
        case AV_CODEC_ID_EAC3:
            info.codec = model::AudioBitstreamCodec::Eac3;
            info.profile = "E-AC-3";
            break;
        case AV_CODEC_ID_OPUS: {
            info.codec = model::AudioBitstreamCodec::Opus;
            info.profile = "Opus";
            OpusHead head;
            if (stream->codecpar->extradata != nullptr && stream->codecpar->extradata_size > 0 &&
                AudioBitstreamParser::ParseOpusHead(stream->codecpar->extradata,
                                                    static_cast<size_t>(stream->codecpar->extradata_size), head)) {
                ApplyOpusHead(state, head);
            }
            break;
        }
        case AV_CODEC_ID_MP3:
            info.codec = model::AudioBitstreamCodec::Mp3;
            // profile 留到首个帧头解析出来再填（Layer I/II/III 只有帧头才知道）
            break;
        case AV_CODEC_ID_FLAC: {
            info.codec = model::AudioBitstreamCodec::Flac;
            info.profile = "FLAC";
            // MP4 的 dfLa / Matroska 的私有数据里带 STREAMINFO；
            // 裸 .flac 的 extradata 通常为空，改由首包开头的 "fLaC" 补
            FlacStreamInfo flac;
            if (stream->codecpar->extradata != nullptr && stream->codecpar->extradata_size > 0 &&
                AudioBitstreamParser::ParseFlacStreamInfo(
                    stream->codecpar->extradata, static_cast<size_t>(stream->codecpar->extradata_size), flac)) {
                state.flac_info = flac;
                state.flac_info_ready = true;
                ApplyFlacStreamInfo(state, flac);
            }
            break;
        }
        default:
            info.codec = model::AudioBitstreamCodec::Other;
            info.note = "该编码未实现逐帧位流头解析（只做容器参数登记）";
            break;
        }

        states_.push_back(std::move(state));
    }
    registered_ = true;
    result_.analyzed = !states_.empty();
    if (states_.empty()) {
        result_.notes.push_back("没有音频流，跳过音频位流参数分析");
    }
}

AudioBitstreamAnalyzer::StreamState* AudioBitstreamAnalyzer::FindState(int stream_index) {
    for (StreamState& state : states_) {
        if (state.info.stream_index == stream_index)
            return &state;
    }
    return nullptr;
}

void AudioBitstreamAnalyzer::AddInconsistency(StreamState& state, const std::string& text) {
    if (text.empty())
        return;
    for (const std::string& existing : state.info.inconsistencies) {
        if (existing == text)
            return;
    }
    state.info.inconsistencies.push_back(text);
}

void AudioBitstreamAnalyzer::StreamState::ObserveFrame(int sample_rate, int channels, int frame_bytes,
                                                       int audio_object_type, int acmod, int bit_rate_kbps) {
    if (sample_rate <= 0 && channels <= 0 && frame_bytes <= 0)
        return;
    if (!has_header) {
        has_header = true;
        last_sample_rate = sample_rate;
        last_channels = channels;
        last_frame_bytes = frame_bytes;
        last_audio_object_type = audio_object_type;
        last_acmod = acmod;
        last_bit_rate_kbps = bit_rate_kbps;
        return;
    }

    if (sample_rate > 0 && last_sample_rate > 0 && sample_rate != last_sample_rate) {
        info.params_changed_midstream = true;
        if (!reported_sample_rate_change) {
            reported_sample_rate_change = true;
            info.inconsistencies.push_back("位流采样率在扫描中从 " + std::to_string(last_sample_rate) + " Hz 变为 " +
                                           std::to_string(sample_rate) + " Hz（可能是拼接/中途转码）");
        }
    }
    if (channels > 0 && last_channels > 0 && channels != last_channels) {
        info.params_changed_midstream = true;
        if (!reported_channel_change) {
            reported_channel_change = true;
            info.inconsistencies.push_back("位流声道数在扫描中从 " + std::to_string(last_channels) + " 变为 " +
                                           std::to_string(channels) + "（可能是拼接/中途转码）");
        }
    }
    if (frame_bytes > 0 && last_frame_bytes > 0) {
        const double ratio = static_cast<double>(frame_bytes) / static_cast<double>(last_frame_bytes);
        if (ratio > 1.05 || ratio < 0.95) {
            ++frame_size_changes;
            if (frame_size_changes > 4)
                info.vbr = true;
        }
    }
    if (bit_rate_kbps > 0 && last_bit_rate_kbps > 0 && std::abs(bit_rate_kbps - last_bit_rate_kbps) > 8) {
        info.params_changed_midstream = true;
        if (!reported_bitrate_change) {
            reported_bitrate_change = true;
            info.inconsistencies.push_back("位流码率在扫描中从 " + std::to_string(last_bit_rate_kbps) + " kbps 变为 " +
                                           std::to_string(bit_rate_kbps) + " kbps");
        }
    }

    if (sample_rate > 0)
        last_sample_rate = sample_rate;
    if (channels > 0)
        last_channels = channels;
    if (frame_bytes > 0)
        last_frame_bytes = frame_bytes;
    if (audio_object_type >= 0)
        last_audio_object_type = audio_object_type;
    if (acmod >= 0)
        last_acmod = acmod;
    if (bit_rate_kbps > 0)
        last_bit_rate_kbps = bit_rate_kbps;
}

void AudioBitstreamAnalyzer::ApplyAacConfig(StreamState& state, const AacAudioSpecificConfig& asc,
                                            model::AudioBitstreamCodec codec) {
    state.info.codec = codec;
    if (asc.sample_rate > 0)
        state.info.sample_rate = asc.sample_rate;
    if (asc.channels > 0)
        state.info.channels = asc.channels;
    state.info.channel_config = asc.channel_configuration;
    state.info.audio_object_type = asc.audio_object_type;
    state.info.profile = model::AudioObjectTypeName(asc.audio_object_type);
    // SBR/PS：sample_rate 存的是**输出**采样率（ApplyAacConfig 里 asc.sample_rate
    // 已经换算过），core 值单独留一份，报告里能把"core 24 kHz -> 输出 48 kHz"讲清楚。
    state.info.aac_sbr = asc.sbr;
    state.info.aac_ps = asc.ps;
    state.info.aac_core_sample_rate = asc.core_sample_rate;
    state.info.aac_output_sample_rate = asc.sample_rate;
    state.info.aac_sbr_source = asc.sbr_source;
    if (asc.sbr || asc.ps) {
        std::string ext = asc.sbr ? "SBR" : std::string();
        if (asc.ps)
            ext += (ext.empty() ? "" : "+") + std::string("PS");
        std::string detail = "AAC 扩展已识别：" + ext;
        if (asc.core_sample_rate > 0 && asc.sample_rate > asc.core_sample_rate)
            detail += "（core " + std::to_string(asc.core_sample_rate) + " Hz -> 输出 " +
                      std::to_string(asc.sample_rate) + " Hz）";
        if (asc.ps)
            detail += "；PS 把单声道上混成立体声，声道数按 2 计";
        detail += asc.sbr_from_sync_extension ? "；来源：AudioSpecificConfig 同步扩展(0x2B7)"
                                              : "；来源：AudioSpecificConfig 的 AOT";
        state.info.note = (state.info.note.empty() ? std::string() : state.info.note + "；") + detail;
    } else if (!asc.sbr_signaled) {
        // 没有显式信令 ≠ 没有 SBR：HE-AAC 允许 implicit signaling，
        // 真正的 SBR 标志在 raw_data_block 的 ID_FIL 扩展元素里，本工具未解析。
        state.info.note = (state.info.note.empty() ? std::string() : state.info.note + "；") +
                          "未找到 SBR/PS 显式信令（implicit signaling 需解析码流内 ID_FIL 扩展，本工具未做）";
    }
    if (!asc.note.empty())
        state.info.note = (state.info.note.empty() ? asc.note : state.info.note + "；" + asc.note);
}

void AudioBitstreamAnalyzer::ApplyLatmConfig(StreamState& state, const LatmHeader& header) {
    state.info.codec = model::AudioBitstreamCodec::AacLatm;
    if (header.sample_rate > 0)
        state.info.sample_rate = header.sample_rate;
    if (header.channels > 0)
        state.info.channels = header.channels;
    state.info.channel_config = header.channel_configuration;
    state.info.audio_object_type = header.audio_object_type;
    state.info.profile = model::AudioObjectTypeName(header.audio_object_type);
    if (!header.note.empty())
        state.info.note = header.note;
}

void AudioBitstreamAnalyzer::ApplyAc3Header(StreamState& state, const Ac3Header& header) {
    model::AudioBitstreamStream& info = state.info;
    if (header.eac3) {
        info.codec = model::AudioBitstreamCodec::Eac3;
        info.profile = "E-AC-3";
    } else {
        info.codec = model::AudioBitstreamCodec::Ac3;
        info.profile = "AC-3";
    }
    info.eac3 = header.eac3;
    info.bsid = header.bsid;
    info.bsmod = header.bsmod;
    if (header.sample_rate > 0)
        info.sample_rate = header.sample_rate;
    info.bit_rate_kbps = header.bit_rate_kbps;
    if (header.has_dialnorm) {
        info.has_dialnorm = true;
        info.dialnorm_dbfs = header.dialnorm_dbfs;
    }

    if (header.eac3 && header.strmtyp == 1) {
        // 从属子流只补充声道；它没有独立的节目参数（dialnorm/acmod 属于独立子流）。
        bool seen = false;
        for (int id : state.dependent_substream_ids) {
            if (id == header.substreamid) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            state.dependent_substream_ids.push_back(header.substreamid);
            ++info.dependent_substream_count;
            if (header.has_chanmap) {
                info.dependent_channels += header.chanmap_channels;
            }
        }
        return;
    }

    if (header.acmod >= 0) {
        info.acmod = header.acmod;
        info.lfeon = header.lfeon;
    }
    if (header.channels > 0)
        info.channels = header.channels;
    if (header.atmos_joc) {
        if (!info.atmos_joc && options_.detect_atmos) {
            // 措辞必须体现"启发式"：这是线索，不是扩展字段解析出来的结论。
            info.atmos_source = header.atmos_heuristic
                                    ? "E-AC-3 载荷内 ASCII \"joc\" 标记（启发式线索，非 addbsi 字段解析）"
                                    : "E-AC-3 BSI (joc marker)";
        }
        info.atmos_joc = info.atmos_joc || options_.detect_atmos;
        info.atmos_heuristic = info.atmos_heuristic || header.atmos_heuristic;
        info.joc_complexity_index = header.joc_complexity_index;
    }
}

void AudioBitstreamAnalyzer::ApplyOpusHead(StreamState& state, const OpusHead& head) {
    model::AudioBitstreamStream& info = state.info;
    info.codec = model::AudioBitstreamCodec::Opus;
    info.opus_head = true;
    info.channels = head.channels;
    info.pre_skip = head.pre_skip;
    info.input_sample_rate = head.input_sample_rate;
    info.output_gain_q8 = head.output_gain_q8;
    info.mapping_family = head.mapping_family;
    info.stream_count = head.stream_count;
    info.coupled_count = head.coupled_count;
    // Opus 解码输出固定 48 kHz；input_sample_rate 只是原始采样率提示。
    info.sample_rate = 48000;
    info.profile = "Opus";
}

void AudioBitstreamAnalyzer::ApplyOpusToc(StreamState& state, const OpusToc& toc) {
    model::AudioBitstreamStream& info = state.info;
    info.codec = model::AudioBitstreamCodec::Opus;
    if (info.sample_rate <= 0)
        info.sample_rate = 48000;
    if (toc.channels > 0 && info.channels <= 0)
        info.channels = toc.channels;
    info.opus_mode = toc.mode;
    info.opus_bandwidth = toc.bandwidth;
    info.opus_frame_ms = toc.frame_duration_ms;
}

void AudioBitstreamAnalyzer::ParseAacPacket(StreamState& state, const AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    const size_t size = static_cast<size_t>(pkt->size);
    if (data == nullptr || size == 0)
        return;

    AdtsHeader adts;
    const bool adts_ok = AudioBitstreamParser::ParseAdts(data, size, adts);
    if (adts_ok || adts.truncated) {
        state.info.codec = model::AudioBitstreamCodec::AacAdts;
        // 同步字对上了、头字段也读完了，但声明帧长超过本包剩余字节 = 包被截断。
        // 这**不是**解析成功：以前落到下面的"裸 AAC"分支，或直接被算进
        // parsed_packets，把损坏样本记成了好样本。
        if (!adts_ok) {
            ++state.info.invalid_packets;
            if (state.info.note.empty()) {
                state.info.note = "ADTS 声明帧长超过包剩余字节（截断包），不计入解析成功";
            }
            return;
        }
        size_t offset = 0;
        int frames = 0;
        bool truncated_tail = false;
        while (offset + 7 <= size && frames < 64) {
            AdtsHeader header;
            if (!AudioBitstreamParser::ParseAdts(data + offset, size - offset, header)) {
                // 后续帧截断：前面的帧已经统计过，这里只记一笔，不能把整包算坏
                if (header.truncated)
                    truncated_tail = true;
                break;
            }
            if (!state.has_header) {
                state.info.sample_rate = header.sample_rate;
                state.info.channels = header.channels;
                state.info.channel_config = header.channel_configuration;
                state.info.audio_object_type = header.audio_object_type;
                state.info.profile = model::AudioObjectTypeName(header.audio_object_type);
            }
            // channel_configuration == 0 表示"声道布局写在 PCE 里"。以前这一支
            // channels 恒为 0，容器声道数因此失去了对照物，且 Finish() 里要专门
            // 抑制"容器 vs 位流声道不一致"告警 —— 等于这条流不校验声道。
            // PCE 只在首个 raw_data_block 里，试一次即可。
            if (header.channel_configuration == 0 && !state.aac_pce_attempted) {
                state.aac_pce_attempted = true;
                const size_t rdb_off = offset + static_cast<size_t>(header.header_bytes);
                if (rdb_off < size) {
                    AacProgramConfig pce;
                    if (AudioBitstreamParser::ParseAacProgramConfig(data + rdb_off, size - rdb_off, pce)) {
                        ApplyAacPce(state, pce);
                    } else if (!pce.note.empty() && state.info.note.empty()) {
                        state.info.note = "ADTS 声明 PCE 声道配置，但未能解析：" + pce.note;
                    }
                }
            }
            state.ObserveFrame(header.sample_rate, header.channels, header.frame_length, header.audio_object_type, -1,
                               0);
            state.info.frame_count += header.num_raw_data_blocks + 1;
            ++frames;
            if (header.frame_length <= 0)
                break;
            offset += static_cast<size_t>(header.frame_length);
        }
        if (frames > 0) {
            ++state.info.parsed_packets;
            if (truncated_tail && state.info.note.empty()) {
                state.info.note = "包尾有一帧被截断（已统计到截断点之前的帧）";
            }
        } else {
            ++state.info.invalid_packets;
        }
        return;
    }

    int mux_length = 0;
    bool same_mux = false;
    if (AudioBitstreamParser::ParseLoasHeader(data, size, mux_length, same_mux)) {
        state.info.codec = model::AudioBitstreamCodec::AacLatm;
        bool parsed = false;
        if (!same_mux && size > 3) {
            LatmHeader header;
            if (AudioBitstreamParser::ParseLatmStreamMuxConfig(data + 3, size - 3, header)) {
                state.latm_config = header;
                state.latm_config_ready = true;
                ApplyLatmConfig(state, header);
                state.ObserveFrame(header.sample_rate, header.channels, mux_length, header.audio_object_type, -1, 0);
                parsed = true;
            }
        } else if (same_mux && state.latm_config_ready) {
            state.ObserveFrame(state.latm_config.sample_rate, state.latm_config.channels, mux_length,
                               state.latm_config.audio_object_type, -1, 0);
            parsed = true;
        }
        if (parsed) {
            ++state.info.parsed_packets;
            // 一个 AudioMuxElement 里含 numSubFrames+1 个 subFrame，每个 subFrame
            // 是一个 AAC 原始数据块（= 一帧）。以前不看这个字段、每包记 1 帧，
            // 多 subFrame 的流帧数按倍数少算，连带把时长与平均码率都算错。
            const int sub = state.latm_config.num_sub_frames;
            state.info.frame_count += (sub > 0 ? sub + 1 : 1);
            if (sub > 0)
                state.info.latm_num_sub_frames = sub;
        } else {
            ++state.info.invalid_packets;
        }
        return;
    }

    // MP4 / MOV 的裸 AAC：没有逐帧同步字，参数来自 extradata 的 AudioSpecificConfig。
    state.info.codec = model::AudioBitstreamCodec::AacRaw;
    if (state.asc_ready) {
        state.ObserveFrame(state.asc.sample_rate, state.asc.channels, pkt->size, state.asc.audio_object_type, -1, 0);
        ++state.info.parsed_packets;
        ++state.info.frame_count;
    } else {
        ++state.info.invalid_packets;
        if (state.info.note.empty()) {
            state.info.note = "裸 AAC 且没有可解析的 AudioSpecificConfig，无法确认位流参数";
        }
    }
}

void AudioBitstreamAnalyzer::ParseLatmPacket(StreamState& state, const AVPacket* pkt) {
    ParseAacPacket(state, pkt);
}

void AudioBitstreamAnalyzer::ParseAc3Packet(StreamState& state, const AVPacket* pkt, bool force_eac3) {
    const uint8_t* data = pkt->data;
    const size_t size = static_cast<size_t>(pkt->size);
    if (data == nullptr || size < 6) {
        ++state.info.invalid_packets;
        return;
    }

    size_t offset = 0;
    int frames = 0;
    bool truncated_tail = false;
    while (offset + 6 <= size && frames < 64) {
        Ac3Header header;
        if (!AudioBitstreamParser::ParseAc3(data + offset, size - offset, force_eac3, header)) {
            break;
        }
        if (header.frame_size_bytes <= 0)
            break;
        // 与 ADTS 同一类问题：帧头声明了帧长，但包里的剩余字节不够。
        // 以前直接 ObserveFrame + parsed_packets++，截断包被记成解析成功。
        if (static_cast<size_t>(header.frame_size_bytes) > size - offset) {
            truncated_tail = true;
            break;
        }
        ApplyAc3Header(state, header);
        state.ObserveFrame(header.sample_rate, header.channels, header.frame_size_bytes, -1, header.acmod,
                           header.bit_rate_kbps);
        state.info.frame_count += 1;
        ++frames;
        offset += static_cast<size_t>(header.frame_size_bytes);
    }
    if (frames > 0) {
        ++state.info.parsed_packets;
        if (truncated_tail && state.info.note.empty()) {
            state.info.note = "AC-3 包尾帧声明长度超过剩余字节（截断）";
        }
    } else {
        ++state.info.invalid_packets;
    }
}

void AudioBitstreamAnalyzer::ParseOpusPacket(StreamState& state, const AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    const size_t size = static_cast<size_t>(pkt->size);
    if (data == nullptr || size == 0) {
        ++state.info.invalid_packets;
        return;
    }
    if (size >= 8 && std::memcmp(data, "OpusHead", 8) == 0) {
        OpusHead head;
        if (AudioBitstreamParser::ParseOpusHead(data, size, head)) {
            ApplyOpusHead(state, head);
            ++state.info.parsed_packets;
            return;
        }
    }

    OpusToc toc;
    if (!AudioBitstreamParser::ParseOpusToc(data, size, toc)) {
        ++state.info.invalid_packets;
        return;
    }
    ApplyOpusToc(state, toc);
    state.ObserveFrame(48000, toc.channels, pkt->size, -1, -1, 0);
    state.info.frame_count += toc.frame_count;
    ++state.info.parsed_packets;
}

void AudioBitstreamAnalyzer::ApplyMp3Header(StreamState& state, const Mp3FrameHeader& hdr) {
    model::AudioBitstreamStream& info = state.info;
    info.codec = model::AudioBitstreamCodec::Mp3;
    info.mpeg_version = hdr.mpeg_version;
    info.mp3_layer = hdr.layer;
    info.channel_mode = hdr.channel_mode;
    info.crc_present = hdr.crc_present;
    if (info.sample_rate <= 0)
        info.sample_rate = hdr.sample_rate;
    if (info.channels <= 0)
        info.channels = hdr.channels;
    if (info.bit_rate_kbps <= 0)
        info.bit_rate_kbps = hdr.bitrate_kbps;
    if (info.profile.empty()) {
        std::string profile = "MPEG-";
        profile += (hdr.mpeg_version == 1) ? "1" : (hdr.mpeg_version == 2 ? "2" : "2.5");
        profile += " Layer ";
        profile += static_cast<char>('0' + hdr.layer);
        info.profile = profile;
    }
}

void AudioBitstreamAnalyzer::ParseMp3Packet(StreamState& state, const AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    const size_t size = static_cast<size_t>(pkt->size);
    if (data == nullptr || size < 4) {
        ++state.info.invalid_packets;
        return;
    }

    // 一包可能含多帧（AVI/TS 里常见），也可能只含一帧的一部分（截断的末包）。
    size_t offset = 0;
    int frames = 0;
    bool truncated_tail = false;
    bool free_format_seen = false;
    while (offset + 4 <= size && frames < 64) {
        Mp3FrameHeader hdr;
        if (!AudioBitstreamParser::ParseMp3FrameHeader(data + offset, size - offset, hdr)) {
            if (hdr.truncated)
                truncated_tail = true;
            if (hdr.free_format) {
                free_format_seen = true;
                if (state.info.note.empty()) {
                    state.info.note = "MP3 使用 free format（帧头未写码率），本实现无法逐帧定界，"
                                      "只统计到首个可解析帧；这些包计为「未支持」而非「坏帧」";
                }
            }
            break;
        }
        if (!state.has_header) {
            ApplyMp3Header(state, hdr);
            // Xing/Info/VBRI 只在首帧，且只解析一次
            if (!state.mp3_vbr_parsed) {
                state.mp3_vbr_parsed = true;
                Mp3VbrInfo vbr;
                if (AudioBitstreamParser::ParseMp3VbrInfo(data + offset, size - offset, hdr, vbr)) {
                    state.info.vbr_tag = vbr.tag;
                    state.info.declared_frame_count = vbr.frame_count;
                    state.info.declared_stream_bytes = vbr.stream_bytes;
                    state.info.has_encoder_delay = vbr.has_encoder_delay;
                    state.info.encoder_delay_samples = vbr.encoder_delay;
                    state.info.encoder_padding_samples = vbr.encoder_padding;
                    state.info.encoder = vbr.encoder;
                }
            }
        }
        state.ObserveFrame(hdr.sample_rate, hdr.channels, hdr.frame_bytes, -1, -1, hdr.bitrate_kbps);
        ++state.info.frame_count;
        ++frames;
        if (hdr.frame_bytes <= 0)
            break;
        offset += static_cast<size_t>(hdr.frame_bytes);
    }

    if (frames > 0) {
        ++state.info.parsed_packets;
        if (truncated_tail && state.info.note.empty()) {
            state.info.note = "包尾有一帧被截断（已统计到截断点之前的帧）";
        }
    } else if (free_format_seen) {
        // ❗ 不能计 invalid：free format 是合法编码，只是帧长算不出来。
        // 混进 invalid_packets 会让"全部帧头校验失败"的假告警出现在正常文件上，
        // 也会让位流维度被误判为 Partial（真实原因是不支持，不是素材坏了）。
        ++state.info.unsupported_packets;
    } else {
        ++state.info.invalid_packets;
        if (state.info.note.empty())
            state.info.note = "MP3 帧头同步字校验失败（不是 MPEG Audio 帧或包起点不在帧边界）";
    }
}

void AudioBitstreamAnalyzer::ApplyFlacStreamInfo(StreamState& state, const FlacStreamInfo& info) {
    model::AudioBitstreamStream& s = state.info;
    s.codec = model::AudioBitstreamCodec::Flac;
    s.profile = "FLAC";
    s.flac_streaminfo = true;
    s.streaminfo_min_block = info.min_block_size;
    s.streaminfo_max_block = info.max_block_size;
    s.declared_total_samples = info.total_samples;
    if (info.sample_rate > 0)
        s.sample_rate = info.sample_rate;
    if (info.channels > 0)
        s.channels = info.channels;
    // FLAC 的"位深"不是采样格式，而是 STREAMINFO 里的有效位深；容器侧没有对应字段，
    // 所以不走 container_mismatch，只在 UI/报告里单独展示。
    // min/max block size 相差过大（超过 4 倍）通常意味着分段编码或流式拼接。
    if (info.min_block_size > 0 && info.max_block_size > info.min_block_size * 4)
        s.flac_high_variance_blocks = true;
}

void AudioBitstreamAnalyzer::ParseFlacPacket(StreamState& state, const AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    const size_t size = static_cast<size_t>(pkt->size);
    if (data == nullptr || size < 2) {
        ++state.info.invalid_packets;
        return;
    }

    // 裸 .flac 的第一包可能还带着 "fLaC" + metadata blocks：先补 STREAMINFO，
    // 这种包不含音频帧，不能算"帧头解析失败"。
    if (!state.flac_info_ready && size >= 8 && std::memcmp(data, "fLaC", 4) == 0) {
        FlacStreamInfo info;
        if (AudioBitstreamParser::ParseFlacStreamInfo(data, size, info)) {
            state.flac_info = info;
            state.flac_info_ready = true;
            ApplyFlacStreamInfo(state, info);
            state.info.note = "STREAMINFO 取自码流开头的 fLaC 头（该包是元数据，不含音频帧）";
        }
        return;
    }

    size_t offset = 0;
    int frames = 0;
    bool crc_bad = false;
    while (offset + 6 <= size && frames < 64) {
        FlacFrameHeader hdr;
        if (!AudioBitstreamParser::ParseFlacFrameHeader(data + offset, size - offset, hdr)) {
            if (hdr.truncated && state.info.note.empty())
                state.info.note = "FLAC 帧头被截断（包尾不足）";
            break;
        }
        if (!state.flac_frame_seen) {
            state.flac_frame_seen = true;
            if (!state.flac_info_ready) {
                // 没有 STREAMINFO：只把帧头里明确写了的字段当事实，
                // 编码值为 0（"沿用它处"）时留空，绝不填默认值。
                if (hdr.sample_rate > 0)
                    state.info.sample_rate = hdr.sample_rate;
                if (hdr.channels > 0)
                    state.info.channels = hdr.channels;
            }
        }
        if (hdr.crc_checked) {
            state.info.flac_frame_crc_checked = true;
            if (!hdr.crc_ok) {
                crc_bad = true;
                ++state.info.flac_frame_crc_failures;
            }
        }
        // FLAC 帧时长 = block_size / sample_rate；ObserveFrame 只需要帧字节做 VBR 启发，
        // 采样率/声道取帧头（为 0 时回落到已知值）。帧字节这里给 0：FLAC 是纯 VBR，
        // 用"帧长摆动"判 VBR 没有意义（恒为真）。
        const int sr = hdr.sample_rate > 0 ? hdr.sample_rate : state.info.sample_rate;
        state.ObserveFrame(sr, hdr.channels, 0, -1, -1, 0);
        ++state.info.frame_count;
        ++frames;
        // FLAC 帧的结束位置只能靠逐帧解析算出来；本层不做完整帧同步扫描，
        // 所以一包多帧时只看首帧，避免把后继字节当成新帧头误判。
        break;
    }

    if (frames > 0) {
        ++state.info.parsed_packets;
        if (crc_bad && state.info.note.empty())
            state.info.note = "FLAC 帧头 CRC-8 校验失败（帧头损坏）";
    } else {
        ++state.info.invalid_packets;
        if (state.info.note.empty())
            state.info.note = "FLAC 帧同步字校验失败（不是 FLAC 帧或包起点不在帧边界）";
    }
}

void AudioBitstreamAnalyzer::ApplyAacPce(StreamState& state, const AacProgramConfig& pce) {
    if (!pce.valid)
        return;
    // PCE 只在 channel_configuration == 0 时出现，此时 ADTS/ASC 给不出声道布局，
    // 容器声明的声道数也就失去了对照物。PCE 是目前唯一的一手证据。
    state.info.channels = pce.channels;
    state.info.channel_config = 0;
    if (state.info.sample_rate <= 0)
        state.info.sample_rate = pce.sample_rate;
    // 结构化落一份：报告里"声道布局取自 PCE"不能只是 note 里的一句话，
    // 否则下游无法判断 channels 是码流事实还是容器抄来的。
    state.info.aac_pce = true;
    state.info.pce_object_type = pce.object_type;
    state.info.pce_sample_rate = pce.sample_rate;
    state.info.pce_channels = pce.channels;
    state.info.pce_front = pce.front_channels;
    state.info.pce_side = pce.side_channels;
    state.info.pce_back = pce.back_channels;
    state.info.pce_lfe = pce.lfe_channels;
    state.info.pce_layout = pce.layout;
    state.info.note = (state.info.note.empty() ? "" : state.info.note + "；") + "声道布局取自 PCE: " + pce.layout +
                      "（" + std::to_string(pce.channels) + " 声道）";
}

void AudioBitstreamAnalyzer::OnPacket(const AVPacket* pkt, const AVStream* stream) {
    if (pkt == nullptr || stream == nullptr || stream->codecpar == nullptr)
        return;
    StreamState* state = FindState(pkt->stream_index);
    if (state == nullptr)
        return;

    ++state->info.packet_count;
    if (state->parsed_limit > 0 &&
        state->info.judged_packets() + state->info.unsupported_packets >= state->parsed_limit) {
        return;
    }

    switch (stream->codecpar->codec_id) {
    case AV_CODEC_ID_AAC:
        ParseAacPacket(*state, pkt);
        break;
    case AV_CODEC_ID_AAC_LATM:
        ParseLatmPacket(*state, pkt);
        break;
    case AV_CODEC_ID_AC3:
        ParseAc3Packet(*state, pkt, false);
        break;
    case AV_CODEC_ID_EAC3:
        ParseAc3Packet(*state, pkt, true);
        break;
    case AV_CODEC_ID_OPUS:
        ParseOpusPacket(*state, pkt);
        break;
    case AV_CODEC_ID_MP3:
        ParseMp3Packet(*state, pkt);
        break;
    case AV_CODEC_ID_FLAC:
        ParseFlacPacket(*state, pkt);
        break;
    default:
        // 未实现逐帧解析的编码（ALAC / DTS / TrueHD / PCM ...）：只登记"未支持"，
        // 绝不能记进 invalid_packets —— 那是"素材损坏"的口径。
        ++state->info.unsupported_packets;
        break;
    }
}

const model::AudioBitstreamResult& AudioBitstreamAnalyzer::Finish() {
    if (finished_)
        return result_;
    finished_ = true;

    result_.streams.clear();
    result_.parsed_stream_count = 0;
    result_.atmos_stream_count = 0;

    for (StreamState& state : states_) {
        model::AudioBitstreamStream& info = state.info;

        if (info.codec == model::AudioBitstreamCodec::Eac3) {
            if (info.dependent_channels > 0) {
                info.channels = state.last_channels > 0 ? state.last_channels : info.channels;
                info.channels += info.dependent_channels;
                info.note = (info.note.empty() ? "" : info.note + "；") + "E-AC-3 从属子流 +" +
                            std::to_string(info.dependent_channels) + " 声道，总声道数 " +
                            std::to_string(info.channels);
            }
        }

        if (info.packet_count == 0) {
            info.note = "扫描期间没有收到该音频流的包";
        } else if (info.judged_packets() == 0) {
            // 未实现逐帧解析：这是"工具还没做"，不是"素材坏了"，文案必须说清楚。
            info.note = (info.note.empty() ? "" : info.note + "；") + "该编码尚未实现逐帧位流头解析，" +
                        std::to_string(info.unsupported_packets) + " 个包未参与帧头判定";
        } else if (info.parsed_packets == 0) {
            info.note = (info.note.empty() ? "" : info.note + "；") + "没有解析出可用的音频帧头";
        } else if (info.invalid_packets > 0) {
            info.note = (info.note.empty() ? "" : info.note + "；") + std::to_string(info.invalid_packets) +
                        " 个包帧头解析失败";
        }

        // Atmos 的判定方式是启发式的：必须在结论旁自带免责说明，
        // 否则下游（报告/UI）会把它当确定事实。
        if (info.atmos_joc && info.atmos_heuristic) {
            info.note = (info.note.empty() ? "" : info.note + "；") +
                        "Atmos/JOC 仅为启发式线索（未按扩展语法解析 addbsi 字段），不能作为确定结论";
        }

        // ---- 位流声明 vs 容器声明 ----
        if (info.container_sample_rate > 0 && info.sample_rate > 0 && info.container_sample_rate != info.sample_rate) {
            info.container_mismatch = true;
            ++info.container_mismatch_fields;
            AddInconsistency(state, "容器声明采样率 " + std::to_string(info.container_sample_rate) + " Hz，位流头是 " +
                                        std::to_string(info.sample_rate) + " Hz");
        }
        if (info.container_channels > 0 && info.channels > 0 && info.container_channels != info.channels) {
            // PCE 没解出来时 channels 才是 0 —— 那种情况下面这个条件根本不会进来。
            // 所以这里不再需要为 "aac_pce" 开口子：一旦 PCE 解析成功，
            // 位流声道数就是一手证据，容器对不上就必须报。
            const bool eac3_unresolved_dependent =
                (info.codec == model::AudioBitstreamCodec::Eac3 && info.dependent_substream_count == 0 &&
                 info.container_channels > info.channels);
            if (!eac3_unresolved_dependent) {
                info.container_mismatch = true;
                ++info.container_mismatch_fields;
                AddInconsistency(state, "容器声明 " + std::to_string(info.container_channels) + " 声道，位流头是 " +
                                            std::to_string(info.channels) + " 声道");
            } else {
                info.note = (info.note.empty() ? "" : info.note + "；") +
                            "E-AC-3 容器声道数高于核心子流，可能有未解析到的从属子流";
            }
        }
        // ---- MP3：头部声明的帧数/字节数 vs 实测 ----
        // Xing/Info/VBRI 的总帧数是**声明值**。扫描完整文件时两者必须吻合；
        // 差得多说明文件被拼接/截断，或者头部是别的编码器留下的。
        if (info.codec == model::AudioBitstreamCodec::Mp3 && info.declared_frame_count > 0 && info.frame_count > 0) {
            const int64_t delta = info.frame_count - info.declared_frame_count;
            const int64_t magnitude = delta < 0 ? -delta : delta;
            // 容差 1 帧：编码器写 Xing 时通常不含首帧自身或含一个空帧
            if (magnitude > 1) {
                info.container_mismatch = true;
                ++info.container_mismatch_fields;
                AddInconsistency(state, "MP3 头部（" + info.vbr_tag + "）声明 " +
                                            std::to_string(info.declared_frame_count) + " 帧，实测 " +
                                            std::to_string(info.frame_count) + " 帧（差 " + std::to_string(delta) +
                                            "）");
            }
        }
        if (info.codec == model::AudioBitstreamCodec::Flac) {
            if (!info.flac_streaminfo) {
                info.note = (info.note.empty() ? "" : info.note + "；") +
                            "没有拿到 STREAMINFO（容器未携带且码流头未解析），"
                            "采样率/位深/总样本数只能依赖帧头声明";
            } else if (info.declared_total_samples == 0) {
                info.note = (info.note.empty() ? "" : info.note + "；") +
                            "STREAMINFO 未声明总样本数（流式编码），无法用声明值校验时长";
            }
            if (info.flac_high_variance_blocks) {
                info.note = (info.note.empty() ? "" : info.note + "；") +
                            "STREAMINFO 的 min/max block size 相差超过 4 倍，可能是分段编码或拼接";
            }
        }
        if (info.codec == model::AudioBitstreamCodec::Ac3 || info.codec == model::AudioBitstreamCodec::Eac3) {
            if (!info.has_dialnorm) {
                info.note = (info.note.empty() ? "" : info.note + "；") + "AC-3/E-AC-3 头里没有 dialnorm";
            }
        }

        if (info.parsed_packets > 0)
            ++result_.parsed_stream_count;
        if (info.atmos_joc)
            ++result_.atmos_stream_count;
        result_.streams.push_back(std::move(info));
    }

    if (!states_.empty() && result_.parsed_stream_count == 0) {
        bool any_judged = false;
        bool any_unsupported = false;
        for (const StreamState& state : states_) {
            if (state.info.judged_packets() > 0)
                any_judged = true;
            if (state.info.unsupported_packets > 0)
                any_unsupported = true;
        }
        if (any_judged) {
            result_.notes.push_back("音频流存在，但没有任何一条解析出位流头");
        } else if (any_unsupported) {
            // 全是未实现解析的编码：只登记容器参数，不能报成"解析失败"。
            result_.notes.push_back("音频流存在，但均为尚未实现逐帧位流头解析的编码（只做容器参数登记）");
        }
    }
    return result_;
}

} // namespace videoeye
