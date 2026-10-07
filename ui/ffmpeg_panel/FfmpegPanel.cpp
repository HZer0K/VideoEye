#include "ui/ffmpeg_panel/FfmpegPanel.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QUrl>
#include <QVBoxLayout>

#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegToolLocator.h"
#include "ui/ffmpeg_panel/FfmpegDictionaryWidget.h"
#include "ui/ffmpeg_panel/FfmpegOutputWidget.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {

FfmpegPanel::FfmpegPanel(QWidget* parent)
    : QWidget(parent) {
    BuildUi();

    runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(runner_, &ffmpeg::FfmpegProcessRunner::OutputLine,
            this, &FfmpegPanel::OnRunOutput);
    connect(runner_, &ffmpeg::FfmpegProcessRunner::Finished,
            this, &FfmpegPanel::OnRunFinished);

    probe_runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(probe_runner_, &ffmpeg::FfmpegProcessRunner::OutputLine,
            this, &FfmpegPanel::OnProbeOutput);
    connect(probe_runner_, &ffmpeg::FfmpegProcessRunner::Finished,
            this, &FfmpegPanel::OnProbeFinished);

    const QString saved = settings_.value(QStringLiteral("ffmpeg/toolPath")).toString();
    if (!saved.isEmpty()) {
        tool_path_edit_->setText(saved);
    }

    RefreshToolInfo();
    RefreshExplanation();
    StartProbes();
}

FfmpegPanel::~FfmpegPanel() = default;

// ===================== UI =====================

