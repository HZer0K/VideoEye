// QcRunner 生命周期与"超时 detach"的回归测试
//
// 背景（评审 P1-1 / P1-2 的落点）：
//   1) 旧实现：Box 里只放 AnalysisEngine 的裸指针，引擎本体挂在 AnalyzeFile 的局部
//      shared_ptr 上。等待预算到点后 worker 被 detach，而 AnalyzeFile 一返回
//      局部 shared_ptr 就析构 —— 后台线程随后访问的就是已释放的引擎（悬空访问）。
//      现在 Box 自持 shared_ptr<AnalysisEngine>。本文件用"预算 1ms + 取消等待 0"
//      把"判定放弃 → detach" 这条路径稳定打出来，并在返回后让测试继续存活一小段时间，
//      给被 detach 的 worker 留出收尾窗口 —— 这条用例就是 ASan 的回归落点
//      （回退成裸指针实现时，ASan 会在这里报 use-after-free）。
//   1b) 旧实现的等待预算只有一个 join_budget_ms，同时被当成"分析能跑多久"和"关停等
//      多久"：报告页把 5 秒的关停回收预算塞进来，于是正常运行超过 5 秒的分析也被
//      请求取消。现在三项策略各归各的（见 QcRunBudgets.h），"跑了很久但不限时 ==
//      继续等"由下面的策略用例钉住。
//   2) 旧实现"未获准（handle.valid()==false）仍会创建线程"：QcRunner 已改成无状态
//      同步执行器，不再自持 TaskManager，这条路径随之消失；调度拒绝语义由
//      TaskManager 的 BeginHandleRejectionReturnsFullyEmptyHandle 与并发句柄配对
//      测试覆盖（见 test_task_manager.cpp）。
//
// 为什么用"垃圾内容 + 媒体扩展名"的假文件触发超时：
//   IsAnalyzableFile 要求"存在 / 非空 / 不是目录"，而 AnalysisEngine 要真实走完
//   avformat_open_input（读盘 + 探测）才可能失败结算 —— 比主线程"到点检查预算"
//   的一步慢几个数量级，"1 毫秒运行时长预算"因此稳定走放弃分支，且不依赖任何真实
//   媒体文件（CI 不需要媒体样本）。

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "core/qc/AnalysisTaskRegistry.h"
#include "core/qc/QcProfileMapper.h"
#include "core/qc/QcRunBudgets.h"
#include "core/qc/QcRunner.h"
// 报告页的单文件等待策略住在这个"不依赖 Qt"的头里，因此这个不带 Qt 的测试目标也能
// 直接断言它（评审 P1-1：单文件分析必须是不限时的）。
#include "ui/reporting_panel/analysis_task.h"

namespace fs = std::filesystem;

using videoeye::qc::AnalysisTaskRegistry;
using videoeye::qc::ClassifyMediaInput;
using videoeye::qc::DecideWait;
using videoeye::qc::IsAnalyzableFile;
using videoeye::qc::IsAnalyzableInput;
using videoeye::qc::IsNetworkUri;
using videoeye::qc::MediaInput;
using videoeye::qc::OptionsForDepth;
using videoeye::qc::QcAbandonReason;
using videoeye::qc::QcAnalysisDepth;
using videoeye::qc::QcAnalyzeRequest;
using videoeye::qc::QcProfile;
using videoeye::qc::QcRunBudgets;
using videoeye::qc::QcRunCallbacks;
using videoeye::qc::QcRunner;
using videoeye::qc::QcRunResult;
using videoeye::qc::QcWaitDecision;
using videoeye::qc::QcWaitState;
using videoeye::ui::SingleAnalysisBudgets;

