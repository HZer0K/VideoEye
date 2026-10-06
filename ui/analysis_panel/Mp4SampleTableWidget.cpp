#include "ui/analysis_panel/Mp4SampleTableWidget.h"

#include <QAbstractItemView>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/Mp4ConsistencyIssue.h"

#include <QColor>
#include <QDateTime>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>

namespace videoeye {
namespace ui {

Mp4SampleTableWidget::Mp4SampleTableWidget(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void Mp4SampleTableWidget::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    // 顶部: 轨道选择 + 联动提示
    QHBoxLayout* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(new QLabel(tr("轨道:"), this));
    track_combo_ = new QComboBox(this);
    track_combo_->setMinimumWidth(240);
    track_combo_->setToolTip(tr("选择要展开样本的轨道（视频/音频/字幕等）"));
    top->addWidget(track_combo_);
    top->addStretch();
    focus_label_ = new QLabel(tr("点击左侧结构树的 stts/ctts/stss/stco/stsz 可联动本表"), this);
    focus_label_->setStyleSheet("font-size: 11px; color: #8B949E;");
    top->addWidget(focus_label_);
    export_button_ = new QPushButton(tr("导出样本 CSV"), this);
    export_button_->setToolTip(tr("把当前轨道的样本表导出为 CSV（含偏移/DTS/PTS/大小/关键帧）"));
    top->addWidget(export_button_);
    layout->addLayout(top);

    summary_label_ = new QLabel(tr("样本表：未分析"), this);
    summary_label_->setStyleSheet("font-size: 11px; color: #8B949E; padding: 2px;");
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    sample_table_ = new QTableWidget(0, 9, this);
    sample_table_->setHorizontalHeaderLabels({
        tr("#"), tr("偏移"), tr("DTS(s)"), tr("PTS(s)"), tr("ΔCTS(ms)"),
        tr("时长(ms)"), tr("大小(B)"), tr("Chunk"), tr("关键帧")});
    sample_table_->horizontalHeader()->setStretchLastSection(true);
    sample_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sample_table_->setAlternatingRowColors(true);
    sample_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    sample_table_->setStyleSheet(
        "QTableWidget { background-color: #0D1117; alternate-background-color: #12181F; color: #F0F6FC; }"
        "QHeaderView::section { background-color: #161B22; color: #8B949E; padding: 3px; border: none; }");

    // 底部: 一致性问题 + 分片列表
    QTabWidget* bottom = new QTabWidget(this);
    issue_table_ = new QTableWidget(0, 5, bottom);
    issue_table_->setHorizontalHeaderLabels({tr("级别"), tr("位置"), tr("问题"), tr("说明"), tr("建议")});
    issue_table_->horizontalHeader()->setStretchLastSection(true);
    issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issue_table_->setAlternatingRowColors(true);
    issue_table_->verticalHeader()->setVisible(false);
    bottom->addTab(issue_table_, tr("一致性问题"));

    fragment_table_ = new QTableWidget(0, 8, bottom);
    fragment_table_->setHorizontalHeaderLabels({
        tr("#"), tr("序号"), tr("Track"), tr("偏移"), tr("tfdt"),
        tr("时长"), tr("样本数"), tr("字节数")});
    fragment_table_->horizontalHeader()->setStretchLastSection(true);
    fragment_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fragment_table_->setAlternatingRowColors(true);
    fragment_table_->verticalHeader()->setVisible(false);
    bottom->addTab(fragment_table_, tr("分片 (moof)"));

    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(sample_table_);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

    connect(track_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Mp4SampleTableWidget::OnTrackChanged);
    connect(export_button_, &QPushButton::clicked,
            this, &Mp4SampleTableWidget::OnExportCsv);
}

void Mp4SampleTableWidget::SetSamples(const model::Mp4SampleTableResult& samples) {
    samples_ = samples;
    focus_box_.clear();
    {
        const QSignalBlocker blocker(track_combo_);
        track_combo_->clear();
        for (const auto& t : samples_.tracks) {
            QString label = QString("Track %1 (%2").arg(t.track_id)
                                .arg(QString::fromStdString(t.type));
            if (!t.codec.empty()) {
                label += " / " + QString::fromStdString(t.codec);
            }
            label += QString(") · %1 样本").arg(t.stsz_sample_count);
            track_combo_->addItem(label, static_cast<quint32>(t.track_id));
        }
    }
    UpdateSummary();
    RebuildSampleTable();
    RebuildIssueTable();
    RebuildFragmentTable();
}

void Mp4SampleTableWidget::SetFocusBox(const QString& box) {
    focus_box_ = box;
    const bool is_fragment_box = (box == "moof" || box == "traf" || box == "tfhd" ||
                                  box == "tfdt" || box == "trun" || box == "mfhd");
    if (is_fragment_box) {
        focus_label_->setText(tr("已联动：%1（分片信息见下方「分片 (moof)」）").arg(box));
    } else {
        focus_label_->setText(tr("已联动：%1（高亮关联列）").arg(box));
    }
    RebuildSampleTable();
}

void Mp4SampleTableWidget::ShowFragmentTab() {
    if (auto* bottom = qobject_cast<QTabWidget*>(fragment_table_->parentWidget())) {
        bottom->setCurrentWidget(fragment_table_);
    }
}

void Mp4SampleTableWidget::UpdateSummary() {
    const auto& r = samples_;
    if (!r.valid) {
        summary_label_->setText(
            tr("样本表：未分析（文件不是 MP4/MOV，或容器解析失败）"));
        summary_label_->setStyleSheet("font-size: 11px; color: #8B949E; padding: 2px;");
        return;
    }

    QString order;
    for (const auto& box : r.top_level_order) {
        if (!order.isEmpty()) order += " → ";
        order += QString::fromStdString(box);
    }
    const int errors = r.CountIssues(model::IssueSeverity::Error) +
                       r.CountIssues(model::IssueSeverity::Critical);
    const int warnings = r.CountIssues(model::IssueSeverity::Warning);
    const int infos = r.CountIssues(model::IssueSeverity::Info);

    summary_label_->setText(
        QString("文件 %1 字节 | 顶层: %2 | moov: %3 | faststart: %4 | 分片: %5 (sidx %6 / styp %7) | "
                "问题: 错误 %8 / 警告 %9 / 提示 %10")
            .arg(QString::number(static_cast<quint64>(r.file_size)))
            .arg(order.isEmpty() ? "-" : order)
            .arg(r.moov_size > 0 ? QString("0x%1 (%2 B)")
                                       .arg(static_cast<quint64>(r.moov_offset), 0, 16)
                                       .arg(QString::number(static_cast<quint64>(r.moov_size)))
                                 : tr("无"))
            .arg(r.IsFastStart() ? tr("是") : tr("否"))
            .arg(r.fragments.size())
            .arg(r.sidx_count)
            .arg(r.styp_count)
            .arg(errors)
            .arg(warnings)
            .arg(infos));
    summary_label_->setStyleSheet(
        errors > 0 ? "font-size: 11px; color: #F85149; padding: 2px;"
                   : (warnings > 0 ? "font-size: 11px; color: #D29922; padding: 2px;"
                                   : "font-size: 11px; color: #3FB950; padding: 2px;"));
}

void Mp4SampleTableWidget::RebuildSampleTable() {
    sample_table_->setRowCount(0);
    if (!samples_.valid) return;

    const int track_index = track_combo_->currentIndex();
    if (track_index < 0 || track_index >= static_cast<int>(samples_.tracks.size())) return;
    const model::Mp4TrackSampleTable& track = samples_.tracks[track_index];
    if (track.samples.empty()) return;

    // 结构树点了 stss 时只看关键帧
    const bool keyframe_only = (focus_box_ == "stss");
    // 结构树联动的列高亮
    QVector<int> focus_cols;
    if (focus_box_ == "stts")       focus_cols = {2, 5};
    else if (focus_box_ == "ctts")  focus_cols = {3, 4};
    else if (focus_box_ == "stsz")  focus_cols = {6};
    else if (focus_box_ == "stsc")  focus_cols = {7};
    else if (focus_box_ == "stco" ||
             focus_box_ == "co64")  focus_cols = {1, 7};
    else if (focus_box_ == "stss")  focus_cols = {8};

    constexpr int kMaxSampleRows = 5000;  // UI 安全上限（样本本身可按 options 截断）

    // 先算出实际要显示多少行，一次性 setRowCount：
    // 逐行 insertRow 在几千行时是 O(n^2)，实测 5000 行要 200ms+。
    int visible = 0;
    for (const auto& s : track.samples) {
        if (keyframe_only && !s.keyframe) continue;
        if (visible >= kMaxSampleRows) break;
        ++visible;
    }
    TableBatch table(sample_table_);
    table.SetRowCount(visible);

    int row = 0;
    for (const auto& s : track.samples) {
        if (keyframe_only && !s.keyframe) continue;
        if (row >= kMaxSampleRows) break;

        auto set = [&](int col, const QString& text) {
            return table.SetText(row, col, text);
        };
        set(0, QString::number(s.index));
        set(1, QString("0x%1").arg(static_cast<quint64>(s.offset), 0, 16));
        set(2, QString::number(s.DtsSeconds(track.media_timescale), 'f', 4));
        set(3, QString::number(s.CtsSeconds(track.media_timescale), 'f', 4));
        set(4, track.media_timescale > 0
                   ? QString::number(static_cast<double>(s.cts_delta) * 1000.0 /
                                         track.media_timescale, 'f', 2)
                   : QString::number(s.cts_delta));
        set(5, track.media_timescale > 0
                   ? QString::number(static_cast<double>(s.duration) * 1000.0 /
                                         track.media_timescale, 'f', 2)
                   : QString::number(s.duration));
        set(6, QString::number(static_cast<quint64>(s.size)));
        set(7, QString("%1.%2").arg(s.chunk_index).arg(s.index_in_chunk));
        set(8, s.keyframe ? QString::fromUtf8("\u2713") : QString());

        // 异常行着色: 红=会导致读不到数据/解码错乱，黄=可能影响兼容与体验
        QStringList reasons;
        bool error_row = false, warn_row = false;
        auto note = [&reasons](const char* text) { reasons << QString::fromUtf8(text); };
        if (s.HasFlag(model::Mp4SampleFlags::kOffsetOutOfRange)) {
            error_row = true; note("偏移越过文件末尾");
        }
        if (s.HasFlag(model::Mp4SampleFlags::kDtsNotMonotonic)) {
            error_row = true; note("DTS 回退");
        }
        if (s.HasFlag(model::Mp4SampleFlags::kNegativeCts)) { warn_row = true; note("合成时间为负"); }
        if (s.HasFlag(model::Mp4SampleFlags::kZeroSize)) { warn_row = true; note("大小为 0"); }
        if (s.HasFlag(model::Mp4SampleFlags::kZeroDuration)) { warn_row = true; note("时长为 0"); }
        if (s.HasFlag(model::Mp4SampleFlags::kChunkDiscontinuity)) {
            warn_row = true; note("chunk 内偏移不连续");
        }

        if (error_row || warn_row) {
            const QColor bg = error_row ? QColor("#5C1F1F") : QColor("#5C4A1F");
            for (int c = 0; c < sample_table_->columnCount(); ++c) {
                auto* item = sample_table_->item(row, c);
                if (item) item->setBackground(bg);
            }
            for (int c = 0; c < sample_table_->columnCount(); ++c) {
                auto* item = sample_table_->item(row, c);
                if (item) item->setToolTip(reasons.join(" / "));
            }
        }
        // 结构树联动的高亮列
        for (int col : focus_cols) {
            auto* item = sample_table_->item(row, col);
            if (item) item->setForeground(QColor("#58A6FF"));
        }
        ++row;
    }

    // resizeColumnsToContents 要逐行算文本宽度 (5000 行 × 9 列 ≈ 300ms),
    // 行数多时改用固定列宽, 把主线程还给用户。
    if (sample_table_->rowCount() <= 1000) {
        sample_table_->resizeColumnsToContents();
    } else {
        static const int kColumnWidths[] = {70, 110, 90, 90, 80, 80, 80, 90, 60};
        for (int c = 0; c < sample_table_->columnCount() && c < 9; ++c) {
            sample_table_->setColumnWidth(c, kColumnWidths[c]);
        }
    }
}

void Mp4SampleTableWidget::RebuildIssueTable() {
    issue_table_->setRowCount(0);
    if (!samples_.valid) return;

    int row = 0;
    for (const auto& issue : samples_.issues) {
        issue_table_->insertRow(row);
        auto set = [&](int col, const QString& text) {
            issue_table_->setItem(row, col, new QTableWidgetItem(text));
        };
        set(0, QString::fromUtf8(model::ToString(issue.severity)));

        QString where = tr("文件级");
        if (issue.track_id >= 0) {
            where = QString("Track %1").arg(issue.track_id);
        } else if (issue.fragment_index >= 0) {
            where = QString("分片 #%1").arg(issue.fragment_index);
        }
        if (issue.has_sample_index) where += QString(" @样本#%1").arg(issue.sample_index);
        if (issue.occurrence_count > 1) where += QString(" ×%1").arg(issue.occurrence_count);
        set(1, where);

        set(2, QString::fromStdString(issue.title));
        set(3, QString::fromStdString(issue.detail));
        set(4, QString::fromStdString(issue.suggestion));

        const QColor fg = (issue.severity == model::IssueSeverity::Error ||
                           issue.severity == model::IssueSeverity::Critical)
                              ? QColor("#F85149")
                              : (issue.severity == model::IssueSeverity::Warning
                                     ? QColor("#D29922")
                                     : QColor("#8B949E"));
        for (int c = 0; c < issue_table_->columnCount(); ++c) {
            if (auto* item = issue_table_->item(row, c)) item->setForeground(fg);
        }
        ++row;
    }
    issue_table_->resizeColumnsToContents();
}

void Mp4SampleTableWidget::RebuildFragmentTable() {
    fragment_table_->setRowCount(0);
    if (!samples_.valid || samples_.fragments.empty()) return;

    int row = 0;
    for (const auto& f : samples_.fragments) {
        fragment_table_->insertRow(row);
        auto set = [&](int col, const QString& text) {
            fragment_table_->setItem(row, col, new QTableWidgetItem(text));
        };
        const model::Mp4TrackSampleTable* track = samples_.FindTrack(f.track_id);
        const uint32_t ts = track ? track->media_timescale : samples_.movie_timescale;
        set(0, QString::number(f.index));
        set(1, QString::number(f.sequence_number));
        set(2, QString::number(f.track_id));
        set(3, QString("0x%1").arg(static_cast<quint64>(f.offset), 0, 16));
        set(4, f.has_tfdt ? QString::number(static_cast<quint64>(f.base_media_decode_time))
                          : tr("缺失"));
        set(5, ts > 0 ? QString("%1 (%2 s)")
                            .arg(QString::number(static_cast<quint64>(f.duration)))
                            .arg(QString::number(static_cast<double>(f.duration) / ts, 'f', 3))
                      : QString::number(static_cast<quint64>(f.duration)));
        set(6, QString::number(f.sample_count));
        set(7, QString::number(static_cast<quint64>(f.total_size)));
        if (!f.has_tfdt || (!f.base_data_offset_present && !f.default_base_is_moof &&
                            !f.trun_data_offset_present)) {
            for (int c = 0; c < fragment_table_->columnCount(); ++c) {
                if (auto* item = fragment_table_->item(row, c)) {
                    item->setBackground(QColor("#5C4A1F"));
                }
            }
        }
        ++row;
    }
    fragment_table_->resizeColumnsToContents();
}

void Mp4SampleTableWidget::OnTrackChanged(int) {
    RebuildSampleTable();
}

void Mp4SampleTableWidget::OnExportCsv() {
    if (!samples_.valid || samples_.tracks.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 MP4 样本数据。"));
        return;
    }
    const int track_index = track_combo_->currentIndex();
    if (track_index < 0 || track_index >= static_cast<int>(samples_.tracks.size())) return;
    const model::Mp4TrackSampleTable& track = samples_.tracks[track_index];

    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出 MP4 样本表 CSV"),
        QString("videoeye_mp4_samples_track%1_%2.csv")
            .arg(track.track_id)
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("CSV 文件 (*.csv);;所有文件 (*)"));
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "index,offset,dts,pts,cts_delta,duration,size,chunk,index_in_chunk,keyframe,flags\n";
    const double ts = static_cast<double>(track.media_timescale);
    for (const auto& s : track.samples) {
        out << s.index << ','
            << s.offset << ','
            << QString::number(ts > 0 ? s.dts / ts : 0.0, 'f', 6) << ','
            << QString::number(ts > 0 ? s.cts / ts : 0.0, 'f', 6) << ','
            << s.cts_delta << ','
            << s.duration << ','
            << s.size << ','
            << s.chunk_index << ','
            << s.index_in_chunk << ','
            << (s.keyframe ? 1 : 0) << ','
            << s.flags << '\n';
    }
    file.close();
    QMessageBox::information(this, tr("导出完成"),
                             tr("已导出 %1 个样本到:\n%2").arg(track.samples.size()).arg(filename));
}

} // namespace ui
} // namespace videoeye
