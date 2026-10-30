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
// 三条判据
// --------
//   1) 每个枚举值都恰好出现一次（新增维度却忘了在聚合里加一段，这里立刻红）；
//   2) 七种容器**各自**都能把「容器体检」点亮成"执行过"，且能力评级为完整；
//   3) 体检没扫完（scan_complete=false）时必须降级成 Partial 并给出理由。
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
using videoeye::model::ContainerDiagnosticsInfo;
using videoeye::model::DimensionStatus;

constexpr int kDimensionCount = static_cast<int>(AnalysisDimension::Count);

// 一份空结果跑一遍聚合。seed 用于在收尾前塞入某个维度的事实。
AnalysisResult Finalize(AnalysisOptions options, const std::function<void(AnalysisResult&)>& seed) {
    AnalysisResult result;
    if (seed)
        seed(result);
    AnalysisResultAssembler assembler(result, options);
    assembler.FinalizeDimensionStatus(0, 0);
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
