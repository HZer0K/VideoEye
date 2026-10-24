// MP3 / FLAC / AAC-PCE 帧头解析的正反例回归测试（阶段 D2）。
//
// 为什么这些字节都要按规范逐位算出来而不是"用实现跑一遍把结果抄下来"：
// 2026-10 的 VP9 与 H.264 QP 两次事故都出在"合成数据与实现共享同一个错误假设"
// —— 数据是用实现自己生成/校验的，实现错了测试照样绿。
// 所以本文件的期望值一律来自：
//   * ISO/IEC 11172-3 / 13818-3（MP3 帧头字段与码率表）
//   * RFC 9639 / FLAC 格式规范（STREAMINFO 布局、帧头、CRC-8 poly=0x07）
//   * ISO/IEC 14496-3 表 4.5（program_config_element）
// 其中 CRC-8 的期望值 0x44 由独立实现（按 poly=0x07 手算）得出，不是从被测代码取的。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/analysis/codec/AudioBitstreamParser.h"

namespace {

using videoeye::AacProgramConfig;
using videoeye::AudioBitstreamParser;
using videoeye::FlacFrameHeader;
using videoeye::FlacStreamInfo;
using videoeye::Mp3FrameHeader;
using videoeye::Mp3VbrInfo;

// ---------------------------------------------------------------------------
// MP3 帧头
// ---------------------------------------------------------------------------

// MPEG-1 Layer III / 128 kbps / 44100 Hz / 立体声 / 无 CRC
//   0xFF 0xFB: 11 bit 同步 + version=11(MPEG-1) + layer=01(III) + protection=1
//   0x90     : bitrate_index=1001(9 → 128 kbps @MPEG1 L3) + rate_index=00(44100)
//   0x00     : channel_mode=00(立体声)
// 帧长 = 144 * 128000 / 44100 = 417.9 → 417，所以必须给满 417 字节才算完整一帧
namespace {

std::vector<uint8_t> MakeMp3Frame() {
    std::vector<uint8_t> frame(417, 0);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    frame[3] = 0x00;
    return frame;
}

} // namespace

TEST(Mp3FrameHeaderTest, ParsesMpeg1Layer3Stereo) {
    const std::vector<uint8_t> frame = MakeMp3Frame();
    Mp3FrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3FrameHeader(frame.data(), frame.size(), header));
    EXPECT_TRUE(header.valid);
    EXPECT_EQ(header.mpeg_version, 1);
    EXPECT_EQ(header.layer, 3);
    EXPECT_EQ(header.bitrate_kbps, 128);
    EXPECT_EQ(header.sample_rate, 44100);
    EXPECT_EQ(header.channels, 2);
    EXPECT_EQ(header.channel_mode, 0);
    EXPECT_EQ(header.samples_per_frame, 1152);
    EXPECT_FALSE(header.crc_present);
    EXPECT_FALSE(header.padding);
    EXPECT_EQ(header.frame_bytes, 417);
    EXPECT_FALSE(header.truncated);
}

// 反例：只给帧头，声明帧长 417 > 剩余字节。
// 契约是「返回 false 但把 truncated/各字段填好」——上层据此区分
// "包尾截断"（不算坏帧）与"同步字不对"（真坏帧）。两者混为一谈会让
// 正常的流式收尾被报成帧头校验失败。
TEST(Mp3FrameHeaderTest, FlagsTruncatedWhenFrameIncomplete) {
    const std::vector<uint8_t> header_only = {0xFF, 0xFB, 0x90, 0x00};
    Mp3FrameHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseMp3FrameHeader(header_only.data(), header_only.size(), header));
    EXPECT_TRUE(header.truncated);
    EXPECT_EQ(header.frame_bytes, 417); // 字段仍然填好，供上层判断
    EXPECT_FALSE(header.valid);
}