void FfmpegPanel::BuildUi() {
    QHBoxLayout* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    root->addWidget(splitter);

    QWidget* left = new QWidget(splitter);
    QVBoxLayout* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(6);
    left_layout->addWidget(BuildCommandArea(), 0);
    output_widget_ = new FfmpegOutputWidget(left);
    // 组件不直接写状态栏，「已复制 / 已完成」这类文案统一经页面转发出去
    connect(output_widget_, &FfmpegOutputWidget::StatusMessage,
            this, &FfmpegPanel::StatusMessage);
    left_layout->addWidget(output_widget_, 1);

    // 指令字典区（右侧）：搜索 / 分类 / 详情 / 插入全在组件内。构造顺序要求它先建好，
    // 下面 InsertRequested 才能接到命令输入框上。
    dict_widget_ = new FfmpegDictionaryWidget(splitter);
    // 「插入到命令」的目标（命令编辑框）属于页面，组件只发片段，这里负责落地。
    connect(dict_widget_, &FfmpegDictionaryWidget::InsertRequested,
            this, [this](const QString& snippet) {
        command_edit_->insert(snippet);
        command_edit_->setFocus();
    });

    dict_widget_->setMinimumWidth(280);
    splitter->addWidget(left);
    splitter->addWidget(dict_widget_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({760, 320});
}

QWidget* FfmpegPanel::BuildCommandArea() {
    QGroupBox* box = new QGroupBox(tr("命令"), this);
    QVBoxLayout* layout = new QVBoxLayout(box);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);

    // ---- 程序路径 ----
    QHBoxLayout* tool_row = new QHBoxLayout();
    tool_row->setSpacing(6);
    QLabel* tool_label = new QLabel(tr("ffmpeg 程序:"), box);
    tool_path_edit_ = new QLineEdit(box);
    tool_path_edit_->setPlaceholderText(tr("留空则自动查找（随包分发 → 构建期路径 → PATH）"));
    tool_path_edit_->setToolTip(tr("Linux/macOS 上如果 ffmpeg 不在 PATH 里，在这里填绝对路径"));
    connect(tool_path_edit_, &QLineEdit::editingFinished, this, &FfmpegPanel::OnToolPathEdited);
    browse_button_ = new QPushButton(tr("浏览…"), box);
    connect(browse_button_, &QPushButton::clicked, this, &FfmpegPanel::OnBrowseTool);
    reset_button_ = new QPushButton(tr("自动"), box);
    reset_button_->setToolTip(tr("清空手动路径，回到自动查找"));
    connect(reset_button_, &QPushButton::clicked, this, &FfmpegPanel::OnResetTool);
    tool_row->addWidget(tool_label);
    tool_row->addWidget(tool_path_edit_, 1);
    tool_row->addWidget(browse_button_);
    tool_row->addWidget(reset_button_);
    layout->addLayout(tool_row);

    tool_status_label_ = new QLabel(tr("(正在探测 ffmpeg…)"), box);
    tool_status_label_->setWordWrap(true);
    tool_status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tool_status_label_->setStyleSheet(
        QStringLiteral("color: %1;").arg(QLatin1String(theme::color::kTextSecondary)));
    layout->addWidget(tool_status_label_);

    // ---- 找不到 ffmpeg 时的补救入口（平时隐藏）----
    tool_missing_bar_ = new QWidget(box);
    QHBoxLayout* missing_row = new QHBoxLayout(tool_missing_bar_);
    missing_row->setContentsMargins(0, 0, 0, 0);
    missing_row->setSpacing(6);
    guide_button_ = new QPushButton(tr("安装指引…"), tool_missing_bar_);
    guide_button_->setToolTip(tr("按当前平台给出安装命令、下载页与手动指定步骤"));
    connect(guide_button_, &QPushButton::clicked, this, &FfmpegPanel::OnShowInstallGuide);
    download_button_ = new QPushButton(tr("打开下载页"), tool_missing_bar_);
    connect(download_button_, &QPushButton::clicked, this, &FfmpegPanel::OnOpenDownloadPage);
    redetect_button_ = new QPushButton(tr("重新检测"), tool_missing_bar_);
    redetect_button_->setToolTip(tr("装完之后点一下，会重新走一遍自动查找"));
    connect(redetect_button_, &QPushButton::clicked, this, &FfmpegPanel::OnRedetectTool);
    missing_row->addWidget(guide_button_);
    missing_row->addWidget(download_button_);
    missing_row->addWidget(redetect_button_);
    missing_row->addStretch(1);
    tool_missing_bar_->setVisible(false);
    layout->addWidget(tool_missing_bar_);

    // ---- 命令输入 ----
    command_edit_ = new QLineEdit(box);
    command_edit_->setFont(theme::font::monoFont(9));
    command_edit_->setPlaceholderText(
        tr("ffmpeg -i \"输入.mp4\" -c:v libx264 -crf 23 输出.mp4   （回车运行）"));
    command_edit_->setToolTip(tr("只支持单独一条 ffmpeg 命令：不支持 | 、&& 、> 等 shell 操作符；"
                                 "含空格的路径请用引号包起来"));
    connect(command_edit_, &QLineEdit::returnPressed, this, &FfmpegPanel::OnRun);
    connect(command_edit_, &QLineEdit::textChanged, this, &FfmpegPanel::OnCommandTextChanged);
    layout->addWidget(command_edit_);

    QHBoxLayout* button_row = new QHBoxLayout();
    button_row->setSpacing(6);
    insert_input_button_ = new QPushButton(tr("插入当前媒体路径"), box);
    insert_input_button_->setToolTip(tr("把主窗口当前打开的媒体路径（带引号）插到光标处"));
    connect(insert_input_button_, &QPushButton::clicked, this, &FfmpegPanel::OnInsertInputPath);
    insert_output_button_ = new QPushButton(tr("插入输出路径…"), box);
    connect(insert_output_button_, &QPushButton::clicked, this, &FfmpegPanel::OnInsertOutputPath);
    run_button_ = new QPushButton(tr("运行"), box);
    run_button_->setObjectName(QStringLiteral("PrimaryButton"));
    connect(run_button_, &QPushButton::clicked, this, &FfmpegPanel::OnRun);
    stop_button_ = new QPushButton(tr("停止"), box);
    stop_button_->setEnabled(false);
    connect(stop_button_, &QPushButton::clicked, this, &FfmpegPanel::OnStop);
    QPushButton* clear_button = new QPushButton(tr("清空"), box);
    connect(clear_button, &QPushButton::clicked, this, &FfmpegPanel::OnClearCommand);
    button_row->addWidget(insert_input_button_);
    button_row->addWidget(insert_output_button_);
    button_row->addStretch(1);
    button_row->addWidget(clear_button);
    button_row->addWidget(stop_button_);
    button_row->addWidget(run_button_);
    layout->addLayout(button_row);

    return box;
}

// ===================== 程序路径 =====================

QString FfmpegPanel::CurrentToolPath() const {
    return tool_path_edit_ ? tool_path_edit_->text().trimmed() : QString();
}

