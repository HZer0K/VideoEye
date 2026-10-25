#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h 会把 min/max 定义成宏，之后 std::max/std::min 全被撕碎（本项目里
// MetricSeries.h / LoudnessPoint.h 就在用 std::max）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcProfileMapper.h"
#include "core/qc/QcReportFormat.h"
#include "core/qc/QcRuleEngine.h"
#include "ui/reporting_panel/analysis_task.h"
#include "ui/reporting_panel/batch_filter.h"
#include "ui/reporting_panel/report_path.h"
#include "ui/reporting_panel/result_stamp.h"

namespace fs = std::filesystem;

// 本文件的断言一律写 model::Xxx, 但它不像其它测试那样 using namespace videoeye,
// 所以这里补一个命名空间别名，免得把每条 using 都改成 videoeye::model::Xxx。
namespace model = videoeye::model;

using videoeye::AnalysisOptions;
using videoeye::model::AnalysisResult;
using videoeye::model::AnalysisStatus;
using videoeye::qc::BatchItemStatus;
using videoeye::qc::BatchQcItemResult;
using videoeye::qc::QcProfile;
using videoeye::qc::QcReportFormat;
using videoeye::qc::QcRuleEngine;
using videoeye::qc::QcRunResult;
using videoeye::ui::AnalysisOptionsCovers;
using videoeye::ui::AnalysisTask;
using videoeye::ui::ApplyExportPaths;
using videoeye::ui::BatchMatchesSeverityFilter;
using videoeye::ui::BatchMatchesStatusFilter;
using videoeye::ui::CanReuseAnalysis;
using videoeye::ui::FileVersionStamp;
using videoeye::ui::IsResultFresh;
using videoeye::ui::kBatchSeverityFilterAll;
using videoeye::ui::kBatchSeverityFilterClean;
using videoeye::ui::kBatchSeverityFilterCritical;
using videoeye::ui::kBatchSeverityFilterError;
using videoeye::ui::kBatchSeverityFilterInfo;
using videoeye::ui::kBatchSeverityFilterWarning;
using videoeye::ui::kBatchStatusFilterAll;
using videoeye::ui::QueryFileVersion;
using videoeye::ui::RecycleTask;
using videoeye::ui::ReportBasePath;
using videoeye::ui::ResultStamp;

namespace {

// 跨平台路径后缀判断（忽略分隔符差异）
bool EndsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size())
        return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---------------------------------------------------------------------------
// P0#2：批量导出路径唯一性 —— 不同子目录中的同名文件必须落到不同路径，
// 且保留相对子目录、去掉扩展名。
// ---------------------------------------------------------------------------
TEST(ReportBasePathTest, SameNameDifferentSubdirAreDistinct) {
    const std::string root = "C:/root";
    const std::string out = "C:/out";
    const auto p1 = ReportBasePath(out, root, "C:/root/sub1/clip.mp4").string();
    const auto p2 = ReportBasePath(out, root, "C:/root/sub2/clip.mp4").string();

    EXPECT_NE(p1, p2);                                   // 同名文件不得互相覆盖
    EXPECT_EQ(fs::path(p1).filename().string(), "clip"); // 去掉扩展名
    EXPECT_EQ(fs::path(p2).filename().string(), "clip");
    // 相对子目录被保留
    EXPECT_NE(fs::path(p1).parent_path().string().find("sub1"), std::string::npos);
    EXPECT_NE(fs::path(p2).parent_path().string().find("sub2"), std::string::npos);
}

TEST(ReportBasePathTest, DeepRelativeSubdirPreserved) {
    const auto p = ReportBasePath("C:/out", "C:/root", "C:/root/a/b/clip.mp4").string();
    EXPECT_EQ(fs::path(p).filename().string(), "clip");
    EXPECT_NE(fs::path(p).parent_path().string().find("a"), std::string::npos);
    EXPECT_NE(fs::path(p).parent_path().string().find("b"), std::string::npos);
}

TEST(ReportBasePathTest, NotUnderRootFallsBackToFileName) {
    // file 不在 root 之下：应退回只取文件名（不带扩展名），且不抛异常
    const auto p = ReportBasePath("C:/out", "C:/other", "C:/root/clip.mp4").string();
    EXPECT_EQ(fs::path(p).filename().string(), "clip");
    EXPECT_TRUE(fs::path(p).has_parent_path());
}

