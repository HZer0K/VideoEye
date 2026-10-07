#pragma once

// 分析页各组件共用的小工具。
//
// 这里只放"批量提交/批量填充"这类与 Qt 控件打交道、但不依赖任何页面状态的
// 无状态辅助类：AnalysisPanel 与从它拆出去的页面组件（ContainerStructurePage /
// AudioQcPage / ColorHdrPage / SubtitleAuxPage ...）都要用，所以单独成头，
// 避免每拆一个页面就抄一份。导出 CSV 的统一写入端也在这里（见文件末尾）。

#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QMessageBox>
#include <QPointF>
#include <QString>
#include <QTableWidget>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

#include "core/domain/model/MetricSeries.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

// 表格单元格写值：item 不存在时先建一个（保持行高与选中行为一致）。
// 各页面组件都在用，所以放这里；AnalysisPanel 自己还有一个同名的成员函数，
// 类内查找优先命中成员，不会冲突。
inline void SetTableItemText(QTableWidget* table, int row, int column, const QString& text) {
    if (!table) return;
    QTableWidgetItem* item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem();
        table->setItem(row, column, item);
    }
    item->setText(text);
}

// 指标值 + 单位 -> 展示串（异常表 / 汇总行都在用）
inline QString FormatMetricValue(double value, const QString& unit) {
    if (unit == QStringLiteral("B")) {
        if (value >= 1024.0 * 1024.0) return QString::number(value / 1048576.0, 'f', 2) + " MB";
        if (value >= 1024.0) return QString::number(value / 1024.0, 'f', 1) + " KB";
        return QString::number(value, 'f', 0) + " B";
    }
    if (unit == QStringLiteral("kbps")) return QString::number(value, 'f', 0) + " kbps";
    if (unit == QStringLiteral("帧") || unit == QStringLiteral("个")) {
        return QString::number(value, 'f', 0) + " " + unit;
    }
    if (unit == QStringLiteral("s")) return QString::number(value, 'f', 2) + " s";
    return QString::number(value, 'f', 3);
}

inline QString FormatKb(double bytes) { return QString::number(bytes / 1024.0, 'f', 1); }

// 图表数据批量提交。
//
// 逐点 Append() 每加一个点都会触发一次图表重算/重绘，几千点时主线程会被
// 拖到秒级卡顿（实测 7 条曲线 × 4000 点 ≈ 3.5 s）。先在内存里攒好，析构时
// 用 Replace() 一次性提交（只触发一次刷新）。
class SeriesBatch {
public:
    explicit SeriesBatch(ChartSeries* series) : series_(series) {}
    ~SeriesBatch() {
        if (series_) series_->Replace(points_);
    }

    void Reserve(int n) { points_.reserve(n); }
    void Add(double x, double y) { points_.append(QPointF(x, y)); }
    bool Empty() const { return points_.isEmpty(); }

private:
    ChartSeries* series_ = nullptr;
    QVector<QPointF> points_;
};

// 曲线抽稀后再提交：每组保留最大值（避免把峰值抹掉），并且走 SeriesBatch 一次性提交。
inline void AppendDecimated(ChartSeries* series, const model::MetricSeries& curve, int limit) {
    const std::size_t n = curve.Size();
    if (n == 0) return;
    SeriesBatch batch(series);
    if (n <= static_cast<std::size_t>(limit)) {
        batch.Reserve(static_cast<int>(n));
        for (const auto& s : curve.samples) batch.Add(s.timestamp_seconds, s.value);
        return;
    }
    const std::size_t group = (n + limit - 1) / limit;
    batch.Reserve(limit);
    for (std::size_t i = 0; i < n; i += group) {
        const std::size_t end = std::min(i + group, n);
        double v = curve.samples[i].value;
        for (std::size_t j = i + 1; j < end; ++j) v = std::max(v, curve.samples[j].value);
        batch.Add(curve.samples[i].timestamp_seconds, v);
    }
}

// 裁剪记录向量到指定上限：移除最早的多余记录，并让表格从头重建
// （synced_count 归零 + 置脏，下一次 FlushPending 会整表重填）。
//
// 原为各页面里的匿名命名空间副本，随「音频帧子页」从 FramePacketView 抽出时提到这里：
// 视频帧 / GOP / 包 / 音频帧四张表共用同一套裁剪语义，不该每拆一个子页就抄一份。
template<typename T>
inline void TrimRecords(std::vector<T>& records, std::size_t& synced_count,
                        QTableWidget* table, bool& table_dirty, std::size_t max_count) {
    if (records.size() <= max_count) return;
    const std::size_t remove_count = records.size() - max_count;
    records.erase(records.begin(), records.begin() + remove_count);
    if (table) {
        table->setRowCount(0);
    }
    synced_count = 0;
    table_dirty = true;
}

