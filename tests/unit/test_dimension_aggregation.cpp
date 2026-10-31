// 「维度状态」聚合的回归：FinalizeDimensionStatus 到底把哪些维度算成"跑过"。
//
// 为什么需要这一层（现有的两条用例都盖不到）
// ----------------------------------------
//   * test_dimension_status.cpp 测的是 DimensionStatus 这个**结构**自己的口径
//     （MarkConclusive 在什么条件下允许判"完整"），它不碰任何分析器；
//   * test_analysis_pipeline_edges.cpp 只验"dimensions 是权威口径、历史布尔要跟着走"，
//     用的是音频位流这一个维度。
// 于是"每个维度都被填进去了吗""某项事实到底算不算这个维度跑过"这两件事长期没人守。
//
// 2026-10-30 现场（本文件存在的原因）
// ----------------------------------
// ContainerDiagnosticsInfo 有 **7** 个成员（avi/flv/asf/ogg/mkv/**mxf**/**mpeg_ps**），
// DimensionStatus.h 的枚举注释与 QcRuleEngine 的注释也都写着 7 种容器，唯独
// FinalizeDimensionStatus 的聚合只看前 5 个。后果不是"少一条笔记"，而是报告自相矛盾：
// 一个 MXF 文件，container.mxf.* 规则照常跑、照常报缺陷，而「容器体检」这个维度却
// 被判成"未执行"，理由还写着"不是 AVI/FLV/ASF/OGG/MKV 容器"—— 那份文件正是 MXF。
// 这正是"证据与覆盖"这套结构要消灭的假象，所以它必须有用例兜住。
//
// 五条判据
// --------
//   1) 每个枚举值都恰好出现一次（新增维度却忘了在聚合里加一段，这里立刻红）；
//   2) 七种容器**各自**都能把「容器体检」点亮成"执行过"，且能力评级为完整；
//   3) 体检没扫完（scan_complete=false）时必须降级成 Partial 并给出理由；
//   4) 每个维度的结论**只由它自己那份事实决定**（"有事实 -> 执行"与
//      "选项开着但没有事实 -> 必须未执行"两张面都验）—— 见文末的 AllDimensionCases；
//   5) 维度在 dimensions 里的**顺序**固定（报告章节次序就是它）。
//
// 判据 4 是 2026-10-31 拆 FinalizeDimensionStatus 之前补的网：那个函数有 300 行、
// 17 个几乎同构的分支，拆成"每维度一个小函数"时**顺序变了、条件写漏了都不会有
// 编译错误**，而当时守着这块的只有判据 1~3（只覆盖容器体检一个维度）。先把它描述
// 成可执行的断言，再动刀。
//
// 造数据的方式是**直接写 result 里的事实字段**，不跑任何分析器：这一层测的是
// "拿到这些事实后怎么下结论"，与分析器本身对不对是两回事（后者各有专门的用例）。

#include <gtest/gtest.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/analysis/orchestration/detail/AnalysisResultAssembler.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/DimensionStatus.h"

