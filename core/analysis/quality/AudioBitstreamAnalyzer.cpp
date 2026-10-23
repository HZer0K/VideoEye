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
    if (tag == 0) return std::string();
    char text[5] = {0};
    text[0] = static_cast<char>((tag >> 24) & 0xFF);
    text[1] = static_cast<char>((tag >> 16) & 0xFF);
    text[2] = static_cast<char>((tag >> 8) & 0xFF);
    text[3] = static_cast<char>(tag & 0xFF);
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c > 0x7E) return std::string();
    }
    return std::string(text, 4);
}

std::string DescribeChannelLayout(const AVCodecParameters* par) {
    if (par == nullptr) return std::string();
    char buffer[128] = {0};
    if (par->ch_layout.nb_channels > 0 &&
        av_channel_layout_describe(&par->ch_layout, buffer, sizeof(buffer)) >= 0) {
        return std::string(buffer);
    }
    return std::string();
}

}  // namespace

void AudioBitstreamAnalyzer::Reset(const AudioBitstreamOptions& options) {
    options_ = options;
    result_ = model::AudioBitstreamResult{};
    states_.clear();
    registered_ = false;
    finished_ = false;
}

void AudioBitstreamAnalyzer::RegisterStreams(const AVFormatContext* fmt) {
    if (fmt == nullptr) return;
    states_.clear();
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        if (stream == nullptr || stream->codecpar == nullptr) continue;
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;

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
                if (stream->codecpar->extradata != nullptr &&
                    stream->codecpar->extradata_size > 0 &&
                    AudioBitstreamParser::ParseAacAudioSpecificConfig(
                        stream->codecpar->extradata,
                        static_cast<size_t>(stream->codecpar->extradata_size), asc)) {
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
                if (stream->codecpar->extradata != nullptr &&
                    stream->codecpar->extradata_size > 0 &&
                    AudioBitstreamParser::ParseOpusHead(
                        stream->codecpar->extradata,
                        static_cast<size_t>(stream->codecpar->extradata_size), head)) {
                    ApplyOpusHead(state, head);
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
        if (state.info.stream_index == stream_index) return &state;
    }
    return nullptr;
}

void AudioBitstreamAnalyzer::AddInconsistency(StreamState& state, const std::string& text) {
    if (text.empty()) return;
    for (const std::string& existing : state.info.inconsistencies) {
        if (existing == text) return;
    }
    state.info.inconsistencies.push_back(text);
}

void AudioBitstreamAnalyzer::StreamState::ObserveFrame(int sample_rate, int channels,
                                                       int frame_bytes, int audio_object_type,
                                                       int acmod, int bit_rate_kbps) {
    if (sample_rate <= 0 && channels <= 0 && frame_bytes <= 0) return;
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
            info.inconsistencies.push_back(
                "位流采样率在扫描中从 " + std::to_string(last_sample_rate) + " Hz 变为 " +
                std::to_string(sample_rate) + " Hz（可能是拼接/中途转码）");
        }
    }
    if (channels > 0 && last_channels > 0 && channels != last_channels) {
        info.params_changed_midstream = true;
        if (!reported_channel_change) {
            reported_channel_change = true;
            info.inconsistencies.push_back(
                "位流声道数在扫描中从 " + std::to_string(last_channels) + " 变为 " +
                std::to_string(channels) + "（可能是拼接/中途转码）");
        }
    }
    if (frame_bytes > 0 && last_frame_bytes > 0) {
        const double ratio = static_cast<double>(frame_bytes) /
                             static_cast<double>(last_frame_bytes);
        if (ratio > 1.05 || ratio < 0.95) {
            ++frame_size_changes;
            if (frame_size_changes > 4) info.vbr = true;
        }
    }
    if (bit_rate_kbps > 0 && last_bit_rate_kbps > 0 &&
        std::abs(bit_rate_kbps - last_bit_rate_kbps) > 8) {
        info.params_changed_midstream = true;
        if (!reported_bitrate_change) {
            reported_bitrate_change = true;
            info.inconsistencies.push_back(
                "位流码率在扫描中从 " + std::to_string(last_bit_rate_kbps) + " kbps 变为 " +
                std::to_string(bit_rate_kbps) + " kbps");
        }
    }

    if (sample_rate > 0) last_sample_rate = sample_rate;
    if (channels > 0) last_channels = channels;
    if (frame_bytes > 0) last_frame_bytes = frame_bytes;
    if (audio_object_type >= 0) last_audio_object_type = audio_object_type;
    if (acmod >= 0) last_acmod = acmod;
    if (bit_rate_kbps > 0) last_bit_rate_kbps = bit_rate_kbps;
}