namespace {

// AnalyzeFile 放弃收尾的固定文案；用整串相等断言，防止未来改文案时静默漂移。
constexpr char kOverBudgetError[] = "分析超出允许的运行时长";
constexpr char kIoStalledError[] = "分析输入长时间无响应";
constexpr char kCancelledError[] = "分析被取消";

// 用 Fast 档：假文件本来打不开，档位只影响"打开成功后"的扫描深度。
QcProfile FastProfile() {
    QcProfile profile;
    profile.id = "unit-test";
    profile.name = "unit-test";
    profile.depth = QcAnalysisDepth::Fast;
    return profile;
}

// 建一个临时目录；同名目录先清掉，避免上次失败运行的残留干扰。
fs::path MakeScratchDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// 写一个"能过 IsAnalyzableFile 但 FFmpeg 一定打不开"的假媒体文件。
// 4 MiB 接近 FFmpeg 默认探测上限（5 MiB），把 open 失败前的读盘/探测拉得足够长：
// 1 毫秒的运行时长预算必定在引擎结算之前命中（NextWaitSlice 会把轮询夹到 1ms），
// 放弃分支因此是确定的。
fs::path WriteGarbageMediaFile(const fs::path& dir, const std::string& file_name) {
    const fs::path file = dir / file_name;
    std::ofstream out(file, std::ios::binary);
    // 用全 0 而不是可读文本：重复的 ASCII 文本会被 amrwb demuxer **成功识别**成音频
    // （FFmpeg 7.1 上实测 format=amrwb，一路解出 89243 个包 / 1906 秒），于是引擎
    // 走的是成功路径，"引擎失败"这条用例就测不到了。样本必须让所有平台的所有
    // demuxer 都探测失败，全 0 在 FFmpeg 7.1 与 8.1 上都是 Invalid data。
    const std::string chunk(4096, '\0');
    const int chunk_count = (4 * 1024 * 1024) / static_cast<int>(chunk.size());
    for (int i = 0; i < chunk_count; ++i) {
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    out.close();
    return file;
}

} // namespace

// 生命周期错误路径：这三种输入在起线程之前就被挡下，不创建盒子 / worker。
TEST(QcRunnerTest, RejectsMissingEmptyAndDirectoryPaths) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_lifecycle");
    QcRunner runner;
    const QcProfile profile = FastProfile();

    const QcRunResult missing = runner.AnalyzeFile((dir / "missing.mp4").string(), profile);
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("文件不存在"), std::string::npos);

    const fs::path empty = dir / "empty.mp4";
    std::ofstream(empty).close();
    const QcRunResult empty_result = runner.AnalyzeFile(empty.string(), profile);
    EXPECT_FALSE(empty_result.ok);
    EXPECT_NE(empty_result.error.find("文件为空"), std::string::npos);

    const QcRunResult directory = runner.AnalyzeFile(dir.string(), profile);
    EXPECT_FALSE(directory.ok);
    EXPECT_NE(directory.error.find("目录"), std::string::npos);

    // IsAnalyzableFile 是批量发现的前置闸门，直接断言它的边界行为。
    std::string reason;
    EXPECT_FALSE(IsAnalyzableFile("", reason));
    EXPECT_FALSE(reason.empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 正常结算路径（预算充足）：引擎自己失败 → 回调交回结果 → join 收线程，
// 不出现"预算内没有结束"。这条用例保证超时分支之外的主流程没被等待循环改坏。
TEST(QcRunnerTest, ReturnsEngineFailureWithinBudgetWithoutTimeout) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_within_budget");
    const fs::path file = WriteGarbageMediaFile(dir, "garbage.mp4");

    QcRunner runner;
    const QcRunResult result = runner.AnalyzeFile(file.string(), FastProfile(), OptionsForDepth(QcAnalysisDepth::Fast),
                                                  QcRunCallbacks{}, QcRunBudgets::WithAnalysisBudget(8000));
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error, kOverBudgetError); // 走的是结算路径，不是超时
    EXPECT_FALSE(result.timed_out);            // 预算内结算不得误标超时
    EXPECT_FALSE(result.error.empty());        // 失败原因来自引擎（FFmpeg 的打开错误）
    EXPECT_EQ(result.profile_id, "unit-test");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- 评审要求的核心回归：极短运行时长预算 -> 判定放弃 + detach ----