// MPEG-2 Layer III：samples_per_frame 减半到 576，采样率表也换一套
//   0xFF 0xF3: version=10(MPEG-2) + layer=01(III)
//   0x90     : bitrate_index=9 → 80 kbps @MPEG2 L3，rate_index=0 → 22050 Hz
// 帧长 = 72 * 80000 / 22050 = 261.2 → 261
TEST(Mp3FrameHeaderTest, ParsesMpeg2Layer3) {
    std::vector<uint8_t> frame(261, 0);
    frame[0] = 0xFF;
    frame[1] = 0xF3;
    frame[2] = 0x90;
    frame[3] = 0x00;
    Mp3FrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3FrameHeader(frame.data(), frame.size(), header));
    EXPECT_EQ(header.mpeg_version, 2);
    EXPECT_EQ(header.layer, 3);
    EXPECT_EQ(header.bitrate_kbps, 80);
    EXPECT_EQ(header.sample_rate, 22050);
    EXPECT_EQ(header.samples_per_frame, 576);
    EXPECT_EQ(header.frame_bytes, 261);
}

// 反例：layer 位是保留值 00（0xFF 0xF9）—— 必须拒绝，不能蒙一个 layer 出来
TEST(Mp3FrameHeaderTest, RejectsReservedLayer) {
    const std::vector<uint8_t> data = {0xFF, 0xF9, 0x90, 0x00};
    Mp3FrameHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseMp3FrameHeader(data.data(), data.size(), header));
    EXPECT_FALSE(header.valid);
}

// 反例：同步字差一位。0xFE 是常见误判来源（11 bit 同步只满足前 11 位不够）
TEST(Mp3FrameHeaderTest, RejectsBrokenSyncword) {
    const std::vector<uint8_t> data = {0xFE, 0xFB, 0x90, 0x00};
    Mp3FrameHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseMp3FrameHeader(data.data(), data.size(), header));
}

// 反例：bitrate_index=0 表示 free format，帧长不可定界。
// 契约与 truncated 一致：返回 false 但置 free_format，**不能**判成"坏帧"——
// 上层据此把这类包计为「未支持」而不是「帧头校验失败」。
TEST(Mp3FrameHeaderTest, FlagsFreeFormat) {
    const std::vector<uint8_t> data = {0xFF, 0xFB, 0x00, 0x00};
    Mp3FrameHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseMp3FrameHeader(data.data(), data.size(), header));
    EXPECT_TRUE(header.free_format);
    EXPECT_EQ(header.bitrate_kbps, 0);
    EXPECT_EQ(header.sample_rate, 44100); // 采样率仍然有效，能读到的字段照填
}

// 反例：buffer 不足 4 字节
TEST(Mp3FrameHeaderTest, RejectsShortBuffer) {
    const std::vector<uint8_t> data = {0xFF, 0xFB, 0x90};
    Mp3FrameHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseMp3FrameHeader(data.data(), data.size(), header));
}

TEST(Mp3FrameHeaderTest, SamplesPerFrameTable) {
    EXPECT_EQ(AudioBitstreamParser::Mp3SamplesPerFrame(1, 1), 384);
    EXPECT_EQ(AudioBitstreamParser::Mp3SamplesPerFrame(1, 2), 1152);
    EXPECT_EQ(AudioBitstreamParser::Mp3SamplesPerFrame(1, 3), 1152);
    // MPEG-2 / 2.5 的层 II 仍是 1152，层 III 降到 576（这是 MP3 时长估算最常错的一处）
    EXPECT_EQ(AudioBitstreamParser::Mp3SamplesPerFrame(2, 2), 1152);
    EXPECT_EQ(AudioBitstreamParser::Mp3SamplesPerFrame(2, 3), 576);
    EXPECT_EQ(AudioBitstreamParser::Mp3SamplesPerFrame(25, 3), 576);
}

// ---------------------------------------------------------------------------
// MP3 VBR 头（Xing / Info / VBRI）
// ---------------------------------------------------------------------------

namespace {

// 造一个"头部 + side info + 指定 tag"的 MPEG-1 立体声 Layer III 帧。
// MPEG-1 立体声 side info 是 32 字节，所以 tag 落在 4 + 32 = 36。
std::vector<uint8_t> MakeMp3FrameWithTag(const char* tag) {
    std::vector<uint8_t> frame(417, 0);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    frame[3] = 0x00;
    std::memcpy(frame.data() + 36, tag, 4);
    return frame;
}

} // namespace

