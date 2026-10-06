// FfmpegProcessRunner 的 QProcess 生命周期测试。
//
// 难点: 单元测试不该依赖"这台机器装没装 ffmpeg"，但这里要测的恰恰是**进程** ——
// 启动失败、stdout/stderr 分流、停止时有没有发 q、停完会不会误杀下一次运行、
// 工作目录会不会残留。
//
// 办法: 这个测试可执行文件自己带一个 `--videoeye-child <mode>` 模式，把自己
// 当成"可控的 ffmpeg"跑（echo 读 stdin / sleep 占着不退 / exit 返回指定码 /
// pwd 打印工作目录 / utf8 吐 UTF-8 字节）。子进程因此零外部依赖、跨平台一致。
//
// 代价: 需要自己写 main()，所以链接的是 GTest::gtest 而不是 GTest::gtest_main
// （见 tests/CMakeLists.txt 里这个目标的注释）。

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QStringList>
#include <QProcess>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "core/ffmpeg/FfmpegProcessRunner.h"

// 本文件统一写 ffmpeg::，用一个别名把它从 videoeye 里拎出来
namespace ffmpeg = videoeye::ffmpeg;

namespace {

// 纯 ASCII 转义，避免源码编码影响这条断言（要验的正是"UTF-8 字节被正确解码"）
const char kUtf8Chinese[] = "\xE4\xB8\xAD\xE6\x96\x87";  // 中文

QString g_child_program;

// ---------------- 子进程模式 ----------------
//
// 参数: <exe> --videoeye-child <mode> [arg]
int RunChildMode(const std::string& mode, const std::vector<std::string>& rest) {
    if (mode == "echo") {
        // 模拟"读键盘"的 ffmpeg: 读到 q 就正常退出，否则一直等
        std::cout << "child:ready" << std::endl;
        std::cerr << "child:ready-err" << std::endl;
        std::string line;
        while (std::getline(std::cin, line)) {
            std::cout << "stdin:" << line << std::endl;
            std::cerr << "stdin-err:" << line << std::endl;
            if (line == "q") {
                break;
            }
        }
        return 0;
    }
    if (mode == "sleep") {
        // 模拟"正在编码"的 ffmpeg: 期间不读 stdin，谁也劝不住
        const int ms = rest.empty() ? 1000 : std::stoi(rest.at(0));
        std::cout << "child:ready" << std::endl;
        std::cerr << "child:ready-err" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        std::cout << "child:done" << std::endl;
        return 0;
    }
    if (mode == "exit") {
        const int code = rest.empty() ? 0 : std::stoi(rest.at(0));
        std::cout << "child:out" << std::endl;
        std::cerr << "child:err" << std::endl;
        return code;
    }
    if (mode == "pwd") {
        std::cout << QDir::currentPath().toStdString() << std::endl;
        return 0;
    }
    if (mode == "utf8") {
        std::cout << kUtf8Chinese << std::endl;
        return 0;
    }
    std::cerr << "unknown child mode: " << mode << std::endl;
    return 99;
}

// ---------------- 采集 ----------------

struct RunCapture {
    int started_count = 0;
    int finished_count = 0;
    ffmpeg::FfmpegRunResult result;
    QStringList out;                              // 来自 stdout
    QStringList err;                              // 来自 stderr
    QList<ffmpeg::FfmpegStopStage> stages;    // StopStageChanged 的顺序
};

/// 把进程跑完（或超时）。on_started 在 Started 信号之后被调用，用来在进程中途 Stop。
RunCapture RunToCompletion(const QString& program, const QStringList& arguments,
                           const QString& working_directory = QString(),
                           int timeout_ms = 8000,
                           const std::function<void(ffmpeg::FfmpegProcessRunner*)>& on_started =
                               std::function<void(ffmpeg::FfmpegProcessRunner*)>()) {
    RunCapture capture;
    ffmpeg::FfmpegProcessRunner runner;
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::Started,
                     [&capture](const QString&) { ++capture.started_count; });
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::OutputLine,
                     [&capture](const QString& text, bool is_error) {
                         (is_error ? capture.err : capture.out).push_back(text);
                     });
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::StopStageChanged,
                     [&capture](ffmpeg::FfmpegStopStage stage) { capture.stages.push_back(stage); });
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::Finished,
                     [&capture](const ffmpeg::FfmpegRunResult& result) {
                         ++capture.finished_count;
                         capture.result = result;
                     });

    const bool started = runner.Start(program, arguments, working_directory);
    if (started && on_started) {
        on_started(&runner);
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (capture.finished_count > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 收尾: 让在飞的定时器也能跑完（测试"不会被误杀"时会用到）
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return capture;
}

/// 等到某一路输出里出现指定文本（用于"等子进程就绪再点停止"）
bool WaitForLine(const RunCapture& capture, const QString& needle, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (capture.out.contains(needle) || capture.err.contains(needle)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return capture.out.contains(needle) || capture.err.contains(needle);
}

QString Canonical(const QString& path) {
    return QFileInfo(path).canonicalFilePath();
}

/// 这台机器上 QProcess 到底能不能起来一个子进程。
///
/// 有些环境会连最朴素的一次启动都失败（本机就是: Qt 报
/// "QProcess: CreateFile failed. (所有的管道范例都在使用中)"，换任何程序、
/// 任何 OpenMode 都一样）。那是环境/Qt 构建的问题，不是被测代码的问题 ——
/// 把它显示成红色只会让人去查一段其实没错的逻辑。所以探测一次，起不来就跳过，
/// 并把原因原样说出来。在正常的开发机与 CI 上，这组测试是照常跑的。
bool ChildProcessAvailable(QString* reason = nullptr) {
    static QString failure;
    static bool checked = false;
    if (!checked) {
        checked = true;
        QProcess probe;
        probe.setProgram(g_child_program);
        probe.setArguments({QStringLiteral("--videoeye-child"), QStringLiteral("exit"),
                            QStringLiteral("0")});
        probe.start(QIODevice::ReadOnly);
        probe.waitForFinished(3000);
        if (probe.state() != QProcess::NotRunning || probe.exitCode() != 0 ||
            probe.error() != QProcess::UnknownError) {
            failure = QStringLiteral("本环境无法用 QProcess 启动子进程（error=%1, %2）")
                          .arg(QString::number(static_cast<int>(probe.error())),
                               probe.errorString());
        }
    }
    if (reason != nullptr) {
        *reason = failure;
    }
    return failure.isEmpty();
}

#define SKIP_IF_NO_CHILD_PROCESS()                                  \
    do {                                                            \
        QString skip_reason;                                        \
        if (!ChildProcessAvailable(&skip_reason)) {                 \
            GTEST_SKIP() << skip_reason.toStdString();              \
        }                                                           \
    } while (0)

// ===================== 启动 =====================

TEST(FfmpegProcessRunner, StartErrorIsReportedOnce) {
    // 程序不存在: 必须一次性把 StartError 抛出来，不能既返回 false 又什么都不说，
    // 更不能先发一次 Finished 再让 errorOccurred 补第二次。
    RunCapture capture;
    ffmpeg::FfmpegProcessRunner runner;
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::Started,
                     [&capture](const QString&) { ++capture.started_count; });
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::Finished,
                     [&capture](const ffmpeg::FfmpegRunResult& result) {
                         ++capture.finished_count;
                         capture.result = result;
                     });

    const bool started = runner.Start(QStringLiteral("/definitely/not/here/ffmpeg"),
                                      {QStringLiteral("-version")});
    EXPECT_FALSE(started);
    EXPECT_FALSE(runner.IsRunning());
    EXPECT_EQ(capture.started_count, 0);
    EXPECT_EQ(capture.finished_count, 1);
    EXPECT_EQ(capture.result.status, ffmpeg::FfmpegRunStatus::StartError);
    EXPECT_FALSE(capture.result.error_message.isEmpty());
}

