#include "ui/main_window/OverviewPage.h"

#include <QBrush>
#include <QColor>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {

namespace {

// 分析页快捷跳转：pageId 与 AddPageWithScroll 登记的一致（MainWindow 据此切 stack）。
struct JumpTarget {
    const char* page_id;
    const char* title;
};

const JumpTarget kJumpTargets[] = {
    {"quality_diagnostics", "质量诊断"},
    {"file_structure", "文件结构"},
    {"playback_stats_packets", "播放统计与帧包"},
    {"motion_vectors", "运动矢量与块分析"},
    {"scene_change", "场景切换"},
    {"visual_quality", "画面质量"},
    {"bitrate_gop", "码率与 GOP"},
    {"audio_qc", "音频 QC"},
    {"color_hdr", "色彩与 HDR"},
    {"event_timeline", "事件与时间轴"},
    {"streaming_packets", "流媒体包"},
    {"codec_parameters", "编码参数集"},
    {"subtitle_aux", "字幕与辅助数据"},
    {"report_batch_qc", "报告与批量 QC"},
};

// 状态徽章的配色（与 QSS 里的 StatusBadge 同风格，按状态换色）
struct BadgeStyle {
    const char* fg;
    const char* bg;
    const char* border;
};

BadgeStyle BadgeStyleFor(const OverviewPage::AnalysisSummary& s) {
    if (s.scan_running)
        return {"#58A6FF", "rgba(88, 166, 255, 0.12)", "rgba(88, 166, 255, 0.3)"};
    if (s.failed)
        return {"#F85149", "rgba(248, 81, 73, 0.12)", "rgba(248, 81, 73, 0.3)"};
    if (s.has_result && s.partial)
        return {"#D29922", "rgba(210, 153, 34, 0.12)", "rgba(210, 153, 34, 0.3)"};
    if (s.has_result)
        return {"#3FB950", "rgba(63, 185, 80, 0.12)", "rgba(63, 185, 80, 0.3)"};
    return {"#8B949E", "rgba(139, 148, 158, 0.12)", "rgba(139, 148, 158, 0.3)"};
}

QString NonEmptyOr(const std::string& value, const QString& fallback) {
    return value.empty() ? fallback : QString::fromStdString(value);
}

} // namespace

OverviewPage::OverviewPage(QWidget* parent) : QWidget(parent) {
    BuildUi();
    RefreshUi();
}

// ---------------------------------------------------------------------------
// 构建
// ---------------------------------------------------------------------------