TEST(Mp3VbrInfoTest, ParsesXingFrameCountAndBytes) {
    std::vector<uint8_t> frame = MakeMp3FrameWithTag("Xing");
    const size_t p = 36 + 4;
    // flags = 0x0003：后跟 frames 与 bytes 两个 4 字节字段
    frame[p + 0] = 0x00;
    frame[p + 1] = 0x00;
    frame[p + 2] = 0x00;
    frame[p + 3] = 0x03;
    frame[p + 4] = 0x00;
    frame[p + 5] = 0x00;
    frame[p + 6] = 0x04;
    frame[p + 7] = 0xD2; // frames = 1234
    frame[p + 8] = 0x00;
    frame[p + 9] = 0x00;
    frame[p + 10] = 0xDD;
    frame[p + 11] = 0xD5; // bytes = 56789

    Mp3FrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3FrameHeader(frame.data(), frame.size(), header));
    Mp3VbrInfo info;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3VbrInfo(frame.data(), frame.size(), header, info));
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.tag, "Xing");
    EXPECT_EQ(info.frame_count, 1234);
    EXPECT_EQ(info.stream_bytes, 56789);
}

// "Info" = CBR 文件里的同类头，参数布局与 Xing 完全相同，只有 tag 不同。
// 分开测是因为把它当 Xing 会让"VBR 判定"全部误报。
TEST(Mp3VbrInfoTest, ParsesInfoTagAsCbrMarker) {
    std::vector<uint8_t> frame = MakeMp3FrameWithTag("Info");
    const size_t p = 36 + 4;
    frame[p + 3] = 0x01; // flags = frames only
    frame[p + 4] = 0x00;
    frame[p + 5] = 0x00;
    frame[p + 6] = 0x03;
    frame[p + 7] = 0xE8; // frames = 1000

    Mp3FrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3FrameHeader(frame.data(), frame.size(), header));
    Mp3VbrInfo info;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3VbrInfo(frame.data(), frame.size(), header, info));
    EXPECT_EQ(info.tag, "Info");
    EXPECT_EQ(info.frame_count, 1000);
    EXPECT_EQ(info.stream_bytes, 0); // 没声明就是 0，不能瞎填
}

// VBRI 固定在帧头之后 32 字节（与 Xing 的 36 不同），这是最容易搞混的一处
TEST(Mp3VbrInfoTest, ParsesVbriAtFixed32Offset) {
    std::vector<uint8_t> frame(417, 0);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    frame[3] = 0x00;
    std::memcpy(frame.data() + 36, "VBRI", 4);

    Mp3FrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3FrameHeader(frame.data(), frame.size(), header));
    Mp3VbrInfo info;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3VbrInfo(frame.data(), frame.size(), header, info));
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.tag, "VBRI");
}

// 反例：Xing/Info 只存在于 Layer III。层 I/II 帧即便 36 字节处恰好是 "Xing"
// 也不能认为有 VBR 头，否则普通 CBR 音频会被判成 VBR。
TEST(Mp3VbrInfoTest, RejectsForNonLayer3Frame) {
    // MPEG-1 Layer II：0xFF 0xFD（layer bits = 10）
    // 帧长 = 144 * 160000 / 44100 = 522，所以缓冲要够长才不会先判 truncated
    std::vector<uint8_t> frame(1024, 0);
    frame[0] = 0xFF;
    frame[1] = 0xFD;
    frame[2] = 0x90;
    frame[3] = 0x00;
    std::memcpy(frame.data() + 36, "Xing", 4);

    Mp3FrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3FrameHeader(frame.data(), frame.size(), header));
    EXPECT_EQ(header.layer, 2);
    Mp3VbrInfo info;
    EXPECT_FALSE(AudioBitstreamParser::ParseMp3VbrInfo(frame.data(), frame.size(), header, info));
}

// 反例：没有 VBR 头的普通帧
TEST(Mp3VbrInfoTest, ReturnsFalseWithoutTag) {
    std::vector<uint8_t> frame(417, 0xAA);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    frame[3] = 0x00;

    Mp3FrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseMp3FrameHeader(frame.data(), frame.size(), header));
    Mp3VbrInfo info;
    EXPECT_FALSE(AudioBitstreamParser::ParseMp3VbrInfo(frame.data(), frame.size(), header, info));
}

// ---------------------------------------------------------------------------
// FLAC STREAMINFO
// ---------------------------------------------------------------------------

