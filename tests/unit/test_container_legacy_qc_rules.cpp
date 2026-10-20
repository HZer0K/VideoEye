// AVI / FLV / ASF / OGG 容器级 QC 规则（container.avi.* / flv.* / asf.* / ogg.*）单元测试
//
// 与分析器测试的分工：
//   test_container_structure_analyzers.cpp 测「解析器算出来的数字对不对」；
//   这一份测「规则引擎有没有把数字转成报告」。
//
// 样本一律手工构造 AnalysisResult::container_diagnostics —— 规则只认这份摘要，
// 不读文件，因此这里不需要任何真实媒体资产。

#include <gtest/gtest.h>

#include <string>

#include "core/analysis/diagnostics/QcRuleEngine.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"

namespace {

using namespace videoeye;

bool HasIssue(const model::QcReport& report, const std::string& rule_id) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id == rule_id) return true;
    }
    return false;
}

bool HasAnyLegacyIssue(const model::QcReport& report) {
    static const char* kPrefixes[] = {"container.avi.", "container.flv.",
                                      "container.asf.", "container.ogg."};
    for (const auto& issue : report.issues) {
        for (const char* prefix : kPrefixes) {
            if (issue.rule_id.rfind(prefix, 0) == 0) return true;
        }
    }
    return false;
}

model::AnalysisResult MakeResult(const std::string& format, const std::string& ext) {
    model::AnalysisResult result;
    result.file_path = "clip." + ext;
    result.file_extension = ext;
    result.container_format = format;
    result.duration_seconds = 60.0;
    return result;
}

// 一份"什么都没坏"的 AVI：两个流 / idx1 + indx + dmlh 齐全 / 无坏偏移。
model::AviDiagnostics HealthyAvi() {
    model::AviDiagnostics avi;
    avi.analyzed = true;
    avi.has_idx1 = true;
    avi.has_indx = true;
    avi.has_dmlh = true;
    avi.idx1_entries = 1000;
    avi.video_streams = 1;
    avi.audio_streams = 1;
    return avi;
}

model::FlvDiagnostics HealthyFlv() {
    model::FlvDiagnostics flv;
    flv.analyzed = true;
    flv.header_has_video = true;
    flv.header_has_audio = true;
    flv.has_metadata = true;
    flv.video_tags = 100;
    flv.audio_tags = 100;
    flv.video_codec_avc = true;
    flv.video_sequence_header_seen = true;
    return flv;
}

model::AsfDiagnostics HealthyAsf() {
    model::AsfDiagnostics asf;
    asf.analyzed = true;
    asf.has_data_object = true;
    asf.duration_seconds = 60.0;
    return asf;
}

model::OggDiagnostics HealthyOgg() {
    model::OggDiagnostics ogg;
    ogg.analyzed = true;
    ogg.pages = 100;
    ogg.logical_streams = 2;
    return ogg;
}

}  // namespace

// ---------------- AVI ----------------

TEST(LegacyContainerQcRulesTest, HealthyAviProducesNoIssues) {
    model::AnalysisResult result = MakeResult("avi", "avi");
    result.container_diagnostics.avi = HealthyAvi();
    EXPECT_FALSE(HasAnyLegacyIssue(QcRuleEngine().Evaluate(result)));
}

TEST(LegacyContainerQcRulesTest, AviReportsStructuralDefects) {
    model::AnalysisResult result = MakeResult("avi", "avi");
    model::AviDiagnostics avi = HealthyAvi();
    avi.riff_size_exceeds_file = true;
    avi.idx1_bad_offsets = 3;
    avi.indx_truncated = true;
    avi.truncated_chunks = 2;
    avi.zero_length_streams = 1;
    avi.has_avix = true;
    avi.has_dmlh = false;
    result.container_diagnostics.avi = avi;

    const model::QcReport report = QcRuleEngine().Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "container.avi.riff_size_exceeds_file"));
    EXPECT_TRUE(HasIssue(report, "container.avi.index_bad_offsets"));
    EXPECT_TRUE(HasIssue(report, "container.avi.indx_truncated"));
    EXPECT_TRUE(HasIssue(report, "container.avi.truncated_chunks"));
    EXPECT_TRUE(HasIssue(report, "container.avi.zero_length_streams"));
    EXPECT_TRUE(HasIssue(report, "container.avi.no_opendml"));
}

TEST(LegacyContainerQcRulesTest, AviMissingIndexOnlyWhenMediaStreamsExist) {
    {
        model::AnalysisResult result = MakeResult("avi", "avi");
        model::AviDiagnostics avi = HealthyAvi();
        avi.has_idx1 = false;
        avi.has_indx = false;
        result.container_diagnostics.avi = avi;
        EXPECT_TRUE(HasIssue(QcRuleEngine().Evaluate(result), "container.avi.missing_index"));
    }
    {
        // 空 AVI（没有音视频流）：没有索引不算问题
        model::AnalysisResult result = MakeResult("avi", "avi");
        model::AviDiagnostics avi = HealthyAvi();
        avi.has_idx1 = false;
        avi.has_indx = false;
        avi.video_streams = 0;
        avi.audio_streams = 0;
        result.container_diagnostics.avi = avi;
        EXPECT_FALSE(HasIssue(QcRuleEngine().Evaluate(result), "container.avi.missing_index"));
    }
}

TEST(LegacyContainerQcRulesTest, UnanalyzedAviProducesNoIssues) {
    model::AnalysisResult result = MakeResult("avi", "avi");
    result.container_diagnostics.avi = model::AviDiagnostics{};
    EXPECT_FALSE(HasAnyLegacyIssue(QcRuleEngine().Evaluate(result)));
}

