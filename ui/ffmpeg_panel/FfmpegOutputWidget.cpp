#include "ui/ffmpeg_panel/FfmpegOutputWidget.h"

#include "ui/ffmpeg_panel/FfmpegChangeGraphView.h"

#include <QClipboard>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegFailureDiagnostics.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {
namespace {

constexpr int kMaxLogChars = 4 * 1024 * 1024; // 日志缓存上限，防止长任务把内存吃光
// 攒批上限。50ms 一次的定时器在主线程被长操作顶住时不会触发，这里兜一个硬上限，
// 免得 -loglevel debug 底下先把内存吃光。
constexpr int kMaxPendingLines = 2000;
constexpr int kLogFlushIntervalMs = 50;
// 历史行数上限与视图的 maximumBlockCount 对齐：视图留不住的旧行，历史也不必留着，
// 否则「过滤一下」能翻出视图里根本没有的内容，用户会以为是 bug。
constexpr int kMaxHistoryLines = 20000;
// 高亮上限：-loglevel debug 里过滤 "frame" 能命中上万处，全标上比不标更卡。
constexpr int kMaxHighlightSpans = 800;
constexpr int kFilterDebounceMs = 150;

QString FormatDuration(qint64 ms) {
    if (ms < 1000) {
        return QStringLiteral("%1 ms").arg(ms);
    }
    const double sec = ms / 1000.0;
    if (sec < 60.0) {
        return QString::number(sec, 'f', 1) + QStringLiteral(" s");
    }
    const int minutes = static_cast<int>(sec) / 60;
    const double rest = sec - minutes * 60.0;
    return QStringLiteral("%1 分 %2 秒").arg(minutes).arg(QString::number(rest, 'f', 1));
}

QString EscapeHtml(const QString& text) {
    return text.toHtmlEscaped();
}

void AppendLines(QPlainTextEdit* view, const QStringList& lines) {
    if (lines.isEmpty()) {
        return;
    }
    // 一次写多行：appendPlainText 里的换行会切成多个段落，滚动条只动一次。
    view->appendPlainText(lines.join(QLatin1Char('\n')));
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
}

bool LineMatches(const QString& line, const QString& needle) {
    // 空过滤词 = 不过滤。大小写不敏感：ffmpeg 的报错首字母大写（Invalid / Error），
    // 让人必须先记住大小写才能搜到东西是说不过去的。
    return needle.isEmpty() || line.contains(needle, Qt::CaseInsensitive);
}

QStringList SelectLines(const QStringList& lines, const QString& needle) {
    if (needle.isEmpty()) {
        return lines;
    }
    QStringList out;
    out.reserve(lines.size());
    for (const QString& line : lines) {
        if (LineMatches(line, needle)) {
            out.append(line);
        }
    }
    return out;
}

void ReplaceViewText(QPlainTextEdit* view, const QStringList& lines) {
    // 重建期间不要触发 view 自己的信号，也不要在半成品状态上滚到底
    const QSignalBlocker blocker(view);
    view->setPlainText(lines.join(QLatin1Char('\n')));
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
}

/// 命中片段的高亮选择集（只标匹配到的那几个字符，不是整行）
QList<QTextEdit::ExtraSelection> BuildMatchSelections(QPlainTextEdit* view, const QString& needle) {
    QList<QTextEdit::ExtraSelection> selections;
    if (needle.isEmpty()) {
        return selections;
    }
    QColor background;
    background.setNamedColor(QString::fromLatin1(theme::color::kWarning));
    background.setAlpha(70); // 半透明：底色是浅是深都能看清文字
    QTextCharFormat format;
    format.setBackground(background);

    QTextBlock block = view->document()->firstBlock();
    while (block.isValid() && selections.size() < kMaxHighlightSpans) {
        const QString text = block.text();
        int from = text.indexOf(needle, 0, Qt::CaseInsensitive);
        while (from >= 0 && selections.size() < kMaxHighlightSpans) {
            QTextCursor cursor(block);
            cursor.setPosition(block.position() + from);
            cursor.setPosition(block.position() + from + needle.size(), QTextCursor::KeepAnchor);
            QTextEdit::ExtraSelection selection;
            selection.cursor = cursor;
            selection.format = format;
            selections.append(selection);
            from = text.indexOf(needle, from + needle.size(), Qt::CaseInsensitive);
        }
        block = block.next();
    }
    return selections;
}

} // namespace