TEST(FlacStreamInfoTest, ParsesBare34ByteStreamInfo) {
    // 直接用 STREAMINFO 的规范位布局拼一遍，确保期望值来自 RFC 而不是实现
    std::vector<uint8_t> b(34, 0);
    b[0] = 0x10;
    b[1] = 0x00; // min_block = 4096
    b[2] = 0x10;
    b[3] = 0x00; // max_block = 4096
    // 从第 10 字节开始：20 bit 采样率 | 3 bit 声道-1 | 5 bit 位深-1 | 36 bit 总样本数
    // 44100 → 0x0AC44；channels-1 = 1；bits-1 = 15；total = 441000
    const uint64_t packed = (static_cast<uint64_t>(44100) << 44) | (static_cast<uint64_t>(1) << 41) |
                            (static_cast<uint64_t>(15) << 36) | static_cast<uint64_t>(441000);
    for (int i = 0; i < 8; ++i)
        b[10 + i] = static_cast<uint8_t>((packed >> (56 - 8 * i)) & 0xFF);

    FlacStreamInfo info;
    ASSERT_TRUE(AudioBitstreamParser::ParseFlacStreamInfo(b.data(), b.size(), info));
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.min_block_size, 4096);
    EXPECT_EQ(info.max_block_size, 4096);
    EXPECT_EQ(info.sample_rate, 44100);
    EXPECT_EQ(info.channels, 2);
    EXPECT_EQ(info.bits_per_sample, 16);
    EXPECT_EQ(info.total_samples, 441000u);
    EXPECT_FALSE(info.has_magic); // MP4 / Matroska 的 extradata 不带 fLaC magic
}

TEST(FlacStreamInfoTest, ParsesWithFlacMagicAndBlockHeader) {
    std::vector<uint8_t> b(34, 0);
    b[0] = 0x0B;
    b[1] = 0xB8; // min_block = 3000
    b[2] = 0x0B;
    b[3] = 0xB8; // max_block = 3000
    const uint64_t packed = (static_cast<uint64_t>(48000) << 44) | (static_cast<uint64_t>(1) << 41) |
                            (static_cast<uint64_t>(23) << 36) | static_cast<uint64_t>(0); // 24 bit，总样本数未知
    for (int i = 0; i < 8; ++i)
        b[10 + i] = static_cast<uint8_t>((packed >> (56 - 8 * i)) & 0xFF);

    std::vector<uint8_t> file = {'f', 'L', 'a', 'C'};
    file.push_back(0x80); // last=1, type=0(STREAMINFO)
    file.push_back(0x00);
    file.push_back(0x00);
    file.push_back(0x22); // 长度 34
    file.insert(file.end(), b.begin(), b.end());

    FlacStreamInfo info;
    ASSERT_TRUE(AudioBitstreamParser::ParseFlacStreamInfo(file.data(), file.size(), info));
    EXPECT_TRUE(info.has_magic);
    EXPECT_EQ(info.sample_rate, 48000);
    EXPECT_EQ(info.bits_per_sample, 24);
    // 流式编码不写总样本数：0 表示未知，报告里不能当成"0 个样本"
    EXPECT_EQ(info.total_samples, 0u);
}

// 反例：不足 34 字节
TEST(FlacStreamInfoTest, RejectsShortBuffer) {
    std::vector<uint8_t> b(20, 0);
    FlacStreamInfo info;
    EXPECT_FALSE(AudioBitstreamParser::ParseFlacStreamInfo(b.data(), b.size(), info));
    EXPECT_FALSE(info.valid);
}

// 反例：第一个 metadata block 不是 STREAMINFO（type=1 = PADDING）
TEST(FlacStreamInfoTest, RejectsNonStreaminfoFirstBlock) {
    std::vector<uint8_t> file = {'f', 'L', 'a', 'C', 0x81, 0x00, 0x00, 0x22};
    file.resize(8 + 34, 0);
    FlacStreamInfo info;
    EXPECT_FALSE(AudioBitstreamParser::ParseFlacStreamInfo(file.data(), file.size(), info));
}

// ---------------------------------------------------------------------------
// FLAC 帧头 + CRC-8
// ---------------------------------------------------------------------------

namespace {

// 0xFF 0xF8: 14 bit 同步 + reserved=0 + variable=0
// 0x89     : block_code=8(256 样本) + rate_code=9(44100)
// 0x18     : channel_code=1(2 声道) + depth_code=4(16 bit) + reserved=0
// 0x00     : UTF-8 帧号 0
// 0x44     : CRC-8（poly=0x07，覆盖前 5 字节；由独立实现手算得出）
const std::vector<uint8_t> kFlacFrameHeader = {0xFF, 0xF8, 0x89, 0x18, 0x00, 0x44};

} // namespace

