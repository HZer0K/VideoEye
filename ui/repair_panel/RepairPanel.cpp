#include "ui/repair_panel/RepairPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <sstream>
#include <utility>
#include <vector>

#include "core/qt/RepairController.h"
#include "core/reporting/RepairReportExporter.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {
namespace {

const char* SeverityText(model::IssueSeverity severity) {
    switch (severity) {
    case model::IssueSeverity::Critical:
        return "严重";
    case model::IssueSeverity::Error:
        return "错误";
    case model::IssueSeverity::Warning:
        return "警告";
    case model::IssueSeverity::Info:
        return "提示";
    }
    return "未知";
}

QString SeverityColor(model::IssueSeverity severity) {
    switch (severity) {
    case model::IssueSeverity::Critical:
    case model::IssueSeverity::Error:
        return QString::fromLatin1(theme::color::kDanger);
    case model::IssueSeverity::Warning:
        return QString::fromLatin1(theme::color::kWarning);
    default:
        return QString::fromLatin1(theme::color::kTextSecondary);
    }
}

QString StatusColor(model::RepairBusinessStatus status) {
    switch (status) {
    case model::RepairBusinessStatus::Repaired:
        return QString::fromLatin1(theme::color::kSuccess);
    case model::RepairBusinessStatus::PartiallyRecovered:
    case model::RepairBusinessStatus::NeedsReference:
        return QString::fromLatin1(theme::color::kWarning);
    case model::RepairBusinessStatus::Unsupported:
    case model::RepairBusinessStatus::NoRecoverableContent:
    case model::RepairBusinessStatus::Failed:
        return QString::fromLatin1(theme::color::kDanger);
    case model::RepairBusinessStatus::Canceled:
    case model::RepairBusinessStatus::TimedOut:
        return QString::fromLatin1(theme::color::kTextSecondary);
    }
    return QString::fromLatin1(theme::color::kTextSecondary);
}

QString LocationText(const model::DamageLocation& location) {
    std::ostringstream out;
    if (location.bytes.has_value()) {
        out << "bytes [" << location.bytes->begin << ", ";
        if (location.bytes->known_end)
            out << location.bytes->end;
        else
            out << "?";
        out << ")";
    }
    if (location.track_id.has_value())
        out << " track=" << *location.track_id;
    if (location.stream_index.has_value())
        out << " stream=" << *location.stream_index;
    if (location.sample_ordinal.has_value())
        out << " sample#" << *location.sample_ordinal;
    if (location.item_count.has_value())
        out << " ×" << *location.item_count;
    if (location.estimated_seconds.has_value())
        out << " ~" << QString::number(*location.estimated_seconds, 'f', 3).toStdString() << "s";
    if (location.precision != model::DamagePrecision::Exact)
        out << " [" << model::ToString(location.precision) << "]";
    return QString::fromStdString(out.str());
}

QLineEdit* MakeReadOnlyPathEdit(const QString& placeholder, QWidget* parent) {
    auto* edit = new QLineEdit(parent);
    edit->setPlaceholderText(placeholder);
    edit->setClearButtonEnabled(true);
    return edit;
}

} // namespace

RepairPanel::RepairPanel(QWidget* parent) : QWidget(parent), controller_(new qt::RepairController(this)) {
    BuildUi();
    connect(controller_, &qt::RepairController::ProgressReported, this, &RepairPanel::OnProgress);
    connect(controller_, &qt::RepairController::RepairFinished, this, &RepairPanel::OnFinished);

    // 报告写入不占 UI 线程：worker 只持有结果快照与路径，完成后由主线程轮询落地。
    report_timer_ = new QTimer(this);
    report_timer_->setInterval(50);
    connect(report_timer_, &QTimer::timeout, this, [this]() { PollReportWrite(); });
    SetRunning(false);
}

RepairPanel::~RepairPanel() {
    if (report_timer_)
        report_timer_->stop();
    // std::async 的 future 析构会等待任务结束；显式等一次，避免析构点隐式阻塞。
    if (report_future_.valid())
        report_future_.wait();
}