void FfmpegPanel::RefreshToolInfo() {
    tool_info_ = ffmpeg::FfmpegToolLocator::Resolve(CurrentToolPath());
    const bool usable = !tool_info_.path.isEmpty() && tool_info_.exists;

    QString text;
    QString color = QLatin1String(theme::color::kTextSecondary);
    if (tool_info_.path.isEmpty()) {
        text = tr("未找到 ffmpeg 可执行程序 —— 本页的「运行」需要它，"
                  "右侧指令字典与命令解释不依赖它，照常可用。"
                  "点「安装指引…」看按平台的装法，或用「浏览…」直接指定已装好的程序。");
        color = QLatin1String(theme::color::kDanger);
    } else if (!tool_info_.exists) {
        text = tr("程序路径: %1（来源: %2）\n该文件不存在或不可执行，请重新指定。")
                   .arg(tool_info_.path, tool_info_.origin);
        color = QLatin1String(theme::color::kDanger);
    } else {
        const QString version = tool_version_.isEmpty() ? tr("(版本未知)") : tool_version_;
        text = tr("程序: %1\n来源: %2\n版本: %3")
                   .arg(tool_info_.path, tool_info_.origin, version);
        if (ffmpeg::FfmpegCapabilityCache::Instance().encoders_known()) {
            text += tr("\n已加载 %1 个编码器、%2 个滤镜")
                        .arg(ffmpeg::FfmpegCapabilityCache::Instance().encoder_count())
                        .arg(ffmpeg::FfmpegCapabilityCache::Instance().filter_count());
        }
        color = QLatin1String(theme::color::kSuccess);
    }
    tool_status_label_->setText(text);
    tool_status_label_->setStyleSheet(QStringLiteral("color: %1;").arg(color));

    // 缺程序时把补救入口露出来，并明确禁用运行 —— 与其点了报个莫名其妙的错，
    // 不如一开始就说明"为什么不能点"。
    if (tool_missing_bar_) {
        tool_missing_bar_->setVisible(!usable);
    }
    const QString run_tip = usable ? QString()
                                   : tr("先安装 ffmpeg 或在上方指定可执行程序路径，才能运行命令");
    run_button_->setEnabled(usable && !runner_->IsRunning());
    run_button_->setToolTip(run_tip);
    command_edit_->setToolTip(usable
        ? tr("只支持单独一条 ffmpeg 命令：不支持 | 、&& 、> 等 shell 操作符；"
             "含空格的路径请用引号包起来")
        : run_tip);
}

void FfmpegPanel::OnShowInstallGuide() {
    const ffmpeg::FfmpegInstallGuide guide = ffmpeg::FfmpegToolLocator::InstallGuide();

    QString html = QStringLiteral("<h3 style='margin:2px'>%1</h3>")
                       .arg(tr("安装 ffmpeg（%1）").arg(guide.platform));
    html += QStringLiteral("<p>%1</p>").arg(guide.headline.toHtmlEscaped());

    if (!guide.package_commands.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("包管理器（任选一条）"));
        for (const QString& cmd : guide.package_commands) {
            html += QStringLiteral("<li><code>%1</code></li>").arg(cmd.toHtmlEscaped());
        }
        html += QStringLiteral("</ul>");
    }
    if (!guide.manual_steps.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p><ol>").arg(tr("手动安装"));
        for (const QString& step : guide.manual_steps) {
            html += QStringLiteral("<li>%1</li>").arg(step.toHtmlEscaped());
        }
        html += QStringLiteral("</ol>");
    }
    if (!guide.note.isEmpty()) {
        html += QStringLiteral("<p style='color:%1'>%2</p>")
                    .arg(QLatin1String(theme::color::kTextSecondary), guide.note.toHtmlEscaped());
    }
    html += QStringLiteral("<p style='color:%1'>%2</p>")
                .arg(QLatin1String(theme::color::kWarning),
                     tr("注意：只有这一页需要 ffmpeg 可执行程序。"
                        "媒体信息、流分析、QC 报告、播放器等功能用的是 VideoEye 内置链接的 FFmpeg 库，"
                        "不装 ffmpeg 也完全能用。").toHtmlEscaped());

    QDialog dialog(this);
    dialog.setWindowTitle(tr("安装 ffmpeg"));
    dialog.resize(620, 480);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);

    QTextBrowser* browser = new QTextBrowser(&dialog);
    browser->setOpenExternalLinks(true);
    browser->setHtml(html);
    layout->addWidget(browser, 1);

    QHBoxLayout* button_row = new QHBoxLayout();
    QPushButton* copy_button = new QPushButton(tr("复制指引"), &dialog);
    QPushButton* open_button = new QPushButton(tr("打开下载页"), &dialog);
    QPushButton* pick_button = new QPushButton(tr("选择已安装的程序…"), &dialog);
    QPushButton* close_button = new QPushButton(tr("关闭"), &dialog);
    connect(copy_button, &QPushButton::clicked, this, [this, &dialog]() {
        QGuiApplication::clipboard()->setText(BuildInstallGuideText());
        emit StatusMessage(tr("安装指引已复制到剪贴板"));
    });
    connect(open_button, &QPushButton::clicked, this, &FfmpegPanel::OnOpenDownloadPage);
    connect(pick_button, &QPushButton::clicked, this, [this, &dialog]() {
        dialog.accept();
        OnBrowseTool();
    });
    connect(close_button, &QPushButton::clicked, &dialog, &QDialog::accept);
    button_row->addWidget(copy_button);
    button_row->addWidget(open_button);
    button_row->addWidget(pick_button);
    button_row->addStretch(1);
    button_row->addWidget(close_button);
    layout->addLayout(button_row);

    dialog.exec();
}

