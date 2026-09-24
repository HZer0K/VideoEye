#include "ui/streaming_panel/StreamingPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace videoeye {
namespace ui {
namespace {

constexpr int kMaxSegmentNodes = 100; // 结构树里单个列表最多画多少分片
constexpr int kMaxTimelineRows = 200; // 时间轴对齐表最多画多少行
constexpr int kMaxTimelineCols = 8;   // 时间轴对齐表最多画多少条码率层
constexpr double kAlignTolerance = 0.05;

QString Seconds(double seconds) {
    return QString::number(seconds, 'f', 3);
}

QString Kbps(int64_t bps) {
    return QString::number(bps / 1000);
}

} // namespace

StreamingPanel::StreamingPanel(QWidget* parent) : QWidget(parent) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(8, 8, 8, 8);
    root_layout->setSpacing(6);

    summary_label_ = new QLabel(tr("尚未分析流媒体包"), this);
    summary_label_->setWordWrap(true);
    summary_label_->setStyleSheet("QLabel { font-weight: bold; }");
    root_layout->addWidget(summary_label_);

    // ---- 工具栏 ----
    auto* toolbar = new QHBoxLayout();
    refresh_button_ = new QPushButton(tr("重新扫描"), this);
    copy_json_button_ = new QPushButton(tr("复制 JSON"), this);
    export_json_button_ = new QPushButton(tr("导出 JSON"), this);
    toolbar->addWidget(refresh_button_);
    toolbar->addWidget(copy_json_button_);
    toolbar->addWidget(export_json_button_);
    toolbar->addStretch();
    root_layout->addLayout(toolbar);

    connect(refresh_button_, &QPushButton::clicked, this, &StreamingPanel::OnRefreshClicked);
    connect(copy_json_button_, &QPushButton::clicked, this, &StreamingPanel::OnCopyJsonClicked);
    connect(export_json_button_, &QPushButton::clicked, this, &StreamingPanel::OnExportJsonClicked);

    // ---- 主体：左结构树 / 右上 ladder / 右下时间轴+问题 ----
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    structure_tree_ = new QTreeWidget(splitter);
    structure_tree_->setHeaderLabels(QStringList() << tr("结构") << tr("值"));
    structure_tree_->setAlternatingRowColors(true);
    structure_tree_->setRootIsDecorated(true);

    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);

    ladder_table_ = new QTableWidget(right);
    ladder_table_->setAlternatingRowColors(true);
    ladder_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ladder_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ladder_table_->horizontalHeader()->setStretchLastSection(true);

    auto* lower_tabs = new QTabWidget(right);
    timeline_table_ = new QTableWidget(lower_tabs);
    timeline_table_->setAlternatingRowColors(false);
    timeline_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    timeline_table_->horizontalHeader()->setStretchLastSection(true);

    issue_table_ = new QTableWidget(lower_tabs);
    issue_table_->setAlternatingRowColors(true);
    issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    issue_table_->horizontalHeader()->setStretchLastSection(true);

    lower_tabs->addTab(timeline_table_, tr("分片时间轴对齐"));
    lower_tabs->addTab(issue_table_, tr("问题"));

    right_layout->addWidget(ladder_table_, 2);
    right_layout->addWidget(lower_tabs, 3);

    splitter->addWidget(structure_tree_);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    root_layout->addWidget(splitter, 1);
}

StreamingPanel::~StreamingPanel() = default;

void StreamingPanel::SetResult(const model::StreamingPackageResult& result) {
    current_result_ = result;
    has_result_ = result.valid;

    UpdateSummary(result);
    BuildStructureTree(result);
    BuildLadderTable(result);
    BuildTimelineTable(result);
    BuildIssueTable(result);
}

void StreamingPanel::Clear() {
    current_result_ = model::StreamingPackageResult{};
    has_result_ = false;
    summary_label_->setText(tr("尚未分析流媒体包"));
    structure_tree_->clear();
    ladder_table_->setRowCount(0);
    ladder_table_->setColumnCount(0);
    timeline_table_->setRowCount(0);
    timeline_table_->setColumnCount(0);
    issue_table_->setRowCount(0);
    issue_table_->setColumnCount(0);
}

QString StreamingPanel::SeverityText(model::IssueSeverity severity) {
    return QString::fromUtf8(model::ToString(severity));
}

QColor StreamingPanel::SeverityColor(model::IssueSeverity severity) {
    switch (severity) {
    case model::IssueSeverity::Critical:
        return QColor("#F85149");
    case model::IssueSeverity::Error:
        return QColor("#F85149");
    case model::IssueSeverity::Warning:
        return QColor("#D29922");
    default:
        return QColor("#8B949E");
    }
}

QString StreamingPanel::DescribeResolution(int width, int height) {
    if (width > 0 && height > 0)
        return QString("%1x%2").arg(width).arg(height);
    return QStringLiteral("-");
}