void RepairPanel::BuildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // ---------------- 配置 ----------------
    auto* config_group = new QGroupBox(QStringLiteral("修复配置"), this);
    auto* grid = new QGridLayout(config_group);
    grid->setContentsMargins(14, 18, 14, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);

    int row = 0;
    grid->addWidget(new QLabel(QStringLiteral("源文件"), config_group), row, 0);
    source_edit_ = MakeReadOnlyPathEdit(QStringLiteral("选择要修复的视频文件"), config_group);
    grid->addWidget(source_edit_, row, 1);
    auto* source_button = new QPushButton(QStringLiteral("浏览…"), config_group);
    grid->addWidget(source_button, row, 2);
    connect(source_button, &QPushButton::clicked, this, &RepairPanel::OnBrowseSource);

    ++row;
    grid->addWidget(new QLabel(QStringLiteral("参考样片"), config_group), row, 0);
    reference_edit_ = MakeReadOnlyPathEdit(
        QStringLiteral("可选：为缺少初始化段的 fMP4 分片提供同源初始化段或完整参考文件"), config_group);
    grid->addWidget(reference_edit_, row, 1);
    auto* reference_button = new QPushButton(QStringLiteral("浏览…"), config_group);
    grid->addWidget(reference_button, row, 2);
    connect(reference_button, &QPushButton::clicked, this, &RepairPanel::OnBrowseReference);

    ++row;
    grid->addWidget(new QLabel(QStringLiteral("产物目录"), config_group), row, 0);
    output_edit_ = MakeReadOnlyPathEdit(QStringLiteral("默认与源文件同目录"), config_group);
    grid->addWidget(output_edit_, row, 1);
    auto* output_button = new QPushButton(QStringLiteral("选择…"), config_group);
    grid->addWidget(output_button, row, 2);
    connect(output_button, &QPushButton::clicked, this, &RepairPanel::OnBrowseOutput);

    ++row;
    grid->addWidget(new QLabel(QStringLiteral("产物容器"), config_group), row, 0);
    container_combo_ = new QComboBox(config_group);
    container_combo_->addItem(QStringLiteral("自动（沿用源容器）"),
                              static_cast<int>(model::RepairOutputContainer::Auto));
    container_combo_->addItem(QStringLiteral("MP4"), static_cast<int>(model::RepairOutputContainer::Mp4));
    container_combo_->addItem(QStringLiteral("MKV"), static_cast<int>(model::RepairOutputContainer::Mkv));
    container_combo_->addItem(QStringLiteral("MPEG-TS"), static_cast<int>(model::RepairOutputContainer::Ts));
    container_combo_->setToolTip(QStringLiteral("产物封装格式。\n"
                                                "MPEG-TS 会把 H.264/HEVC 转换为 Annex B 码流，载荷字节与源不一致，"
                                                "报告会标注跳过载荷哈希比对。"));
    grid->addWidget(container_combo_, row, 1);

    ++row;
    grid->addWidget(new QLabel(QStringLiteral("修复模式"), config_group), row, 0);
    mode_combo_ = new QComboBox(config_group);
    mode_combo_->addItem(QStringLiteral("保码流（推荐）"), static_cast<int>(model::RepairMode::PreserveBitstream));
    mode_combo_->addItem(QStringLiteral("救内容（解码后重编码，画质有损）"),
                         static_cast<int>(model::RepairMode::RecoverContent));
    mode_combo_->setToolTip(QStringLiteral("保码流：不重编码，只重建索引与时间轴。\n"
                                           "救内容：解码可读视频帧后重新编码（S08），适合编码层已损坏、"
                                           "但仍有可解码帧的文件；产物有代际画质损失，时间轴会按推定帧率重写。"));
    grid->addWidget(mode_combo_, row, 1);

    auto* level_row = new QHBoxLayout();
    level_row->setSpacing(8);
    level_row->addWidget(new QLabel(QStringLiteral("验证等级"), config_group));
    level_combo_ = new QComboBox(config_group);
    level_combo_->addItem(QStringLiteral("V2 全量解码（推荐）"),
                          static_cast<int>(model::RepairVerificationLevel::V2_FullDecode));
    level_combo_->addItem(QStringLiteral("V1 抽帧验证"),
                          static_cast<int>(model::RepairVerificationLevel::V1_FrameSample));
    level_combo_->addItem(QStringLiteral("V0 仅容器可读"),
                          static_cast<int>(model::RepairVerificationLevel::V0_Container));
    level_row->addWidget(level_combo_, 1);
    grid->addLayout(level_row, row, 2);

    ++row;
    grid->addWidget(new QLabel(QStringLiteral("损坏区间处理"), config_group), row, 0);
    gap_combo_ = new QComboBox(config_group);
    gap_combo_->addItem(QStringLiteral("保留原时间间隔（默认）"),
                        static_cast<int>(model::RepairGapPolicy::PreserveOriginalTimeline));
    gap_combo_->addItem(QStringLiteral("删除损坏区间，连续播放"),
                        static_cast<int>(model::RepairGapPolicy::MergeContiguous));
    gap_combo_->setToolTip(QStringLiteral("跳段恢复得到多段内容时，产物时间轴如何处理。\n"
                                          "保留间隔：时间轴与源一致，中间存在空洞；\n"
                                          "连续播放：把后段平移到前段末尾，产物时长变短。"));
    grid->addWidget(gap_combo_, row, 1);

    ++row;
    grid->addWidget(new QLabel(QStringLiteral("恢复能力"), config_group), row, 0);
    auto* capability_row = new QHBoxLayout();
    capability_row->setSpacing(12);
    segment_check_ = new QCheckBox(QStringLiteral("跳段继续恢复"), config_group);
    segment_check_->setChecked(true);
    segment_check_->setToolTip(
        QStringLiteral("读到损坏区间后继续向后寻找可解码恢复点，抢救后半段内容；关闭则读到错误即停止。"));
    timeline_check_ = new QCheckBox(QStringLiteral("启用时间轴修复"), config_group);
    timeline_check_->setChecked(true);
    timeline_check_->setToolTip(
        QStringLiteral("对重复 / 回退的时间戳做显式单调重写；完全缺失的时间戳默认只统计、不伪造。"));
    fill_missing_check_ = new QCheckBox(QStringLiteral("合成缺失时间戳"), config_group);
    fill_missing_check_->setChecked(false);
    fill_missing_check_->setToolTip(
        QStringLiteral("PTS/DTS 同时缺失时，按同流上一帧 + 标称帧时长外推合成，才能写入产物。\n"
                       "合成值不是源侧真实时间，播放时长可能偏差；默认关闭。需同时启用时间轴修复。"));
    single_track_check_ = new QCheckBox(QStringLiteral("允许单轨抢救"), config_group);
    single_track_check_->setChecked(true);
    single_track_check_->setToolTip(
        QStringLiteral("仅当某条轨的全部样本都确认不可读时，把它从产物中剔除，保证其余轨道仍可播放；"
                       "被剔除轨道的内容不可找回。"));
    keep_partial_check_ = new QCheckBox(QStringLiteral("取消时保留已写前缀"), config_group);
    keep_partial_check_->setChecked(false);
    keep_partial_check_->setToolTip(QStringLiteral("取消或超出预算时，把已经写入的产物前缀另存为 *.partial.<ext>。\n"
                                                   "该前缀未经验证，不能当作修复完成；默认关闭。"));
    capability_row->addWidget(segment_check_);
    capability_row->addWidget(timeline_check_);
    capability_row->addWidget(fill_missing_check_);
    capability_row->addWidget(single_track_check_);
    capability_row->addWidget(keep_partial_check_);
    capability_row->addStretch(1);
    grid->addLayout(capability_row, row, 1, 1, 2);
    // 合成时间戳属于时间轴修复的扩展；未启用时间轴修复时该项无意义，直接禁用。
    connect(timeline_check_, &QCheckBox::toggled, this, [this](bool checked) {
        if (fill_missing_check_)
            fill_missing_check_->setEnabled(checked && start_button_ != nullptr && start_button_->isEnabled());
    });

    root->addWidget(config_group);

    // ---------------- 批量队列 ----------------
    // 队列非空时「开始修复」按队列串行跑；为空时退回单文件流程（源文件那一行）。
    auto* batch_group = new QGroupBox(QStringLiteral("批量队列"), this);
    auto* batch_layout = new QVBoxLayout(batch_group);
    batch_layout->setContentsMargins(14, 18, 14, 12);
    batch_layout->setSpacing(8);

    batch_table_ = new QTableWidget(0, 2, batch_group);
    QStringList batch_headers;
    batch_headers << QStringLiteral("源文件") << QStringLiteral("状态");
    batch_table_->setHorizontalHeaderLabels(batch_headers);
    batch_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    batch_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    batch_table_->setAlternatingRowColors(true);
    batch_table_->verticalHeader()->setVisible(false);
    batch_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    batch_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    batch_table_->setMinimumHeight(96);
    batch_table_->setToolTip(QStringLiteral("队列非空时按顺序串行修复；前一项的产物与报告写完才启动下一项。"));
    batch_layout->addWidget(batch_table_, 1);

    auto* batch_buttons = new QHBoxLayout();
    batch_buttons->setSpacing(8);
    batch_add_button_ = new QPushButton(QStringLiteral("添加文件…"), batch_group);
    batch_remove_button_ = new QPushButton(QStringLiteral("移除选中"), batch_group);
    batch_clear_button_ = new QPushButton(QStringLiteral("清空队列"), batch_group);
    for (auto* button : {batch_add_button_, batch_remove_button_, batch_clear_button_})
        button->setMinimumHeight(28);
    batch_buttons->addWidget(batch_add_button_);
    batch_buttons->addWidget(batch_remove_button_);
    batch_buttons->addWidget(batch_clear_button_);
    batch_buttons->addStretch(1);
    batch_layout->addLayout(batch_buttons);
    connect(batch_add_button_, &QPushButton::clicked, this, &RepairPanel::OnAddBatchFiles);
    connect(batch_remove_button_, &QPushButton::clicked, this, &RepairPanel::OnRemoveBatchSelection);
    connect(batch_clear_button_, &QPushButton::clicked, this, &RepairPanel::OnClearBatch);
    root->addWidget(batch_group);

    // ---------------- 操作 + 进度 ----------------
    auto* action_row = new QHBoxLayout();
    action_row->setSpacing(10);
    start_button_ = new QPushButton(QStringLiteral("开始修复"), this);
    start_button_->setDefault(true);
    cancel_button_ = new QPushButton(QStringLiteral("取消"), this);
    open_output_button_ = new QPushButton(QStringLiteral("打开产物"), this);
    open_report_button_ = new QPushButton(QStringLiteral("打开报告"), this);
    for (auto* button : {start_button_, cancel_button_, open_output_button_, open_report_button_})
        button->setMinimumHeight(30);
    action_row->addWidget(start_button_);
    action_row->addWidget(cancel_button_);
    action_row->addSpacing(8);
    action_row->addWidget(open_output_button_);
    action_row->addWidget(open_report_button_);
    action_row->addStretch(1);
    root->addLayout(action_row);
    connect(start_button_, &QPushButton::clicked, this, &RepairPanel::OnStart);
    connect(cancel_button_, &QPushButton::clicked, this, &RepairPanel::OnCancel);
    connect(open_output_button_, &QPushButton::clicked, this, &RepairPanel::OnOpenOutput);
    connect(open_report_button_, &QPushButton::clicked, this, &RepairPanel::OnOpenReport);

    auto* progress_row = new QHBoxLayout();
    progress_row->setSpacing(10);
    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(true);
    progress_row->addWidget(progress_bar_, 1);
    stage_label_ = new QLabel(QStringLiteral("待开始"), this);
    stage_label_->setMinimumWidth(260);
    stage_label_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kTextSecondary)));
    progress_row->addWidget(stage_label_);
    root->addLayout(progress_row);

    // ---------------- 结果 ----------------
    auto* result_group = new QGroupBox(QStringLiteral("修复结果"), this);
    auto* result_layout = new QVBoxLayout(result_group);
    result_layout->setContentsMargins(14, 18, 14, 12);
    result_layout->setSpacing(10);

    auto* status_row = new QHBoxLayout();
    status_row->setSpacing(10);
    status_badge_ = new QLabel(QStringLiteral("待开始"), result_group);
    QFont badge_font = status_badge_->font();
    badge_font.setPointSize(badge_font.pointSize() + 2);
    badge_font.setBold(true);
    status_badge_->setFont(badge_font);
    status_row->addWidget(status_badge_);
    summary_label_ =
        new QLabel(QStringLiteral("选择文件后开始修复；诊断、执行、验证都会在这里给出结论。"), result_group);
    summary_label_->setWordWrap(true);
    summary_label_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kTextSecondary)));
    status_row->addWidget(summary_label_, 1);
    result_layout->addLayout(status_row);

    auto* metrics = new QGridLayout();
    metrics->setHorizontalSpacing(18);
    metrics->setVerticalSpacing(6);
    const char* captions[6] = {"播放验证", "内容保留", "时间轴变化", "画质风险", "编码变化", "验证覆盖"};
    for (int i = 0; i < 6; ++i) {
        auto* caption = new QLabel(QString::fromUtf8(captions[i]), result_group);
        caption->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kTextMuted)));
        metric_labels_[i] = new QLabel(QStringLiteral("—"), result_group);
        metric_labels_[i]->setWordWrap(true);
        metric_labels_[i]->setStyleSheet(
            QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kTextPrimary)));
        metrics->addWidget(caption, (i / 2) * 2, i % 2);
        metrics->addWidget(metric_labels_[i], (i / 2) * 2 + 1, i % 2);
    }
    metrics->setColumnStretch(0, 1);
    metrics->setColumnStretch(1, 1);
    result_layout->addLayout(metrics);

    findings_table_ = new QTableWidget(0, 4, result_group);
    QStringList headers;
    headers << QStringLiteral("层级") << QStringLiteral("证据") << QStringLiteral("定位") << QStringLiteral("级别");
    findings_table_->setHorizontalHeaderLabels(headers);
    findings_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    findings_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    findings_table_->setAlternatingRowColors(true);
    findings_table_->verticalHeader()->setVisible(false);
    findings_table_->horizontalHeader()->setStretchLastSection(false);
    findings_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    findings_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    findings_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    findings_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    findings_table_->setColumnWidth(2, 230);
    findings_table_->setMinimumHeight(160);
    findings_table_->setWordWrap(true);
    result_layout->addWidget(findings_table_, 1);

    auto* report_row = new QHBoxLayout();
    report_label_ = new QLabel(QStringLiteral("未生成报告"), result_group);
    report_label_->setWordWrap(true);
    report_label_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kTextSecondary)));
    report_row->addWidget(report_label_, 1);
    result_layout->addLayout(report_row);

    root->addWidget(result_group, 1);
}