TEST(FlacFrameHeaderTest, ParsesHeaderAndVerifiesCrc8) {
    FlacFrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseFlacFrameHeader(kFlacFrameHeader.data(), kFlacFrameHeader.size(), header));
    EXPECT_TRUE(header.valid);
    EXPECT_EQ(header.block_size, 256);
    EXPECT_EQ(header.sample_rate, 44100);
    EXPECT_EQ(header.channels, 2);
    EXPECT_EQ(header.bits_per_sample, 16);
    EXPECT_EQ(header.channel_assignment, 1);
    EXPECT_FALSE(header.variable_block_size);
    EXPECT_TRUE(header.crc_checked);
    EXPECT_TRUE(header.crc_ok);
}

// 反例：改动一个被 CRC 覆盖的字节，CRC 必须对不上。
// 这条正是"截断/损坏"识别的核心 —— 只要 CRC 不校验，坏帧会被算成好帧。
TEST(FlacFrameHeaderTest, DetectsCrc8Mismatch) {
    std::vector<uint8_t> broken = kFlacFrameHeader;
    broken[4] = 0x01; // 帧号 0 → 1，CRC 不再匹配
    FlacFrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseFlacFrameHeader(broken.data(), broken.size(), header));
    EXPECT_TRUE(header.crc_checked);
    EXPECT_FALSE(header.crc_ok);
}

// 反例：同步字不对（0xF8 前面必须是 0xFF，且 data[1] 高 6 位要全 1）
TEST(FlacFrameHeaderTest, RejectsBrokenSync) {
    std::vector<uint8_t> data = kFlacFrameHeader;
    data[1] = 0xF0;
    FlacFrameHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseFlacFrameHeader(data.data(), data.size(), header));
}

// 反例：保留位非 0（data[1] bit1）
TEST(FlacFrameHeaderTest, RejectsReservedBit) {
    std::vector<uint8_t> data = kFlacFrameHeader;
    data[1] |= 0x02;
    FlacFrameHeader header;
    EXPECT_FALSE(AudioBitstreamParser::ParseFlacFrameHeader(data.data(), data.size(), header));
}

// 反例：缺 CRC 字节时只能标 truncated，不能凭空判"CRC 通过"。
// block_code=7 会多吃 2 字节块大小，于是 CRC 落在缓冲之外。
TEST(FlacFrameHeaderTest, MarksTruncatedWhenCrcMissing) {
    const std::vector<uint8_t> data = {0xFF, 0xF8, 0x79, 0x18, 0x00, 0x00, 0x00};
    FlacFrameHeader header;
    ASSERT_TRUE(AudioBitstreamParser::ParseFlacFrameHeader(data.data(), data.size(), header));
    EXPECT_FALSE(header.crc_checked);
    EXPECT_FALSE(header.crc_ok);
    EXPECT_TRUE(header.truncated);
}

// ---------------------------------------------------------------------------
// AAC Program Config Element
// ---------------------------------------------------------------------------

namespace {

// 5.1 布局的 PCE（按 ISO/IEC 14496-3 表 4.5 逐位拼出）：
//   id_syn_ele=5(PCE) / tag=0 / object_type=LC / sf_index=4(44100)
//   front=1(CPE→2ch) / side=1(CPE→2ch) / back=1(SCE→1ch) / lfe=1
//   num_assoc=0（3 bit）/ num_cc=0
// 合计 6 声道，布局文本 "2/2/1 + LFE"
const std::vector<uint8_t> kPce51 = {0xA1, 0x20, 0x88, 0x88, 0x01, 0x08, 0x88, 0xC0};

} // namespace

TEST(AacProgramConfigTest, ParsesFiveOneLayout) {
    AacProgramConfig pce;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacProgramConfig(kPce51.data(), kPce51.size(), pce));
    EXPECT_TRUE(pce.valid);
    EXPECT_EQ(pce.element_instance_tag, 0);
    EXPECT_EQ(pce.object_type, 2); // LC
    EXPECT_EQ(pce.sampling_frequency_index, 4);
    EXPECT_EQ(pce.sample_rate, 44100);
    EXPECT_EQ(pce.front_channels, 2);
    EXPECT_EQ(pce.side_channels, 2);
    EXPECT_EQ(pce.back_channels, 1);
    EXPECT_EQ(pce.lfe_channels, 1);
    EXPECT_EQ(pce.channels, 6);
    EXPECT_EQ(pce.layout, "2/2/1 + LFE");
}

