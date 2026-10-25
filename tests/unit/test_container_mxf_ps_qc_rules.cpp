// MXF / MPEG-PS 容器级 QC（container.mxf.* / container.ps.*）单元测试
//
// 三层分工：
//   * MxfScannerTest —— 造最小 MXF 字节流（KLV 键 + BER 长度），验证分区结构、
//     Primer / Index Table、片尾 RIP、KLV 截断与重同步计数。
//   * PsScannerTest —— 造最小 MPEG-PS 字节流（Pack Header + PSM + PES + End Code），
//     验证 End Code、DTS 单调性、时间戳缺失与 PSM 对账。
//     ⭐ 其中 BFramePtsReorderingIsNotBackwards 是最重要的一条：PES 按解码序排列，
//     有 B 帧时相邻 PTS 本来就回退一个 GOP，拿 PTS 判单调会把所有带 B 帧的素材误报。
//   * 规则层 —— 手工构造诊断，验证规则把事实翻译成报告（级别、文案、门控）。
//
// 样本不依赖任何真实媒体文件；扫描器测试把字节写进临时文件再扫。

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analysis/container/MpegPsStructureAnalyzer.h"
#include "core/analysis/container/MxfStructureAnalyzer.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/ContainerDiagnosticsInfo.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "core/qc/QcRuleDefaults.h"
#include "core/qc/QcRuleEngine.h"

