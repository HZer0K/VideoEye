#pragma once

// 播放期持续增长的明细表（视频帧 / 包 / 音频帧）共用的 QAbstractTableModel。
//
// 为什么要有它：这三张表以前是 QTableWidget，行数 = 记录条数。10 万级记录下
// 就是 60 万个 QTableWidgetItem —— 每个 item 都是一个堆对象（QVariant + QString），
// 内存跟着记录数线性涨到百 MB 量级；而且"超限裁剪"走的是 setRowCount(0) + 逐条重插，
// 一次裁剪就把主线程卡住。
//
// 换到 model/view 之后：
//   * 内存只跟**记录结构体**走，不再乘以列数 —— 单元格文本是 data() 时现算的，
//     只为可见的那几十行生成；
//   * 裁剪是 beginRemoveRows（O(可见行数)），不再是整表重建；
//   * 增量追加是 beginInsertRows，一次通知一批，游标同步不再靠外部记账。
//
// 为什么是模板而不是基类 + 虚 data()：三张表的记录类型（VideoFrameRecord /
// PacketRecord / AudioFrameRecord）都是 POD 风格的值类型，用列适配器（ColumnSpec）
// 描述"每一列取什么文本"就够，不必为每种记录各写一个 QAbstractTableModel 子类
// —— 那意味着三份几乎一样的 rowCount / data / headerData。
//
// 注意：本类**没有 Q_OBJECT**。moc 不处理模板类，而这里也不需要自定义信号
// （模型变化本来就通过 begin*/end* 通知视图）。别为了"看起来规范"加 Q_OBJECT，
// 加了只会在 AUTOMOC 阶段报一个看不懂的错。

#include <QAbstractTableModel>
#include <QColor>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "infrastructure/logging/PerfMetrics.h"

namespace videoeye {
namespace ui {

// 记录缓存的默认上限。
//
// 10 万级的来源：1 小时 25fps 的素材约 9 万帧，加上包记录（每帧多个包）会更多。
// 定在 10 万是"能装下一小时视频的全部帧记录"—— 再大就只是把内存账单推后，
// 而 QC 结论并不依赖把第 12 万帧还留在表里。
inline constexpr std::size_t kDefaultMaxRecordCount = 100000;

template <typename Record>
class RecordTableModel : public QAbstractTableModel {
public:
    // 一列的定义。foreground 可为空（不染色）。
    struct ColumnSpec {
        QString header;
        std::function<QString(const Record&)> text;
        std::function<QColor(const Record&)> foreground;
    };

    explicit RecordTableModel(std::vector<ColumnSpec> columns, QObject* parent = nullptr)
        : QAbstractTableModel(parent), columns_(std::move(columns)) {
    }

    // ---- QAbstractTableModel ----

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        // 只有根节点有行：子索引一律 0，否则树视图会把每行当成一个有子节点的分支。
        return parent.isValid() ? 0 : static_cast<int>(visible_.size());
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(columns_.size());
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!index.isValid())
            return QVariant();
        const int row = index.row();
        const int column = index.column();
        if (row < 0 || row >= static_cast<int>(visible_.size()))
            return QVariant();
        if (column < 0 || column >= static_cast<int>(columns_.size()))
            return QVariant();

        const Record& record = records_[visible_[static_cast<std::size_t>(row)]];
        switch (role) {
        case Qt::DisplayRole:
            return columns_[static_cast<std::size_t>(column)].text
                       ? columns_[static_cast<std::size_t>(column)].text(record)
                       : QVariant();
        case Qt::ForegroundRole: {
            const auto& getter = columns_[static_cast<std::size_t>(column)].foreground;
            if (!getter)
                return QVariant();
            const QColor color = getter(record);
            return color.isValid() ? QVariant(color) : QVariant();
        }
        default:
            return QVariant();
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
            return QVariant();
        if (section < 0 || section >= static_cast<int>(columns_.size()))
            return QVariant();
        return columns_[static_cast<std::size_t>(section)].header;
    }

    // ---- 数据维护 ----

    const std::vector<Record>& records() const {
        return records_;
    }
    std::size_t record_count() const {
        return records_.size();
    }

