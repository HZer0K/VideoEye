#include "ui/analysis_panel/DiagnosticsIssueView.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

namespace {

// 严重度筛选的档位：(显示文案, IssueSeverity)；-1 表示"全部"。
// 顺序按严重度从高到低，与问题清单的默认观感一致。
struct SeverityOption {
    const char* label;
    int value;
};

const SeverityOption kSeverityOptions[] = {
    {"全部严重度", -1},
    {"致命", static_cast<int>(model::IssueSeverity::Critical)},
    {"错误", static_cast<int>(model::IssueSeverity::Error)},
    {"警告", static_cast<int>(model::IssueSeverity::Warning)},
    {"提示", static_cast<int>(model::IssueSeverity::Info)},
};

constexpr int kAllSeverities = -1;
constexpr int kAllCategories = -1;

QString SeverityColor(model::IssueSeverity severity) {
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

QString StreamText(const model::DiagnosticIssue& issue) {
    if (issue.stream_index < 0)
        return QObject::tr("文件级");
    return QObject::tr("流 #%1").arg(issue.stream_index);
}

// 位置：文件级问题没有时间点，写成"全局"而不是 0.0 s —— 后者看起来像"开头有问题"。
QString PositionText(const model::DiagnosticIssue& issue) {
    if (issue.IsGlobal())
        return QObject::tr("全局");
    return QString::fromStdString(issue.range.ToString());
}

} // namespace

DiagnosticsIssueView::DiagnosticsIssueView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void DiagnosticsIssueView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(6);

    // ---- 筛选行：严重度 / 类别 + 条数 ----
    QWidget* filter_row = new QWidget(this);
    QHBoxLayout* filter_layout = new QHBoxLayout(filter_row);
    filter_layout->setContentsMargins(0, 0, 0, 0);
    filter_layout->setSpacing(6);

    auto make_hint = [filter_row](const QString& text) {
        QLabel* label = new QLabel(text, filter_row);
        label->setObjectName(QStringLiteral("HintLabel"));
        return label;
    };
    filter_layout->addWidget(make_hint(tr("严重度")));
    severity_filter_ = new QComboBox(filter_row);
    severity_filter_->setToolTip(tr("只看某一档严重度的问题；「全部严重度」不筛选"));
    for (const SeverityOption& option : kSeverityOptions)
        severity_filter_->addItem(tr(option.label), option.value);
    connect(severity_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { RebuildIssueTable(); });
    filter_layout->addWidget(severity_filter_);

    filter_layout->addWidget(make_hint(tr("类别")));
    category_filter_ = new QComboBox(filter_row);
    category_filter_->setToolTip(tr("只看某一类问题；「全部类别」不筛选"));
    // 类别表直接来自 domain 枚举，避免界面上另抄一份中文清单
    category_filter_->addItem(tr("全部类别"), kAllCategories);
    for (int value = static_cast<int>(model::IssueCategory::Container);
         value <= static_cast<int>(model::IssueCategory::Other); ++value) {
        const auto category = static_cast<model::IssueCategory>(value);
        category_filter_->addItem(QString::fromUtf8(model::ToString(category)), value);
    }
    connect(category_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { RebuildIssueTable(); });
    filter_layout->addWidget(category_filter_);

    filter_layout->addStretch();
    count_label_ = make_hint(tr("共 0 条"));
    filter_layout->addWidget(count_label_);
    layout->addWidget(filter_row);

    // ---- 主体：曲线（上） / 问题表 + 详情（下） ----
    // 用分割器而不是死板的三段：纵向空间紧张时用户可以把曲线压扁，
    // 把高度全给问题表（以前"图表 220 + 表 200"的最小高度直接把页面顶出滚动条）。
    QSplitter* body = new QSplitter(Qt::Vertical, this);

    chart_ = new MetricChartWidget(body);
    chart_->SetTitle(tr("逐秒码率 / 帧率"));
    bitrate_series_ = chart_->AddLineSeries(tr("码率 (kbps)"), QColor("#1e88e5"));
    fps_series_ = chart_->AddLineSeries(tr("帧率 (fps)"), QColor("#43a047"));
    axis_x_ = chart_->AxisX();
    axis_bitrate_ = chart_->AxisY(); // 左轴
    axis_fps_ = chart_->AxisY2();    // 右轴
    chart_->SetAxisY2Visible(true);
    chart_->AttachAxis(fps_series_, axis_fps_);
    axis_x_->SetTitleText(tr("时间 (s)"));
    axis_bitrate_->SetTitleText(tr("kbps"));
    axis_fps_->SetTitleText(tr("fps"));
    chart_->setMinimumHeight(150);
    body->addWidget(chart_);