FfmpegOutputWidget::FfmpegOutputWidget(QWidget* parent) : QGroupBox(tr("输出与解释"), parent) {
    log_flush_timer_ = new QTimer(this);
    log_flush_timer_->setSingleShot(true);
    log_flush_timer_->setInterval(kLogFlushIntervalMs);
    connect(log_flush_timer_, &QTimer::timeout, this, &FfmpegOutputWidget::FlushLog);

    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(kFilterDebounceMs);
    connect(filter_debounce_, &QTimer::timeout, this, [this] { ApplyLogFilter(); });

    SetupUi();
}

void FfmpegOutputWidget::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);

    // ---- 状态行 ----
    QHBoxLayout* status_row = new QHBoxLayout();
    status_row->setSpacing(12);
    status_label_ = new QLabel(tr("状态: 未运行"), this);
    progress_label_ = new QLabel(tr("进度: —"), this);
    progress_label_->setToolTip(ProgressHint());
    exit_code_label_ = new QLabel(tr("退出码: —"), this);
    elapsed_label_ = new QLabel(tr("耗时: —"), this);
    copy_button_ = new QPushButton(tr("复制日志"), this);
    save_button_ = new QPushButton(tr("保存日志…"), this);
    connect(copy_button_, &QPushButton::clicked, this, &FfmpegOutputWidget::OnCopyLog);
    connect(save_button_, &QPushButton::clicked, this, &FfmpegOutputWidget::OnSaveLog);
    status_row->addWidget(status_label_);
    status_row->addWidget(progress_label_);
    status_row->addWidget(exit_code_label_);
    status_row->addWidget(elapsed_label_);
    status_row->addStretch(1);
    status_row->addWidget(copy_button_);
    status_row->addWidget(save_button_);
    layout->addLayout(status_row);

    output_tabs_ = new QTabWidget(this);

    // Tab 1: 命令解释
    QWidget* explain_page = new QWidget(output_tabs_);
    QVBoxLayout* explain_layout = new QVBoxLayout(explain_page);
    explain_layout->setContentsMargins(4, 4, 4, 4);
    explain_layout->setSpacing(4);
    QSplitter* explain_splitter = new QSplitter(Qt::Vertical, explain_page);
    explain_list_ = new QListWidget(explain_splitter);
    explain_list_->setFont(theme::font::monoFont(9));
    explain_list_->setAlternatingRowColors(true);
    connect(explain_list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { OnExplanationCurrentChanged(); });
    explain_detail_ = new QTextBrowser(explain_splitter);
    explain_detail_->setOpenExternalLinks(true);
    explain_splitter->addWidget(explain_list_);
    explain_splitter->addWidget(explain_detail_);
    explain_splitter->setStretchFactor(0, 1);
    explain_splitter->setStretchFactor(1, 1);
    explain_splitter->setSizes({180, 220});
    explain_layout->addWidget(explain_splitter);
    output_tabs_->addTab(explain_page, tr("命令解释"));

    // Tab 2 / 3: 日志与错误
    log_view_ = new QPlainTextEdit(output_tabs_);
    log_view_->setReadOnly(true);
    log_view_->setFont(theme::font::monoFont(9));
    log_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_view_->setMaximumBlockCount(20000);
    log_view_->setToolTip(tr("按到达顺序合并 stdout 与 stderr；来自 stderr 的行以 [err] 开头。"));
    output_tabs_->addTab(log_view_, tr("运行日志"));

    error_view_ = new QPlainTextEdit(output_tabs_);
    error_view_->setReadOnly(true);
    error_view_->setFont(theme::font::monoFont(9));
    error_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    error_view_->setMaximumBlockCount(20000);
    error_view_->setToolTip(tr("只显示 stderr。ffmpeg 的正常进度与 banner 也写在 stderr 上，"
                               "所以这里往往是完整输出；要看时间顺序请回到「运行日志」。\n"
                               "失败时末尾会附一段 VideoEye 归纳的诊断（不是 ffmpeg 原话）。"));
    error_view_->setStyleSheet(
        QStringLiteral("QPlainTextEdit { color: %1; }").arg(QLatin1String(theme::color::kWarning)));
    output_tabs_->addTab(error_view_, tr("错误输出"));

    // Tab 4: 转换图谱（输入 → 滤镜 → 编码 → 输出，逐字段标变化）
    // 画布可能比可视区高（多音轨 / 多条字幕），所以放进滚动区：宁可滚动，
    // 也不能把下面几行裁掉 —— 被裁掉的恰好可能是"字幕流被丢了"。
    QScrollArea* graph_scroll = new QScrollArea(output_tabs_);
    graph_scroll->setWidgetResizable(true);
    graph_scroll->setFrameShape(QFrame::NoFrame);
    change_graph_view_ = new FfmpegChangeGraphView(graph_scroll);
    graph_scroll->setWidget(change_graph_view_);
    output_tabs_->addTab(graph_scroll, tr("转换图谱"));

    // ---- 过滤栏 ----
    // 只在两个日志页签上有意义：命令解释页显示的是解析结果，没有"日志行"可过滤。
    // 隐藏而不是禁用，是因为禁用态的输入框会让人以为功能坏了。
    filter_bar_ = new QWidget(this);
    QHBoxLayout* filter_layout = new QHBoxLayout(filter_bar_);
    filter_layout->setContentsMargins(0, 0, 0, 0);
    filter_layout->setSpacing(6);
    filter_layout->addWidget(new QLabel(tr("过滤:"), filter_bar_));
    filter_edit_ = new QLineEdit(filter_bar_);
    filter_edit_->setPlaceholderText(tr("输入一段文字，只留下含它的行（不区分大小写）；Esc 清空"));
    filter_edit_->setToolTip(tr("过滤只改**显示**：复制日志与保存日志拿到的仍是完整内容，\n"
                                "不能因为筛掉了几行就丢掉排障线索。"));
    filter_edit_->setClearButtonEnabled(true);
    filter_edit_->installEventFilter(this); // Esc 清空
    connect(filter_edit_, &QLineEdit::textChanged, this, [this] {
        // 逐字输入时不必每个字符都重建两万行
        if (filter_debounce_ != nullptr) {
            filter_debounce_->start();
        }
    });
    filter_count_label_ = new QLabel(filter_bar_);
    filter_count_label_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::color::kTextSecondary)));
    filter_count_label_->setVisible(false);
    filter_layout->addWidget(filter_edit_, 1);
    filter_layout->addWidget(filter_count_label_);
    layout->addWidget(filter_bar_);
    filter_bar_->setVisible(false); // 默认停在「命令解释」页

    layout->addWidget(output_tabs_, 1);

    connect(output_tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        // 命令解释（0）与转换图谱（3）都不是日志，没有"可过滤的行"。
        filter_bar_->setVisible(index == 1 || index == 2);
        UpdateFilterCount();
        if (index != 0) {
            RefreshHighlights();
        }
    });
}