void StreamingPanel::UpdateSummary(const model::StreamingPackageResult& result) {
    if (!result.valid) {
        summary_label_->setText(tr("流媒体包解析失败：%1").arg(QString::fromStdString(result.error_message)));
        return;
    }
    const QString kind = QString::fromUtf8(model::ToString(result.kind));
    const QString live = result.IsLive() ? tr("直播") : tr("点播");
    const int errors =
        result.CountIssues(model::IssueSeverity::Error) + result.CountIssues(model::IssueSeverity::Critical);
    const int warnings = result.CountIssues(model::IssueSeverity::Warning);
    const int infos = result.CountIssues(model::IssueSeverity::Info);

    QString text = QString("%1 | %2 | %3 个码率层 | %4 个分片 | 问题: %5 错误 / %6 警告 / %7 提示")
                       .arg(kind)
                       .arg(live)
                       .arg(result.ladder.size())
                       .arg(static_cast<qulonglong>(result.TotalSegments()))
                       .arg(errors)
                       .arg(warnings)
                       .arg(infos);
    if (result.remote) {
        text += tr(" | 含远端 URI（当前版本不下载）");
    }
    if (result.truncated) {
        text += tr(" | 输出已按上限截断");
    }
    summary_label_->setText(text);
}

void StreamingPanel::BuildStructureTree(const model::StreamingPackageResult& result) {
    structure_tree_->clear();
    if (!result.valid)
        return;

    auto add = [](QTreeWidgetItem* parent, const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem(parent);
        item->setText(0, name);
        item->setText(1, value);
        return item;
    };
    auto add_top = [&](const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem(structure_tree_);
        item->setText(0, name);
        item->setText(1, value);
        return item;
    };

    auto* root = add_top(QString::fromUtf8(model::ToString(result.kind)), QString::fromStdString(result.manifest_path));

    if (result.IsDash()) {
        add(root, tr("MPD 类型"), QString::fromStdString(result.mpd_type));
        if (result.media_presentation_duration_s > 0.0) {
            add(root, tr("总时长"), Seconds(result.media_presentation_duration_s) + " s");
        }
        for (const model::DashPeriodInfo& period : result.periods) {
            auto* p = add(root, QString("Period %1").arg(period.index),
                          QString("duration=%1 s").arg(Seconds(period.duration_seconds)));
            for (const model::DashAdaptationSetInfo& as : period.adaptation_sets) {
                auto* a = add(p, QString("AdaptationSet %1").arg(as.index),
                              QString::fromStdString(as.content_type + " " + as.mime_type));
                for (int ri : as.representation_indices) {
                    if (ri < 0 || static_cast<size_t>(ri) >= result.representations.size())
                        continue;
                    const model::DashRepresentationInfo& rep = result.representations[ri];
                    auto* r = add(a, QString("Representation %1").arg(QString::fromStdString(rep.id)),
                                  QString("%1 %2 kbps")
                                      .arg(DescribeResolution(rep.width, rep.height))
                                      .arg(Kbps(rep.bandwidth_bps)));
                    for (int i = 0; i < static_cast<int>(rep.segments.size()) && i < kMaxSegmentNodes; ++i) {
                        const model::SegmentInfo& seg = rep.segments[i];
                        add(r, QString("Segment %1").arg(seg.sequence),
                            QString("t=%1 d=%2 %3")
                                .arg(Seconds(seg.start_seconds))
                                .arg(Seconds(seg.duration_seconds))
                                .arg(seg.exists ? tr("ok") : tr("缺失")));
                    }
                    if (static_cast<int>(rep.segments.size()) > kMaxSegmentNodes) {
                        add(r,
                            tr("... 其余 %1 个分片省略").arg(static_cast<int>(rep.segments.size()) - kMaxSegmentNodes),
                            QString());
                    }
                }
            }
        }
    } else {
        for (const model::HlsVariantInfo& v : result.variants) {
            add(root, QString("variant #%1").arg(v.index),
                QString("%1 %2 kbps").arg(DescribeResolution(v.width, v.height)).arg(Kbps(v.bandwidth_bps)));
        }
        for (const model::HlsRenditionInfo& r : result.renditions) {
            add(root, QString::fromStdString(r.type + " " + r.name), QString::fromStdString("group=" + r.group_id));
        }
        for (const model::MediaPlaylistInfo& pl : result.playlists) {
            auto* p = add(root, QString("%1 #%2").arg(QString::fromStdString(pl.role)).arg(pl.index),
                          QString("%1 分片 target=%2 s").arg(pl.SegmentCount()).arg(pl.target_duration_s));
            if (pl.encrypted)
                add(p, tr("加密"), QString::fromStdString(pl.key_method));
            if (pl.has_init_section)
                add(p, tr("初始化段"), QString::fromStdString(pl.init_uri));
            if (pl.low_latency) {
                add(p, tr("LL-HLS"), tr("%1 个部分分片").arg(pl.partial_count));
            }
            for (int i = 0; i < static_cast<int>(pl.segments.size()) && i < kMaxSegmentNodes; ++i) {
                const model::SegmentInfo& seg = pl.segments[i];
                add(p, seg.partial ? tr("Part") : QString("Segment %1").arg(seg.sequence),
                    QString("t=%1 d=%2 %3%4")
                        .arg(Seconds(seg.start_seconds))
                        .arg(Seconds(seg.duration_seconds))
                        .arg(seg.exists ? tr("ok") : tr("缺失"))
                        .arg(seg.discontinuity_before ? tr(" [discontinuity]") : QString()));
            }
            if (static_cast<int>(pl.segments.size()) > kMaxSegmentNodes) {
                add(p, tr("... 其余 %1 个分片省略").arg(static_cast<int>(pl.segments.size()) - kMaxSegmentNodes),
                    QString());
            }
        }
    }

    structure_tree_->expandToDepth(2);
    structure_tree_->resizeColumnToContents(0);
}

