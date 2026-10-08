#include "ui/analysis_panel/SubtitleAuxPage.h"

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

#include "infrastructure/logging/ScopedTimer.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"

#include "core/domain/model/AuxiliaryDataInfo.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "core/domain/model/SubtitleCueInfo.h"
#include "core/domain/model/TimecodeInfo.h"

#include <QColor>
#include <QSignalBlocker>

namespace videoeye {
namespace ui {

SubtitleAuxPage::SubtitleAuxPage(QWidget* parent) : QWidget(parent) {
    SetupUi();
}


// ============================================================
// 字幕 / 时码 / 辅助数据页（功能 9）
//
// 数据全部来自「诊断与报告」那一次全文件扫描（AnalysisResult 的 subtitle /
// timecode / aux_data），这一页不再单独发起 demux —— 字幕包本来就在同一次
// demux 里过了一遍，重复扫一遍纯属浪费 I/O。
// ============================================================

namespace {

// 统一建表: 列宽策略 / 选择行为 / 表头字号在本面板各页保持一致
QTableWidget* MakeAuxTable(const QStringList& headers, QWidget* parent) {
    QTableWidget* table = new QTableWidget(parent);
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

// CSV 字段转义（与导出报告其它页保持一致：含分隔符/引号/换行才加引号）
QString AuxCsvField(const QString& text) {
    if (text.contains(',') || text.contains('"') || text.contains('\n')) {
        QString out = text;
        out.replace('"', QStringLiteral("\"\""));
        return QLatin1Char('"') + out + QLatin1Char('"');
    }
    return text;
}

QString AuxSecondsText(double seconds) {
    if (seconds < 0.0) return QStringLiteral("-");
    return QString::number(seconds, 'f', 3);
}

}  // namespace

void SubtitleAuxPage::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 顶部: 汇总 + 开始分析
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        summary_label_ = new QLabel(
            tr("未分析：点「开始分析」对当前文件做一次全文件扫描（与「诊断与报告」共用同一次扫描结果）。"),
            row);
        summary_label_->setWordWrap(true);
        rl->addWidget(summary_label_, 1);
        start_button_ = new QPushButton(tr("开始分析"), row);
        start_button_->setToolTip(
            tr("字幕 cue、时码轨、章节、SCTE-35 与 metadata 都来自同一次全文件 demux，不额外读一遍文件。"));
        connect(start_button_, &QPushButton::clicked,
                this, &SubtitleAuxPage::OnStartAnalysis);
        rl->addWidget(start_button_);
        layout->addWidget(row);
    }

    sub_tabs_ = new QTabWidget(this);
    layout->addWidget(sub_tabs_, 1);

    // ---------- 子页 1: 字幕 ----------
    {
        QWidget* page = new QWidget();
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        pl->setSpacing(4);

        QWidget* filter_row = new QWidget(page);
        QHBoxLayout* fl = new QHBoxLayout(filter_row);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->addWidget(new QLabel(tr("字幕流:"), filter_row));
        stream_combo_ = new QComboBox(filter_row);
        stream_combo_->setMinimumWidth(220);
        connect(stream_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &SubtitleAuxPage::OnSubtitleStreamChanged);
        fl->addWidget(stream_combo_);

        issues_only_check_ = new QCheckBox(tr("只看有问题的"), filter_row);
        connect(issues_only_check_, &QCheckBox::toggled,
                this, [this](bool) { RebuildSubtitleCueTable(); });
        fl->addWidget(issues_only_check_);

        fl->addStretch();
        QPushButton* export_csv = new QPushButton(tr("导出 cue CSV"), filter_row);
        connect(export_csv, &QPushButton::clicked, this, &SubtitleAuxPage::OnExportSubtitleCsv);
        fl->addWidget(export_csv);
        pl->addWidget(filter_row);

        stream_table_ = MakeAuxTable(
            {tr("流"), tr("编码"), tr("格式"), tr("承载"), tr("语言"), tr("handler"), tr("默认"),
             tr("强制"), tr("Cue 数"), tr("包数"), tr("说明")},
            page);
        stream_table_->setMaximumHeight(150);
        pl->addWidget(stream_table_);

        cue_table_ = MakeAuxTable(
            {tr("#"), tr("开始"), tr("结束"), tr("时长"), tr("字符"), tr("语言"), tr("问题"), tr("文本")},
            page);
        cue_table_->setMinimumHeight(240);
        cue_table_->setToolTip(tr("点击一行可让播放器跳到该字幕的开始时间"));
        connect(cue_table_, &QTableWidget::cellClicked,
                this, &SubtitleAuxPage::OnCueCellClicked);
        pl->addWidget(cue_table_, 1);

        sub_tabs_->addTab(page, tr("字幕"));
    }

