#pragma once

// 音频位流头解析（D1）：纯 C++17，不依赖 FFmpeg / Qt。
//
// 只解析"交付审核需要、又不值得为此解码"的头字段：
//   * ADTS：MPEG 版本 / profile / 采样率 / 声道配置 / 帧长 / 每帧 raw_data_block 数；
//   * LATM/LOAS：AudioMuxElement 的 StreamMuxConfig（AOT / 采样率 / 声道 / 帧长类型）；
//   * AC-3：fscod / frmsizecod / bsid / bsmod / acmod / lfeon / dialnorm / 码率；
//   * E-AC-3：strmtyp / substreamid / frmsiz / acmod / dialnorm / chanmap；
//   * Opus：OpusHead 与 TOC（config / 立体声标志 / 帧数 / 帧长 / 模式 / 带宽）；
//   * AAC AudioSpecificConfig（MP4 extradata 的 5 字节头）、PCE、ADTS CRC-16；
//   * MP3：帧头（版本/层/码率/采样率/声道模式）+ Xing/Info/VBRI/LAME 延迟；
//   * FLAC：STREAMINFO + 帧头（块大小/采样率/位深/CRC-8）。
//
// 不做的事情：不解析音频 payload、不判断响度（响度必须解码后算）。
// CRC 只对**能明确定义覆盖范围**的做校验（ADTS / FLAC 帧头）；MP3 的 CRC-16
// 覆盖范围随保护位与 side info 变化，当前只报"存在"不报"通过"。
// 这些头字段足够支撑"码流声明 vs 容器声明 vs 实测"三方差值。

#include <cstddef>
#include <cstdint>
#include <string>

namespace videoeye {

// ADTS 头（7 字节；protection_absent == 0 时多 2 字节 CRC，共 9 字节）
struct AdtsHeader {
    bool valid = false;
    int mpeg_version = 4;      // 4 = MPEG-4，2 = MPEG-2
    int audio_object_type = 0; // profile + 1（2 = AAC LC）
    int sampling_frequency_index = 0;
    int sample_rate = 0;
    int channel_configuration = 0;
    int channels = 0;
    int frame_length = 0; // 含头
    int header_bytes = 7;
    int num_raw_data_blocks = 0; // 每帧 AAC 帧数 = num_raw_data_blocks + 1
    bool protection_absent = true;
    // 同步字与头字段都读对了，但声明的 frame_length 超过本包剩余字节。
    // ❗ 这种包**不是**"解析成功"：以前 valid=true 让调用方 ++parsed_packets，
    // 截断数据被算成解析成功，损坏率因此偏低。调用方必须单独看这个标志。
    bool truncated = false;
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
    int frame_samples = 0; // AC-3 固定 1536；E-AC-3 = numblkscod 对应块数 * 256
    int num_blocks = 0;
    int acmod = -1;
    bool lfeon = false;
    int channels = 0; // acmod + lfe（E-AC-3 不含从属子流）
    bool has_dialnorm = false;
    int dialnorm_dbfs = 0;
    int strmtyp = -1;     // E-AC-3 only
    int substreamid = -1; // E-AC-3 only
    bool has_chanmap = false;
    uint16_t chanmap = 0;
    int chanmap_channels = 0;
    bool atmos_joc = false;
    // 这个判定**不是**规范级解析：只是在载荷前 96 字节里找 ASCII "joc"。
    // 置 true 表示"疑似 Atmos（启发式线索）"，调用方必须按线索而非结论呈现。
    bool atmos_heuristic = false;
    int joc_complexity_index = -1;
};

// Opus 的配置头。两种封装**字节序不同**，字段却完全一样：
//   * Ogg / Matroska：`OpusHead` magic + 小端（RFC 7845）
//   * MP4 / MOV：`dOps` box 的主体，没有 magic + **大端**（ISO/IEC 14496-3）
// 以前两条路共用一段小端解码，走 MP4 那条时 pre_skip / input_rate 全是垃圾数。
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
    bool big_endian = false; // 记录实际走的是哪条（dOps = true）
};