namespace {

using namespace videoeye;
using videoeye::qc::QcRuleEngine;

// ---------------------------------------------------------------------------
// 通用
// ---------------------------------------------------------------------------

std::string WriteTemp(const std::string& name, const std::string& suffix, const std::string& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("videoeye_mxfps_" + name + suffix);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return {};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path.string();
}

std::string Be16(uint16_t v) {
    std::string out(2, '\0');
    out[0] = static_cast<char>((v >> 8) & 0xFF);
    out[1] = static_cast<char>(v & 0xFF);
    return out;
}

std::string Be32(uint32_t v) {
    std::string out(4, '\0');
    for (int i = 3; i >= 0; --i) {
        out[static_cast<size_t>(i)] = static_cast<char>(v & 0xFF);
        v >>= 8;
    }
    return out;
}

std::string Be64(uint64_t v) {
    std::string out(8, '\0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<size_t>(i)] = static_cast<char>(v & 0xFF);
        v >>= 8;
    }
    return out;
}

// ---------------------------------------------------------------------------
// MXF 字节构造（SMPTE 377M：16 字节 UL 键 + BER 长度 + 载荷）
// ---------------------------------------------------------------------------

const std::string kSmptePrefix("\x06\x0E\x2B\x34", 4);
// 类别字节 5 决定 KLV 类型（0x05 Primer / 0x53 Index Table），键 8..11 固定。
const std::string kClassTail("\x02\x05\x01\x01\x0D\x01\x02\x01\x01", 9);

std::string BerLength(size_t len) {
    if (len < 0x80)
        return std::string(1, static_cast<char>(len));
    size_t width = 0;
    for (size_t v = len; v != 0; v >>= 8)
        ++width;
    std::string out(1, static_cast<char>(0x80 | width));
    for (int i = static_cast<int>(width) - 1; i >= 0; --i)
        out.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    return out;
}

std::string Klv(const std::string& key, const std::string& value) {
    return key + BerLength(value.size()) + value;
}

// 声明长度与实交载荷不一致（造截断用）
std::string LyingKlv(const std::string& key, size_t declared, const std::string& actual) {
    return key + BerLength(declared) + actual;
}

std::string PartitionKey(uint8_t kind) {
    std::string key = kSmptePrefix + kClassTail;
    key.push_back(static_cast<char>(kind)); // key[13]：0x02 头 / 0x03 体 / 0x04 尾
    key += std::string("\x01\x00", 2);
    return key;
}

std::string PrimerKey() {
    std::string key = kSmptePrefix + kClassTail;
    key.push_back('\x05');
    key += std::string("\x01\x00", 2);
    return key;
}

std::string IndexKey() {
    std::string key = kSmptePrefix;
    key += std::string("\x02\x53\x01\x01\x0D\x01\x02\x01\x01", 9); // key[5] = 0x53
    key += std::string("\x05\x01\x00", 3);
    return key;
}

std::string EssenceKey(uint8_t track, uint8_t element_type) {
    std::string key = kSmptePrefix;
    key += std::string("\x01\x02\x01\x01\x0D\x01\x03\x01", 8); // key[4]=0x01，键 8..11 = 0D 01 03 01
    key.push_back(static_cast<char>(track));
    key.push_back(static_cast<char>(element_type));
    key += std::string("\x01\x00", 2);
    return key;
}

std::string RipKey() {
    std::string key = kSmptePrefix + kClassTail;
    key.push_back('\x11');
    key += std::string("\x01\x00", 2);
    return key;
}

// Partition Pack 载荷：major/minor/KAG/this/prev/footer/header_bytes/index_bytes/index_sid/body_offset/body_sid
// + OperationalPattern（偏移 64，16 字节）
std::string PartitionValue(uint8_t op_byte = 0x01) {
    std::string v;
    v += Be16(1);   // major
    v += Be16(2);   // minor
    v += Be32(512); // KAG
    v += Be64(0);   // this
    v += Be64(0);   // prev
    v += Be64(0);   // footer
    v += Be64(0);   // header byte count
    v += Be64(0);   // index byte count
    v += Be32(0);   // index SID
    v += Be64(0);   // body offset
    v += Be32(0);   // body SID
    while (v.size() < 64)
        v.push_back('\0');
    v += std::string(16, static_cast<char>(op_byte)); // OperationalPattern
    return v;
}

// RIP 载荷：分区对数量(4) + N*(偏移 8 + 长度 4) + CRC32(4)
std::string RipValue(uint32_t pairs) {
    std::string v = Be32(pairs);
    for (uint32_t i = 0; i < pairs; ++i) {
        v += Be64(i * 1024);
        v += Be32(1024);
    }
    v += Be32(0); // CRC
    return v;
}

std::string MxfHealthy() {
    std::string out;
    out += Klv(PartitionKey(0x02), PartitionValue());
    out += Klv(PrimerKey(), std::string(32, '\x01'));
    out += Klv(EssenceKey(1, 0x01), std::string(64, '\xAA')); // 视频
    out += Klv(EssenceKey(2, 0x02), std::string(32, '\xBB')); // 音频
    out += Klv(IndexKey(), std::string(48, '\x00'));
    out += Klv(PartitionKey(0x04), PartitionValue());
    out += Klv(RipKey(), RipValue(2));
    return out;
}

bool ScanMxf(const std::string& name, const std::string& bytes, model::MxfDiagnostics* out) {
    const std::string path = WriteTemp(name, ".mxf", bytes);
    if (path.empty())
        return false;
    model::ContainerStructureResult structure;
    MxfStructureOptions opt;
    opt.build_tree = false;
    opt.max_scan_bytes = 0; // 与 QC 一致：扫全文件
    MxfStructureAnalyzer analyzer;
    const bool ok = analyzer.Analyze(path, structure, nullptr, opt);
    if (out != nullptr)
        *out = structure.diagnostics.mxf;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return ok;
}

TEST(MxfScannerTest, HealthyFileFacts) {
    model::MxfDiagnostics diag;
    ASSERT_TRUE(ScanMxf("healthy", MxfHealthy(), &diag));
    EXPECT_TRUE(diag.analyzed);
    EXPECT_TRUE(diag.scan_complete);
    EXPECT_FALSE(diag.truncated);
    EXPECT_EQ(1u, diag.header_partitions);
    EXPECT_EQ(1u, diag.footer_partitions);
    EXPECT_EQ(0x02, diag.first_partition_kind);
    EXPECT_TRUE(diag.has_primer);
    EXPECT_EQ(1u, diag.index_segments);
    EXPECT_TRUE(diag.has_rip);
    EXPECT_FALSE(diag.rip_malformed);
    EXPECT_EQ(2u, diag.essence_objects);
    EXPECT_EQ(2u, diag.tracks);
    EXPECT_EQ(96u, diag.essence_bytes);
    EXPECT_EQ(0u, diag.resyncs);
    EXPECT_EQ(0u, diag.truncated_values);
}

TEST(MxfScannerTest, MissingFooterAndRipAreBothVisible) {
    std::string bytes;
    bytes += Klv(PartitionKey(0x02), PartitionValue());
    bytes += Klv(PrimerKey(), std::string(32, '\x01'));
    bytes += Klv(EssenceKey(1, 0x01), std::string(64, '\xAA'));
    bytes += Klv(IndexKey(), std::string(48, '\x00'));

    model::MxfDiagnostics diag;
    ASSERT_TRUE(ScanMxf("no_footer", bytes, &diag));
    EXPECT_EQ(0u, diag.footer_partitions);
    EXPECT_FALSE(diag.has_rip) << "RIP 靠反读文件尾得到，这里文件确实没有";
    EXPECT_TRUE(diag.scan_complete);
    EXPECT_FALSE(diag.truncated);
}

// RIP 声明的分区对数装不进它自己声明的长度 → 结构性坏包
TEST(MxfScannerTest, RipDeclaringMorePairsThanItsValueIsMalformed) {
    std::string bytes;
    bytes += Klv(PartitionKey(0x02), PartitionValue());
    bytes += Klv(PrimerKey(), std::string(32, '\x01'));
    bytes += Klv(EssenceKey(1, 0x01), std::string(64, '\xAA'));
    bytes += Klv(IndexKey(), std::string(48, '\x00'));
    bytes += Klv(PartitionKey(0x04), PartitionValue());
    bytes += Klv(RipKey(), Be32(100) + Be32(0)); // 声明 100 对，值只有 8 字节

    model::MxfDiagnostics diag;
    ASSERT_TRUE(ScanMxf("rip_bad", bytes, &diag));
    EXPECT_TRUE(diag.has_rip);
    EXPECT_TRUE(diag.rip_malformed);
}

TEST(MxfScannerTest, KlvOverrunningFileEndMarksTruncated) {
    std::string bytes;
    bytes += Klv(PartitionKey(0x02), PartitionValue());
    bytes += Klv(PrimerKey(), std::string(32, '\x01'));
    bytes += LyingKlv(EssenceKey(1, 0x01), 1u << 20, std::string(64, '\xAA')); // 声明 1 MB，实际 64 B

    model::MxfDiagnostics diag;
    ASSERT_TRUE(ScanMxf("truncated", bytes, &diag));
    EXPECT_TRUE(diag.truncated);
    EXPECT_EQ(1u, diag.truncated_values);
    EXPECT_EQ(0u, diag.footer_partitions);
}

TEST(MxfScannerTest, GarbageBetweenKlvCountsResync) {
    std::string bytes;
    bytes += Klv(PartitionKey(0x02), PartitionValue());
    bytes += Klv(PrimerKey(), std::string(32, '\x01'));
    bytes += std::string("\xDE\xAD\xBE\xEF\x00\x11", 6); // 键前缀失配
    bytes += Klv(EssenceKey(1, 0x01), std::string(64, '\xAA'));

    model::MxfDiagnostics diag;
    ASSERT_TRUE(ScanMxf("resync", bytes, &diag));
    EXPECT_EQ(1u, diag.resyncs);
    EXPECT_EQ(1u, diag.essence_objects);
}

TEST(MxfScannerTest, HeaderWithoutPrimerIsVisible) {
    std::string bytes;
    bytes += Klv(PartitionKey(0x02), PartitionValue());
    bytes += Klv(EssenceKey(1, 0x01), std::string(64, '\xAA'));

    model::MxfDiagnostics diag;
    ASSERT_TRUE(ScanMxf("no_primer", bytes, &diag));
    EXPECT_FALSE(diag.has_primer);
    EXPECT_EQ(0x02, diag.first_partition_kind);
}

TEST(MxfScannerTest, RejectsBytesWithoutPartition) {
    model::MxfDiagnostics diag;
    EXPECT_FALSE(ScanMxf("garbage", std::string(256, '\x7F'), &diag));
    EXPECT_FALSE(diag.analyzed);
}

// ---------------------------------------------------------------------------
// MPEG-PS 字节构造
// ---------------------------------------------------------------------------

std::string PsPackHeader() {
    std::string p("\x00\x00\x01\xBA", 4);
    // MPEG-2 Pack Header：'01' 标志 + SCR + program_mux_rate + 保留位，共 10 字节
    p += std::string("\x44\x00\x04\x00\x04\x01\x00\x00\x00\x00", 10);
    return p;
}

std::string PsSystemHeader() {
    std::string body(6, '\0');
    std::string out("\x00\x00\x01\xBB", 4);
    out += Be16(static_cast<uint16_t>(body.size()));
    out += body;
    return out;
}

struct PsMapEntry {
    uint8_t stream_type = 0;
    uint8_t stream_id = 0;
};

std::string PsPsm(const std::vector<PsMapEntry>& entries) {
    std::string map;
    for (const PsMapEntry& e : entries) {
        map.push_back(static_cast<char>(e.stream_type));
        map.push_back(static_cast<char>(e.stream_id));
        map += Be16(0);
    }
    std::string payload;
    payload += std::string("\xE0", 1);     // current_next + version
    payload += std::string("\xFF\xFF", 2); // reserved + marker
    payload += Be16(0);                    // program_stream_info_length
    payload += Be16(static_cast<uint16_t>(map.size()));
    payload += map;
    std::string out("\x00\x00\x01\xBC", 4);
    out += Be16(static_cast<uint16_t>(payload.size()));
    out += payload;
    return out;
}

// 33 位时间戳编码（MPEG-1 与 MPEG-2 的 5 字节格式相同，只有前缀 nibble 不同）
std::string EncodeTs(uint64_t value, bool paired) {
    std::string out(5, '\0');
    out[0] = static_cast<char>(0x20 | (paired ? 0x10 : 0x00) | (((value >> 30) & 0x07) << 1) | 0x01);
    out[1] = static_cast<char>((value >> 22) & 0xFF);
    out[2] = static_cast<char>((((value >> 15) & 0x7F) << 1) | 0x01);
    out[3] = static_cast<char>((value >> 7) & 0xFF);
    out[4] = static_cast<char>(((value & 0x7F) << 1) | 0x01);
    return out;
}

std::string PesPrefix(uint8_t stream_id, const std::string& payload) {
    std::string out("\x00\x00\x01", 3);
    out.push_back(static_cast<char>(stream_id));
    out += Be16(static_cast<uint16_t>(payload.size()));
    out += payload;
    return out;
}

// MPEG-2 PES：'10' 标志 + PTS_DTS_flags + header_data_length + PTS[+DTS]
std::string PsPesMpeg2(uint8_t stream_id, uint64_t pts, bool with_dts, uint64_t dts, size_t data_size = 16) {
    std::string hdr;
    hdr.push_back(static_cast<char>(0x80));
    hdr.push_back(static_cast<char>(with_dts ? 0xC0 : 0x80));
    hdr.push_back(static_cast<char>(with_dts ? 10 : 5));
    hdr += EncodeTs(pts, with_dts);
    if (with_dts)
        hdr += EncodeTs(dts, true);
    return PesPrefix(stream_id, hdr + std::string(data_size, '\xAA'));
}

// MPEG-1 PES：最多 16 字节 0xFF 填充 + '0010' PTS
std::string PsPesMpeg1(uint8_t stream_id, uint64_t pts, size_t data_size = 16) {
    std::string payload;
    payload += std::string("\xFF\xFF\xFF", 3);
    payload += EncodeTs(pts, false);
    payload += std::string(data_size, '\xAA');
    return PesPrefix(stream_id, payload);
}

// 没有时间戳的 PES：载荷必须是 0x00 —— 用 0xAA 会被 ParsePesStamps 当成
// '10' 开头的 MPEG-2 PES 头（0xAA & 0xC0 == 0x80），凭空"解析"出时间戳。
std::string PsPes(uint8_t stream_id, size_t data_size = 16) {
    return PesPrefix(stream_id, std::string(data_size, '\x00'));
}

const std::string kEndCode("\x00\x00\x01\xB9", 4);

bool ScanPs(const std::string& name, const std::string& bytes, model::MpegPsDiagnostics* out) {
    const std::string path = WriteTemp(name, ".mpg", bytes);
    if (path.empty())
        return false;
    model::ContainerStructureResult structure;
    MpegPsStructureOptions opt;
    opt.build_tree = false;
    opt.max_scan_bytes = 0;
    MpegPsStructureAnalyzer analyzer;
    const bool ok = analyzer.Analyze(path, structure, nullptr, opt);
    if (out != nullptr)
        *out = structure.diagnostics.mpeg_ps;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return ok;
}

std::vector<PsMapEntry> AvMap() {
    return {PsMapEntry{0x02, 0xE0}, PsMapEntry{0x04, 0xC0}};
}

TEST(PsScannerTest, HealthyFileFacts) {
    std::string bytes = PsPackHeader();
    bytes += PsSystemHeader();
    bytes += PsPsm(AvMap());
    bytes += PsPesMpeg2(0xE0, 1000, true, 900);
    bytes += PsPesMpeg2(0xE0, 4000, true, 3900);
    bytes += PsPesMpeg2(0xC0, 1100, false, 0);
    bytes += PsPesMpeg2(0xC0, 4100, false, 0);
    bytes += kEndCode;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("healthy", bytes, &diag));
    EXPECT_TRUE(diag.analyzed);
    EXPECT_TRUE(diag.scan_complete);
    EXPECT_EQ(1u, diag.pack_headers);
    EXPECT_EQ(1u, diag.mpeg2_pack_headers);
    EXPECT_EQ(0u, diag.mpeg1_pack_headers);
    EXPECT_EQ(1u, diag.system_headers);
    EXPECT_EQ(1u, diag.psm_count);
    EXPECT_EQ(4u, diag.pes_packets);
    EXPECT_TRUE(diag.has_end_code);
    EXPECT_EQ(0u, diag.truncated_start_codes);
    EXPECT_EQ(2u, diag.streams) << "padding（0xBE）不能算成一条数据流";
    EXPECT_EQ(1u, diag.video_streams);
    EXPECT_EQ(1u, diag.audio_streams);
    EXPECT_EQ(2u, diag.streams_with_timestamp);
    EXPECT_EQ(0u, diag.streams_without_timestamp);
    EXPECT_EQ(0u, diag.timestamp_backwards);
    EXPECT_EQ(0u, diag.unmapped_streams);
    EXPECT_EQ(0u, diag.declared_but_missing_streams);
}