// 目录逃逸：file 借 ".." 爬出 root。relative() 算出来的相对部分带 ".."，
// 拼到 out_dir 上就写到导出目录外面去了 —— 必须退回只取文件名。
TEST(ReportBasePathTest, ParentEscapeFallsBackToFileName) {
    const auto p = ReportBasePath("C:/out", "C:/root/sub", "C:/root/../outside/clip.mp4").string();
    EXPECT_EQ(fs::path(p).filename().string(), "clip");
    EXPECT_TRUE(fs::path(p).has_parent_path());
    // 落盘路径里不允许残留任何 ".." 分量
    EXPECT_EQ(p.find(".."), std::string::npos);
}

// root 为空时 relative() 会退回绝对路径（跨盘更是如此），同样不能照拼
TEST(ReportBasePathTest, EmptyRootFallsBackToFileName) {
    const auto p = ReportBasePath("C:/out", "", "C:/root/clip.mp4").string();
    EXPECT_EQ(fs::path(p).filename().string(), "clip");
    EXPECT_EQ(p.find(".."), std::string::npos);
}

// 建一个目录链接（Windows 用 junction，POSIX 用符号链接）。
//
// 为什么不能只用 std::filesystem::create_directory_symlink: 它在未开启"开发者模式"
// 的 Windows 上会**静默退化成一个普通目录**（返回成功但根本不是链接，链接也就
// 测不出任何东西）。junction 则不需要任何特权，而且是问题单里点名的那一种
// （out/sub 是指向外部的 Windows junction）。
bool MakeDirectoryLink(const fs::path& link, const fs::path& target, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    fs::create_directory(link, ec);
    if (ec && !fs::is_directory(link, ec))
        return false;
    ec.clear();
    const std::wstring sub = L"\\??\\" + target.wstring();
    const std::wstring print = target.wstring();
    const USHORT sub_bytes = static_cast<USHORT>(sub.size() * sizeof(WCHAR));
    const USHORT print_bytes = static_cast<USHORT>(print.size() * sizeof(WCHAR));
    const size_t path_bytes = sub_bytes + sizeof(WCHAR) + print_bytes + sizeof(WCHAR);
    const USHORT data_len = static_cast<USHORT>(8 + path_bytes);
    const size_t total = 8 + data_len;

    struct ReparseBuffer {
        ULONG tag;
        USHORT data_length;
        USHORT reserved;
        USHORT sub_name_offset;
        USHORT sub_name_length;
        USHORT print_name_offset;
        USHORT print_name_length;
        WCHAR path_buffer[1];
    };
    std::vector<char> buf(total, 0);
    auto* rp = reinterpret_cast<ReparseBuffer*>(buf.data());
    rp->tag = IO_REPARSE_TAG_MOUNT_POINT;
    rp->data_length = data_len;
    rp->sub_name_offset = 0;
    rp->sub_name_length = sub_bytes;
    rp->print_name_offset = static_cast<USHORT>(sub_bytes + sizeof(WCHAR));
    rp->print_name_length = print_bytes;
    WCHAR* cursor = rp->path_buffer;
    std::memcpy(cursor, sub.data(), sub_bytes);
    cursor += sub.size();
    *cursor++ = L'\0';
    std::memcpy(cursor, print.data(), print_bytes);
    cursor += print.size();
    *cursor++ = L'\0';

    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(total), nullptr, 0,
                                    &returned, nullptr);
    if (!ok)
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    CloseHandle(handle);
    return ok != 0;
#else
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
}

// 建出来的必须**真的是**链接：往目标里放一个文件，能通过 link 看到才算数。
// （std::filesystem 在 Windows 上可能静默退化成普通目录，这里把它挡掉。）
bool LinkReallyWorks(const fs::path& link, const fs::path& target) {
    std::error_code ec;
    const auto marker = target / "ve_link_marker.txt";
    std::ofstream(marker.string()) << "x";
    const bool visible = fs::exists(link / "ve_link_marker.txt", ec);
    fs::remove(marker, ec);
    return visible;
}

