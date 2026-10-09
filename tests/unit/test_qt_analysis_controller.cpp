// QtAnalysisController 线程句柄回收回归测试（复查 P1-1）
//
// 根因(已修): OnWorkerExited 在"没有排队请求"时直接 return，**没有 join 那个已经结束的
// 线程**，于是 worker_ 一直保持 joinable。后续 StartAnalysis 看到 joinable 就认定
// "旧线程还活着"、把本次请求放进 pending_ 等 WorkerExited 来叫醒 —— 而那条通知早已被
// 消费掉了，不会再来第二次。结果就是「第一次分析成功、第二次永远卡在排队」，
// 界面上表现为诊断页只能成功分析一次（IsRunning() 永远为 true）。
//
// 修复: 用 worker_finished_ 把「句柄可回收」和「线程真的还在跑」分开 ——
// StartAnalysis 对已结束的线程就地回收并直接启动；OnWorkerExited 也无条件回收句柄。
//
// 这里用极小的 HLS 清单做输入：清单解析走纯 stdlib 路径（不经 FFmpeg 解码），
// 快且确定，但完整穿过"工作线程 -> 引擎 -> 回调 -> 排队投递回 UI 线程"整条链 ——
// 正是缺陷所在的那条链。

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#include "core/qt/QtAnalysisController.h"

#include "BlockingTcpEndpoint.h"

namespace {

using namespace videoeye;

// 落一份极小的 media playlist。name 只用来区分两次分析（内容相同也不影响判定，
// 判定看的是 generation）。
QString WriteManifest(const QString& name) {
    const QString path = QDir::tempPath() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    f.write("#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-TARGETDURATION:4\n"
            "#EXT-X-MEDIA-SEQUENCE:0\n"
            "#EXTINF:4.0,\n"
            "seg0.ts\n"
            "#EXTINF:4.0,\n"
            "seg1.ts\n"
            "#EXT-X-ENDLIST\n");
    f.close();
    return path;
}

// 收集所有**终态**（成功或失败都算）的 generation。
// 只要一次分析真正启动过，它必然产生且只产生一个终态。
struct TerminalSink {
    std::vector<quint64> generations;

    void Record(quint64 gen) { generations.push_back(gen); }
    bool Saw(quint64 gen) const {
        return std::find(generations.begin(), generations.end(), gen) != generations.end();
    }
    int Count() const { return static_cast<int>(generations.size()); }
};

void WireSink(videoeye::qt::QtAnalysisController& controller, TerminalSink& sink) {
    QObject::connect(&controller, &videoeye::qt::QtAnalysisController::AnalysisFinished,
                     &controller,
                     [&sink](quint64 gen, bool, const model::AnalysisResult&) { sink.Record(gen); });
    QObject::connect(&controller, &videoeye::qt::QtAnalysisController::AnalysisFailed,
                     &controller, [&sink](quint64 gen, const QString&) { sink.Record(gen); });
}

// 泵事件直到谓词成立或超时。
bool PumpUntil(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

// 单纯把事件队列泵空一段时间（用来确保"排队的 WorkerExited 一定已经被消费"）。
void PumpFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
}

// QCoreApplication 会长期持有 argv 指针，所以 argc/argv 必须是静态存储 ——
// 用栈上的局部数组会在函数返回后留下悬垂指针。
void EnsureApp() {
    static int argc = 1;
    static char arg0[] = "test_qt_analysis_controller";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
}

} // namespace

// 连续三次分析：每一次都必须真的启动。
// 原缺陷下第一次之后就卡住了 —— 出错的不是"某一次"，而是"第一次之后的每一次"。
TEST(QtAnalysisControllerLifecycle, ConsecutiveAnalysesAllStart) {
    EnsureApp();

    const QString manifest = WriteManifest("videoeye_qtac_manifest.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    videoeye::qt::QtAnalysisController controller;
    TerminalSink sink;
    WireSink(controller, sink);

    quint64 previous = 0;
    for (int round = 1; round <= 3; ++round) {
        const quint64 gen = controller.StartAnalysis(manifest.toStdString());
        EXPECT_GT(gen, previous) << "generation 必须单调递增";
        previous = gen;

        ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen); }, 15000))
            << "第 " << round << " 次分析没有在超时前回包（gen=" << gen << "）";
        // 终态之后必须回到 Idle：原缺陷会留下 pending_ 让 IsRunning() 永远为 true。
        ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000))
            << "第 " << round << " 次分析结束后 IsRunning() 仍为 true";
        // 关键前置条件：把排队的 WorkerExited 彻底消费掉。
        // 原实现此刻会带着一个 joinable 的"已结束句柄"返回，下一次 StartAnalysis
        // 就会误判成旧线程还活着而永久排队。
        PumpFor(120);
    }

    EXPECT_EQ(sink.Count(), 3);
    QFile::remove(manifest);
}