// Program End Code 只作为事实记录：ffmpeg 的 mpeg/vob 复用器从不写它（实测 6.1 产出的
// .vob 结尾是 padding），DVD 的 VOB 按规范也不写。所以**不能**因此报缺陷，
// 切片中断交给 truncated_start_code 兜（切口必然落在某个元素内部）。
TEST(PsScannerTest, MissingEndCodeIsAFactNotADefect) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm({PsMapEntry{0x02, 0xE0}});
    bytes += PsPesMpeg2(0xE0, 1000, false, 0);

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("no_end", bytes, &diag));
    EXPECT_FALSE(diag.has_end_code);
    EXPECT_TRUE(diag.scan_complete);

    model::AnalysisResult result;
    result.file_path = "clip.mpg";
    result.file_extension = "mpg";
    result.container_format = "mpeg";
    result.duration_seconds = 60.0;
    result.container_diagnostics.mpeg_ps = diag;
    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    for (const auto& issue : report.issues) {
        EXPECT_NE(0u, issue.rule_id.rfind("container.ps.", 0)) << "不该命中 " << issue.rule_id;
    }
}

// padding（0xBE）必须走独立分支：0xBD..0xEF 的 PES 范围覆盖了 0xBE，
// 判断顺序写反会把每个 padding 都当成一条数据流。
TEST(PsScannerTest, PaddingPacketIsNotCountedAsAStream) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm({PsMapEntry{0x02, 0xE0}});
    bytes += PsPesMpeg2(0xE0, 1000, false, 0);
    std::string padding("\x00\x00\x01\xBE", 4);
    padding += Be16(8);
    padding += std::string(8, '\xFF');
    bytes += padding;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("padding_tail", bytes, &diag));
    EXPECT_EQ(1u, diag.padding_packets);
    EXPECT_EQ(1u, diag.streams) << "padding 不能同时被算成数据流";
}