// ===================== 命令解释 =====================

void FfmpegOutputWidget::RefreshExplanation(const QString& command_text) {
    explain_list_->blockSignals(true);
    explain_list_->clear();
    explain_list_->blockSignals(false);
    explain_detail_->clear();

    const auto parsed = ffmpeg::ParseCommandLine(command_text);
    if (parsed.status != ffmpeg::CommandParseStatus::Ok) {
        if (parsed.status != ffmpeg::CommandParseStatus::Empty) {
            explain_detail_->setHtml(QStringLiteral("<p style='color:%1'>%2</p>")
                                         .arg(QLatin1String(theme::color::kDanger), EscapeHtml(parsed.message)));
        }
        return;
    }

    explanation_ = ffmpeg::FfmpegCommandExplainer::Explain(parsed.arguments);

    explain_list_->blockSignals(true);
    for (const auto& item : explanation_.tokens) {
        QString label = QStringLiteral("[%1] %2").arg(item.role_label, item.token);
        if (!item.value.isEmpty()) {
            label += QStringLiteral(" %1").arg(item.value);
        }
        if (!item.warning.isEmpty()) {
            label += QStringLiteral("  ⚠");
        }
        QListWidgetItem* row = new QListWidgetItem(label);
        if (!item.known && item.role == ffmpeg::FfmpegTokenRole::UnknownOption) {
            row->setForeground(QColor(QLatin1String(theme::color::kTextMuted)));
        }
        explain_list_->addItem(row);
    }
    explain_list_->blockSignals(false);

    // 默认视图: 流程 + 命令级提醒
    QString html = QStringLiteral("<h3 style='margin:2px'>%1</h3>").arg(EscapeHtml(explanation_.flow));
    if (!explanation_.notes.isEmpty()) {
        html += QStringLiteral("<p style='color:%1'><b>提醒</b></p><ul>").arg(QLatin1String(theme::color::kWarning));
        for (const QString& note : explanation_.notes) {
            html += QStringLiteral("<li>%1</li>").arg(EscapeHtml(note));
        }
        html += QStringLiteral("</ul>");
    }
    html += QStringLiteral("<p style='color:%1'>点击上方任意一项查看它的作用、适用位置与示例。</p>")
                .arg(QLatin1String(theme::color::kTextSecondary));
    explain_detail_->setHtml(html);
}

