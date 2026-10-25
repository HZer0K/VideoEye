// MKV / WebM 容器级 QC（container.mkv.*）单元测试
//
// 两层分工：
//   * MkvContainerScannerTest —— 造一份最小 MKV 字节流，验证元素级顺序扫描
//     真的把 Cues / Cluster 时间轴 / CodecPrivate / unknown-size 读对了。
//     （unknown-size Cluster 的边界是这一层最容易写错的地方，必须钉住。）
//   * MkvQcRulesTest —— 手工构造 MkvDiagnostics，验证规则把事实翻译成
//     报告（级别、文案、门控）。
//
// 样本不依赖任何真实媒体文件；扫描器测试把字节写进临时文件再扫。

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analysis/container/MkvContainerScanner.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/ContainerDiagnosticsInfo.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "core/qc/QcRuleDefaults.h"
#include "core/qc/QcRuleEngine.h"

namespace {

using namespace videoeye;
using videoeye::qc::QcRuleEngine;

// ---------------------------------------------------------------------------
// EBML 字节构造
// ---------------------------------------------------------------------------

// 元素 ID 按自然宽度写（ID 自带标记位，不能当 VINT 值编码）
void AppendId(std::string& out, uint64_t id) {
    int width = 1;
    if (id > 0xFFFFFFULL)
        width = 4;
    else if (id > 0xFFFFULL)
        width = 3;
    else if (id > 0xFFULL)
        width = 2;
    for (int i = width - 1; i >= 0; --i) {
        out.push_back(static_cast<char>((id >> (8 * i)) & 0xFF));
    }
}

// 长度 VINT：最小宽度，且避开"全 1"（那是未知长度的标记）
std::string SizeVint(uint64_t size) {
    for (int width = 1; width <= 8; ++width) {
        const uint64_t max_value = (1ULL << (7 * width)) - 2;
        if (size <= max_value) {
            uint64_t value = size | (1ULL << (7 * width));
            std::string out(static_cast<size_t>(width), '\0');
            for (int i = width - 1; i >= 0; --i) {
                out[static_cast<size_t>(i)] = static_cast<char>(value & 0xFF);
                value >>= 8;
            }
            return out;
        }
    }
    return {};
}

// 8 字节全 1 的"未知长度"标记
std::string UnknownSizeVint() {
    return std::string("\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8);
}

std::string Element(uint64_t id, const std::string& payload) {
    std::string out;
    AppendId(out, id);
    out += SizeVint(payload.size());
    out += payload;
    return out;
}

std::string UnknownSizeElement(uint64_t id, const std::string& payload) {
    std::string out;
    AppendId(out, id);
    out += UnknownSizeVint();
    out += payload;
    return out;
}

std::string BeUInt(uint64_t value) {
    std::string out;
    do {
        out.insert(out.begin(), static_cast<char>(value & 0xFF));
        value >>= 8;
    } while (value != 0);
    return out;
}

constexpr uint64_t kIdEbmlHeader = 0x1A45DFA3;
constexpr uint64_t kIdDocType = 0x4282;
constexpr uint64_t kIdSegment = 0x18538067;
constexpr uint64_t kIdSeekHead = 0x114D9B74;
constexpr uint64_t kIdInfo = 0x1549A966;
constexpr uint64_t kIdTimestampScale = 0x2AD7B1;
constexpr uint64_t kIdTracks = 0x1654AE6B;
constexpr uint64_t kIdTrackEntry = 0xAE;
constexpr uint64_t kIdTrackType = 0x83;
constexpr uint64_t kIdCodecId = 0x86;
constexpr uint64_t kIdCodecPrivate = 0x63A2;
constexpr uint64_t kIdCluster = 0x1F43B675;
constexpr uint64_t kIdClusterTimecode = 0xE7;
constexpr uint64_t kIdSimpleBlock = 0xA3;
constexpr uint64_t kIdCues = 0x1C53BB6B;

std::string EbmlHeader(const std::string& doc_type) {
    return Element(kIdEbmlHeader, Element(kIdDocType, doc_type));
}

std::string InfoWithScale(uint64_t scale) {
    return Element(kIdInfo, Element(kIdTimestampScale, BeUInt(scale)));
}

// 一条视频轨；has_codec_private 决定是否带 0x63A2
std::string TrackEntry(uint64_t type, const std::string& codec_id, bool has_codec_private) {
    std::string entry = Element(kIdTrackType, BeUInt(type));
    entry += Element(kIdCodecId, codec_id);
    if (has_codec_private) {
        entry += Element(kIdCodecPrivate, std::string("\x01\x64\x00\x1F", 4)); // 假 avcC
    }
    return Element(kIdTrackEntry, entry);
}

std::string Cluster(uint64_t ticks) {
    return Element(kIdCluster, Element(kIdClusterTimecode, BeUInt(ticks)));
}

// 把字节写进临时文件；返回路径（失败返回空串）
std::string WriteTemp(const std::string& name, const std::string& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("videoeye_mkv_" + name + ".mkv");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return {};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path.string();
}

// 扫描一份字节流，返回诊断
bool ScanBytes(const std::string& name, const std::string& bytes, model::MkvDiagnostics* out) {
    const std::string path = WriteTemp(name, bytes);
    if (path.empty())
        return false;
    MkvContainerScanner scanner;
    const bool ok = scanner.Analyze(path, *out);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return ok;
}

// ---------------------------------------------------------------------------
// 扫描器
// ---------------------------------------------------------------------------

TEST(MkvContainerScannerTest, ReadsCuesClustersAndCodecPrivate) {
    std::string segment;
    segment += InfoWithScale(1000000); // 1 ms 刻度
    segment += Element(kIdTracks, TrackEntry(1, "V_MPEG4/ISO/AVC", /*has_codec_private=*/false));
    segment += Cluster(0);
    segment += Cluster(5000);
    segment += Cluster(2000); // 回退一次
    segment += Element(kIdCues, "");

    const std::string bytes = EbmlHeader("matroska") + Element(kIdSegment, segment);

    model::MkvDiagnostics diag;
    ASSERT_TRUE(ScanBytes("basic", bytes, &diag));
    EXPECT_TRUE(diag.analyzed);
    EXPECT_TRUE(diag.scan_complete);
    EXPECT_FALSE(diag.truncated);
    EXPECT_EQ("matroska", diag.doc_type);
    EXPECT_EQ(3u, diag.clusters);
    EXPECT_EQ(3u, diag.clusters_with_timecode);
    EXPECT_EQ(1u, diag.cluster_timestamp_backwards);
    EXPECT_TRUE(diag.has_cues);
    EXPECT_FALSE(diag.has_seek_head);
    EXPECT_EQ(1u, diag.video_tracks);
    EXPECT_EQ(0u, diag.audio_tracks);
    EXPECT_EQ(1u, diag.tracks_missing_codec_private);
    EXPECT_EQ("V_MPEG4/ISO/AVC", diag.missing_codec_private_codecs);
    EXPECT_EQ(0u, diag.unknown_size_elements);
}

TEST(MkvContainerScannerTest, CodecPrivatePresentIsNotReported) {
    std::string segment = InfoWithScale(1000000);
    segment += Element(kIdTracks, TrackEntry(1, "V_MPEGH/ISO/HEVC", /*has_codec_private=*/true) +
                                      TrackEntry(2, "A_AAC", /*has_codec_private=*/true));
    const std::string bytes = EbmlHeader("webm") + Element(kIdSegment, segment);

    model::MkvDiagnostics diag;
    ASSERT_TRUE(ScanBytes("codec_private_ok", bytes, &diag));
    EXPECT_EQ("webm", diag.doc_type);
    EXPECT_EQ(1u, diag.video_tracks);
    EXPECT_EQ(1u, diag.audio_tracks);
    EXPECT_EQ(0u, diag.tracks_missing_codec_private);
    EXPECT_TRUE(diag.missing_codec_private_codecs.empty());
}

// Unknown-size Segment + Unknown-size Cluster：Cluster 的结束位置只能靠
// "下一个一级元素"推断。这条错了会让后面的 Cues 完全读不到（甚至死循环）。
TEST(MkvContainerScannerTest, UnknownSizeClusterEndsAtNextLevel1Element) {
    std::string cluster_body = Element(kIdClusterTimecode, BeUInt(1500));
    cluster_body += Element(kIdSimpleBlock, std::string(32, '\xAB')); // 载荷要整块跳过
    std::string segment = InfoWithScale(1000000);
    segment += UnknownSizeElement(kIdCluster, cluster_body);
    segment += Element(kIdCues, "");

    const std::string bytes = EbmlHeader("matroska") + UnknownSizeElement(kIdSegment, segment);

    model::MkvDiagnostics diag;
    ASSERT_TRUE(ScanBytes("unknown_size", bytes, &diag));
    EXPECT_EQ(2u, diag.unknown_size_elements); // Segment + Cluster
    EXPECT_EQ(1u, diag.clusters);
    EXPECT_EQ(1u, diag.clusters_with_timecode);
    EXPECT_TRUE(diag.has_cues) << "unknown-size Cluster 的边界算错时，后面的 Cues 会被吞掉";
    EXPECT_FALSE(diag.truncated);
}

TEST(MkvContainerScannerTest, DetectsSeekHeadAndTruncation) {
    std::string segment = Element(kIdSeekHead, "");
    segment += InfoWithScale(1000000);
    segment += Cluster(0);
    // 末尾留两个字节：一个 4 字节 ID 的长度不够，元素头解析失败 → 截断
    segment += std::string("\x1F\x43", 2);

    const std::string bytes = EbmlHeader("matroska") + Element(kIdSegment, segment);

    model::MkvDiagnostics diag;
    ASSERT_TRUE(ScanBytes("truncated", bytes, &diag));
    EXPECT_TRUE(diag.has_seek_head);
    EXPECT_TRUE(diag.truncated);
    EXPECT_EQ(1u, diag.clusters);
}

TEST(MkvContainerScannerTest, RejectsNonEbmlBytes) {
    model::MkvDiagnostics diag;
    EXPECT_FALSE(ScanBytes("garbage", std::string("not an ebml file at all"), &diag));
    EXPECT_FALSE(diag.analyzed);

    // 有 EBML 魔数但没有 Segment
    EXPECT_FALSE(ScanBytes("no_segment", EbmlHeader("matroska"), &diag));
    EXPECT_FALSE(diag.analyzed);
}

TEST(MkvContainerScannerTest, TimecodeScaleDrivesSeconds) {
    // TimestampScale = 100000（0.1 ms 刻度）：5000 ticks 只有 0.5 秒，不是 5 秒
    std::string segment = InfoWithScale(100000);
    segment += Cluster(0);
    segment += Cluster(5000);
    const std::string bytes = EbmlHeader("matroska") + Element(kIdSegment, segment);

    model::MkvDiagnostics diag;
    ASSERT_TRUE(ScanBytes("scale", bytes, &diag));
    EXPECT_DOUBLE_EQ(0.5, diag.last_cluster_seconds);
}

// 真实语料（ffmpeg 生成，随仓库走）：合成样本的构造方式与解析实现同源，
// 光靠它们测不出"两边一起错"。这两条是语料目录里"专门为 unknown-size / 截断
// 造的"样本，断言取弱不变量（元素个数 / 截断标志），不绑具体 Cluster 数。
TEST(MkvContainerScannerTest, CorpusSamplesCarryTheirIntendedDefect) {
#ifdef VIDEOEYE_CONTAINER_CORPUS_DIR
    const std::filesystem::path dir(VIDEOEYE_CONTAINER_CORPUS_DIR);
    MkvContainerScanner scanner;

    const std::filesystem::path unknown_size = dir / "ebml_unknown_size.mkv";
    if (std::filesystem::exists(unknown_size)) {
        model::MkvDiagnostics diag;
        ASSERT_TRUE(scanner.Analyze(unknown_size.string(), diag)) << unknown_size.string();
        EXPECT_EQ("matroska", diag.doc_type);
        EXPECT_GE(diag.unknown_size_elements, 2u) << "Segment 与 Cluster 都应是未知长度写法";
        EXPECT_GT(diag.clusters_with_timecode, 0u) << "unknown-size Cluster 里也要读得到 Timecode";
        EXPECT_FALSE(diag.truncated);
    }

    const std::filesystem::path truncated = dir / "truncated.mkv";
    if (std::filesystem::exists(truncated)) {
        model::MkvDiagnostics diag;
        ASSERT_TRUE(scanner.Analyze(truncated.string(), diag)) << truncated.string();
        EXPECT_TRUE(diag.truncated);
    }
#endif
}

// ---------------------------------------------------------------------------
// 规则层
// ---------------------------------------------------------------------------

model::AnalysisResult MakeResult(const model::MkvDiagnostics& mkv, double duration = 60.0) {
    model::AnalysisResult result;
    result.file_path = "clip.mkv";
    result.file_extension = "mkv";
    result.container_format = "matroska,webm";
    result.duration_seconds = duration;
    result.container_diagnostics.mkv = mkv;
    return result;
}

model::MkvDiagnostics HealthyMkv() {
    model::MkvDiagnostics mkv;
    mkv.analyzed = true;
    mkv.scan_complete = true;
    mkv.doc_type = "matroska";
    mkv.has_seek_head = true;
    mkv.has_cues = true;
    mkv.clusters = 100;
    mkv.clusters_with_timecode = 100;
    mkv.video_tracks = 1;
    mkv.audio_tracks = 1;
    return mkv;
}

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

TEST(MkvQcRulesTest, HealthyFileProducesNoMkvIssues) {
    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(HealthyMkv()));
    for (const auto& issue : report.issues) {
        EXPECT_NE(0u, issue.rule_id.rfind("container.mkv.", 0)) << "健康 MKV 不应命中 " << issue.rule_id;
    }
}

TEST(MkvQcRulesTest, MissingCuesIsReportedOnCompleteScan) {
    model::MkvDiagnostics mkv = HealthyMkv();
    mkv.has_cues = false;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeResult(mkv)), "container.mkv.missing_cues", &issue));
    EXPECT_EQ(model::IssueSeverity::Warning, issue.severity);
    EXPECT_NE(std::string::npos, issue.detail.find("Cues")) << issue.detail;
}