    // ---------- 子页 2: 时码与章节 ----------
    {
        QWidget* page = new QWidget();
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        pl->setSpacing(4);

        timecode_table_ = MakeAuxTable({tr("项目"), tr("值"), tr("说明")}, page);
        timecode_table_->setMaximumHeight(200);
        pl->addWidget(timecode_table_);

        pl->addWidget(new QLabel(tr("章节时间线"), page));
        chapter_table_ = MakeAuxTable(
            {tr("#"), tr("起点"), tr("终点"), tr("时长"), tr("标题"), tr("语言"), tr("问题")}, page);
        pl->addWidget(chapter_table_, 1);

        QPushButton* export_csv = new QPushButton(tr("导出时码与章节 CSV"), page);
        connect(export_csv, &QPushButton::clicked, this, &SubtitleAuxPage::OnExportTimecodeCsv);
        pl->addWidget(export_csv, 0, Qt::AlignLeft);

        sub_tabs_->addTab(page, tr("时码与章节"));
    }

    // ---------- 子页 3: 辅助数据与 SCTE-35 ----------
    {
        QWidget* page = new QWidget();
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        pl->setSpacing(4);

        aux_stream_table_ = MakeAuxTable(
            {tr("流"), tr("类型"), tr("编码"), tr("tag"), tr("handler"), tr("语言"), tr("包数"),
             tr("字节"), tr("说明")},
            page);
        aux_stream_table_->setMaximumHeight(150);
        pl->addWidget(aux_stream_table_);

        // SCTE-35 标记图: 横轴时间, 每个点是一条 cue（OUT 在上方, IN 在下方）
        scte35_marker_chart_ = new MetricChartWidget(page);
        scte35_marker_chart_->setMinimumHeight(150);
        scte35_marker_chart_->SetTitle(tr("SCTE-35 插入点时间轴"));
        scte35_marker_series_ = scte35_marker_chart_->AddScatterSeries(tr("cue"), QColor("#e53935"));
        scte35_marker_series_->SetPointsVisible(true);
        scte35_marker_axis_x_ = scte35_marker_chart_->AxisX();
        scte35_marker_axis_y_ = scte35_marker_chart_->AxisY();
        if (scte35_marker_axis_x_) scte35_marker_axis_x_->SetTitleText(tr("时间 (秒)"));
        if (scte35_marker_axis_y_) scte35_marker_axis_y_->SetTitleText(tr("OUT / IN"));
        pl->addWidget(scte35_marker_chart_);

        scte35_table_ = MakeAuxTable(
            {tr("#"), tr("时间"), tr("命令"), tr("Event ID"), tr("OUT/IN"), tr("splice 时间"),
             tr("时长"), tr("分段类型"), tr("分段时长"), tr("UPID"), tr("CRC")},
            page);
        scte35_table_->setMinimumHeight(200);
        pl->addWidget(scte35_table_, 1);

        QPushButton* export_csv = new QPushButton(tr("导出 SCTE-35 CSV"), page);
        connect(export_csv, &QPushButton::clicked, this, &SubtitleAuxPage::OnExportScte35Csv);
        pl->addWidget(export_csv, 0, Qt::AlignLeft);

        sub_tabs_->addTab(page, tr("辅助数据与 SCTE-35"));
    }

