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
#include "core/reporting/QcReportExporter.h"

namespace videoeye {
namespace ui {

// 相对路径是否可以安全地拼到导出目录下。
// 判据只有两条：不能是绝对路径（跨盘、root 为空时 relative() 会原样返回它），
// 也不能含 ".." 分量 —— 两者都会让最终结果逃出 out_dir。
inline bool IsContainedRelativePath(const std::filesystem::path& rel) {
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) {
        if (part == std::filesystem::path("..")) return false;
    }
    return true;
}

// 目录 dir 是否**实际**位于 parent 之下（解析符号链接 / Windows junction 之后）。
//
// 只查输入路径里的 ".." 是不够的：导出目录里**已经存在**的链接同样能把写操作带出去
// —— out/sub 是指向外部目录的 junction 时，out/sub/clip.json 会如实落到外部。
// 所以最终落盘目录要在解析完链接之后再判一次从属关系。
inline bool IsContainedUnder(const std::filesystem::path& dir,
                             const std::filesystem::path& parent) {
    std::error_code ec;
    // weakly_canonical 会解析已存在的那一段（含 junction / 符号链接），
    // 不存在的那一段只做词法归一化 —— 不要求路径已经存在。
    const auto child = std::filesystem::weakly_canonical(dir, ec);
    if (ec) return false;
    const auto base = std::filesystem::weakly_canonical(parent, ec);
    if (ec) return false;
    const auto rel = std::filesystem::relative(child, base, ec);
    if (ec) return false;
    return IsContainedRelativePath(rel);
}

// 批量导出时每个文件的主路径：保留相对子目录 + 文件 stem，不带扩展名。
// 导出与 UI 共用这一组路径，保证同名文件（位于不同子目录）不会互相覆盖，
// 且表格显示的路径就是真实落盘路径。当 file_path 不在 root 之下、或最终落盘
// 目录逃出 out_dir 时退回只取文件名。
//
// containment 是**严格**的，两侧都要查：
//   1. 输入侧：直接对原始字符串做 relative() 只算出一条相对路径，它照样可能带着
//      ".."（root 与 file_path 一个走符号链接、一个是相对路径、或 root 本身为空
//      都会这样），拼到 out_dir 上就是目录逃逸。所以两侧先各自 weakly_canonical
//      （去掉 . / .. / 符号链接，且不要求路径已存在），再算相对部分，最后仍要过
//      一遍 IsContainedRelativePath —— 拿不到可信的相对路径就退回文件名。
//   2. 输出侧：out_dir 里的子目录**可能已经是链接**（Windows junction / 符号链接）。
//      输入侧查不出这种情况（"sub/clip.json" 本身很干净），但拼出来的
//      out_dir/sub 解析后可能指向盘外。所以最终父目录还要再判一次从属关系。
inline std::filesystem::path ReportBasePath(const std::string& out_dir,
                                            const std::string& root,
                                            const std::string& file_path) {
    std::error_code ec;
    const auto abs_file =
        std::filesystem::weakly_canonical(std::filesystem::path(file_path), ec);
    const auto abs_root = std::filesystem::weakly_canonical(std::filesystem::path(root), ec);
    std::filesystem::path rel;
    if (!ec && !abs_root.empty()) {
        const auto candidate = std::filesystem::relative(abs_file, abs_root, ec);
        if (!ec && IsContainedRelativePath(candidate)) rel = candidate;
    }
    if (rel.empty()) rel = std::filesystem::path(file_path).filename();

    const std::filesystem::path out(out_dir);
    const std::filesystem::path parent = out / rel.parent_path();
    // 链接目录一律拒绝，退回"铺平到 out_dir"：宁可少保留一层子目录，
    // 也不能把报告写到导出目录之外。
    if (!IsContainedUnder(parent, out)) return out / rel.stem();
    return parent / rel.stem();
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
    reporting::QcExportBundle bundle;
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
                           ? reporting::QcReportExporter::ExportPdf(target, bundle).ok
                           : reporting::QcReportExporter::Export(target, bundle, format);
        if (!ok) any_fail = true;
    }
    result.output_path = base.string() + qc::QcReportExtension(fmts.front());
    result.export_failed = any_fail;
}

}  // namespace ui
}  // namespace videoeye
