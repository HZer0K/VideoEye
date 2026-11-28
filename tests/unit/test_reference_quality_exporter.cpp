// 参考帧质量对比的导出层（core/reporting/ReferenceQualityExporter）。
//
// 为什么值得单测: 这一层只做"把已经算好的结果写成文件"，但它踩的坑都是**静默**
// 的 —— 写坏了不崩、不报，只是：
//   1) 少写 BOM：Excel 双击打开，中文列乱码（只有真导出一次才看得见）；
//   2) NaN 写 0：报告里出现一个看起来合法的"0 dB"，而这帧其实根本没对上；
//   3) 无损帧的 +inf 丢掉：CSV 里写成空、JSON 里只能写 null，"完全一致"与
//      "没算出来"变成同一格；
//   4) 元数据（参数 / 覆盖 / 错误原因）不落盘：同一对文件两次导出结果不同时，
//      没人能从报告里还原当时用的参数。
//
// 这里不建 QApplication、不弹对话框，只钉上面四条。

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "core/domain/model/ReferenceQualityResult.h"
#include "core/reporting/ReferenceQualityExporter.h"
#include "infrastructure/serialization/Json.h"

using videoeye::JsonParse;
using videoeye::JsonValue;
using videoeye::model::kQualityNoValue;
using videoeye::model::ReferenceFrameMetric;
using videoeye::model::ReferenceQualityResult;
using videoeye::model::ReferenceQualitySummary;
using videoeye::reporting::ReferenceQualityExporter;
using videoeye::reporting::ReferenceQualityExportInput;

namespace {

struct ScopedDir {
    explicit ScopedDir(const std::string& tag)
        : path(std::filesystem::temp_directory_path() /
               ("videoeye_refcmp_" + tag + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }
    ~ScopedDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string File(const std::string& name) const {
        return (path / name).string();
    }
    std::filesystem::path path;
};

std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// 一份"混合"结果: 第 0 帧正常、第 1 帧无损（PSNR = +inf）、第 2 帧尺寸不一致（NaN + 错误文本）。
ReferenceQualityResult MakeMixedResult() {
    ReferenceQualityResult result;
    result.valid = true;
    result.options.start_seconds = 0.0;
    result.options.end_seconds = -1.0;
    result.options.offset_seconds = 0.5;
    result.options.alignment_tolerance_seconds = 0.042;
    result.options.max_frames = 0;

    ReferenceQualitySummary& summary = result.summary;
    summary.reference_duration_seconds = 2.0;
    summary.distorted_duration_seconds = 2.5;
    summary.reference_sample_count = 50;
    summary.distorted_sample_count = 60;
    summary.target_count = 3;
    summary.compared_frames = 2;
    summary.failed_frames = 1;
    summary.skipped_frames = 0;
    summary.lossless_frames = 1;
    summary.coverage = 2.0 / 3.0;
    summary.mean_psnr_db = 40.0; // 只统计有限值: 无损帧不参与平均
    summary.mean_ssim = 0.995;
    summary.min_psnr_db = 40.0;
    summary.min_ssim = 0.99;
    summary.min_psnr_frame_index = 0;
    summary.min_ssim_frame_index = 0;
    summary.max_alignment_delta_seconds = 0.012;
    summary.truncated = false;
    summary.duration_mismatch = true;

    ReferenceFrameMetric first;
    first.index = 0;
    first.timestamp_seconds = 0.0;
    first.reference_frame_index = 3;
    first.distorted_frame_index = 4;
    first.reference_timestamp_seconds = 0.12;
    first.distorted_timestamp_seconds = 0.62;
    first.alignment_delta_seconds = 0.0;
    first.width = 320;
    first.height = 180;
    first.psnr_db = 40.0;
    first.ssim = 0.99;
    first.valid = true;
    result.frames.push_back(first);

    ReferenceFrameMetric lossless = first;
    lossless.index = 1;
    lossless.timestamp_seconds = 0.5;
    lossless.reference_timestamp_seconds = 0.52;
    lossless.distorted_timestamp_seconds = 1.02;
    lossless.psnr_db = std::numeric_limits<double>::infinity();
    lossless.ssim = 1.0;
    result.frames.push_back(lossless);

    ReferenceFrameMetric failed = first;
    failed.index = 2;
    failed.timestamp_seconds = 1.0;
    failed.reference_timestamp_seconds = 1.02;
    failed.distorted_timestamp_seconds = 1.52;
    failed.psnr_db = kQualityNoValue;
    failed.ssim = kQualityNoValue;
    failed.valid = false;
    failed.error_message = "尺寸不一致: 320x180 vs 320x240\n（不拉伸）";
    result.frames.push_back(failed);
    return result;
}

std::vector<std::string> DataLines(const std::string& content) {
    // 首行带着 UTF-8 BOM（EF BB BF），直接按 '#' 判断会把元数据首行误算成数据行。
    std::string body = content;
    if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0)
        body.erase(0, 3);
    std::vector<std::string> lines;
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        lines.push_back(line);
    }
    return lines;
}