// 输出侧的目录逃逸：输入路径本身很干净（"sub/clip.mp4"），但导出目录里的
// out/sub 已经是指向外部目录的 junction / 符号链接 —— 照拼的话报告就落到盘外去了。
// 必须校验最终父目录的**实际**位置，链接目录一律退回铺平。
TEST(ReportBasePathTest, LinkedOutputSubdirFallsBackToFlatName) {
    const auto tmp = fs::temp_directory_path() / "ve_rp_link_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "out", ec);
    fs::create_directories(tmp / "outside", ec);

    const auto link = tmp / "out" / "sub";
    std::error_code link_ec;
    const bool made = MakeDirectoryLink(link, tmp / "outside", link_ec);
    if (!made || !LinkReallyWorks(link, tmp / "outside")) {
        fs::remove_all(tmp, ec);
        GTEST_SKIP() << "当前环境建不出可用的目录链接，跳过: " << link_ec.message();
    }

    const auto p =
        ReportBasePath((tmp / "out").string(), (tmp / "root").string(), (tmp / "root" / "sub" / "clip.mp4").string());
    // 不能落进链接指向的 outside 目录里
    const auto canonical_parent = fs::weakly_canonical(fs::path(p).parent_path(), ec);
    ASSERT_FALSE(ec);
    EXPECT_NE(fs::weakly_canonical(tmp / "outside", ec), canonical_parent) << "报告路径穿过了输出目录里的链接";
    // 退回"铺平到导出目录"：文件名还在，子目录不再保留
    EXPECT_EQ(fs::path(p).filename().string(), "clip");
    EXPECT_EQ(fs::path(p).parent_path(), tmp / "out");

    fs::remove_all(tmp, ec);
}