void RepairPanel::SetCurrentFile(const QString& path) {
    if (path.isEmpty() || controller_->IsRunning() || batch_running_)
        return;
    source_edit_->setText(path);
}

void RepairPanel::OnBrowseSource() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要修复的文件"), QFileInfo(source_edit_->text()).absolutePath(),
        QStringLiteral("媒体文件 (*.mp4 *.mov *.m4v *.m4a *.3gp *.mkv *.avi *.ts);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    source_edit_->setText(path);
    if (output_edit_->text().isEmpty())
        output_edit_->setPlaceholderText(QFileInfo(path).absolutePath());
}

void RepairPanel::OnBrowseReference() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择参考样片 / 初始化段"), QFileInfo(source_edit_->text()).absolutePath(),
        QStringLiteral("媒体文件 (*.mp4 *.mov *.m4v *.m4a *.m4s *.3gp);;所有文件 (*)"));
    if (!path.isEmpty())
        reference_edit_->setText(path);
}

void RepairPanel::OnBrowseOutput() {
    const QString start =
        output_edit_->text().isEmpty() ? QFileInfo(source_edit_->text()).absolutePath() : output_edit_->text();
    const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择产物目录"), start);
    if (!directory.isEmpty())
        output_edit_->setText(directory);
}

void RepairPanel::OnStart() {
    if (controller_->IsRunning() || batch_running_)
        return;

    // 队列非空时按队列串行执行；每项复用当前配置快照，只替换源文件。
    QStringList batch_sources;
    for (int row = 0; row < batch_table_->rowCount(); ++row) {
        auto* item = batch_table_->item(row, 0);
        if (item && !item->text().trimmed().isEmpty())
            batch_sources << item->text().trimmed();
    }
    if (!batch_sources.isEmpty()) {
        batch_requests_.clear();
        for (const QString& source : batch_sources) {
            batch_requests_.push_back(BuildRequest(source));
            MarkBatchRow(static_cast<int>(batch_requests_.size()) - 1, QStringLiteral("待执行"),
                         QString::fromLatin1(theme::color::kTextMuted));
        }
        batch_index_ = 0;
        batch_running_ = true;
        batch_canceled_ = false;
        last_write_report_ = true;
        ResetForNewRun();
        SetRunning(true);
        StartNextBatchItem();
        return;
    }

    const QString source = source_edit_->text().trimmed();
    if (source.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法开始"),
                             QStringLiteral("请先选择要修复的源文件，或在批量队列里添加文件。"));
        return;
    }
    const QFileInfo info(source);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(this, QStringLiteral("无法开始"),
                             QStringLiteral("源文件不存在或不是普通文件：\n%1").arg(source));
        return;
    }

    const model::RepairRequest request = BuildRequest(source);
    last_write_report_ = request.write_report;

    ResetForNewRun();
    const quint64 task_id = controller_->StartRepair(request);
    if (task_id == 0) {
        QMessageBox::warning(this, QStringLiteral("无法开始"),
                             QStringLiteral("已有修复任务在运行，或后台任务队列已满。"));
        return;
    }
    SetRunning(true);
}