namespace {

using videoeye::AnalysisOptions;
using videoeye::AnalysisResultAssembler;
using videoeye::model::AnalysisCapability;
using videoeye::model::AnalysisDimension;
using videoeye::model::AnalysisResult;
using videoeye::model::AudioBitstreamStream;
using videoeye::model::ContainerDiagnosticsInfo;
using videoeye::model::DimensionName;
using videoeye::model::DimensionStatus;
using videoeye::model::FrameOrderTimeline;
using videoeye::model::ParameterTimeline;
using videoeye::model::StreamDigest;

constexpr int kDimensionCount = static_cast<int>(AnalysisDimension::Count);

// 一份空结果跑一遍聚合。seed 用于在收尾前塞入某个维度的事实。
// audio_* 只有「音频 QC」用得到：它的覆盖分母是调用参数，不是 result 字段。
AnalysisResult Finalize(AnalysisOptions options, const std::function<void(AnalysisResult&)>& seed,
                        int audio_analyzed = 0, int audio_total = 0) {
    AnalysisResult result;
    if (seed)
        seed(result);
    AnalysisResultAssembler assembler(result, options);
    assembler.FinalizeDimensionStatus(audio_analyzed, audio_total);
    return result;
}

// 找不到就给一条空壳（调用方用 ASSERT 判过，这里只是免去到处判空）。
DimensionStatus StatusOrEmpty(const AnalysisResult& result, AnalysisDimension dimension) {
    const DimensionStatus* found = result.FindDimension(dimension);
    return found ? *found : DimensionStatus{};
}

// 七种容器的"点亮"动作。写成表而不是七段复制粘贴：漏一种时，红的是同一个用例名，
// 失败信息里带格式名，加一种也不用改判定逻辑。
struct FormatCase {
    const char* name;
    std::function<void(ContainerDiagnosticsInfo&)> mark;       // 只标 analyzed
    std::function<void(ContainerDiagnosticsInfo&)> incomplete; // analyzed + 没扫完
};

const std::vector<FormatCase>& AllContainerFormats() {
    static const std::vector<FormatCase> cases = {
        {"avi", [](ContainerDiagnosticsInfo& d) { d.avi.analyzed = true; },
         [](ContainerDiagnosticsInfo& d) {
             d.avi.analyzed = true;
             d.avi.scan_complete = false;
         }},
        {"flv", [](ContainerDiagnosticsInfo& d) { d.flv.analyzed = true; },
         [](ContainerDiagnosticsInfo& d) {
             d.flv.analyzed = true;
             d.flv.scan_complete = false;
         }},
        {"asf", [](ContainerDiagnosticsInfo& d) { d.asf.analyzed = true; },
         [](ContainerDiagnosticsInfo& d) {
             d.asf.analyzed = true;
             d.asf.scan_complete = false;
         }},
        {"ogg", [](ContainerDiagnosticsInfo& d) { d.ogg.analyzed = true; },
         [](ContainerDiagnosticsInfo& d) {
             d.ogg.analyzed = true;
             d.ogg.scan_complete = false;
         }},
        {"mkv", [](ContainerDiagnosticsInfo& d) { d.mkv.analyzed = true; },
         [](ContainerDiagnosticsInfo& d) {
             d.mkv.analyzed = true;
             d.mkv.scan_complete = false;
         }},
        // 下面两种就是 2026-10-30 漏掉的那两个。加进表里是为了让它们**只能**被当成
        // 一等公民对待 —— 谁再写 "AVI/FLV/ASF/OGG/MKV" 这串，这一行会红。
        {"mxf", [](ContainerDiagnosticsInfo& d) { d.mxf.analyzed = true; },
         [](ContainerDiagnosticsInfo& d) {
             d.mxf.analyzed = true;
             d.mxf.scan_complete = false;
         }},
        {"mpeg_ps", [](ContainerDiagnosticsInfo& d) { d.mpeg_ps.analyzed = true; },
         [](ContainerDiagnosticsInfo& d) {
             d.mpeg_ps.analyzed = true;
             d.mpeg_ps.scan_complete = false;
         }},
    };
    return cases;
}

} // namespace

// ---------------------------------------------------------------------------
// 判据 1：一个不多、一个不少
//
// SetDimension 对同维度是**覆盖**语义（幂等），所以"漏填一个"会让表少一行，
// "重复填一个"也会让表少一行 —— 两种错都收敛到同一个断言上。
// 新增 AnalysisDimension 取值时，这一条会先红，提醒去聚合里补一段。
// ---------------------------------------------------------------------------
TEST(DimensionAggregation, EveryDimensionAppearsExactlyOnce) {
    const AnalysisResult result = Finalize(AnalysisOptions{}, nullptr);

    std::set<int> seen;
    for (const DimensionStatus& status : result.dimensions) {
        const int code = static_cast<int>(status.dimension);
        EXPECT_TRUE(seen.insert(code).second) << "维度 " << static_cast<int>(status.dimension) << " 被填了两次";
    }
    EXPECT_EQ(static_cast<int>(result.dimensions.size()), kDimensionCount)
        << "填进去的维度数与 AnalysisDimension::Count 对不上 —— 新增维度时忘了在 "
           "FinalizeDimensionStatus 里补一段（或重复填导致覆盖）";
    for (int code = 0; code < kDimensionCount; ++code) {
        EXPECT_TRUE(seen.count(code) == 1) << "维度 " << code << " 没被填进结果，报告里会整段消失";
    }
}

