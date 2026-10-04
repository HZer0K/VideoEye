// 取消 / 异常 / 畸形码流 的回归测试。
//
// 这几条都是代码复查里被点名的边界：要么能让分析线程长时间占满 CPU，
// 要么能让进程直接退出，或者把"被取消"报成"失败"。单点复现成本低，
// 但一旦回归，现象往往隔很久才在真机上暴露（要在切文件/关窗口那一下）。
//
// 覆盖:
//   1. VVC 畸形 SPS 的 QP 表：num_points 取自 ue(v) 且无上界，j++ 到 UINT32_MAX
//      会回绕 -> 循环永不退出（跑进 std::async 里限时，卡死会变成失败而不是挂住 CI）；
//   2. BitReader::ReadUEBounded 必须把越界码值夹住；
//   3. IsobmffParser：取消令牌必须在 **box 循环内部** 才有效，只在入口查一次的话
//      取消后仍会把整个文件扫完；
//   4. TaskManager：被顶下来的孤儿线程要计入并发预算，否则反复替换同一 slot 能无限超发；
//   5. QtWorkerOwner：body() 抛异常时线程要正常退出，且 on_error 被调到。

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QObject>
#include <QThread>

#include "core/analysis/codec/VvcBitstreamParser.h"
#include "core/media/codec/BitReader.h"
#include "core/media/container/IsobmffParser.h"
#include "core/qt/QtWorkerOwner.h"
#include "infrastructure/concurrency/Cancellation.h"
#include "infrastructure/concurrency/TaskManager.h"

