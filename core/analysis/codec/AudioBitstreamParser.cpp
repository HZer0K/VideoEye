#include "core/analysis/codec/AudioBitstreamParser.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace videoeye {
namespace {

// 大端位读取器。音频头字段最多 33 bit，统一用 uint64_t 承接。
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool Read(int bits, uint64_t& value) {
        if (bits < 0 || bits > 64) return false;
        if (bit_pos_ + static_cast<size_t>(bits) > size_ * 8) return false;
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
        if (!Read(bits, wide)) return false;
        value = static_cast<uint32_t>(wide);
        return true;
    }

    bool Skip(int bits) {
        if (bits < 0) return false;
        if (bit_pos_ + static_cast<size_t>(bits) > size_ * 8) return false;
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
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,  7350,  0,     0,     0,
};

// AC-3 帧长表（单位：16-bit words），[frmsizecod][fscod]
constexpr uint16_t kAc3FrameSizeWords[38][3] = {
    {64, 69, 96},    {64, 70, 96},    {80, 87, 120},   {80, 88, 120},
    {96, 104, 144},  {96, 105, 144},  {112, 121, 168}, {112, 122, 168},
    {128, 139, 192}, {128, 140, 192}, {160, 174, 240}, {160, 175, 240},
    {192, 208, 288}, {192, 209, 288}, {224, 243, 336}, {224, 244, 336},
    {256, 278, 384}, {256, 279, 384}, {320, 348, 480}, {320, 349, 480},
    {384, 417, 576}, {384, 418, 576}, {448, 487, 672}, {448, 488, 672},
    {512, 557, 768}, {512, 558, 768}, {640, 696, 960}, {640, 697, 960},
    {768, 835, 1152}, {768, 836, 1152}, {896, 975, 1344}, {896, 976, 1344},
    {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
    {1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920},
};

constexpr int kAc3BitratesKbps[19] = {
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
    192, 224, 256, 320, 384, 448, 512, 576, 640,
};

constexpr int kEac3SampleRates[3] = {48000, 44100, 32000};
constexpr int kEac3SampleRates2[3] = {24000, 22050, 16000};
constexpr int kEac3Blocks[4] = {1, 2, 3, 6};

int AcmodChannels(int acmod) {
    static const int kChannels[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    if (acmod < 0 || acmod > 7) return 0;
    return kChannels[acmod];
}

int CountBits16(uint16_t value) {
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if ((value >> i) & 1u) ++count;
    }
    return count;
}

// 在 E-AC-3 BSI 前若干字节里找 Dolby JOC 的 ASCII 标记。
// 这不是规范级解析（JOC 的 addbsi 载荷格式不在公开的 E-AC-3 基础规范里），
// 因此只在命中时标记、并记录来源；找不到就不声称支持 Atmos。
bool FindJocMarker(const uint8_t* data, size_t size, int& complexity_index) {
    if (data == nullptr) return false;
    const size_t limit = std::min<size_t>(size, 96);
    for (size_t i = 0; i + 2 < limit; ++i) {
        const uint8_t a = static_cast<uint8_t>(data[i] | 0x20);
        const uint8_t b = static_cast<uint8_t>(data[i + 1] | 0x20);
        const uint8_t c = static_cast<uint8_t>(data[i + 2] | 0x20);
        if (a == 'j' && b == 'o' && c == 'c') {
            complexity_index = -1;
            if (i + 3 < limit) {
                const int value = data[i + 3];
                if (value >= 0 && value <= 31) complexity_index = value;
            }
            return true;
        }
    }
    return false;
}

bool ParseAacStreamInfo(BitReader& br, LatmHeader& out) {
    uint32_t value = 0;
    if (!br.Read(5, value)) return false;
    int aot = static_cast<int>(value);
    if (aot == 31) {
        if (!br.Read(6, value)) return false;
        aot = 32 + static_cast<int>(value);
    }
    out.audio_object_type = aot;

    if (!br.Read(4, value)) return false;
    const int sf_index = static_cast<int>(value);
    int sample_rate = AudioBitstreamParser::AacSampleRateFromIndex(sf_index);
    if (sf_index == 15) {
        if (!br.Read(24, value)) return false;
        sample_rate = static_cast<int>(value);
    }
    out.sampling_frequency_index = sf_index;
    out.sample_rate = sample_rate;

    if (!br.Read(4, value)) return false;
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
                if (br.Read(24, explicit_rate)) ext_rate = static_cast<int>(explicit_rate);
            }
            if (ext_rate > 0) out.sample_rate = ext_rate;
        }
        if (aot == 29) out.note = "HE-AACv2 (SBR+PS)";
        else out.note = "HE-AAC (SBR)";
    }

    if (aot == 22) {
        if (br.Read(4, value)) {
            out.channel_configuration = static_cast<int>(value);
            out.channels = AudioBitstreamParser::AacChannelsFromConfig(out.channel_configuration);
        }
    }
    return true;
}

}  // namespace