//
// 断言之外的关键：函数返回后测试不立刻结束，继续睡 ~1.5s —— 被 detach 的 worker
// 仍在跑（盒子由它自己保活）。在"引擎挂局部 shared_ptr"的旧实现下，这段时间里
// worker 访问 box->engine 就是 use-after-free，ASan 构建必报；现在的实现应当安静。
TEST(QcRunnerTest, TinyAnalysisBudgetAbandonsAndDetachesWithoutCrash) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_timeout");
    const fs::path file = WriteGarbageMediaFile(dir, "garbage.mp4");

    QcRunner runner;
    const auto started = std::chrono::steady_clock::now();
    // 运行 1ms 就判超预算，且取消等待 0ms —— 判定与放弃都在毫秒级发生。
    const QcRunResult result = runner.AnalyzeFile(file.string(), FastProfile(), OptionsForDepth(QcAnalysisDepth::Fast),
                                                  QcRunCallbacks{}, QcRunBudgets{1, 0, 0});
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, kOverBudgetError);
    // 放弃必须被标记出来（评审 P1-3）：调用方据此区分"引擎自己报失败"与"执行器放弃了它"，
    // 也说明这次返回时后台线程已被 detach 并收到强制取消请求。
    EXPECT_TRUE(result.timed_out);
    // 预算 1ms：引擎打开 4MiB 垃圾文件也要几十毫秒，主线程不能等它。
    EXPECT_LT(wall_ms, 1000.0);

    // 并发额度（评审 P2-3）：被放弃的线程仍占着额度 —— 它还没退出，FFmpeg / 内存 /
    // 磁盘都还被它拿着。派发侧因此能统计"实际存活的分析任务"，而不是"已返回的条目数"。
    EXPECT_GE(AnalysisTaskRegistry::Instance().TotalCount(), 1);

    // ASan 回归窗口：给被 detach 的 worker 足够时间跑完 Run 并触发回调收尾。
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    // 线程体真正退出之后额度必须归还 —— 不归还的话批量侧会永久背压。
    EXPECT_EQ(AnalysisTaskRegistry::Instance().TotalCount(), 0);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- 评审 P1-1 的核心回归：正常运行超过 5 秒**不该**被取消 ----
//
// 以前报告页把 kDefaultRecycleBudgetMs(5000) 当成"分析能跑多久"传进来，于是任何跑过
// 5 秒的合法分析（长视频 / 多音轨 / 深度档）都会被请求取消并按超时收尾。
// 现在"允许跑多久"是独立的 analysis_budget_ms，默认 0 = 不限时：跑了 10 分钟只要
// 还在正常跑（或被明确允许）就继续等。这条用例用纯策略函数钉住，不依赖真实媒体。
TEST(QcRunnerTest, LongRunningAnalysisWithoutBudgetKeepsWaiting) {
    // 报告页单文件分析用的策略：不限时（关停走取消令牌 + cancel_wait_ms）
    const QcRunBudgets budgets = SingleAnalysisBudgets();
    EXPECT_EQ(budgets.analysis_budget_ms, 0) << "单文件分析不该带运行时长上限";

    QcWaitState running;
    running.elapsed_ms = 6000; // 已经跑了 6 秒 —— 换旧实现这里就已经被判超时了
    running.idle_ms = 20;      // 而且一直在出进度，不是卡住
    EXPECT_EQ(DecideWait(budgets, running), QcWaitDecision::Continue);

    running.elapsed_ms = 10 * 60 * 1000; // 10 分钟也一样
    running.idle_ms = 50;
    EXPECT_EQ(DecideWait(budgets, running), QcWaitDecision::Continue);

    // 给了上限才会在到点时判定放弃
    const QcRunBudgets limited = QcRunBudgets::WithAnalysisBudget(5000);
    EXPECT_EQ(DecideWait(limited, running), QcWaitDecision::RequestCancel);

    // 判完之后只看 cancel_wait_ms：没到就继续等它自己收尾，到了才放弃线程
    QcWaitState abandoning;
    abandoning.reason = QcAbandonReason::OverBudget;
    abandoning.abandon_elapsed_ms = 100;
    EXPECT_EQ(DecideWait(budgets, abandoning), QcWaitDecision::Continue);
    abandoning.abandon_elapsed_ms = budgets.cancel_wait_ms;
    EXPECT_EQ(DecideWait(budgets, abandoning), QcWaitDecision::Abandon);
}

// IO 空闲超时：与总时长无关 —— "跑了很久但一直在出结果"不算卡住，
// "开跑之后一点动静都没有"才放弃。
TEST(QcRunnerTest, IdleTimeoutAbandonsStalledAnalysis) {
    const QcRunBudgets budgets{0, 1000, 0}; // 不限总时长，静默 1 秒即判定无响应

    QcWaitState busy_but_slow;
    busy_but_slow.elapsed_ms = 9 * 60 * 1000; // 跑了 9 分钟
    busy_but_slow.idle_ms = 100;              // 但 100ms 前刚出过进度
    EXPECT_EQ(DecideWait(budgets, busy_but_slow), QcWaitDecision::Continue);

    QcWaitState stalled;
    stalled.elapsed_ms = 1200;
    stalled.idle_ms = 1200; // 开跑之后毫无动静
    EXPECT_EQ(DecideWait(budgets, stalled), QcWaitDecision::RequestCancel);
}