// 导出目录本身干净时，正常子目录必须照旧保留（上面的退回逻辑不能误伤）
TEST(ReportBasePathTest, NormalOutputSubdirIsStillPreserved) {
    const auto tmp = fs::temp_directory_path() / "ve_rp_nolink_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "out" / "sub", ec);

    const auto p =
        ReportBasePath((tmp / "out").string(), (tmp / "root").string(), (tmp / "root" / "sub" / "clip.mp4").string());
    EXPECT_EQ(fs::weakly_canonical(fs::path(p).parent_path(), ec), fs::weakly_canonical(tmp / "out" / "sub", ec));
    EXPECT_EQ(fs::path(p).filename().string(), "clip");

    fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// P0#2：导出落盘 —— 成功时返回真实路径、文件确实生成；目标目录不可写时
// 标记 export_failed=true（让上层区分"分析成功 / 导出失败"）。
// ---------------------------------------------------------------------------
TEST(ApplyExportPathsTest, WritableDirWritesAllFormats) {
    const auto tmp = fs::temp_directory_path() / "ve_rp_writable_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const auto root = tmp / "root";
    fs::create_directories(root / "a", ec);
    const std::string file = (root / "a" / "b.mp4").string();

    QcRunResult result;
    result.ok = true;
    result.report.file_path = file;
    result.report.file_name = "b.mp4";
    result.report.container_format = "mp4";
    result.report.duration_seconds = 1.0;

    QcProfile profile;
    const std::vector<QcReportFormat> formats{QcReportFormat::Json, QcReportFormat::Csv, QcReportFormat::Html};

    ApplyExportPaths(result, tmp.string(), root.string(), file, formats, profile);

    EXPECT_FALSE(result.export_failed);
    // 主格式取 formats 的首项（Json）
    EXPECT_TRUE(EndsWith(result.output_path, ".json"));
    EXPECT_TRUE(fs::exists(result.output_path));
    EXPECT_TRUE(fs::exists(fs::path(result.output_path).replace_extension(".csv")));
    EXPECT_TRUE(fs::exists(fs::path(result.output_path).replace_extension(".html")));

    fs::remove_all(tmp, ec);
}

TEST(ApplyExportPathsTest, EmptyOutDirIsNoOp) {
    QcRunResult result;
    result.ok = true;
    QcProfile profile;
    ApplyExportPaths(result, "", "C:/root", "C:/root/a.mp4", {QcReportFormat::Json}, profile);
    EXPECT_FALSE(result.export_failed);
    EXPECT_TRUE(result.output_path.empty());
}

TEST(ApplyExportPathsTest, FailedAnalysisIsNoOp) {
    QcRunResult result;
    result.ok = false;
    QcProfile profile;
    ApplyExportPaths(result, fs::temp_directory_path().string(), "C:/root", "C:/root/a.mp4", {QcReportFormat::Json},
                     profile);
    EXPECT_FALSE(result.export_failed);
    EXPECT_TRUE(result.output_path.empty());
}

TEST(ApplyExportPathsTest, UnwritableDirMarksExportFailed) {
    const auto tmp = fs::temp_directory_path() / "ve_rp_unwritable_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // blocker 是一个普通文件：以它为 out_dir 时 create_directories 必然失败，
    // 后续写文件也会失败 —— 模拟"目标目录不可写"。
    const auto blocker = tmp / "blocker";
    // 注意: 不能写成 `{ std::ofstream(blocker); }` —— 那是 most vexing parse,
    // 会被解析成"声明一个名为 blocker 的 ofstream 变量", 文件根本没被创建,
    // 于是这个用例其实一直在测一个可写目录（恒定失败）。必须给出变量名。
    { std::ofstream f(blocker); }

    const auto root = tmp / "root";
    fs::create_directories(root / "a", ec);
    const std::string file = (root / "a" / "b.mp4").string();

    QcRunResult result;
    result.ok = true;
    result.report.file_path = file;
    result.report.file_name = "b.mp4";
    result.report.container_format = "mp4";

    QcProfile profile;
    ApplyExportPaths(result, blocker.string(), root.string(), file, {QcReportFormat::Json}, profile);

    EXPECT_TRUE(result.export_failed);                  // 导出失败被标记
    EXPECT_TRUE(EndsWith(result.output_path, ".json")); // 路径仍填回（UI 可展示）
    EXPECT_FALSE(fs::exists(result.output_path));       // 实际没落盘

    fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// P0#1：线程回收 —— 对仍 joinable 的 std::thread 直接赋值会 terminate；
// RecycleTask 先 join 再 reset，连续分析两次不会崩。
//
// 注意线程体里要自己置 body_done：RecycleTask 靠它判断"任务体在预算内回来了"
// （std::thread 没有带超时的 join），真实任务体本来就是这么干的。
// ---------------------------------------------------------------------------
namespace {

// 起一个"立刻跑完并自报回来"的线程，模拟一次秒结束的分析任务体。
std::shared_ptr<AnalysisTask> MakeFinishedTask() {
    auto task = std::make_shared<AnalysisTask>();
    task->thread = std::thread([task] { task->body_done.store(true, std::memory_order_release); });
    return task;
}

} // namespace

TEST(AnalysisTaskTest, RecycleJoinsJoinableAndResets) {
    auto task = MakeFinishedTask();
    RecycleTask(task);
    EXPECT_FALSE(task); // 已 reset
}

TEST(AnalysisTaskTest, RestartAfterRecycleDoesNotTerminate) {
    auto task = MakeFinishedTask();
    RecycleTask(task); // 第一次"运行"结束并回收
    EXPECT_FALSE(task);

    // 第二次运行：若第一次仍 joinable，下面对 task 重新赋值（std::thread 赋值）会 terminate
    task = MakeFinishedTask();
    RecycleTask(task);
    EXPECT_FALSE(task);
}

TEST(AnalysisTaskTest, RecycleNullAndUnstartedSafe) {
    std::shared_ptr<AnalysisTask> nulltask;
    EXPECT_NO_THROW(RecycleTask(nulltask));

    auto empty = std::make_shared<AnalysisTask>(); // 默认构造，未启动线程
    // 未启动的线程立刻放行（不让 WaitTaskBody 白等满整个预算）
    RecycleTask(empty);
    EXPECT_FALSE(empty);
}

// 预算耗尽只是**告警线**，不是 join 的上界：报告页走严格 Cooperative（见 RecycleTask），
// 超预算也必须等任务体真正退出，绝不 detach —— 任务体捕获了面板 this，detach 之后
// 它会往已销毁的 QWidget 上排队消息。
//
// 这里用一个"600ms 才退出、且忘了置 body_done"的任务体：预算(150ms)必然错过，
// 断言返回值如实反映观测结果(false)，同时总耗时必须覆盖任务体的 600ms ——
// 也就是说 join 确实等到了它退出，而不是放弃。
TEST(AnalysisTaskTest, RecycleGivesUpAfterBudget) {
    auto task = std::make_shared<AnalysisTask>();
    task->thread = std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        // 故意不置 body_done：模拟任务体没按约定自报返回
    });

    const auto start = std::chrono::steady_clock::now();
    const bool body_back = RecycleTask(task, 150);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(body_back) << "预算内没等到 body_done, 必须如实返回 false（告警线）";
    EXPECT_GE(elapsed_ms, 600) << "严格 Cooperative: 超预算后仍然 join 到任务体真正退出（不 detach）; 实测 "
                               << elapsed_ms << "ms";
    EXPECT_LT(elapsed_ms, 5000);
    EXPECT_FALSE(task); // 无论如何都把句柄还了出来
}