// ⭐ 这条是本文件里最重要的一条：视频 PES 按解码序排列，有 B 帧时相邻 PTS
// 本来就回退一个 GOP（I 0 / P 3 / B 1 / B 2）。只有 DTS 才能用来判单调。
TEST(PsScannerTest, BFramePtsReorderingIsNotBackwards) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm({PsMapEntry{0x02, 0xE0}});
    // 解码序：I(PTS 0, DTS 0) → P(PTS 3, DTS 1) → B(PTS 1, DTS 2) → B(PTS 2, DTS 3)
    bytes += PsPesMpeg2(0xE0, 0, true, 0);
    bytes += PsPesMpeg2(0xE0, 3000, true, 1000);
    bytes += PsPesMpeg2(0xE0, 1000, true, 2000);
    bytes += PsPesMpeg2(0xE0, 2000, true, 3000);
    bytes += kEndCode;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("bframes", bytes, &diag));
    EXPECT_EQ(0u, diag.timestamp_backwards) << "PTS 回退是 B 帧的正常重排，不能算时间轴错乱";
}

TEST(PsScannerTest, DtsBackwardsIsCounted) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm({PsMapEntry{0x02, 0xE0}});
    bytes += PsPesMpeg2(0xE0, 9000, true, 9000);
    bytes += PsPesMpeg2(0xE0, 1000, true, 1000); // DTS 回退
    bytes += kEndCode;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("dts_back", bytes, &diag));
    EXPECT_EQ(1u, diag.timestamp_backwards);
}