// 取消后重新分析 —— 两条路径都要能起得来：
//   1) 旧线程**还在跑**时取消并重启 -> 本次请求排队, 等旧线程的 WorkerExited 到了续跑；
//   2) 空闲态（上一轮已结束、通知也已消费）取消并重启 -> 句柄就地回收后直接启动。
// 第 (2) 条正是原缺陷的现场: 少了它, 用户"取消/失败后再点一次开始分析"就永远没反应。
TEST(QtAnalysisControllerLifecycle, RestartAfterCancelStillStarts) {
    EnsureApp();

    const QString manifest = WriteManifest("videoeye_qtac_cancel.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    videoeye::qt::QtAnalysisController controller;
    TerminalSink sink;
    WireSink(controller, sink);

    // --- (1) 飞行中取消 + 立刻重启 ---
    controller.StartAnalysis(manifest.toStdString());
    controller.Cancel();
    const quint64 gen_active = controller.StartAnalysis(manifest.toStdString());
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen_active); }, 15000))
        << "旧线程还在跑时重启的那次分析必须真的启动（gen=" << gen_active << "）";
    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000));
    PumpFor(120);   // 消费掉可能还排队的 WorkerExited

    // --- (2) 空闲态取消 + 重启 ---
    controller.Cancel();   // 没有 pending 时是空操作, 但会顺手置引擎取消标志
    const quint64 gen_idle = controller.StartAnalysis(manifest.toStdString());
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen_idle); }, 15000))
        << "空闲态下重启的分析必须真的启动（gen=" << gen_idle
        << "；原缺陷: 已结束的句柄仍 joinable -> 永久排队）";
    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000))
        << "重新分析结束后 IsRunning() 仍为 true";
    // 迟到的旧代际回包不能顶替新代际：最后一次终态必须属于 gen_idle。
    EXPECT_EQ(sink.generations.back(), gen_idle);

    QFile::remove(manifest);
}

namespace {

// 损坏输入：打开 / 探测必然走到失败分支（而不是"扫描完但结果为空"）。
// 用 8KB 伪随机字节，避免全 0 被某些探测当成空文件提前短路。
QString WriteCorruptInput(const QString& name) {
    const QString path = QDir::tempPath() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    QByteArray junk(8192, '\0');
    for (int i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 73 + 11) & 0xFF);
    f.write(junk);
    f.close();
    return path;
}

} // namespace

// 失败输入必须产生终态，且**不留下粘连状态** —— 失败之后立刻能重新开始一次分析。
// 覆盖 2.1「失败后可以重新开始」与 2.3「任务最终一定进入终态」。
TEST(QtAnalysisControllerLifecycle, FailedInputReachesTerminalAndAllowsRestart) {
    EnsureApp();

    const QString corrupt = WriteCorruptInput("videoeye_qtac_corrupt.mp4");
    ASSERT_FALSE(corrupt.isEmpty());
    const QString manifest = WriteManifest("videoeye_qtac_after_fail.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    videoeye::qt::QtAnalysisController controller;
    TerminalSink sink;
    WireSink(controller, sink);

    // 第一次：损坏输入 -> 必须在预算内落到终态（成功或失败都算），且回到 Idle。
    const quint64 gen_bad = controller.StartAnalysis(corrupt.toStdString());
    ASSERT_GT(gen_bad, 0u);
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen_bad); }, 15000))
        << "损坏输入没有在超时前产生终态（gen=" << gen_bad << "）";
    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000))
        << "失败后 IsRunning() 仍为 true，界面会永久停在分析中";
    PumpFor(120);  // 消费掉排队的 WorkerExited

    // 第二次：失败之后仍能重新开始，且这次的分析真的启动并成功收尾。
    const quint64 gen_good = controller.StartAnalysis(manifest.toStdString());
    ASSERT_GT(gen_good, gen_bad) << "重新开始的代际必须仍是单调递增的";
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen_good); }, 15000))
        << "失败后重新开始的分析没有启动（gen=" << gen_good << "）";
    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000));
    EXPECT_EQ(sink.generations.back(), gen_good) << "失败后重启的终态必须属于新代际";

    QFile::remove(corrupt);
    QFile::remove(manifest);
}