void StreamingPanel::BuildLadderTable(const model::StreamingPackageResult& result) {
    ladder_table_->setRowCount(0);
    if (!result.valid)
        return;

    const QStringList headers = QStringList() << tr("码率层") << tr("分辨率") << tr("码率(kbps)") << tr("编码")
                                              << tr("容器") << tr("分片数") << tr("平均时长(s)") << tr("最长时长(s)")
                                              << tr("初始化段") << tr("关键帧数");
    ladder_table_->setColumnCount(headers.size());
    ladder_table_->setHorizontalHeaderLabels(headers);
    ladder_table_->setRowCount(static_cast<int>(result.ladder.size()));

    for (int row = 0; row < static_cast<int>(result.ladder.size()); ++row) {
        const model::StreamingLadderEntry& e = result.ladder[row];
        int col = 0;
        auto set = [&](const QString& text) { ladder_table_->setItem(row, col++, new QTableWidgetItem(text)); };
        set(QString::fromStdString(e.label));
        set(DescribeResolution(e.width, e.height));
        set(Kbps(e.bandwidth_bps));
        set(QString::fromStdString(e.video_codec.empty() ? e.audio_codec : e.video_codec));
        set(QString::fromStdString(e.container_hint));
        set(QString::number(e.segment_count));
        set(Seconds(e.avg_segment_duration_s));
        set(Seconds(e.max_segment_duration_s));
        set(e.has_init_section ? tr("有") : tr("无"));
        set(QString::number(e.keyframe_times.size()));
    }
    ladder_table_->resizeColumnsToContents();
}

void StreamingPanel::BuildTimelineTable(const model::StreamingPackageResult& result) {
    timeline_table_->setRowCount(0);
    if (!result.valid || result.ladder.empty())
        return;

    // 参考轨：关键帧时间点最多的那条（ABR 通常以它为对齐基准）
    int reference = -1;
    for (int i = 0; i < static_cast<int>(result.ladder.size()); ++i) {
        if (result.ladder[i].keyframe_times.empty())
            continue;
        if (reference < 0 || result.ladder[i].keyframe_times.size() > result.ladder[reference].keyframe_times.size()) {
            reference = i;
        }
    }

    const int cols = std::min<int>(static_cast<int>(result.ladder.size()), kMaxTimelineCols);
    QStringList headers;
    headers << tr("分片 #");
    for (int c = 0; c < cols; ++c) {
        headers << QString::fromStdString(result.ladder[c].label);
    }
    timeline_table_->setColumnCount(headers.size());
    timeline_table_->setHorizontalHeaderLabels(headers);

    int rows = 0;
    for (int c = 0; c < cols; ++c) {
        rows = std::max(rows, static_cast<int>(result.ladder[c].keyframe_times.size()));
    }
    rows = std::min(rows, kMaxTimelineRows);
    timeline_table_->setRowCount(rows);

    const QColor kAlignBg("#5A2B2B");
    const QColor kAlignFg("#F85149");
    const QColor kMissingFg("#6E7681");

    for (int r = 0; r < rows; ++r) {
        auto* index_item = new QTableWidgetItem(QString::number(r));
        timeline_table_->setItem(r, 0, index_item);

        double ref_time = 0.0;
        bool has_ref = false;
        if (reference >= 0 && reference < cols &&
            r < static_cast<int>(result.ladder[reference].keyframe_times.size())) {
            ref_time = result.ladder[reference].keyframe_times[r];
            has_ref = true;
        }

        for (int c = 0; c < cols; ++c) {
            const model::StreamingLadderEntry& e = result.ladder[c];
            auto* item = new QTableWidgetItem();
            if (r < static_cast<int>(e.keyframe_times.size())) {
                const double t = e.keyframe_times[r];
                item->setText(Seconds(t));
                if (has_ref && std::fabs(t - ref_time) > kAlignTolerance) {
                    item->setBackground(kAlignBg);
                    item->setForeground(kAlignFg);
                    item->setToolTip(tr("与参考层 %1 的起点偏差 %2 秒")
                                         .arg(QString::fromStdString(result.ladder[reference].label))
                                         .arg(Seconds(t - ref_time)));
                }
                if (c == reference) {
                    QFont font = item->font();
                    font.setBold(true);
                    item->setFont(font);
                }
            } else {
                item->setText(QStringLiteral("-"));
                item->setForeground(kMissingFg);
            }
            timeline_table_->setItem(r, c + 1, item);
        }
    }
    timeline_table_->resizeColumnsToContents();
}