// 端到端：不限时的正常分析不会因为"跑得久"被误标超时（引擎自己失败就是失败）。
TEST(QcRunnerTest, UnlimitedBudgetNeverMarksTimeout) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_unlimited");
    const fs::path file = WriteGarbageMediaFile(dir, "garbage.mp4");

    QcRunner runner;
    const QcRunResult result = runner.AnalyzeFile(file.string(), FastProfile(), OptionsForDepth(QcAnalysisDepth::Fast),
                                                  QcRunCallbacks{}, QcRunBudgets::Unlimited());
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.timed_out) << "不限时就不该有任何放弃判定";
    EXPECT_NE(result.error, kOverBudgetError);
    EXPECT_NE(result.error, kIoStalledError);
    EXPECT_FALSE(result.error.empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 调用方取消：放弃线程但**不**标 timed_out（批量侧按文案落成 Cancelled，
// 标了就会把"用户点了停止"显示成"文件有问题"）。
TEST(QcRunnerTest, CallerCancelAbandonsThreadWithoutTimeoutFlag) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_cancel");
    const fs::path file = WriteGarbageMediaFile(dir, "garbage.mp4");

    QcRunner runner;
    QcRunCallbacks callbacks;
    callbacks.should_cancel = []() { return true; };
    const QcRunResult result = runner.AnalyzeFile(file.string(), FastProfile(), OptionsForDepth(QcAnalysisDepth::Fast),
                                                  callbacks, QcRunBudgets{0, 0, 0});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, kCancelledError);
    EXPECT_FALSE(result.timed_out) << "调用方取消不该被记成超时";

    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 给被放弃的线程一点收尾时间
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// MakeAnalyzeFunction 的封装只做参数绑定，错误路径同样在起线程前返回。
TEST(QcRunnerTest, MakeAnalyzeFunctionKeepsPathErrors) {
    const auto analyze = QcRunner::MakeAnalyzeFunction(FastProfile(), OptionsForDepth(QcAnalysisDepth::Fast));
    QcAnalyzeRequest request;
    request.path = (fs::temp_directory_path() / "videoeye_qc_runner_absent.mp4").string();
    std::error_code ec;
    fs::remove(request.path, ec);

    const QcRunResult result = analyze(request);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("文件不存在"), std::string::npos);
    EXPECT_EQ(result.profile_id, "unit-test");
}

// ---- 统一输入类型：网络源 / 本地文件的归类（评审 P2-7）----
//
// 归类必须是纯字符串判定：不联网、不判存在性，否则这条用例就会依赖网络。
TEST(QcRunnerTest, ClassifiesNetworkUrisAndLocalPaths) {
    EXPECT_EQ(ClassifyMediaInput("http://host/a.mp4").kind, MediaInput::Kind::NetworkUri);
    EXPECT_EQ(ClassifyMediaInput("https://host/a.m3u8").kind, MediaInput::Kind::NetworkUri);
    EXPECT_EQ(ClassifyMediaInput("rtmp://host/live").kind, MediaInput::Kind::NetworkUri);
    EXPECT_EQ(ClassifyMediaInput("rtsp://host/stream").kind, MediaInput::Kind::NetworkUri);
    EXPECT_EQ(ClassifyMediaInput("rtsps://host/stream").kind, MediaInput::Kind::NetworkUri);
    EXPECT_EQ(ClassifyMediaInput("udp://239.0.0.1:1234").kind, MediaInput::Kind::NetworkUri);
    EXPECT_EQ(ClassifyMediaInput("srt://host:9000").kind, MediaInput::Kind::NetworkUri);
    // scheme 大小写不敏感
    EXPECT_EQ(ClassifyMediaInput("RTMP://host/live").kind, MediaInput::Kind::NetworkUri);

    EXPECT_EQ(ClassifyMediaInput("/tmp/a.mp4").kind, MediaInput::Kind::LocalFile);
    EXPECT_EQ(ClassifyMediaInput("C:/videos/a.mp4").kind, MediaInput::Kind::LocalFile);
    EXPECT_EQ(ClassifyMediaInput("").kind, MediaInput::Kind::LocalFile);
    // file:// 指向本地文件系统，不算网络源
    EXPECT_EQ(ClassifyMediaInput("file:///tmp/a.mp4").kind, MediaInput::Kind::LocalFile);

    EXPECT_TRUE(IsNetworkUri("http://host/a.mp4"));
    EXPECT_FALSE(IsNetworkUri("/tmp/a.mp4"));

    // 归类不改写 uri 本体
    EXPECT_EQ(ClassifyMediaInput("rtmp://host/live").uri, std::string("rtmp://host/live"));
}