void FfmpegOutputWidget::OnExplanationCurrentChanged() {
    const int row = explain_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(explanation_.tokens.size())) {
        return;
    }
    const auto& item = explanation_.tokens.at(row);

    QString html = QStringLiteral("<h3 style='margin:2px'>%1</h3>").arg(EscapeHtml(item.token));
    if (!item.value.isEmpty()) {
        html += QStringLiteral("<p><b>值:</b> <code>%1</code></p>").arg(EscapeHtml(item.value));
    }
    html += QStringLiteral("<p><b>角色:</b> %1</p>").arg(EscapeHtml(item.role_label));
    if (!item.warning.isEmpty()) {
        html += QStringLiteral("<p style='color:%1'><b>⚠ %2</b></p>")
                    .arg(QLatin1String(theme::color::kWarning), EscapeHtml(item.warning));
    }
    if (!item.description.isEmpty()) {
        html += QStringLiteral("<p>%1</p>")
                    .arg(EscapeHtml(item.description).replace(QLatin1String("\n"), QStringLiteral("<br>")));
    }
    if (!item.position.isEmpty()) {
        html += QStringLiteral("<p><b>适用位置:</b> %1</p>").arg(EscapeHtml(item.position));
    }
    if (!item.example.isEmpty()) {
        html += QStringLiteral("<p><b>示例:</b> <code>%1</code></p>").arg(EscapeHtml(item.example));
    }
    if (!item.typical_values.isEmpty()) {
        html += QStringLiteral("<p><b>常见取值:</b> <code>%1</code></p>")
                    .arg(EscapeHtml(item.typical_values.join(QStringLiteral(" / "))));
    }
    if (!item.related.isEmpty()) {
        html += QStringLiteral("<p><b>相关:</b> %1</p>").arg(EscapeHtml(item.related.join(QStringLiteral("、"))));
    }
    explain_detail_->setHtml(html);
}

// ===================== 运行生命周期 =====================

void FfmpegOutputWidget::SetStatusText(const QString& text, const QString& color) {
    status_label_->setText(text);
    status_label_->setStyleSheet(color.isEmpty() ? QString() : QStringLiteral("color: %1;").arg(color));
}

void FfmpegOutputWidget::ClearOutput() {
    FlushLog(); // 理论上此时缓冲是空的，留着是为了"清空"这个动作本身不留半行残留
    pending_log_lines_.clear();
    pending_error_lines_.clear();
    if (log_flush_timer_ != nullptr) {
        log_flush_timer_->stop();
    }
    // 历史与视图一起清：否则过滤框里敲个字，清掉的内容又"复活"了
    log_history_.clear();
    error_history_.clear();
    log_matched_ = 0;
    error_matched_ = 0;
    log_view_->clear();
    error_view_->clear();
    log_view_->setExtraSelections({});
    error_view_->setExtraSelections({});
    UpdateFilterCount();
    last_log_text_.clear();
    run_elapsed_.invalidate();
    SetStatusText(tr("状态: 未运行"), QString());
    progress_label_->setText(tr("进度: —"));
    progress_label_->setToolTip(ProgressHint());
    exit_code_label_->setText(tr("退出码: —"));
    elapsed_label_->setText(tr("耗时: —"));
    ClearChangeGraph();
}