    // ---------- 子页 4: metadata ----------
    {
        QWidget* page = new QWidget();
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        pl->setSpacing(4);

        metadata_table_ = MakeAuxTable({tr("作用域"), tr("键"), tr("值")}, page);
        pl->addWidget(metadata_table_, 1);

        QPushButton* export_csv = new QPushButton(tr("导出 metadata CSV"), page);
        connect(export_csv, &QPushButton::clicked, this, &SubtitleAuxPage::OnExportMetadataCsv);
        pl->addWidget(export_csv, 0, Qt::AlignLeft);

        sub_tabs_->addTab(page, tr("metadata"));
    }

}

void SubtitleAuxPage::ApplyRuleThresholds(const std::vector<model::QcRule>& rules,
                                         videoeye::SubtitleOptions& options) const {
    if (const model::QcRule* rule = model::FindQcRule(rules, "subtitle.cue_too_short")) {
        if (rule->threshold > 0.0) options.min_cue_duration_seconds = rule->threshold;
    }
    if (const model::QcRule* rule = model::FindQcRule(rules, "subtitle.cue_too_long")) {
        if (rule->threshold > 0.0) options.max_cue_duration_seconds = rule->threshold;
    }
    if (const model::QcRule* rule = model::FindQcRule(rules, "subtitle.cue_too_fast")) {
        if (rule->threshold > 0.0) options.max_chars_per_second = rule->threshold;
    }
}

void SubtitleAuxPage::OnStartAnalysis() {
    emit ScanRequested();
}

void SubtitleAuxPage::SetScanActive(bool active) {
    scan_active_ = active;
    if (start_button_) start_button_->setEnabled(!active);
}

int SubtitleAuxPage::CurrentSubtitleStreamIndex() const {
    if (stream_combo_ == nullptr) return -1;
    return stream_combo_->currentData().toInt();
}

void SubtitleAuxPage::OnSubtitleStreamChanged(int /*index*/) {
    RebuildSubtitleCueTable();
}

void SubtitleAuxPage::RefreshUi() {
    UpdateSummary();
    RebuildSubtitleStreamTable();
    RebuildSubtitleCueTable();
    RebuildTimecodeTable();
    RebuildChapterTable();
    RebuildAuxStreamTable();
    RebuildScte35Table();
    RebuildMetadataTable();
    UpdateScte35MarkerChart();

    // 拿到素材自带起始时码就同步给播放器（时间轴旁的 SMPTE 时码以它为基准）
    if (result_.timecode_analyzed && result_.timecode.has_primary) {
        emit StartTimecodeReady(
            QString::fromStdString(result_.timecode.primary.ToString()),
            result_.timecode.primary_frame_rate);
    }
}

void SubtitleAuxPage::UpdateSummary() {
    if (summary_label_ == nullptr) return;
    const model::AnalysisResult& r = result_;

    if (!r.subtitle_analyzed && !r.timecode_analyzed && !r.aux_data_analyzed) {
        summary_label_->setText(
            tr("未分析：点「开始分析」对当前文件做一次全文件扫描。"));
        return;
    }

    QStringList parts;
    parts << tr("字幕流 %1 条（文本 %2 / 图形 %3），cue %4 条，问题 %5 处")
                 .arg(r.subtitle.streams.size())
                 .arg(r.subtitle.text_stream_count)
                 .arg(r.subtitle.bitmap_stream_count)
                 .arg(r.subtitle.cues.size())
                 .arg(r.subtitle.issues.size());
    if (r.timecode_analyzed) {
        parts << (r.timecode.has_primary
                      ? tr("首帧时码 %1 @ %2 fps%3")
                            .arg(QString::fromStdString(r.timecode.primary.ToString()))
                            .arg(r.timecode.primary_frame_rate, 0, 'f', 3)
                            .arg(r.timecode.primary_drop_frame ? tr("（drop-frame）") : tr(""))
                      : tr("未找到时码"));
    }
    parts << tr("章节 %1 个，问题 %2 处")
                 .arg(r.timecode.chapters.size())
                 .arg(r.timecode.chapter_issues.size());
    parts << tr("SCTE-35 cue %1 条（OUT %2 / IN %3）")
                 .arg(r.aux_data.scte35_cue_count)
                 .arg(r.aux_data.scte35_out_count)
                 .arg(r.aux_data.scte35_in_count);
    parts << tr("metadata %1 条").arg(r.aux_data.metadata.size());

    summary_label_->setText(parts.join(tr(" ｜ ")));
}

void SubtitleAuxPage::RebuildSubtitleStreamTable() {
    if (stream_table_ == nullptr) return;
    const model::AnalysisResult& r = result_;

    // 流下拉: 重建时保留"全部"选项
    const int previous = CurrentSubtitleStreamIndex();
    {
        QSignalBlocker blocker(stream_combo_);
        stream_combo_->clear();
        stream_combo_->addItem(tr("全部字幕流"), -1);
        for (const model::SubtitleStreamInfo& s : r.subtitle.streams) {
            stream_combo_->addItem(
                tr("流 %1 · %2 · %3")
                    .arg(s.stream_index)
                    .arg(QString::fromStdString(model::ToString(s.format)))
                    .arg(s.language.empty() ? tr("无语言") : QString::fromStdString(s.language)),
                s.stream_index);
        }
        const int restore = stream_combo_->findData(previous);
        stream_combo_->setCurrentIndex(restore >= 0 ? restore : 0);
    }

    stream_table_->setRowCount(static_cast<int>(r.subtitle.streams.size()));
    int row = 0;
    for (const model::SubtitleStreamInfo& s : r.subtitle.streams) {
        int col = 0;
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::number(s.stream_index)));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.codec_name)));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(model::ToString(s.format))));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(model::ToString(s.kind))));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.language)));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.handler_name)));
        stream_table_->setItem(row, col++, new QTableWidgetItem(s.default_disposition ? tr("是") : tr("否")));
        stream_table_->setItem(row, col++, new QTableWidgetItem(s.forced ? tr("是") : tr("否")));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::number(s.cue_count)));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::number(s.packet_count)));
        stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.note)));
        ++row;
    }
    stream_table_->resizeColumnsToContents();
}

