#pragma once

// 批量导出的路径计算与落盘逻辑（从 ui/reporting_panel/ReportingPanel.cpp 抽出，便于单测）。
//
// 这两个函数原本是 ReportingPanel 匿名命名空间里的自由函数 / lambda 内部实现，
// 但评审要求对"不同目录同名文件互相覆盖"与"目标目录不可写"做回归测试，必须可被
// 测试直接调用。这里不依赖 Qt，UI 与单测共用同一份实现，保证行为一致。

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"
#include "utils/QcReportExporter.h"

namespace videoeye {
namespace ui {

// 批量导出时每个文件的主路径：保留相对子目录 + 文件 stem，不带扩展名。
// 导出与 UI 共用这一组路径，保证同名文件（位于不同子目录）不会互相覆盖，
// 且表格显示的路径就是真实落盘路径。当 file_path 不在 root 之下时退回只取文件名。
inline std::filesystem::path ReportBasePath(const std::string& out_dir,
                                            const std::string& root,
                                            const std::string& file_path) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(file_path, root, ec);
    const auto rel = ec ? std::filesystem::path(file_path).filename() : relative;
    return std::filesystem::path(out_dir) / rel.parent_path() / rel.stem();
}

// 分析成功后按选定格式落盘；与 UI 共用同一组"保留相对子目录"的路径。
// 导出失败（目录不可写 / 写入异常）时仍填 output_path，但 export_failed=true，
// 让上层能区分"分析成功 / 导出失败"。out_dir 为空或分析未成功时原样返回。
inline void ApplyExportPaths(qc::QcRunResult& result,
                             const std::string& out_dir,
                             const std::string& root,
                             const std::string& file_path,
                             const std::vector<qc::QcReportFormat>& formats,
                             const qc::QcProfile& profile) {
    if (out_dir.empty() || !result.ok) return;
    const std::filesystem::path base = ReportBasePath(out_dir, root, file_path);
    std::error_code ec;
    std::filesystem::create_directories(base.parent_path(), ec);  // 创建保留下来的子目录
    utils::QcExportBundle bundle;
    bundle.profile_id = profile.id;
    bundle.profile_name = profile.name;
    bundle.run = result;
    const auto fmts = formats.empty()
                         ? std::vector<qc::QcReportFormat>{qc::QcReportFormat::Json}
                         : formats;
    bool any_fail = false;
    for (const auto format : fmts) {
        const std::string target = base.string() + qc::QcReportExtension(format);
        const bool ok = (format == qc::QcReportFormat::Pdf)
                           ? utils::QcReportExporter::ExportPdf(target, bundle).ok
                           : utils::QcReportExporter::Export(target, bundle, format);
        if (!ok) any_fail = true;
    }
    result.output_path = base.string() + qc::QcReportExtension(fmts.front());
    result.export_failed = any_fail;
}

}  // namespace ui
}  // namespace videoeye