QString FfmpegPanel::BuildInstallGuideText() const {
    const ffmpeg::FfmpegInstallGuide guide = ffmpeg::FfmpegToolLocator::InstallGuide();
    QString out = tr("安装 ffmpeg（%1）\n").arg(guide.platform);
    out += guide.headline + QLatin1Char('\n');
    if (!guide.package_commands.isEmpty()) {
        out += tr("\n[包管理器]\n");
        for (const QString& cmd : guide.package_commands) {
            out += QStringLiteral("  %1\n").arg(cmd);
        }
    }
    if (!guide.manual_steps.isEmpty()) {
        out += tr("\n[手动安装]\n");
        for (int i = 0; i < guide.manual_steps.size(); ++i) {
            out += QStringLiteral("  %1. %2\n").arg(i + 1).arg(guide.manual_steps.at(i));
        }
    }
    if (!guide.note.isEmpty()) {
        out += QStringLiteral("\n%1\n").arg(guide.note);
    }
    out += tr("\n下载页: %1\n").arg(guide.download_url);
    return out;
}

void FfmpegPanel::OnOpenDownloadPage() {
    const QString url = ffmpeg::FfmpegToolLocator::InstallGuide().download_url;
    if (url.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl(url));
    emit StatusMessage(tr("已打开下载页: %1").arg(url));
}

void FfmpegPanel::OnRedetectTool() {
    tool_version_.clear();
    ffmpeg::FfmpegCapabilityCache::Instance().Clear();
    RefreshToolInfo();
    StartProbes();
    emit StatusMessage(tool_info_.exists ? tr("已找到 ffmpeg: %1").arg(tool_info_.path)
                                        : tr("仍未找到 ffmpeg，可按「安装指引…」装好后再点一次"));
}

void FfmpegPanel::OnBrowseTool() {
    const QString start = tool_info_.path.isEmpty() ? QDir::homePath() : tool_info_.path;
#ifdef Q_OS_WIN
    const QString filter = tr("可执行程序 (*.exe);;所有文件 (*)");
#else
    const QString filter = tr("所有文件 (*)");
#endif
    const QString path = QFileDialog::getOpenFileName(this, tr("选择 ffmpeg 可执行程序"), start, filter);
    if (path.isEmpty()) {
        return;
    }
    tool_path_edit_->setText(path);
    settings_.setValue(QStringLiteral("ffmpeg/toolPath"), path);
    tool_version_.clear();
    ffmpeg::FfmpegCapabilityCache::Instance().Clear();
    RefreshToolInfo();
    StartProbes();
}

void FfmpegPanel::OnResetTool() {
    tool_path_edit_->clear();
    settings_.remove(QStringLiteral("ffmpeg/toolPath"));
    tool_version_.clear();
    ffmpeg::FfmpegCapabilityCache::Instance().Clear();
    RefreshToolInfo();
    StartProbes();
}