// Opus 包 TOC（RFC 6716 §3.1 / §3.2.1）
struct OpusToc {
    bool valid = false;
    int config = 0;
    bool stereo = false;
    int frame_count_code = 0;
    int frame_count = 1;
    // ❗ 只有 **code 3** 在第二个字节里带 vbr / padding 标志（bit7 / bit6）。
    // code 1 是"两帧等长、无额外字节"，code 2 是"两帧不等长、紧跟一个长度字段"——
    // 把 code 1 的第 2 字节当 vbr 标志读，看到的是第一帧的载荷首字节。
    // "TOC 读得出来"和"包结构合法"是两件事，后者要靠这里的结构判定。
    bool vbr = false;
    bool padding = false;
    int mode = -1;      // 0 = SILK / 1 = Hybrid / 2 = CELT
    int bandwidth = -1; // 0..4
    double frame_duration_ms = 0.0;
    int channels = 0;
    int frame_bytes = 0;       // 整个包的字节数
    int payload_offset = 1;    // 第一个帧载荷的起点（code 2 要跳过长度字段）
    int first_frame_bytes = 0; // code 1/2 的单帧字节数（0 = 不由 TOC 决定）
};

// AAC AudioSpecificConfig（MP4 extradata / LATM 共用）
struct AacAudioSpecificConfig {
    bool valid = false;
    int audio_object_type = 0;
    int sampling_frequency_index = 0;
    int sample_rate = 0;
    int channel_configuration = 0;
    int channels = 0;
    bool sbr = false; // HE-AAC
    bool ps = false;  // HE-AACv2
    int extension_sample_rate = 0;
    std::string note;

    // ---- SBR / PS 的证据来源 ----
    //
    // 早先只看 AOT 是不是 5/29。但 HE-AAC 在 MP4/MOV 里更常见的写法是
    // "core 仍写 LC(AOT=2)，末尾追加 11 bit syncExtensionType=0x2B7
    //  + sbrPresentFlag，再跟 0x548 + psPresentFlag"（ISO/IEC 14496-3 §1.6.2.1）。
    // 只认 AOT 会把这类文件全判成 LC：采样率报 core 的 24 kHz（实际输出 48 kHz）、
    // 声道报 1（PS 上混成立体声后是 2）。所以必须把 sync extension 也读懂，
    // 并且**记住证据从哪来** —— "AOT 直接声明"和"同步扩展补充"可信度不同，
    // 而"读到了扩展但 sbrPresentFlag=0"是第三种、也是最容易被忽略的结论：
    // 明确声明没有 SBR。
    int core_sample_rate = 0;  // ASC 里 samplingFrequencyIndex 对应的值（SBR 之前）
    bool sbr_signaled = false; // 是否有任何显式信令（AOT 或 sync extension）表态
    bool sbr_from_sync_extension = false;
    bool ps_from_sync_extension = false;
    std::string sbr_source; // "aot" / "sync_extension" / 空（未表态）
};

// 一个 MPEG-1/2 Audio（MP3）帧头。
//
// 为什么要单独做：MP3 是交付素材里最常见的"没有 extradata"的音频编码，
// 采样率/声道数/码率全在逐帧头里。以前它落进 Other 分支只登记容器参数，
// 于是"码流声明 44.1 kHz、容器写 48 kHz"这类问题永远看不到。
struct Mp3FrameHeader {
    bool valid = false;
    int mpeg_version = 1; // 1 = MPEG-1，2 = MPEG-2，25 = MPEG-2.5
    int layer = 3;        // 1 / 2 / 3
    int bitrate_kbps = 0;
    int sample_rate = 0;
    int channels = 0;
    int channel_mode = -1; // 0=stereo 1=joint stereo 2=dual channel 3=mono
    int samples_per_frame = 0;
    int frame_bytes = 0;      // 含 4 字节头
    bool crc_present = false; // protection_bit == 0
    bool padding = false;
    bool free_format = false; // bitrate_index == 0
    // 同步字与头字段读完了，但声明帧长超过本包剩余字节
    bool truncated = false;
};

// 首帧里的 VBR/编码器信息（Xing / Info / VBRI）
struct Mp3VbrInfo {
    bool valid = false;
    std::string tag;          // "Xing" / "Info" / "VBRI"
    int frame_count = 0;      // 声明的总帧数（0 = 未给）
    int64_t stream_bytes = 0; // 声明的总字节数（0 = 未给）
    bool has_toc = false;
    int quality = -1;
    // LAME / Lavc 扩展里的编码器延迟与尾部填充（单位：样本）
    bool has_encoder_delay = false;
    int encoder_delay = 0;
    int encoder_padding = 0;
    std::string encoder; // "LAME3.100" / "Lavc..." 等
};

// FLAC STREAMINFO（34 字节，可带 "fLaC" magic）
struct FlacStreamInfo {
    bool valid = false;
    int min_block_size = 0;
    int max_block_size = 0;
    int sample_rate = 0;
    int channels = 0;
    int bits_per_sample = 0;
    uint64_t total_samples = 0; // 0 = 未知（流式编码）
    bool has_magic = false;     // 输入是否带 "fLaC" 头（MP4/Matroska 里通常不带）
    bool md5_present = false;
};