model::RepairRequest RepairPanel::BuildRequest(const QString& source) const {
    model::RepairRequest request;
    request.source_path = source.toStdString();
    request.output_directory = output_edit_->text().trimmed().toStdString();
    request.reference_path = reference_edit_->text().trimmed().toStdString();
    request.mode = static_cast<model::RepairMode>(mode_combo_->currentData().toInt());
    request.output_container = static_cast<model::RepairOutputContainer>(container_combo_->currentData().toInt());
    request.verification_level = static_cast<model::RepairVerificationLevel>(level_combo_->currentData().toInt());
    request.gap_policy = static_cast<model::RepairGapPolicy>(gap_combo_->currentData().toInt());
    request.allow_segment_salvage = segment_check_->isChecked();
    request.repair_timeline = timeline_check_->isChecked();
    request.fill_missing_timestamps = request.repair_timeline && fill_missing_check_->isChecked();
    request.allow_single_track = single_track_check_->isChecked();
    request.keep_partial_on_cancel = keep_partial_check_->isChecked();
    request.write_report = true;
    return request;
}

void RepairPanel::StartNextBatchItem() {
    if (!batch_running_)
        return;
    while (batch_index_ < static_cast<int>(batch_requests_.size())) {
        if (batch_canceled_) {
            FinishBatch(batch_index_);
            return;
        }
        const int index = batch_index_;
        const QString source = QString::fromStdString(batch_requests_[index].source_path);
        const QFileInfo info(source);
        if (!info.exists() || !info.isFile()) {
            MarkBatchRow(index, QStringLiteral("跳过（文件不存在）"), QString::fromLatin1(theme::color::kTextMuted));
            ++batch_index_;
            continue;
        }

        last_write_report_ = batch_requests_[index].write_report;
        ResetForNewRun();
        progress_bar_->setValue(0);
        stage_label_->setText(
            QStringLiteral("批量 %1/%2 · %3").arg(index + 1).arg(batch_requests_.size()).arg(info.fileName()));
        MarkBatchRow(index, QStringLiteral("运行中…"), QString::fromLatin1(theme::color::kAccent));
        SetRunning(true);

        const quint64 task_id = controller_->StartRepair(batch_requests_[index]);
        if (task_id == 0) {
            MarkBatchRow(index, QStringLiteral("启动失败（任务槽忙）"), QString::fromLatin1(theme::color::kDanger));
            FinishBatch(index + 1);
            return;
        }
        return;
    }
    FinishBatch(batch_index_);
}