// 纯音频流没有 DTS，PTS 就是解码序 —— 这类流的 PTS 回退要算数。
TEST(PsScannerTest, AudioPtsBackwardsIsCounted) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm({PsMapEntry{0x04, 0xC0}});
    bytes += PsPesMpeg2(0xC0, 5000, false, 0);
    bytes += PsPesMpeg2(0xC0, 2000, false, 0);
    bytes += kEndCode;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("audio_back", bytes, &diag));
    EXPECT_EQ(1u, diag.timestamp_backwards);
}

// MPEG-1 PES 头（0xFF 填充 + '0010' 前缀）认不出来时，所有 VCD 都会被误报成"流缺时间戳"。
TEST(PsScannerTest, Mpeg1PesTimestampsAreParsed) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm(AvMap());
    bytes += PsPesMpeg1(0xE0, 1000);
    bytes += PsPesMpeg1(0xE0, 2000);
    bytes += PsPesMpeg1(0xC0, 1500);
    bytes += kEndCode;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("mpeg1", bytes, &diag));
    EXPECT_EQ(2u, diag.streams_with_timestamp);
    EXPECT_EQ(0u, diag.streams_without_timestamp);
    EXPECT_EQ(0u, diag.timestamp_backwards);
}

TEST(PsScannerTest, StreamWithoutTimestampIsCounted) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm(AvMap());
    bytes += PsPesMpeg2(0xE0, 1000, false, 0); // 视频有时间戳
    bytes += PsPes(0xC0);                      // 音频两个包都没有
    bytes += PsPes(0xC0);
    bytes += kEndCode;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("no_ts", bytes, &diag));
    EXPECT_EQ(1u, diag.streams_with_timestamp);
    EXPECT_EQ(1u, diag.streams_without_timestamp);
}

TEST(PsScannerTest, PsmMismatchIsCountedBothWays) {
    // PSM 只声明 0xE0，实际还有 0xC0 → 未声明 1 条
    std::string undeclared = PsPackHeader();
    undeclared += PsPsm({PsMapEntry{0x02, 0xE0}});
    undeclared += PsPesMpeg2(0xE0, 1000, false, 0);
    undeclared += PsPesMpeg2(0xC0, 1100, false, 0);
    undeclared += kEndCode;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("unmapped", undeclared, &diag));
    EXPECT_EQ(1u, diag.unmapped_streams);
    EXPECT_EQ(0u, diag.declared_but_missing_streams);

    // PSM 声明 0xE0 + 0xC0，实际只有 0xE0 → 声明了却没数据 1 条
    std::string missing = PsPackHeader();
    missing += PsPsm(AvMap());
    missing += PsPesMpeg2(0xE0, 1000, false, 0);
    missing += kEndCode;

    ASSERT_TRUE(ScanPs("declared_missing", missing, &diag));
    EXPECT_EQ(0u, diag.unmapped_streams);
    EXPECT_EQ(1u, diag.declared_but_missing_streams);
}

TEST(PsScannerTest, TruncatedStartCodeIsCounted) {
    std::string bytes = PsPackHeader();
    bytes += PsPsm(AvMap());
    // 声明 400 字节载荷，实际只给 8 字节
    std::string payload = std::string(8, '\xAA');
    bytes += std::string("\x00\x00\x01\xE0", 4);
    bytes += Be16(400);
    bytes += payload;

    model::MpegPsDiagnostics diag;
    ASSERT_TRUE(ScanPs("truncated", bytes, &diag));
    EXPECT_EQ(1u, diag.truncated_start_codes);
    EXPECT_FALSE(diag.has_end_code);
}