void OverviewPage::BuildUi() {
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // ---- 文件头：文件名 + 状态徽章 ----
    QHBoxLayout* header_row = new QHBoxLayout();
    file_name_label_ = new QLabel(tr("未打开文件"), this);
    QFont name_font = file_name_label_->font();
    name_font.setPointSize(name_font.pointSize() + 3);
    name_font.setBold(true);
    file_name_label_->setFont(name_font);
    file_name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header_row->addWidget(file_name_label_, 1);

    state_badge_ = new QLabel(this);
    state_badge_->setObjectName("StatusBadge");
    header_row->addWidget(state_badge_);
    root->addLayout(header_row);

    file_path_label_ = new QLabel(this);
    file_path_label_->setObjectName("MediaPath");
    file_path_label_->setWordWrap(false);
    file_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(file_path_label_);

    // ---- 文件摘要 ----
    summary_group_ = new QGroupBox(tr("文件摘要"), this);
    QGridLayout* summary_grid = new QGridLayout(summary_group_);
    summary_grid->setHorizontalSpacing(16);
    summary_grid->setVerticalSpacing(6);
    auto add_pair = [this, &summary_grid](int row, int col, const QString& label) {
        QLabel* key = new QLabel(label, summary_group_);
        key->setObjectName("HintLabel");
        summary_grid->addWidget(key, row, col * 2);
        QLabel* value = new QLabel(QStringLiteral("—"), summary_group_);
        summary_grid->addWidget(value, row, col * 2 + 1);
        return value;
    };
    summary_container_ = add_pair(0, 0, tr("容器格式"));
    summary_duration_ = add_pair(0, 1, tr("时长"));
    summary_size_ = add_pair(1, 0, tr("文件大小"));
    summary_bitrate_ = add_pair(1, 1, tr("总码率"));
    summary_video_ = add_pair(2, 0, tr("视频"));
    summary_audio_ = add_pair(2, 1, tr("音频"));
    summary_grid->setColumnStretch(1, 1);
    summary_grid->setColumnStretch(3, 1);
    root->addWidget(summary_group_);

    // ---- 分析状态 ----
    analysis_group_ = new QGroupBox(tr("分析状态"), this);
    QVBoxLayout* analysis_layout = new QVBoxLayout(analysis_group_);

    QHBoxLayout* verdict_row = new QHBoxLayout();
    verdict_label_ = new QLabel(analysis_group_);
    verdict_label_->setWordWrap(true);
    verdict_label_->setTextFormat(Qt::RichText);
    verdict_row->addWidget(verdict_label_, 1);
    rescan_button_ = new QPushButton(tr("重新分析"), analysis_group_);
    rescan_button_->setToolTip(tr("用当前诊断选项对当前文件再跑一次全文件扫描"));
    connect(rescan_button_, &QPushButton::clicked, this, &OverviewPage::RescanRequested);
    verdict_row->addWidget(rescan_button_);
    analysis_layout->addLayout(verdict_row);

    error_label_ = new QLabel(analysis_group_);
    error_label_->setObjectName("HintLabel");
    error_label_->setWordWrap(true);
    error_label_->setStyleSheet(QStringLiteral("color: #F85149;"));
    analysis_layout->addWidget(error_label_);

    issues_table_ = new QTableWidget(0, 3, analysis_group_);
    issues_table_->setHorizontalHeaderLabels({tr("严重度"), tr("位置"), tr("问题")});
    issues_table_->verticalHeader()->setVisible(false);
    issues_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    issues_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    issues_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issues_table_->setAlternatingRowColors(true);
    issues_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    issues_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    issues_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    issues_table_->setMinimumHeight(150);
    issues_table_->setToolTip(tr("点击条目跳到「质量诊断」页查看完整问题清单"));
    connect(issues_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        // 占位行（分析进行中 / 无结果 / 未发现异常）被 setSpan 铺满三列，
        // 点它仍会触发 cellClicked，但它们不带跳转数据 —— 这里按"无数据就不跳"处理。
        QTableWidgetItem* first = issues_table_->item(row, 0);
        if (!first)
            return;
        const QVariant target = first->data(Qt::UserRole);
        if (target.isValid()) {
            emit NavigateToPage(target.toString());
        }
    });
    analysis_layout->addWidget(issues_table_, 1);

    // ---- 各分析页快捷跳转 ----
    jump_group_ = new QGroupBox(tr("跳转到分析页"), this);
    QGridLayout* jump_grid = new QGridLayout(jump_group_);
    jump_grid->setSpacing(6);
    const int kColumns = 5;
    for (int i = 0; i < static_cast<int>(sizeof(kJumpTargets) / sizeof(kJumpTargets[0])); ++i) {
        QPushButton* button = new QPushButton(tr(kJumpTargets[i].title), jump_group_);
        button->setProperty("pageId", QString::fromLatin1(kJumpTargets[i].page_id));
        button->setToolTip(tr(kJumpTargets[i].title));
        connect(button, &QPushButton::clicked, this,
                [this, button]() { emit NavigateToPage(button->property("pageId").toString()); });
        jump_grid->addWidget(button, i / kColumns, i % kColumns);
    }
    for (int c = 0; c < kColumns; ++c) {
        jump_grid->setColumnStretch(c, 1);
    }
    root->addWidget(jump_group_);

    // ---- 完整媒体信息（原媒体信息页的纯文本整段保留在下方）----
    mediainfo_group_ = new QGroupBox(tr("完整媒体信息"), this);
    QVBoxLayout* mediainfo_layout = new QVBoxLayout(mediainfo_group_);
    mediainfo_layout->setContentsMargins(8, 8, 8, 8);
    mediainfo_text_ = new QTextEdit(mediainfo_group_);
    mediainfo_text_->setReadOnly(true);
    mediainfo_text_->setFont(theme::font::monoFont(9));
    mediainfo_text_->setLineWrapMode(QTextEdit::NoWrap);
    mediainfo_text_->setPlainText(tr("请打开一个媒体文件以查看详细信息"));
    mediainfo_layout->addWidget(mediainfo_text_);
    root->addWidget(mediainfo_group_, 1);

    // ---- 空态提示（未打开文件时替代摘要/分析两区）----
    empty_hint_ = new QLabel(tr("打开一个媒体文件后，这里会显示文件摘要、分析状态与最重要的异常；"
                                "完整媒体信息保留在下方。"),
                             this);
    empty_hint_->setObjectName("HintLabel");
    empty_hint_->setWordWrap(true);
    root->addWidget(empty_hint_);
}