// ---------------------------------------------------------------------------
// 判据 2：七种容器各自都算"容器体检跑过"
// ---------------------------------------------------------------------------
TEST(DimensionAggregation, LegacyContainerCoversEveryDiagnosticsFormat) {
    for (const FormatCase& format : AllContainerFormats()) {
        AnalysisOptions options; // analyze_legacy_container 默认开
        const AnalysisResult result =
            Finalize(options, [&format](AnalysisResult& r) { format.mark(r.container_diagnostics); });

        const DimensionStatus status = StatusOrEmpty(result, AnalysisDimension::LegacyContainer);
        EXPECT_TRUE(status.executed) << format.name << " 的体检事实已经填进 result，"
                                     << "「容器体检」维度却被判成未执行 —— 报告会显示"
                                     << "「" << status.failure_reason << "」，而那份文件正是 " << format.name;
        EXPECT_EQ(status.capability, AnalysisCapability::Supported)
            << format.name << " 扫完了却没能给出完整评级（实际 " << static_cast<int>(status.capability) << "）";
        EXPECT_TRUE(status.failure_reason.empty())
            << format.name << " 扫描完整，不该带失败理由: " << status.failure_reason;
        EXPECT_TRUE(result.DimensionExecuted(AnalysisDimension::LegacyContainer));
    }
}

// ---------------------------------------------------------------------------
// 判据 3：没扫完必须降级
//
// 与判据 2 是同一张表的两列：只验"点亮"会漏掉"点亮但半截"，而半截恰恰是最容易
// 被当成"没问题"的情况（结构扫描遇到畸形数据提前退出，后面的损伤全没看见）。
// ---------------------------------------------------------------------------
TEST(DimensionAggregation, LegacyContainerDegradesWhenScanIncomplete) {
    for (const FormatCase& format : AllContainerFormats()) {
        AnalysisOptions options;
        const AnalysisResult result =
            Finalize(options, [&format](AnalysisResult& r) { format.incomplete(r.container_diagnostics); });

        const DimensionStatus status = StatusOrEmpty(result, AnalysisDimension::LegacyContainer);
        EXPECT_TRUE(status.executed) << format.name;
        EXPECT_EQ(status.capability, AnalysisCapability::Partial)
            << format.name << " 没扫完，能力评级必须是 Partial（实际 " << static_cast<int>(status.capability) << "）";
        EXPECT_FALSE(status.failure_reason.empty())
            << format.name << " 降级必须给出理由，否则读者只看到 Partial 不知为何";
    }
}

// ---------------------------------------------------------------------------
// 反向：一份体检事实都没有时不得谎报"跑过"
//
// 这条守的是判据 2 的反面 —— 把「容器体检」改成无条件 executed 也能让上面两条全绿，
// 但那样每个非 AVI/MXF… 的文件都会多一行"已执行、无问题"的假结论。
// ---------------------------------------------------------------------------
TEST(DimensionAggregation, LegacyContainerStaysSkippedWithoutAnyFacts) {
    AnalysisOptions options;
    const AnalysisResult result = Finalize(options, nullptr);

    const DimensionStatus status = StatusOrEmpty(result, AnalysisDimension::LegacyContainer);
    EXPECT_FALSE(status.executed) << "没有任何体检事实时必须判未执行";
    EXPECT_FALSE(result.DimensionExecuted(AnalysisDimension::LegacyContainer));
    EXPECT_FALSE(status.failure_reason.empty()) << "未执行也要说清为什么，不能让读者当成「没问题」";
}