void FfmpegOutputWidget::PrepareForRun() {
    pending_log_lines_.clear();
    pending_error_lines_.clear();
    if (log_flush_timer_ != nullptr) {
        log_flush_timer_->stop();
    }
    log_history_.clear();
    error_history_.clear();
    log_matched_ = 0;
    error_matched_ = 0;
    log_view_->clear();
    error_view_->clear();
    log_view_->setExtraSelections({});
    error_view_->setExtraSelections({});
    UpdateFilterCount();
    last_log_text_.clear();
    run_elapsed_.start();
    SetStatusText(tr("状态: 运行中"), QLatin1String(theme::color::kAccent));
    progress_label_->setText(tr("进度: —"));
    progress_label_->setToolTip(ProgressHint());
    exit_code_label_->setText(tr("退出码: —"));
    elapsed_label_->setText(tr("耗时: —"));
    // 新一轮：上一轮的图谱不能留在这儿冒充这次的结果
    ClearChangeGraph();
    output_tabs_->setCurrentWidget(log_view_);
}

void FfmpegOutputWidget::ShowChangeGraphs(std::vector<ffmpeg::ChangeGraph> graphs) {
    if (change_graph_view_ == nullptr) {
        return;
    }
    change_graph_view_->SetGraphs(std::move(graphs));
    if (output_tabs_ != nullptr && change_graph_view_->HasGraph()) {
        output_tabs_->setTabToolTip(3, tr("这次运行改变了哪些内容（实测对比）"));
    }
}

void FfmpegOutputWidget::SetChangeGraphStaleHint(const QString& text) {
    if (change_graph_view_ != nullptr) {
        change_graph_view_->SetStaleHint(text);
    }
}

void FfmpegOutputWidget::ClearChangeGraph() {
    if (change_graph_view_ != nullptr) {
        change_graph_view_->Clear();
    }
    if (output_tabs_ != nullptr) {
        output_tabs_->setTabToolTip(3, QString());
    }
}

void FfmpegOutputWidget::FocusChangeGraphTab() {
    if (change_graph_view_ == nullptr || !change_graph_view_->HasGraph() || output_tabs_ == nullptr) {
        return;
    }
    // 用户正在看错误输出时不抢页面 —— 失败现场比"变了什么"更要紧。
    if (output_tabs_->currentWidget() == error_view_) {
        return;
    }
    output_tabs_->setCurrentWidget(output_tabs_->widget(3));
}

void FfmpegOutputWidget::AppendLog(const QString& text, bool is_error) {
    // ffmpeg 把 banner、正常进度、报错**全都**写在 stderr 上（本机 7.0.2 验证），
    // 按通道分页会让「运行日志」只剩一条命令抬头。所以主视图按到达顺序合并两路，
    // 来自 stderr 的行加 [err] 前缀保留原始通道；纯 stderr 另有分页便于单独排障。
    const QString merged = is_error ? QStringLiteral("[err] %1").arg(text) : text;
    pending_log_lines_.append(merged);
    if (is_error) {
        pending_error_lines_.append(text);
    }
    // 明文缓存按到达即时累积：复制 / 保存读的是它，不能等攒批定时器。
    if (last_log_text_.size() < kMaxLogChars) {
        last_log_text_ += merged + QLatin1Char('\n');
    }
    if (pending_log_lines_.size() >= kMaxPendingLines) {
        FlushLog(); // 主线程被长操作顶住时的兜底：先落地，别把内存吃光
        return;
    }
    if (log_flush_timer_ != nullptr && !log_flush_timer_->isActive()) {
        log_flush_timer_->start();
    }
}

void FfmpegOutputWidget::AppendNotice(const QString& text) {
    // 先落地压着的行，保证「结论」排在这轮输出之后
    FlushLog();
    // VideoEye 自己的结论**不过滤**：用户可能挂着一个上一轮留下的过滤词，
    // 恰好把 ffmpeg 的报错筛没了 —— 失败之后最该看见的正是这段归纳，
    // 让它跟着一起被筛掉等于在最需要的时候给一片空白。
    AppendLines(log_view_, {text});
    AppendLines(error_view_, {text});
    log_history_.append(text);
    error_history_.append(text);
    if (!ActiveFilter().isEmpty()) {
        // 过滤态下计数要跟着涨：这两行确实进视图了
        log_matched_ += 1;
        error_matched_ += 1;
        UpdateFilterCount();
    }
    if (last_log_text_.size() < kMaxLogChars) {
        last_log_text_ += text + QLatin1Char('\n');
    }
}

