#pragma once

// 音频位流头解析（D1）：纯 C++17，不依赖 FFmpeg / Qt。
//
// 只解析"交付审核需要、又不值得为此解码"的头字段：
//   * ADTS：MPEG 版本 / profile / 采样率 / 声道配置 / 帧长 / 每帧 raw_data_block 数；
//   * LATM/LOAS：AudioMuxElement 的 StreamMuxConfig（AOT / 采样率 / 声道 / 帧长类型）；
//   * AC-3：fscod / frmsizecod / bsid / bsmod / acmod / lfeon / dialnorm / 码率；
//   * E-AC-3：strmtyp / substreamid / frmsiz / acmod / dialnorm / chanmap；
//   * Opus：OpusHead 与 TOC（config / 立体声标志 / 帧数 / 帧长 / 模式 / 带宽）；
//   * AAC AudioSpecificConfig（MP4 extradata 的 5 字节头）。
//
// 不做的事情：不校验 CRC、不解析音频 payload、不判断响度（响度必须解码后算）。
// 这些头字段足够支撑"码流声明 vs 容器声明 vs 实测"三方差值。

#include <cstddef>
#include <cstdint>
#include <string>

namespace videoeye {

// ADTS 头（7 字节；protection_absent == 0 时多 2 字节 CRC，共 9 字节）
struct AdtsHeader {
    bool valid = false;
    int mpeg_version = 4;             // 4 = MPEG-4，2 = MPEG-2
    int audio_object_type = 0;        // profile + 1（2 = AAC LC）
    int sampling_frequency_index = 0;
    int sample_rate = 0;
    int channel_configuration = 0;
    int channels = 0;
    int frame_length = 0;             // 含头
    int header_bytes = 7;
    int num_raw_data_blocks = 0;      // 每帧 AAC 帧数 = num_raw_data_blocks + 1
    bool protection_absent = true;
};

// LATM/LOAS 的 StreamMuxConfig 关键字段
struct LatmHeader {
    bool valid = false;
    int audio_object_type = 0;
    int sampling_frequency_index = 0;
    int sample_rate = 0;
    int channel_configuration = 0;
    int channels = 0;
    int frame_length_type = -1;
    int frame_length = 0;
    int num_sub_frames = 0;
    int audio_mux_version = 0;
    bool crc_check_present = false;
    bool use_same_stream_mux = false;
    std::string note;
};

// AC-3 / E-AC-3 同步帧头（只解析 BSI 前半段）
struct Ac3Header {
    bool valid = false;
    bool eac3 = false;
    int bsid = -1;
    int bsmod = -1;
    int sample_rate = 0;
    int bit_rate_kbps = 0;
    int frame_size_bytes = 0;
    int frame_samples = 0;            // AC-3 固定 1536；E-AC-3 = numblkscod 对应块数 * 256
    int num_blocks = 0;
    int acmod = -1;
    bool lfeon = false;
    int channels = 0;                 // acmod + lfe（E-AC-3 不含从属子流）
    bool has_dialnorm = false;
    int dialnorm_dbfs = 0;
    int strmtyp = -1;                 // E-AC-3 only
    int substreamid = -1;             // E-AC-3 only
    bool has_chanmap = false;
    uint16_t chanmap = 0;
    int chanmap_channels = 0;
    bool atmos_joc = false;
    // 这个判定**不是**规范级解析：只是在载荷前 96 字节里找 ASCII "joc"。
    // 置 true 表示"疑似 Atmos（启发式线索）"，调用方必须按线索而非结论呈现。
    bool atmos_heuristic = false;
    int joc_complexity_index = -1;
};

// OpusHead（Ogg / Matroska / MP4 的 dOps 前 19 字节）
struct OpusHead {
    bool valid = false;
    int version = 0;
    int channels = 0;
    int pre_skip = 0;
    int input_sample_rate = 0;
    int output_gain_q8 = 0;
    int mapping_family = 0;
    int stream_count = 0;
    int coupled_count = 0;
};

// Opus 包 TOC
struct OpusToc {
    bool valid = false;
    int config = 0;
    bool stereo = false;
    int frame_count_code = 0;
    int frame_count = 1;
    int mode = -1;                    // 0 = SILK / 1 = Hybrid / 2 = CELT
    int bandwidth = -1;               // 0..4
    double frame_duration_ms = 0.0;
    int channels = 0;
    int frame_bytes = 0;
};

// AAC AudioSpecificConfig（MP4 extradata / LATM 共用）
struct AacAudioSpecificConfig {
    bool valid = false;
    int audio_object_type = 0;
    int sampling_frequency_index = 0;
    int sample_rate = 0;
    int channel_configuration = 0;
    int channels = 0;
    bool sbr = false;                 // HE-AAC
    bool ps = false;                  // HE-AACv2
    int extension_sample_rate = 0;
    std::string note;
};

class AudioBitstreamParser {
public:
    // ADTS：data 必须从 syncword 开始
    static bool ParseAdts(const uint8_t* data, size_t size, AdtsHeader& out);

    // LOAS：返回 audioMuxLengthBytes 与 useSameStreamMux（这两项在加密/复用场景也要用）
    static bool ParseLoasHeader(const uint8_t* data, size_t size, int& audio_mux_length_bytes,
                                bool& use_same_stream_mux);
    // 解析 StreamMuxConfig；data 指向 AudioMuxElement 的 useSameStreamMux 位之后
    static bool ParseLatmStreamMuxConfig(const uint8_t* data, size_t size, LatmHeader& out);

    // AC-3 / E-AC-3。force_eac3 由调用方按 codec id 给出；为 false 时按 AC-3 解析。
    static bool ParseAc3(const uint8_t* data, size_t size, bool force_eac3, Ac3Header& out);

    static bool ParseOpusHead(const uint8_t* data, size_t size, OpusHead& out);
    static bool ParseOpusToc(const uint8_t* data, size_t size, OpusToc& out);

    static bool ParseAacAudioSpecificConfig(const uint8_t* data, size_t size,
                                            AacAudioSpecificConfig& out);

    // 采样率表 / 声道配置表，单测与 UI 共用
    static int AacSampleRateFromIndex(int index);
    static int AacChannelsFromConfig(int channel_configuration);
    static int Ac3FrameSizeBytes(int fscod, int frmsizecod);
};

}  // namespace videoeye