// ---------------------------------------------------------------------------
// P1#4：扫描状态映射 —— 完整 / 抽样 / 失败 / 取消 必须被正确翻译到报告标记，
// 抽样不得给出"通过"结论，失败 / 取消不得视为完整。
// ---------------------------------------------------------------------------
TEST(QcScanStatusTest, CompleteIsFull) {
    model::AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = model::AnalysisStatus::Complete;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_TRUE(report.completed);
    EXPECT_FALSE(report.partial);
}

TEST(QcScanStatusTest, SampledIsPartialAndNotPass) {
    model::AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = model::AnalysisStatus::Sampled;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_TRUE(report.completed);                  // 抽样是"跑完"的部分结果，可结算
    EXPECT_TRUE(report.partial);                    // 但标记为部分
    EXPECT_EQ(report.verdict, "抽样完成·仅供参考"); // 禁止给出"通过"
}

TEST(QcScanStatusTest, FailedIsNotCompleted) {
    model::AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = model::AnalysisStatus::Failed;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_FALSE(report.completed);
    EXPECT_FALSE(report.partial);
}

TEST(QcScanStatusTest, CancelledIsNotCompleted) {
    model::AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = model::AnalysisStatus::Cancelled;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_FALSE(report.completed);
    EXPECT_FALSE(report.partial);
}

// ---------------------------------------------------------------------------
// 报告页"结果新鲜度"：切换文件 / 模板后上一次结果必须作废，否则会把旧结果导出去。
// （UI 侧的导出按钮使能就是这套判定的直接映射。）
// ---------------------------------------------------------------------------
TEST(ResultFreshnessTest, MatchingFileAndProfileIsFresh) {
    const ResultStamp stamp{"a.mp4", "hls-vod", true};
    EXPECT_TRUE(IsResultFresh(stamp, "a.mp4", "hls-vod"));
}

TEST(ResultFreshnessTest, SwitchingFileInvalidates) {
    const ResultStamp stamp{"a.mp4", "hls-vod", true};
    EXPECT_FALSE(IsResultFresh(stamp, "b.mp4", "hls-vod")) << "切换到另一个文件后旧结果必须过期";
}

TEST(ResultFreshnessTest, SwitchingProfileInvalidates) {
    const ResultStamp stamp{"a.mp4", "hls-vod", true};
    EXPECT_FALSE(IsResultFresh(stamp, "a.mp4", "broadcast")) << "切换到另一个模板后旧结果必须过期";
}

TEST(ResultFreshnessTest, InvalidStampOrEmptyPathIsNeverFresh) {
    EXPECT_FALSE(IsResultFresh(ResultStamp{}, "a.mp4", "hls-vod")) << "没产出过结果（失败/未分析）永远不新鲜";
    EXPECT_FALSE(IsResultFresh(ResultStamp{"", "", true}, "a.mp4", "hls-vod"));
    EXPECT_FALSE(IsResultFresh(ResultStamp{"a.mp4", "hls-vod", true}, "", "hls-vod"))
        << "未选择文件时不新鲜（导出应禁用）";
}

