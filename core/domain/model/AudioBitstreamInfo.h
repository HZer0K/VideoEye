#pragma once

// 音频位流参数（D1）。
//
// 音频 QC 的响度/真峰值是**解码后**从 PCM 算的；但交付审核里还有一半问题
// 出在**码流头**上：ADTS 的采样率/声道配置与容器不一致、AC-3 的 dialnorm
// 与实测响度差一大截、E-AC-3 到底是普通 5.1 还是 Atmos/JOC、Opus 的
// pre-skip / output gain 被封装改坏。这些信息不解码也能拿到，而且必须在
// 解复用阶段逐包看才能发现"中途变参数"。
//
// 本文件只放结果模型，纯 C++17，不依赖 FFmpeg / Qt：
//   * 位流解析 -> core/analysis/codec/AudioBitstreamParser.h
//   * FFmpeg 侧登记流/喂包 -> core/analysis/quality/AudioBitstreamAnalyzer.h
//
// QC 规则 id 前缀为 "audio.bitstream." / "audio.dialnorm."。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 解析到的音频位流类型
enum class AudioBitstreamCodec {
    Unknown = 0,
    AacAdts,   // ADTS（裸 AAC / TS / 部分 MP4）
    AacLatm,   // LATM / LOAS
    AacRaw,    // MP4 的 AudioSpecificConfig + 裸 AAC 帧
    Ac3,       // AC-3
    Eac3,      // E-AC-3（含 Atmos/JOC）
    Opus,      // Ogg / Matroska / WebM Opus
    Other,     // 未实现逐帧解析的编码（MP3 / FLAC / PCM ...）
};

// E-AC-3 的独立/从属子流类型
enum class Eac3StreamType {
    Independent = 0,   // strmtyp == 0
    Dependent = 1,     // strmtyp == 1
    Converted = 2,     // strmtyp == 2
    Reserved = 3,      // strmtyp == 3
};

// 一条音频流在码流层看到的事实
struct AudioBitstreamStream {
    int stream_index = -1;
    std::string codec_name;   // avcodec_get_name()
    std::string codec_tag;    // MP4 四字符 tag（可能为空）
    AudioBitstreamCodec codec = AudioBitstreamCodec::Unknown;

    int64_t packet_count = 0;       // 交给分析器的包数
    int64_t parsed_packets = 0;     // 至少解析出一个帧头的包数
    int64_t invalid_packets = 0;    // 帧头解析失败的包数
    int64_t frame_count = 0;        // 解析出的帧数（AAC/AC-3 可能一包多帧）

    // 位流头里声明的参数
    int sample_rate = 0;
    int channels = 0;
    int channel_config = -1;        // AAC channel_configuration / AC-3 acmod
    int audio_object_type = -1;     // AAC Audio Object Type（2 = LC，5 = HE-AAC）
    std::string profile;            // 人类可读的 profile，如 "AAC LC" / "AC-3"

    // AC-3 / E-AC-3
    bool eac3 = false;
    int bsid = -1;
    int bsmod = -1;
    int acmod = -1;
    bool lfeon = false;
    bool has_dialnorm = false;
    int dialnorm_dbfs = 0;          // 已换算成 dBFS（通常是 -31..-1）
    // dialnorm 与实测积分响度的差值（LU）。只有"同一条流 + 音频 QC 跑过"时才有意义，
    // 所以用 available 显式区分"差值为 0"和"没算"。正值 = 实际比声明更响。
    bool dialnorm_delta_available = false;
    double dialnorm_delta_lu = 0.0;
    int bit_rate_kbps = 0;
    bool vbr = false;               // 同一流内码率/帧长明显变化
    int dependent_substream_count = 0;
    int dependent_channels = 0;     // 从属子流 chanmap 的声道数

    // Dolby Atmos / JOC（Joint Object Coding）
    bool atmos_joc = false;
    int joc_complexity_index = -1;
    std::string atmos_source;       // "E-AC-3 addbsi" / "dec3" / "..."

    // Opus
    bool opus_head = false;
    int pre_skip = 0;
    int input_sample_rate = 0;
    int output_gain_q8 = 0;         // Q7.8 dB
    int mapping_family = 0;
    int stream_count = 0;
    int coupled_count = 0;
    int opus_mode = -1;             // 0 = SILK / 1 = Hybrid / 2 = CELT
    int opus_bandwidth = -1;        // 0 = NB / 1 = MB / 2 = WB / 3 = SWB / 4 = FB
    double opus_frame_ms = 0.0;

    // 容器侧参数（用于"码流 vs 容器"比对）
    int container_sample_rate = 0;
    int container_channels = 0;
    std::string container_channel_layout;

    // 解析过程中发现的不一致（人类可读；空 = 没发现）
    std::vector<std::string> inconsistencies;
    // 同一批事实的**结构化**版本：QC 规则要按类判定，靠 strings 匹配太脆。
    // container_mismatch_fields = 采样率/声道两项里对不上的条数。
    bool container_mismatch = false;
    int container_mismatch_fields = 0;
    // 扫描中位流参数中途改变（采样率/声道数/码率任一）
    bool params_changed_midstream = false;
    // 降级/解析说明
    std::string note;

    // 已换算成 dB 的 Opus output gain
    double OutputGainDb() const { return static_cast<double>(output_gain_q8) / 256.0; }
    bool HasBitstreamParams() const { return sample_rate > 0 || channels > 0; }
};

// 一次音频位流分析的汇总
struct AudioBitstreamResult {
    bool analyzed = false;
    std::vector<AudioBitstreamStream> streams;

    int parsed_stream_count = 0;
    int atmos_stream_count = 0;
    std::vector<std::string> notes;

    std::string ToString() const;
};

// 显示用名称
const char* ToString(AudioBitstreamCodec codec);
const char* AudioBitstreamCodecCode(AudioBitstreamCodec codec);
const char* AudioObjectTypeName(int audio_object_type);
const char* OpusModeName(int mode);
const char* OpusBandwidthName(int bandwidth);

// AC-3 acmod -> 基础声道数（不含 LFE）。
//   0 = 1+1（双单声道，2 声道） / 1 = 1/0 / 2 = 2/0 / 3 = 3/0 /
//   4 = 2/1 / 5 = 3/1 / 6 = 2/2 / 7 = 3/2
int AcmodChannelCount(int acmod);
// acmod + lfeon -> 可读布局，如 "3/2 + LFE (5.1)"
std::string DescribeAcmod(int acmod, bool lfeon);

}  // namespace model
}  // namespace videoeye
