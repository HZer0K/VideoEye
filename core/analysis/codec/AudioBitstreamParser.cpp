#include "core/analysis/codec/AudioBitstreamParser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace videoeye {
namespace {

// 大端位读取器。音频头字段最多 33 bit，统一用 uint64_t 承接。
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {
    }

    bool Read(int bits, uint64_t& value) {
        if (bits < 0 || bits > 64)
            return false;
        if (bit_pos_ + static_cast<size_t>(bits) > size_ * 8)
            return false;
        uint64_t result = 0;
        for (int i = 0; i < bits; ++i) {
            const size_t bit = bit_pos_ + static_cast<size_t>(i);
            const uint8_t byte = data_[bit / 8];
            result = (result << 1) | ((byte >> (7 - (bit % 8))) & 1u);
        }
        bit_pos_ += static_cast<size_t>(bits);
        value = result;
        return true;
    }

    bool Read(int bits, uint32_t& value) {
        uint64_t wide = 0;
        if (!Read(bits, wide))
            return false;
        value = static_cast<uint32_t>(wide);
        return true;
    }

    bool Skip(int bits) {
        if (bits < 0)
            return false;
        if (bit_pos_ + static_cast<size_t>(bits) > size_ * 8)
            return false;
        bit_pos_ += static_cast<size_t>(bits);
        return true;
    }

    size_t BitsLeft() const {
        const size_t total = size_ * 8;
        return bit_pos_ <= total ? total - bit_pos_ : 0;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t bit_pos_ = 0;
};

constexpr int kAacSampleRates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0,
};

// AC-3 帧长表（单位：16-bit words），[frmsizecod][fscod]
constexpr uint16_t kAc3FrameSizeWords[38][3] = {
    {64, 69, 96},       {64, 70, 96},       {80, 87, 120},      {80, 88, 120},      {96, 104, 144},
    {96, 105, 144},     {112, 121, 168},    {112, 122, 168},    {128, 139, 192},    {128, 140, 192},
    {160, 174, 240},    {160, 175, 240},    {192, 208, 288},    {192, 209, 288},    {224, 243, 336},
    {224, 244, 336},    {256, 278, 384},    {256, 279, 384},    {320, 348, 480},    {320, 349, 480},
    {384, 417, 576},    {384, 418, 576},    {448, 487, 672},    {448, 488, 672},    {512, 557, 768},
    {512, 558, 768},    {640, 696, 960},    {640, 697, 960},    {768, 835, 1152},   {768, 836, 1152},
    {896, 975, 1344},   {896, 976, 1344},   {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
    {1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920},
};

constexpr int kAc3BitratesKbps[19] = {
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
};

constexpr int kEac3SampleRates[3] = {48000, 44100, 32000};
constexpr int kEac3SampleRates2[3] = {24000, 22050, 16000};
constexpr int kEac3Blocks[4] = {1, 2, 3, 6};

int AcmodChannels(int acmod) {
    static const int kChannels[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    if (acmod < 0 || acmod > 7)
        return 0;
    return kChannels[acmod];
}

int CountBits16(uint16_t value) {
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if ((value >> i) & 1u)
            ++count;
    }
    return count;
}

// 在 E-AC-3 载荷前若干字节里找 Dolby JOC 的 ASCII 标记。
//
// ❗ 这是**启发式线索，不是规范级解析**：JOC 的 addbsi 载荷格式不在公开的
// E-AC-3 基础规范里，这里只是在固定窗口内做子串搜索 —— 普通音频数据里恰好
// 出现 0x6A 0x6F 0x63 这三个字节就会命中（实测把 "joc" 塞进任意载荷位置即可
// 让非 Atmos 素材变成 Atmos）。因此命中只能标成"疑似"，由调用方按线索呈现，
// 绝不能当成确定结论。
bool FindJocMarker(const uint8_t* data, size_t size, int& complexity_index) {
    if (data == nullptr)
        return false;
    const size_t limit = std::min<size_t>(size, 96);
    for (size_t i = 0; i + 2 < limit; ++i) {
        const uint8_t a = static_cast<uint8_t>(data[i] | 0x20);
        const uint8_t b = static_cast<uint8_t>(data[i + 1] | 0x20);
        const uint8_t c = static_cast<uint8_t>(data[i + 2] | 0x20);
        if (a == 'j' && b == 'o' && c == 'c') {
            complexity_index = -1;
            if (i + 3 < limit) {
                const int value = data[i + 3];
                if (value >= 0 && value <= 31)
                    complexity_index = value;
            }
            return true;
        }
    }
    return false;
}

bool ParseAacStreamInfo(BitReader& br, LatmHeader& out) {
    uint32_t value = 0;
    if (!br.Read(5, value))
        return false;
    int aot = static_cast<int>(value);
    if (aot == 31) {
        if (!br.Read(6, value))
            return false;
        aot = 32 + static_cast<int>(value);
    }
    out.audio_object_type = aot;

    if (!br.Read(4, value))
        return false;
    const int sf_index = static_cast<int>(value);
    int sample_rate = AudioBitstreamParser::AacSampleRateFromIndex(sf_index);
    if (sf_index == 15) {
        if (!br.Read(24, value))
            return false;
        sample_rate = static_cast<int>(value);
    }
    out.sampling_frequency_index = sf_index;
    out.sample_rate = sample_rate;

    if (!br.Read(4, value))
        return false;
    out.channel_configuration = static_cast<int>(value);
    out.channels = AudioBitstreamParser::AacChannelsFromConfig(out.channel_configuration);

    // HE-AAC 的扩展采样率/PS 标志。基础 StreamInfo 里这几段是可选的，
    // 解析失败时不把整个配置判死：AOT/采样率/声道已经拿到了。
    if (aot == 5 || aot == 29) {
        if (br.Read(4, value)) {
            const int ext_index = static_cast<int>(value);
            int ext_rate = AudioBitstreamParser::AacSampleRateFromIndex(ext_index);
            if (ext_index == 15) {
                uint32_t explicit_rate = 0;
                if (br.Read(24, explicit_rate))
                    ext_rate = static_cast<int>(explicit_rate);
            }
            if (ext_rate > 0)
                out.sample_rate = ext_rate;
        }
        if (aot == 29)
            out.note = "HE-AACv2 (SBR+PS)";
        else
            out.note = "HE-AAC (SBR)";
    }

    if (aot == 22) {
        if (br.Read(4, value)) {
            out.channel_configuration = static_cast<int>(value);
            out.channels = AudioBitstreamParser::AacChannelsFromConfig(out.channel_configuration);
        }
    }
    return true;
}

// 跳过 GASpecificConfig()（ISO/IEC 14496-3 §4.4.1.1）里与 SBR 无关的字段，
// 目的是把读位游标送到 AudioSpecificConfig 末尾的同步扩展处。
//
// 为什么必须走位而不能直接找 0x2B7：这 11 位可能恰好出现在其它字段里，
// 位置错了会把随机数据读成"sbrPresentFlag=1"，比不判更糟。
//
// 只处理 channelConfiguration != 0 的情况：== 0 时要在此完整解析一个
// program_config_element()，而现有的 ParseAacProgramConfig 是给 raw_data_block
// 用的（开头带 5 bit id_syn_ele），语法不同不能复用。这类流本来就已经从 PCE
// 拿到了声道布局，少这一条 sync extension 影响有限 —— 但要把原因写进 note，
// 不能静默跳过。
bool SkipGaSpecificConfig(BitReader& br, int aot, int channel_configuration, std::string& note) {
    uint32_t value = 0;
    if (channel_configuration == 0) {
        note = "channelConfiguration=0（声道布局在 PCE 里），跳过后置 SBR/PS 同步扩展";
        return false;
    }
    if (!br.Read(1, value)) // frameLengthFlag
        return false;
    if (!br.Read(1, value)) // dependsOnCoreCoder
        return false;
    if (value != 0 && !br.Skip(14)) // coreCoderDelay
        return false;
    if (!br.Read(1, value)) // extensionFlag
        return false;
    const bool extension_flag = value != 0;
    if (aot == 6 || aot == 20) {
        if (!br.Skip(3)) // layerNr
            return false;
    }
    if (extension_flag) {
        if (aot == 22) {
            if (!br.Skip(5 + 11)) // numOfSubFrame + layer_length
                return false;
        }
        if (aot == 17 || aot == 19 || aot == 20 || aot == 23) {
            if (!br.Skip(3)) // aacSectionDataResilienceFlag 等三个
                return false;
        }
        if (!br.Skip(1)) // extensionFlag3
            return false;
    }
    return true;
}

// AudioSpecificConfig 末尾的 SBR/PS 同步扩展（ISO/IEC 14496-3 §1.6.2.1）：
//
//   syncExtensionType        11  固定 0x2B7
//   extensionAudioObjectType  5  固定 5（SBR）
//   sbrPresentFlag            1
//   if sbrPresentFlag:
//       extensionSamplingFrequencyIndex 4（15 时再 24 bit 显式值）
//       syncExtensionType     11  固定 0x548
//       psPresentFlag          1
//
// 返回值表示"是否读到了同步扩展"，与 sbrPresentFlag 的真假无关 ——
// 读到 sbrPresentFlag=0 是一个**明确结论**（这个文件就是没有 SBR），
// 与"根本没读到扩展"（不知道）必须区分。
bool ParseAacSyncExtension(BitReader& br, AacAudioSpecificConfig& out) {
    uint32_t value = 0;
    if (!br.Read(11, value) || value != 0x2B7)
        return false;
    if (!br.Read(5, value) || value != 5) // extensionAudioObjectType = SBR
        return false;
    uint32_t sbr_present = 0;
    if (!br.Read(1, sbr_present))
        return false;
    out.sbr_signaled = true;
    out.sbr_source = "sync_extension";
    out.sbr_from_sync_extension = true;
    if (sbr_present == 0) {
        // 明确声明没有 SBR：清掉之前可能从 AOT 推出来的判断
        out.sbr = false;
        out.ps = false;
        return true;
    }
    out.sbr = true;
    uint32_t ext_index = 0;
    if (!br.Read(4, ext_index))
        return true;
    int ext_rate = 0;
    if (ext_index == 15) {
        uint32_t explicit_rate = 0;
        if (br.Read(24, explicit_rate))
            ext_rate = static_cast<int>(explicit_rate);
    } else {
        ext_rate = AudioBitstreamParser::AacSampleRateFromIndex(static_cast<int>(ext_index));
    }
    if (ext_rate > 0)
        out.extension_sample_rate = ext_rate;
    // PS：再一条 11 bit syncExtensionType = 0x548
    uint32_t ps_sync = 0;
    if (br.Read(11, ps_sync) && ps_sync == 0x548) {
        uint32_t ps_present = 0;
        if (br.Read(1, ps_present)) {
            out.ps = (ps_present != 0);
            out.ps_from_sync_extension = true;
        }
    }
    return true;
}

} // namespace

