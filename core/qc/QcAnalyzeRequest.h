#pragma once

// 单文件分析的最小契约
//
// 单独一个头文件的原因：BatchQcRunner 只需要知道"把一个路径交给某个函数，能拿到 QcRunResult"，
// 并不需要知道背后是 QcRunner（那条路会把 Qt / QtAnalysisController 全拖进来）。
// 抽出去之后批量扫描这一层就没有任何 Qt / FFmpeg 符号依赖，单测可以直接喂假实现。

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "core/qc/AnalysisTaskRegistry.h"
#include "core/qc/QcRunBudgets.h"

namespace videoeye {
namespace qc {

// 一次分析的完整产物
struct QcRunResult {
    bool ok = false; // 分析是否跑完（false = 打开失败 / 被取消 / 超时）
    // 分析被判超时/无响应：后台线程被放弃（detach）并已收到取消请求。与"引擎自己报
    // 失败"区分开：调用方据此知道这次结果不完整、且后台可能还在收尾以便记录资源回收。
    // ❗ 调用方主动取消导致的放弃**不**置这一位（那时 error = "分析被取消"），
    // 否则批量侧会把"用户点了停止"记成超时 —— 两者的处置方式不同。
    bool timed_out = false;
    std::string error; // ok=false 时的原因
    model::QcReport report;
    model::AnalysisResult analysis; // 原始数据，对比模式要靠它拿详细的编码参数
    std::string profile_id;
    double elapsed_ms = 0.0;

    // 批量导出：导出的实际落盘路径与是否全部格式成功。
    // 让 UI 表格显示的路径 = 真实文件，并让"分析成功但导出失败"可区分。
    std::string output_path;               // 主格式（第一个勾选格式）的落盘路径
    std::vector<std::string> output_paths; // 各选中格式的实际路径（与 formats 一一对应）
    bool export_failed = false;            // 任一格式导出失败（仅当 out_dir 非空且分析成功时）
};

// 统一输入类型：QC 的输入既可能是本地文件，也可能是网络源（HTTP / RTMP / RTSP ...）。
//
// 以前"输入"只是一条裸字符串，于是同一条路径在不同层被两种口径判定：
//   * ReportingPanel::SetCurrentFile 存什么就是什么（URL 也照存）；
//   * QcRunner::IsAnalyzableFile 只认本地文件 —— HTTP/RTMP/RTSP 一律被判成"文件不存在"。
// 于是报告页拿到网络源时永远分析不了，还报"文件不存在"这种误导性原因。
//
// 抽出这个类型之后，"这是不是本地文件"只有一个判定点：单文件 QC 据 Kind 决定是否
// 走 FFmpeg 网络输入与网络超时策略；本地批量扫描仍然只接受 LocalFile。
struct MediaInput {
    enum class Kind { LocalFile, NetworkUri };

    std::string uri;
    Kind kind = Kind::LocalFile;
};

// 按 scheme 把一条 uri 归类为本地文件 / 网络源。不联网、不判存在性，纯字符串分类，
// 可被单测直接驱动。识别的 scheme: http/https/rtmp/rtmps/rtsp/rtsps/rtp/udp/tcp/srt/
// hls，以及 ffmpeg 惯用的其它 "xxx://" 形态（带任意 scheme 的 URI 一律按网络源对待）。
MediaInput ClassifyMediaInput(const std::string& uri);

// uri 是否看起来是网络源（ClassifyMediaInput(...).kind == NetworkUri 的便捷判断）。
bool IsNetworkUri(const std::string& uri);

// 一次分析请求。批量扫描会把自己的 cancel 标记塞进来，用来中断正在跑的单文件分析
// （只靠"不再派新任务"是不够的 —— 已经在扫的大文件还会跑很久）。
struct QcAnalyzeRequest {
    std::string path;
    const std::atomic<bool>* cancel = nullptr;
    // 本次分析的等待策略（允许跑多久 / IO 多久无响应 / 取消后等多久）。
    // 批量调度把 BatchQcOptions 的 per_item_timeout_ms / io_idle_timeout_ms /
    // cancel_wait_ms 透传到这里，让"每个文件最长跑多久"成为批量参数的一部分，
    // 而不是在别处埋一个魔法值。
    //
    // ❗ 以前这里只有一个 join_budget_ms，它同时被当成"分析能跑多久"和"关停等多久"，
    // 于是单文件分析被 5 秒的关停预算误杀（评审 P1-1）。三项策略现在各归各的，
    // 见 QcRunBudgets.h。
    QcRunBudgets budgets;

    // 调用方（批量调度）在派发前占好的并发额度。交给分析函数之后由**分析线程体**
    // 在真正退出时归还；调用方只在"分析函数没有认领它"时兜底归还（见
    // AnalysisTaskRegistry）。没有占额度时留空 —— 执行器会自己登记一条，
    // 于是"当前实际存活多少条分析"是全局可观测的。
    std::shared_ptr<AnalysisTaskSlot> slot;
};

// 分析函数签名：QcRunner::MakeAnalyzeFunction 产出它，BatchQcRunner 消费它，
// 单测里则换成不需要任何媒体文件的假实现。
using QcAnalyzeFn = std::function<QcRunResult(const QcAnalyzeRequest&)>;

} // namespace qc
} // namespace videoeye