// ---------------- FLV ----------------

TEST(LegacyContainerQcRulesTest, HealthyFlvProducesNoIssues) {
    model::AnalysisResult result = MakeResult("flv", "flv");
    result.container_diagnostics.flv = HealthyFlv();
    EXPECT_FALSE(HasAnyLegacyIssue(QcRuleEngine().Evaluate(result)));
}

TEST(LegacyContainerQcRulesTest, FlvReportsMetadataAndSequenceHeaderProblems) {
    model::AnalysisResult result = MakeResult("flv", "flv");
    model::FlvDiagnostics flv = HealthyFlv();
    flv.has_metadata = false;
    flv.video_sequence_header_seen = false;
    result.container_diagnostics.flv = flv;

    const model::QcReport report = QcRuleEngine().Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "container.flv.missing_metadata"));
    EXPECT_TRUE(HasIssue(report, "container.flv.missing_avc_sequence_header"));
}

TEST(LegacyContainerQcRulesTest, FlvReportsTimestampAndTruncation) {
    model::AnalysisResult result = MakeResult("flv", "flv");
    model::FlvDiagnostics flv = HealthyFlv();
    flv.timestamp_backwards = 4;
    flv.truncated_tags = 1;
    flv.tag_size_mismatch = 2;
    result.container_diagnostics.flv = flv;

    const model::QcReport report = QcRuleEngine().Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "container.flv.timestamp_backwards"));
    EXPECT_TRUE(HasIssue(report, "container.flv.truncated_tags"));
    EXPECT_TRUE(HasIssue(report, "container.flv.tag_size_mismatch"));
}

TEST(LegacyContainerQcRulesTest, FlvPartialScanSuppressesFileWideConclusions) {
    model::AnalysisResult result = MakeResult("flv", "flv");
    model::FlvDiagnostics flv = HealthyFlv();
    flv.scan_complete = false;
    flv.scanned_tags = 500;
    flv.has_metadata = false;
    flv.video_sequence_header_seen = false;
    result.container_diagnostics.flv = flv;

    // 只扫了前 500 个 tag 时，"没有元数据 / 没有序列头"都不能下结论
    const model::QcReport report = QcRuleEngine().Evaluate(result);
    EXPECT_FALSE(HasIssue(report, "container.flv.missing_metadata"));
    EXPECT_FALSE(HasIssue(report, "container.flv.missing_avc_sequence_header"));
}

// ---------------- ASF ----------------

TEST(LegacyContainerQcRulesTest, HealthyAsfProducesNoIssues) {
    model::AnalysisResult result = MakeResult("asf", "wmv");
    result.container_diagnostics.asf = HealthyAsf();
    EXPECT_FALSE(HasAnyLegacyIssue(QcRuleEngine().Evaluate(result)));
}

TEST(LegacyContainerQcRulesTest, AsfReportsObjectAndDurationProblems) {
    model::AnalysisResult result = MakeResult("asf", "wmv");
    model::AsfDiagnostics asf;
    asf.analyzed = true;
    asf.has_data_object = false;
    asf.invalid_object_sizes = 2;
    asf.header_size_exceeds_file = true;
    asf.duration_invalid = true;
    asf.duration_seconds = 0.0;
    result.container_diagnostics.asf = asf;

    const model::QcReport report = QcRuleEngine().Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "container.asf.missing_data_object"));
    EXPECT_TRUE(HasIssue(report, "container.asf.header_object_size_invalid"));
    EXPECT_TRUE(HasIssue(report, "container.asf.header_size_exceeds_file"));
    EXPECT_TRUE(HasIssue(report, "container.asf.duration_invalid"));
}

// ---------------- OGG ----------------

TEST(LegacyContainerQcRulesTest, HealthyOggProducesNoIssues) {
    model::AnalysisResult result = MakeResult("ogg", "ogg");
    result.container_diagnostics.ogg = HealthyOgg();
    EXPECT_FALSE(HasAnyLegacyIssue(QcRuleEngine().Evaluate(result)));
}

TEST(LegacyContainerQcRulesTest, OggReportsPageLevelProblems) {
    model::AnalysisResult result = MakeResult("ogg", "ogg");
    model::OggDiagnostics ogg = HealthyOgg();
    ogg.sequence_gaps = 2;
    ogg.missing_bos = 1;
    ogg.missing_eos = 1;
    ogg.truncated_pages = 1;
    ogg.resyncs = 3;
    result.container_diagnostics.ogg = ogg;

    const model::QcReport report = QcRuleEngine().Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "container.ogg.page_sequence_gap"));
    EXPECT_TRUE(HasIssue(report, "container.ogg.missing_bos"));
    EXPECT_TRUE(HasIssue(report, "container.ogg.missing_eos"));
    EXPECT_TRUE(HasIssue(report, "container.ogg.truncated_pages"));
    EXPECT_TRUE(HasIssue(report, "container.ogg.resyncs"));
}

TEST(LegacyContainerQcRulesTest, OggPartialScanDoesNotClaimMissingEos) {
    model::AnalysisResult result = MakeResult("ogg", "ogg");
    model::OggDiagnostics ogg = HealthyOgg();
    ogg.scan_complete = false;
    ogg.missing_eos = 1;
    result.container_diagnostics.ogg = ogg;

    EXPECT_FALSE(HasIssue(QcRuleEngine().Evaluate(result), "container.ogg.missing_eos"));
}