void SubtitleAuxPage::RebuildSubtitleCueTable() {
    if (cue_table_ == nullptr) return;
    const model::AnalysisResult& r = result_;
    const int stream_filter = CurrentSubtitleStreamIndex();
    const bool issues_only = (issues_only_check_ != nullptr) &&
                             issues_only_check_->isChecked();

    // 先按条件筛出要展示的 cue
    std::vector<const model::SubtitleCue*> rows;
    for (const model::SubtitleCue& cue : r.subtitle.cues) {
        if (stream_filter >= 0 && cue.stream_index != stream_filter) continue;
        if (issues_only && !cue.has_issue) continue;
        rows.push_back(&cue);
    }

    cue_table_->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const model::SubtitleCue& cue = *rows[static_cast<size_t>(row)];
        // 该 cue 上挂了哪些问题（同一条可能命中多条规则）
        QStringList problems;
        for (const model::SubtitleIssue& issue : r.subtitle.issues) {
            if (issue.stream_index == cue.stream_index && issue.cue_index == cue.index) {
                problems << QString::fromStdString(model::ToString(issue.type));
            }
        }

        int col = 0;
        cue_table_->setItem(row, col++, new QTableWidgetItem(QString::number(cue.index + 1)));
        cue_table_->setItem(row, col++, new QTableWidgetItem(AuxSecondsText(cue.start_seconds)));
        cue_table_->setItem(row, col++, new QTableWidgetItem(AuxSecondsText(cue.end_seconds)));
        cue_table_->setItem(row, col++, new QTableWidgetItem(
            cue.duration_seconds > 0.0 ? QString::number(cue.duration_seconds, 'f', 3) : QStringLiteral("-")));
        cue_table_->setItem(row, col++, new QTableWidgetItem(QString::number(cue.char_count)));
        cue_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(cue.language)));
        cue_table_->setItem(row, col++, new QTableWidgetItem(problems.join(tr("、"))));
        cue_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(cue.text)));

        if (problems.isEmpty()) continue;
        // 有问题的行标红，跟"诊断与报告"里的问题色保持一致
        QColor tint("#5a1f1f");
        for (int c = 0; c < cue_table_->columnCount(); ++c) {
            QTableWidgetItem* item = cue_table_->item(row, c);
            if (item != nullptr) item->setBackground(tint);
        }
    }
    cue_table_->resizeColumnsToContents();
}