void FfmpegOutputWidget::FlushLog() {
    if (log_flush_timer_ != nullptr) {
        log_flush_timer_->stop();
    }
    if (pending_log_lines_.isEmpty() && pending_error_lines_.isEmpty()) {
        return;
    }

    const QString needle = ActiveFilter();
    const bool filtering = !needle.isEmpty();

    // 历史照单全收（过滤只改显示），视图只收命中的行。
    log_history_.append(pending_log_lines_);
    error_history_.append(pending_error_lines_);

    const QStringList visible_log = SelectLines(pending_log_lines_, needle);
    const QStringList visible_error = SelectLines(pending_error_lines_, needle);
    pending_log_lines_.clear();
    pending_error_lines_.clear();

    AppendLines(log_view_, visible_log);
    AppendLines(error_view_, visible_error);
    if (filtering) {
        log_matched_ += static_cast<int>(visible_log.size());
        error_matched_ += static_cast<int>(visible_error.size());
    }

    // 超出上限：视图自己会丢掉最旧的行，历史必须跟着丢，否则"过滤"能翻出
    // 视图里已经没有的内容。裁剪后行号整体前移，重建一次最省事。
    const bool trimmed = log_history_.size() > kMaxHistoryLines || error_history_.size() > kMaxHistoryLines;
    if (trimmed) {
        if (log_history_.size() > kMaxHistoryLines) {
            log_history_.erase(log_history_.begin(), log_history_.begin() + (log_history_.size() - kMaxHistoryLines));
        }
        if (error_history_.size() > kMaxHistoryLines) {
            error_history_.erase(error_history_.begin(),
                                 error_history_.begin() + (error_history_.size() - kMaxHistoryLines));
        }
        ApplyLogFilter(); // 内部会重建视图、重算计数
        return;
    }

    UpdateFilterCount();
    if (filtering) {
        // 只把新写入的行补上高亮（全量重扫两万行不值当）
        RefreshHighlights();
    }
}

QString FfmpegOutputWidget::ActiveFilter() const {
    return filter_edit_ != nullptr ? filter_edit_->text().trimmed() : QString();
}

void FfmpegOutputWidget::SetLogFilter(const QString& text) {
    if (filter_edit_ == nullptr) {
        return;
    }
    if (filter_edit_->text() == text) {
        ApplyLogFilter();
        return;
    }
    // 外部设置（含测试）立即生效，不等 150ms 防抖
    const QSignalBlocker blocker(filter_edit_);
    filter_edit_->setText(text);
    if (filter_debounce_ != nullptr) {
        filter_debounce_->stop();
    }
    ApplyLogFilter();
}

void FfmpegOutputWidget::ApplyLogFilter() {
    if (filter_edit_ == nullptr) {
        return;
    }
    const QString needle = ActiveFilter();
    const QStringList shown_log = SelectLines(log_history_, needle);
    const QStringList shown_error = SelectLines(error_history_, needle);
    log_matched_ = needle.isEmpty() ? 0 : static_cast<int>(shown_log.size());
    error_matched_ = needle.isEmpty() ? 0 : static_cast<int>(shown_error.size());

    ReplaceViewText(log_view_, shown_log);
    ReplaceViewText(error_view_, shown_error);
    RefreshHighlights();
    UpdateFilterCount();
}

void FfmpegOutputWidget::UpdateFilterCount() {
    if (filter_count_label_ == nullptr || filter_edit_ == nullptr) {
        return;
    }
    const QString needle = ActiveFilter();
    if (needle.isEmpty()) {
        filter_count_label_->clear();
        filter_count_label_->setVisible(false);
        return;
    }
    const bool on_log_tab = output_tabs_ == nullptr || output_tabs_->currentWidget() == log_view_;
    const int matched = on_log_tab ? log_matched_ : error_matched_;
    const int total = on_log_tab ? static_cast<int>(log_history_.size()) : static_cast<int>(error_history_.size());
    // 「匹配 0 / 340 行」比空屏有用：至少证明日志本身有内容，只是这词不在里面
    filter_count_label_->setText(tr("匹配 %1 / 共 %2 行").arg(matched).arg(total));
    filter_count_label_->setVisible(true);
}