int AudioBitstreamParser::AacSampleRateFromIndex(int index) {
    if (index < 0 || index > 15) return 0;
    return kAacSampleRates[index];
}

int AudioBitstreamParser::AacChannelsFromConfig(int channel_configuration) {
    // 0 = 由 PCE/GASpecificConfig 定义，头里读不出声道数。
    static const int kChannels[8] = {0, 1, 2, 3, 4, 5, 6, 8};
    if (channel_configuration < 0 || channel_configuration > 7) return 0;
    return kChannels[channel_configuration];
}

int AudioBitstreamParser::Ac3FrameSizeBytes(int fscod, int frmsizecod) {
    if (fscod < 0 || fscod > 2 || frmsizecod < 0 || frmsizecod > 37) return 0;
    return static_cast<int>(kAc3FrameSizeWords[frmsizecod][fscod]) * 2;
}

bool AudioBitstreamParser::ParseAdts(const uint8_t* data, size_t size, AdtsHeader& out) {
    out = AdtsHeader{};
    if (data == nullptr || size < 7) return false;
    if (data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) return false;

    out.mpeg_version = ((data[1] >> 3) & 1u) ? 2 : 4;
    out.protection_absent = (data[1] & 1u) != 0;
    const int profile = (data[2] >> 6) & 0x03;
    out.audio_object_type = profile + 1;
    out.sampling_frequency_index = (data[2] >> 2) & 0x0F;
    out.sample_rate = AacSampleRateFromIndex(out.sampling_frequency_index);
    out.channel_configuration =
        ((data[2] & 0x01) << 2) | ((static_cast<int>(data[3]) >> 6) & 0x03);
    out.channels = AacChannelsFromConfig(out.channel_configuration);
    out.frame_length = ((static_cast<int>(data[3]) & 0x03) << 11) |
                       (static_cast<int>(data[4]) << 3) |
                       ((static_cast<int>(data[5]) >> 5) & 0x07);
    out.num_raw_data_blocks = data[6] & 0x03;
    out.header_bytes = out.protection_absent ? 7 : 9;
    if (out.frame_length < out.header_bytes) return false;
    out.valid = true;
    return true;
}

bool AudioBitstreamParser::ParseLoasHeader(const uint8_t* data, size_t size,
                                           int& audio_mux_length_bytes,
                                           bool& use_same_stream_mux) {
    audio_mux_length_bytes = 0;
    use_same_stream_mux = false;
    if (data == nullptr || size < 4) return false;
    // syncword 11 bit = 0x2B7：0x56 0xE0..0xFF
    if (data[0] != 0x56 || (data[1] & 0xE0) != 0xE0) return false;
    audio_mux_length_bytes =
        ((static_cast<int>(data[1]) & 0x1F) << 8) | static_cast<int>(data[2]);
    use_same_stream_mux = (data[3] & 0x80) != 0;
    return audio_mux_length_bytes > 0;
}

