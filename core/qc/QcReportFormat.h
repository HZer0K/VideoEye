#pragma once

// QC 报告的导出格式。单独抽成一个头文件是因为它被两边共用：
// 批量扫描要知道"每个文件要导出哪些格式"，报告导出器要按格式分派实现。
// 塞进任何一方都会让另一边多出一条不必要的依赖。

#include <string>
#include <vector>

namespace videoeye {
namespace qc {

enum class QcReportFormat {
    Json,   // 完整结构，机器读取 / CI 比对用
    Csv,    // 每个问题一行，表格软件里做透视用
    Html,   // 面向人工审核（含中文，浏览器直接打开）
    Text,   // 纯文本，快速过一眼
    Pdf,    // 打印/归档。见 QcReportExporter 关于中文的说明
};

const char* ToString(QcReportFormat format);
// 带点的规范扩展名，如 ".json"
std::string QcReportExtension(QcReportFormat format);

// 接受 "json" / ".JSON" / "html" 等写法
bool ParseQcReportFormat(const std::string& text, QcReportFormat& out);

// 按扩展名推断格式（文件名没有已知扩展名时返回 false）
bool QcReportFormatFromPath(const std::string& path, QcReportFormat& out);

}  // namespace qc
}  // namespace videoeye