void AudioBitstreamAnalyzer::ApplyAacConfig(StreamState& state,
                                            const AacAudioSpecificConfig& asc,
                                            model::AudioBitstreamCodec codec) {
    state.info.codec = codec;
    if (asc.sample_rate > 0) state.info.sample_rate = asc.sample_rate;
    if (asc.channels > 0) state.info.channels = asc.channels;
    state.info.channel_config = asc.channel_configuration;
    state.info.audio_object_type = asc.audio_object_type;
    state.info.profile = model::AudioObjectTypeName(asc.audio_object_type);
    if (!asc.note.empty()) state.info.note = asc.note;
}

void AudioBitstreamAnalyzer::ApplyLatmConfig(StreamState& state, const LatmHeader& header) {
    state.info.codec = model::AudioBitstreamCodec::AacLatm;
    if (header.sample_rate > 0) state.info.sample_rate = header.sample_rate;
    if (header.channels > 0) state.info.channels = header.channels;
    state.info.channel_config = header.channel_configuration;
    state.info.audio_object_type = header.audio_object_type;
    state.info.profile = model::AudioObjectTypeName(header.audio_object_type);
    if (!header.note.empty()) state.info.note = header.note;
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
    if (header.sample_rate > 0) info.sample_rate = header.sample_rate;
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
    if (header.channels > 0) info.channels = header.channels;
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
    if (info.sample_rate <= 0) info.sample_rate = 48000;
    if (toc.channels > 0 && info.channels <= 0) info.channels = toc.channels;
    info.opus_mode = toc.mode;
    info.opus_bandwidth = toc.bandwidth;
    info.opus_frame_ms = toc.frame_duration_ms;
}