// ---------------------------------------------------------------------------
// 判据 4：每个维度只读自己那份事实
//
// 一行 = 一个维度 = 它的事实来源 + 它该给出的评级。两张面各自跑一遍：
//   * light_up 跑完 -> executed 必须为真，评级必须等于 lit；
//   * light_up 不跑（选项默认全开）-> executed 必须为假、给出理由、且结论不可信。
//
// 第二张面是**反向守护**：把任一维度改成无条件 executed 也能让第一张面全绿，
// 但那会给"选项开着、素材里没这类数据"的文件凭空造出"已执行、无问题"的假结论。
// ---------------------------------------------------------------------------
struct DimensionCase {
    AnalysisDimension dimension = AnalysisDimension::FileProbe;
    // 点亮这个维度：往 result 里塞它自己的事实（选项默认全开，不必改）。
    // 空 = 该维度不由全文件扫描产出（容器结构在「容器结构」页单独跑），永远不该 executed。
    std::function<void(AnalysisResult&)> light_up;
    // 音频 QC 的覆盖分母来自**调用参数**而不是 result 字段，所以单独给。
    int audio_analyzed = 0;
    int audio_total = 0;
    AnalysisCapability lit = AnalysisCapability::Supported;
    // 事实缺席（但选项开着）时的期望。
    bool dark_executed = false;
    AnalysisCapability dark = AnalysisCapability::Unavailable;
};

DimensionCase MakeCase(AnalysisDimension dimension, std::function<void(AnalysisResult&)> light_up,
                       AnalysisCapability lit = AnalysisCapability::Supported) {
    DimensionCase one;
    one.dimension = dimension;
    one.light_up = std::move(light_up);
    one.lit = lit;
    return one;
}

// 文件探测这条维度特殊：只要有流就算跑过，一条流都没有则是"跑了但失败"。
DimensionCase FileProbeCase() {
    DimensionCase one = MakeCase(AnalysisDimension::FileProbe, [](AnalysisResult& r) {
        StreamDigest video;
        video.index = 0;
        video.media_type = 0; // AVMEDIA_TYPE_VIDEO
        video.codec_name = "h264";
        r.streams.push_back(video);
    });
    one.dark_executed = true;
    one.dark = AnalysisCapability::Failed;
    return one;
}

DimensionCase AudioQcCase() {
    DimensionCase one = MakeCase(AnalysisDimension::AudioQc, [](AnalysisResult& r) {
        r.audio_qc.analyzed = true;
        r.audio_qc.coverage_ratio = 1.0;
    });
    one.audio_analyzed = 1;
    one.audio_total = 1;
    return one;
}

// 一条能追踪顺序的视频轨时间线（其余字段取默认）。
FrameOrderTimeline SampleFrameOrder(int stream_index) {
    FrameOrderTimeline timeline;
    timeline.stream_index = stream_index;
    timeline.available = true;
    timeline.order_source = "poc";
    timeline.frames_observed = 10;
    return timeline;
}

