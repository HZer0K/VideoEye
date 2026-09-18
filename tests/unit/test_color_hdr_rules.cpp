// ============================================================================
// 色彩与 HDR 元数据（core/analyzer/ColorHdrAnalyzer 的模型层 + QC 规则）单元测试
//
// 覆盖第 6 节功能的验收场景：
//   1) SDR Rec.709 样本 -> BT.709 / BT.709 / BT.709
//   2) HDR10 样本      -> PQ + BT.2020 + 10bit + MaxCLL/MaxFALL，且不报警
//   3) 缺元数据的 HDR  -> 输出 warning
//   4) full / limited range 必须正确区分，且冲突组合要告警
//
// 说明：模型层与规则层都不依赖 FFmpeg，因此这里直接构造 ColorInfo /
// HdrMetadataInfo 喂给 QcRuleEngine（FFmpeg 侧的实际映射由
// ColorHdrAnalyzer.cpp 里的 static_assert 保证与枚举一致）。
// ============================================================================

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/analyzer/AnalysisTask.h"
#include "core/analyzer/ColorHdrAnalyzer.h"
#include "core/analyzer/QcRuleEngine.h"
#include "core/model/QcReport.h"

namespace {

using namespace videoeye;

// AVColorPrimaries / AVColorTransferCharacteristic / AVColorSpace / AVColorRange 数值
constexpr int kPriBt709 = 1;
constexpr int kPriUnspecified = 2;
constexpr int kPriBt2020 = 9;
constexpr int kTrcBt709 = 1;
constexpr int kTrcUnspecified = 2;
constexpr int kTrcPq = 16;
constexpr int kTrcHlg = 18;
constexpr int kSpcBt709 = 1;
constexpr int kSpcBt2020Ncl = 9;
constexpr int kRangeUnspecified = 0;
constexpr int kRangeLimited = 1;
constexpr int kRangeFull = 2;

// 按 FFmpeg 原生枚举值填一份 ColorInfo（与 ColorHdrAnalyzer::UpdateFromStream 同一条路径）
model::ColorInfo MakeColor(int primaries, int transfer, int matrix, int range,
                           int bit_depth, const std::string& pix_fmt = "yuv420p",
                           const std::string& subsampling = "4:2:0") {
    model::ColorInfo color;
    color.analyzed = true;
    color.stream_index = 0;
    color.codec_name = "hevc";
    color.profile_name = "Main 10";

    color.primaries_raw = primaries;
    color.transfer_raw = transfer;
    color.matrix_raw = matrix;
    color.range_raw = range;

    color.primaries = model::ClassifyPrimariesRaw(primaries);
    color.transfer = model::ClassifyTransferRaw(transfer);
    color.matrix = model::ClassifyMatrixRaw(matrix);
    color.range = model::ClassifyRangeRaw(range);

    color.primaries_name = model::PrimariesDisplayName(primaries);
    color.transfer_name = model::TransferDisplayName(transfer);
    color.matrix_name = model::MatrixDisplayName(matrix);
    color.range_name = model::RangeDisplayName(range);

    color.pixel_format.name = pix_fmt;
    color.pixel_format.valid = true;
    color.pixel_format.bit_depth = bit_depth;
    color.pixel_format.chroma_subsampling = subsampling;
    return color;
}

model::MasteringDisplayMetadata MakeMastering() {
    model::MasteringDisplayMetadata md;
    md.present = true;
    md.has_primaries = true;
    md.has_luminance = true;
    md.red_x = 0.68;   md.red_y = 0.32;
    md.green_x = 0.265; md.green_y = 0.69;
    md.blue_x = 0.15;  md.blue_y = 0.06;
    md.white_x = 0.3127; md.white_y = 0.3290;
    md.max_luminance = 1000.0;
    md.min_luminance = 0.005;
    return md;
}

analyzer::ColorHdrAnalysis MakeAnalysis(const model::ColorInfo& color,
                                        const model::HdrMetadataInfo& hdr) {
    analyzer::ColorHdrAnalysis analysis;
    analysis.analyzed = true;
    analysis.stream_index = color.stream_index;
    analysis.color = color;
    analysis.hdr = hdr;
    analysis.hdr.format = model::ClassifyHdrFormat(analysis.color, analysis.hdr);
    analysis.hdr.analyzed = true;
    return analysis;
}

analyzer::AnalysisResult MakeResult(const analyzer::ColorHdrAnalysis& analysis) {
    analyzer::AnalysisResult result;
    result.file_path = "test.mp4";
    result.container_format = "mov,mp4,m4a,3gp,3g2,mj2";
    result.duration_seconds = 10.0;

    analyzer::StreamDigest video;
    video.index = 0;
    video.media_type = 0;   // AVMEDIA_TYPE_VIDEO
    video.codec_name = "hevc";
    video.width = 1920;
    video.height = 1080;
    result.streams.push_back(video);

    result.color_hdr = analysis;
    return result;
}

bool HasIssue(const model::QcReport& report, const std::string& rule_id,
              model::DiagnosticIssue* out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id) continue;
        if (out) *out = issue;
        return true;
    }
    return false;
}