void RepairPanel::AdvanceBatch() {
    if (!batch_running_)
        return;
    ++batch_index_;
    StartNextBatchItem();
}

void RepairPanel::FinishBatch(int first_unstarted) {
    if (!batch_running_)
        return;
    const QString pending_text = batch_canceled_ ? QStringLiteral("已取消（未执行）") : QStringLiteral("未执行");
    for (int row = std::max(first_unstarted, 0); row < batch_table_->rowCount(); ++row)
        MarkBatchRow(row, pending_text, QString::fromLatin1(theme::color::kTextMuted));

    batch_running_ = false;
    batch_canceled_ = false;
    batch_index_ = -1;
    batch_requests_.clear();
    SetRunning(false);
    cancel_button_->setEnabled(false);
    stage_label_->setText(QStringLiteral("批量队列已结束"));
}

void RepairPanel::MarkBatchRow(int row, const QString& status, const QString& color) {
    if (!batch_table_ || row < 0 || row >= batch_table_->rowCount())
        return;
    auto* item = batch_table_->item(row, 1);
    if (!item) {
        item = new QTableWidgetItem();
        batch_table_->setItem(row, 1, item);
    }
    item->setText(status);
    item->setForeground(QColor(color));
}

void RepairPanel::OnAddBatchFiles() {
    if (controller_->IsRunning() || batch_running_)
        return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("添加到批量队列"), QFileInfo(source_edit_->text()).absolutePath(),
        QStringLiteral("媒体文件 (*.mp4 *.mov *.m4v *.m4a *.3gp *.mkv *.avi *.ts);;所有文件 (*)"));
    for (const QString& path : paths) {
        bool duplicate = false;
        for (int row = 0; row < batch_table_->rowCount(); ++row) {
            auto* existing = batch_table_->item(row, 0);
            if (existing && existing->text() == path) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        const int row = batch_table_->rowCount();
        batch_table_->insertRow(row);
        auto* source = new QTableWidgetItem(path);
        source->setToolTip(path);
        batch_table_->setItem(row, 0, source);
        MarkBatchRow(row, QStringLiteral("待执行"), QString::fromLatin1(theme::color::kTextMuted));
    }
}

void RepairPanel::OnRemoveBatchSelection() {
    if (controller_->IsRunning() || batch_running_)
        return;
    QSet<int> rows;
    for (auto* item : batch_table_->selectedItems())
        rows.insert(item->row());
    QList<int> sorted = rows.values();
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int row : sorted)
        batch_table_->removeRow(row);
}

void RepairPanel::OnClearBatch() {
    if (controller_->IsRunning() || batch_running_)
        return;
    batch_table_->setRowCount(0);
}

