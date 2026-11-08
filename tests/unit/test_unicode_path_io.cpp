#include <gtest/gtest.h>

// 中文（UTF-8）路径在「写入侧」与「目录遍历侧」的守护。
//
// 已有的 test_parser_review_regressions.cpp::UnicodeFiles 只覆盖**读取侧**的几个
// 解析器；写侧（报告落盘、批量扫描发现、修复输入 stat）一直只跑英文临时目录 ——
// 于是 MSVC 的 ANSI 语义在这些路径上无人看守。
//
// 2026-11 用探针在 MSVC（代码页 936）实测，三种失败形态都**不报编码错误**：
//   * exists(utf8 std::string)      → false，且 ec 为空（静默当"文件不存在"）
//   * file_size(utf8 std::string)   → 恒失败，哪怕这个 string 刚由 u8string() 取出
//   * path().string() 中文名        → GBK 字节（下游按 UTF-8 用即打不开）
//   * path().string() emoji 名      → 抛 system_error（遍历目录时抛就是崩）
//
// 对应到用户可见现象：中文目录批量扫描"一个文件都没有"、修复报"输入文件不存在"、
// 产物大小 0、扫描到中文/表情文件名时整个进程退出。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairInspector.h"
#include "core/reporting/QcReportExporter.h"
#include "tests/support/TestMediaFactory.h"
#include "ui/reporting_panel/report_path.h"

namespace fs = std::filesystem;

using videoeye::ToNativePath;
using videoeye::ToUtf8String;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::model::QcReport;
using videoeye::model::RepairRequest;
using videoeye::qc::BatchQcItem;
using videoeye::qc::BatchQcRunner;
using videoeye::qc::QcProfile;
using videoeye::qc::QcReportFormat;
using videoeye::qc::QcRunResult;
using videoeye::repair::RepairInspector;
using videoeye::reporting::QcExportBundle;
using videoeye::reporting::QcReportExporter;