const std::vector<DimensionCase>& AllDimensionCases() {
    static const std::vector<DimensionCase> cases = {
        FileProbeCase(),
        // 容器结构：全文件扫描不产出这一项，任何时候都只能是"未执行 + 为什么"。
        MakeCase(AnalysisDimension::ContainerStructure, nullptr),
        MakeCase(AnalysisDimension::VideoBitstream,
                 [](AnalysisResult& r) {
                     r.bitstream_analyzed = true;
                     r.bitstream_analysis.capability = AnalysisCapability::Supported;
                     r.bitstream_analysis.stream_index = 0;
                 }),
        MakeCase(AnalysisDimension::FrameStructure,
                 [](AnalysisResult& r) {
                     r.bitstream_analysis.slice_sei.analyzed = true;
                     r.bitstream_analysis.slice_sei.packets_scanned = 128;
                 }),
        MakeCase(AnalysisDimension::FrameOrder,
                 [](AnalysisResult& r) { r.frame_order_timelines.push_back(SampleFrameOrder(0)); }),
        MakeCase(AnalysisDimension::ParameterTimeline,
                 [](AnalysisResult& r) {
                     ParameterTimeline timeline;
                     timeline.stream_index = 0;
                     timeline.packets_scanned = 128;
                     r.parameter_timelines.push_back(timeline);
                 }),
        MakeCase(AnalysisDimension::BitrateGop,
                 [](AnalysisResult& r) {
                     StreamDigest video;
                     video.index = 0;
                     video.media_type = 0;
                     r.streams.push_back(video);
                     r.bitrate_gop.frame_types_known = true;
                     r.bitrate_gop.total_frames = 300;
                 }),
        MakeCase(AnalysisDimension::ColorHdr,
                 [](AnalysisResult& r) {
                     r.color_hdr.analyzed = true;
                     r.color_hdr.color.analyzed = true;
                 }),
        AudioQcCase(),
        MakeCase(AnalysisDimension::AudioBitstream,
                 [](AnalysisResult& r) {
                     r.audio_bitstream_analyzed = true;
                     r.audio_bitstream.analyzed = true;
                     r.audio_bitstream.parsed_stream_count = 1;
                     AudioBitstreamStream stream;
                     stream.stream_index = 1;
                     stream.parsed_packets = 100;
                     r.audio_bitstream.streams.push_back(stream);
                 }),
        MakeCase(AnalysisDimension::Subtitle, [](AnalysisResult& r) { r.subtitle_analyzed = true; }),
        MakeCase(AnalysisDimension::Timecode, [](AnalysisResult& r) { r.timecode_analyzed = true; }),
        MakeCase(AnalysisDimension::AuxiliaryData, [](AnalysisResult& r) { r.aux_data_analyzed = true; }),
        MakeCase(AnalysisDimension::Mp4SampleTable,
                 [](AnalysisResult& r) {
                     r.mp4_samples_analyzed = true;
                     r.mp4_samples.valid = true;
                 }),
        MakeCase(AnalysisDimension::TsTransport,
                 [](AnalysisResult& r) {
                     r.ts_transport.applicable = true;
                     r.ts_transport.analyzed = true;
                     r.ts_transport.scan_complete = true;
                     r.ts_transport.scanned_packets = 1000;
                     r.ts_transport.estimated_packets = 1000;
                 }),
        MakeCase(AnalysisDimension::Streaming,
                 [](AnalysisResult& r) {
                     r.streaming_analyzed = true;
                     r.streaming_package.valid = true;
                 }),
        MakeCase(AnalysisDimension::LegacyContainer,
                 [](AnalysisResult& r) { r.container_diagnostics.mkv.analyzed = true; }),
    };
    return cases;
}

// 表本身也要守：漏一个维度就等于那个维度没有守护。
TEST(DimensionAggregation, FactTableCoversEveryDimension) {
    const std::vector<DimensionCase>& cases = AllDimensionCases();
    std::set<int> seen;
    for (const DimensionCase& one : cases) {
        const int code = static_cast<int>(one.dimension);
        EXPECT_TRUE(seen.insert(code).second) << "维度 " << DimensionName(one.dimension) << " 在表里出现了两次";
    }
    EXPECT_EQ(static_cast<int>(cases.size()), kDimensionCount)
        << "新增 AnalysisDimension 时必须在本表补一行 —— 否则这个维度的事实来源无人守护";
    for (int code = 0; code < kDimensionCount; ++code) {
        const AnalysisDimension dimension = static_cast<AnalysisDimension>(code);
        EXPECT_TRUE(seen.count(code) == 1) << "维度 " << DimensionName(dimension) << " 没有对应的用例";
    }
}

