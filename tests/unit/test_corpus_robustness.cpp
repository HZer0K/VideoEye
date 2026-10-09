#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "core/analysis/container/AsfStructureAnalyzer.h"
#include "core/analysis/container/EbmlAnalyzer.h"
#include "core/analysis/container/FlvStructureAnalyzer.h"
#include "core/analysis/diagnostics/Scte35Analyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"
#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"
#include "core/media/codec/ExtradataParser.h"
#include "core/media/container/IsobmffParser.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

// ==========================================================================
// 阶段 3.1: 确定性异常语料回归测试
//
// 语料不由手写二进制提交，而是 tests/corpus/generate_corpus.py 逐字节生成
// （每个样本的"畸形点"在脚本里都有注释）；这里只负责把每个样本喂给全部
// 自研解析器，钉住三件事：
//   1) 任何一个畸形样本都不允许崩溃；
//   2) 每个样本的解析必须有界终止（单样本预算 5 秒，语料全是 KB 级；
//      超过即说明解析循环失去上界）；
//   3) 解析产物列表必须有界（深度/节点/列表上限真的在起作用）；
//   4) 少数样本钉住明确语义（负 r 夹断、未知长度 EBML、BOM 小写 HLS、
//      不完整字幕 cue、错误 SCTE-35 CRC）。
//
// VIDEOEYE_CORPUS_DIR 由 tests/CMakeLists.txt 按源码树位置注入。
// ==========================================================================