void StreamingPanel::BuildIssueTable(const model::StreamingPackageResult& result) {
    issue_table_->setRowCount(0);
    if (!result.valid)
        return;

    const QStringList headers = QStringList() << tr("级别") << tr("问题") << tr("说明") << tr("建议") << tr("次数");
    issue_table_->setColumnCount(headers.size());
    issue_table_->setHorizontalHeaderLabels(headers);
    issue_table_->setRowCount(static_cast<int>(result.issues.size()));

    // 严重度高的排前面
    std::vector<const model::StreamingIssue*> ordered;
    for (const model::StreamingIssue& issue : result.issues)
        ordered.push_back(&issue);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const model::StreamingIssue* a, const model::StreamingIssue* b) {
                         return static_cast<int>(a->severity) > static_cast<int>(b->severity);
                     });

    for (int row = 0; row < static_cast<int>(ordered.size()); ++row) {
        const model::StreamingIssue& issue = *ordered[row];
        const QColor color = SeverityColor(issue.severity);
        int col = 0;
        auto set = [&](const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setForeground(color);
            issue_table_->setItem(row, col++, item);
        };
        set(SeverityText(issue.severity));
        set(QString::fromStdString(issue.title));
        set(QString::fromStdString(issue.detail));
        set(QString::fromStdString(issue.suggestion));
        set(QString::number(issue.occurrence_count));
    }
    issue_table_->resizeColumnsToContents();
}

QString StreamingPanel::ExportToJson() const {
    QJsonObject root;
    root["manifest"] = QString::fromStdString(current_result_.manifest_path);
    root["kind"] = QString::fromUtf8(model::ToString(current_result_.kind));
    root["valid"] = current_result_.valid;
    root["live"] = current_result_.IsLive();

    QJsonArray ladder;
    for (const model::StreamingLadderEntry& e : current_result_.ladder) {
        QJsonObject o;
        o["label"] = QString::fromStdString(e.label);
        o["bandwidth_bps"] = static_cast<qint64>(e.bandwidth_bps);
        o["width"] = e.width;
        o["height"] = e.height;
        o["codecs"] = QString::fromStdString(e.codecs);
        o["container"] = QString::fromStdString(e.container_hint);
        o["segment_count"] = e.segment_count;
        o["avg_segment_duration"] = e.avg_segment_duration_s;
        o["max_segment_duration"] = e.max_segment_duration_s;
        o["has_init_section"] = e.has_init_section;
        QJsonArray keyframes;
        for (double t : e.keyframe_times)
            keyframes.append(t);
        o["keyframe_times"] = keyframes;
        ladder.append(o);
    }
    root["ladder"] = ladder;

    QJsonArray issues;
    for (const model::StreamingIssue& issue : current_result_.issues) {
        QJsonObject o;
        o["code"] = QString::fromStdString(issue.code);
        o["severity"] = SeverityText(issue.severity);
        o["title"] = QString::fromStdString(issue.title);
        o["detail"] = QString::fromStdString(issue.detail);
        o["suggestion"] = QString::fromStdString(issue.suggestion);
        o["metric"] = issue.metric_value;
        o["threshold"] = issue.threshold;
        o["occurrence"] = issue.occurrence_count;
        issues.append(o);
    }
    root["issues"] = issues;

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

void StreamingPanel::CopyJsonToClipboard() const {
    QApplication::clipboard()->setText(ExportToJson());
}

void StreamingPanel::OnRefreshClicked() {
    emit RefreshRequested();
}

void StreamingPanel::OnCopyJsonClicked() {
    CopyJsonToClipboard();
}

void StreamingPanel::OnExportJsonClicked() {
    const QString path =
        QFileDialog::getSaveFileName(this, tr("导出流媒体包 JSON"), QStringLiteral("streaming_package.json"),
                                     tr("JSON 文件 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    file.write(ExportToJson().toUtf8());
    file.close();
}

} // namespace ui
} // namespace videoeye