void SubtitleAuxPage::RebuildTimecodeTable() {
    if (timecode_table_ == nullptr) return;
    const model::TimecodeAnalysisResult& tc = result_.timecode;

    struct Row {
        QString key;
        QString value;
        QString note;
    };
    std::vector<Row> rows;
    rows.push_back({tr("首帧时码"),
                    tc.has_primary ? QString::fromStdString(tc.primary.ToString()) : tr("无"),
                    tc.has_primary ? tr("优先取 MOV/MP4 tmcd 轨的首个样本，其次取 metadata timecode tag")
                                   : tr("既没有 tmcd 时码轨，也没有 metadata timecode tag")});
    rows.push_back({tr("帧率"),
                    tc.primary_frame_rate > 0.0 ? QString::number(tc.primary_frame_rate, 'f', 3) : tr("未知"),
                    tr("换算时码用的帧率（取第一条视频流）")});
    rows.push_back({tr("drop-frame"),
                    tc.primary_drop_frame ? tr("是") : tr("否"),
                    tr("29.97/59.94 素材应为 drop-frame，否则一小时会累积约 3.6 秒偏差")});

    for (const model::TimecodeTrack& track : tc.tracks) {
        rows.push_back({tr("时码源 %1").arg(track.stream_index >= 0 ? track.stream_index : 0),
                        track.first_timecode.valid
                            ? QString::fromStdString(track.first_timecode.ToString())
                            : tr("无"),
                        tr("%1%2%3")
                            .arg(QString::fromStdString(model::ToString(track.source)))
                            .arg(track.metadata_key.empty() ? QString()
                                                            : tr(" · tag=%1").arg(QString::fromStdString(track.metadata_key)))
                            .arg(track.note.empty() ? QString()
                                                    : tr(" · %1").arg(QString::fromStdString(track.note)))});
    }
    if (tc.tracks.empty()) {
        rows.push_back({tr("时码源"), tr("无"), tr("未发现 tmcd 轨或 timecode tag")});
    }

    timecode_table_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        timecode_table_->setItem(i, 0, new QTableWidgetItem(rows[static_cast<size_t>(i)].key));
        timecode_table_->setItem(i, 1, new QTableWidgetItem(rows[static_cast<size_t>(i)].value));
        timecode_table_->setItem(i, 2, new QTableWidgetItem(rows[static_cast<size_t>(i)].note));
    }
    timecode_table_->resizeColumnsToContents();
}