TEST(MkvQcRulesTest, MissingCuesIsSuppressedWhenNotConclusive) {
    // 1) 只扫了窗口 → 不能断言"没有索引"
    model::MkvDiagnostics partial = HealthyMkv();
    partial.has_cues = false;
    partial.scan_complete = false;
    // 2) 截断文件：没有索引是截断的账（由 truncated 规则报），这里不重复扣分
    model::MkvDiagnostics truncated = HealthyMkv();
    truncated.has_cues = false;
    truncated.truncated = true;

    QcRuleEngine engine;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(partial)), "container.mkv.missing_cues"));
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(truncated)), "container.mkv.missing_cues"));

    // 3) 直播录制（时长未知）本来就不写 Cues
    model::MkvDiagnostics live = HealthyMkv();
    live.has_cues = false;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(live, /*duration=*/0.0)), "container.mkv.missing_cues"));
}

TEST(MkvQcRulesTest, ClusterTimestampBackwardsIsError) {
    model::MkvDiagnostics mkv = HealthyMkv();
    mkv.cluster_timestamp_backwards = 3;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeResult(mkv)), "container.mkv.cluster_timestamp_backwards", &issue));
    EXPECT_EQ(model::IssueSeverity::Error, issue.severity);
    EXPECT_EQ(3.0, issue.metric_value);
    EXPECT_NE(std::string::npos, issue.detail.find("3")) << issue.detail;
}