void RepairPanel::OnCancel() {
    if (!controller_->IsRunning()) {
        // 批量模式的空档：上一项已结束、报告仍在异步写盘，下一项尚未启动。
        // 此时直接结束队列，不再等待报告（报告本身会照常写完）。
        if (batch_running_) {
            batch_canceled_ = true;
            FinishBatch(batch_index_ + 1);
        }
        return;
    }
    cancel_button_->setEnabled(false);
    stage_label_->setText(QStringLiteral("正在取消…"));
    if (batch_running_)
        batch_canceled_ = true;
    controller_->Cancel();
}

void RepairPanel::OnOpenOutput() {
    if (last_output_path_.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(last_output_path_));
}

void RepairPanel::OnOpenReport() {
    if (last_report_html_path_.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(last_report_html_path_));
}

void RepairPanel::OnProgress(quint64 task_id, int percent, const QString& stage, const QString& detail) {
    Q_UNUSED(task_id);
    progress_bar_->setValue(std::clamp(percent, 0, 100));
    QString text;
    if (batch_running_ && batch_index_ >= 0 && batch_index_ < static_cast<int>(batch_requests_.size()))
        text = QStringLiteral("批量 %1/%2 · ").arg(batch_index_ + 1).arg(batch_requests_.size());
    text += stage;
    if (!detail.isEmpty())
        text += QStringLiteral(" · ") + detail;
    stage_label_->setText(text);
}

void RepairPanel::OnFinished(quint64 task_id, const QString& status_text, const QString& summary) {
    Q_UNUSED(task_id);
    Q_UNUSED(status_text);
    Q_UNUSED(summary);
    progress_bar_->setValue(100);

    if (!batch_running_) {
        SetRunning(false);
        stage_label_->setText(QStringLiteral("已完成"));
        ShowResult();
        return;
    }

    // 批量：先记录当前项的结论，控件保持禁用；报告写完（PollReportWrite）后才推进。
    auto result = controller_->result();
    if (batch_index_ >= 0 && batch_index_ < static_cast<int>(batch_requests_.size())) {
        if (result) {
            MarkBatchRow(batch_index_, QString::fromStdString(model::DisplayName(result->status)),
                         StatusColor(result->status));
        } else {
            MarkBatchRow(batch_index_, QStringLiteral("无结果"), QString::fromLatin1(theme::color::kDanger));
        }
    }
    ShowResult();
    stage_label_->setText(report_future_.valid() ? QStringLiteral("等待报告写入…") : QStringLiteral("准备下一项…"));
    if (!report_future_.valid())
        AdvanceBatch();
}

void RepairPanel::SetRunning(bool running) {
    const bool idle = !running;
    start_button_->setEnabled(idle);
    cancel_button_->setEnabled(!idle);
    source_edit_->setEnabled(idle);
    reference_edit_->setEnabled(idle);
    output_edit_->setEnabled(idle);
    mode_combo_->setEnabled(idle);
    container_combo_->setEnabled(idle);
    level_combo_->setEnabled(idle);
    gap_combo_->setEnabled(idle);
    segment_check_->setEnabled(idle);
    timeline_check_->setEnabled(idle);
    fill_missing_check_->setEnabled(idle && timeline_check_->isChecked());
    single_track_check_->setEnabled(idle);
    keep_partial_check_->setEnabled(idle);
    batch_table_->setEnabled(idle);
    batch_add_button_->setEnabled(idle);
    batch_remove_button_->setEnabled(idle);
    batch_clear_button_->setEnabled(idle);
    if (running) {
        open_output_button_->setEnabled(false);
        open_report_button_->setEnabled(false);
    }
}

void RepairPanel::ClearPendingReport() {
    if (report_timer_)
        report_timer_->stop();
    if (report_future_.valid())
        report_future_.wait();
    pending_report_html_.clear();
    pending_report_json_.clear();
}

void RepairPanel::ResetForNewRun() {
    ClearPendingReport();
    open_report_button_->setEnabled(false);
    progress_bar_->setValue(0);
    stage_label_->setText(QStringLiteral("准备中…"));
    status_badge_->setText(QStringLiteral("诊断中"));
    status_badge_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kAccent)));
    summary_label_->setText(QStringLiteral("正在读取文件并建立诊断证据…"));
    for (auto* label : metric_labels_)
        label->setText(QStringLiteral("—"));
    findings_table_->setRowCount(0);
    report_label_->setText(last_write_report_ ? QStringLiteral("修复结束后会自动异步写入 JSON / TXT / HTML 报告")
                                              : QStringLiteral("本次请求不生成报告"));
    last_output_path_.clear();
    last_report_html_path_.clear();
    last_report_json_path_.clear();
}