void FfmpegOutputWidget::RefreshHighlights() {
    const QString needle = ActiveFilter();
    log_view_->setExtraSelections(BuildMatchSelections(log_view_, needle));
    error_view_->setExtraSelections(BuildMatchSelections(error_view_, needle));
}

bool FfmpegOutputWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == filter_edit_ && event->type() == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() == Qt::Key_Escape && !filter_edit_->text().isEmpty()) {
            SetLogFilter(QString());
            return true; // 别让 Esc 冒泡去关窗口
        }
    }
    return QGroupBox::eventFilter(watched, event);
}

void FfmpegOutputWidget::ShowProgress(const ffmpeg::FfmpegProgressSnapshot& snapshot) {
    progress_label_->setText(tr("进度: %1").arg(ffmpeg::FormatProgressSummary(snapshot)));

    // 悬停看完整字段：状态行就那么宽，帧率与已写入大小挤进去会把状态/退出码顶掉。
    const QString detail = ffmpeg::FormatProgressDetail(snapshot);
    progress_label_->setToolTip(detail.isEmpty() ? ProgressHint()
                                                 : QStringLiteral("%1\n\n%2").arg(detail, ProgressHint()));

    if (run_elapsed_.isValid()) {
        elapsed_label_->setText(tr("耗时: %1").arg(FormatDuration(run_elapsed_.elapsed())));
    }
}

QString FfmpegOutputWidget::ProgressHint() {
    return tr("百分比 = 已处理时长 / 输入总时长。「约剩」由 ffmpeg 报的处理速度推算，"
              "命令里写了 -ss / -t 时两者都会偏离本次要产出的时长。");
}