TEST(DimensionAggregation, EveryDimensionLightsUpFromItsOwnFacts) {
    for (const DimensionCase& one : AllDimensionCases()) {
        if (!one.light_up)
            continue;
        const AnalysisResult result = Finalize(AnalysisOptions{}, one.light_up, one.audio_analyzed, one.audio_total);
        const DimensionStatus status = StatusOrEmpty(result, one.dimension);
        EXPECT_TRUE(status.executed) << DimensionName(one.dimension)
                                     << " 的事实已经塞进 result，维度却被判成未执行 —— 报告会显示「"
                                     << status.failure_reason << "」";
        EXPECT_EQ(status.capability, one.lit)
            << DimensionName(one.dimension) << " 的评级与事实不符（实际 " << static_cast<int>(status.capability)
            << "，期望 " << static_cast<int>(one.lit) << "）";
        EXPECT_TRUE(result.DimensionExecuted(one.dimension))
            << DimensionName(one.dimension) << " 的历史布尔没跟着 dimensions 走";
    }
}

TEST(DimensionAggregation, EveryDimensionStaysSkippedWithoutFacts) {
    // 选项默认全开、result 里一条事实都没有 —— 这正是"素材里没这类数据"的样子。
    const AnalysisResult empty = Finalize(AnalysisOptions{}, nullptr);
    for (const DimensionCase& one : AllDimensionCases()) {
        const DimensionStatus status = StatusOrEmpty(empty, one.dimension);
        const std::string label = DimensionName(one.dimension);
        EXPECT_EQ(status.executed, one.dark_executed)
            << label << " 在没有任何事实的情况下被判成" << (status.executed ? "已执行" : "未执行")
            << " —— 报告会把「没这类数据」读成「查过且没问题」";
        EXPECT_EQ(status.capability, one.dark)
            << label << " 的缺席评级不对（实际 " << static_cast<int>(status.capability) << "）";
        if (!status.executed) {
            EXPECT_FALSE(status.failure_reason.empty()) << label << " 未执行却没给理由";
            EXPECT_FALSE(status.conclusive) << label << " 未执行却被判成「结论可信」";
        }
    }
}

// ---------------------------------------------------------------------------
// 判据 5：维度顺序
//
// 报告章节就是按这个顺序铺开的，而**顺序错了不会有任何编译错误**。这里钉死的
// 两条不显然的约定：
//   * 「帧级显示顺序」紧跟在「帧级结构」之后 —— 它们是同一次包扫描产出的两类结论，
//     读者按"帧结构 -> 帧顺序"连着看才对得上；枚举值 FrameOrder 排在末尾只是因为
//     已发布的取值不能重排。
//   * 「容器体检」在最后 —— 它只对七种老容器适用，放在最后不打断逐轨的叙事。
// ---------------------------------------------------------------------------
TEST(DimensionAggregation, DimensionsAppearInTheDocumentedOrder) {
    const AnalysisResult result = Finalize(AnalysisOptions{}, nullptr);

    const std::vector<AnalysisDimension> expected = {
        AnalysisDimension::FileProbe,      AnalysisDimension::ContainerStructure, AnalysisDimension::VideoBitstream,
        AnalysisDimension::FrameStructure, AnalysisDimension::FrameOrder,         AnalysisDimension::ParameterTimeline,
        AnalysisDimension::BitrateGop,     AnalysisDimension::ColorHdr,           AnalysisDimension::AudioQc,
        AnalysisDimension::AudioBitstream, AnalysisDimension::Subtitle,           AnalysisDimension::Timecode,
        AnalysisDimension::AuxiliaryData,  AnalysisDimension::Mp4SampleTable,     AnalysisDimension::TsTransport,
        AnalysisDimension::Streaming,      AnalysisDimension::LegacyContainer,
    };
    ASSERT_EQ(result.dimensions.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(result.dimensions[i].dimension, expected[i])
            << "第 " << i << " 行应该是 " << DimensionName(expected[i]) << "，实际是 "
            << DimensionName(result.dimensions[i].dimension) << " —— 报告章节会跟着错位";
    }
}
