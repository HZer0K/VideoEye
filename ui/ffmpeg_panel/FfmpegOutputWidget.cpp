#include "ui/ffmpeg_panel/FfmpegOutputWidget.h"

#include <QClipboard>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollBar>
#include <QSplitter>
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

} // namespace

FfmpegOutputWidget::FfmpegOutputWidget(QWidget* parent) : QGroupBox(tr("输出与解释"), parent) {
    log_flush_timer_ = new QTimer(this);
    log_flush_timer_->setSingleShot(true);
    log_flush_timer_->setInterval(kLogFlushIntervalMs);
    connect(log_flush_timer_, &QTimer::timeout, this, &FfmpegOutputWidget::FlushLog);
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

    layout->addWidget(output_tabs_, 1);
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
    log_view_->clear();
    error_view_->clear();
    last_log_text_.clear();
    run_elapsed_.invalidate();
    SetStatusText(tr("状态: 未运行"), QString());
    progress_label_->setText(tr("进度: —"));
    progress_label_->setToolTip(ProgressHint());
    exit_code_label_->setText(tr("退出码: —"));
    elapsed_label_->setText(tr("耗时: —"));
}

void FfmpegOutputWidget::PrepareForRun() {
    pending_log_lines_.clear();
    pending_error_lines_.clear();
    if (log_flush_timer_ != nullptr) {
        log_flush_timer_->stop();
    }
    log_view_->clear();
    error_view_->clear();
    last_log_text_.clear();
    run_elapsed_.start();
    SetStatusText(tr("状态: 运行中"), QLatin1String(theme::color::kAccent));
    progress_label_->setText(tr("进度: —"));
    progress_label_->setToolTip(ProgressHint());
    exit_code_label_->setText(tr("退出码: —"));
    elapsed_label_->setText(tr("耗时: —"));
    output_tabs_->setCurrentWidget(log_view_);
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
    // 诊断最多十来行，不必走 AppendLog 里那个 2000 行兜底；攒批定时器还是要用，
    // 否则一次失败要连刷十几次滚动条。
    pending_log_lines_.append(text);
    pending_error_lines_.append(text);
    if (last_log_text_.size() < kMaxLogChars) {
        last_log_text_ += text + QLatin1Char('\n');
    }
    if (log_flush_timer_ != nullptr && !log_flush_timer_->isActive()) {
        log_flush_timer_->start();
    }
}

void FfmpegOutputWidget::FlushLog() {
    if (log_flush_timer_ != nullptr) {
        log_flush_timer_->stop();
    }
    AppendLines(log_view_, pending_log_lines_);
    pending_log_lines_.clear();
    AppendLines(error_view_, pending_error_lines_);
    pending_error_lines_.clear();
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
        if (!error_view_->toPlainText().trimmed().isEmpty()) {
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
        const std::vector<ffmpeg::FfmpegDiagnosis> diagnoses = ffmpeg::DiagnoseFailure(error_view_->toPlainText());
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
        if (!error_view_->toPlainText().trimmed().isEmpty()) {
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