void SubtitleAuxPage::RebuildChapterTable() {
    if (chapter_table_ == nullptr) return;
    const model::TimecodeAnalysisResult& tc = result_.timecode;

    chapter_table_->setRowCount(static_cast<int>(tc.chapters.size()));
    for (int row = 0; row < static_cast<int>(tc.chapters.size()); ++row) {
        const model::ChapterInfo& ch = tc.chapters[static_cast<size_t>(row)];
        QStringList problems;
        for (const model::ChapterIssue& issue : tc.chapter_issues) {
            if (issue.chapter_index == ch.index) {
                problems << QString::fromStdString(model::ToString(issue.type));
            }
        }

        int col = 0;
        chapter_table_->setItem(row, col++, new QTableWidgetItem(QString::number(ch.index + 1)));
        chapter_table_->setItem(row, col++, new QTableWidgetItem(AuxSecondsText(ch.start_seconds)));
        chapter_table_->setItem(row, col++, new QTableWidgetItem(AuxSecondsText(ch.end_seconds)));
        chapter_table_->setItem(row, col++, new QTableWidgetItem(QString::number(ch.Duration(), 'f', 3)));
        chapter_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(ch.title)));
        chapter_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(ch.language)));
        chapter_table_->setItem(row, col++, new QTableWidgetItem(problems.join(tr("、"))));

        if (problems.isEmpty()) continue;
        QColor tint("#5a1f1f");
        for (int c = 0; c < chapter_table_->columnCount(); ++c) {
            QTableWidgetItem* item = chapter_table_->item(row, c);
            if (item != nullptr) item->setBackground(tint);
        }
    }
    chapter_table_->resizeColumnsToContents();
}

void SubtitleAuxPage::RebuildAuxStreamTable() {
    if (aux_stream_table_ == nullptr) return;
    const model::AuxiliaryDataResult& aux = result_.aux_data;

    aux_stream_table_->setRowCount(static_cast<int>(aux.streams.size()));
    for (int row = 0; row < static_cast<int>(aux.streams.size()); ++row) {
        const model::AuxDataStream& s = aux.streams[static_cast<size_t>(row)];
        int col = 0;
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::number(s.stream_index)));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(model::ToString(s.kind))));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.codec_name)));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.codec_tag)));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.handler_name)));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.language)));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::number(s.packet_count)));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::number(s.byte_count)));
        aux_stream_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(s.note)));
    }
    aux_stream_table_->resizeColumnsToContents();
}

void SubtitleAuxPage::RebuildScte35Table() {
    if (scte35_table_ == nullptr) return;
    const model::AuxiliaryDataResult& aux = result_.aux_data;

    scte35_table_->setRowCount(static_cast<int>(aux.cues.size()));
    for (int row = 0; row < static_cast<int>(aux.cues.size()); ++row) {
        const model::Scte35Cue& cue = aux.cues[static_cast<size_t>(row)];
        QString seg_type;
        QString seg_duration;
        QString upid;
        if (!cue.segmentation.empty()) {
            seg_type = QString::fromStdString(cue.segmentation.front().type_name);
            seg_duration = cue.segmentation.front().has_duration
                               ? QString::number(cue.segmentation.front().duration_seconds, 'f', 3)
                               : QStringLiteral("-");
            upid = QString::fromStdString(cue.segmentation.front().upid_summary);
        }

        int col = 0;
        scte35_table_->setItem(row, col++, new QTableWidgetItem(QString::number(cue.index + 1)));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(AuxSecondsText(cue.packet_pts_seconds)));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(QString::fromStdString(model::ToString(cue.command))));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(
            cue.has_event_id ? QStringLiteral("0x%1").arg(cue.event_id, 8, 16, QLatin1Char('0')) : QStringLiteral("-")));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(QString::fromUtf8(cue.NetworkIndicatorText())));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(
            cue.has_splice_time ? QString::number(cue.splice_time_seconds, 'f', 3) : QStringLiteral("-")));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(
            cue.has_duration ? QString::number(cue.break_duration_seconds, 'f', 3) : QStringLiteral("-")));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(seg_type));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(seg_duration));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(upid));
        scte35_table_->setItem(row, col++, new QTableWidgetItem(
            cue.crc_checked ? (cue.crc_valid ? tr("通过") : tr("失败")) : tr("未校验")));
    }
    scte35_table_->resizeColumnsToContents();
}

void SubtitleAuxPage::RebuildMetadataTable() {
    if (metadata_table_ == nullptr) return;
    const model::AuxiliaryDataResult& aux = result_.aux_data;

    metadata_table_->setRowCount(static_cast<int>(aux.metadata.size()));
    for (int row = 0; row < static_cast<int>(aux.metadata.size()); ++row) {
        const model::MetadataTagEntry& tag = aux.metadata[static_cast<size_t>(row)];
        metadata_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(tag.scope)));
        metadata_table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(tag.key)));
        metadata_table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(tag.value)));
    }
    metadata_table_->resizeColumnsToContents();
}