namespace {

constexpr const char* kCorpusDir = VIDEOEYE_CORPUS_DIR;
using Clock = std::chrono::steady_clock;

std::string CorpusPath(const std::string& rel) {
    return std::string(kCorpusDir) + "/" + rel;
}

std::vector<uint8_t> ReadBytes(const std::string& rel) {
    std::ifstream in(CorpusPath(rel), std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string ReadText(const std::string& rel) {
    const std::vector<uint8_t> bytes = ReadBytes(rel);
    return std::string(bytes.begin(), bytes.end());
}

// ---- 有界性统计（递归，深度本身也有上限，不会把测试自己转晕） ----
size_t CountBoxes(const std::vector<videoeye::IsobmffBox>& boxes, int depth = 0) {
    if (depth > 32)
        return boxes.size();
    size_t n = boxes.size();
    for (const auto& b : boxes)
        n += CountBoxes(b.children, depth + 1);
    return n;
}

size_t CountEbmlNodes(const std::vector<videoeye::model::EbmlElementNode>& nodes, int depth = 0) {
    if (depth > 128)
        return nodes.size();
    size_t n = nodes.size();
    for (const auto& e : nodes)
        n += CountEbmlNodes(e.children, depth + 1);
    return n;
}

size_t CountContainerElements(const std::vector<videoeye::model::ContainerElement>& elems, int depth = 0) {
    if (depth > 128)
        return elems.size();
    size_t n = elems.size();
    for (const auto& e : elems)
        n += CountContainerElements(e.children, depth + 1);
    return n;
}

long long ElapsedMs(const Clock::time_point& t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// 全语料统一预算：KB 级样本里跑出 5 秒只有一种解释 —— 循环上界丢了。
constexpr long long kPerSampleBudgetMs = 5000;

} // namespace

// --------------------------------------------------------------------------
// 伞形测试: 全部语料 × 全部解析器
// --------------------------------------------------------------------------
TEST(CorpusRobustnessTest, AllSamplesTerminateWithBoundedResults) {
    namespace fs = std::filesystem;

    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(kCorpusDir)) {
        if (entry.is_regular_file())
            files.push_back(entry.path().string());
    }
    // 语料被删/挪走时，这条测试必须红，而不是"零样本全过"
    ASSERT_GE(files.size(), 20u) << "语料目录: " << kCorpusDir;

    for (const std::string& path : files) {
        const std::string name = fs::path(path).filename().string();

        // ---- 路径型解析器 ----
        {
            const auto t0 = Clock::now();

            videoeye::IsobmffFile mp4;
            videoeye::IsobmffParser::Parse(path, mp4);

            videoeye::model::EbmlAnalysisResult ebml;
            videoeye::EbmlAnalyzer ebml_analyzer;
            ebml_analyzer.Analyze(path, ebml, nullptr);

            videoeye::model::ContainerStructureResult flv;
            videoeye::FlvStructureAnalyzer flv_analyzer;
            flv_analyzer.Analyze(path, flv, nullptr);

            videoeye::model::ContainerStructureResult asf;
            videoeye::AsfStructureAnalyzer asf_analyzer;
            asf_analyzer.Analyze(path, asf, nullptr);

            videoeye::model::StreamingPackageResult hls;
            videoeye::HlsManifestAnalyzer hls_analyzer;
            hls_analyzer.AnalyzeFile(path, hls);

            videoeye::model::StreamingPackageResult dash;
            videoeye::DashManifestAnalyzer dash_analyzer;
            dash_analyzer.AnalyzeFile(path, dash);

            EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs) << "路径型解析器超预算: " << name;

            EXPECT_LE(CountBoxes(mp4.top_level), 200000u) << name;
            EXPECT_LE(CountEbmlNodes(ebml.element_tree), 200000u) << name;
            EXPECT_LE(CountContainerElements(flv.element_tree), 100000u) << name;
            EXPECT_LE(CountContainerElements(asf.element_tree), 100000u) << name;
            EXPECT_LE(hls.variants.size(), 128u) << name;
            EXPECT_LE(hls.playlists.size(), 256u) << name;
            for (const auto& pl : hls.playlists) {
                EXPECT_LE(pl.segments.size(), 5001u) << name;
            }
            EXPECT_LE(dash.representations.size(), 1024u) << name;
            for (const auto& rep : dash.representations) {
                EXPECT_LE(rep.segments.size(), 5001u) << name;
            }
        }

        // ---- 字节型解析器 ----
        {
            const std::vector<uint8_t> bytes = ReadBytes(name);
            const auto t0 = Clock::now();

            videoeye::BitstreamAnalyzer bs;
            bs.Analyze(bytes.data(), bytes.size(), AV_CODEC_ID_H264);
            videoeye::ExtradataParser::Parse(bytes.data(), bytes.size());

            const std::string text(bytes.begin(), bytes.end());
            std::vector<videoeye::ParsedSubtitleCue> cues;
            videoeye::SubtitleAnalyzer::ParseSrtText(text, cues);
            videoeye::SubtitleAnalyzer::ParseWebVttText(text, cues);
            videoeye::SubtitleAnalyzer::ParseAssText(text, cues);

            videoeye::model::Scte35Cue cue;
            videoeye::Scte35Analyzer::ParseSection(bytes.data(), bytes.size(), cue);

            EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs) << "字节型解析器超预算: " << name;
            EXPECT_LE(cues.size(), 100000u) << name;
        }
    }
}

// --------------------------------------------------------------------------
// codec/: 截断 avcC 不得产出"有效结果"（valid + 非空 error 的自相矛盾）
// --------------------------------------------------------------------------
TEST(CorpusRobustnessTest, TruncatedAvcCIsInvalidWithReason) {
    const std::vector<uint8_t> bytes = ReadBytes("codec/truncated_avcc.bin");
    ASSERT_FALSE(bytes.empty());

    const auto result = videoeye::ExtradataParser::Parse(bytes.data(), bytes.size());

    // SPS 声明 16 字节、实际只剩 4 字节：解析必须在写错误原因的同时
    // 把 valid 置回 false（阶段 3.2 "错误状态不可继续产出有效结果"）
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_TRUE(result.nal_units.empty());
}

// 极大 ue(v)：33 个前导零必须让 SPS 解析整体判无效，而不是移位溢出后
// 产出一份字段错位的"有效"SPS
TEST(CorpusRobustnessTest, HugeUevSpsIsRejected) {
    const std::vector<uint8_t> bytes = ReadBytes("codec/huge_uev.h264");
    ASSERT_FALSE(bytes.empty());

    videoeye::BitstreamAnalyzer analyzer;
    const videoeye::model::BitstreamAnalysisResult r = analyzer.Analyze(bytes.data(), bytes.size(), AV_CODEC_ID_H264);

    EXPECT_FALSE(r.h264_sps.present);
    EXPECT_EQ(r.width, 0);
    EXPECT_EQ(r.height, 0);
}