int AudioBitstreamParser::AacSampleRateFromIndex(int index) {
    if (index < 0 || index > 15)
        return 0;
    return kAacSampleRates[index];
}

int AudioBitstreamParser::AacChannelsFromConfig(int channel_configuration) {
    // 0 = 由 PCE/GASpecificConfig 定义，头里读不出声道数。
    static const int kChannels[8] = {0, 1, 2, 3, 4, 5, 6, 8};
    if (channel_configuration < 0 || channel_configuration > 7)
        return 0;
    return kChannels[channel_configuration];
}

int AudioBitstreamParser::Ac3FrameSizeBytes(int fscod, int frmsizecod) {
    if (fscod < 0 || fscod > 2 || frmsizecod < 0 || frmsizecod > 37)
        return 0;
    return static_cast<int>(kAc3FrameSizeWords[frmsizecod][fscod]) * 2;
}

bool AudioBitstreamParser::ParseAdts(const uint8_t* data, size_t size, AdtsHeader& out) {
    out = AdtsHeader{};
    if (data == nullptr || size < 7)
        return false;
    if (data[0] != 0xFF || (data[1] & 0xF6) != 0xF0)
        return false;

    out.mpeg_version = ((data[1] >> 3) & 1u) ? 2 : 4;
    out.protection_absent = (data[1] & 1u) != 0;
    const int profile = (data[2] >> 6) & 0x03;
    out.audio_object_type = profile + 1;
    out.sampling_frequency_index = (data[2] >> 2) & 0x0F;
    // 索引 13/14 是保留值、15 是 escape（真实采样率写在 AudioSpecificConfig 里，
    // ADTS 头本身没有）。以前一律 AacSampleRateFromIndex → 0，却仍判 valid=true，
    // 于是一条"采样率 0"的畸形容器还能得到"解析成功"。
    if (out.sampling_frequency_index >= 13)
        return false;
    out.sample_rate = AacSampleRateFromIndex(out.sampling_frequency_index);
    if (out.sample_rate <= 0)
        return false;
    out.channel_configuration = ((data[2] & 0x01) << 2) | ((static_cast<int>(data[3]) >> 6) & 0x03);
    out.channels = AacChannelsFromConfig(out.channel_configuration);
    out.frame_length = ((static_cast<int>(data[3]) & 0x03) << 11) | (static_cast<int>(data[4]) << 3) |
                       ((static_cast<int>(data[5]) >> 5) & 0x07);
    out.num_raw_data_blocks = data[6] & 0x03;
    out.header_bytes = out.protection_absent ? 7 : 9;
    if (out.frame_length < out.header_bytes)
        return false;
    // 声明帧长超过本包剩余字节 = 包被截断。头字段全读对了，但这个包**不是**有效帧：
    // 用 truncated 单独报告，调用方不能把它记成 parsed_packets。
    if (static_cast<size_t>(out.frame_length) > size) {
        out.truncated = true;
        return false;
    }
    out.valid = true;
    return true;
}