void SubtitleAuxPage::UpdateScte35MarkerChart() {
    if (scte35_marker_chart_ == nullptr || scte35_marker_series_ == nullptr) return;
    scte35_marker_series_->Clear();

    const model::AuxiliaryDataResult& aux = result_.aux_data;
    for (const model::Scte35Cue& cue : aux.cues) {
        const double t = cue.has_splice_time ? cue.splice_time_seconds : cue.packet_pts_seconds;
        if (t < 0.0) continue;
        // OUT（进广告）画在 1，IN（回节目）画在 0，一眼能看出插入点成对出现
        const double y = cue.out_of_network ? 1.0 : 0.0;
        scte35_marker_series_->Append(t, y);
    }

    double max_time = result_.duration_seconds;
    for (const model::Scte35Cue& cue : aux.cues) {
        const double t = cue.has_splice_time ? cue.splice_time_seconds : cue.packet_pts_seconds;
        if (t > max_time) max_time = t;
    }
    if (scte35_marker_axis_x_) {
        scte35_marker_axis_x_->SetRange(0.0, max_time > 0.0 ? max_time * 1.05 : 1.0);
    }
    if (scte35_marker_axis_y_) {
        scte35_marker_axis_y_->SetRange(-0.5, 1.5);
    }
    scte35_marker_chart_->update();
}

void SubtitleAuxPage::OnCueCellClicked(int row, int /*column*/) {
    if (cue_table_ == nullptr || row < 0) return;
    QTableWidgetItem* start_item = cue_table_->item(row, 1);
    if (start_item == nullptr) return;
    bool ok = false;
    const double seconds = start_item->text().toDouble(&ok);
    if (ok && seconds >= 0.0) {
        emit SeekRequested(seconds);
    }
}

void SubtitleAuxPage::OnExportSubtitleCsv() {
    const model::AnalysisResult& r = result_;
    if (r.subtitle.cues.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有字幕 cue 数据可导出"));
        return;
    }
    ExportCsvStream(this, tr("导出字幕 cue CSV"), QStringLiteral("subtitle_cues.csv"),
        [this, &r](QTextStream& out) {
            out << "流,序号,开始(秒),结束(秒),时长(秒),字符数,语言,问题代码,文本\n";
            for (const model::SubtitleCue& cue : r.subtitle.cues) {
                QStringList codes;
                for (const model::SubtitleIssue& issue : r.subtitle.issues) {
                    if (issue.stream_index == cue.stream_index && issue.cue_index == cue.index) {
                        codes << QString::fromUtf8(model::SubtitleIssueCode(issue.type));
                    }
                }
                out << cue.stream_index << ','
                    << cue.index + 1 << ','
                    << QString::number(cue.start_seconds, 'f', 3) << ','
                    << QString::number(cue.end_seconds, 'f', 3) << ','
                    << QString::number(cue.duration_seconds, 'f', 3) << ','
                    << cue.char_count << ','
                    << AuxCsvField(QString::fromStdString(cue.language)) << ','
                    << AuxCsvField(codes.join(QStringLiteral(" "))) << ','
                    << AuxCsvField(QString::fromStdString(cue.text)) << '\n';
            }
        },
        static_cast<int>(r.subtitle.cues.size()));
}