// --------------------------------------------------------------------------
// container/: 空文件与极短文件
// --------------------------------------------------------------------------
TEST(CorpusRobustnessTest, EmptyAndTinyFilesRejectedByAllContainerParsers) {
    for (const char* rel : {"container/empty.bin", "container/tiny.bin"}) {
        const std::string path = CorpusPath(rel);

        videoeye::IsobmffFile mp4;
        EXPECT_FALSE(videoeye::IsobmffParser::Parse(path, mp4)) << rel;

        videoeye::model::EbmlAnalysisResult ebml;
        videoeye::EbmlAnalyzer ebml_analyzer;
        EXPECT_FALSE(ebml_analyzer.Analyze(path, ebml, nullptr)) << rel;
        EXPECT_FALSE(ebml.valid) << rel;

        videoeye::model::ContainerStructureResult flv;
        videoeye::FlvStructureAnalyzer flv_analyzer;
        EXPECT_FALSE(flv_analyzer.Analyze(path, flv, nullptr)) << rel;

        videoeye::model::ContainerStructureResult asf;
        videoeye::AsfStructureAnalyzer asf_analyzer;
        EXPECT_FALSE(asf_analyzer.Analyze(path, asf, nullptr)) << rel;
    }
}

// 错误 MP4 largesize：size==1 时 largesize=8 连 box 头都装不下，必须当场停
TEST(CorpusRobustnessTest, Mp4BadLargesizeStopsAtFirstBox) {
    const std::string path = CorpusPath("container/mp4_bad_largesize.mp4");
    videoeye::IsobmffFile file;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(path, file));

    ASSERT_EQ(file.top_level.size(), 1u);
    EXPECT_EQ(file.top_level_order.size(), 1u);
    EXPECT_EQ(file.top_level[0].type, "ftyp");
}

// largesize = 0xFFFFFFFFFFFFFFF8：pos+size 在 uint64 上回绕，
// 朴素边界比较会把它当合法 box 然后死循环 —— 必须被溢出检查截住
TEST(CorpusRobustnessTest, Mp4SizeOverflowIsClampedToFileEnd) {
    const std::string path = CorpusPath("container/mp4_size_overflow.mp4");
    videoeye::IsobmffFile file;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(path, file));

    // ftyp + free（被夹到文件末尾），不允许出现更多"幽灵 box"
    ASSERT_EQ(file.top_level.size(), 2u);
    for (const auto& box : file.top_level) {
        EXPECT_LE(box.size, file.file_size) << box.type;
    }
}

// 截断 MP4：moov 声明 4096 字节、文件到此结束，box 必须被夹到文件末尾
TEST(CorpusRobustnessTest, Mp4TruncatedMoovIsBounded) {
    const std::string path = CorpusPath("container/truncated.mp4");
    videoeye::IsobmffFile file;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(path, file));

    ASSERT_EQ(file.top_level.size(), 2u);
    ASSERT_EQ(file.top_level[1].type, "moov");
    EXPECT_LE(file.top_level[1].size, file.file_size);
    EXPECT_LE(CountBoxes(file.top_level), 8u);
}

// 错误 FLV header：魔数 "FLA" 必须整体判失败，而不是带病继续
TEST(CorpusRobustnessTest, FlvBadHeaderRejected) {
    const std::string path = CorpusPath("container/flv_bad_header.flv");
    videoeye::model::ContainerStructureResult result;
    videoeye::FlvStructureAnalyzer analyzer;
    EXPECT_FALSE(analyzer.Analyze(path, result, nullptr));
    EXPECT_FALSE(result.error_message.empty());
}

// 错误 FLV DataSize（声明 0xFFFFFF、实际 6 字节）：seek 到文件尾后必须停
TEST(CorpusRobustnessTest, FlvWrongTagSizeTerminates) {
    const std::string path = CorpusPath("container/flv_wrong_size.flv");
    videoeye::model::ContainerStructureResult result;
    videoeye::FlvStructureAnalyzer analyzer;
    ASSERT_TRUE(analyzer.Analyze(path, result, nullptr));

    ASSERT_EQ(result.element_tree.size(), 1u);
    EXPECT_EQ(result.element_tree[0].children.size(), 1u);
}

