#pragma once

// 批量 QC 结果的展示口径与终态判定（从 ReportingPanel 抽出，便于单测）。
//
// 抽出来的三条都是**错了不崩、只会悄悄给错答案**的那种口径，而它们原本住在
// UI 回调里（RunBatch 的完成段 / ShowBatchDetail），测不到：
//
//   1. 整批任务的终态。超时必须按"没干净跑完"算 —— 它的后台线程被放弃、结果不完整，
//      算成功的话用户会以为这一批全跑完了；
//   2. 汇总行与详情文本。十一个计数 / 八段文本，漏一个或串位都不容易被肉眼发现；
//   3. 表格行的取值。数值列必须以**数值**存进 DisplayRole，而不是格式化后的字符串
//      —— QTableWidgetItem::operator< 走 QVariant 比较，存字符串会让 "10" 排在 "9" 前面
//      （这是以前真出过的 bug，见 DoubleFormatDelegate 的注释）。
//
// 与 batch_filter.h / result_stamp.h 同一思路：能被测试直接调用的逻辑一律抽成自由函数。

#include <QObject> // QObject::tr（文本模板的翻译入口）
#include <QString>

#include "core/domain/task/TaskProtocol.h"
#include "core/qc/BatchQcRunner.h"

namespace videoeye {
namespace ui {

// 条目状态文本。BatchItemStatus 的文本是 qc::ToString 的字符串常量（不是 tr 字面量），
// 这里只做转码，不做翻译。
inline QString BatchItemStatusText(qc::BatchItemStatus status) {
    return QString::fromStdString(qc::ToString(status));
}

// 整批任务的终态。
//
// 优先级与 ThreadBody::ResolveTerminal 一致（取消 > 失败 > 成功）：用户点了取消，
// 结论就该是 Canceled，哪怕同时有条目失败。
// **超时也算没干净跑完**：那条目的后台线程被放弃、结果不完整，不能算成功。
inline task::TaskState BatchTerminalState(const qc::BatchQcSummary& summary, bool canceled) {
    if (canceled) {
        return task::TaskState::Canceled;
    }
    if (summary.failed != 0 || summary.timed_out != 0) {
        return task::TaskState::Failed;
    }
    return task::TaskState::Succeeded;
}

// 汇总行文本（批量跑完贴在进度条下方）。
//
// total 传**发现到的条目数**而不是 summary.total：被跳过 / 未跑到的条目也在表格里
// 占一行，用 summary.total 会让"共 N 个"与表格行数对不上。
//
// 注：文本用 QObject::tr 而不是某个 QObject 子类的 tr()，翻译上下文随之变成 QObject。
// 项目当前没有 .ts 翻译文件（构建期就有 "Translations will not be available"），无影响。
inline QString BatchSummaryText(const qc::BatchQcSummary& summary, int total) {
    return QObject::tr("共 %1 个 ｜ 完成 %2 ｜ 失败 %3 ｜ 超时 %4 ｜ 取消 %5 ｜ 跳过 %6 ｜ "
                       "致命 %7 错误 %8 警告 %9 提示 %10 ｜ 耗时 %11 ms")
        .arg(total)
        .arg(summary.succeeded)
        .arg(summary.failed)
        .arg(summary.timed_out)
        .arg(summary.cancelled)
        .arg(summary.skipped)
        .arg(summary.critical_count)
        .arg(summary.error_count)
        .arg(summary.warning_count)
        .arg(summary.info_count)
        .arg(static_cast<int>(summary.elapsed_ms));
}

// 一条结果的详情文本（选中表格行时展开在下方）。
//
// 表格十列装不下失败原因与问题清单，所以详情是"选中才展开"的第二层信息。
inline QString BatchDetailText(const qc::BatchQcItemResult& item) {
    QString text = QObject::tr("文件: %1\n").arg(QString::fromStdString(item.path));
    text += QObject::tr("状态: %1").arg(BatchItemStatusText(item.status));
    if (item.status == qc::BatchItemStatus::ExportFailed) {
        // 分析本身没问题，是报告落盘失败 —— 处置方式与 Failed 完全不同，必须点明
        text += QObject::tr("（分析完成，但报告落盘失败）");
    }
    text += QStringLiteral("\n");
    if (!item.error.empty()) {
        text += QObject::tr("原因: %1\n").arg(QString::fromStdString(item.error));
    }
    text += QObject::tr("评分: %1 / 100 ｜ 结论: %2\n")
                .arg(item.score, 0, 'f', 1)
                .arg(QString::fromStdString(item.verdict));
    text += QObject::tr("问题: 致命 %1 ｜ 错误 %2 ｜ 警告 %3 ｜ 提示 %4\n")
                .arg(item.critical_count)
                .arg(item.error_count)
                .arg(item.warning_count)
                .arg(item.info_count);
    text += QObject::tr("耗时: %1 ms").arg(item.elapsed_ms, 0, 'f', 0);
    if (!item.output_path.empty()) {
        text += QObject::tr(" ｜ 输出: %1").arg(QString::fromStdString(item.output_path));
    }
    // 报告本体默认不驻留内存（keep_reports=false，见 RunBatch），issues 为空；
    // 万一将来打开保留，这里直接把问题清单列出来，UI 不用再改。
    if (!item.report.issues.empty()) {
        text += QObject::tr("\n问题清单:");
        for (const model::DiagnosticIssue& issue : item.report.issues) {
            text += QObject::tr("\n  [%1] %2 — %3")
                        .arg(QString::fromStdString(issue.SeverityText()), QString::fromStdString(issue.title),
                             QString::fromStdString(issue.detail));
        }
    }
    return text;
}

// 表格一行的十列取值（纯数据，不碰 QTableWidget —— 这样它能被单测）。
//
// 数值列的类型就是口径：score / elapsed_ms 是 double、四个计数是 int。
// 铺行时必须原样塞进 Qt::DisplayRole（见 MakeBatchRowCells 的使用处），
// 显示格式交给 DoubleFormatDelegate，绝不能先格式化成字符串再存。
struct BatchRowCells {
    QString path;
    QString status;
    double score = 0.0;
    QString verdict;
    int critical = 0;
    int error_count = 0;
    int warning = 0;
    int info = 0;
    double elapsed_ms = 0.0;
    QString output_path;
};

inline BatchRowCells MakeBatchRowCells(const qc::BatchQcItemResult& item) {
    BatchRowCells cells;
    cells.path = QString::fromStdString(item.path);
    cells.status = BatchItemStatusText(item.status);
    cells.score = item.score;
    cells.verdict = QString::fromStdString(item.verdict);
    cells.critical = item.critical_count;
    cells.error_count = item.error_count;
    cells.warning = item.warning_count;
    cells.info = item.info_count;
    cells.elapsed_ms = item.elapsed_ms;
    cells.output_path = QString::fromStdString(item.output_path);
    return cells;
}

} // namespace ui
} // namespace videoeye