    // 追加一批记录：一次 rowsInserted 通知。
    //
    // 设计上刻意不提供"逐条 Append"：播放期回调每帧/每包来一次，若每次都
    // beginInsertRows，等于把 120ms 批量刷新的节流又拆回逐条。调用方应当
    // 先在自己的缓冲里攒好，FlushPending 时一次性交进来。
    void AppendRecords(std::vector<Record> batch) {
        if (batch.empty())
            return;

        const std::size_t first_new = records_.size();
        std::vector<std::size_t> new_visible;
        new_visible.reserve(batch.size());
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (!filter_ || filter_(batch[i])) {
                new_visible.push_back(first_new + i);
            }
        }

        const int first_row = static_cast<int>(visible_.size());
        const int last_row = first_row + static_cast<int>(new_visible.size()) - 1;
        if (last_row >= first_row) {
            beginInsertRows(QModelIndex(), first_row, last_row);
        }
        records_.insert(records_.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        visible_.insert(visible_.end(), new_visible.begin(), new_visible.end());
        if (last_row >= first_row) {
            endInsertRows();
        }
    }

    // 从头部裁剪 n 条（超限淘汰最早的）。
    //
    // 返回实际裁掉的数量。裁剪是 beginRemoveRows 而不是"整表重建"：视图只重算
    // 受影响的行，10 万行下这两者的差别是"几毫秒"与"卡住主线程"。
    std::size_t TrimFront(std::size_t n) {
        if (n == 0 || n > records_.size()) {
            n = std::min(n, records_.size());
        }
        if (n == 0)
            return 0;

        // visible_ 是严格递增的记录索引序列，被裁掉的那一段落在最前面。
        std::size_t removed_visible = 0;
        while (removed_visible < visible_.size() && visible_[removed_visible] < n) {
            ++removed_visible;
        }
        const int first_row = 0;
        const int last_row = static_cast<int>(removed_visible) - 1;
        if (last_row >= first_row) {
            beginRemoveRows(QModelIndex(), first_row, last_row);
        }
        visible_.erase(visible_.begin(), visible_.begin() + static_cast<long>(removed_visible));
        for (auto& index : visible_)
            index -= n;
        records_.erase(records_.begin(), records_.begin() + static_cast<long>(n));
        if (last_row >= first_row) {
            endRemoveRows();
        }
        PerfMetrics::GetInstance().RecordCount("table_records_trimmed", static_cast<long long>(n));
        return n;
    }

    void Clear() {
        if (records_.empty() && visible_.empty())
            return;
        beginResetModel();
        records_.clear();
        visible_.clear();
        endResetModel();
    }

    // ---- 筛选 ----

    // filter 为空 = 不过滤。设置后会按新条件重算可见行（整表 reset ——
    // 筛选是用户主动切换的低频操作，不值得为它维护增量）。
    void SetFilter(std::function<bool(const Record&)> filter) {
        filter_ = std::move(filter);
        RebuildVisible();
    }

    bool HasFilter() const {
        return static_cast<bool>(filter_);
    }

    // ---- 记录索引 <-> 行号 ----

    // 记录索引对应的行号；该记录被筛掉时返回 -1。
    int RowOfRecord(std::size_t record_index) const {
        const auto it = std::lower_bound(visible_.begin(), visible_.end(), record_index);
        if (it == visible_.end() || *it != record_index)
            return -1;
        return static_cast<int>(std::distance(visible_.begin(), it));
    }

    // 行号对应的记录索引；row 越界返回 records_.size()（= 无效）。
    std::size_t RecordIndexOfRow(int row) const {
        if (row < 0 || static_cast<std::size_t>(row) >= visible_.size())
            return records_.size();
        return visible_[static_cast<std::size_t>(row)];
    }

    // 取某一行的记录（越界返回 nullptr）
    const Record* RecordAt(int row) const {
        const std::size_t index = RecordIndexOfRow(row);
        return index < records_.size() ? &records_[index] : nullptr;
    }

private:
    void RebuildVisible() {
        beginResetModel();
        visible_.clear();
        visible_.reserve(records_.size());
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (!filter_ || filter_(records_[i]))
                visible_.push_back(i);
        }
        endResetModel();
    }

    std::vector<ColumnSpec> columns_;
    std::vector<Record> records_;
    // 通过筛选的记录索引，严格递增。用它而不是"在 data() 里现扫一遍"：
    // 后者会让 rowCount 与 data 的语义对不上（筛掉的行到底算不算一行）。
    std::vector<std::size_t> visible_;
    std::function<bool(const Record&)> filter_;
};

} // namespace ui
} // namespace videoeye