    QSplitter* lower = new QSplitter(Qt::Horizontal, body);
    lower->setChildrenCollapsible(false);

    issue_table_ = new QTableWidget(0, 4, lower);
    issue_table_->setHorizontalHeaderLabels({tr("严重度"), tr("问题"), tr("位置"), tr("类别")});
    issue_table_->verticalHeader()->setVisible(false);
    issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    issue_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    issue_table_->setTextElideMode(Qt::ElideRight);
    issue_table_->setToolTip(tr("选中一行，右侧显示说明、证据与建议；双击可直接跳到该时间点"));
    issue_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    issue_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    issue_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    issue_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    issue_table_->setMinimumWidth(320);
    issue_table_->setMinimumHeight(140);
    connect(issue_table_, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) { UpdateDetail(); });
    connect(issue_table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { OnJumpToIssue(); });
    lower->addWidget(issue_table_);

    QWidget* detail_panel = new QWidget(lower);
    QVBoxLayout* detail_layout = new QVBoxLayout(detail_panel);
    detail_layout->setContentsMargins(6, 0, 0, 0);
    detail_layout->setSpacing(4);
    QLabel* detail_title = new QLabel(tr("问题详情"), detail_panel);
    detail_title->setObjectName(QStringLiteral("HintLabel"));
    detail_layout->addWidget(detail_title);
    detail_text_ = new QTextBrowser(detail_panel);
    detail_text_->setOpenExternalLinks(false);
    detail_text_->setMinimumWidth(240);
    detail_layout->addWidget(detail_text_, 1);

    // 跳转是按问题清单里选中那一行的位置 seek；时间轴的样本/曲线/导出已收敛到
    // 「事件与时间轴」页，本子页只保留"问题 -> 跳转"这一条动作。
    jump_button_ = new QPushButton(tr("跳转到问题位置"), detail_panel);
    connect(jump_button_, &QPushButton::clicked, this, &DiagnosticsIssueView::OnJumpToIssue);
    detail_layout->addWidget(jump_button_);
    lower->addWidget(detail_panel);

    lower->setStretchFactor(0, 3);
    lower->setStretchFactor(1, 2);
    lower->setSizes({520, 320});
    body->addWidget(lower);

    body->setStretchFactor(0, 1);
    body->setStretchFactor(1, 2);
    layout->addWidget(body, 1);

    UpdateDetail();
}

void DiagnosticsIssueView::SetReport(const model::AnalysisResult& result, const model::QcReport& report) {
    result_ = &result;
    issues_ = report.issues;
    RebuildIssueTable();
    UpdateChart();
}

void DiagnosticsIssueView::Clear() {
    result_ = nullptr;
    issues_.clear();
    visible_issues_.clear();
    if (issue_table_)
        issue_table_->setRowCount(0);
    if (count_label_)
        count_label_->setText(tr("共 0 条"));
    if (bitrate_series_)
        bitrate_series_->Clear();
    if (fps_series_)
        fps_series_->Clear();
    UpdateDetail();
}

bool DiagnosticsIssueView::PassesFilter(const model::DiagnosticIssue& issue) const {
    const int severity = severity_filter_ ? severity_filter_->currentData().toInt() : kAllSeverities;
    if (severity != kAllSeverities && static_cast<int>(issue.severity) != severity)
        return false;
    const int category = category_filter_ ? category_filter_->currentData().toInt() : kAllCategories;
    if (category != kAllCategories && static_cast<int>(issue.category) != category)
        return false;
    return true;
}