void OverviewPage::RefreshUi() {
    const bool has_file = summary_.has_file;
    file_name_label_->setVisible(has_file);
    file_path_label_->setVisible(has_file);
    summary_group_->setVisible(has_file);
    analysis_group_->setVisible(has_file);
    jump_group_->setVisible(has_file);
    mediainfo_group_->setVisible(has_file);
    empty_hint_->setVisible(!has_file);

    if (!has_file) {
        return;
    }

    // 状态徽章
    QString badge_text;
    if (summary_.scan_running) {
        badge_text = tr("分析中…");
    } else if (summary_.failed) {
        badge_text = tr("扫描失败");
    } else if (summary_.has_result && summary_.partial) {
        badge_text = tr("已取消（结果不完整）");
    } else if (summary_.has_result) {
        badge_text = tr("分析完成");
    } else {
        badge_text = tr("未分析");
    }
    const BadgeStyle style = BadgeStyleFor(summary_);
    state_badge_->setText(badge_text);
    state_badge_->setStyleSheet(
        QStringLiteral("color: %1; background-color: %2; border: 1px solid %3;"
                       "border-radius: 4px; padding: 2px 8px; font-size: 12px; font-weight: 600;")
            .arg(QString::fromLatin1(style.fg), QString::fromLatin1(style.bg), QString::fromLatin1(style.border)));

    // 结论行（富文本：评分 + 结论 + 分级计数）
    if (summary_.scan_running) {
        verdict_label_->setText(tr("正在分析当前文件…（由「质量诊断」页编排，可在那里取消）"));
    } else if (summary_.failed) {
        verdict_label_->setText(tr("最近一次全文件扫描失败，没有可用结果。"));
    } else if (summary_.has_result) {
        verdict_label_->setText(
            QStringLiteral("<b>%1 %2</b> · %3%4 · "
                           "<span style=\"color:#F85149\">%5 %6</span> · "
                           "<span style=\"color:#FF7B72\">%7 %8</span> · "
                           "<span style=\"color:#D29922\">%9 %10</span> · "
                           "<span style=\"color:#58A6FF\">%11 %12</span>")
                .arg(tr("评分"), QString::number(summary_.score, 'f', 1), summary_.verdict,
                     summary_.partial ? tr("（结果不完整）") : QString(), tr("致命"),
                     QString::number(summary_.critical), tr("错误"), QString::number(summary_.error_count), tr("警告"),
                     QString::number(summary_.warning), tr("提示"), QString::number(summary_.info)));
    } else {
        verdict_label_->setText(tr("尚未分析 —— 打开文件后会自动运行一次全文件扫描，也可点右侧「重新分析」。"));
    }

    // 失败原因
    error_label_->setVisible(summary_.failed && !summary_.error.isEmpty());
    error_label_->setText(summary_.failed ? summary_.error : QString());

    // 重新分析按钮
    rescan_button_->setEnabled(has_file && !summary_.scan_running);

    // 最重要的异常表
    issues_table_->setRowCount(0);
    if (summary_.scan_running) {
        const int row = issues_table_->rowCount();
        issues_table_->insertRow(row);
        auto* item = new QTableWidgetItem(tr("分析进行中，结束后这里显示最重要的异常…"));
        item->setFlags(Qt::ItemIsEnabled); // 占位行不可点、不带跳转数据
        issues_table_->setItem(row, 0, item);
        issues_table_->setSpan(row, 0, 1, 3);
    } else if (summary_.failed || !summary_.has_result) {
        const int row = issues_table_->rowCount();
        issues_table_->insertRow(row);
        auto* item =
            new QTableWidgetItem(summary_.failed ? tr("扫描失败，无异常数据（原因见上）") : tr("暂无分析结果"));
        item->setFlags(Qt::ItemIsEnabled);
        issues_table_->setItem(row, 0, item);
        issues_table_->setSpan(row, 0, 1, 3);
    } else if (summary_.top_issues.isEmpty()) {
        const int row = issues_table_->rowCount();
        issues_table_->insertRow(row);
        auto* item = new QTableWidgetItem(tr("未发现异常"));
        item->setFlags(Qt::ItemIsEnabled);
        issues_table_->setItem(row, 0, item);
        issues_table_->setSpan(row, 0, 1, 3);
    } else {
        for (const model::DiagnosticIssue& issue : summary_.top_issues) {
            const int row = issues_table_->rowCount();
            issues_table_->insertRow(row);
            auto* severity_item = new QTableWidgetItem(SeverityText(issue.severity));
            severity_item->setForeground(QBrush(QColor(SeverityColor(issue.severity))));
            severity_item->setData(Qt::UserRole, QStringLiteral("quality_diagnostics"));
            issues_table_->setItem(row, 0, severity_item);
            issues_table_->setItem(row, 1, new QTableWidgetItem(FormatPosition(issue)));
            auto* title_item = new QTableWidgetItem(QString::fromStdString(issue.title));
            title_item->setToolTip(QString::fromStdString(issue.detail));
            issues_table_->setItem(row, 2, title_item);
        }
    }
}