TEST(PsScannerTest, RejectsBytesWithoutPackHeader) {
    model::MpegPsDiagnostics diag;
    EXPECT_FALSE(ScanPs("raw_es", std::string(512, '\x11'), &diag));
    EXPECT_FALSE(diag.analyzed);
}

// ---------------------------------------------------------------------------
// 规则层
// ---------------------------------------------------------------------------

bool HasIssue(const model::QcReport& report, const std::string& rule_id, model::DiagnosticIssue* out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id)
            continue;
        if (out != nullptr)
            *out = issue;
        return true;
    }
    return false;
}

model::AnalysisResult MakeMxfResult(const model::MxfDiagnostics& mxf) {
    model::AnalysisResult result;
    result.file_path = "clip.mxf";
    result.file_extension = "mxf";
    result.container_format = "mxf";
    result.duration_seconds = 60.0;
    result.container_diagnostics.mxf = mxf;
    return result;
}

model::AnalysisResult MakePsResult(const model::MpegPsDiagnostics& ps) {
    model::AnalysisResult result;
    result.file_path = "clip.mpg";
    result.file_extension = "mpg";
    result.container_format = "mpeg";
    result.duration_seconds = 60.0;
    result.container_diagnostics.mpeg_ps = ps;
    return result;
}

model::MxfDiagnostics HealthyMxf() {
    model::MxfDiagnostics mxf;
    mxf.analyzed = true;
    mxf.scan_complete = true;
    mxf.header_partitions = 1;
    mxf.footer_partitions = 1;
    mxf.first_partition_kind = 0x02;
    mxf.has_primer = true;
    mxf.primer_packs = 1;
    mxf.index_segments = 3;
    mxf.has_rip = true;
    mxf.essence_objects = 500;
    mxf.tracks = 2;
    return mxf;
}

model::MpegPsDiagnostics HealthyPs() {
    model::MpegPsDiagnostics ps;
    ps.analyzed = true;
    ps.scan_complete = true;
    ps.pack_headers = 200;
    ps.mpeg2_pack_headers = 200;
    ps.psm_count = 1;
    ps.pes_packets = 400;
    ps.has_end_code = true;
    ps.streams = 2;
    ps.video_streams = 1;
    ps.audio_streams = 1;
    ps.streams_with_timestamp = 2;
    return ps;
}

TEST(MxfQcRulesTest, HealthyFileProducesNoMxfIssues) {
    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeMxfResult(HealthyMxf()));
    for (const auto& issue : report.issues) {
        EXPECT_NE(0u, issue.rule_id.rfind("container.mxf.", 0)) << "健康 MXF 不应命中 " << issue.rule_id;
    }
}

TEST(MxfQcRulesTest, MissingFooterPartitionIsWarning) {
    model::MxfDiagnostics mxf = HealthyMxf();
    mxf.footer_partitions = 0;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeMxfResult(mxf)), "container.mxf.missing_footer_partition", &issue));
    EXPECT_EQ(model::IssueSeverity::Warning, issue.severity);
    EXPECT_NE(std::string::npos, issue.detail.find("Footer")) << issue.detail;
}

TEST(MxfQcRulesTest, MissingFooterIsSuppressedWhenNotConclusive) {
    model::MxfDiagnostics partial = HealthyMxf();
    partial.footer_partitions = 0;
    partial.scan_complete = false;

    // 截断文件：缺 Footer 是截断的账，由 truncated_klv 报，不重复扣分
    model::MxfDiagnostics truncated = HealthyMxf();
    truncated.footer_partitions = 0;
    truncated.truncated = true;

    QcRuleEngine engine;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeMxfResult(partial)), "container.mxf.missing_footer_partition"));
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeMxfResult(truncated)), "container.mxf.missing_footer_partition"));
    // 截断把 RIP 一并截掉了，同样不该再报一条
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeMxfResult(truncated)), "container.mxf.missing_rip"));
}

TEST(MxfQcRulesTest, MissingRipIsInfoAndIgnoresScanWindow) {
    model::MxfDiagnostics mxf = HealthyMxf();
    mxf.has_rip = false;
    mxf.scan_complete = false; // RIP 靠反读文件尾，与扫描窗口无关

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeMxfResult(mxf)), "container.mxf.missing_rip", &issue));
    EXPECT_EQ(model::IssueSeverity::Info, issue.severity);
    EXPECT_NE(std::string::npos, issue.detail.find("RIP")) << issue.detail;
}

TEST(MxfQcRulesTest, MissingIndexNeedsCompleteScanAndEssence) {
    model::MxfDiagnostics mxf = HealthyMxf();
    mxf.index_segments = 0;

    QcRuleEngine engine;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeMxfResult(mxf)), "container.mxf.missing_index"));

    model::MxfDiagnostics partial = mxf;
    partial.scan_complete = false;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeMxfResult(partial)), "container.mxf.missing_index"));

    model::MxfDiagnostics no_essence = mxf;
    no_essence.essence_objects = 0;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeMxfResult(no_essence)), "container.mxf.missing_index"));
}