void RepairPanel::ShowResult() {
    auto result = controller_->result();
    if (!result) {
        status_badge_->setText(QStringLiteral("无结果"));
        summary_label_->setText(QStringLiteral("任务结束但没有拿到结果，请查看日志。"));
        return;
    }

    progress_bar_->setValue(100);

    // 报告在界面层落盘（core/qt 不依赖 core/reporting），且走后台线程：
    // 报告体积增大或存储慢时不再卡住 UI 线程。产物发布与报告保存是两个独立结果。
    if (!last_write_report_) {
        report_label_->setText(QStringLiteral("本次请求不生成报告（write_report=false）"));
        open_report_button_->setEnabled(false);
    } else if (!result->report_json_path.empty()) {
        StartReportWrite(result);
    } else {
        report_label_->setText(QStringLiteral("没有报告输出路径"));
        open_report_button_->setEnabled(false);
    }

    UpdateOutputs(*result);

    if (result->output_published && !result->output_path.empty()) {
        last_output_path_ = QString::fromStdString(result->output_path);
        open_output_button_->setEnabled(true);
    } else {
        last_output_path_.clear();
        open_output_button_->setEnabled(false);
    }

    status_badge_->setText(QString::fromStdString(model::DisplayName(result->status)));
    status_badge_->setStyleSheet(QStringLiteral("color:%1;").arg(StatusColor(result->status)));

    QString summary = QString::fromStdString(result->summary);
    if (!result->error.empty() && summary != QString::fromStdString(result->error))
        summary += QStringLiteral("（") + QString::fromStdString(result->error) + QStringLiteral("）");
    summary_label_->setText(summary);

    std::vector<model::DamageFinding> findings = result->diagnosis.findings;
    findings.insert(findings.end(), result->residual_findings.begin(), result->residual_findings.end());
    PopulateFindings(findings);
}

void RepairPanel::StartReportWrite(const std::shared_ptr<const model::RepairResult>& result) {
    ClearPendingReport();
    const std::string json_path = result->report_json_path;
    const std::string text_path = result->report_text_path;
    const std::string html_path = result->report_html_path;
    pending_report_html_ = QString::fromStdString(html_path);
    pending_report_json_ = QString::fromStdString(json_path);

    report_label_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kTextSecondary)));
    report_label_->setText(QStringLiteral("报告写入中…（产物发布结果单独显示）"));

    // 只捕获只读结果快照与路径，不捕获 this —— worker 不接触任何 QWidget。
    report_future_ = std::async(std::launch::async, [result, json_path, text_path, html_path]() {
        return reporting::RepairReportExporter::WriteAll(*result, json_path, text_path, html_path);
    });
    report_timer_->start();
}

void RepairPanel::PollReportWrite() {
    if (!report_future_.valid())
        return;
    if (report_future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        return;
    report_timer_->stop();
    const reporting::RepairReportWriteResult written = report_future_.get();
    if (written.AllOk()) {
        last_report_html_path_ = pending_report_html_;
        last_report_json_path_ = pending_report_json_;
        report_label_->setStyleSheet(
            QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kTextSecondary)));
        report_label_->setText(
            QStringLiteral("报告已写入：%1（JSON: %2）").arg(last_report_html_path_, last_report_json_path_));
        report_label_->setToolTip(report_label_->text());
        open_report_button_->setEnabled(true);
    } else {
        report_label_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::color::kDanger)));
        report_label_->setText(
            QStringLiteral("报告写入失败：%1（产物不受影响）").arg(QString::fromStdString(written.error)));
        open_report_button_->setEnabled(false);
    }
    pending_report_html_.clear();
    pending_report_json_.clear();
    // 批量：报告落盘完成才算这一项真正收尾，之后才能清空界面进入下一项。
    if (batch_running_)
        AdvanceBatch();
}

