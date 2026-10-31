#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "core/analysis/codec/FrameOrderTracker.h"
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
            // VVC 也过一遍：它的 NAL header 是 2 字节、参数集解析路径独立，
            // 只喂 H.264 等于这条路径在语料上从没被跑过。
            videoeye::BitstreamAnalyzer bs_vvc;
            bs_vvc.Analyze(bytes.data(), bytes.size(), AV_CODEC_ID_VVC);
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

// HEVC PPS 的 num_tile_columns_minus1 是荒谬大的 ue(v)（20 亿，规范上限 19）：
// 拿它当循环上界就是 20 亿次 push_back（CI 的 FuzzBitstreamExtradata 就是这么
// 在 17 字节输入上 10 秒超时的）。必须判整份 PPS 无效，而不是按它分配/循环。
TEST(CorpusRobustnessTest, HevcPpsAbsurdTileCountIsRejected) {
    const std::vector<uint8_t> bytes = ReadBytes("codec/hevc_pps_absurd_tile_count.bin");
    ASSERT_FALSE(bytes.empty());

    videoeye::BitstreamAnalyzer analyzer;
    const Clock::time_point t0 = Clock::now();
    const videoeye::model::BitstreamAnalysisResult r = analyzer.Analyze(bytes.data(), bytes.size(), AV_CODEC_ID_HEVC);

    EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs);
    EXPECT_FALSE(r.hevc_pps.present);
    EXPECT_EQ(r.hevc_pps.num_tile_columns_minus1, 0);
}

// --------------------------------------------------------------------------
// codec/VVC: 参数集与帧头的拒绝路径
//
// 这几条是 VVC 帧级解析能"被信任"的前提：畸形样本要么被**整份拒绝**，要么
// 严格回滚到"位流位置仍然确定"的那个前缀，绝不允许读出一份看起来自洽的字段。
// 所有断言都同时限时 —— 这类缺陷的表现形式通常不是崩溃，而是解析循环失去上界。
//
// ❗ 注意两种口径的差别（测过就知道，别抄成一样的）：
//    SPS 中途非法 => present=false，但**已读到的半截字段还在**；
//    PPS 深解析非法 => present=true + 后置字段全 0 + parse_depth=SubpicIdMapping。
//
// ❗ 也注意这两条断言**能**证明什么、**不能**证明什么。两条 SPS 样本的守护点
//    在上界上，但"样本被拒"这个结果并不能证明上界起作用：荒谬值之后位流本来
//    就不够读，尾巴耗尽同样会置错判非法。实测把 `sps_num_subpics_minus1` 的守卫
//    换成"夹到 3 次"、把 `ReadUEBounded(31)` 换回 `ReadUE()`，本用例**仍然全绿**
//    （已跑过变异）。所以：
//      * 上界真正的守护是这里的**限时断言**（去掉守卫会让循环次数由码流决定）与
//        ReadUE 自身 32 位前缀的上限，不是下面的 present/Failed；
//      * 别为了"让变异变红"去给样本加全 0 尾巴 —— 那只会让每个 ue(v) 各吃 32 位，
//        样本反而更难读通（试过）。
// --------------------------------------------------------------------------
TEST(CorpusRobustnessTest, VvcAbsurdParameterSetCountsAreRejected) {
    // 两条 SPS 样本：参数集存在但字段荒谬 → 整份判非法 → Failed。
    const char* kSpsSamples[] = {
        "codec/vvc_sps_absurd_subpic_count.bin",
        "codec/vvc_sps_absurd_poc_msb_len.bin",
    };
    for (const char* rel : kSpsSamples) {
        const std::vector<uint8_t> bytes = ReadBytes(rel);
        ASSERT_FALSE(bytes.empty()) << rel;

        videoeye::BitstreamAnalyzer analyzer;
        const Clock::time_point t0 = Clock::now();
        const videoeye::model::BitstreamAnalysisResult r =
            analyzer.Analyze(bytes.data(), bytes.size(), AV_CODEC_ID_VVC);
        EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs) << rel;

        // ❗ 判"被拒绝"只能看 present —— 三条参数集解析路径（H.264 / HEVC / VVC）
        //    都是"就地解析、中途 return sps"，拒绝时**已读到的那半截字段仍在**
        //    （这两条样本的宽高就是 1920x1080，看起来完全自洽）。
        //    所以不要在别处拿 present==false 的参数字段当数据用；这里也**不**断言
        //    宽高为 0，那是给一个并不存在的约定上锁。真正的守护是下面这条 Failed。
        EXPECT_FALSE(r.vvc_sps.present) << rel;
        // 有 SPS 输入却没解析出来 => ParseFailed => Failed（与 HEVC 同口径）
        EXPECT_EQ(r.capability, videoeye::model::AnalysisCapability::Failed) << rel;
        // 拒绝路径必须给出原因，否则界面上只是一个"分析失败"的空壳
        EXPECT_FALSE(r.capability_note.empty()) << rel;
    }

    // PPS 样本：荒谬的 pps_num_exp_tile_columns_minus1。VVC 这里不是整份作废 ——
    // tile/slice 划分段之前解出的字段（宽高、subpic id 映射）与这段无关，
    // 落非法时的契约是「保住浅字段 + 后置字段整体回滚 + parse_depth 停在
    // SubpicIdMapping」（见 VvcBitstreamParser.cpp 顶部那三条原则）。
    // 对齐 HEVC 的 tile 样本"整份拒绝"会得到一个与设计不符的期望。
    {
        const std::vector<uint8_t> bytes = ReadBytes("codec/vvc_pps_absurd_tile_count.bin");
        ASSERT_FALSE(bytes.empty());

        videoeye::BitstreamAnalyzer analyzer;
        const Clock::time_point t0 = Clock::now();
        const videoeye::model::BitstreamAnalysisResult r =
            analyzer.Analyze(bytes.data(), bytes.size(), AV_CODEC_ID_VVC);
        EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs);

        // 浅字段可信（它们在畸形点之前，位流位置没有歧义）
        EXPECT_TRUE(r.vvc_pps.present);
        EXPECT_EQ(r.vvc_pps.pps_pic_width_in_luma_samples, 1920);
        EXPECT_EQ(r.vvc_pps.pps_pic_height_in_luma_samples, 1080);
        // 深解析失败 => 回滚到全 0，且 depth 明说"没走完"
        EXPECT_EQ(r.vvc_pps.pps_num_exp_tile_columns_minus1, 0);
        EXPECT_EQ(r.vvc_pps.pps_num_exp_tile_rows_minus1, 0);
        EXPECT_FALSE(r.vvc_pps.pps_rect_slice_flag);
        EXPECT_NE(r.vvc_pps.parse_depth, videoeye::model::VvcPpsInfo::ParseDepth::Full);
        // PPS-only 输入落在 Partial，且原因可见
        EXPECT_EQ(r.capability, videoeye::model::AnalysisCapability::Partial);
        EXPECT_FALSE(r.capability_note.empty());
    }
}