// ---------------------------------------------------------------------------
// 报告页结果复用：换模板时只有在"同一文件版本 + 所需分析选项已覆盖 + 扫描完整成功"
// 三个条件同时成立时才复用缓存的原始 AnalysisResult，否则必须重新分析。
// ---------------------------------------------------------------------------
TEST(AnalysisOptionsCoversTest, DeeperCachedCoversShallowerRequired) {
    const auto deep = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Deep);
    const auto standard = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Standard);
    const auto fast = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Fast);

    EXPECT_TRUE(AnalysisOptionsCovers(deep, deep));
    EXPECT_TRUE(AnalysisOptionsCovers(deep, standard));
    EXPECT_TRUE(AnalysisOptionsCovers(deep, fast));
    EXPECT_TRUE(AnalysisOptionsCovers(standard, fast));
    // 反向不行：更浅的结果覆盖不了更深的分析要求
    EXPECT_FALSE(AnalysisOptionsCovers(fast, standard)) << "Fast 没跑音频 QC";
    EXPECT_FALSE(AnalysisOptionsCovers(standard, deep)) << "Standard 没解帧类型";
}

TEST(AnalysisOptionsCoversTest, CoarserSampleIntervalDoesNotCover) {
    AnalysisOptions cached; // 默认 1.0s
    AnalysisOptions required;
    required.sample_interval_seconds = 0.5; // 要求更细
    EXPECT_FALSE(AnalysisOptionsCovers(cached, required));

    required.sample_interval_seconds = 2.0; // 要求更粗 -> 覆盖
    EXPECT_TRUE(AnalysisOptionsCovers(cached, required));
}

namespace {

videoeye::ui::ResultStamp MakeStamp(const std::string& path, const videoeye::ui::FileVersionStamp& version,
                                    const AnalysisOptions& options, videoeye::model::AnalysisStatus status) {
    videoeye::ui::ResultStamp stamp;
    stamp.path = path;
    stamp.profile_id = "deep";
    stamp.valid = true;
    stamp.options = options;
    stamp.scan_status = status;
    stamp.version = version;
    return stamp;
}

videoeye::ui::FileVersionStamp VersionOf(std::uint64_t size, std::int64_t mtime_ns) {
    videoeye::ui::FileVersionStamp v;
    v.valid = true;
    v.size = size;
    v.mtime_ns = mtime_ns;
    return v;
}

} // namespace

TEST(CanReuseAnalysisTest, SameVersionCoveredAndCompleteIsReusable) {
    const auto deep = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Deep);
    const auto standard = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Standard);
    const auto stamp = MakeStamp("a.mp4", VersionOf(1024, 100), deep, videoeye::model::AnalysisStatus::Complete);
    EXPECT_TRUE(CanReuseAnalysis(stamp, "a.mp4", VersionOf(1024, 100), standard));
}

TEST(CanReuseAnalysisTest, FileVersionChangeForcesReanalysis) {
    const auto options = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Deep);
    const auto stamp = MakeStamp("a.mp4", VersionOf(1024, 100), options, videoeye::model::AnalysisStatus::Complete);
    // 大小变了
    EXPECT_FALSE(CanReuseAnalysis(stamp, "a.mp4", VersionOf(2048, 100), options));
    // 修改时间变了
    EXPECT_FALSE(CanReuseAnalysis(stamp, "a.mp4", VersionOf(1024, 200), options));
}

// 指纹必须精确到秒以下: 同一秒内被替换成"大小相同"的另一个文件（脚本重导出、
// 拷回旧版本）在秒级指纹下是"没变"，于是把上一次的结果复用出去了。
TEST(CanReuseAnalysisTest, SameSecondButDifferentSubSecondIsNotReusable) {
    const auto options = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Deep);
    // 1.5s 与 1.9s：取整到秒都是 1s，但确实是两个不同的版本
    const auto stamp =
        MakeStamp("a.mp4", VersionOf(1024, 1500000000LL), options, videoeye::model::AnalysisStatus::Complete);
    EXPECT_FALSE(CanReuseAnalysis(stamp, "a.mp4", VersionOf(1024, 1900000000LL), options));
    // 同一版本照常可复用（精度提高不能把"没变"判成"变了"）
    EXPECT_TRUE(CanReuseAnalysis(stamp, "a.mp4", VersionOf(1024, 1500000000LL), options));
}