void FfmpegPanel::OnToolPathEdited() {
    const QString path = CurrentToolPath();
    if (path.isEmpty()) {
        settings_.remove(QStringLiteral("ffmpeg/toolPath"));
    } else {
        settings_.setValue(QStringLiteral("ffmpeg/toolPath"), path);
    }
    tool_version_.clear();
    ffmpeg::FfmpegCapabilityCache::Instance().Clear();
    RefreshToolInfo();
    StartProbes();
}

void FfmpegPanel::StartProbes() {
    const QString program = tool_info_.path;
    if (program.isEmpty() || !tool_info_.exists) {
        CancelProbes();
        return;
    }
    if (probe_runner_->IsRunning()) {
        if (probe_program_ == program && !probe_restart_pending_) {
            return;   // 同一个程序的探测还在跑，让它跑完
        }
        // 程序换了：这一轮剩下的探测如果继续用新路径去跑，就会拼出
        // "旧程序的版本号 + 新程序的编码器列表"这种混杂结果。作废这一轮，
        // 让回调全部失效，等旧进程退出后再用新路径重新起一轮。
        probe_restart_pending_ = true;
        probe_program_ = program;
        ForgetCurrentProbe();
        // 探测只是打印清单，没有输出文件要收尾 —— 直接终止，不用给优雅退出的时间
        probe_runner_->Stop(0, 0);
        return;
    }
    probe_program_ = program;
    ++probe_generation_;
    probe_queue_ = {QStringLiteral("-version"), QStringLiteral("-encoders"),
                    QStringLiteral("-filters"), QStringLiteral("-formats")};
    RunNextProbe();
}

void FfmpegPanel::CancelProbes() {
    probe_queue_.clear();
    probe_restart_pending_ = false;
    probe_current_.clear();
    probe_buffer_.clear();
    ForgetCurrentProbe();
    if (probe_runner_ != nullptr && probe_runner_->IsRunning()) {
        // 探测只是打印清单，没有输出文件要收尾 —— 直接终止，不用给优雅退出的时间
        probe_runner_->Stop(0, 0);
    }
}

void FfmpegPanel::ForgetCurrentProbe() {
    // 让在飞的回调失效：Stop() 之后进程还会补一个 finished()，那一路上
    // 的 buffer / probe_current_ 都属于被作废的那一轮。
    ++probe_generation_;
}

void FfmpegPanel::RunNextProbe() {
    if (probe_queue_.isEmpty()) {
        RefreshToolInfo();
        RefreshExplanation();
        emit StatusMessage(tr("ffmpeg 能力清单已更新"));
        return;
    }
    probe_current_ = probe_queue_.takeFirst();
    probe_buffer_.clear();
    // 整轮绑定同一个程序：中途即使界面上的路径变了，这一趟也照旧用 probe_program_。
    probe_launch_generation_ = probe_generation_;
    probe_runner_->Start(probe_program_, {QStringLiteral("-hide_banner"), probe_current_});
}

void FfmpegPanel::OnProbeOutput(const QString& text, bool /*is_error*/) {
    if (probe_launch_generation_ != probe_generation_) {
        return;   // 上一轮的残余输出，丢弃，别混进 buffer
    }
    probe_buffer_ += text + QLatin1Char('\n');
}

void FfmpegPanel::OnProbeFinished(const ffmpeg::FfmpegRunResult& result) {
    if (probe_launch_generation_ != probe_generation_) {
        // 被"换程序"作废的一趟。它的结果一个字都不能用。
        probe_buffer_.clear();
        MaybeRestartProbes();
        return;
    }

    if (result.status == ffmpeg::FfmpegRunStatus::Failed ||
        result.status == ffmpeg::FfmpegRunStatus::StartError) {
        // -version 失败就别继续了：后面的能力清单同样拿不到
        probe_queue_.clear();
        RefreshToolInfo();
        if (!result.error_message.isEmpty()) {
            emit StatusMessage(tr("ffmpeg 探测失败: %1").arg(result.error_message));
        }
        MaybeRestartProbes();
        return;
    }

    auto& caps = ffmpeg::FfmpegCapabilityCache::Instance();
    if (probe_current_ == QLatin1String("-version")) {
        tool_version_ = ffmpeg::FfmpegToolLocator::ParseVersionLine(probe_buffer_);
    } else if (probe_current_ == QLatin1String("-encoders")) {
        caps.SetEncoders(ffmpeg::FfmpegCommandCatalog::ParseEncoderNames(probe_buffer_));
    } else if (probe_current_ == QLatin1String("-filters")) {
        caps.SetFilters(ffmpeg::FfmpegCommandCatalog::ParseFilterNames(probe_buffer_));
    } else if (probe_current_ == QLatin1String("-formats")) {
        const auto formats = ffmpeg::FfmpegCommandCatalog::ParseFormatNames(probe_buffer_);
        caps.SetFormats(formats.demuxers, formats.muxers);
    }
    RunNextProbe();
}