// 截断的 vvcC：与 avcC / av1C 同口径 —— valid=false 且带原因。
// 旧实现只写 error_message、valid 仍为 true，上层会把全 0 的 config 当成
// 一次成功的配置记录解析（详见 generate_corpus.py 里这条样本的注释）。
TEST(CorpusRobustnessTest, VvcTruncatedVvcCIsInvalidWithReason) {
    const std::vector<uint8_t> bytes = ReadBytes("codec/vvc_truncated_vvcc.bin");
    ASSERT_FALSE(bytes.empty());

    const videoeye::ExtradataResult parsed =
        videoeye::ExtradataParser::ParseWithFormat(videoeye::ExtradataFormat::VvcC, bytes.data(), bytes.size());
    EXPECT_FALSE(parsed.valid);
    EXPECT_FALSE(parsed.error_message.empty());
    // 未被完整读到的 NAL 不得入库（否则调用方会拿到半截 SPS 当参数集）
    EXPECT_TRUE(parsed.nal_units.empty());
}

// 畸形 VVC 样本喂给帧级追踪器：不许产帧，也不许卡住。
// ❗ "不崩"是不够的 —— 一条 POC 全 0 的假时间线同样会让报告说谎，
//    所以这里钉住 frames_observed == 0。
TEST(CorpusRobustnessTest, VvcTrackerDoesNotInventFramesFromMalformedSamples) {
    const char* kSamples[] = {
        "codec/vvc_sps_absurd_subpic_count.bin", "codec/vvc_sps_absurd_poc_msb_len.bin",
        "codec/vvc_pps_absurd_tile_count.bin",   "codec/vvc_ph_missing_pps.bin",
        "codec/vvc_slice_truncated.bin",         "codec/vvc_truncated_vvcc.bin",
    };

    for (const char* rel : kSamples) {
        const std::vector<uint8_t> bytes = ReadBytes(rel);
        ASSERT_FALSE(bytes.empty()) << rel;

        videoeye::FrameOrderTracker tracker;
        tracker.Reset(AV_CODEC_ID_VVC, 0);
        ASSERT_TRUE(tracker.enabled()) << rel;
        tracker.SetFraming(videoeye::FrameOrderTracker::Framing::AnnexB);

        const Clock::time_point t0 = Clock::now();
        tracker.Feed(bytes.data(), bytes.size(), false, 0.0, 0);
        const videoeye::model::FrameOrderTimeline& timeline = tracker.Finish();
        EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs) << rel;

        EXPECT_EQ(timeline.frames_observed, 0) << rel;
        EXPECT_TRUE(timeline.frames.empty()) << rel;
        EXPECT_FALSE(timeline.available) << rel;
        // 不可用时必须给原因，不能空着让人猜
        EXPECT_FALSE(timeline.unavailable_reason.empty()) << rel;
    }
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

// ASF Header Object 声明的长度溢出 int64：obj_start + obj_size 变负数，Seek(负数)
// 被夹到 0，下一轮又读到文件头 —— 循环里"每轮必须严格前进"这条不成立就原地打转
// （FuzzContainerStructure 在 31 字节输入上 10 秒超时，就是这条）。
// 这里的样本与 fuzz 抓到的输入逐字节一致。
TEST(CorpusRobustnessTest, AsfHeaderSizeOverflowTerminatesWithoutInventingObjects) {
    const std::string path = CorpusPath("container/asf_header_size_overflow.wmv");

    videoeye::model::ContainerStructureResult asf;
    videoeye::AsfStructureAnalyzer analyzer;
    const Clock::time_point t0 = Clock::now();
    analyzer.Analyze(path, asf, nullptr);

    EXPECT_LT(ElapsedMs(t0), kPerSampleBudgetMs);
    // 文件头之后只有 7 个字节，连一个 24 字节的对象头都放不下：
    // 不允许凭空"解析出"对象，也不允许把声明长度当真的走一遍。
    EXPECT_EQ(asf.diagnostics.asf.parsed_header_objects, 0u);
    EXPECT_FALSE(asf.streams.size() > 0);
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