// 阻塞网络输入下的取消：Stop/Cancel 必须在预算内回到终态，绝不永久停在"分析中"。
//
// 输入用"只监听、从不回包"的本地 TCP 端口 —— 引擎打开 / 探测阶段会卡在 recv 上，
// 正是"取消链必须能打断阻塞 IO"那条契约唯一可离线复现的形态。
TEST(QtAnalysisControllerLifecycle, CancelOnBlockingInputReturnsWithinBudget) {
    EnsureApp();

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    videoeye::qt::QtAnalysisController controller;
    TerminalSink sink;
    WireSink(controller, sink);

    const quint64 gen = controller.StartAnalysis(endpoint.tcp_url());
    ASSERT_GT(gen, 0u);

    // 等它真的进入飞行态。若输入瞬间就回包（端口未按预期阻塞），说明本用例没测到
    // "取消打断阻塞 IO"，跳过而不是把判定放宽成"随便什么时候回来都算过"。
    ASSERT_TRUE(PumpUntil([&] { return controller.IsRunning(); }, 3000))
        << "分析没有进入运行态，无法验证取消路径";
    if (PumpUntil([&] { return sink.Saw(gen); }, 200)) {
        GTEST_SKIP() << "阻塞输入瞬间就回包了（端口未真正阻塞），本轮不算测到取消路径";
    }

    const auto t0 = std::chrono::steady_clock::now();
    controller.Cancel();
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen); }, 8000))
        << "取消后没有在预算内回到终态（gen=" << gen << "）";
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    EXPECT_LT(dt, 5000) << "取消到终态耗时 " << dt << "ms，接近/超过预算"
                        << "（取消链没打断阻塞的 avformat_open_input）";
    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000))
        << "取消后 IsRunning() 仍为 true —— 界面会永久停在'分析中'";
    EXPECT_EQ(sink.generations.back(), gen);
}

namespace {

QString WriteTextFile(const QString& name, const QByteArray& content) {
    const QString path = QDir::tempPath() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    f.write(content);
    f.close();
    return path;
}

} // namespace

// 残缺清单也必须收敛到终态（2.3「返回不完整的 HLS/DASH 清单」）。
//
// 清单解析走纯 stdlib 路径、绝不发网络请求 —— 这里守住的不是"解析出什么"，而是
// "解析器面对残缺输入也必须返回"：任何一条没有终态的路径都会让界面永久停在"分析中"。
TEST(QtAnalysisControllerLifecycle, MalformedManifestsReachTerminal) {
    EnsureApp();

    const struct {
        const char* name;
        QByteArray content;
    } cases[] = {
        // HLS: 有版本/分片时长头，但没有任何分片、也没有 ENDLIST。
        {"videoeye_qtac_truncated.m3u8", QByteArray("#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.0,\n")},
        // DASH: XML 在根元素中途被截断。
        {"videoeye_qtac_truncated.mpd",
         QByteArray("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<MPD mediaPresentationDuration=")},
    };

    for (const auto& c : cases) {
        const QString path = WriteTextFile(QString::fromLatin1(c.name), c.content);
        ASSERT_FALSE(path.isEmpty());

        videoeye::qt::QtAnalysisController controller;
        TerminalSink sink;
        WireSink(controller, sink);

        const quint64 gen = controller.StartAnalysis(path.toStdString());
        ASSERT_GT(gen, 0u);
        ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen); }, 15000))
            << "残缺清单 " << c.name << " 没有在超时前产生终态（界面会永久停在'分析中'）";
        ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000))
            << "残缺清单 " << c.name << " 结束后 IsRunning() 仍为 true";
        EXPECT_EQ(sink.generations.back(), gen);

        PumpFor(50);
        QFile::remove(path);
    }
}