void FfmpegPanel::MaybeRestartProbes() {
    if (!probe_restart_pending_) {
        return;
    }
    probe_restart_pending_ = false;
    StartProbes();
}

// ===================== 命令编辑 =====================

void FfmpegPanel::OnInsertInputPath() {
    if (current_media_path_.isEmpty()) {
        QMessageBox::information(this, tr("提示"),
                                 tr("还没有打开媒体文件。可以先在主窗口打开一个，或手动输入路径。"));
        return;
    }
    command_edit_->insert(QStringLiteral("\"%1\"").arg(current_media_path_));
    command_edit_->setFocus();
}

void FfmpegPanel::OnInsertOutputPath() {
    QString start;
    if (!current_media_path_.isEmpty()) {
        const QFileInfo info(current_media_path_);
        start = info.dir().filePath(info.completeBaseName() + QStringLiteral("_out.mp4"));
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("选择输出文件"), start);
    if (path.isEmpty()) {
        return;
    }
    command_edit_->insert(QStringLiteral("\"%1\"").arg(path));
    command_edit_->setFocus();
}

void FfmpegPanel::OnClearCommand() {
    command_edit_->clear();
    output_widget_->ClearOutput();
}

void FfmpegPanel::OnCommandTextChanged() {
    RefreshExplanation();
}

void FfmpegPanel::RefreshExplanation() {
    // 命令解释区归 FfmpegOutputWidget；页面只把当前命令文本递过去。
    output_widget_->RefreshExplanation(command_edit_->text());
}

// ===================== 运行 =====================

void FfmpegPanel::SetRunUiState(bool running) {
    // 没有可执行的 ffmpeg 时，运行按钮始终保持禁用（原因写在 tooltip 里）
    const bool have_tool = !tool_info_.path.isEmpty() && tool_info_.exists;
    run_button_->setEnabled(!running && have_tool);
    stop_button_->setEnabled(running);
    browse_button_->setEnabled(!running);
    insert_input_button_->setEnabled(!running);
    insert_output_button_->setEnabled(!running);
}