// 网络源的空地址在起线程前就被挡下；本地文件的三种非法形态仍照旧被拒。
TEST(QcRunnerTest, IsAnalyzableInputSplitsNetworkAndLocalGates) {
    std::string reason;

    MediaInput network;
    network.kind = MediaInput::Kind::NetworkUri;
    network.uri = "rtsp://host/stream";
    EXPECT_TRUE(IsAnalyzableInput(network, reason)) << reason;

    MediaInput empty_network;
    empty_network.kind = MediaInput::Kind::NetworkUri;
    EXPECT_FALSE(IsAnalyzableInput(empty_network, reason));
    EXPECT_FALSE(reason.empty());

    MediaInput missing_local;
    missing_local.kind = MediaInput::Kind::LocalFile;
    missing_local.uri = (fs::temp_directory_path() / "videoeye_qc_absent.mp4").string();
    std::error_code ec;
    fs::remove(missing_local.uri, ec);
    EXPECT_FALSE(IsAnalyzableInput(missing_local, reason));
    EXPECT_NE(reason.find("文件不存在"), std::string::npos);
}

// 网络源进单文件 QC：不再被本地闸门判成"文件不存在"（评审 P2-7 的核心回归）。
//
// 1 毫秒的运行时长预算让 AnalyzeFile 立刻放弃并返回，测试不依赖任何真实网络媒体；
// 关键是错误原因不再是"文件不存在" —— 修复前 rtsp/http/rtmp 一进来就是这句。
TEST(QcRunnerTest, NetworkUriIsNotRejectedAsMissingFile) {
    QcRunner runner;
    MediaInput input;
    input.kind = MediaInput::Kind::NetworkUri;
    input.uri = "rtsp://127.0.0.1:6553/videoeye-not-here";

    const QcRunResult result = runner.AnalyzeFile(input, FastProfile(), OptionsForDepth(QcAnalysisDepth::Fast),
                                                  QcRunCallbacks{}, QcRunBudgets{1, 0, 0});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.find("文件不存在"), std::string::npos)
        << "网络源被本地存在性闸门拦下了（分类没接通）: " << result.error;
}
// ==========================================================================
// 端到端：AnalysisEngine -> FillTsTransport -> AnalysisResult
//
// 容器解析器自己的正确性（真实 TS 上 PAT/PMT/PCR 算得对不对）由
// ContainerStructureAnalyzerTests 里的真实样用例覆盖。这里锁的是**另一条链路**：
// FillTsTransport 会不会为这条 TS 真的跑一趟，以及跑完有没有搬进 AnalysisResult。
//
// 以前这条链路一个用例都没有：只有契约级的规则测试（手工造 TransportStreamInfo
// 喂给规则），于是"iformat 判定错了 / 结果忘了搬"这类问题在报告上表现为
// container.ts.* 全部未命中 —— 和"这条流很干净"长得一模一样。
// ==========================================================================
TEST(QcRunnerTest, TsTransportFactsReachTheQcResult) {
    const fs::path ts = fs::path(VIDEOEYE_CONTAINER_CORPUS_DIR) / "real_h264_aac.ts";
    if (!fs::exists(ts)) {
        GTEST_SKIP() << "缺真实样本 real_h264_aac.ts，"
                        "先跑 tests/corpus/generate_real_corpus.py";
    }

    QcRunner runner;
    const QcRunResult result = runner.AnalyzeFile(ts.string(), FastProfile());
    ASSERT_TRUE(result.ok) << result.error;

    // applicable 与 analyzed 都为真，才代表"这条 TS 真的做过传输层体检"。
    // 任一为假时，container.ts.* 的未命中只是"没有证据"，不等于"没问题" ——
    // 报告层正是靠 applicable 把这两种情况分开的（见 A3 的 ts_transport 区块）。
    const auto& t = result.analysis.ts_transport;
    EXPECT_TRUE(t.applicable);
    EXPECT_TRUE(t.analyzed);
    EXPECT_EQ(t.packet_size, 188);
    EXPECT_GT(t.scanned_packets, 0);
    EXPECT_EQ(t.scanned_packets, t.estimated_packets);
    EXPECT_TRUE(t.scan_complete);
    EXPECT_TRUE(t.has_pat);
    EXPECT_TRUE(t.has_pmt);
    EXPECT_EQ(t.programs, 1);
    EXPECT_EQ(t.streams, 2);
    EXPECT_EQ(t.sync_loss, 0);
}