bool AudioBitstreamParser::ParseLoasHeader(const uint8_t* data, size_t size, int& audio_mux_length_bytes,
                                           bool& use_same_stream_mux) {
    audio_mux_length_bytes = 0;
    use_same_stream_mux = false;
    if (data == nullptr || size < 4)
        return false;
    // syncword 11 bit = 0x2B7：0x56 0xE0..0xFF
    if (data[0] != 0x56 || (data[1] & 0xE0) != 0xE0)
        return false;
    audio_mux_length_bytes = ((static_cast<int>(data[1]) & 0x1F) << 8) | static_cast<int>(data[2]);
    use_same_stream_mux = (data[3] & 0x80) != 0;
    return audio_mux_length_bytes > 0;
}

bool AudioBitstreamParser::ParseLatmStreamMuxConfig(const uint8_t* data, size_t size, LatmHeader& out) {
    out = LatmHeader{};
    if (data == nullptr || size == 0)
        return false;
    BitReader br(data, size);
    uint32_t value = 0;
    if (!br.Read(1, value))
        return false;
    if (value != 0) {
        out.use_same_stream_mux = true;
        out.note = "useSameStreamMux=1（沿用上一帧配置）";
        return false;
    }

    uint32_t audio_mux_version = 0;
    if (!br.Read(1, audio_mux_version))
        return false;
    out.audio_mux_version = static_cast<int>(audio_mux_version);
    if (audio_mux_version == 1) {
        uint32_t version_a = 0;
        if (!br.Read(1, version_a))
            return false;
        if (version_a != 0) {
            out.note = "audioMuxVersionA=1，暂不支持";
            return false;
        }
        // latmGetValue(): 1 bit bytesForValue，随后 (bytesForValue+1) 字节
        uint32_t bytes_for_value = 0;
        if (!br.Read(1, bytes_for_value))
            return false;
        if (!br.Skip(static_cast<int>((bytes_for_value + 1) * 8)))
            return false;
    }

    uint32_t all_streams_same_time_framing = 0;
    if (!br.Read(1, all_streams_same_time_framing))
        return false;
    if (all_streams_same_time_framing == 0) {
        out.note = "allStreamsSameTimeFraming=0，暂不支持";
        return false;
    }
    if (!br.Read(6, value))
        return false;
    out.num_sub_frames = static_cast<int>(value);
    uint32_t num_program = 0;
    if (!br.Read(4, num_program))
        return false;

    bool got_first = false;
    for (uint32_t program = 0; program <= num_program; ++program) {
        uint32_t num_layer = 0;
        if (!br.Read(3, num_layer))
            return false;
        for (uint32_t layer = 0; layer <= num_layer; ++layer) {
            if (layer > 0) {
                uint32_t use_same_config = 0;
                if (!br.Read(1, use_same_config))
                    return false;
                if (use_same_config != 0)
                    continue;
            }
            LatmHeader layer_info;
            if (!ParseAacStreamInfo(br, layer_info))
                return false;
            if (!got_first) {
                out.audio_object_type = layer_info.audio_object_type;
                out.sampling_frequency_index = layer_info.sampling_frequency_index;
                out.sample_rate = layer_info.sample_rate;
                out.channel_configuration = layer_info.channel_configuration;
                out.channels = layer_info.channels;
                out.note = layer_info.note;
                got_first = true;
            }
        }
    }
    if (!got_first)
        return false;

    // frameLengthType：0/1/2 是 AAC 常见取值，后续 CELP 专用值不展开。
    if (!br.Read(3, value))
        return false;
    out.frame_length_type = static_cast<int>(value);
    if (out.frame_length_type == 0) {
        uint32_t fullness = 0;
        if (!br.Read(8, fullness))
            return false;
    } else if (out.frame_length_type == 1) {
        uint32_t frame_length = 0;
        if (!br.Read(9, frame_length))
            return false;
        out.frame_length = static_cast<int>(frame_length);
    } else if (out.frame_length_type == 2) {
        uint32_t frame_length = 0;
        if (!br.Read(6, frame_length))
            return false;
        out.frame_length = static_cast<int>(frame_length);
    }

    // otherDataPresent / otherDataLenBits / crcCheckPresent
    uint32_t other_data_present = 0;
    if (br.Read(1, other_data_present) && other_data_present != 0) {
        bool escaped = true;
        while (escaped && br.BitsLeft() >= 9) {
            uint32_t esc = 0;
            uint32_t bits = 0;
            if (!br.Read(1, esc) || !br.Read(8, bits))
                break;
            escaped = (esc != 0);
        }
    }
    uint32_t crc_present = 0;
    if (br.Read(1, crc_present))
        out.crc_check_present = (crc_present != 0);
    out.valid = true;
    return true;
}