TEST(MxfQcRulesTest, MissingPrimerOnlyWhenFileStartsAtHeader) {
    model::MxfDiagnostics mxf = HealthyMxf();
    mxf.has_primer = false;
    mxf.primer_packs = 0;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeMxfResult(mxf)), "container.mxf.missing_primer", &issue));
    EXPECT_EQ(model::IssueSeverity::Error, issue.severity);

    // 从 Body 分区截出来的一段素材本来就没有头，不该报
    model::MxfDiagnostics sliced = mxf;
    sliced.first_partition_kind = 0x03;
    sliced.header_partitions = 0;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeMxfResult(sliced)), "container.mxf.missing_primer"));
}

TEST(MxfQcRulesTest, TruncationResyncAndIncompleteScan) {
    QcRuleEngine engine;

    model::MxfDiagnostics mxf = HealthyMxf();
    mxf.truncated = true;
    mxf.truncated_values = 2;
    mxf.resyncs = 5;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeMxfResult(mxf)), "container.mxf.truncated_klv", &issue));
    EXPECT_EQ(model::IssueSeverity::Warning, issue.severity);
    EXPECT_EQ(2.0, issue.metric_value);
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeMxfResult(mxf)), "container.mxf.resync", &issue));
    EXPECT_EQ(5.0, issue.metric_value);

    // 只是越过扫描窗口（不是文件尾）不算截断
    model::MxfDiagnostics window_only = HealthyMxf();
    window_only.truncated_values = 1;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeMxfResult(window_only)), "container.mxf.truncated_klv"));

    model::MxfDiagnostics partial = HealthyMxf();
    partial.scan_complete = false;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeMxfResult(partial)), "container.mxf.scan_incomplete"));
}

TEST(PsQcRulesTest, HealthyFileProducesNoPsIssues) {
    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakePsResult(HealthyPs()));
    for (const auto& issue : report.issues) {
        EXPECT_NE(0u, issue.rule_id.rfind("container.ps.", 0)) << "健康 PS 不应命中 " << issue.rule_id;
    }
}

TEST(PsQcRulesTest, TimestampBackwardsIsError) {
    model::MpegPsDiagnostics ps = HealthyPs();
    ps.timestamp_backwards = 3;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakePsResult(ps)), "container.ps.timestamp_backwards", &issue));
    EXPECT_EQ(model::IssueSeverity::Error, issue.severity);
    EXPECT_EQ(3.0, issue.metric_value);
}

// 一条时间戳都解析不出来的文件，说明不了"流缺时间戳"还是"解析失败"，宁可不报。
TEST(PsQcRulesTest, StreamWithoutTimestampRequiresAnotherWorkingStream) {
    model::MpegPsDiagnostics ps = HealthyPs();
    ps.streams_with_timestamp = 1;
    ps.streams_without_timestamp = 1;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakePsResult(ps)), "container.ps.stream_without_timestamp", &issue));
    EXPECT_EQ(model::IssueSeverity::Warning, issue.severity);

    model::MpegPsDiagnostics all_missing = HealthyPs();
    all_missing.streams_with_timestamp = 0;
    all_missing.streams_without_timestamp = 2;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakePsResult(all_missing)), "container.ps.stream_without_timestamp"));
}

TEST(PsQcRulesTest, StreamMapMismatchIsInfoAndNeedsPsm) {
    model::MpegPsDiagnostics ps = HealthyPs();
    ps.unmapped_streams = 1;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakePsResult(ps)), "container.ps.stream_map_mismatch", &issue));
    EXPECT_EQ(model::IssueSeverity::Info, issue.severity);
    EXPECT_NE(std::string::npos, issue.detail.find("PSM")) << issue.detail;

    model::MpegPsDiagnostics no_psm = ps;
    no_psm.psm_count = 0;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakePsResult(no_psm)), "container.ps.stream_map_mismatch"));
}

TEST(PsQcRulesTest, TruncationAndIncompleteScan) {
    QcRuleEngine engine;

    model::MpegPsDiagnostics ps = HealthyPs();
    ps.truncated_start_codes = 2;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakePsResult(ps)), "container.ps.truncated_start_code", &issue));
    EXPECT_EQ(2.0, issue.metric_value);

    model::MpegPsDiagnostics partial = HealthyPs();
    partial.scan_complete = false;
    EXPECT_TRUE(HasIssue(engine.Evaluate(MakePsResult(partial)), "container.ps.scan_incomplete"));
}

// 事实来自分析器而不是手工构造：一条端到端的不变量检查，
// 防止"规则读的字段名"与"分析器写的字段名"悄悄漂移。
TEST(MxfPsQcRulesTest, AnalyzerFactsDriveTheRulesEndToEnd) {
    model::MxfDiagnostics mxf;
    ASSERT_TRUE(ScanMxf("e2e", MxfHealthy(), &mxf));
    EXPECT_TRUE(mxf.analyzed);
    EXPECT_TRUE(mxf.has_rip);
    EXPECT_EQ(1u, mxf.footer_partitions);

    QcRuleEngine engine;
    const model::QcReport mxf_report = engine.Evaluate(MakeMxfResult(mxf));
    EXPECT_FALSE(HasIssue(mxf_report, "container.mxf.missing_footer_partition"));
    EXPECT_FALSE(HasIssue(mxf_report, "container.mxf.missing_rip"));
    EXPECT_FALSE(HasIssue(mxf_report, "container.mxf.missing_primer"));

    std::string bytes = PsPackHeader();
    bytes += PsPsm(AvMap());
    bytes += PsPesMpeg2(0xE0, 1000, true, 900);
    bytes += kEndCode;
    model::MpegPsDiagnostics ps;
    ASSERT_TRUE(ScanPs("e2e", bytes, &ps));

    const model::QcReport ps_report = engine.Evaluate(MakePsResult(ps));
    EXPECT_FALSE(HasIssue(ps_report, "container.ps.missing_end_code"));
    EXPECT_FALSE(HasIssue(ps_report, "container.ps.timestamp_backwards"));
}

