#pragma once

// 批量 QC 表格的筛选口径（纯函数，无 Qt 依赖）。
//
// ReportingPanel 的两个筛选下拉框把这些值存进 QComboBox 的 itemData，表里每一行
// 再按自己的 BatchQcItemResult 判一次是否可见。抽成自由函数是为了能单测 —— 判定
// 本身有边界（"无异常"遇到失败条目算不算匹配、状态与严重度两个条件是"与"还是"或"），
// 纯 UI 交互测不到这些组合。

#include "core/qc/BatchQcRunner.h"

namespace videoeye {
namespace ui {

// 状态筛选值：不限，或 qc::BatchItemStatus 的枚举值。
constexpr int kBatchStatusFilterAll = -1;

// 严重度筛选值：一条阶梯，选中的是"该级别及以上"。
constexpr int kBatchSeverityFilterAll = 0;      // 不限
constexpr int kBatchSeverityFilterCritical = 1; // 致命及以上
constexpr int kBatchSeverityFilterError = 2;    // 错误及以上（含致命）
constexpr int kBatchSeverityFilterWarning = 3;  // 警告及以上（含致命、错误）
constexpr int kBatchSeverityFilterInfo = 4;     // 提示及以上（= 有任何问题）
constexpr int kBatchSeverityFilterClean = 5;    // 无任何异常

inline bool BatchMatchesStatusFilter(int filter, qc::BatchItemStatus status) {
    return filter == kBatchStatusFilterAll || filter == static_cast<int>(status);
}

// 严重度按阶梯判定（"该级别及以上"），不是"恰好等于该级别"。
//
// 为什么必须是阶梯：如果"含警告"只认 warning_count>0，那么一条只有致命问题的结果
// 在"含警告"下会被藏起来 —— 越严重的反而越容易被筛掉，正是筛选最不该犯的错。
// 阶梯下"提示及以上"等价于"有任何问题"，正好与"无异常"互补。
// Pending/Running 不会出现在终态表里，计数为 0，只有"无异常"会匹配它们。
inline bool BatchMatchesSeverityFilter(int filter, const qc::BatchQcItemResult& item) {
    switch (filter) {
    case kBatchSeverityFilterCritical:
        return item.critical_count > 0;
    case kBatchSeverityFilterError:
        return item.BlockingCount() > 0;
    case kBatchSeverityFilterWarning:
        return item.BlockingCount() > 0 || item.warning_count > 0;
    case kBatchSeverityFilterInfo:
        return item.IssueCount() > 0;
    case kBatchSeverityFilterClean:
        return item.IssueCount() == 0;
    case kBatchSeverityFilterAll:
        break;
    }
    return true;
}

} // namespace ui
} // namespace videoeye