// ASF 对象长度小于公共头（size=10 < 24）：obj_size-24 无符号下溢后
// 不得按天文数字申请内存
TEST(CorpusRobustnessTest, AsfObjectShorterThanHeaderIsClipped) {
    const std::string path = CorpusPath("container/asf_object_short.wmv");
    videoeye::model::ContainerStructureResult result;
    videoeye::AsfStructureAnalyzer analyzer;
    ASSERT_TRUE(analyzer.Analyze(path, result, nullptr));

    ASSERT_EQ(result.element_tree.size(), 1u);
    EXPECT_EQ(result.element_tree[0].children.size(), 1u);
}

// --------------------------------------------------------------------------
// container/: EBML
// --------------------------------------------------------------------------
// 未知长度（8 字节全 1）的 Segment 与 Cluster 必须按"延伸到父元素末尾"处理
TEST(CorpusRobustnessTest, EbmlUnknownSizeElementsParseAsClamped) {
    const std::string path = CorpusPath("container/ebml_unknown_size.mkv");
    videoeye::model::EbmlAnalysisResult result;
    videoeye::EbmlAnalyzer analyzer;
    ASSERT_TRUE(analyzer.Analyze(path, result, nullptr));

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.doc_type, "matroska");
    EXPECT_EQ(result.total_clusters, 1);
    EXPECT_TRUE(result.error_message.empty());
}

// 100 层自嵌套 Cluster：递归必须在 kMaxDepth 处停手，并把结果判为无效
TEST(CorpusRobustnessTest, EbmlDeepNestingHitsDepthLimitAndIsInvalid) {
    const std::string path = CorpusPath("container/ebml_deep_nesting.mkv");
    videoeye::model::EbmlAnalysisResult result;
    videoeye::EbmlAnalyzer analyzer;

    const auto t0 = Clock::now();
    const bool ok = analyzer.Analyze(path, result, nullptr);

    EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error_message.empty());
}

// 截断 MKV（Segment 声明 1000 字节、实际 20）：解析必须有界终止
TEST(CorpusRobustnessTest, EbmlTruncatedSegmentTerminates) {
    const std::string path = CorpusPath("container/truncated.mkv");
    videoeye::model::EbmlAnalysisResult result;
    videoeye::EbmlAnalyzer analyzer;

    const auto t0 = Clock::now();
    analyzer.Analyze(path, result, nullptr);

    EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs);
    EXPECT_LE(CountEbmlNodes(result.element_tree), 16u);
}

// --------------------------------------------------------------------------
// streaming/: HLS 与 DASH
// --------------------------------------------------------------------------
// 含 BOM + 全小写标签的清单：剥 BOM、标签大小写归一后必须正常解析
TEST(CorpusRobustnessTest, HlsBomLowercaseManifestIsAccepted) {
    const std::string text = ReadText("streaming/hls_bom_lowercase.m3u8");
    ASSERT_FALSE(text.empty());

    videoeye::model::StreamingPackageResult out;
    const bool ok = videoeye::HlsManifestAnalyzer::ParseText(text, kCorpusDir, out);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(out.valid);
    EXPECT_EQ(out.kind, videoeye::model::StreamingKind::HlsMedia);
    ASSERT_EQ(out.playlists.size(), 1u);
    EXPECT_EQ(out.playlists[0].SegmentCount(), 2);
    EXPECT_TRUE(out.error_message.empty());
}

// 负 DASH r（r="-5"）：夹到"不重复"后展开 1 段，加上 r=2 的 3 段，恰好 4 段；
// 不夹断的实现会回绕成 43 亿次展开
TEST(CorpusRobustnessTest, DashNegativeRIsClamped) {
    const std::string text = ReadText("streaming/dash_negative_r.mpd");
    ASSERT_FALSE(text.empty());

    videoeye::model::StreamingPackageResult out;
    const bool ok = videoeye::DashManifestAnalyzer::ParseText(text, kCorpusDir, out);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(out.valid);
    ASSERT_EQ(out.representations.size(), 1u);
    EXPECT_EQ(out.representations[0].segments.size(), 4u);
    EXPECT_FALSE(out.representations[0].truncated);
}