void DiagnosticsIssueView::RebuildIssueTable() {
    if (!issue_table_)
        return;

    // 重建（含筛选项变化）要把选中还原到同一条问题上：否则改一次筛选，
    // 右侧详情就被清空，用户得重新找一遍刚看的那条。
    const int keep = CurrentIssueIndex();

    visible_issues_.clear();
    for (std::size_t i = 0; i < issues_.size(); ++i) {
        if (PassesFilter(issues_[i]))
            visible_issues_.push_back(i);
    }

    {
        const QSignalBlocker blocker(issue_table_);
        issue_table_->setRowCount(0);
        issue_table_->setRowCount(static_cast<int>(visible_issues_.size()));
        for (int row = 0; row < static_cast<int>(visible_issues_.size()); ++row) {
            const auto& issue = issues_[visible_issues_[row]];
            SetTableItemText(issue_table_, row, 0, QString::fromStdString(issue.SeverityText()));
            SetTableItemText(issue_table_, row, 1, QString::fromStdString(issue.title));
            SetTableItemText(issue_table_, row, 2, PositionText(issue));
            SetTableItemText(issue_table_, row, 3, QString::fromStdString(issue.CategoryText()));

            if (QTableWidgetItem* cell = issue_table_->item(row, 0)) {
                cell->setForeground(QColor(SeverityColor(issue.severity)));
            }
            // 长文本进了详情区，但标题仍可能在窄窗口里被截断 —— 悬停给全量。
            if (QTableWidgetItem* cell = issue_table_->item(row, 1)) {
                cell->setToolTip(QString::fromStdString(issue.detail));
            }
        }
    }

    if (count_label_) {
        count_label_->setText(issues_.size() == visible_issues_.size()
                                  ? tr("共 %1 条").arg(static_cast<int>(issues_.size()))
                                  : tr("共 %1 条 · 显示 %2 条")
                                        .arg(static_cast<int>(issues_.size()))
                                        .arg(static_cast<int>(visible_issues_.size())));
    }

    int row_to_select = -1;
    if (keep >= 0) {
        for (int row = 0; row < static_cast<int>(visible_issues_.size()); ++row) {
            if (static_cast<int>(visible_issues_[row]) == keep) {
                row_to_select = row;
                break;
            }
        }
    }
    if (row_to_select >= 0) {
        issue_table_->setCurrentCell(row_to_select, 0);
        issue_table_->selectRow(row_to_select);
    }
    UpdateDetail();
}

int DiagnosticsIssueView::IssueIndexForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(visible_issues_.size()))
        return -1;
    return static_cast<int>(visible_issues_[row]);
}

int DiagnosticsIssueView::CurrentIssueIndex() const {
    if (!issue_table_)
        return -1;
    return IssueIndexForRow(issue_table_->currentRow());
}

void DiagnosticsIssueView::UpdateDetail() {
    if (!detail_text_)
        return;
    const int index = CurrentIssueIndex();
    if (index < 0 || index >= static_cast<int>(issues_.size())) {
        detail_text_->setHtml(QStringLiteral("<p style=\"color:#8B949E;\">%1</p>")
                                  .arg(tr("选择上方任意一条问题，这里显示它的说明、证据与修复建议。")));
        if (jump_button_) {
            jump_button_->setEnabled(false);
            jump_button_->setToolTip(tr("请先在问题列表中选择一条记录"));
        }
        return;
    }

    const model::DiagnosticIssue& issue = issues_[static_cast<std::size_t>(index)];
    const QString title = QString::fromStdString(issue.title).toHtmlEscaped();
    const QString detail = QString::fromStdString(issue.detail).toHtmlEscaped();
    const QString suggestion = QString::fromStdString(issue.suggestion).toHtmlEscaped();

    QString html;
    html += QStringLiteral("<div style=\"font-size:14px;font-weight:600;color:%1;\">%2</div>")
                .arg(SeverityColor(issue.severity), title);
    // 元信息一行：严重度 · 类别 · 位置 · 流；口径不写清，用户会把"文件级"读成"0 秒处"
    html +=
        QStringLiteral("<div style=\"color:#A5B1BD;font-size:12px;\">%1 · %2 · %3 · %4</div>")
            .arg(QString::fromStdString(issue.SeverityText()).toHtmlEscaped(),
                 QString::fromStdString(issue.CategoryText()).toHtmlEscaped(), PositionText(issue), StreamText(issue));

    auto section = [&html](const QString& heading, const QString& body) {
        html += QStringLiteral("<p style=\"margin-top:10px;margin-bottom:2px;\"><b>%1</b></p>").arg(heading);
        html += QStringLiteral("<p style=\"color:#C9D1D9;margin-top:0;\">%1</p>").arg(body);
    };

    section(tr("说明"), detail.isEmpty() ? tr("（无）") : detail);

    // 证据：规则原文 + 实测/阈值 + 出现次数。阈值与实测值都为 0 时不写 ——
    // "实测 0 ≤ 阈值 0" 对用户没有任何信息量，反而像数据缺失。
    QString evidence =
        QStringLiteral("%1: <code>%2</code>").arg(tr("规则"), QString::fromStdString(issue.rule_id).toHtmlEscaped());
    if (issue.threshold != 0.0 || issue.metric_value != 0.0) {
        evidence += QStringLiteral("<br/>%1: %2")
                        .arg(tr("实测值 / 阈值"), tr("%1 / %2").arg(QString::number(issue.metric_value, 'f', 2),
                                                                    QString::number(issue.threshold, 'f', 2)));
    }
    if (issue.occurrence_count > 1) {
        evidence += QStringLiteral("<br/>%1: %2").arg(tr("出现次数"), QString::number(issue.occurrence_count));
    }
    section(tr("证据"), evidence);

    section(tr("建议"), suggestion.isEmpty() ? tr("（无）") : suggestion);
    detail_text_->setHtml(html);

    if (jump_button_) {
        const bool can_jump = !issue.IsGlobal();
        jump_button_->setEnabled(can_jump);
        jump_button_->setToolTip(can_jump ? tr("按当前选中问题的位置执行 seek")
                                          : tr("该问题为文件级问题，没有可跳转的时间点"));
    }
}