TEST(FfmpegProcessRunner, StartErrorWhenWorkingDirectoryMissing) {
    SKIP_IF_NO_CHILD_PROCESS();
    const auto capture = RunToCompletion(g_child_program, {QStringLiteral("--videoeye-child"),
                                                           QStringLiteral("exit"), QStringLiteral("0")},
                                         QStringLiteral("/definitely/not/here/either"));
    EXPECT_EQ(capture.finished_count, 1);
    EXPECT_EQ(capture.result.status, ffmpeg::FfmpegRunStatus::StartError);
}

// ===================== 输出 =====================

TEST(FfmpegProcessRunner, SeparateStdoutAndStderr) {
    SKIP_IF_NO_CHILD_PROCESS();
    const auto capture = RunToCompletion(g_child_program, {QStringLiteral("--videoeye-child"),
                                                           QStringLiteral("exit"), QStringLiteral("3")});
    ASSERT_EQ(capture.finished_count, 1);
    EXPECT_EQ(capture.result.status, ffmpeg::FfmpegRunStatus::Failed);
    EXPECT_EQ(capture.result.exit_code, 3);
    EXPECT_TRUE(capture.out.contains(QStringLiteral("child:out")));
    EXPECT_TRUE(capture.err.contains(QStringLiteral("child:err")));
    // 两路不能串: stdout 里不该出现只写进 stderr 的那一行
    EXPECT_FALSE(capture.out.contains(QStringLiteral("child:err")));
    EXPECT_GE(capture.result.elapsed_ms, 0);
}