// 极端 duration/timescale（算出 1e15 段）：必须在分片上限处截断并快速返回，
// 这是"解析一份清单永不返回"的直接回归
TEST(CorpusRobustnessTest, DashExtremeSegmentCountIsTruncated) {
    const std::string text = ReadText("streaming/dash_extreme_count.mpd");
    ASSERT_FALSE(text.empty());

    const auto t0 = Clock::now();
    videoeye::model::StreamingPackageResult out;
    const bool ok = videoeye::DashManifestAnalyzer::ParseText(text, kCorpusDir, out);

    EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs);
    EXPECT_TRUE(ok);
    ASSERT_EQ(out.representations.size(), 1u);
    EXPECT_EQ(out.representations[0].segments.size(), 5000u);
    EXPECT_TRUE(out.representations[0].truncated);
}

// --------------------------------------------------------------------------
// subtitle/: 不完整 cue 与 SCTE-35 CRC
// --------------------------------------------------------------------------
TEST(CorpusRobustnessTest, SrtIncompleteCueIsDropped) {
    const std::string text = ReadText("subtitle/srt_incomplete_cue.srt");
    ASSERT_FALSE(text.empty());

    std::vector<videoeye::ParsedSubtitleCue> cues;
    videoeye::SubtitleAnalyzer::ParseSrtText(text, cues);

    // 第二条的"-->"断成"--"：整条丢弃；第一条与第三条（无结尾空行）保留
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_TRUE(cues[0].valid);
    EXPECT_DOUBLE_EQ(cues[0].start_seconds, 1.0);
    EXPECT_DOUBLE_EQ(cues[0].end_seconds, 2.5);
    EXPECT_TRUE(cues[1].valid);
    EXPECT_DOUBLE_EQ(cues[1].start_seconds, 8.0);
    EXPECT_DOUBLE_EQ(cues[1].end_seconds, 9.0);
}

TEST(CorpusRobustnessTest, VttIncompleteCueIsDropped) {
    const std::string text = ReadText("subtitle/vtt_incomplete_cue.vtt");
    ASSERT_FALSE(text.empty());

    std::vector<videoeye::ParsedSubtitleCue> cues;
    videoeye::SubtitleAnalyzer::ParseWebVttText(text, cues);

    // 第二条时间行只剩 "-->"、结束时间被截掉：整条丢弃
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_TRUE(cues[0].valid);
    EXPECT_DOUBLE_EQ(cues[0].start_seconds, 1.0);
    EXPECT_DOUBLE_EQ(cues[0].end_seconds, 2.5);
}

// 结构合法的段 + 正确 CRC：valid 与 crc_valid 都为真
TEST(CorpusRobustnessTest, Scte35ValidSectionPassesCrc) {
    const std::vector<uint8_t> bytes = ReadBytes("subtitle/scte35_valid_crc.bin");
    ASSERT_FALSE(bytes.empty());

    videoeye::model::Scte35Cue cue;
    EXPECT_TRUE(videoeye::Scte35Analyzer::ParseSection(bytes.data(), bytes.size(), cue));

    EXPECT_TRUE(cue.valid);
    EXPECT_TRUE(cue.crc_checked);
    EXPECT_TRUE(cue.crc_valid);
    EXPECT_TRUE(cue.parse_error.empty());
}

// CRC 错了：段本身仍可解析（valid=true），但 crc_valid 必须为 false ——
// "CRC 坏了"要能被质检规则看见，而不是被当成解析失败吞掉
TEST(CorpusRobustnessTest, Scte35BadCrcIsMarkedNotInvalid) {
    const std::vector<uint8_t> bytes = ReadBytes("subtitle/scte35_bad_crc.bin");
    ASSERT_FALSE(bytes.empty());

    videoeye::model::Scte35Cue cue;
    EXPECT_TRUE(videoeye::Scte35Analyzer::ParseSection(bytes.data(), bytes.size(), cue));

    EXPECT_TRUE(cue.valid);
    EXPECT_TRUE(cue.crc_checked);
    EXPECT_FALSE(cue.crc_valid);
}