bool DiagnosticsIssueView::SelectIssue(const QString& rule_id, double start_seconds, int stream_index) {
    if (issues_.empty() || !issue_table_)
        return false;

    // exact=true 比"规则 + 流 + 起点"，false 只比规则 id（同规则的多条里退到第一条）。
    auto find_row = [this, &rule_id, start_seconds, stream_index](bool exact) -> int {
        for (std::size_t row = 0; row < visible_issues_.size(); ++row) {
            const auto& issue = issues_[visible_issues_[row]];
            if (QString::fromStdString(issue.rule_id) != rule_id)
                continue;
            if (!exact)
                return static_cast<int>(row);
            if (issue.stream_index != stream_index)
                continue;
            if (std::fabs(issue.range.start_seconds - start_seconds) > 1e-6)
                continue;
            return static_cast<int>(row);
        }
        return -1;
    };

    auto reset_filters = [this]() {
        const QSignalBlocker block_severity(severity_filter_);
        const QSignalBlocker block_category(category_filter_);
        if (severity_filter_)
            severity_filter_->setCurrentIndex(0);
        if (category_filter_)
            category_filter_->setCurrentIndex(0);
        RebuildIssueTable();
    };

    int row = find_row(true);
    if (row < 0) {
        // 目标可能被当前筛选挡住："一步直达"不该因为几轮之前的筛选选择而失效
        reset_filters();
        row = find_row(true);
    }
    if (row < 0)
        row = find_row(false); // 位置/流对不上（例：报告重算过）就退到同规则的第一条
    if (row < 0)
        return false;

    issue_table_->setCurrentCell(row, 0);
    issue_table_->selectRow(row);
    if (QTableWidgetItem* item = issue_table_->item(row, 0))
        issue_table_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    UpdateDetail();
    return true;
}

void DiagnosticsIssueView::UpdateChart() {
    if (!bitrate_series_ || !fps_series_ || result_ == nullptr)
        return;
    bitrate_series_->Clear();
    fps_series_->Clear();

    const auto& bitrate =
        result_->video_bitrate_kbps.IsEmpty() ? result_->total_bitrate_kbps : result_->video_bitrate_kbps;
    // 抽稀后再提交：长时间分析的采样点上万，全量喂给自绘图表重绘会卡死 UI
    AppendDecimated(bitrate_series_, bitrate, kMaxChartPoints);
    AppendDecimated(fps_series_, result_->video_fps, kMaxChartPoints);

    double max_t = 1.0;
    if (!bitrate.samples.empty())
        max_t = std::max(max_t, bitrate.samples.back().timestamp_seconds);
    if (!result_->video_fps.samples.empty()) {
        max_t = std::max(max_t, result_->video_fps.samples.back().timestamp_seconds);
    }
    axis_x_->SetRange(0, max_t);
    axis_bitrate_->SetRange(0, std::max(1.0, bitrate.Max() * 1.2));
    axis_fps_->SetRange(0, std::max(1.0, result_->video_fps.Max() * 1.2));
}

void DiagnosticsIssueView::OnJumpToIssue() {
    const int index = CurrentIssueIndex();
    if (index < 0) {
        // 双击空表 / 点按钮时没选中：这是操作顺序问题，不是错误，别弹模态框
        return;
    }
    const model::DiagnosticIssue& issue = issues_[static_cast<std::size_t>(index)];
    // 文件级问题没有时间点：按钮本就置灰，这里再兜一次（双击行也会走到这）
    if (issue.IsGlobal())
        return;
    emit SeekRequested(issue.range.start_seconds);
}

} // namespace ui
} // namespace videoeye