int CountColorIssues(const model::QcReport& report) {
    int count = 0;
    for (const auto& issue : report.issues) {
        if (issue.category == model::IssueCategory::ColorHdr) ++count;
    }
    return count;
}

}  // namespace

// ---------------- 1) SDR Rec.709 ----------------

TEST(ColorHdrTest, SdrBt709ClassifiesTo709AcrossPrimariesTransferMatrix) {
    const model::ColorInfo color =
        MakeColor(kPriBt709, kTrcBt709, kSpcBt709, kRangeLimited, 8, "yuv420p");

    EXPECT_EQ(color.primaries, model::ColorPrimariesKind::Bt709);
    EXPECT_EQ(color.transfer, model::TransferKind::Sdr);
    EXPECT_EQ(color.matrix, model::MatrixKind::Bt709);
    EXPECT_EQ(color.range, model::ColorRangeKind::Limited);
    EXPECT_EQ(color.primaries_name, "BT.709");
    EXPECT_EQ(color.transfer_name, "BT.709");
    EXPECT_EQ(color.matrix_name, "BT.709");
    EXPECT_EQ(color.range_name, "Limited");
    EXPECT_EQ(color.EffectiveBitDepth(), 8);
    EXPECT_FALSE(color.IsHdrTransfer());

    model::HdrMetadataInfo hdr;
    EXPECT_EQ(model::ClassifyHdrFormat(color, hdr), model::HdrFormat::Sdr);

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));
    // 一套标准 SDR 组合不应产生任何色彩/HDR 告警
    EXPECT_EQ(CountColorIssues(report), 0);
}

// ---------------- 2) HDR10 ----------------

TEST(ColorHdrTest, Hdr10SampleIsRecognizedWithoutWarnings) {
    const model::ColorInfo color =
        MakeColor(kPriBt2020, kTrcPq, kSpcBt2020Ncl, kRangeLimited, 10, "yuv420p10le");

    EXPECT_EQ(color.primaries, model::ColorPrimariesKind::Bt2020);
    EXPECT_EQ(color.transfer, model::TransferKind::Pq);
    EXPECT_EQ(color.matrix, model::MatrixKind::Bt2020Ncl);
    EXPECT_EQ(color.EffectiveBitDepth(), 10);
    EXPECT_TRUE(color.IsHdrTransfer());

    model::HdrMetadataInfo hdr;
    hdr.mastering_display = MakeMastering();
    hdr.content_light.present = true;
    hdr.content_light.max_cll = 1000;
    hdr.content_light.max_fall = 400;

    ASSERT_EQ(model::ClassifyHdrFormat(color, hdr), model::HdrFormat::Hdr10);

    const auto analysis = MakeAnalysis(color, hdr);
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(analysis));

    EXPECT_FALSE(HasIssue(report, "video.color.hdr_missing_mastering"));
    EXPECT_FALSE(HasIssue(report, "video.color.hdr_missing_light_level"));
    EXPECT_FALSE(HasIssue(report, "video.color.hdr_low_bitdepth"));
    EXPECT_FALSE(HasIssue(report, "video.color.wide_gamut_sdr_transfer"));
    EXPECT_FALSE(HasIssue(report, "video.color.matrix_mismatch"));
    EXPECT_EQ(CountColorIssues(report), 0);

    // MaxCLL / MaxFALL 要出现在导出的 HDR 表格里
    const auto rows = analyzer::BuildHdrRows(analysis);
    bool has_maxcll = false;
    bool has_maxfall = false;
    for (const auto& row : rows) {
        if (row.key == "MaxCLL") {
            has_maxcll = true;
            EXPECT_EQ(row.value, "1000 cd/m²");
        }
        if (row.key == "MaxFALL") {
            has_maxfall = true;
            EXPECT_EQ(row.value, "400 cd/m²");
        }
    }
    EXPECT_TRUE(has_maxcll);
    EXPECT_TRUE(has_maxfall);
}