// 反例：首个元素是 ID_END(7)，说明 raw_data_block 是空的
TEST(AacProgramConfigTest, RejectsEndElementOnly) {
    // 111 = ID_END，后面补 0 让 total 可读
    const std::vector<uint8_t> data = {0xE0, 0x00, 0x00, 0x00};
    AacProgramConfig pce;
    EXPECT_FALSE(AudioBitstreamParser::ParseAacProgramConfig(data.data(), data.size(), pce));
    EXPECT_FALSE(pce.note.empty());
}

// 反例：首个元素是 SCE(0)，PCE 只能在遍历完前序元素后定位，本实现明确不做
TEST(AacProgramConfigTest, RejectsNonPceFirstElement) {
    const std::vector<uint8_t> data = {0x00, 0x11, 0x22, 0x33};
    AacProgramConfig pce;
    EXPECT_FALSE(AudioBitstreamParser::ParseAacProgramConfig(data.data(), data.size(), pce));
    EXPECT_FALSE(pce.note.empty());
}

// 反例：buffer 太短
TEST(AacProgramConfigTest, RejectsShortBuffer) {
    const std::vector<uint8_t> data = {0xA1};
    AacProgramConfig pce;
    EXPECT_FALSE(AudioBitstreamParser::ParseAacProgramConfig(data.data(), data.size(), pce));
}

// ===========================================================================
// AAC SBR / PS：AudioSpecificConfig 的后置同步扩展
// ===========================================================================
//
// 背景：以前只从 AOT 是不是 5/29 推 SBR/PS。但 HE-AAC 在 MP4/MOV 里更常见的
// 写法是 core 仍写 LC(AOT=2)、末尾追加 syncExtensionType=0x2B7 + sbrPresentFlag，
// 再跟 0x548 + psPresentFlag（ISO/IEC 14496-3 §1.6.2.1）。
// 只认 AOT 会把这类文件全判成 LC：采样率报 core 的 24 kHz（实际输出 48 kHz）、
// 声道报 1（PS 上混成 2）。
//
// 全部字节由规范逐位拼出来（见下方各用例的位布局注释），不从实现取结果。
// 采样率索引表（ISO/IEC 14496-3 表 4.55）：6 = 24000，3 = 48000，4 = 44100。

// 位布局（共 49 bit，尾部补 7 个 0）：
//   AOT=2(5) sf=6(4) cc=1(4)                      -> core 24 kHz 单声道 LC
//   GASpecificConfig: frameLengthFlag=0 dependsOnCoreCoder=0 extensionFlag=0 (3)
//   syncExtensionType=0x2B7(11) extAOT=5(5) sbrPresentFlag=1(1)
//   extensionSamplingFrequencyIndex=3(4)          -> 输出 48 kHz
//   syncExtensionType=0x548(11) psPresentFlag=1(1)
TEST(AacSbrPsTest, SyncExtensionDetectsSbrAndPs) {
    const std::vector<uint8_t> asc_bytes = {0x13, 0x08, 0x56, 0xE5, 0x9D, 0x48, 0x80};
    videoeye::AacAudioSpecificConfig asc;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc_bytes.data(), asc_bytes.size(), asc));

    EXPECT_EQ(asc.audio_object_type, 2) << "core 仍是 LC，SBR 靠同步扩展表达";
    EXPECT_TRUE(asc.sbr);
    EXPECT_TRUE(asc.ps);
    EXPECT_EQ(asc.sbr_source, "sync_extension");
    EXPECT_TRUE(asc.sbr_from_sync_extension);
    EXPECT_TRUE(asc.ps_from_sync_extension);
    // core 24 kHz -> 输出 48 kHz。报 core 值是把"听到的"报错。
    EXPECT_EQ(asc.core_sample_rate, 24000);
    EXPECT_EQ(asc.sample_rate, 48000);
    // PS 把单声道上混成立体声：传输 1 声道，听到 2 声道
    EXPECT_EQ(asc.channels, 2);
}