// 规则表里必须真的注册了这两族 —— 只加 Check*Rules 分支不加 default 规则，
// 规则永远不会被 Evaluate 调起（症状是"规则全绿但从没跑过"）。
TEST(MxfPsQcRulesTest, DefaultRulesRegisterBothFamilies) {
    std::vector<model::QcRule> rules = qc::DefaultQcRules();
    for (const char* id : {"container.mxf.missing_footer_partition", "container.mxf.missing_rip",
                           "container.mxf.missing_index", "container.mxf.missing_primer", "container.mxf.truncated_klv",
                           "container.mxf.resync", "container.mxf.scan_incomplete", "container.ps.truncated_start_code",
                           "container.ps.timestamp_backwards", "container.ps.stream_without_timestamp",
                           "container.ps.stream_map_mismatch", "container.ps.scan_incomplete"}) {
        const model::QcRule* rule = model::FindQcRule(rules, id);
        ASSERT_TRUE(rule != nullptr) << "规则表里漏了 " << id;
        EXPECT_EQ(model::IssueCategory::Container, rule->category);
        EXPECT_TRUE(rule->enabled);
        EXPECT_FALSE(rule->suggestion.empty()) << id << " 缺少可执行的修复建议";
    }
}

// 真实语料（ffmpeg 生成，随仓库走）：合成字节的构造方式与解析实现同源，
// 光靠它们测不出"两边一起错"。这里在真实 muxer 产物上断言正面结论，
// 顺带把"ffmpeg 不写 Program End Code"这类事实钉住（改了也不会误报成缺陷）。
TEST(MxfPsQcRulesTest, RealMuxerOutputIsHealthy) {
#ifdef VIDEOEYE_CONTAINER_CORPUS_DIR
    const std::filesystem::path dir(VIDEOEYE_CONTAINER_CORPUS_DIR);

    const std::filesystem::path mxf_path = dir / "real_mpeg2_pcm.mxf";
    if (std::filesystem::exists(mxf_path)) {
        model::ContainerStructureResult structure;
        MxfStructureOptions opt;
        opt.build_tree = false;
        opt.max_scan_bytes = 0;
        MxfStructureAnalyzer analyzer;
        ASSERT_TRUE(analyzer.Analyze(mxf_path.string(), structure, nullptr, opt)) << mxf_path.string();
        const model::MxfDiagnostics& mxf = structure.diagnostics.mxf;
        EXPECT_TRUE(mxf.analyzed);
        EXPECT_TRUE(mxf.scan_complete);
        EXPECT_FALSE(mxf.truncated);
        EXPECT_GE(mxf.header_partitions, 1u);
        EXPECT_GE(mxf.footer_partitions, 1u) << "正常收尾必须有 Footer 分区";
        EXPECT_TRUE(mxf.has_primer);
        EXPECT_GE(mxf.index_segments, 1u);
        EXPECT_TRUE(mxf.has_rip) << "ffmpeg 会写片尾 RIP；漏了说明反读文件尾没走到";
        EXPECT_FALSE(mxf.rip_malformed);
        EXPECT_EQ(0u, mxf.truncated_values);
        EXPECT_EQ(0u, mxf.resyncs);
        EXPECT_EQ(2u, mxf.tracks);
    }

    const std::filesystem::path ps_path = dir / "real_mpeg2_mp2.mpg";
    if (std::filesystem::exists(ps_path)) {
        model::ContainerStructureResult structure;
        MpegPsStructureOptions opt;
        opt.build_tree = false;
        opt.max_scan_bytes = 0;
        MpegPsStructureAnalyzer analyzer;
        ASSERT_TRUE(analyzer.Analyze(ps_path.string(), structure, nullptr, opt)) << ps_path.string();
        const model::MpegPsDiagnostics& ps = structure.diagnostics.mpeg_ps;
        EXPECT_TRUE(ps.analyzed);
        EXPECT_TRUE(ps.scan_complete);
        EXPECT_GE(ps.pack_headers, 1u);
        EXPECT_EQ(0u, ps.mpeg1_pack_headers);
        EXPECT_EQ(0u, ps.truncated_start_codes);
        EXPECT_EQ(0u, ps.timestamp_backwards);
        EXPECT_EQ(0u, ps.streams_without_timestamp);
        EXPECT_EQ(1u, ps.video_streams);
        EXPECT_EQ(1u, ps.audio_streams);
        // vob 复用器不写 Program End Code，这不是缺陷 —— 只是事实
        EXPECT_FALSE(ps.has_end_code);
    }
#endif
}

} // namespace