TEST(ColorHdrTest, HlgDoesNotRequireStaticMetadata) {
    const model::ColorInfo color =
        MakeColor(kPriBt2020, kTrcHlg, kSpcBt2020Ncl, kRangeLimited, 10, "yuv420p10le");
    model::HdrMetadataInfo hdr;   // HLG 本就不需要 MaxCLL/MaxFALL

    ASSERT_EQ(model::ClassifyHdrFormat(color, hdr), model::HdrFormat::Hlg);

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));
    EXPECT_FALSE(HasIssue(report, "video.color.hdr_missing_mastering"));
    EXPECT_FALSE(HasIssue(report, "video.color.hdr_missing_light_level"));
}

// ---------------- 3) 缺元数据的 HDR ----------------

TEST(ColorHdrTest, PqWithoutMetadataWarns) {
    const model::ColorInfo color =
        MakeColor(kPriBt2020, kTrcPq, kSpcBt2020Ncl, kRangeLimited, 10, "yuv420p10le");
    const model::HdrMetadataInfo hdr;   // 什么都没写

    ASSERT_EQ(model::ClassifyHdrFormat(color, hdr), model::HdrFormat::Hdr10Basic);

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));

    model::DiagnosticIssue mastering;
    ASSERT_TRUE(HasIssue(report, "video.color.hdr_missing_mastering", &mastering));
    EXPECT_EQ(mastering.severity, model::IssueSeverity::Warning);
    EXPECT_EQ(mastering.category, model::IssueCategory::ColorHdr);

    model::DiagnosticIssue light;
    ASSERT_TRUE(HasIssue(report, "video.color.hdr_missing_light_level", &light));
    EXPECT_EQ(light.severity, model::IssueSeverity::Warning);
}

TEST(ColorHdrTest, EightBitPqIsAnError) {
    const model::ColorInfo color =
        MakeColor(kPriBt2020, kTrcPq, kSpcBt2020Ncl, kRangeLimited, 8, "yuv420p");
    model::HdrMetadataInfo hdr;
    hdr.mastering_display = MakeMastering();
    hdr.content_light.present = true;
    hdr.content_light.max_cll = 1000;
    hdr.content_light.max_fall = 400;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "video.color.hdr_low_bitdepth", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Error);
    EXPECT_EQ(issue.metric_value, 8.0);
}

TEST(ColorHdrTest, WideGamutWithSdrTransferWarns) {
    // BT.2020 原色 + BT.709 传递函数：SDR BT.2020 或误标的 HDR，需要核查
    const model::ColorInfo color =
        MakeColor(kPriBt2020, kTrcBt709, kSpcBt2020Ncl, kRangeLimited, 10, "yuv420p10le");
    model::HdrMetadataInfo hdr;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "video.color.wide_gamut_sdr_transfer", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
}

TEST(ColorHdrTest, MatrixMismatchWarns) {
    // BT.2020 原色配 BT.709 矩阵
    const model::ColorInfo color =
        MakeColor(kPriBt2020, kTrcPq, kSpcBt709, kRangeLimited, 10, "yuv420p10le");
    model::HdrMetadataInfo hdr;
    hdr.mastering_display = MakeMastering();
    hdr.content_light.present = true;
    hdr.content_light.max_cll = 1000;
    hdr.content_light.max_fall = 400;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));
    EXPECT_TRUE(HasIssue(report, "video.color.matrix_mismatch"));
}