namespace {

using videoeye::analyzer::VvcBitstreamParser;
using videoeye::model::VvcSpsInfo;
using videoeye::task::TaskManager;
using videoeye::utils::BitReader;
using videoeye::utils::NalUnit;

// ---------------------------------------------------------------------------
// 取消标志的统一入口
// ---------------------------------------------------------------------------

TEST(InfrastructureCancellation, NullTokenNeverCancels) {
    EXPECT_FALSE(videoeye::infrastructure::IsCanceled(nullptr));
    EXPECT_FALSE(videoeye::infrastructure::Checkpoint(nullptr));

    std::atomic<bool> flag{false};
    EXPECT_FALSE(videoeye::infrastructure::IsCanceled(&flag));
    flag.store(true, std::memory_order_release);
    EXPECT_TRUE(videoeye::infrastructure::IsCanceled(&flag));
    EXPECT_TRUE(videoeye::infrastructure::Checkpoint(&flag));
}

namespace {

// 一个**语法完全合法**却超出上界的 ue(v)：6 个前导零 + 终止位 1 + 6 位 suffix=0
//   bit 序列: 0 0 0 0 0 0 | 1 | 0 0 0 0 0 0 ...
//   -> value = 0 + 2^6 - 1 = 63，上界取 32 必然越界。
// 刻意不用"一串 0x00"当输入：前导零多到 64 个时 ReadUE 会越过数据末尾，
// 那条路径置的 error 是"位耗尽"而不是"数值超上限"，拿来断言语义会张冠李戴。
const std::vector<uint8_t> kOutOfRangeUe = {0x02, 0x00, 0x00};
constexpr uint32_t kOutOfRangeValue = 63;
constexpr uint32_t kBoundedMax = 32;

}  // namespace

TEST(BitReaderBounded, ReadUEClampedSilentlyClampsMalformedValue) {
    // 语法合法但码值超过上限：夹断版必须把它压回 max_value，并且**不置错误**。
    // 夹断是"诊断优先"——只改数值，不因一个字段就把整份参数集判死。
    BitReader reader;
    reader.Reset(kOutOfRangeUe.data(), kOutOfRangeUe.size());
    EXPECT_EQ(kOutOfRangeValue, reader.ReadUE()) << "用例前提: 裸 ReadUE 读出来是 63";

    BitReader clamped_reader;
    clamped_reader.Reset(kOutOfRangeUe.data(), kOutOfRangeUe.size());
    EXPECT_EQ(kBoundedMax, clamped_reader.ReadUEClamped(kBoundedMax))
        << "越界码值必须被夹到上界, 不能被当成巨大循环次数";
    EXPECT_FALSE(clamped_reader.HasError()) << "夹断不该置错误: 字段读偏了不等于整份参数集不可用";
}

TEST(BitReaderBounded, ReadUEBoundedFlagsOutOfRangeValue) {
    // 与上一条对照。这是 P2 里点出来的坑：旧实现静默夹断，于是"码流非法"和
    // "码流正常但字段偏小"在结果上完全一致 —— 参数集带着 valid=true 出去，
    // 字段却已经全面错位，比直接判无效难查得多。
    BitReader reader;
    reader.Reset(kOutOfRangeUe.data(), kOutOfRangeUe.size());
    const uint32_t value = reader.ReadUEBounded(kBoundedMax);
    EXPECT_LE(value, kBoundedMax) << "越界也要保证返回值不超上界, 不能当循环次数用";
    EXPECT_TRUE(reader.HasError()) << "语法合法但数值超上限 -> 必须置错误, 让参数集判为无效";
    EXPECT_FALSE(reader.GetError().empty());
}

// ---------------------------------------------------------------------------
// 1. VVC 畸形 SPS 的 QP 表点数
// ---------------------------------------------------------------------------

TEST(VvcBitstreamParserMalformed, HugeQpTablePointCountTerminates) {
    // 一串 0xFF：任何"逐字节铺字段"的写法在这上面都会读出一个巨大的 ue(v)。
    // 解析器必须能把这个输入吐回来，而不是在 num_points 的循环里转不出来。
    const std::vector<uint8_t> nal = {
        0x01, 0x0B, 0x02, 0x33, 0x80, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x43, 0x91,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };

    // 放进 async 里跑：万一回归成死循环，这里会被 2s 判失败，而不是把 CI 挂住。
    auto task = std::async(std::launch::async, [&nal]() {
        NalUnit unit;
        unit.type = 1;  // SPS
        unit.size = static_cast<uint32_t>(nal.size());
        unit.data = nal;
        return VvcBitstreamParser::ParseSpsFromNalUnit(unit);
    });

    ASSERT_EQ(task.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "畸形 SPS（num_points 无上界 + j++ 回绕）让解析器卡住了";

    const VvcSpsInfo sps = task.get();
    (void)sps;  // 解析成什么样都行, 关键是它得停下来
}

// ---------------------------------------------------------------------------
// 3. ISOBMFF 解析器的取消传播
// ---------------------------------------------------------------------------

// 造一个"伪 mp4"：ftyp + 60 万个最小的 free box（每个 8 字节，约 4.8MB）。
//
// 为什么用**数量多、单个极小**而不是"单个巨大": top-level 的 box 扫描是
// 一次 while 循环逐个走 box 头，耗时跟 box **个数**成正比而不是文件体积。
// 之前的版本用了 8000 个 4KB 的 box，扫描十几毫秒就跑完了 —— 取消标志
// 还没来得及翻，测试就变成"什么都没验证到"的空过。
std::string MakeScanHeavyIsobmffFile() {
    constexpr int kBoxSize = 8;      // 只有 header, 无 payload
    constexpr int kBoxCount = 600000;

    const std::string path = "cancellation_scan_heavy.mp4";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return std::string();

    const uint8_t ftyp[16] = {0x00, 0x00, 0x00, 0x18, 0x69, 0x73, 0x6F, 0x6D,
                              0x00, 0x00, 0x02, 0x00, 0x69, 0x73, 0x6F, 0x6D};
    std::fwrite(ftyp, 1, sizeof(ftyp), f);

    uint8_t box[8] = {0x00, 0x00, 0x00, 0x00, 0x66, 0x72, 0x65, 0x65};  // size=0 'free'
    box[0] = static_cast<uint8_t>((kBoxSize >> 24) & 0xFF);
    box[1] = static_cast<uint8_t>((kBoxSize >> 16) & 0xFF);
    box[2] = static_cast<uint8_t>((kBoxSize >> 8) & 0xFF);
    box[3] = static_cast<uint8_t>(kBoxSize & 0xFF);
    for (int i = 0; i < kBoxCount; ++i) {
        std::fwrite(box, 1, sizeof(box), f);
    }
    std::fclose(f);
    return path;
}

TEST(IsobmffParserCancel, ScanObservesCancelInsideBoxLoop) {
    const std::string path = MakeScanHeavyIsobmffFile();
    ASSERT_FALSE(path.empty());

    std::atomic<bool> cancel{false};
    videoeye::utils::IsobmffFile out;
    videoeye::utils::IsobmffParser::Options opt;
    opt.cancel = &cancel;

    auto task = std::async(std::launch::async, [&]() {
        return videoeye::utils::IsobmffParser::Parse(path, out, opt);
    });

    // 解析中途翻标志：只要实现在 box 循环里查过取消，就一定会被打断。
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancel.store(true, std::memory_order_release);

    ASSERT_EQ(task.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_FALSE(task.get()) << "取消标志置位后 box 扫描必须立刻停手";
    EXPECT_EQ(out.error_message, "已取消") << "取消与解析失败必须能区分开";

    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 4. TaskManager 孤儿线程进并发预算
// ---------------------------------------------------------------------------

TEST(TaskManagerOrphan, OrphanThreadCountsAgainstConcurrencyBudget) {
    using videoeye::task::CancelToken;
    using videoeye::task::TaskId;
    using videoeye::task::TaskState;

    TaskManager mgr(1);  // 只允许一个任务在跑

    std::atomic<bool> release{false};
    std::atomic<bool> entered{false};

    // 任务 A 占住唯一名额, 且不响应取消（模拟卡在不可中断的调用里）
    const TaskId id_a = mgr.Run("slot", [&](TaskId, CancelToken) {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    ASSERT_NE(id_a, 0u);
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // 任务 B 顶替 A：B 会跑起来、A 转入孤儿列表。这一步**本来就该成功** ——
    // Begin 在扣掉"本 slot 自己"之后只剩 0 个占用，放行新任务。
    std::atomic<bool> second_entered{false};
    const TaskId id_b = mgr.Run("slot", [&](TaskId, CancelToken) {
        second_entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    ASSERT_NE(id_b, 0u);
    // 等到 B 真的跑起来，A 已经转入孤儿列表，此时并发占用是 2（A 孤儿 + B）
    while (!second_entered.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // 任务 C：孤儿 A 还挂在后台，它和 B 加起来已经超出 max_concurrent。
    // 孤儿不计入预算的话这里会被放行，实际线程数一路涨上去。
    std::atomic<bool> third_entered{false};
    const TaskId id_c = mgr.Run("slot", [&](TaskId, CancelToken) {
        third_entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });

    EXPECT_EQ(id_c, 0u) << "孤儿线程必须计入并发预算, 否则同一 slot 可以无限超发线程";
    EXPECT_FALSE(third_entered.load()) << "被拒绝的任务不该真的起线程";

    release.store(true, std::memory_order_release);
    mgr.WaitForAll(5000);
    EXPECT_TRUE(videoeye::task::IsTerminalState(mgr.State("slot")))
        << "收尾后 slot 必须落在终态, 不能永远停在 Running";
}

// ---------------------------------------------------------------------------
// 5. QtWorkerOwner 异常不得让线程入口逃逸
// ---------------------------------------------------------------------------

TEST(QtWorkerOwner, BodyExceptionStillQuitsThreadAndReportsError) {
    using videoeye::qt::QtWorkerOwner;

    std::atomic<bool> error_called{false};
    std::string error_message;

    int argc = 0;
    QCoreApplication app(argc, nullptr);

    QtWorkerOwner owner;
    QThread* thread = owner.StartWorker(
        new QObject(),
        [] {
            throw std::runtime_error("body 故意抛异常");
        },
        [] {},
        [](QThread*) {},
        [&](QThread*, const std::string& msg) {
            error_message = msg;
            error_called.store(true, std::memory_order_release);
        });

    ASSERT_NE(thread, nullptr);
    thread->wait(3000);

    EXPECT_FALSE(thread->isRunning()) << "body 抛异常也必须保证 thread->quit(), 否则 std::thread 入口逃逸会 terminate";
    EXPECT_TRUE(error_called.load()) << "body 抛异常应触发 on_error 失败回调";
    EXPECT_NE(error_message.find("body 故意抛异常"), std::string::npos)
        << "on_error 要带上原始异常文本, 否则界面上只会看到一句干巴巴的失败";
}

// ---------------------------------------------------------------------------
// 6. Qt worker 异常 -> 业务任务终态（MediaPlayer 导出 / 容器分析走同一条链）
//
// 这次复查找出的问题是: worker 线程崩了之后, 线程本身退出得干干净净, 可对应的
// TaskManager 任务停在 Running, 界面既收不到 ExportFinished 也收不到 ExportError
// —— 一个谁也不认的半死状态。修法是把 on_error 接到 TaskManager::End(Failed)。
// 这里把整条链走通, 并且让 on_error 自己再抛一次, 顺带压住"失败回调再抛就会
// 跳掉 quit()" 这个更隐蔽的版本。
// ---------------------------------------------------------------------------

TEST(QtWorkerOwner, ThrowingErrorCallbackStillEndsTaskAsFailed) {
    using videoeye::qt::QtWorkerOwner;
    using videoeye::task::CancelToken;
    using videoeye::task::TaskId;
    using videoeye::task::TaskState;

    int argc = 0;
    QCoreApplication app(argc, nullptr);

    TaskManager mgr(1);
    const TaskId id = mgr.Run("export", [](TaskId, CancelToken) {});
    ASSERT_NE(id, 0u);

    std::atomic<bool> entered{false};
    QtWorkerOwner owner;
    QThread* thread = owner.StartWorker(
        new QObject(),
        [&]() {
            entered.store(true, std::memory_order_release);
            throw std::runtime_error("导出任务体崩了");
        },
        []() {},
        [](QThread*) {},
        [&](QThread*, const std::string&) {
            // 与 MediaPlayer 里 on_error 的落地逻辑一致
            mgr.End("export", id, TaskState::Failed);
            // 失败回调自己再抛一次：这条路径以前会直接跳过 thread->quit()
            throw std::runtime_error("on_error 自己也抛");
        });

    ASSERT_NE(thread, nullptr);
    thread->wait(3000);

    EXPECT_TRUE(entered.load()) << "必须真的跑到任务体, 否则这条用例什么都没验证到";
    EXPECT_FALSE(thread->isRunning())
        << "on_error 自身抛异常也必须保证 thread->quit(), 否则线程挂在 exec() 上越积越多";
    mgr.WaitForAll(3000);
    EXPECT_EQ(TaskState::Failed, mgr.State("export"))
        << "worker 抛异常后任务必须落在 Failed: 停在 Running 会让界面永远等不到终态";
}

}  // namespace