// 非 TS 输入：applicable 必须为假，报告里不该冒出"MPEG-TS 传输层"一节。
TEST(QcRunnerTest, TsTransportStaysInapplicableForNonTsInput) {
    const fs::path avi = fs::path(VIDEOEYE_CONTAINER_CORPUS_DIR) / "real_mpeg4_mp3.avi";
    if (!fs::exists(avi)) {
        GTEST_SKIP() << "缺真实样本 real_mpeg4_mp3.avi，"
                        "先跑 tests/corpus/generate_real_corpus.py";
    }

    QcRunner runner;
    const QcRunResult result = runner.AnalyzeFile(avi.string(), FastProfile());
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.analysis.ts_transport.applicable);
    EXPECT_FALSE(result.analysis.ts_transport.analyzed);
}

// ==========================================================================
// 端到端：AnalysisEngine -> FillLegacyContainerFacts -> container_diagnostics.mkv
//
// MKV 的六条 container.mkv.* 规则靠 AnalysisResult::container_diagnostics.mkv 供料，
// 而这份事实由 AnalysisInputSession 里那趟"容器体检"填充。这条链路以前**根本没有
// MKV 分支**（只接了 avi/flv/asf/ogg），于是规则全部静默 —— 报告上看不出区别，
// 因为"没命中"和"没检查"长得一模一样。契约级用例（手工造 MkvDiagnostics）也照不出
// 这个洞：它们绕过输入会话直接喂事实。
//
// 所以这里必须端到端跑一遍真实 MKV，断言的落点是"事实到了 AnalysisResult 里"。
// ==========================================================================
TEST(QcRunnerTest, MkvContainerFactsReachTheQcResult) {
    const fs::path mkv = fs::path(VIDEOEYE_CONTAINER_CORPUS_DIR) / "real_h264_aac.mkv";
    if (!fs::exists(mkv)) {
        GTEST_SKIP() << "缺真实样本 real_h264_aac.mkv，"
                        "先跑 tests/corpus/generate_real_corpus.py";
    }

    QcRunner runner;
    const QcRunResult result = runner.AnalyzeFile(mkv.string(), FastProfile());
    ASSERT_TRUE(result.ok) << result.error;

    const videoeye::model::MkvDiagnostics& mkv_diag = result.analysis.container_diagnostics.mkv;
    ASSERT_TRUE(mkv_diag.analyzed) << "MKV 容器体检没跑到 —— container.mkv.* 六条规则会集体静默";
    EXPECT_EQ("matroska", mkv_diag.doc_type);
    EXPECT_TRUE(mkv_diag.scan_complete);
    EXPECT_FALSE(mkv_diag.truncated);
    EXPECT_GT(mkv_diag.clusters, 0u);
    EXPECT_EQ(mkv_diag.clusters, mkv_diag.clusters_with_timecode) << "ffmpeg 写的每个 Cluster 都带 Timecode";
    EXPECT_EQ(0u, mkv_diag.cluster_timestamp_backwards);
    // ffmpeg 默认把 Cues 写在片尾：正好验证"扫到片尾"这件事，也让 missing_cues 有得判
    EXPECT_TRUE(mkv_diag.has_cues);
    // AVC / AAC 都必须带 CodecPrivate，缺了就是黑屏/无声
    EXPECT_EQ(0u, mkv_diag.tracks_missing_codec_private) << mkv_diag.missing_codec_private_codecs;

    // 规则层不该在这份干净样本上误报
    for (const auto& issue : result.report.issues) {
        EXPECT_NE(0u, issue.rule_id.rfind("container.mkv.", 0)) << "干净 MKV 被误判: " << issue.rule_id;
    }
}