// ---------------------------------------------------------------------------
// 数据入口
// ---------------------------------------------------------------------------

void OverviewPage::SetMediaInfoText(const QString& text) {
    if (mediainfo_text_) {
        mediainfo_text_->setPlainText(text);
    }
}

void OverviewPage::SetFileSummary(const model::StreamInfo& info) {
    const QString file_name = QString::fromStdString(info.filename);
    const QFileInfo file_info(file_name);
    file_name_label_->setText(file_info.fileName().isEmpty() ? file_name : file_info.fileName());
    file_path_label_->setText(file_name);

    const std::string& format = info.extractor.format;
    const std::string& profile = info.extractor.format_profile;
    summary_container_->setText(
        NonEmptyOr(format, tr("—")) +
        (profile.empty() ? QString() : QStringLiteral(" (%1)").arg(QString::fromStdString(profile))));
    summary_duration_->setText(NonEmptyOr(info.extractor.duration, tr("—")));
    summary_size_->setText(NonEmptyOr(info.extractor.file_size, tr("—")));
    summary_bitrate_->setText(NonEmptyOr(info.extractor.overall_bit_rate, tr("—")));

    if (info.video.format.empty()) {
        summary_video_->setText(tr("无视频流"));
    } else {
        QString video_text = QString::fromStdString(info.video.format);
        if (!info.video.width.empty() && !info.video.height.empty()) {
            video_text += QStringLiteral(" %1x%2").arg(QString::fromStdString(info.video.width),
                                                       QString::fromStdString(info.video.height));
        }
        if (!info.video.frame_rate.empty()) {
            video_text += QStringLiteral(" @ %1 fps").arg(QString::fromStdString(info.video.frame_rate));
        }
        if (!info.video.bit_depth.empty()) {
            video_text += QStringLiteral(", %1 bit").arg(QString::fromStdString(info.video.bit_depth));
        }
        summary_video_->setText(video_text);
    }

    if (info.audio.format.empty()) {
        summary_audio_->setText(tr("无音频流"));
    } else {
        QString audio_text = QString::fromStdString(info.audio.format);
        if (!info.audio.channels.empty()) {
            audio_text += QStringLiteral(", %1 ch").arg(QString::fromStdString(info.audio.channels));
        }
        if (!info.audio.sampling_rate.empty()) {
            audio_text += QStringLiteral(", %1 Hz").arg(QString::fromStdString(info.audio.sampling_rate));
        }
        summary_audio_->setText(audio_text);
    }

    if (!summary_.has_file) {
        summary_.has_file = true;
        RefreshUi();
    }
}