void SubtitleAuxPage::OnExportTimecodeCsv() {
    const model::TimecodeAnalysisResult& tc = result_.timecode;
    if (!tc.has_primary && tc.chapters.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有时码 / 章节数据可导出"));
        return;
    }
    ExportCsvStream(this, tr("导出时码与章节 CSV"), QStringLiteral("timecode_chapters.csv"),
        [this, &tc](QTextStream& out) {
            out << "章节序号,起点(秒),终点(秒),时长(秒),标题,语言,问题\n";
            for (const model::ChapterInfo& ch : tc.chapters) {
                QStringList problems;
                for (const model::ChapterIssue& issue : tc.chapter_issues) {
                    if (issue.chapter_index == ch.index) {
                        problems << QString::fromUtf8(model::ChapterIssueCode(issue.type));
                    }
                }
                out << ch.index + 1 << ','
                    << QString::number(ch.start_seconds, 'f', 3) << ','
                    << QString::number(ch.end_seconds, 'f', 3) << ','
                    << QString::number(ch.Duration(), 'f', 3) << ','
                    << AuxCsvField(QString::fromStdString(ch.title)) << ','
                    << AuxCsvField(QString::fromStdString(ch.language)) << ','
                    << AuxCsvField(problems.join(QStringLiteral(" "))) << '\n';
            }
        },
        static_cast<int>(tc.chapters.size()));
}

void SubtitleAuxPage::OnExportScte35Csv() {
    const model::AuxiliaryDataResult& aux = result_.aux_data;
    if (aux.cues.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有 SCTE-35 cue 数据可导出"));
        return;
    }
    ExportCsvStream(this, tr("导出 SCTE-35 CSV"), QStringLiteral("scte35_cues.csv"),
        [this, &aux](QTextStream& out) {
            out << "序号,流,包时间(秒),命令,Event ID,OUT/IN,splice时间(秒),时长(秒),"
                   "分段类型ID,分段时长(秒),CRC\n";
            for (const model::Scte35Cue& cue : aux.cues) {
                const model::Scte35Segmentation* seg =
                    cue.segmentation.empty() ? nullptr : &cue.segmentation.front();
                out << cue.index + 1 << ','
                    << cue.stream_index << ','
                    << QString::number(cue.packet_pts_seconds, 'f', 3) << ','
                    << QString::fromUtf8(model::Scte35CommandCode(cue.command)) << ','
                    << (cue.has_event_id ? QStringLiteral("0x%1").arg(cue.event_id, 8, 16, QLatin1Char('0'))
                                         : QStringLiteral("-")) << ','
                    << QString::fromUtf8(cue.NetworkIndicatorText()) << ','
                    << (cue.has_splice_time ? QString::number(cue.splice_time_seconds, 'f', 3)
                                            : QStringLiteral("-")) << ','
                    << (cue.has_duration ? QString::number(cue.break_duration_seconds, 'f', 3)
                                         : QStringLiteral("-")) << ','
                    << (seg != nullptr ? QStringLiteral("0x%1").arg(seg->type_id, 2, 16, QLatin1Char('0'))
                                       : QStringLiteral("-")) << ','
                    << (seg != nullptr && seg->has_duration ? QString::number(seg->duration_seconds, 'f', 3)
                                                            : QStringLiteral("-")) << ','
                    << (cue.crc_checked ? (cue.crc_valid ? QStringLiteral("ok") : QStringLiteral("bad"))
                                        : QStringLiteral("n/a")) << '\n';
            }
        },
        static_cast<int>(aux.cues.size()));
}

void SubtitleAuxPage::OnExportMetadataCsv() {
    const model::AuxiliaryDataResult& aux = result_.aux_data;
    if (aux.metadata.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有 metadata 数据可导出"));
        return;
    }
    ExportCsvStream(this, tr("导出 metadata CSV"), QStringLiteral("metadata_tags.csv"),
        [this, &aux](QTextStream& out) {
            out << "作用域,键,值\n";
            for (const model::MetadataTagEntry& tag : aux.metadata) {
                out << AuxCsvField(QString::fromStdString(tag.scope)) << ','
                    << AuxCsvField(QString::fromStdString(tag.key)) << ','
                    << AuxCsvField(QString::fromStdString(tag.value)) << '\n';
            }
        },
        static_cast<int>(aux.metadata.size()));
}

void SubtitleAuxPage::SetResult(const model::AnalysisResult& result) {
    result_ = result;
    RefreshUi();
}

} // namespace ui
} // namespace videoeye