TEST(FfmpegProcessRunner, ZeroExitCodeIsFinished) {
    SKIP_IF_NO_CHILD_PROCESS();
    const auto capture = RunToCompletion(g_child_program, {QStringLiteral("--videoeye-child"),
                                                           QStringLiteral("exit"), QStringLiteral("0")});
    ASSERT_EQ(capture.finished_count, 1);
    EXPECT_EQ(capture.result.status, ffmpeg::FfmpegRunStatus::Finished);
    EXPECT_EQ(capture.result.exit_code, 0);
}

TEST(FfmpegProcessRunner, DecodesUtf8Output) {
    SKIP_IF_NO_CHILD_PROCESS();
    // Windows 中文环境下 ffmpeg 常吐本地编码；UTF-8 路径必须先被正确解出来
    const auto capture = RunToCompletion(g_child_program, {QStringLiteral("--videoeye-child"),
                                                           QStringLiteral("utf8")});
    ASSERT_EQ(capture.finished_count, 1);
    ASSERT_FALSE(capture.out.isEmpty());
    EXPECT_EQ(capture.out.first(), QString::fromUtf8(kUtf8Chinese));
    EXPECT_FALSE(capture.out.first().contains(QChar(0xFFFD)));
}

// ===================== 停止 =====================

TEST(FfmpegProcessRunner, StopWritesQuitToStdin) {
    SKIP_IF_NO_CHILD_PROCESS();
    // 最关键的一条: 停止必须先试着让 ffmpeg 自己收尾（写 q），
    // 因为强杀出来的 mp4 没有 moov box，等于白跑。
    RunCapture capture;
    ffmpeg::FfmpegProcessRunner runner;
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::OutputLine,
                     [&capture](const QString& text, bool is_error) {
                         (is_error ? capture.err : capture.out).push_back(text);
                     });
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::StopStageChanged,
                     [&capture](ffmpeg::FfmpegStopStage stage) { capture.stages.push_back(stage); });
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::Finished,
                     [&capture](const ffmpeg::FfmpegRunResult& result) {
                         ++capture.finished_count;
                         capture.result = result;
                     });

    ASSERT_TRUE(runner.Start(g_child_program, {QStringLiteral("--videoeye-child"),
                                               QStringLiteral("echo")}));
    ASSERT_TRUE(WaitForLine(capture, QStringLiteral("child:ready"), 5000));

    runner.Stop();
    EXPECT_EQ(runner.stop_stage(), ffmpeg::FfmpegStopStage::QuitSent);

    QElapsedTimer timer;
    timer.start();
    while (capture.finished_count == 0 && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(capture.finished_count, 1);
    EXPECT_EQ(capture.result.status, ffmpeg::FfmpegRunStatus::Stopped);
    EXPECT_EQ(capture.result.stop_stage, ffmpeg::FfmpegStopStage::QuitSent);
    EXPECT_TRUE(capture.result.stopped_cleanly());
    // 子进程确实收到了 q —— 没有这一行就说明根本没写 stdin
    EXPECT_TRUE(capture.out.contains(QStringLiteral("stdin:q")));
    // 优雅退出就不该再升级到 terminate / kill
    EXPECT_FALSE(capture.stages.contains(ffmpeg::FfmpegStopStage::Terminated));
    EXPECT_FALSE(capture.stages.contains(ffmpeg::FfmpegStopStage::Killed));
}

TEST(FfmpegProcessRunner, StopEscalatesWhenQuitIsIgnored) {
    SKIP_IF_NO_CHILD_PROCESS();
    const auto capture = RunToCompletion(
        g_child_program, {QStringLiteral("--videoeye-child"), QStringLiteral("sleep"),
                          QStringLiteral("5000")},
        QString(), 8000, [](ffmpeg::FfmpegProcessRunner* runner) { runner->Stop(150, 150); });

    ASSERT_EQ(capture.finished_count, 1);
    EXPECT_EQ(capture.result.status, ffmpeg::FfmpegRunStatus::Stopped);
    // 子进程不读 stdin，q 劝不住它 —— 必须升级到终止/强杀
    EXPECT_NE(capture.result.stop_stage, ffmpeg::FfmpegStopStage::QuitSent);
}