// 大表批量填充: 先关掉刷新与重绘, 一次性预分配行数, 比逐行 insertRow 快一个量级
// (insertRow 每次都要移动后续行, 1 万行时是 O(n^2))。
class TableBatch {
public:
    explicit TableBatch(QTableWidget* table) : table_(table) {
        if (!table_) return;
        was_updates_enabled_ = table_->updatesEnabled();
        table_->setUpdatesEnabled(false);
    }
    ~TableBatch() {
        if (!table_) return;
        table_->setUpdatesEnabled(was_updates_enabled_);
    }

    void SetRowCount(int rows) { if (table_) table_->setRowCount(rows); }
    int RowCount() const { return table_ ? table_->rowCount() : 0; }
    void SetText(int row, int column, const QString& text, bool bold = false) {
        if (!table_) return;
        auto* item = new QTableWidgetItem(text);
        if (bold) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        table_->setItem(row, column, item);
    }
    QTableWidgetItem* Item(int row, int column) const {
        return table_ ? table_->item(row, column) : nullptr;
    }

private:
    QTableWidget* table_ = nullptr;
    bool was_updates_enabled_ = true;
};

// ---------------------------------------------------------------------------
// CSV 导出统一样板
//
// 各页 / 各表的导出原本每处手抄一遍：选文件名 -> QFile 打开 -> UTF-8 BOM ->
// 写表头逐行 -> 成功弹窗。20 来处里过滤串、成功/失败文案、BOM 有无各不相同，
// 其中一半漏了 BOM —— Excel 打开中文列直接乱码，要真导出一次才看得见。
// 这里收成两层，共用同一份实现：
//   WriteCsvStream  —— 纯写入（可单测）：编码 + BOM + 关闭，失败返回 false；
//   ExportCsvStream —— UI 样板：选文件名 -> 写入 -> 成功/失败提示。
// 空数据判断与默认文件名仍留给调用方（提示文案与命名规则各页不同）。
// ---------------------------------------------------------------------------

// 打开 path 写 CSV 正文：UTF-8 + BOM，正文由 write_body 往流里写。
// 打开失败返回 false（不弹窗，调用方 / 单测自己决定怎么提示）。
inline bool WriteCsvStream(const QString& path,
                           const std::function<void(QTextStream&)>& write_body) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    // BOM 必须按原始字节直接写设备，不能 stream << "\xEF\xBB\xBF"：
    // QTextStream 的 UTF-8 编码器会把写进流的开头 U+FEFF 当 BOM 标记消费掉，
    // 那样落盘的文件头是空的（实测踩过），Excel 打开中文列照样乱码。
    file.write("\xEF\xBB\xBF", 3);
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    write_body(stream);
    file.close();
    return true;
}

// UI 层导出样板：选择文件名 -> 写入 -> 结果提示。
// exported_rows 只用于成功提示里的行数；用户取消返回 false（不弹任何框）。
inline bool ExportCsvStream(QWidget* parent, const QString& dialog_title,
                            const QString& default_name,
                            const std::function<void(QTextStream&)>& write_body,
                            int exported_rows) {
    const QString path = QFileDialog::getSaveFileName(
        parent, dialog_title, default_name,
        QCoreApplication::translate("AnalysisPageSupport", "CSV 文件 (*.csv);;所有文件 (*)"));
    if (path.isEmpty()) return false;
    if (!WriteCsvStream(path, write_body)) {
        QMessageBox::warning(parent,
            QCoreApplication::translate("AnalysisPageSupport", "错误"),
            QCoreApplication::translate("AnalysisPageSupport", "无法写入文件: %1").arg(path));
        return false;
    }
    QMessageBox::information(parent,
        QCoreApplication::translate("AnalysisPageSupport", "成功"),
        QCoreApplication::translate("AnalysisPageSupport", "已导出 %1 行到:\n%2")
            .arg(exported_rows).arg(path));
    return true;
}

} // namespace ui
} // namespace videoeye