void FfmpegOutputWidget::ShowRunResult(const ffmpeg::FfmpegRunResult& result) {
    FlushLog(); // 最后几行可能还压在缓冲里，状态行要跟它们一起出现
    // 进度已经定住了，别让悬停还报着运行中那一刻的帧率与剩余时间
    progress_label_->setToolTip(ProgressHint());
    exit_code_label_->setText(tr("退出码: %1").arg(result.exit_code));
    elapsed_label_->setText(tr("耗时: %1").arg(FormatDuration(result.elapsed_ms)));

    // 「输出文件已存在，拒绝了覆盖」是独立结局：退出码可能是 0，也不能算"用户停止且
    // 收尾干净" —— ffmpeg 根本没开始写，原文件原样未动。报"完成"或"已停止（正常收尾）"
    // 都会让用户以为产出已经更新。
    if (result.overwrite_refused) {
        SetStatusText(tr("状态: 未写入（输出文件已存在）"), QLatin1String(theme::color::kDanger));
        progress_label_->setText(tr("进度: —"));
        AppendLog(tr("ffmpeg 停在「输出文件已存在，是否覆盖？」这一问上，没有改写任何数据 —— 原文件保持原样。"
                     "要覆盖就加上 -y，或换一个输出文件名。"),
                  true);
        emit StatusMessage(tr("ffmpeg 没有写入：输出文件已存在（原文件未改动）"));
        FlushLog(); // 上面那行还在缓冲里，不落地就判断不出「错误页非空」
        // 判断"有没有内容"看历史而不是视图：视图可能正被过滤词清空着
        if (!error_history_.isEmpty()) {
            output_tabs_->setCurrentWidget(error_view_);
        }
        return;
    }

    QString text;
    QString color;
    switch (result.status) {
    case ffmpeg::FfmpegRunStatus::Finished:
        text = tr("状态: 完成");
        color = QLatin1String(theme::color::kSuccess);
        // 跑完了就别留着最后一个 99.x% 让人以为还差一点
        progress_label_->setText(tr("进度: 完成"));
        break;
    case ffmpeg::FfmpegRunStatus::Failed:
        text = tr("状态: 失败（退出码 %1）").arg(result.exit_code);
        color = QLatin1String(theme::color::kDanger);
        break;
    case ffmpeg::FfmpegRunStatus::Stopped:
        // "停止"有两种：ffmpeg 自己收了尾（输出完整）和被强杀（输出多半坏了）。
        // 一律显示"已停止"会让用户拿一个坏掉的 mp4 当结果。
        text = result.stopped_cleanly() ? tr("状态: 已停止（正常收尾）") : tr("状态: 已停止（输出可能不可用）");
        color = result.stopped_cleanly() ? QLatin1String(theme::color::kWarning) : QLatin1String(theme::color::kDanger);
        break;
    case ffmpeg::FfmpegRunStatus::StartError:
        text = tr("状态: 无法启动");
        color = QLatin1String(theme::color::kDanger);
        progress_label_->setText(tr("进度: —"));
        break;
    default:
        text = tr("状态: 未运行");
        color = QLatin1String(theme::color::kTextSecondary);
        break;
    }
    SetStatusText(text, color);

    if (!result.error_message.isEmpty()) {
        AppendLog(QStringLiteral("错误: %1").arg(result.error_message), true);
    }
    if (result.status == ffmpeg::FfmpegRunStatus::Failed) {
        FlushLog(); // 诊断读的就是刚落地的那份 stderr，不能还压在缓冲里
        // ffmpeg 的错误原因几乎都在 stderr 上，但那几十行里真正有用的只有一两行。
        // 先把结论翻成人话贴在错误页末尾，再切过去 —— 用户落地就在答案上。
        // 注意：诊断读完整历史，不能读错误页视图 —— 视图可能正被过滤词筛过，
        // 拿筛过的内容去诊断等于让用户的过滤词决定看到什么结论。
        const std::vector<ffmpeg::FfmpegDiagnosis> diagnoses =
            ffmpeg::DiagnoseFailure(error_history_.join(QLatin1Char('\n')));
        if (!diagnoses.empty()) {
            AppendNotice(tr("── 诊断（VideoEye 归纳，不是 ffmpeg 输出）──"));
            for (const auto& diagnosis : diagnoses) {
                AppendNotice(QStringLiteral("● %1").arg(diagnosis.title));
                if (!diagnosis.evidence.isEmpty()) {
                    AppendNotice(tr("    ffmpeg 原话: %1").arg(diagnosis.evidence));
                }
                AppendNotice(tr("    怎么办: %1").arg(diagnosis.suggestion));
            }
            FlushLog();
        }
        if (!error_history_.isEmpty()) {
            output_tabs_->setCurrentWidget(error_view_);
        }
        if (!diagnoses.empty()) {
            emit StatusMessage(tr("ffmpeg 失败（退出码 %1）：%2").arg(result.exit_code).arg(diagnoses.front().title));
        } else {
            emit StatusMessage(tr("ffmpeg 失败（退出码 %1）：请看「错误输出」").arg(result.exit_code));
        }
    } else if (result.status == ffmpeg::FfmpegRunStatus::Finished) {
        emit StatusMessage(tr("ffmpeg 完成，耗时 %1").arg(FormatDuration(result.elapsed_ms)));
    } else if (result.status == ffmpeg::FfmpegRunStatus::Stopped) {
        if (result.stopped_cleanly()) {
            emit StatusMessage(tr("ffmpeg 已停止（正常收尾，输出文件完整）"));
        } else {
            AppendLog(tr("警告: ffmpeg 是被强制终止的 —— mp4/mov 这类需要先写完整索引的容器"
                         "可能缺 moov box 而无法播放，建议重新跑一次。"),
                      true);
            emit StatusMessage(tr("ffmpeg 已停止，但输出可能不可用"));
        }
    }
    // 终态是"一次事件"，没有理由再攒批：最后这几行（错误说明、强杀警告）
    // 必须马上看得见，否则用户读到的是上一个瞬间的日志。
    FlushLog();
    if (!ActiveFilter().isEmpty()) {
        RefreshHighlights(); // 收尾补写的那几行也要标上
    }
}

void FfmpegOutputWidget::OnCopyLog() {
    if (last_log_text_.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("还没有日志可复制。"));
        return;
    }
    QGuiApplication::clipboard()->setText(last_log_text_);
    emit StatusMessage(tr("日志已复制到剪贴板"));
}

void FfmpegOutputWidget::OnSaveLog() {
    if (last_log_text_.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("还没有日志可保存。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("保存日志"), QStringLiteral("ffmpeg-log.txt"),
                                                      tr("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("保存失败"), tr("无法写入文件: %1").arg(path));
        return;
    }
    QTextStream stream(&file);
    stream << last_log_text_;
    file.close();
    emit StatusMessage(tr("日志已保存: %1").arg(path));
}

} // namespace ui
} // namespace videoeye