void OverviewPage::ClearFileSummary() {
    // 分析状态也要一并清：上一次文件的评分/异常留在 summary_ 里的话，
    // 下次打开新文件、分析快照还没到的那一帧会先把旧结论显示出来。
    summary_ = AnalysisSummary{};
    file_name_label_->setText(tr("未打开文件"));
    file_path_label_->clear();
    summary_container_->setText(QStringLiteral("—"));
    summary_duration_->setText(QStringLiteral("—"));
    summary_size_->setText(QStringLiteral("—"));
    summary_bitrate_->setText(QStringLiteral("—"));
    summary_video_->setText(QStringLiteral("—"));
    summary_audio_->setText(QStringLiteral("—"));
    summary_.has_file = false;
    RefreshUi();
}

void OverviewPage::SetOpenedFileWithoutSummary(const QString& path) {
    const QFileInfo file_info(path);
    file_name_label_->setText(file_info.fileName().isEmpty() ? path : file_info.fileName());
    file_path_label_->setText(path);
    summary_container_->setText(QStringLiteral("—"));
    summary_duration_->setText(QStringLiteral("—"));
    summary_size_->setText(QStringLiteral("—"));
    summary_bitrate_->setText(QStringLiteral("—"));
    summary_video_->setText(QStringLiteral("—"));
    summary_audio_->setText(QStringLiteral("—"));
    if (!summary_.has_file) {
        summary_.has_file = true;
        RefreshUi();
    }
}

void OverviewPage::SetAnalysisSummary(const AnalysisSummary& summary) {
    summary_ = summary;
    RefreshUi();
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

QString OverviewPage::SeverityText(model::IssueSeverity severity) {
    switch (severity) {
    case model::IssueSeverity::Critical:
        return tr("致命");
    case model::IssueSeverity::Error:
        return tr("错误");
    case model::IssueSeverity::Warning:
        return tr("警告");
    case model::IssueSeverity::Info:
        return tr("提示");
    }
    return QString();
}

QString OverviewPage::SeverityColor(model::IssueSeverity severity) {
    switch (severity) {
    case model::IssueSeverity::Critical:
        return QStringLiteral("#F85149");
    case model::IssueSeverity::Error:
        return QStringLiteral("#FF7B72");
    case model::IssueSeverity::Warning:
        return QStringLiteral("#D29922");
    case model::IssueSeverity::Info:
        return QStringLiteral("#58A6FF");
    }
    return QStringLiteral("#8B949E");
}

QString OverviewPage::FormatPosition(const model::DiagnosticIssue& issue) {
    if (issue.IsGlobal()) {
        return tr("全局");
    }
    return QStringLiteral("%1 s").arg(QString::number(issue.range.start_seconds, 'f', 1));
}

} // namespace ui
} // namespace videoeye