namespace {

constexpr const char* kDirName = u8"中文目录-\u89c6\u9891";
constexpr const char* kVideoName = u8"\u89c6\u9891.mp4";
constexpr const char* kEmojiName = u8"\u89c6\u9891\U0001F600.mp4"; // 当前代码页表示不了
constexpr const char* kSubDirName = u8"子目录";
constexpr const char* kBaseName = u8"报告-\u7ed3\u679c";

std::string ReadText(const std::string& utf8_path) {
    std::ifstream in(ToNativePath(utf8_path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 一份最小但结构完整的 MP4 头（ftyp + moov），供"修复输入身份"用例使用。
std::string MinimalMp4() {
    const unsigned char bytes[] = {0, 0, 0,   20,  'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0,   0,
                                   0, 0, 'i', 's', 'o', 'm', 0,   0,   0,   8,   'm', 'o', 'o', 'v'};
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

class ChinesePathIo : public testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("videoeye_u8io_" + std::to_string(stamp) + "_" + std::to_string(serial.fetch_add(1)));
        work_ = root_ / fs::u8path(kDirName);
        ASSERT_TRUE(fs::create_directories(work_));
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    // 以 UTF-8 名字写文件，返回该文件的 UTF-8 路径。
    std::string WriteFile(const fs::path& dir, const char* name, const std::string& content) {
        const fs::path path = dir / fs::u8path(name);
        std::ofstream out(path, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        EXPECT_TRUE(out.good()) << ToUtf8String(path);
        return ToUtf8String(path);
    }

    fs::path root_;
    fs::path work_;
};

QcReport MakeReport() {
    QcReport report;
    report.file_name = u8"素材.mp4";
    report.file_path = u8"/media/素材.mp4";
    report.container_format = "mp4";
    report.duration_seconds = 12.5;
    report.score = 71.0;
    report.verdict = u8"通过";
    report.generated_at = "2026-11-09 00:30:00";
    report.completed = true;
    DiagnosticIssue issue;
    issue.rule_id = "u8.path.rule";
    issue.title = u8"示例问题";
    issue.category = IssueCategory::Container;
    issue.severity = IssueSeverity::Warning;
    report.issues.push_back(issue);
    return report;
}

QcExportBundle MakeBundle() {
    QcExportBundle bundle;
    bundle.profile_id = "broadcast";
    bundle.profile_name = u8"广播级";
    bundle.run.ok = true;
    bundle.run.report = MakeReport();
    bundle.has_comparison = false;
    return bundle;
}

} // namespace

// ---------------------------------------------------------------------------
// 1) 目录遍历：中文目录 + 中文名 + emoji 名都要被发现，且返回的是合法 UTF-8
//
// 这一条同时钉死两个方向：入口 root 必须转原生路径（否则遍历恒空），
// 出口必须 u8string()（否则中文名变 GBK 乱码、emoji 名抛异常）。
// ---------------------------------------------------------------------------
TEST_F(ChinesePathIo, BatchDiscoverFindsFilesInChineseDirectory) {
    WriteFile(work_, kVideoName, "video");
    WriteFile(work_, kEmojiName, "emoji");
    WriteFile(work_, "plain.mp4", "plain");
    WriteFile(work_, "note.txt", "text");

    const std::string dir = ToUtf8String(work_);
    const std::vector<BatchQcItem> items = BatchQcRunner::Discover(dir, /*recursive=*/false, {"mp4"});
    ASSERT_EQ(items.size(), 3u) << "中文目录下按扩展名过滤后应发现 3 个 mp4";

    bool saw_video = false;
    bool saw_emoji = false;
    for (const BatchQcItem& item : items) {
        // 名字必须是 UTF-8 原样（不是 GBK 字节）：能原样找到子串才算通过。
        if (item.path.find(u8"\u89c6\u9891.mp4") != std::string::npos &&
            item.path.find("\xF0\x9F\x98\x80") == std::string::npos)
            saw_video = true;
        if (item.path.find("\xF0\x9F\x98\x80") != std::string::npos)
            saw_emoji = true;
        EXPECT_TRUE(fs::exists(ToNativePath(item.path))) << item.path;
        EXPECT_GT(item.file_size_bytes, 0) << item.path;
    }
    EXPECT_TRUE(saw_video) << "中文文件名必须以 UTF-8 原样返回";
    EXPECT_TRUE(saw_emoji) << "含 emoji 的文件名不得被跳过（path().string() 会在这里抛异常）";
}

// ---------------------------------------------------------------------------
// 2) 递归发现：子目录里的文件也要找到，且路径保留子目录（顺序无关，只看集合）
// ---------------------------------------------------------------------------
TEST_F(ChinesePathIo, BatchDiscoverRecursesIntoChineseSubdirectory) {
    WriteFile(work_, kVideoName, "video");
    const fs::path sub = work_ / fs::u8path(kSubDirName);
    ASSERT_TRUE(fs::create_directories(sub));
    WriteFile(sub, kVideoName, "video-in-sub");

    const std::string dir = ToUtf8String(work_);
    const auto shallow = BatchQcRunner::Discover(dir, /*recursive=*/false, {"mp4"});
    const auto deep = BatchQcRunner::Discover(dir, /*recursive=*/true, {"mp4"});
    EXPECT_EQ(shallow.size(), 1u);
    ASSERT_EQ(deep.size(), 2u);
    // 恰好一条带子目录（另一条在根下），且两条都是真实存在的 UTF-8 路径。
    size_t in_subdirectory = 0;
    for (const BatchQcItem& item : deep) {
        if (item.path.find(u8"\u5b50\u76ee\u5f55") != std::string::npos)
            ++in_subdirectory;
        EXPECT_TRUE(fs::exists(ToNativePath(item.path))) << item.path;
    }
    EXPECT_EQ(in_subdirectory, 1u) << "递归结果里应恰好一条来自子目录";
}

// ---------------------------------------------------------------------------
// 3) 反向守护：非法 UTF-8 只是"发现不到"，不能抛异常（ToNativePath 的软回退契约）
// ---------------------------------------------------------------------------
TEST_F(ChinesePathIo, BatchDiscoverToleratesInvalidUtf8) {
    EXPECT_NO_THROW({
        const auto items = BatchQcRunner::Discover(std::string("\xFF\xFE\x80 not a path", 18), false, {"mp4"});
        EXPECT_TRUE(items.empty());
    });
    EXPECT_NO_THROW({
        const auto items = BatchQcRunner::Discover(std::string(), false, {});
        EXPECT_TRUE(items.empty());
    });
}

// ---------------------------------------------------------------------------
// 4) 报告写侧：五种格式都要能落到「中文目录 + 中文文件名」
//
// 这里只判"写成功 + 内容非空 + 魔数/结构自证"，跨格式内容一致性由
// test_report_format_consistency.cpp 负责，不重复。
// ---------------------------------------------------------------------------
TEST_F(ChinesePathIo, ReportExportWritesChinesePathsForAllFormats) {
    const std::string base = ToUtf8String(work_ / fs::u8path(kBaseName));
    const QcExportBundle bundle = MakeBundle();

    struct Expectation {
        QcReportFormat format;
        const char* ext;
        const char* marker;
    };
    const Expectation expectations[] = {
        {QcReportFormat::Json, ".json", "{"},     {QcReportFormat::Csv, ".csv", "u8.path.rule"},
        {QcReportFormat::Html, ".html", "<html"}, {QcReportFormat::Text, ".txt", "u8.path.rule"},
        {QcReportFormat::Pdf, ".pdf", "%PDF"},
    };

    for (const Expectation& expectation : expectations) {
        const std::string target = base + expectation.ext;
        const bool ok = (expectation.format == QcReportFormat::Pdf)
                            ? QcReportExporter::ExportPdf(target, bundle).ok
                            : QcReportExporter::Export(target, bundle, expectation.format);
        ASSERT_TRUE(ok) << "导出失败: " << target;
        ASSERT_TRUE(fs::exists(ToNativePath(target))) << "文件没落盘: " << target;
        const std::string content = ReadText(target);
        EXPECT_GT(content.size(), 0u) << target;
        EXPECT_NE(content.find(expectation.marker), std::string::npos)
            << "内容不含 " << expectation.marker << ": " << target;
    }
}

// ---------------------------------------------------------------------------
// 5) 批量导出路径：保留子目录结构、输出 path 是 UTF-8、文件真的写出来
//
// 入向（ReportBasePath 的 file_path/root/out_dir）与出向（result.output_path
// 与 exports 的 target）都在这一条里过一遍 —— 任一侧退回 ANSI 都会红。
// ---------------------------------------------------------------------------
TEST_F(ChinesePathIo, ApplyExportPathsKeepsSubdirectoryAndUtf8) {
    const fs::path sub = work_ / fs::u8path(kSubDirName);
    ASSERT_TRUE(fs::create_directories(sub));
    const std::string source = WriteFile(sub, kVideoName, "video");
    const std::string out_dir = ToUtf8String(work_ / fs::u8path(u8"导出"));

    QcRunResult result;
    result.ok = true;
    result.report = MakeReport();
    QcProfile profile;
    profile.id = "broadcast";
    profile.name = u8"广播级";

    videoeye::ui::ApplyExportPaths(result, out_dir, ToUtf8String(work_), source, {QcReportFormat::Json}, profile);

    EXPECT_FALSE(result.export_failed) << result.output_path;
    ASSERT_FALSE(result.output_path.empty());
    EXPECT_NE(result.output_path.find(u8"\u5b50\u76ee\u5f55"), std::string::npos)
        << "子目录结构丢了: " << result.output_path;
    EXPECT_NE(result.output_path.find(u8"\u89c6\u9891"), std::string::npos)
        << "文件名不是 UTF-8: " << result.output_path;
    EXPECT_EQ(result.output_path.substr(result.output_path.size() - 5), ".json");
    EXPECT_TRUE(fs::exists(ToNativePath(result.output_path))) << result.output_path;
    EXPECT_GT(ReadText(result.output_path).size(), 0u);
}

// ---------------------------------------------------------------------------
// 6) 修复侧：中文路径的源文件必须能取到身份（大小 + 封装签名）
//
// 实现里 exists()/is_regular_file()/file_size() 只要有一处直传 UTF-8 std::string，
// 这里就会看到 size_bytes==0 且 signature 为空 —— 用户看到的是"输入文件不存在"。
// ---------------------------------------------------------------------------
TEST_F(ChinesePathIo, RepairInspectorReadsChinesePathSource) {
    const std::string media = WriteFile(work_, kVideoName, MinimalMp4());

    RepairRequest request;
    request.source_path = media;
    request.output_directory = ToUtf8String(work_ / fs::u8path(u8"修复输出"));

    const videoeye::repair::RepairProgress progress; // 空 std::function，等价于不订阅进度
    const auto outcome = RepairInspector::Inspect(request, nullptr, progress);

    // 关键断言是 summary / blocking_reasons 里的那条「输入文件不存在」：SnapshotFile
    // 内部自己会转路径，所以 size_bytes 与 signature 在"入口没转"时**照样是对的**。
    EXPECT_EQ(outcome.diagnosis.summary.find(u8"不存在"), std::string::npos) << outcome.diagnosis.summary;
    for (const std::string& reason : outcome.diagnosis.blocking_reasons) {
        EXPECT_EQ(reason.find(u8"不存在"), std::string::npos) << reason;
    }
    EXPECT_EQ(outcome.diagnosis.input.size_bytes, MinimalMp4().size()) << outcome.diagnosis.summary;
    EXPECT_EQ(outcome.diagnosis.input.signature, "ftyp") << outcome.diagnosis.summary;
}

// ---------------------------------------------------------------------------
// 7) 修复产物大小：中文目录下必须是实测值，不能是 0
//
// RepairEngine 发布产物后用 filesystem::file_size 回填 artifacts 的区间；直传
// UTF-8 std::string 时它会失败并把大小记成 0，用户看到 "bytes [0, 0)" 的产物。
// TestMediaFactory 自身没有做路径编码转换，所以先在英文目录造样本再 rename 过来。
// ---------------------------------------------------------------------------
TEST_F(ChinesePathIo, RepairEngineReportsRealOutputSizeForChinesePaths) {
    const fs::path ascii_dir = root_ / "ascii";
    ASSERT_TRUE(fs::create_directories(ascii_dir));
    const auto media = videoeye_test::CreateTestMedia(ToUtf8String(ascii_dir), "u8src", {});
    ASSERT_TRUE(media.ok) << media.error;

    const std::string source = ToUtf8String(work_ / fs::u8path(u8"素材修复.mp4"));
    std::error_code ec;
    fs::rename(ToNativePath(media.path), ToNativePath(source), ec);
    ASSERT_FALSE(ec) << "rename 到中文路径失败: " << ec.message();
    ASSERT_GT(fs::file_size(ToNativePath(source)), 0u);

    RepairRequest request;
    request.source_path = source;
    request.output_directory = ToUtf8String(work_ / fs::u8path(u8"修复输出"));

    std::string last_stage;
    const videoeye::repair::RepairProgress progress = [&last_stage](int percent, const std::string& stage,
                                                                    const std::string& detail) {
        last_stage = "[" + std::to_string(percent) + "%] " + stage + ": " + detail;
    };
    // 捕获异常而不是让它逃逸：RepairOutputWriter / RepairUtils 里有若干
    // `path(std::string)` 与 `dir / std::string`，在 C locale 下对非 ASCII 字节抛
    // system_error。让 FatalFailure 带上"卡在哪个阶段"，比 gtest 的
    // "unknown file: C++ exception" 有用得多。
    videoeye::model::RepairResult result;
    try {
        result = videoeye::repair::RepairEngine::Run(request, nullptr, progress);
    } catch (const std::exception& ex) {
        FAIL() << "修复在阶段 " << last_stage << " 抛出异常: " << ex.what();
        return;
    }

    ASSERT_EQ(result.status, videoeye::model::RepairBusinessStatus::Repaired)
        << "summary=" << result.summary << " error=" << result.error;
    ASSERT_TRUE(result.output_published);
    ASSERT_FALSE(result.artifacts.empty());

    bool saw_output = false;
    for (const auto& artifact : result.artifacts) {
        if (artifact.role != "output")
            continue;
        saw_output = true;
        EXPECT_EQ(artifact.stored_range.find("bytes [0, 0)"), std::string::npos)
            << "产物大小被记成 0（file_size 没走原生路径）: " << artifact.stored_range;
        EXPECT_FALSE(artifact.path.empty());
        EXPECT_TRUE(fs::exists(ToNativePath(artifact.path))) << artifact.path;
        // 产物必须落在**用户指定的那个中文目录**里。只查"文件存在"是不够的：
        // 路径被按 ANSI 解释时会建成另一个乱码目录，文件照样"存在"。
        EXPECT_EQ(ToNativePath(artifact.path).parent_path(), ToNativePath(request.output_directory))
            << "产物跑到了错误的目录: " << artifact.path;
        // 文件名主干必须保住源文件的中文名（不是按 ANSI 压出来的乱码）。
        // parent_path() 只看父目录，抓不到这一半，所以单独判。
        EXPECT_NE(ToUtf8String(ToNativePath(artifact.path).stem()).find(u8"素材修复"), std::string::npos)
            << "产物文件名被压成乱码: " << artifact.path;
    }
    EXPECT_TRUE(saw_output) << "修复成功却没有 output 产物条目";
}