bool AudioBitstreamParser::ParseAc3(const uint8_t* data, size_t size, bool force_eac3, Ac3Header& out) {
    out = Ac3Header{};
    if (data == nullptr || size < 6)
        return false;
    if (data[0] != 0x0B || data[1] != 0x77)
        return false;

    BitReader br(data, size);
    uint32_t value = 0;
    if (!br.Skip(16))
        return false; // syncword

    if (!force_eac3) {
        // ---- AC-3 BSI ----
        if (!br.Skip(16))
            return false; // crc1
        uint32_t fscod = 0;
        uint32_t frmsizecod = 0;
        if (!br.Read(2, fscod) || !br.Read(6, frmsizecod))
            return false;
        out.eac3 = false;
        out.sample_rate = (fscod < 3) ? kEac3SampleRates[fscod] : 0;
        out.frame_size_bytes = Ac3FrameSizeBytes(static_cast<int>(fscod), static_cast<int>(frmsizecod));
        out.num_blocks = 6;
        out.frame_samples = 1536;
        const int bitrate_index = static_cast<int>(frmsizecod) >> 1;
        if (bitrate_index >= 0 && bitrate_index < 19) {
            out.bit_rate_kbps = kAc3BitratesKbps[bitrate_index];
        }
        if (!br.Read(5, value))
            return false;
        out.bsid = static_cast<int>(value);
        if (!br.Read(5, value))
            return false;
        out.bsmod = static_cast<int>(value);
        if (!br.Read(3, value))
            return false;
        out.acmod = static_cast<int>(value);
        if (out.acmod < 0 || out.acmod > 7)
            return false;

        // cmixlev / surmixlev / dsurmod
        if ((out.acmod & 0x1) != 0 && out.acmod != 0x1) {
            if (!br.Skip(2))
                return false;
        }
        if ((out.acmod & 0x4) != 0) {
            if (!br.Skip(2))
                return false;
        }
        if (out.acmod == 0x2) {
            if (!br.Skip(2))
                return false;
        }
        if (!br.Read(1, value))
            return false;
        out.lfeon = (value != 0);
        if (!br.Read(5, value))
            return false;
        out.dialnorm_dbfs = (value > 0) ? -static_cast<int>(value) : 0;
        out.has_dialnorm = (value > 0);
        out.channels = AcmodChannels(out.acmod) + (out.lfeon ? 1 : 0);
        out.valid = out.frame_size_bytes > 0 && out.sample_rate > 0;
        return out.valid;
    }

    // ---- E-AC-3 BSI ----
    out.eac3 = true;
    uint32_t strmtyp = 0;
    uint32_t substreamid = 0;
    uint32_t frmsiz = 0;
    uint32_t fscod = 0;
    if (!br.Read(2, strmtyp) || !br.Read(3, substreamid) || !br.Read(11, frmsiz) || !br.Read(2, fscod)) {
        return false;
    }
    out.strmtyp = static_cast<int>(strmtyp);
    out.substreamid = static_cast<int>(substreamid);
    out.frame_size_bytes = (static_cast<int>(frmsiz) + 1) * 2;

    int numblkscod = 0;
    if (fscod == 3) {
        uint32_t fscod2 = 0;
        if (!br.Read(2, fscod2))
            return false;
        out.sample_rate = (fscod2 < 3) ? kEac3SampleRates2[fscod2] : 0;
        numblkscod = 3;
    } else {
        uint32_t blocks_code = 0;
        if (!br.Read(2, blocks_code))
            return false;
        out.sample_rate = kEac3SampleRates[fscod];
        numblkscod = static_cast<int>(blocks_code);
    }
    out.num_blocks = kEac3Blocks[numblkscod];
    out.frame_samples = out.num_blocks * 256;

    uint32_t acmod = 0;
    uint32_t lfeon = 0;
    if (!br.Read(3, acmod) || !br.Read(1, lfeon))
        return false;
    out.acmod = static_cast<int>(acmod);
    out.lfeon = (lfeon != 0);
    if (!br.Read(5, value))
        return false;
    out.bsid = static_cast<int>(value);
    if (!br.Read(5, value))
        return false;
    out.dialnorm_dbfs = (value > 0) ? -static_cast<int>(value) : 0;
    out.has_dialnorm = (value > 0);

    uint32_t compre = 0;
    if (!br.Read(1, compre))
        return false;
    if (compre != 0) {
        if (!br.Skip(8))
            return false;
    }
    if (out.acmod == 0) {
        if (!br.Skip(5))
            return false; // dialnorm2
        uint32_t compr2e = 0;
        if (!br.Read(1, compr2e))
            return false;
        if (compr2e != 0) {
            if (!br.Skip(8))
                return false;
        }
    }
    if (out.strmtyp == 1) {
        uint32_t chanmape = 0;
        if (!br.Read(1, chanmape))
            return false;
        if (chanmape != 0) {
            uint32_t chanmap = 0;
            if (!br.Read(16, chanmap))
                return false;
            out.has_chanmap = true;
            out.chanmap = static_cast<uint16_t>(chanmap);
            out.chanmap_channels = CountBits16(out.chanmap);
        }
    }

    if (out.sample_rate > 0 && out.frame_samples > 0) {
        out.bit_rate_kbps = static_cast<int>(static_cast<int64_t>(out.frame_size_bytes) * 8 * out.sample_rate /
                                             out.frame_samples / 1000);
    }
    out.channels = AcmodChannels(out.acmod) + (out.lfeon ? 1 : 0);

    int complexity_index = -1;
    if (FindJocMarker(data, size, complexity_index)) {
        // 标记为启发式：调用方不得把它当确定结论输出
        out.atmos_joc = true;
        out.atmos_heuristic = true;
        out.joc_complexity_index = complexity_index;
    }
    out.valid = out.frame_size_bytes > 0 && out.sample_rate > 0 && out.acmod >= 0 && out.acmod <= 7;
    return out.valid;
}

bool AudioBitstreamParser::ParseOpusHead(const uint8_t* data, size_t size, OpusHead& out) {
    out = OpusHead{};
    if (data == nullptr)
        return false;

    // 偏移与字节序在同一个分支里一起定（RFC 7845 的 OpusHead 是小端；
    // ISO/IEC 14496-3 的 dOps 是大端）。旧实现只切了 offset、却一律按小端读，
    // 于是 MP4 的 pre_skip 读成 14337（应为 312）、input_rate 读成负数。
    bool little_endian = false;
    size_t offset = 0;
    if (size >= 8 && std::memcmp(data, "OpusHead", 8) == 0) {
        little_endian = true;
        offset = 8;
        if (size < offset + 11)
            return false; // 固定部分 11 字节
    } else {
        little_endian = false; // dOps：没有 magic，从 version 开始
        if (size < 11)
            return false;
    }

    const auto rd16 = [&](size_t i) {
        const uint32_t b0 = data[offset + i];
        const uint32_t b1 = data[offset + i + 1];
        return static_cast<int>(little_endian ? (b1 << 8) | b0 : (b0 << 8) | b1);
    };
    const auto rd32 = [&](size_t i) {
        const uint32_t b0 = data[offset + i];
        const uint32_t b1 = data[offset + i + 1];
        const uint32_t b2 = data[offset + i + 2];
        const uint32_t b3 = data[offset + i + 3];
        return static_cast<int>(little_endian ? (b3 << 24) | (b2 << 16) | (b1 << 8) | b0
                                              : (b0 << 24) | (b1 << 16) | (b2 << 8) | b3);
    };

    out.big_endian = !little_endian;
    out.version = data[offset];
    out.channels = data[offset + 1];
    out.pre_skip = rd16(2);
    out.input_sample_rate = rd32(4);
    out.output_gain_q8 = static_cast<int16_t>(rd16(8));
    out.mapping_family = data[offset + 10];

    if (out.mapping_family != 0) {
        if (size < offset + 13)
            return false;
        out.stream_count = data[offset + 11];
        out.coupled_count = data[offset + 12];
        if (out.channels > 0 && size < offset + 13 + static_cast<size_t>(out.channels))
            return false;
    }
    out.valid = out.version <= 15 && out.channels > 0;
    return out.valid;
}