// FLAC 帧头（14 bit 同步 + 块大小/采样率/声道/位深编码）
struct FlacFrameHeader {
    bool valid = false;
    int block_size = 0;
    int sample_rate = 0; // 0 = 沿用 STREAMINFO（编码值为 0）
    int channels = 0;
    int bits_per_sample = 0; // 0 = 沿用 STREAMINFO
    int channel_assignment = 0;
    bool variable_block_size = false;
    // 帧头 CRC-8 是否存在并通过；crc_checked=false 表示载荷不足，未校验
    bool crc_checked = false;
    bool crc_ok = false;
    bool truncated = false;
};

// AAC 的 Program Config Element（channel_configuration == 0 时才出现）。
//
// 为什么只做"尽力而为"：PCE 是 raw_data_block 里的一个语法元素，前面可能先出现
// SCE/CPE/FIL，要定位它必须把整个 raw_data_block 走完。这里只处理
// "raw_data_block 的第一个元素就是 PCE"这一种（编码器常规做法），
// 其它情况明确返回 false 并给出原因，绝不猜一个声道布局。
struct AacProgramConfig {
    bool valid = false;
    int element_instance_tag = 0;
    int object_type = 0;
    int sampling_frequency_index = 0;
    int sample_rate = 0;
    int front_channels = 0;
    int side_channels = 0;
    int back_channels = 0;
    int lfe_channels = 0;
    int channels = 0;
    std::string layout; // 例如 "3/2/1 + LFE"
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

    // 自动判定封装：有 "OpusHead" magic 按 Ogg（小端、≥19 字节），
    // 否则按 MP4 dOps（大端、≥11 字节）。偏移与字节序在**同一个分支里**一起定，
    // 不拆成两个独立判据 —— 拆开就一定会出现"offset 对了、字节序错了"的组合。
    static bool ParseOpusHead(const uint8_t* data, size_t size, OpusHead& out);
    // 明确来自 MP4 dOps（强制大端、无 magic）
    static bool ParseDOps(const uint8_t* data, size_t size, OpusHead& out);
    static bool ParseOpusToc(const uint8_t* data, size_t size, OpusToc& out);

    static bool ParseAacAudioSpecificConfig(const uint8_t* data, size_t size, AacAudioSpecificConfig& out);

    // ---- MP3（MPEG-1/2/2.5 Audio Layer I/II/III）----
    // data 必须从 4 字节帧头的第一个字节（0xFF）开始
    static bool ParseMp3FrameHeader(const uint8_t* data, size_t size, Mp3FrameHeader& out);
    // Xing/Info/VBRI 只出现在第一个帧里；hdr 必须是同一帧已解析成功的头
    static bool ParseMp3VbrInfo(const uint8_t* data, size_t size, const Mp3FrameHeader& hdr, Mp3VbrInfo& out);
    // MP3 帧头里的"每帧样本数"，供调用方换算时长
    static int Mp3SamplesPerFrame(int mpeg_version, int layer);

    // ---- FLAC ----
    // 接受两种输入：带 "fLaC" + METADATA_BLOCK_HEADER 的完整头，
    // 或 MP4/Matroska extradata 里的裸 34 字节 STREAMINFO
    static bool ParseFlacStreamInfo(const uint8_t* data, size_t size, FlacStreamInfo& out);
    static bool ParseFlacFrameHeader(const uint8_t* data, size_t size, FlacFrameHeader& out);

    // ---- AAC PCE ----
    // 只在 channel_configuration == 0 时有意义；data 指向 raw_data_block 起点
    static bool ParseAacProgramConfig(const uint8_t* data, size_t size, AacProgramConfig& out);

    // ❗ ADTS 的 CRC-16 覆盖范围在不同实现里不一致（是否含 adts_variable_header
    // 的尾部、是否含 crc_check 字段本身），拿不到参考实现前不做校验 ——
    // 一个口径错的 CRC 结论比没有结论更糟。ADTS 侧只报 protection_absent（见 AdtsHeader）。

    // 采样率表 / 声道配置表，单测与 UI 共用
    static int AacSampleRateFromIndex(int index);
    static int AacChannelsFromConfig(int channel_configuration);
    static int Ac3FrameSizeBytes(int fscod, int frmsizecod);
};

} // namespace videoeye