const JsonValue* Member(const JsonValue& object, const char* key) {
    return object.Find(key);
}

} // namespace

TEST(ReferenceQualityExporterTest, CsvKeepsBomMetadataAndPerFrameRows) {
    ScopedDir dir("csv");
    const std::string path = dir.File("cmp.csv");
    const ReferenceQualityExportInput input{"/media/ref.mp4", "/media/test.mp4"};
    ASSERT_TRUE(ReferenceQualityExporter::ExportCsv(path, MakeMixedResult(), input));

    const std::string content = ReadWholeFile(path);
    ASSERT_GE(content.size(), 3u);
    EXPECT_EQ(content.substr(0, 3), std::string("\xEF\xBB\xBF", 3)) << "缺 UTF-8 BOM，Excel 打开中文列会乱码";

    // 元数据：#报告（含两个输入与参数）、#统计（覆盖率）、#指标
    EXPECT_NE(content.find("#报告"), std::string::npos);
    EXPECT_NE(content.find("/media/ref.mp4"), std::string::npos);
    EXPECT_NE(content.find("#统计"), std::string::npos);
    EXPECT_NE(content.find("#指标"), std::string::npos);

    const std::vector<std::string> lines = DataLines(content);
    ASSERT_EQ(lines.size(), 4u) << "应为 1 行表头 + 3 行帧";
    EXPECT_NE(lines[0].find("PSNR(dB)"), std::string::npos);

    // 第 0 帧: 有限值照常落盘
    EXPECT_NE(lines[1].find("40.000"), std::string::npos);

    // 第 1 帧: 无损 -> inf（不是 0、也不是空）
    const std::string& lossless = lines[2];
    EXPECT_NE(lossless.find(",inf,"), std::string::npos) << "无损帧的 PSNR 必须写成 inf: " << lossless;

    // 第 2 帧: NaN -> 空单元；错误文本进引号且换行被压平（否则一帧拆成两行）
    const std::string& failed = lines[3];
    EXPECT_NE(failed.find("尺寸不一致: 320x180 vs 320x240 （不拉伸）"), std::string::npos)
        << "错误文本应换行压平后保留: " << failed;
    // PSNR / SSIM 两列为空 -> 出现连续的 ",,"（中间没有数字）
    EXPECT_NE(failed.find(",否,"), std::string::npos) << "失败帧的有效列应为「否」: " << failed;
}

TEST(ReferenceQualityExporterTest, CsvDoesNotWriteZeroForMissingMetrics) {
    ScopedDir dir("csv_nan");
    const std::string path = dir.File("cmp.csv");
    ReferenceQualityResult result = MakeMixedResult();
    result.frames.resize(1);
    result.frames.front().psnr_db = kQualityNoValue;
    result.frames.front().ssim = kQualityNoValue;
    result.frames.front().valid = false;
    ASSERT_TRUE(ReferenceQualityExporter::ExportCsv(path, result));

    const std::vector<std::string> lines = DataLines(ReadWholeFile(path));
    ASSERT_EQ(lines.size(), 2u);
    // 列序: ...,宽,高,PSNR,SSIM,有效,错误 -> 宽高之后必须紧跟两个空单元。
    // 出现数字（哪怕是 0.000）都会被读成"测出来是 0 dB"。
    EXPECT_NE(lines[1].find(",320,180,,,"), std::string::npos)
        << "没算出来的 PSNR / SSIM 必须留空，不能写 0: " << lines[1];
}

