// SliceSummary / SeiSummary 文案契约的回归（core/domain/model/SliceSeiInfo.*）。
//
// 守的是"结论层不许自证"这一条：QP 的 min/max/avg 只对**成功解出 QP 的那部分
// slice** 成立（分母是 qp_parsed_slices，不是 slice_count）。把这层信息吞掉，
// 读报告的人会把区间当成全片逐帧曲线 —— 数字看着完全合理，覆盖面却是错的。
//
// 这不是假想：VVC 的 sh_qp_delta 排在 ref_pic_lists() / pred_weight_table()
// 之后，非 I slice 很容易整段拿不到 QP，于是"1 个 IDR 有 QP、52 个 P/B 没有"
// 是常态。只写 QP=[22, 34] avg=26.7 就是一句误导。

#include "core/domain/model/SliceSeiInfo.h"

#include <string>

#include <gtest/gtest.h>

namespace {

using videoeye::model::SeiSummary;
using videoeye::model::SliceSeiSummary;
using videoeye::model::SliceSummary;

SliceSummary QpCovering(int parsed_slices, int total_slices) {
    SliceSummary s;
    s.analyzed = true;
    s.supported = true;
    s.slice_count = total_slices;
    s.qp_parsed_slices = parsed_slices;
    s.qp_min = 22;
    s.qp_max = 34;
    s.qp_avg = 26.7;
    s.qp_valid = parsed_slices > 0;
    return s;
}

// ---- QP 覆盖面 ----

TEST(SliceSeiSummaryTests, QpCoveringFewerSlicesReportsDenominator) {
    const SliceSummary s = QpCovering(3, 53);
    const std::string text = s.Summary();
    EXPECT_NE(text.find("QP=[22, 34]"), std::string::npos) << text;
    EXPECT_NE(text.find("覆盖 3/53 slice"), std::string::npos)
        << "QP 只覆盖部分 slice 时必须报出分母，否则会被读成全片逐帧曲线: " << text;
}

TEST(SliceSeiSummaryTests, QpCoveringAllSlicesOmitsDenominator) {
    const SliceSummary s = QpCovering(10, 10);
    const std::string text = s.Summary();
    EXPECT_NE(text.find("QP=[22, 34]"), std::string::npos) << text;
    EXPECT_EQ(text.find("覆盖"), std::string::npos) << "全覆盖时不该再挂一句分母，那只是噪音: " << text;
}

TEST(SliceSeiSummaryTests, QpCoveringOneOfManyStillShowsDenominator) {
    // VVC 最典型的形态：只有 IDR 能出 QP。1/52 尤其不能省分母。
    const SliceSummary s = QpCovering(1, 52);
    const std::string text = s.Summary();
    EXPECT_NE(text.find("覆盖 1/52 slice"), std::string::npos) << text;
}

TEST(SliceSeiSummaryTests, ZeroSliceCountDoesNotRenderAFakeDenominator) {
    // 防御：slice_count 为 0 时 qp_parsed_slices 也是 0，不该出现 "0/0"。
    SliceSummary s;
    s.analyzed = true;
    s.supported = true;
    s.qp_valid = true;
    const std::string text = s.Summary();
    EXPECT_EQ(text.find("覆盖"), std::string::npos) << text;
}

// ---- QP 不可用时的原因 ----

TEST(SliceSeiSummaryTests, QpUnavailableUsesTheGivenReason) {
    SliceSummary s;
    s.analyzed = true;
    s.supported = true;
    s.slice_count = 8;
    s.qp_valid = false;
    s.qp_unavailable_reason = "非 I slice：前面压着 ref_pic_lists()";
    const std::string text = s.Summary();
    EXPECT_NE(text.find("QP 不可用"), std::string::npos) << text;
    EXPECT_NE(text.find("ref_pic_lists()"), std::string::npos)
        << "具体原因必须原样透出，统一写成\"缺少 PPS 基准\"会把人带错方向: " << text;
}

TEST(SliceSeiSummaryTests, QpUnavailableFallsBackToDefaultReason) {
    SliceSummary s;
    s.analyzed = true;
    s.supported = true;
    s.qp_valid = false;
    const std::string text = s.Summary();
    EXPECT_NE(text.find("pic_init_qp_minus26"), std::string::npos) << text;
}

// ---- 未分析 / 不支持 ----

TEST(SliceSeiSummaryTests, NotAnalyzedSaysSo) {
    EXPECT_EQ(SliceSummary().Summary(), "未分析");
    EXPECT_EQ(SliceSeiSummary().Summary(), "未分析");
}

TEST(SliceSeiSummaryTests, UnsupportedCarriesItsReason) {
    SliceSummary s;
    s.analyzed = true;
    s.supported = false;
    s.unsupported_reason = "该编码没有 slice 解析";
    EXPECT_NE(s.Summary().find("该编码没有 slice 解析"), std::string::npos);
}

// ---- 顶层拼接与截断标注 ----

TEST(SliceSeiSummaryTests, TopLevelJoinsSliceAndSei) {
    SliceSeiSummary sum;
    sum.analyzed = true;
    sum.codec_name = "vvc";
    sum.slices = QpCovering(3, 53);
    sum.sei.analyzed = true;
    sum.sei.total_messages = 2;
    const std::string text = sum.Summary();
    EXPECT_NE(text.find("slice 53 个"), std::string::npos) << text;
    EXPECT_NE(text.find("SEI"), std::string::npos) << text;
    EXPECT_NE(text.find("覆盖 3/53 slice"), std::string::npos) << text;
}

TEST(SliceSeiSummaryTests, TruncatedWindowIsStated) {
    SliceSeiSummary sum;
    sum.analyzed = true;
    sum.slices = QpCovering(2, 2);
    sum.sei.analyzed = true;
    sum.truncated = true;
    sum.packets_scanned = 60;
    const std::string text = sum.Summary();
    EXPECT_NE(text.find("窗口截断"), std::string::npos) << text;
    EXPECT_NE(text.find("60"), std::string::npos) << text;
}

// ---- SEI：没有 HDR 消息时不能只说"0 条" ----

TEST(SliceSeiSummaryTests, SeiWithoutHdrMessagesSaysNothingDetected) {
    SeiSummary sei;
    sei.analyzed = true;
    sei.total_messages = 5;
    const std::string text = sei.Summary();
    EXPECT_NE(text.find("未检出"), std::string::npos)
        << "有消息但没有一条 HDR 相关的，必须说\"未检出\"而不是让读者以为一条 SEI 都没有: " << text;
}

} // namespace
