#pragma once

// 单文件分析的最小契约
//
// 单独一个头文件的原因：BatchQcRunner 只需要知道"把一个路径交给某个函数，能拿到 QcRunResult"，
// 并不需要知道背后是 QcRunner（那条路会把 Qt / AnalysisCoordinator 全拖进来）。
// 抽出去之后批量扫描这一层就没有任何 Qt / FFmpeg 符号依赖，单测可以直接喂假实现。

#include <atomic>
#include <functional>
#include <string>

#include "core/analyzer/AnalysisTask.h"
#include "core/model/QcReport.h"

namespace videoeye {
namespace qc {

// 一次分析的完整产物
struct QcRunResult {
    bool ok = false;               // 分析是否跑完（false = 打开失败 / 被取消）
    std::string error;             // ok=false 时的原因
    model::QcReport report;
    analyzer::AnalysisResult analysis;  // 原始数据，对比模式要靠它拿详细的编码参数
    std::string profile_id;
    double elapsed_ms = 0.0;
};

// 一次分析请求。批量扫描会把自己的 cancel 标记塞进来，用来中断正在跑的单文件分析
// （只靠"不再派新任务"是不够的 —— 已经在扫的大文件还会跑很久）。
struct QcAnalyzeRequest {
    std::string path;
    const std::atomic<bool>* cancel = nullptr;
};

// 分析函数签名：QcRunner::MakeAnalyzeFunction 产出它，BatchQcRunner 消费它，
// 单测里则换成不需要任何媒体文件的假实现。
using QcAnalyzeFn = std::function<QcRunResult(const QcAnalyzeRequest&)>;

}  // namespace qc
}  // namespace videoeye
