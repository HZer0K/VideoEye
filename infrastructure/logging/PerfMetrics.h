#pragma once

// 性能指标记录通道。
//
// 与 infrastructure/logging/ScopedTimer.h(VE_PERF) 的分工：
//   * VE_PERF  —— 开发期定位卡顿用的**临时埋点**，名字随手起、阈值 1ms 以上就打，
//                 看的是"这一段代码慢不慢"，属于诊断日志；
//   * 本文件   —— **长期口径**的性能指标：打开耗时 / 分析耗时 / 峰值内存 /
//                 取消响应时间 / 报告导出耗时。名字是常量（见下方 kMetric*），
//                 单位固定，带预算，超预算自动升到 Warning 级。
//
// 硬约束：这些数字**只写日志，不进普通用户界面**。
// 用户看到的只能是"分析完成 / 已取消"这类状态；毫秒数、MB 数属于工程信息，
// 出现在弹窗里只会让人困惑。要看就翻 videoeye_debug.log（与 exe 同目录）。
//
// 依赖方向：infrastructure 只能靠 stdlib（scripts/check_layering.py 硬规则），
// 所以这里 include 的同层头只有 Logger.h，进程内存采样走各平台系统 API。

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace videoeye {

// ---------------------------------------------------------------------------
// 指标名常量
//
// 用常量而不是字面量：指标名是要跨版本对比的口径，"analysis_ms" 拼成
// "analyze_ms" 之后历史数据就断了。
// ---------------------------------------------------------------------------

inline constexpr const char* kMetricOpenMs = "open_ms";                    // 打开媒体耗时
inline constexpr const char* kMetricAnalysisMs = "analysis_ms";            // 全文件分析耗时
inline constexpr const char* kMetricCancelResponseMs = "cancel_response_ms";  // 取消响应（请求→状态更新）
inline constexpr const char* kMetricCancelSettleMs = "cancel_settle_ms";   // 取消收敛（请求→后台终态）
inline constexpr const char* kMetricReportExportMs = "report_export_ms";   // 报告导出耗时
inline constexpr const char* kMetricFrameExportMs = "frame_export_ms";     // 媒体导出（片段）耗时
inline constexpr const char* kMetricPeakMemoryMb = "peak_memory_mb";       // 峰值内存（OS 高水位）
inline constexpr const char* kMetricRssMemoryMb = "rss_memory_mb";         // 当前驻留内存
//
// 表格行数 / 曲线点数**不设指标**：它们有硬上限（kDefaultMaxRecordCount /
// kMaxChartPoints），由代码保证、由单测钉住，而不是靠事后看日志发现"长歪了"。
// 每次批量追加都写一条 gauge 的话，一小时视频能刷出上万行日志，把真正要看的
// 打开 / 分析 / 导出耗时淹掉 —— 那才是指标该记的东西。

// ---------------------------------------------------------------------------
// 延迟预算
//
// 预算存在的意义不是"报警"，而是让"慢"有个可判定的边界 —— 否则每次讨论
// "取消是不是太慢了"都只能靠感觉。超预算只写 Warning 日志、不改变行为。
// ---------------------------------------------------------------------------

// 取消请求 → 界面状态更新（按钮置灰 / 进度条转"取消中"）
// 这段全在主线程上，理论上是一个事件循环的事；超过它就说明中间夹了同步等待。
inline constexpr double kCancelResponseBudgetMs = 100.0;

// 取消请求 → 后台线程收敛（终态回调到达）
// 后台退出依赖 FFmpeg 中断回调与循环检查点，卡在解码里时可能要几百毫秒；
// 但绝不该到秒级 —— 到秒级就说明某一层没看取消标志。
inline constexpr double kCancelSettleBudgetMs = 3000.0;

// 打开媒体：探测 + 首帧。超过 5s 用户会觉得"卡住了"。
inline constexpr double kOpenBudgetMs = 5000.0;

// 报告导出：写文件 + 编码。10s 是"还能忍"的上界。
inline constexpr double kReportExportBudgetMs = 10000.0;

// ---------------------------------------------------------------------------
// 记录器
// ---------------------------------------------------------------------------

class PerfMetrics {
public:
    static PerfMetrics& GetInstance();

    // 记录一次耗时（毫秒）。budget_ms > 0 时超预算升 Warning。
    // 同时按 name 维护 count/sum/min/max，供 Snapshot() 取累计值。
    void RecordTiming(const std::string& name, double ms, double budget_ms = 0.0);

    // 记录一个瞬时量（带单位，只用于日志展示；不做预算判定）。
    void RecordGauge(const std::string& name, double value, const std::string& unit);

    // 记一次事件（累计次数）。
    void RecordCount(const std::string& name, long long delta = 1);

    struct Stat {
        long long count = 0;
        double last = 0.0;
        double sum = 0.0;
        double min = 0.0;
        double max = 0.0;
    };

    // 取某个指标的累计值；没记录过返回 count == 0 的 Stat。
    Stat Snapshot(const std::string& name) const;
    // 已记录的指标名（顺序不定，给测试遍历用）
    std::vector<std::string> Names() const;
    void Reset();

    // 进程内存采样（MB）。采样失败返回 0 —— 与"真的用了 0 MB"区分不了，
    // 所以调用方拿到 0 时应跳过记录，别写一条假指标进去。
    static double CurrentRssMb();
    // 峰值驻留内存（MB）。取的是 OS 维护的高水位，不需要自己轮询。
    static double PeakRssMb();

private:
    PerfMetrics() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Stat> stats_;
};

// ---------------------------------------------------------------------------
// RAII 计时：构造开始，析构时自动记录
//
// 与 ScopedTimer 的区别：这个走 PerfMetrics 的长期口径（写进 stats_ 可累计），
// 且支持 Discard() —— 流程被取消时"耗时"没有意义，不该混进正常样本里。
// ---------------------------------------------------------------------------
class PerfScope {
public:
    explicit PerfScope(std::string name, double budget_ms = 0.0);
    ~PerfScope();

    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;

    // 放弃这次记录（析构时什么都不写）。用于"被取消 / 提前 return 的异常路径"。
    void Discard() { discarded_ = true; }
    double ElapsedMs() const;
    const std::string& name() const { return name_; }

private:
    std::string name_;
    double budget_ms_ = 0.0;
    bool discarded_ = false;
    std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// 取消延迟探针
//
// 取消有两个时刻，分开记才有意义：
//   * Arm()   —— 用户在界面上按下取消（UI 状态必须立刻变）
//   * Settle() —— 后台线程真正退出、终态回调到达（这才是"任务真的停了"）
// 只记第二个的话，"点了取消但界面 3 秒没反应"这种体验问题会被平均值掩盖。
//
// 允许重复 Arm()（用户连点取消）：以**最后一次**为准，Settle() 只生效一次。
// ---------------------------------------------------------------------------
class CancelLatencyProbe {
public:
    // 记录"发出取消请求"的时刻。
    void Arm();
    bool Armed() const { return armed_; }

    // 距上次 Arm 过去了多久（毫秒）。未 Arm 返回 -1。
    double ElapsedMs() const;

    // 后台收敛：记录 cancel_settle_ms 并返回延迟。
    // 未 Arm、或已经 Settle 过（重复终态）返回 -1 —— 后者不该发生，发生了说明
    // 终态被发了两次，调用方有责任把它暴露出来而不是静默吞掉。
    double Settle();

    // 放弃这次测量（例如任务其实没在跑）。
    void Abandon() { armed_ = false; settled_ = false; }

private:
    bool armed_ = false;
    bool settled_ = false;
    std::chrono::steady_clock::time_point armed_at_;
};

}  // namespace videoeye

// 用法：{ VE_METRIC_SCOPE(videoeye::kMetricReportExportMs, videoeye::kReportExportBudgetMs); ... }
#define VE_METRIC_CONCAT_IMPL(a, b) a##b
#define VE_METRIC_CONCAT(a, b) VE_METRIC_CONCAT_IMPL(a, b)
#define VE_METRIC_SCOPE(name, budget) \
    ::videoeye::PerfScope VE_METRIC_CONCAT(_ve_metric_scope_, __LINE__)(name, budget)