void FfmpegPanel::OnRun() {
    if (runner_->IsRunning()) {
        QMessageBox::information(this, tr("提示"), tr("有命令正在运行，请先停止或等待它结束。"));
        return;
    }

    const auto parsed = ffmpeg::ParseCommandLine(command_edit_->text());
    if (parsed.status != ffmpeg::CommandParseStatus::Ok) {
        QMessageBox::warning(this, tr("无法运行"), parsed.message);
        return;
    }
    if (parsed.arguments.isEmpty()) {
        QMessageBox::warning(this, tr("无法运行"),
                             tr("命令里没有任何参数。至少要给出输入与输出，比如:\n"
                                "ffmpeg -i \"输入.mp4\" \"输出.mp4\""));
        return;
    }

    const QString pipe = ffmpeg::DetectStdoutMediaOutput(parsed.arguments);
    if (!pipe.isEmpty()) {
        QMessageBox::warning(this, tr("无法运行"),
                             tr("参数「%1」会把媒体数据写到标准输出（管道）。\n"
                                "本页的日志区只接收文本，二进制流灌进去只会把界面卡死。\n"
                                "请把结果改成输出到文件（例如 out.mp4）。").arg(pipe));
        return;
    }

    tool_info_ = ffmpeg::FfmpegToolLocator::Resolve(CurrentToolPath());
    if (tool_info_.path.isEmpty() || !tool_info_.exists) {
        QMessageBox warning_box(this);
        warning_box.setWindowTitle(tr("无法运行"));
        warning_box.setIcon(QMessageBox::Warning);
        warning_box.setText(tr("找不到可执行的 ffmpeg 程序%1。")
                                .arg(tool_info_.path.isEmpty()
                                         ? QString()
                                         : tr("（%1）").arg(tool_info_.path)));
        warning_box.setInformativeText(
            tr("本页是把命令交给原生 ffmpeg 程序执行，所以必须先有这个程序。\n"
               "指令字典和命令解释不需要它，可以继续使用。"));
        QPushButton* guide_btn = warning_box.addButton(tr("安装指引…"), QMessageBox::ActionRole);
        warning_box.addButton(tr("选择程序…"), QMessageBox::ActionRole);
        warning_box.addButton(QMessageBox::Cancel);
        warning_box.exec();
        if (warning_box.clickedButton() == guide_btn) {
            OnShowInstallGuide();
        } else if (warning_box.clickedButton() != warning_box.button(QMessageBox::Cancel)) {
            OnBrowseTool();
        }
        return;
    }

    // 命令里写了完整路径 → **用它**。这是原生 ffmpeg 命令行的直觉：
    // 用户把 C:\tools\ffmpeg.exe 写在命令头上，就是要跑那一个程序。
    // 只写裸的 "ffmpeg"（或省略程序名）才用本页设置里的那个。
    QString program = tool_info_.path;
    if (parsed.program_is_path) {
        if (!ffmpeg::FfmpegToolLocator::IsExecutable(parsed.program_token)) {
            QMessageBox::warning(this, tr("无法运行"),
                                 tr("命令里的 ffmpeg 路径「%1」不存在或不可执行。\n\n"
                                    "请改成一个真实存在的 ffmpeg；或者把命令开头写成不带路径的 "
                                    "ffmpeg —— 那样会使用本页设置的「%2」。")
                                     .arg(parsed.program_token, tool_info_.path));
            return;
        }
        program = parsed.program_token;
    }

    current_arguments_ = parsed.arguments;
    output_widget_->PrepareForRun();
    output_widget_->AppendLog(
        QStringLiteral("$ %1")
            .arg(ffmpeg::FfmpegProcessRunner::BuildDisplayCommand(program, parsed.arguments)),
        false);
    if (program != tool_info_.path) {
        output_widget_->AppendLog(
            tr("(命令里写了完整路径: 本次用它执行；本页设置的 %1 已被忽略)")
                .arg(tool_info_.path),
            false);
    }
    SetRunUiState(true);
    const bool started = runner_->Start(program, parsed.arguments);
    if (!started) {
        SetRunUiState(false);
    }
    // 启动失败时不能再说"已启动" —— 否则状态栏会前后矛盾
    emit StatusMessage(started ? tr("已启动 ffmpeg") : tr("ffmpeg 启动失败"));
}

void FfmpegPanel::OnStop() {
    if (!runner_->IsRunning()) {
        return;
    }
    if (ffmpeg::FfmpegProcessRunner::CanQuitViaStdin(current_arguments_)) {
        output_widget_->AppendLog(
            tr("(用户请求停止: 已向 ffmpeg 发送 q，等它正常收尾；超时后会强制终止)"), false);
    } else {
        output_widget_->AppendLog(
            tr("(用户请求停止: 命令里有 -nostdin，ffmpeg 不读标准输入，"
               "无法优雅收尾，只能直接终止 —— 输出文件可能不完整)"),
            true);
    }
    runner_->Stop();
    emit StatusMessage(tr("正在停止 ffmpeg"));
}

void FfmpegPanel::OnRunOutput(const QString& text, bool is_error) {
    output_widget_->AppendLog(text, is_error);
}

void FfmpegPanel::OnRunFinished(const ffmpeg::FfmpegRunResult& result) {
    SetRunUiState(false);
    // 状态行 / 退出码 / 耗时 / 失败切页 / 强杀警告全在输出区组件内；
    // 组件另经 StatusMessage 把终态文案发回页面统一展示。
    output_widget_->ShowRunResult(result);
}

void FfmpegPanel::SetCurrentFile(const QString& path) {
    current_media_path_ = path;
    insert_input_button_->setEnabled(!path.isEmpty() && !runner_->IsRunning());
    insert_input_button_->setToolTip(path.isEmpty()
                                         ? tr("还没有打开媒体文件")
                                         : tr("插入: %1").arg(path));
}

}  // namespace ui
}  // namespace videoeye