bool AudioBitstreamParser::ParseDOps(const uint8_t* data, size_t size, OpusHead& out) {
    out = OpusHead{};
    if (data == nullptr || size < 11)
        return false;
    // dOps 一定没有 magic；有 magic 说明调用方给错了输入（那是 Ogg 的 OpusHead）
    if (size >= 8 && std::memcmp(data, "OpusHead", 8) == 0)
        return false;
    return ParseOpusHead(data, size, out);
}

bool AudioBitstreamParser::ParseOpusToc(const uint8_t* data, size_t size, OpusToc& out) {
    out = OpusToc{};
    if (data == nullptr || size < 1)
        return false;
    const uint8_t toc = data[0];
    out.config = (toc >> 3) & 0x1F;
    out.stereo = (toc & 0x04) != 0;
    out.frame_count_code = toc & 0x03;
    out.frame_bytes = static_cast<int>(size);

    // config -> mode / bandwidth / frame duration
    static const int kMode[32] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    };
    static const int kBandwidth[32] = {
        0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 0, 0, 0, 0, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    };
    static const double kFrameMs[32] = {
        10,  20, 40, 60, 10,  20, 40, 60, 10,  20, 40, 60, 10,  20, 10, 20,
        2.5, 5,  10, 20, 2.5, 5,  10, 20, 2.5, 5,  10, 20, 2.5, 5,  10, 20,
    };
    out.mode = kMode[out.config];
    out.bandwidth = kBandwidth[out.config];
    out.frame_duration_ms = kFrameMs[out.config];

    // RFC 6716 §3.2.1：code 1 与 code 3 的第 2 个字节里都有 vbr 标志（code 3 还有
    // padding 标志）—— 只有 toc 首字节时无法确认包结构，必须要求 size >= 2。
    switch (out.frame_count_code) {
    case 0:
        out.frame_count = 1;
        break;
    case 1:
        if (size < 2)
            return false; // 缺 vbr 字节
        out.vbr = (data[1] & 0x80) != 0;
        out.frame_count = 2;
        break;
    case 2:
        out.frame_count = 2; // 2 帧 CBR，没有额外字节
        break;
    case 3:
        if (size < 2)
            return false; // 缺 vbr/padding/M 字节
        out.vbr = (data[1] & 0x80) != 0;
        out.padding = (data[1] & 0x40) != 0;
        out.frame_count = data[1] & 0x3F;
        break;
    default:
        break;
    }
    out.channels = out.stereo ? 2 : 1;
    // RFC 6716 §3.2.5：一包的总时长不得超过 120 ms。60ms×3 = 180ms 的 TOC
    // 是畸形数据，以前因为只看 frame_count<=48 而被判 valid。
    const double total_ms = out.frame_duration_ms * out.frame_count;
    out.valid = out.frame_count > 0 && out.frame_count <= 48 && total_ms <= 120.0 + 1e-9;
    return out.valid;
}

bool AudioBitstreamParser::ParseAacAudioSpecificConfig(const uint8_t* data, size_t size, AacAudioSpecificConfig& out) {
    out = AacAudioSpecificConfig{};
    if (data == nullptr || size == 0)
        return false;
    BitReader br(data, size);
    uint32_t value = 0;
    if (!br.Read(5, value))
        return false;
    int aot = static_cast<int>(value);
    if (aot == 31) {
        if (!br.Read(6, value))
            return false;
        aot = 32 + static_cast<int>(value);
    }
    out.audio_object_type = aot;

    if (!br.Read(4, value))
        return false;
    out.sampling_frequency_index = static_cast<int>(value);
    if (out.sampling_frequency_index == 15) {
        if (!br.Read(24, value))
            return false;
        out.sample_rate = static_cast<int>(value);
    } else {
        out.sample_rate = AacSampleRateFromIndex(out.sampling_frequency_index);
    }
    if (!br.Read(4, value))
        return false;
    out.channel_configuration = static_cast<int>(value);
    out.channels = AacChannelsFromConfig(out.channel_configuration);
    // core 采样率：SBR 生效后 sample_rate 会被换成输出采样率，原始值留在这里对照
    out.core_sample_rate = out.sample_rate;

    if (aot == 5 || aot == 29) {
        // 显式 HE-AAC：AOT 本身就是信令；这类 ASC 不含 GASpecificConfig，
        // channelConfiguration 之后直接是 extensionSamplingFrequencyIndex。
        out.sbr = true;
        out.sbr_signaled = true;
        out.sbr_source = "aot";
        out.ps = (aot == 29);
        if (!br.Read(4, value))
            return false;
        const int ext_index = static_cast<int>(value);
        if (ext_index == 15) {
            if (!br.Read(24, value))
                return false;
            out.extension_sample_rate = static_cast<int>(value);
        } else {
            out.extension_sample_rate = AacSampleRateFromIndex(ext_index);
        }
        if (out.extension_sample_rate > 0)
            out.sample_rate = out.extension_sample_rate;
    } else {
        if (aot == 22) {
            // ER BSAC 的 extensionChannelConfiguration 在 GASpecificConfig 之前
            if (!br.Read(4, value))
                return false;
            out.channel_configuration = static_cast<int>(value);
            out.channels = AacChannelsFromConfig(out.channel_configuration);
        }
        // 其余 AOT（绝大多数是 2 = LC）有 GASpecificConfig：先走到它能看到的
        // 末尾，再看有没有 SBR/PS 同步扩展。走位失败也不能把整个 ASC 判死 ——
        // AOT / 采样率 / 声道已经拿到了，只是 SBR 未知。
        std::string skip_note;
        if (SkipGaSpecificConfig(br, aot, out.channel_configuration, skip_note)) {
            ParseAacSyncExtension(br, out);
        } else if (!skip_note.empty()) {
            out.note = skip_note;
        }
    }

    // SBR 的输出采样率恒为 core 的 2 倍（ISO/IEC 14496-3 §4.6.18）：显式给了
    // extensionSamplingFrequency 就用它，没给就按 2 倍推 —— 不能停在 core 值上，
    // 否则会把 48 kHz 的 HE-AAC 报成 24 kHz。
    if (out.sbr && out.sample_rate == out.core_sample_rate && out.core_sample_rate > 0)
        out.sample_rate = out.core_sample_rate * 2;
    // PS 把单声道上混成立体声：声道数报 1 是位流传输的声道数，不是听到的声道数
    if (out.ps && out.channels == 1)
        out.channels = 2;

    out.valid = out.sample_rate > 0;
    return out.valid;
}