void AudioBitstreamAnalyzer::ParseAacPacket(StreamState& state, const AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    const size_t size = static_cast<size_t>(pkt->size);
    if (data == nullptr || size == 0) return;

    AdtsHeader adts;
    if (AudioBitstreamParser::ParseAdts(data, size, adts)) {
        state.info.codec = model::AudioBitstreamCodec::AacAdts;
        size_t offset = 0;
        int frames = 0;
        while (offset + 7 <= size && frames < 64) {
            AdtsHeader header;
            if (!AudioBitstreamParser::ParseAdts(data + offset, size - offset, header)) break;
            if (!state.has_header) {
                state.info.sample_rate = header.sample_rate;
                state.info.channels = header.channels;
                state.info.channel_config = header.channel_configuration;
                state.info.audio_object_type = header.audio_object_type;
                state.info.profile = model::AudioObjectTypeName(header.audio_object_type);
            }
            state.ObserveFrame(header.sample_rate, header.channels, header.frame_length,
                               header.audio_object_type, -1, 0);
            state.info.frame_count += header.num_raw_data_blocks + 1;
            ++frames;
            if (header.frame_length <= 0) break;
            offset += static_cast<size_t>(header.frame_length);
        }
        if (frames > 0) {
            ++state.info.parsed_packets;
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
                state.ObserveFrame(header.sample_rate, header.channels, mux_length,
                                   header.audio_object_type, -1, 0);
                parsed = true;
            }
        } else if (same_mux && state.latm_config_ready) {
            state.ObserveFrame(state.latm_config.sample_rate, state.latm_config.channels,
                               mux_length, state.latm_config.audio_object_type, -1, 0);
            parsed = true;
        }
        if (parsed) {
            ++state.info.parsed_packets;
            ++state.info.frame_count;
        } else {
            ++state.info.invalid_packets;
        }
        return;
    }

    // MP4 / MOV 的裸 AAC：没有逐帧同步字，参数来自 extradata 的 AudioSpecificConfig。
    state.info.codec = model::AudioBitstreamCodec::AacRaw;
    if (state.asc_ready) {
        state.ObserveFrame(state.asc.sample_rate, state.asc.channels, pkt->size,
                           state.asc.audio_object_type, -1, 0);
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

void AudioBitstreamAnalyzer::ParseAc3Packet(StreamState& state, const AVPacket* pkt,
                                            bool force_eac3) {
    const uint8_t* data = pkt->data;
    const size_t size = static_cast<size_t>(pkt->size);
    if (data == nullptr || size < 6) {
        ++state.info.invalid_packets;
        return;
    }

    size_t offset = 0;
    int frames = 0;
    while (offset + 6 <= size && frames < 64) {
        Ac3Header header;
        if (!AudioBitstreamParser::ParseAc3(data + offset, size - offset, force_eac3, header)) {
            break;
        }
        ApplyAc3Header(state, header);
        state.ObserveFrame(header.sample_rate, header.channels, header.frame_size_bytes,
                           -1, header.acmod, header.bit_rate_kbps);
        state.info.frame_count += 1;
        ++frames;
        if (header.frame_size_bytes <= 0) break;
        offset += static_cast<size_t>(header.frame_size_bytes);
    }
    if (frames > 0) {
        ++state.info.parsed_packets;
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

void AudioBitstreamAnalyzer::OnPacket(const AVPacket* pkt, const AVStream* stream) {
    if (pkt == nullptr || stream == nullptr || stream->codecpar == nullptr) return;
    StreamState* state = FindState(pkt->stream_index);
    if (state == nullptr) return;

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
        default:
            // 未实现逐帧解析的编码（MP3/FLAC/PCM ...）：只登记"未支持"，
            // 绝不能记进 invalid_packets —— 那是"素材损坏"的口径。
            ++state->info.unsupported_packets;
            break;
    }
}

const model::AudioBitstreamResult& AudioBitstreamAnalyzer::Finish() {
    if (finished_) return result_;
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
                info.note = (info.note.empty() ? "" : info.note + "；") +
                            "E-AC-3 从属子流 +" + std::to_string(info.dependent_channels) +
                            " 声道，总声道数 " + std::to_string(info.channels);
            }
        }

        if (info.packet_count == 0) {
            info.note = "扫描期间没有收到该音频流的包";
        } else if (info.judged_packets() == 0) {
            // 未实现逐帧解析：这是"工具还没做"，不是"素材坏了"，文案必须说清楚。
            info.note = (info.note.empty() ? "" : info.note + "；") + "该编码尚未实现逐帧位流头解析，" +
                        std::to_string(info.unsupported_packets) + " 个包未参与帧头判定";
        } else if (info.parsed_packets == 0) {
            info.note = (info.note.empty() ? "" : info.note + "；") +
                        "没有解析出可用的音频帧头";
        } else if (info.invalid_packets > 0) {
            info.note = (info.note.empty() ? "" : info.note + "；") +
                        std::to_string(info.invalid_packets) + " 个包帧头解析失败";
        }

        // Atmos 的判定方式是启发式的：必须在结论旁自带免责说明，
        // 否则下游（报告/UI）会把它当确定事实。
        if (info.atmos_joc && info.atmos_heuristic) {
            info.note = (info.note.empty() ? "" : info.note + "；") +
                        "Atmos/JOC 仅为启发式线索（未按扩展语法解析 addbsi 字段），不能作为确定结论";
        }

        // ---- 位流声明 vs 容器声明 ----
        if (info.container_sample_rate > 0 && info.sample_rate > 0 &&
            info.container_sample_rate != info.sample_rate) {
            info.container_mismatch = true;
            ++info.container_mismatch_fields;
            AddInconsistency(state,
                             "容器声明采样率 " + std::to_string(info.container_sample_rate) +
                                 " Hz，位流头是 " + std::to_string(info.sample_rate) + " Hz");
        }
        if (info.container_channels > 0 && info.channels > 0 &&
            info.container_channels != info.channels) {
            const bool aac_pce =
                (info.codec == model::AudioBitstreamCodec::AacAdts && info.channel_config == 0);
            const bool eac3_unresolved_dependent =
                (info.codec == model::AudioBitstreamCodec::Eac3 &&
                 info.dependent_substream_count == 0 &&
                 info.container_channels > info.channels);
            if (!aac_pce && !eac3_unresolved_dependent) {
                info.container_mismatch = true;
                ++info.container_mismatch_fields;
                AddInconsistency(state,
                                 "容器声明 " + std::to_string(info.container_channels) +
                                     " 声道，位流头是 " + std::to_string(info.channels) +
                                     " 声道");
            } else if (eac3_unresolved_dependent) {
                info.note = (info.note.empty() ? "" : info.note + "；") +
                            "E-AC-3 容器声道数高于核心子流，可能有未解析到的从属子流";
            }
        }
        if (info.codec == model::AudioBitstreamCodec::Ac3 ||
            info.codec == model::AudioBitstreamCodec::Eac3) {
            if (!info.has_dialnorm) {
                info.note = (info.note.empty() ? "" : info.note + "；") +
                            "AC-3/E-AC-3 头里没有 dialnorm";
            }
        }

        if (info.parsed_packets > 0) ++result_.parsed_stream_count;
        if (info.atmos_joc) ++result_.atmos_stream_count;
        result_.streams.push_back(std::move(info));
    }

    if (!states_.empty() && result_.parsed_stream_count == 0) {
        bool any_judged = false;
        bool any_unsupported = false;
        for (const StreamState& state : states_) {
            if (state.info.judged_packets() > 0) any_judged = true;
            if (state.info.unsupported_packets > 0) any_unsupported = true;
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

}  // namespace videoeye
