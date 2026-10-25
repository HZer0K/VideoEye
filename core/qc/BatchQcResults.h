#pragma once

// 批量 QC 的**结果类型**（从 BatchQcRunner.h 拆出）。
//
// 为什么要单独成文件：报告导出只需要 BatchQcRun 与汇总计数，不需要执行器本身。
// 以前 QcReportExporter.h 为了拿这些类型 include 了 BatchQcRunner.h，于是
// VideoEyeReporting 必须链 VideoEyeQc（执行器）→ VideoEyeAnalysis → FFmpeg，
// "报告层零 FFmpeg"的边界被一条 include 打穿。现在结果类型归 VideoEyeQcRules，
// 执行器与报告导出共用；导出器只链规则层，链接闭包里不再出现分析层与 FFmpeg。

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/QcReport.h"

namespace videoeye {
namespace qc {

enum class BatchItemStatus {
    Pending,      // 还没轮到
    Running,      // 正在分析
    Succeeded,    // 分析完成（不代表没问题，只看 report 里的严重度计数）
    Failed,       // 打开失败 / 分析器报错
    TimedOut,     // 超过 per_item_timeout_ms 仍未结算（后台线程被放弃并已请求取消）
    Cancelled,    // 取消后未执行，或执行中被中断
    Skipped,      // 被选项排除（如超过大小上限）
    ExportFailed, // 分析成功，但报告导出失败（目录不可写 / 写入异常）
};

const char* ToString(BatchItemStatus status);

struct BatchQcItemResult {
    std::string path;
    BatchItemStatus status = BatchItemStatus::Pending;
    std::string error;
    // 报告本体。默认保留（UI 要按文件看详情）；批量导出成百上千个文件时可以把
    // options.keep_reports 关掉，只留下面几个计数，省内存。
    model::QcReport report;
    int critical_count = 0;
    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
    double score = 0.0;
    std::string verdict;
    double elapsed_ms = 0.0;
    std::string output_path; // 主导出文件的落盘路径（由调用方通过 output_path_factory 决定）

    bool HasReport() const {
        return status == BatchItemStatus::Succeeded;
    }
    int IssueCount() const {
        return critical_count + error_count + warning_count + info_count;
    }
    int BlockingCount() const {
        return critical_count + error_count;
    }
};

struct BatchQcSummary {
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    int timed_out = 0; // 单项超过 per_item_timeout_ms（后台线程被放弃并已请求取消）
    int cancelled = 0;
    int skipped = 0; // 超过大小上限被跳过
    int critical_count = 0;
    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
    double elapsed_ms = 0.0;
    bool completed = false; // false = 被取消，列表里有没跑到的数据
};

struct BatchQcRun {
    std::vector<BatchQcItemResult> items;
    BatchQcSummary summary;
};

} // namespace qc
} // namespace videoeye
