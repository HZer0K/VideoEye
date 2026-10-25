#include "ui/streaming_panel/StreamingPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include "ui/streaming_panel/StreamingFormat.h"
#include "ui/streaming_panel/StreamingIssueView.h"
#include "ui/streaming_panel/StreamingLadderView.h"
#include "ui/streaming_panel/StreamingPackageJson.h"
#include "ui/streaming_panel/StreamingStructureView.h"
#include "ui/streaming_panel/StreamingTimelineView.h"

namespace videoeye {
namespace ui {

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
    refresh_button_ = new QPushButton(tr("开始分析"), this);
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

    structure_view_ = new StreamingStructureView(splitter);

    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);

    ladder_view_ = new StreamingLadderView(right);

    auto* lower_tabs = new QTabWidget(right);
    timeline_view_ = new StreamingTimelineView(lower_tabs);
    issue_view_ = new StreamingIssueView(lower_tabs);

    lower_tabs->addTab(timeline_view_, tr("分片时间轴对齐"));
    lower_tabs->addTab(issue_view_, tr("问题"));

    right_layout->addWidget(ladder_view_, 2);
    right_layout->addWidget(lower_tabs, 3);

    splitter->addWidget(structure_view_);
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
    structure_view_->SetResult(result);
    ladder_view_->SetResult(result);
    timeline_view_->SetResult(result);
    issue_view_->SetResult(result);
}

void StreamingPanel::Clear() {
    current_result_ = model::StreamingPackageResult{};
    has_result_ = false;
    summary_label_->setText(tr("尚未分析流媒体包"));
    structure_view_->Clear();
    ladder_view_->Clear();
    timeline_view_->Clear();
    issue_view_->Clear();
}

int StreamingPanel::IssueCount() const {
    return issue_view_ ? issue_view_->IssueCount() : 0;
}

void StreamingPanel::SetScanActive(bool active) {
    scan_active_ = active;
    // 扫描中共用同一份清单解析，重复点「开始分析」只会被诊断页拦下；直接禁用按钮更清楚。
    if (refresh_button_)
        refresh_button_->setEnabled(!active);
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
        text += tr(" | 含远端 URI（远端清单/分片不下载）");
    }
    // 探测覆盖度：清单里声称 N 个分片，这一轮真探过的只有 M 个（探测有上限）。
    // 不写出来的话，"N 个分片"会被读成"N 个都验过"，抽样结果就变成了假的全量结论。
    const auto probed = static_cast<qulonglong>(result.ProbedSegments());
    const auto total = static_cast<qulonglong>(result.TotalSegments());
    if (total > 0 && probed < total) {
        text += tr(" | 分片抽查 %1/%2，其余未探测").arg(probed).arg(total);
    }
    if (result.truncated) {
        text += tr(" | 输出已按上限截断");
    }
    // 加密 / 广告标记 / 快进档：这三样决定了"这份清单能做哪些业务"，
    // 摘要里不写的话，用户只能从结构树里一条条点开看。
    if (result.IsEncrypted()) {
        text += tr(" | 已加密（分片载荷不可读，码流类结论仅供参考）");
    }
    const auto dr_count = static_cast<qulonglong>(result.DateRangeCount());
    if (dr_count > 0) {
        text += tr(" | %1 个广告/章节标记").arg(dr_count);
    }
    const auto iframe_count = static_cast<qulonglong>(result.IframeVariantCount());
    if (iframe_count > 0) {
        text += tr(" | %1 档快进(I-FRAME)").arg(iframe_count);
    }
    const auto event_count = static_cast<qulonglong>(result.EventCount());
    if (event_count > 0) {
        text += tr(" | %1 个事件").arg(event_count);
    }
    if (result.IsLive()) {
        // 直播包只是一次快照：清单会随服务端滚动而变化，我们不重拉、不轮询。
        text += tr(" | 单次快照（不持续轮询）");
        if (result.IsDash()) {
            if (result.has_minimum_update_period) {
                text += tr(" | minimumUpdatePeriod=%1 s").arg(StreamingSeconds(result.minimum_update_period_s));
            }
            if (result.has_time_shift_buffer_depth) {
                text += tr(" | 时移窗口=%1 s").arg(StreamingSeconds(result.time_shift_buffer_depth_s));
            }
            if (!result.has_availability_start_time) {
                text += tr(" | 缺 availabilityStartTime");
            }
        } else {
            int live_playlists = 0;
            double window = 0.0;
            for (const model::MediaPlaylistInfo& pl : result.playlists) {
                if (!pl.IsLive())
                    continue;
                ++live_playlists;
                if (pl.WindowSeconds() > window)
                    window = pl.WindowSeconds();
            }
            text += tr(" | %1 条直播清单，窗口=%2 s").arg(live_playlists).arg(StreamingSeconds(window));
        }
    }
    summary_label_->setText(text);
}

QString StreamingPanel::ExportToJson() const {
    return BuildStreamingPackageJson(current_result_);
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