bool AudioBitstreamParser::ParseLatmStreamMuxConfig(const uint8_t* data, size_t size,
                                                     LatmHeader& out) {
    out = LatmHeader{};
    if (data == nullptr || size == 0) return false;
    BitReader br(data, size);
    uint32_t value = 0;
    if (!br.Read(1, value)) return false;
    if (value != 0) {
        out.use_same_stream_mux = true;
        out.note = "useSameStreamMux=1（沿用上一帧配置）";
        return false;
    }

    uint32_t audio_mux_version = 0;
    if (!br.Read(1, audio_mux_version)) return false;
    out.audio_mux_version = static_cast<int>(audio_mux_version);
    if (audio_mux_version == 1) {
        uint32_t version_a = 0;
        if (!br.Read(1, version_a)) return false;
        if (version_a != 0) {
            out.note = "audioMuxVersionA=1，暂不支持";
            return false;
        }
        // latmGetValue(): 1 bit bytesForValue，随后 (bytesForValue+1) 字节
        uint32_t bytes_for_value = 0;
        if (!br.Read(1, bytes_for_value)) return false;
        if (!br.Skip(static_cast<int>((bytes_for_value + 1) * 8))) return false;
    }

    uint32_t all_streams_same_time_framing = 0;
    if (!br.Read(1, all_streams_same_time_framing)) return false;
    if (all_streams_same_time_framing == 0) {
        out.note = "allStreamsSameTimeFraming=0，暂不支持";
        return false;
    }
    if (!br.Read(6, value)) return false;
    out.num_sub_frames = static_cast<int>(value);
    uint32_t num_program = 0;
    if (!br.Read(4, num_program)) return false;

    bool got_first = false;
    for (uint32_t program = 0; program <= num_program; ++program) {
        uint32_t num_layer = 0;
        if (!br.Read(3, num_layer)) return false;
        for (uint32_t layer = 0; layer <= num_layer; ++layer) {
            if (layer > 0) {
                uint32_t use_same_config = 0;
                if (!br.Read(1, use_same_config)) return false;
                if (use_same_config != 0) continue;
            }
            LatmHeader layer_info;
            if (!ParseAacStreamInfo(br, layer_info)) return false;
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
    if (!got_first) return false;

    // frameLengthType：0/1/2 是 AAC 常见取值，后续 CELP 专用值不展开。
    if (!br.Read(3, value)) return false;
    out.frame_length_type = static_cast<int>(value);
    if (out.frame_length_type == 0) {
        uint32_t fullness = 0;
        if (!br.Read(8, fullness)) return false;
    } else if (out.frame_length_type == 1) {
        uint32_t frame_length = 0;
        if (!br.Read(9, frame_length)) return false;
        out.frame_length = static_cast<int>(frame_length);
    } else if (out.frame_length_type == 2) {
        uint32_t frame_length = 0;
        if (!br.Read(6, frame_length)) return false;
        out.frame_length = static_cast<int>(frame_length);
    }

    // otherDataPresent / otherDataLenBits / crcCheckPresent
    uint32_t other_data_present = 0;
    if (br.Read(1, other_data_present) && other_data_present != 0) {
        bool escaped = true;
        while (escaped && br.BitsLeft() >= 9) {
            uint32_t esc = 0;
            uint32_t bits = 0;
            if (!br.Read(1, esc) || !br.Read(8, bits)) break;
            escaped = (esc != 0);
        }
    }
    uint32_t crc_present = 0;
    if (br.Read(1, crc_present)) out.crc_check_present = (crc_present != 0);
    out.valid = true;
    return true;
}

bool AudioBitstreamParser::ParseAc3(const uint8_t* data, size_t size, bool force_eac3,
                                     Ac3Header& out) {
    out = Ac3Header{};
    if (data == nullptr || size < 6) return false;
    if (data[0] != 0x0B || data[1] != 0x77) return false;

    BitReader br(data, size);
    uint32_t value = 0;
    if (!br.Skip(16)) return false;   // syncword

    if (!force_eac3) {
        // ---- AC-3 BSI ----
        if (!br.Skip(16)) return false;   // crc1
        uint32_t fscod = 0;
        uint32_t frmsizecod = 0;
        if (!br.Read(2, fscod) || !br.Read(6, frmsizecod)) return false;
        out.eac3 = false;
        out.sample_rate = (fscod < 3) ? kEac3SampleRates[fscod] : 0;
        out.frame_size_bytes = Ac3FrameSizeBytes(static_cast<int>(fscod),
                                                 static_cast<int>(frmsizecod));
        out.num_blocks = 6;
        out.frame_samples = 1536;
        const int bitrate_index = static_cast<int>(frmsizecod) >> 1;
        if (bitrate_index >= 0 && bitrate_index < 19) {
            out.bit_rate_kbps = kAc3BitratesKbps[bitrate_index];
        }
        if (!br.Read(5, value)) return false;
        out.bsid = static_cast<int>(value);
        if (!br.Read(5, value)) return false;
        out.bsmod = static_cast<int>(value);
        if (!br.Read(3, value)) return false;
        out.acmod = static_cast<int>(value);
        if (out.acmod < 0 || out.acmod > 7) return false;

        // cmixlev / surmixlev / dsurmod
        if ((out.acmod & 0x1) != 0 && out.acmod != 0x1) {
            if (!br.Skip(2)) return false;
        }
        if ((out.acmod & 0x4) != 0) {
            if (!br.Skip(2)) return false;
        }
        if (out.acmod == 0x2) {
            if (!br.Skip(2)) return false;
        }
        if (!br.Read(1, value)) return false;
        out.lfeon = (value != 0);
        if (!br.Read(5, value)) return false;
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
    if (!br.Read(2, strmtyp) || !br.Read(3, substreamid) || !br.Read(11, frmsiz) ||
        !br.Read(2, fscod)) {
        return false;
    }
    out.strmtyp = static_cast<int>(strmtyp);
    out.substreamid = static_cast<int>(substreamid);
    out.frame_size_bytes = (static_cast<int>(frmsiz) + 1) * 2;

    int numblkscod = 0;
    if (fscod == 3) {
        uint32_t fscod2 = 0;
        if (!br.Read(2, fscod2)) return false;
        out.sample_rate = (fscod2 < 3) ? kEac3SampleRates2[fscod2] : 0;
        numblkscod = 3;
    } else {
        uint32_t blocks_code = 0;
        if (!br.Read(2, blocks_code)) return false;
        out.sample_rate = kEac3SampleRates[fscod];
        numblkscod = static_cast<int>(blocks_code);
    }
    out.num_blocks = kEac3Blocks[numblkscod];
    out.frame_samples = out.num_blocks * 256;

    uint32_t acmod = 0;
    uint32_t lfeon = 0;
    if (!br.Read(3, acmod) || !br.Read(1, lfeon)) return false;
    out.acmod = static_cast<int>(acmod);
    out.lfeon = (lfeon != 0);
    if (!br.Read(5, value)) return false;
    out.bsid = static_cast<int>(value);
    if (!br.Read(5, value)) return false;
    out.dialnorm_dbfs = (value > 0) ? -static_cast<int>(value) : 0;
    out.has_dialnorm = (value > 0);

    uint32_t compre = 0;
    if (!br.Read(1, compre)) return false;
    if (compre != 0) {
        if (!br.Skip(8)) return false;
    }
    if (out.acmod == 0) {
        if (!br.Skip(5)) return false;   // dialnorm2
        uint32_t compr2e = 0;
        if (!br.Read(1, compr2e)) return false;
        if (compr2e != 0) {
            if (!br.Skip(8)) return false;
        }
    }
    if (out.strmtyp == 1) {
        uint32_t chanmape = 0;
        if (!br.Read(1, chanmape)) return false;
        if (chanmape != 0) {
            uint32_t chanmap = 0;
            if (!br.Read(16, chanmap)) return false;
            out.has_chanmap = true;
            out.chanmap = static_cast<uint16_t>(chanmap);
            out.chanmap_channels = CountBits16(out.chanmap);
        }
    }

    if (out.sample_rate > 0 && out.frame_samples > 0) {
        out.bit_rate_kbps = static_cast<int>(
            static_cast<int64_t>(out.frame_size_bytes) * 8 * out.sample_rate /
            out.frame_samples / 1000);
    }
    out.channels = AcmodChannels(out.acmod) + (out.lfeon ? 1 : 0);

    int complexity_index = -1;
    if (FindJocMarker(data, size, complexity_index)) {
        out.atmos_joc = true;
        out.joc_complexity_index = complexity_index;
    }
    out.valid = out.frame_size_bytes > 0 && out.sample_rate > 0 && out.acmod >= 0 &&
                out.acmod <= 7;
    return out.valid;
}

bool AudioBitstreamParser::ParseOpusHead(const uint8_t* data, size_t size, OpusHead& out) {
    out = OpusHead{};
    if (data == nullptr || size < 19) return false;

    size_t offset = 0;
    const bool has_magic = size >= 8 && std::memcmp(data, "OpusHead", 8) == 0;
    if (has_magic) {
        offset = 8;
        if (size < offset + 11) return false;
    } else {
        // MP4 dOps：没有 "OpusHead" magic，直接是 version/channels/...
        offset = 0;
    }

    out.version = data[offset];
    out.channels = data[offset + 1];
    out.pre_skip = static_cast<int>(data[offset + 2]) |
                   (static_cast<int>(data[offset + 3]) << 8);
    out.input_sample_rate = static_cast<int>(data[offset + 4]) |
                            (static_cast<int>(data[offset + 5]) << 8) |
                            (static_cast<int>(data[offset + 6]) << 16) |
                            (static_cast<int>(data[offset + 7]) << 24);
    const uint16_t gain = static_cast<uint16_t>(data[offset + 8]) |
                          (static_cast<uint16_t>(data[offset + 9]) << 8);
    out.output_gain_q8 = static_cast<int16_t>(gain);
    out.mapping_family = data[offset + 10];

    if (out.mapping_family != 0) {
        if (size < offset + 13) return false;
        out.stream_count = data[offset + 11];
        out.coupled_count = data[offset + 12];
        if (out.channels > 0 && size < offset + 13 + out.channels) return false;
    }
    out.valid = out.version <= 15 && out.channels > 0;
    return out.valid;
}

bool AudioBitstreamParser::ParseOpusToc(const uint8_t* data, size_t size, OpusToc& out) {
    out = OpusToc{};
    if (data == nullptr || size < 1) return false;
    const uint8_t toc = data[0];
    out.config = (toc >> 3) & 0x1F;
    out.stereo = (toc & 0x04) != 0;
    out.frame_count_code = toc & 0x03;
    out.frame_bytes = static_cast<int>(size);

    // config -> mode / bandwidth / frame duration
    static const int kMode[32] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    };
    static const int kBandwidth[32] = {
        0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4,
        0, 0, 0, 0, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    };
    static const double kFrameMs[32] = {
        10, 20, 40, 60, 10, 20, 40, 60, 10, 20, 40, 60, 10, 20, 10, 20,
        2.5, 5, 10, 20, 2.5, 5, 10, 20, 2.5, 5, 10, 20, 2.5, 5, 10, 20,
    };
    out.mode = kMode[out.config];
    out.bandwidth = kBandwidth[out.config];
    out.frame_duration_ms = kFrameMs[out.config];

    switch (out.frame_count_code) {
        case 0: out.frame_count = 1; break;
        case 1:
        case 2: out.frame_count = 2; break;
        case 3:
            if (size < 2) return false;
            out.frame_count = data[1] & 0x3F;
            break;
        default: break;
    }
    out.channels = out.stereo ? 2 : 1;
    out.valid = out.frame_count > 0 && out.frame_count <= 48;
    return out.valid;
}

bool AudioBitstreamParser::ParseAacAudioSpecificConfig(const uint8_t* data, size_t size,
                                                       AacAudioSpecificConfig& out) {
    out = AacAudioSpecificConfig{};
    if (data == nullptr || size == 0) return false;
    BitReader br(data, size);
    uint32_t value = 0;
    if (!br.Read(5, value)) return false;
    int aot = static_cast<int>(value);
    if (aot == 31) {
        if (!br.Read(6, value)) return false;
        aot = 32 + static_cast<int>(value);
    }
    out.audio_object_type = aot;

    if (!br.Read(4, value)) return false;
    out.sampling_frequency_index = static_cast<int>(value);
    if (out.sampling_frequency_index == 15) {
        if (!br.Read(24, value)) return false;
        out.sample_rate = static_cast<int>(value);
    } else {
        out.sample_rate = AacSampleRateFromIndex(out.sampling_frequency_index);
    }
    if (!br.Read(4, value)) return false;
    out.channel_configuration = static_cast<int>(value);
    out.channels = AacChannelsFromConfig(out.channel_configuration);

    if (aot == 5 || aot == 29) {
        out.sbr = true;
        out.ps = (aot == 29);
        if (!br.Read(4, value)) return false;
        const int ext_index = static_cast<int>(value);
        if (ext_index == 15) {
            if (!br.Read(24, value)) return false;
            out.extension_sample_rate = static_cast<int>(value);
        } else {
            out.extension_sample_rate = AacSampleRateFromIndex(ext_index);
        }
        if (out.extension_sample_rate > 0) out.sample_rate = out.extension_sample_rate;
    } else if (aot == 22) {
        if (!br.Read(4, value)) return false;
        out.channel_configuration = static_cast<int>(value);
        out.channels = AacChannelsFromConfig(out.channel_configuration);
    }
    out.valid = out.sample_rate > 0;
    return out.valid;
}

}  // namespace videoeye