TEST(MkvQcRulesTest, MissingCodecPrivateListsCodecIds) {
    model::MkvDiagnostics mkv = HealthyMkv();
    mkv.tracks_missing_codec_private = 2;
    mkv.missing_codec_private_codecs = "V_MPEG4/ISO/AVC, A_AAC";

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeResult(mkv)), "container.mkv.missing_codec_private", &issue));
    EXPECT_EQ(model::IssueSeverity::Error, issue.severity);
    EXPECT_NE(std::string::npos, issue.detail.find("V_MPEG4/ISO/AVC")) << issue.detail;
    EXPECT_NE(std::string::npos, issue.detail.find("A_AAC")) << issue.detail;
}

TEST(MkvQcRulesTest, UnknownSizeAndTruncationAndIncompleteScan) {
    QcRuleEngine engine;

    model::MkvDiagnostics unknown = HealthyMkv();
    unknown.unknown_size_elements = 4;
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeResult(unknown)), "container.mkv.unknown_size_element", &issue));
    EXPECT_EQ(model::IssueSeverity::Info, issue.severity);

    model::MkvDiagnostics truncated = HealthyMkv();
    truncated.truncated = true;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeResult(truncated)), "container.mkv.truncated"));

    model::MkvDiagnostics partial = HealthyMkv();
    partial.scan_complete = false;
    ASSERT_TRUE(HasIssue(engine.Evaluate(MakeResult(partial)), "container.mkv.scan_incomplete"));
}