TEST(ColorHdrTest, TotallyUnspecifiedMetadataIsReported) {
    const model::ColorInfo color = MakeColor(kPriUnspecified, kTrcUnspecified, kSpcBt709,
                                             kRangeUnspecified, 8, "yuv420p");
    model::HdrMetadataInfo hdr;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "video.color.unspecified", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Info);
    EXPECT_EQ(issue.occurrence_count, 3);   // primaries / transfer / range
}

// ---------------- 4) full range vs limited range ----------------

TEST(ColorHdrTest, FullAndLimitedRangeAreDistinguished) {
    const model::ColorInfo limited =
        MakeColor(kPriBt709, kTrcBt709, kSpcBt709, kRangeLimited, 8, "yuv420p");
    const model::ColorInfo full =
        MakeColor(kPriBt709, kTrcBt709, kSpcBt709, kRangeFull, 8, "yuv420p");

    EXPECT_EQ(limited.range, model::ColorRangeKind::Limited);
    EXPECT_EQ(full.range, model::ColorRangeKind::Full);
    EXPECT_NE(limited.range_name, full.range_name);
    EXPECT_EQ(limited.range_name, "Limited");
    EXPECT_EQ(full.range_name, "Full");
    EXPECT_FALSE(limited.IsFullRange());
    EXPECT_TRUE(full.IsFullRange());

    // 两种都不应触发 range 冲突（像素格式没有隐含范围）
    model::HdrMetadataInfo hdr;
    analyzer::QcRuleEngine engine;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(MakeAnalysis(limited, hdr))),
                          "video.color.range_conflict"));
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(MakeAnalysis(full, hdr))),
                          "video.color.range_conflict"));
}

TEST(ColorHdrTest, YuvjPixelFormatImpliesFullRange) {
    model::PixelFormatInfo pf;
    pf.name = "yuvj420p";
    EXPECT_TRUE(pf.ImpliesFullRange());
    pf.name = "yuv420p";
    EXPECT_FALSE(pf.ImpliesFullRange());
}

TEST(ColorHdrTest, YuvjMarkedAsLimitedIsAConflict) {
    // yuvj420p 隐含 full range，容器却标了 Limited
    const model::ColorInfo color =
        MakeColor(kPriBt709, kTrcBt709, kSpcBt709, kRangeLimited, 8, "yuvj420p");
    model::HdrMetadataInfo hdr;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "video.color.range_conflict", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
}

TEST(ColorHdrTest, RgbMarkedAsLimitedIsAConflict) {
    model::ColorInfo color = MakeColor(kPriBt709, kTrcBt709, kSpcBt709, kRangeLimited, 8,
                                       "rgb24", "");
    color.pixel_format.rgb = true;
    model::HdrMetadataInfo hdr;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));
    EXPECT_TRUE(HasIssue(report, "video.color.range_conflict"));
}

// ---------------- Dolby Vision ----------------

TEST(ColorHdrTest, DolbyVisionWithoutCompatibilityLayerIsReported) {
    const model::ColorInfo color =
        MakeColor(kPriBt2020, kTrcPq, kSpcBt2020Ncl, kRangeLimited, 10, "yuv420p10le");
    model::HdrMetadataInfo hdr;
    hdr.dolby_vision.present = true;
    hdr.dolby_vision.profile = 5;
    hdr.dolby_vision.level = 6;
    hdr.dolby_vision.rpu_present = true;
    hdr.dolby_vision.compatibility_id = 0;   // Profile 5：无 HDR10/SDR 兼容层

    ASSERT_EQ(model::ClassifyHdrFormat(color, hdr), model::HdrFormat::DolbyVision);

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(MakeAnalysis(color, hdr)));

    EXPECT_TRUE(HasIssue(report, "video.color.dv_no_compatibility"));
    // 有 DV 配置记录时不再要求 MaxCLL/MaxFALL（DV 由 RPU 提供动态元数据）
    EXPECT_FALSE(HasIssue(report, "video.color.hdr_missing_mastering"));
}

// 未分析过色彩/HDR 时不得误报
TEST(ColorHdrTest, UnanalyzedResultProducesNoColorIssues) {
    analyzer::AnalysisResult result;
    result.file_path = "unknown.bin";
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_EQ(CountColorIssues(report), 0);
}