TEST(ReferenceQualityExporterTest, JsonRoundTripsOptionsSummaryAndFrames) {
    ScopedDir dir("json");
    const std::string path = dir.File("cmp.json");
    const ReferenceQualityExportInput input{"/media/ref.mp4", "/media/test.mp4"};
    ASSERT_TRUE(ReferenceQualityExporter::ExportJson(path, MakeMixedResult(), input));

    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadWholeFile(path), root, &error)) << error;
    ASSERT_TRUE(root.IsObject());

    ASSERT_NE(Member(root, "reference_file"), nullptr);
    EXPECT_EQ(Member(root, "reference_file")->StringValueOr(), "/media/ref.mp4");
    EXPECT_TRUE(Member(root, "valid")->BoolValueOr(false));

    const JsonValue* options = Member(root, "options");
    ASSERT_NE(options, nullptr);
    EXPECT_DOUBLE_EQ(options->Find("offset_seconds")->NumberValueOr(0.0), 0.5);
    EXPECT_DOUBLE_EQ(options->Find("alignment_tolerance_seconds")->NumberValueOr(0.0), 0.042);

    const JsonValue* summary = Member(root, "summary");
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->Find("compared_frames")->IntValueOr(-1), 2);
    EXPECT_EQ(summary->Find("lossless_frames")->IntValueOr(-1), 1);
    EXPECT_EQ(summary->Find("failed_frames")->IntValueOr(-1), 1);
    EXPECT_TRUE(summary->Find("duration_mismatch")->BoolValueOr(false));
    EXPECT_FALSE(summary->Find("all_lossless")->BoolValueOr(true));

    const JsonValue* frames = Member(root, "frames");
    ASSERT_NE(frames, nullptr);
    ASSERT_TRUE(frames->IsArray());
    ASSERT_EQ(frames->Size(), 3u);

    // 有限值 -> 数字；无损 -> psnr_db 为 null 但 lossless=true；算不出来 -> 两个都是 null
    const JsonValue& first = frames->Items()[0];
    EXPECT_DOUBLE_EQ(first.Find("psnr_db")->NumberValueOr(0.0), 40.0);
    EXPECT_FALSE(first.Find("lossless")->BoolValueOr(true));

    const JsonValue& lossless = frames->Items()[1];
    EXPECT_TRUE(lossless.Find("psnr_db")->IsNull()) << "JSON 没有 Infinity，只能落 null";
    EXPECT_TRUE(lossless.Find("lossless")->BoolValueOr(false)) << "必须用 lossless 区分「无损」与「没算出来」";

    const JsonValue& failed = frames->Items()[2];
    EXPECT_TRUE(failed.Find("psnr_db")->IsNull());
    EXPECT_FALSE(failed.Find("valid")->BoolValueOr(true));
    EXPECT_EQ(failed.Find("error_message")->StringValueOr(), "尺寸不一致: 320x180 vs 320x240\n（不拉伸）")
        << "JSON 不需要压平换行，原文应完整保留";
}

TEST(ReferenceQualityExporterTest, JsonMarksAllLosslessWhenEveryFrameIsIdentical) {
    ScopedDir dir("json_lossless");
    const std::string path = dir.File("cmp.json");
    ReferenceQualityResult result = MakeMixedResult();
    for (ReferenceFrameMetric& frame : result.frames) {
        frame.valid = true;
        frame.psnr_db = std::numeric_limits<double>::infinity();
        frame.ssim = 1.0;
        frame.error_message.clear();
    }
    result.summary.compared_frames = 3;
    result.summary.lossless_frames = 3;
    result.summary.failed_frames = 0;
    result.summary.mean_psnr_db = std::numeric_limits<double>::infinity();

    ASSERT_TRUE(ReferenceQualityExporter::ExportJson(path, result));
    JsonValue root;
    ASSERT_TRUE(JsonParse(ReadWholeFile(path), root));
    const JsonValue* summary = Member(root, "summary");
    ASSERT_NE(summary, nullptr);
    // 均值是 +inf -> null；all_lossless 必须补上这个语义
    EXPECT_TRUE(summary->Find("mean_psnr_db")->IsNull());
    EXPECT_TRUE(summary->Find("all_lossless")->BoolValueOr(false));
}

TEST(ReferenceQualityExporterTest, FailedRunStillRecordsItsReason) {
    ScopedDir dir("failed");
    ReferenceQualityResult result;
    result.valid = false;
    result.error_message = "参考视频没有可解码的视频帧";
    const std::string csv_path = dir.File("failed.csv");
    const std::string json_path = dir.File("failed.json");
    ASSERT_TRUE(ReferenceQualityExporter::ExportCsv(csv_path, result));
    ASSERT_TRUE(ReferenceQualityExporter::ExportJson(json_path, result));

    const std::vector<std::string> lines = DataLines(ReadWholeFile(csv_path));
    ASSERT_EQ(lines.size(), 1u) << "失败时只有表头，没有数据行";
    EXPECT_NE(ReadWholeFile(csv_path).find("参考视频没有可解码的视频帧"), std::string::npos)
        << "失败原因必须落进元数据，否则空表等于「一切正常」";

    JsonValue root;
    ASSERT_TRUE(JsonParse(ReadWholeFile(json_path), root));
    EXPECT_EQ(Member(root, "error_message")->StringValueOr(), "参考视频没有可解码的视频帧");
    EXPECT_EQ(Member(root, "frames")->Size(), 0u);
}

TEST(ReferenceQualityExporterTest, ReturnsFalseWhenTargetCannotBeOpened) {
    ScopedDir dir("bad_path");
    const std::string path = dir.File("no_such_subdir/out.csv");
    ReferenceQualityResult result = MakeMixedResult();
    EXPECT_FALSE(ReferenceQualityExporter::ExportCsv(path, result))
        << "打不开必须返回 false，静默成功会让用户以为导出成功";
    EXPECT_FALSE(ReferenceQualityExporter::ExportJson(path, result));
    EXPECT_FALSE(std::filesystem::exists(path));
}
