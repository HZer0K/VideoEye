#pragma once

// 报告导出的**纯口径**（从 ReportingPanel.cpp 抽出，便于单测）。
//
// 这一层不碰 QWidget，也不认识 QFileDialog / QMessageBox —— 只回答三个问题：
//   * 单文件报告该叫什么名字（SingleExportBaseName）；
//   * 批量汇总导出前那一份 summary 该怎么从条目列表重算（RecomputeBatchSummary）；
//   * "先选目录、分析完再导出"这条延迟路径的票据怎么管（DeferredExportTicket）。
//
// 前两条原本埋在 UI 回调里，错法都是"数字对不上 / 文件名不对"这一档 —— 不崩、
// 不报，只会悄悄给错答案。第三条的两个不变量（取走即用掉、失败即丢弃）以前靠
// 三处代码各自记得写 clear()，漏一处就会在下次成功时悄悄导出一份不相干的东西。

#include <filesystem>
#include <string>
#include <vector>

#include <QObject>  // tr 的翻译上下文（与 batch_result_model.h 一致）
#include <QString>

#include "core/qc/BatchQcRunner.h"

namespace videoeye {
namespace ui {

// 单文件报告的文件名主干（不带目录、不带扩展名）。
//
// 优先用**报告里记的文件路径**而不是当前文件路径：批量 / 历史结果 / 网络源下二者
// 可能不同，报告自己认的是 report.file_path；它为空（没跑出结果）才退回当前文件。
inline std::string SingleExportBaseName(const std::string& report_file_path,
                                        const std::string& fallback_path) {
    const std::string& chosen = report_file_path.empty() ? fallback_path : report_file_path;
    return std::filesystem::path(chosen).stem().string();
}

// 批量汇总导出前的 summary 重算。
//
// 导出入口拿得到的是条目列表（表格里那份 run.summary 没有留下来），所以按同一口径
// 重算一遍。两条口径要点：
//   * ExportFailed 计入 **failed** —— "分析成功但报告落盘失败"对汇总来说就是没成功，
//     与表格里的展示口径一致（见 batch_result_model.h 的 BatchTerminalState）；
//   * completed = 一个都没取消 —— 取消过就意味着列表里有没跑到的数据，这份汇总
//     不能自称完整。
// elapsed_ms 不重算：条目里没有可靠的墙钟，留 0 比编一个假的强。
inline qc::BatchQcSummary RecomputeBatchSummary(
    const std::vector<qc::BatchQcItemResult>& items) {
    qc::BatchQcSummary summary;
    for (const qc::BatchQcItemResult& item : items) {
        summary.total += 1;
        switch (item.status) {
            case qc::BatchItemStatus::Succeeded: summary.succeeded += 1; break;
            case qc::BatchItemStatus::Failed:    summary.failed += 1;    break;
            case qc::BatchItemStatus::ExportFailed: summary.failed += 1; break;
            case qc::BatchItemStatus::TimedOut:  summary.timed_out += 1; break;
            case qc::BatchItemStatus::Cancelled: summary.cancelled += 1; break;
            case qc::BatchItemStatus::Skipped:   summary.skipped += 1;   break;
            // Pending / Running 不该出现在终态列表里；真出现也不计数 —— 它既没成功
            // 也没失败，算进任何一档都会让 "已完成 + 失败 + ..." 对不上 total。
            case qc::BatchItemStatus::Pending:
            case qc::BatchItemStatus::Running:
            default: break;
        }
        summary.critical_count += item.critical_count;
        summary.error_count += item.error_count;
        summary.warning_count += item.warning_count;
        summary.info_count += item.info_count;
    }
    summary.completed = (summary.cancelled == 0);
    return summary;
}

// 「无结果时点导出」这条延迟路径的票据。
//
// 用户点一次导出，期望拿到的是**当前文件 + 当前模板**的报告。结果不新鲜时不能直接
// 导出（那会静默导出上一次的结果），只能先取好输出目录挂起、等这次分析成功后在
// 同一目录继续导出。票据的两个不变量：
//   * Arm 空目录 = 不登记（用户取消了对话框，不该留下一个空票据让下次成功时导出到
//     莫名其妙的地方）；
//   * Take 取走即用掉 —— 一次分析只对应一次导出，不会因为完成回调跑两次而导出两遍；
//   * 失败 / 取消必须 Clear() —— 免得下一次成功时悄悄导出一份不相干的东西。
class DeferredExportTicket {
public:
    bool armed() const { return !dir_.isEmpty(); }
    const QString& dir() const { return dir_; }

    void Arm(const QString& directory) { dir_ = directory; }  // 空串即等于不登记
    void Clear() { dir_.clear(); }

    // 取走即作废。未登记时返回空串，调用方据此判断"没有待导出"。
    QString Take() {
        const QString taken = dir_;
        dir_.clear();
        return taken;
    }

private:
    QString dir_;
};

}  // namespace ui
}  // namespace videoeye
