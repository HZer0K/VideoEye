#include "core/qc/BatchQcResults.h"

namespace videoeye {
namespace qc {

const char* ToString(BatchItemStatus status) {
    switch (status) {
    case BatchItemStatus::Pending:
        return "等待中";
    case BatchItemStatus::Running:
        return "分析中";
    case BatchItemStatus::Succeeded:
        return "完成";
    case BatchItemStatus::Failed:
        return "失败";
    case BatchItemStatus::TimedOut:
        return "超时";
    case BatchItemStatus::Cancelled:
        return "已取消";
    case BatchItemStatus::Skipped:
        return "已跳过";
    case BatchItemStatus::ExportFailed:
        return "导出失败";
    }
    return "未知";
}

} // namespace qc
} // namespace videoeye