// 反例：同样 AOT=2 但没有任何同步扩展 —— 不能凭空说"有 SBR"，
// 也不能说"确认没有 SBR"（implicit signaling 的可能性还在）。
TEST(AacSbrPsTest, NoSyncExtensionLeavesSbrUnknown) {
    const std::vector<uint8_t> asc_bytes = {0x13, 0x08};
    videoeye::AacAudioSpecificConfig asc;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc_bytes.data(), asc_bytes.size(), asc));

    EXPECT_FALSE(asc.sbr);
    EXPECT_FALSE(asc.ps);
    EXPECT_FALSE(asc.sbr_signaled) << "没有信令 = 不知道，不等于「确认没有」";
    EXPECT_TRUE(asc.sbr_source.empty());
    EXPECT_EQ(asc.sample_rate, 24000) << "没有 SBR 就用 core 采样率";
    EXPECT_EQ(asc.channels, 1);
}

// 反例：同步扩展里 sbrPresentFlag=0 —— 这是**明确结论**（该文件没有 SBR），
// 与"没读到扩展"（不知道）必须分开。sbr_signaled 就是区分这两者的开关。
TEST(AacSbrPsTest, ExplicitSbrAbsentIsADefiniteAnswer) {
    const std::vector<uint8_t> asc_bytes = {0x12, 0x10, 0x56, 0xE5, 0x00};
    videoeye::AacAudioSpecificConfig asc;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc_bytes.data(), asc_bytes.size(), asc));

    EXPECT_FALSE(asc.sbr);
    EXPECT_TRUE(asc.sbr_signaled) << "读到了扩展、且它说没有 SBR";
    EXPECT_EQ(asc.sbr_source, "sync_extension");
}

// 正例（对照）：AOT=5 显式 HE-AAC，不依赖同步扩展
TEST(AacSbrPsTest, AotFiveIsExplicitSbr) {
    const std::vector<uint8_t> asc_bytes = {0x2B, 0x09, 0x80};
    videoeye::AacAudioSpecificConfig asc;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc_bytes.data(), asc_bytes.size(), asc));

    EXPECT_EQ(asc.audio_object_type, 5);
    EXPECT_TRUE(asc.sbr);
    EXPECT_TRUE(asc.sbr_signaled);
    EXPECT_EQ(asc.sbr_source, "aot");
    EXPECT_EQ(asc.core_sample_rate, 24000);
    EXPECT_EQ(asc.sample_rate, 48000);
}

// 反例（走位正确性）：GASpecificConfig 里 dependsOnCoreCoder=1 时有 14 bit
// coreCoderDelay，不跳过就会把后面所有字段读错位 —— 通常会读不出 0x2B7，
// 或者把随机位当成 sbrPresentFlag=1。这是最容易写错、也最难从现象看出来的一处。
TEST(AacSbrPsTest, SkipsCoreCoderDelayBeforeSyncExtension) {
    const std::vector<uint8_t> asc_bytes = {0x13, 0x0A, 0x91, 0xA1, 0x5B, 0x96, 0x75, 0x22};
    videoeye::AacAudioSpecificConfig asc;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc_bytes.data(), asc_bytes.size(), asc));

    EXPECT_TRUE(asc.sbr) << "coreCoderDelay 没跳过的话这里读不到同步扩展";
    EXPECT_TRUE(asc.ps);
    EXPECT_EQ(asc.sample_rate, 48000);
}

// 反例：channelConfiguration=0 时 GASpecificConfig 里是一个完整的 PCE，
// 本实现不去解它（PCE 语法与 raw_data_block 版不同），因此必须**放弃**读同步扩展，
// 并留下说明 —— 不能猜位、更不能把后面的字节当成 0x2B7。
TEST(AacSbrPsTest, ChannelConfigZeroSkipsSyncExtensionWithNote) {
    const std::vector<uint8_t> asc_bytes = {0x13, 0x00, 0x00};
    videoeye::AacAudioSpecificConfig asc;
    ASSERT_TRUE(AudioBitstreamParser::ParseAacAudioSpecificConfig(asc_bytes.data(), asc_bytes.size(), asc));

    EXPECT_FALSE(asc.sbr);
    EXPECT_FALSE(asc.sbr_signaled);
    EXPECT_NE(asc.note.find("channelConfiguration=0"), std::string::npos)
        << "必须说明为什么没判 SBR，而不是静默返回「没有」";
}

} // namespace