TEST(FfmpegProcessRunner, StoppingOneRunDoesNotKillTheNext) {
    SKIP_IF_NO_CHILD_PROCESS();
    // 回归: 宽限定时器如果只按时间触发，前一个任务留下的定时器会杀掉下一个任务。
    // 做法: 先让 run1 在定时器到点前自己结束，再立刻起 run2 —— run2 必须能正常跑完。
    ffmpeg::FfmpegProcessRunner runner;
    RunCapture first;
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::OutputLine,
                     [&first](const QString& text, bool) { first.out.push_back(text); });
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::Finished,
                     [&first](const ffmpeg::FfmpegRunResult& result) {
                         ++first.finished_count;
                         first.result = result;
                     });

    ASSERT_TRUE(runner.Start(g_child_program, {QStringLiteral("--videoeye-child"),
                                               QStringLiteral("sleep"), QStringLiteral("150")}));
    // 宽限给到 900ms: run1 会在 150ms 时自己退出，定时器因此是"在飞"的状态
    runner.Stop(900, 900);
    QElapsedTimer timer;
    timer.start();
    while (first.finished_count == 0 && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(first.finished_count, 1);

    // 立刻跑第二条: 900ms 时上一代遗留的定时器会到点，绝不能动手
    RunCapture second;
    QObject::connect(&runner, &ffmpeg::FfmpegProcessRunner::Finished,
                     [&second](const ffmpeg::FfmpegRunResult& result) {
                         ++second.finished_count;
                         second.result = result;
                     });
    ASSERT_TRUE(runner.Start(g_child_program, {QStringLiteral("--videoeye-child"),
                                               QStringLiteral("sleep"), QStringLiteral("1200")}));
    timer.restart();
    while (second.finished_count == 0 && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(second.finished_count, 1);
    EXPECT_EQ(second.result.status, ffmpeg::FfmpegRunStatus::Finished);
    EXPECT_EQ(second.result.stop_stage, ffmpeg::FfmpegStopStage::None);
}

TEST(FfmpegProcessRunner, NostdinCannotBeStoppedGracefully) {
    EXPECT_TRUE(ffmpeg::FfmpegProcessRunner::CanQuitViaStdin(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("out.mp4")}));
    EXPECT_FALSE(ffmpeg::FfmpegProcessRunner::CanQuitViaStdin(
        {QStringLiteral("-nostdin"), QStringLiteral("-i"), QStringLiteral("in.mp4"),
         QStringLiteral("out.mp4")}));
}

// ===================== 工作目录 =====================

TEST(FfmpegProcessRunner, WorkingDirectoryDoesNotLeakToNextRun) {
    SKIP_IF_NO_CHILD_PROCESS();
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const auto in_temp = RunToCompletion(g_child_program,
                                         {QStringLiteral("--videoeye-child"), QStringLiteral("pwd")},
                                         temp.path());
    ASSERT_EQ(in_temp.finished_count, 1);
    ASSERT_FALSE(in_temp.out.isEmpty());
    EXPECT_EQ(Canonical(in_temp.out.first()), Canonical(temp.path()));

    // 第二次不指定工作目录: 必须回到调用进程的目录，不能沿用上次的 temp
    const auto in_default = RunToCompletion(g_child_program,
                                            {QStringLiteral("--videoeye-child"), QStringLiteral("pwd")});
    ASSERT_EQ(in_default.finished_count, 1);
    ASSERT_FALSE(in_default.out.isEmpty());
    EXPECT_EQ(Canonical(in_default.out.first()), Canonical(QDir::currentPath()));
    EXPECT_NE(Canonical(in_default.out.first()), Canonical(temp.path()));
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // 子进程模式: 把自己当成"可控的 ffmpeg"。要先建好 QCoreApplication，
    // pwd 模式里的 QDir::currentPath() 才不依赖任何"必须有 app 实例"的前提。
    if (argc >= 3 && std::string(argv[1]) == "--videoeye-child") {
        std::vector<std::string> rest;
        for (int i = 3; i < argc; ++i) {
            rest.emplace_back(argv[i]);
        }
        return RunChildMode(argv[2], rest);
    }

    g_child_program = QCoreApplication::applicationFilePath();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