TEST(CanReuseAnalysisTest, NetworkUriOrMissingVersionIsNeverReusable) {
    const auto options = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Deep);
    // 网络源没有本地指纹（valid=false）—— 默认重新分析
    const auto stamp = MakeStamp("http://host/a.m3u8", videoeye::ui::FileVersionStamp{}, options,
                                 videoeye::model::AnalysisStatus::Complete);
    EXPECT_FALSE(CanReuseAnalysis(stamp, "http://host/a.m3u8", videoeye::ui::FileVersionStamp{}, options));
    // 当前路径取不到指纹同样不复用
    EXPECT_FALSE(CanReuseAnalysis(stamp, "a.mp4", videoeye::ui::FileVersionStamp{}, options));
}

TEST(CanReuseAnalysisTest, IncompleteScanIsNeverReusable) {
    const auto options = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Deep);
    const auto version = VersionOf(1024, 100);
    EXPECT_FALSE(CanReuseAnalysis(MakeStamp("a.mp4", version, options, videoeye::model::AnalysisStatus::Failed),
                                  "a.mp4", version, options));
    EXPECT_FALSE(CanReuseAnalysis(MakeStamp("a.mp4", version, options, videoeye::model::AnalysisStatus::Cancelled),
                                  "a.mp4", version, options));
    // 抽样完成属于"跑完"，可复用（报告本身会标 partial）
    EXPECT_TRUE(CanReuseAnalysis(MakeStamp("a.mp4", version, options, videoeye::model::AnalysisStatus::Sampled),
                                 "a.mp4", version, options));
}

TEST(CanReuseAnalysisTest, InsufficientOptionsOrDifferentPathForcesReanalysis) {
    const auto fast = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Fast);
    const auto deep = videoeye::qc::OptionsForDepth(videoeye::qc::QcAnalysisDepth::Deep);
    const auto version = VersionOf(1024, 100);
    const auto stamp = MakeStamp("a.mp4", version, fast, videoeye::model::AnalysisStatus::Complete);
    // 缓存只跑了 Fast，新模板要求 Deep
    EXPECT_FALSE(CanReuseAnalysis(stamp, "a.mp4", version, deep));
    // 换了文件
    EXPECT_FALSE(CanReuseAnalysis(stamp, "b.mp4", version, fast));
    // 戳无效（没成功产出过结果）
    videoeye::ui::ResultStamp invalid = stamp;
    invalid.valid = false;
    EXPECT_FALSE(CanReuseAnalysis(invalid, "a.mp4", version, fast));
}

TEST(QueryFileVersionTest, LocalFileYieldsConsistentVersion) {
    const auto dir = fs::temp_directory_path() / "ve_result_stamp_version";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string file = (dir / "probe.bin").string();

    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "0123456789";
    }
    const auto first = QueryFileVersion(file);
    const auto second = QueryFileVersion(file);
    EXPECT_TRUE(first.valid);
    EXPECT_EQ(first.size, 10u);
    EXPECT_TRUE(first == second) << "同一文件两次取指纹必须一致";

    fs::remove_all(dir, ec);
}

TEST(QueryFileVersionTest, MissingFileAndNetworkUriHaveNoVersion) {
    EXPECT_FALSE(QueryFileVersion("").valid);
    EXPECT_FALSE(QueryFileVersion("C:/definitely/not/here/ve_nope.mp4").valid) << "取不到指纹时不得冒充可用版本";
    EXPECT_FALSE(QueryFileVersion("http://127.0.0.1:9/live.m3u8").valid) << "网络源没有本地指纹，默认重新分析";
}