void RepairPanel::UpdateOutputs(const model::RepairResult& result) {
    const bool verify_ok = result.verification.status == model::RepairCheckStatus::Passed;
    const bool verify_bad = result.verification.status == model::RepairCheckStatus::Failed;

    QString playability;
    if (result.verification.structure_only) {
        // V0 只做结构验证，不能显示"通过"或暗示可播放。
        playability = QStringLiteral("结构有效，尚未验证播放");
    } else if (result.verification.status == model::RepairCheckStatus::NotRun) {
        playability = QStringLiteral("未验证（未产出可验证内容）");
    } else {
        playability = QStringLiteral("%1 %2").arg(QString::fromUtf8(model::DisplayName(result.verification.level)),
                                                  verify_ok    ? QStringLiteral("通过")
                                                  : verify_bad ? QStringLiteral("未通过")
                                                               : QStringLiteral("未完全通过"));
    }
    SetMetric(0, playability,
              verify_ok    ? QString::fromLatin1(theme::color::kSuccess)
              : verify_bad ? QString::fromLatin1(theme::color::kDanger)
                           : QString::fromLatin1(theme::color::kWarning));

    QString content;
    if (result.fidelity.content_completeness == "complete_for_selected_checks") {
        content = QStringLiteral("所选检查范围内完整");
    } else if (result.fidelity.content_completeness == "partial") {
        content = QStringLiteral("存在截断 / 读取中断，部分内容缺失");
    } else {
        content = QStringLiteral("未知（未产出可核对内容）");
    }
    if (!result.recovered_segments.empty())
        content += QStringLiteral(" · 恢复 %1 段").arg(result.recovered_segments.size());
    if (!result.plan.dropped_input_streams.empty())
        content += QStringLiteral(" · 省略 %1 条轨").arg(result.plan.dropped_input_streams.size());
    SetMetric(1, content,
              result.fidelity.content_completeness == "complete_for_selected_checks" &&
                      result.recovered_segments.empty() && result.plan.dropped_input_streams.empty()
                  ? QString::fromLatin1(theme::color::kSuccess)
              : result.fidelity.content_completeness == "partial" || !result.recovered_segments.empty() ||
                      !result.plan.dropped_input_streams.empty()
                  ? QString::fromLatin1(theme::color::kWarning)
                  : QString::fromLatin1(theme::color::kTextSecondary));

    QString timeline;
    if (result.fidelity.timeline_status == "unchanged") {
        timeline = QStringLiteral("未改写（时基已核验）");
    } else if (result.fidelity.timeline_status == "adjusted") {
        timeline = QStringLiteral("已主动调整");
        if (result.timeline_repair.rewritten_timestamps > 0)
            timeline += QStringLiteral(" · 重写 %1 个时间戳").arg(result.timeline_repair.rewritten_timestamps);
        if (result.timeline_repair.gap_removed)
            timeline += QStringLiteral(" · 合并缺口 %1s")
                            .arg(QString::number(result.timeline_repair.removed_gap_seconds, 'f', 3));
    } else if (result.fidelity.timeline_status == "modified") {
        timeline = QStringLiteral("实测不符，播放速度可能已变");
    } else {
        timeline = QStringLiteral("无法判定（时基未核验）");
    }
    SetMetric(2, timeline,
              result.fidelity.timeline_status == "modified"   ? QString::fromLatin1(theme::color::kDanger)
              : result.fidelity.timeline_status == "adjusted" ? QString::fromLatin1(theme::color::kWarning)
                                                              : QString::fromLatin1(theme::color::kTextSecondary));

    QString quality;
    if (result.fidelity.video_reencoded || result.fidelity.audio_reencoded) {
        quality = result.fidelity.perceptual_fidelity == "measured_psnr"
                      ? QStringLiteral("已重编码 · 抽帧 PSNR 已量化")
                      : QStringLiteral("存在重编码 · 画质风险需人工复核");
    } else if (result.output_published) {
        quality = QStringLiteral("保码流 · 画质风险未量化");
    } else {
        quality = QStringLiteral("无产物");
    }
    SetMetric(3, quality,
              (result.fidelity.video_reencoded || result.fidelity.audio_reencoded)
                  ? QString::fromLatin1(theme::color::kWarning)
                  : QString::fromLatin1(theme::color::kTextSecondary));

    QString encoding;
    if (result.fidelity.video_reencoded || result.fidelity.audio_reencoded) {
        encoding = QStringLiteral("发生重编码");
    } else if (result.fidelity.video == model::RepairFidelityStatus::VerifiedMatch) {
        encoding = QStringLiteral("未重编码 · 视频载荷哈希一致");
    } else if (result.fidelity.video == model::RepairFidelityStatus::Failed) {
        encoding = QStringLiteral("未重编码 · 载荷哈希不一致");
    } else if (result.output_published) {
        encoding = QStringLiteral("未重编码 · 载荷未核对");
    } else {
        encoding = QStringLiteral("无产物");
    }
    SetMetric(4, encoding,
              (result.fidelity.video_reencoded || result.fidelity.audio_reencoded ||
               result.fidelity.video == model::RepairFidelityStatus::Failed)
                  ? QString::fromLatin1(theme::color::kDanger)
                  : QString::fromLatin1(theme::color::kTextSecondary));

    QString coverage = QStringLiteral("%1 · 解码 %2 帧 · 跳转 %3/%4")
                           .arg(QString::fromUtf8(model::DisplayName(result.verification.level)))
                           .arg(result.verification.decoded_frames)
                           .arg(result.verification.seek_passes)
                           .arg(result.verification.seek_checks);
    if (result.verification.decode_errors > 0)
        coverage += QStringLiteral(" · 错误 %1").arg(result.verification.decode_errors);
    SetMetric(5, coverage,
              result.verification.decode_errors > 0 ? QString::fromLatin1(theme::color::kWarning)
                                                    : QString::fromLatin1(theme::color::kTextPrimary));
}

void RepairPanel::SetMetric(int index, const QString& value, const QString& color) {
    if (index < 0 || index >= 6 || !metric_labels_[index])
        return;
    metric_labels_[index]->setText(value);
    metric_labels_[index]->setStyleSheet(QStringLiteral("color:%1;").arg(color));
}

void RepairPanel::PopulateFindings(const std::vector<model::DamageFinding>& findings) {
    findings_table_->clearSpans();
    findings_table_->setRowCount(0);
    for (const auto& finding : findings) {
        const int row = findings_table_->rowCount();
        findings_table_->insertRow(row);

        auto* layer = new QTableWidgetItem(QString::fromStdString(model::ToString(finding.layer)));
        layer->setToolTip(QString::fromStdString(finding.code));
        findings_table_->setItem(row, 0, layer);

        auto* evidence = new QTableWidgetItem(QString::fromStdString(finding.evidence));
        evidence->setToolTip(QString::fromStdString(finding.evidence));
        findings_table_->setItem(row, 1, evidence);

        auto* location = new QTableWidgetItem(LocationText(finding.location));
        location->setToolTip(QString::fromStdString(finding.location.provenance));
        findings_table_->setItem(row, 2, location);

        auto* severity = new QTableWidgetItem(QString::fromUtf8(SeverityText(finding.severity)));
        severity->setForeground(QColor(SeverityColor(finding.severity)));
        findings_table_->setItem(row, 3, severity);

        findings_table_->setRowHeight(row, 34);
    }
    if (findings.empty()) {
        findings_table_->setRowCount(1);
        auto* empty = new QTableWidgetItem(QStringLiteral("没有检测到结构级损伤证据"));
        empty->setForeground(QColor(QString::fromLatin1(theme::color::kTextMuted)));
        findings_table_->setItem(0, 0, empty);
        findings_table_->setSpan(0, 0, 1, 4);
    }
}

} // namespace ui
} // namespace videoeye