TEST(MkvQcRulesTest, RulesAreAbsentForNonMkvContainer) {
    // analyzed=false（AVI/FLV/TS 等）时所有 container.mkv.* 必须静默
    model::MkvDiagnostics mkv = HealthyMkv();
    mkv.analyzed = false;
    mkv.has_cues = false;
    mkv.cluster_timestamp_backwards = 5;
    mkv.truncated = true;
    mkv.scan_complete = false;

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(mkv));
    for (const auto& issue : report.issues) {
        EXPECT_NE(0u, issue.rule_id.rfind("container.mkv.", 0)) << "未分析的容器不该命中 " << issue.rule_id;
    }
}

TEST(MkvQcRulesTest, DefaultRulesRegisterMkvFamily) {
    std::vector<model::QcRule> rules = qc::DefaultQcRules();
    for (const char* id : {"container.mkv.missing_cues", "container.mkv.cluster_timestamp_backwards",
                           "container.mkv.missing_codec_private", "container.mkv.unknown_size_element",
                           "container.mkv.truncated", "container.mkv.scan_incomplete"}) {
        const model::QcRule* rule = model::FindQcRule(rules, id);
        ASSERT_TRUE(rule != nullptr) << "规则表里漏了 " << id;
        EXPECT_EQ(model::IssueCategory::Container, rule->category);
        EXPECT_TRUE(rule->enabled);
        EXPECT_FALSE(rule->suggestion.empty()) << id << " 缺少可执行的修复建议";
    }
}

} // namespace