// ---------------------------------------------------------------------------
// 批量表格筛选口径：状态 / 严重度两个下拉框的纯判定。
// UI 只负责把 combo 的 itemData 传进来，这里守住各口径的边界 ——
// 尤其"含某级别"是 ≥ 不是 =（一条同时有致命和警告的结果在"含警告"下也要可见），
// 以及"无异常"对失败条目（计数全 0）同样匹配。
// ---------------------------------------------------------------------------
TEST(BatchStatusFilterTest, AllMatchesEveryStatus) {
    using videoeye::qc::BatchItemStatus;
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::Pending));
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::Running));
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::Succeeded));
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::Failed));
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::TimedOut));
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::Cancelled));
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::Skipped));
    EXPECT_TRUE(BatchMatchesStatusFilter(kBatchStatusFilterAll, BatchItemStatus::ExportFailed));
}

TEST(BatchStatusFilterTest, SpecificStatusMatchesOnlyItself) {
    using videoeye::qc::BatchItemStatus;
    const int failed = static_cast<int>(BatchItemStatus::Failed);
    EXPECT_TRUE(BatchMatchesStatusFilter(failed, BatchItemStatus::Failed));
    // 导出失败是"分析成功但没落盘"，与 Failed 是两个终态，不能互相匹配
    EXPECT_FALSE(BatchMatchesStatusFilter(failed, BatchItemStatus::ExportFailed));
    EXPECT_FALSE(BatchMatchesStatusFilter(failed, BatchItemStatus::TimedOut));
    EXPECT_FALSE(BatchMatchesStatusFilter(failed, BatchItemStatus::Succeeded));
}

TEST(BatchSeverityFilterTest, AtLeastNotEquals) {
    videoeye::qc::BatchQcItemResult item; // 一条同时带致命与警告的结果
    item.critical_count = 1;
    item.warning_count = 2;

    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterAll, item));
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterCritical, item));
    // 阶梯语义：带致命的条目必须在"错误及以上""警告及以上""有任何问题"下都可见。
    // 若退化成"恰好等于该级别"，最严重的条目反而会被更宽松的筛选藏起来。
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterError, item));
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterWarning, item));
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterInfo, item));
    EXPECT_FALSE(BatchMatchesSeverityFilter(kBatchSeverityFilterClean, item));
}

TEST(BatchSeverityFilterTest, ErrorOnlyItem) {
    videoeye::qc::BatchQcItemResult item;
    item.error_count = 3;
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterError, item));
    // 错误属于阻塞级，因此在更宽松的两档下也要在
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterWarning, item));
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterInfo, item));
    // 但没有致命，不该被最严的一档捞出来
    EXPECT_FALSE(BatchMatchesSeverityFilter(kBatchSeverityFilterCritical, item));
    EXPECT_FALSE(BatchMatchesSeverityFilter(kBatchSeverityFilterClean, item));
}

TEST(BatchSeverityFilterTest, WarningOnlyItemStaysBelowBlockingFilters) {
    videoeye::qc::BatchQcItemResult item;
    item.warning_count = 1;
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterWarning, item));
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterInfo, item));
    // 警告不算阻塞级，两档阻塞筛选都不该命中
    EXPECT_FALSE(BatchMatchesSeverityFilter(kBatchSeverityFilterCritical, item));
    EXPECT_FALSE(BatchMatchesSeverityFilter(kBatchSeverityFilterError, item));
    EXPECT_FALSE(BatchMatchesSeverityFilter(kBatchSeverityFilterClean, item));
}

TEST(BatchSeverityFilterTest, CleanMatchesOnlyCleanFilter) {
    videoeye::qc::BatchQcItemResult item; // 分析完成且零问题
    item.status = videoeye::qc::BatchItemStatus::Succeeded;
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterClean, item));
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterAll, item));
    EXPECT_FALSE(BatchMatchesSeverityFilter(kBatchSeverityFilterInfo, item)) << "零问题不等于含提示";

    // 失败条目计数也是全 0：归进"无异常"是合理的（要看失败请用状态筛选），
    // 这条用例把该语义钉住，防止以后有人给 Clean 加"必须成功"的隐藏条件。
    videoeye::qc::BatchQcItemResult failed;
    failed.status = videoeye::qc::BatchItemStatus::Failed;
    EXPECT_TRUE(BatchMatchesSeverityFilter(kBatchSeverityFilterClean, failed));
}

} // namespace