// ---------------------------------------------------------------------------
// MP3（MPEG-1 / MPEG-2 / MPEG-2.5 Audio）
// ---------------------------------------------------------------------------

namespace {

// 码率表（kbps）。下标 0 = free format，15 = 无效，所以表按 [15] 建、1..14 有效。
constexpr int kMp3BitrateV1L1[15] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448};
constexpr int kMp3BitrateV1L2[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
constexpr int kMp3BitrateV1L3[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
constexpr int kMp3BitrateV2L1[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256};
// MPEG-2 / 2.5 的 Layer II 与 Layer III 共用一张表
constexpr int kMp3BitrateV2L23[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};

// 采样率表：[版本][索引]。版本用 0=MPEG1 1=MPEG2 2=MPEG2.5 编码，避免再做分支。
constexpr int kMp3SampleRates[3][3] = {
    {44100, 48000, 32000},
    {22050, 24000, 16000},
    {11025, 12000, 8000},
};

int Mp3BitrateKbps(int version, int layer, int index) {
    if (index <= 0 || index >= 15)
        return 0;
    if (version == 1) {
        if (layer == 1)
            return kMp3BitrateV1L1[index];
        if (layer == 2)
            return kMp3BitrateV1L2[index];
        return kMp3BitrateV1L3[index];
    }
    if (layer == 1)
        return kMp3BitrateV2L1[index];
    return kMp3BitrateV2L23[index];
}

// 帧头之后、Xing/Info 之前那段 side information 的长度（只有 Layer III 有）
int Mp3SideInfoBytes(int version, bool mono) {
    if (version == 1)
        return mono ? 17 : 32;
    return mono ? 9 : 17;
}

// FLAC 帧头 CRC-8：poly x^8 + x^2 + x + 1（0x07），初值 0，不含 CRC 字节本身
uint8_t FlacCrc8(const uint8_t* data, size_t size) {
    uint8_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x80u) ? static_cast<uint8_t>((crc << 1) ^ 0x07u) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

uint32_t Rd32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

bool AudioBitstreamParser::ParseMp3FrameHeader(const uint8_t* data, size_t size, Mp3FrameHeader& out) {
    out = Mp3FrameHeader{};
    if (data == nullptr || size < 4)
        return false;
    // 11 bit 同步字全 1
    if (data[0] != 0xFF || (data[1] & 0xE0) != 0xE0)
        return false;

    const int version_bits = (data[1] >> 3) & 0x03;
    if (version_bits == 1)
        return false; // 保留值
    const int layer_bits = (data[1] >> 1) & 0x03;
    if (layer_bits == 0)
        return false; // 保留值

    // version_bits: 0=MPEG2.5 2=MPEG2 3=MPEG1；layer_bits: 1=III 2=II 3=I
    int version_index = 0; // 0=MPEG1 1=MPEG2 2=MPEG2.5
    if (version_bits == 0) {
        version_index = 2;
        out.mpeg_version = 25;
    } else if (version_bits == 2) {
        version_index = 1;
        out.mpeg_version = 2;
    } else {
        version_index = 0;
        out.mpeg_version = 1;
    }
    out.layer = 4 - layer_bits; // 1 -> 3, 2 -> 2, 3 -> 1

    out.crc_present = (data[1] & 0x01u) == 0;

    const int bitrate_index = (data[2] >> 4) & 0x0F;
    const int sr_index = (data[2] >> 2) & 0x03;
    out.padding = ((data[2] >> 1) & 0x01u) != 0;
    if (sr_index == 3)
        return false; // 保留值
    out.sample_rate = kMp3SampleRates[version_index][sr_index];
    if (out.sample_rate <= 0)
        return false;

    out.bitrate_kbps = Mp3BitrateKbps(out.mpeg_version, out.layer, bitrate_index);
    if (bitrate_index == 0) {
        // free format：码率没写，帧长算不出来。这是合法流，但本解析器无法定界，
        // 必须显式标出而不是当成"解析成功"。
        out.free_format = true;
        return false;
    }
    if (bitrate_index == 15 || out.bitrate_kbps <= 0)
        return false;

    out.channel_mode = (data[3] >> 6) & 0x03;
    out.channels = (out.channel_mode == 3) ? 1 : 2;

    out.samples_per_frame = Mp3SamplesPerFrame(out.mpeg_version, out.layer);
    const double bps = static_cast<double>(out.bitrate_kbps) * 1000.0;
    const double sr = static_cast<double>(out.sample_rate);
    int frame_bytes = 0;
    if (out.layer == 1) {
        frame_bytes = static_cast<int>((12.0 * bps / sr + (out.padding ? 1.0 : 0.0)) * 4.0);
    } else if (out.layer == 3 && out.mpeg_version != 1) {
        frame_bytes = static_cast<int>(72.0 * bps / sr) + (out.padding ? 1 : 0);
    } else {
        frame_bytes = static_cast<int>(144.0 * bps / sr) + (out.padding ? 1 : 0);
    }
    if (out.crc_present)
        frame_bytes += 0; // CRC 的 2 字节已含在 frame_bytes 公式里（公式按总长算）
    if (frame_bytes <= 4)
        return false;
    out.frame_bytes = frame_bytes;
    if (static_cast<size_t>(out.frame_bytes) > size) {
        out.truncated = true;
        return false;
    }
    out.valid = true;
    return true;
}

int AudioBitstreamParser::Mp3SamplesPerFrame(int mpeg_version, int layer) {
    if (layer == 1)
        return 384;
    if (layer == 2)
        return 1152;
    return (mpeg_version == 1) ? 1152 : 576;
}

bool AudioBitstreamParser::ParseMp3VbrInfo(const uint8_t* data, size_t size, const Mp3FrameHeader& hdr,
                                           Mp3VbrInfo& out) {
    out = Mp3VbrInfo{};
    if (data == nullptr || !hdr.valid || hdr.layer != 3)
        return false; // Xing/Info 只存在于 Layer III

    const size_t side = static_cast<size_t>(Mp3SideInfoBytes(hdr.mpeg_version, hdr.channels == 1));
    const size_t xing_off = 4 + (hdr.crc_present ? 2u : 0u) + side;
    if (xing_off + 8 <= size) {
        const uint8_t* p = data + xing_off;
        const bool is_xing = (std::memcmp(p, "Xing", 4) == 0);
        const bool is_info = (std::memcmp(p, "Info", 4) == 0);
        if (is_xing || is_info) {
            out.valid = true;
            out.tag = is_xing ? "Xing" : "Info";
            const uint32_t flags = Rd32BE(p + 4);
            size_t pos = xing_off + 8;
            if ((flags & 0x01) != 0 && pos + 4 <= size) {
                out.frame_count = static_cast<int>(Rd32BE(data + pos));
                pos += 4;
            }
            if ((flags & 0x02) != 0 && pos + 4 <= size) {
                out.stream_bytes = static_cast<int64_t>(Rd32BE(data + pos));
                pos += 4;
            }
            if ((flags & 0x04) != 0) {
                out.has_toc = true;
                pos += 100;
            }
            if ((flags & 0x08) != 0 && pos + 4 <= size) {
                out.quality = static_cast<int>(Rd32BE(data + pos));
                pos += 4;
            }
            // LAME / Lavc 扩展：9 字节编码器串 + 固定布局，delay/padding 在 tag 起点 +21
            if (pos + 24 <= size) {
                const uint8_t* tag = data + pos;
                bool printable = true;
                for (int i = 0; i < 9; ++i) {
                    if (tag[i] < 0x20 || tag[i] > 0x7E) {
                        printable = false;
                        break;
                    }
                }
                if (printable) {
                    out.encoder.assign(reinterpret_cast<const char*>(tag), 9);
                    out.has_encoder_delay = true;
                    out.encoder_delay = (static_cast<int>(tag[21]) << 4) | (static_cast<int>(tag[22]) >> 4);
                    out.encoder_padding = ((static_cast<int>(tag[22]) & 0x0F) << 8) | static_cast<int>(tag[23]);
                }
            }
            return true;
        }
    }

    // VBRI 固定在帧头之后 32 字节处（Fraunhofer 编码器）
    const size_t vbri_off = 4 + 32;
    if (vbri_off + 18 <= size && std::memcmp(data + vbri_off, "VBRI", 4) == 0) {
        const uint8_t* p = data + vbri_off;
        out.valid = true;
        out.tag = "VBRI";
        // 偏移: magic(4) version(2) delay(2) quality(2) bytes(4) frames(4)
        out.quality = static_cast<int>((static_cast<uint32_t>(p[8]) << 8) | p[9]);
        out.stream_bytes = static_cast<int64_t>(Rd32BE(p + 10));
        out.frame_count = static_cast<int>(Rd32BE(p + 14));
        return true;
    }
    return false;
}

bool AudioBitstreamParser::ParseFlacStreamInfo(const uint8_t* data, size_t size, FlacStreamInfo& out) {
    out = FlacStreamInfo{};
    if (data == nullptr)
        return false;
    size_t pos = 0;
    // 带 "fLaC" magic：后面跟一个 METADATA_BLOCK_HEADER（1 bit last + 7 bit type + 24 bit len）
    if (size >= 8 && std::memcmp(data, "fLaC", 4) == 0) {
        out.has_magic = true;
        const uint8_t block_type = data[4] & 0x7F;
        const uint32_t block_len = (static_cast<uint32_t>(data[5]) << 16) | (static_cast<uint32_t>(data[6]) << 8) |
                                   static_cast<uint32_t>(data[7]);
        if (block_type != 0)
            return false; // 第一个 metadata block 必须是 STREAMINFO
        if (block_len < 34 || size < 8 + 34)
            return false;
        pos = 8;
    } else if (size >= 34) {
        // MP4（dfLa）/ Matroska 的 extradata 直接就是 34 字节 STREAMINFO
        pos = 0;
    } else {
        return false;
    }

    const uint8_t* p = data + pos;
    BitReader br(p, 34);
    uint64_t value = 0;
    if (!br.Read(16, value))
        return false;
    out.min_block_size = static_cast<int>(value);
    if (!br.Read(16, value))
        return false;
    out.max_block_size = static_cast<int>(value);
    if (!br.Read(24, value))
        return false; // min frame size（未使用）
    if (!br.Read(24, value))
        return false; // max frame size（未使用）
    if (!br.Read(20, value))
        return false;
    out.sample_rate = static_cast<int>(value);
    if (!br.Read(3, value))
        return false;
    out.channels = static_cast<int>(value) + 1;
    if (!br.Read(5, value))
        return false;
    out.bits_per_sample = static_cast<int>(value) + 1;
    if (!br.Read(36, value))
        return false;
    out.total_samples = value;
    out.md5_present = true;
    out.valid = (out.sample_rate > 0);
    return out.valid;
}

bool AudioBitstreamParser::ParseFlacFrameHeader(const uint8_t* data, size_t size, FlacFrameHeader& out) {
    out = FlacFrameHeader{};
    if (data == nullptr || size < 6)
        return false;
    // 14 bit 同步 0b11111111111110
    if (data[0] != 0xFF || (data[1] & 0xFC) != 0xF8)
        return false;
    const bool reserved1 = (data[1] & 0x02) != 0;
    if (reserved1)
        return false;
    out.variable_block_size = (data[1] & 0x01) != 0;
    const int block_code = (data[2] >> 4) & 0x0F;
    const int rate_code = data[2] & 0x0F;
    const int channel_code = (data[3] >> 4) & 0x0F;
    const int depth_code = (data[3] >> 1) & 0x07;
    const bool reserved2 = (data[3] & 0x01) != 0;
    if (reserved2 || block_code == 0 || rate_code == 15 || channel_code > 10)
        return false;
    if (depth_code == 3)
        return false; // 保留值

    size_t pos = 4;
    // UTF-8 编码的帧号/样本号（1..7 字节）：首字节高位决定长度
    if (pos >= size)
        return false;
    const uint8_t lead = data[pos];
    int extra = 0;
    if ((lead & 0x80) == 0) {
        extra = 0;
    } else if ((lead & 0xE0) == 0xC0) {
        extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
    } else if ((lead & 0xFC) == 0xF8) {
        extra = 4;
    } else if ((lead & 0xFE) == 0xFC) {
        extra = 5;
    } else if (lead == 0xFE) {
        extra = 6;
    } else {
        return false;
    }
    pos += 1 + static_cast<size_t>(extra);
    if (pos > size)
        return false;

    switch (block_code) {
    case 6:
        if (pos + 1 > size)
            return false;
        out.block_size = static_cast<int>(data[pos]) + 1;
        pos += 1;
        break;
    case 7:
        if (pos + 2 > size)
            return false;
        out.block_size = (static_cast<int>(data[pos]) << 8 | static_cast<int>(data[pos + 1])) + 1;
        pos += 2;
        break;
    default: {
        static const int kBlockSizes[16] = {0,   192, 576,  1152, 2304, 4608, 0,     0,
                                            256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
        out.block_size = kBlockSizes[block_code];
        break;
    }
    }

    switch (rate_code) {
    case 0:
        out.sample_rate = 0; // 沿用 STREAMINFO
        break;
    case 12:
        if (pos + 1 > size)
            return false;
        out.sample_rate = static_cast<int>(data[pos]) * 1000;
        pos += 1;
        break;
    case 13:
        if (pos + 2 > size)
            return false;
        out.sample_rate = static_cast<int>(data[pos]) << 8 | static_cast<int>(data[pos + 1]);
        pos += 2;
        break;
    case 14:
        if (pos + 2 > size)
            return false;
        out.sample_rate = (static_cast<int>(data[pos]) << 8 | static_cast<int>(data[pos + 1])) * 10;
        pos += 2;
        break;
    default: {
        static const int kRates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
        out.sample_rate = kRates[rate_code];
        break;
    }
    }

    out.channel_assignment = channel_code;
    out.channels = (channel_code < 8) ? (channel_code + 1) : 2;
    switch (depth_code) {
    case 1:
        out.bits_per_sample = 8;
        break;
    case 2:
        out.bits_per_sample = 12;
        break;
    case 4:
        out.bits_per_sample = 16;
        break;
    case 5:
        out.bits_per_sample = 20;
        break;
    case 6:
        out.bits_per_sample = 24;
        break;
    case 7:
        out.bits_per_sample = 32;
        break;
    default:
        out.bits_per_sample = 0;
        break;
    }

    if (out.block_size <= 0)
        return false;
    // 头 CRC-8 覆盖到 CRC 字节之前
    if (pos + 1 <= size && pos > 0) {
        out.crc_checked = true;
        out.crc_ok = (FlacCrc8(data, pos) == data[pos]);
    } else {
        out.truncated = true;
    }
    out.valid = true;
    return true;
}

bool AudioBitstreamParser::ParseAacProgramConfig(const uint8_t* data, size_t size, AacProgramConfig& out) {
    out = AacProgramConfig{};
    if (data == nullptr || size < 2)
        return false;
    BitReader br(data, size);
    uint64_t value = 0;
    // raw_data_block 由若干 syntax element 组成，每个以 3 bit element_instance_tag 开头，
    // 元素类型在 ID_END(7) 之前由 4 bit 的 id_syn_ele 给出。这里只看**第一个**元素：
    // 不是 PCE(5) 就放弃（要走完 SCE/CPE 的整个 payload 才能到下一个元素）。
    if (!br.Read(3, value))
        return false;
    const int id_syn_ele = static_cast<int>(value);
    if (id_syn_ele == 7) {
        out.note = "raw_data_block 只有一个 ID_END，没有有效元素";
        return false;
    }
    if (id_syn_ele != 5) {
        out.note = "raw_data_block 的首个元素不是 PCE（需遍历前序元素才能定位），未解析声道布局";
        return false;
    }
    if (!br.Read(4, value))
        return false;
    out.element_instance_tag = static_cast<int>(value);
    if (!br.Read(2, value))
        return false;
    out.object_type = static_cast<int>(value);
    if (!br.Read(4, value))
        return false;
    out.sampling_frequency_index = static_cast<int>(value);
    out.sample_rate = AacSampleRateFromIndex(out.sampling_frequency_index);

    int front = 0;
    int side = 0;
    int back = 0;
    int lfe = 0;
    int assoc = 0;
    int cc = 0;
    if (!br.Read(4, value))
        return false;
    front = static_cast<int>(value);
    if (!br.Read(4, value))
        return false;
    side = static_cast<int>(value);
    if (!br.Read(4, value))
        return false;
    back = static_cast<int>(value);
    if (!br.Read(4, value))
        return false;
    lfe = static_cast<int>(value);
    // ❗ ISO/IEC 14496-3 表 4.5：num_assoc_data_elements 是 **3 bit**，
    // 其余三个通道元素计数都是 4 bit。这里曾是 4 bit，会把后面所有字段整体错位一位，
    // 结果就是"PCE 能解析出东西但声道布局是错的" —— 比直接失败更危险。
    if (!br.Read(3, value))
        return false;
    assoc = static_cast<int>(value); // num_assoc_data（3 bit，不是 4）
    if (!br.Read(4, value))
        return false;
    cc = static_cast<int>(value); // num_valid_cc
    if (!br.Read(1, value))
        return false;
    const bool mono_mixdown = value != 0;
    if (!br.Read(1, value))
        return false;
    const bool stereo_mixdown = value != 0;
    if (!br.Read(1, value))
        return false;
    const bool matrix_mixdown = value != 0;
    if (mono_mixdown) {
        if (!br.Skip(4))
            return false;
    }
    if (stereo_mixdown) {
        if (!br.Skip(4))
            return false;
    }
    if (matrix_mixdown) {
        if (!br.Skip(3))
            return false;
    }

    // 每个前端/侧/后环绕元素：1 bit is_cpe + 4 bit tag_select
    int channels = 0;
    for (int group = 0; group < 3; ++group) {
        const int count = (group == 0) ? front : (group == 1 ? side : back);
        int counted = 0;
        for (int i = 0; i < count; ++i) {
            if (!br.Read(1, value))
                return false;
            const bool is_cpe = value != 0;
            if (!br.Skip(4))
                return false;
            counted += is_cpe ? 2 : 1;
        }
        channels += counted;
        if (group == 0)
            out.front_channels = counted;
        else if (group == 1)
            out.side_channels = counted;
        else
            out.back_channels = counted;
    }
    out.lfe_channels = lfe;
    out.channels = channels + lfe;

    for (int i = 0; i < assoc; ++i) {
        if (!br.Skip(4))
            return false;
    }
    for (int i = 0; i < cc; ++i) {
        if (!br.Skip(5))
            return false; // 1 bit is_ind_sw + 4 bit tag
    }
    if (!br.Skip(1))
        return false; // byte_alignment

    char layout[64] = {0};
    std::snprintf(layout, sizeof(layout), "%d/%d/%d%s", out.front_channels, out.side_channels, out.back_channels,
                  lfe > 0 ? " + LFE" : "");
    out.layout = layout;
    out.valid = (out.channels > 0 && out.sample_rate > 0);
    if (!out.valid)
        out.note = "PCE 里的声道数或采样率无效";
    return out.valid;
}

} // namespace videoeye
