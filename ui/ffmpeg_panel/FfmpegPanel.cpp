#include "ui/ffmpeg_panel/FfmpegPanel.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "core/ffmpeg/FfmpegChangeGraph.h"
#include "core/ffmpeg/FfmpegCommandDraft.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegCommandPreflight.h"
#include "core/ffmpeg/FfmpegCommandTemplate.h"
#include "core/ffmpeg/FfmpegRunOutputs.h"
#include "core/ffmpeg/FfmpegToolLocator.h"
#include "ui/ffmpeg_panel/FfmpegCommandEditorDialog.h"
#include "ui/ffmpeg_panel/FfmpegDictionaryWidget.h"
#include "ui/ffmpeg_panel/FfmpegOutputWidget.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {

FfmpegPanel::FfmpegPanel(QWidget* parent) : QWidget(parent) {
    BuildUi();

    runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(runner_, &ffmpeg::FfmpegProcessRunner::OutputLine, this, &FfmpegPanel::OnRunOutput);
    connect(runner_, &ffmpeg::FfmpegProcessRunner::Finished, this, &FfmpegPanel::OnRunFinished);

    probe_runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(probe_runner_, &ffmpeg::FfmpegProcessRunner::OutputLine, this, &FfmpegPanel::OnProbeOutput);
    connect(probe_runner_, &ffmpeg::FfmpegProcessRunner::Finished, this, &FfmpegPanel::OnProbeFinished);

    // 转换图谱的媒体探测：**独立的 runner**。与上面那轮能力探测共用会把「换程序时
    // 整轮作废」的语义带过来，而这两件事互相不该牵连。
    media_probe_runner_ = new ffmpeg::FfmpegProcessRunner(this);
    connect(media_probe_runner_, &ffmpeg::FfmpegProcessRunner::OutputLine, this, &FfmpegPanel::OnMediaProbeOutput);
    connect(media_probe_runner_, &ffmpeg::FfmpegProcessRunner::Finished, this, &FfmpegPanel::OnMediaProbeFinished);
    // 网络流 / 坏文件的 -i 可能卡很久。10 秒还没读完文件头就当没探到 ——
    // 图谱少一行，比界面挂在那儿强。
    media_probe_timeout_ = new QTimer(this);
    media_probe_timeout_->setSingleShot(true);
    connect(media_probe_timeout_, &QTimer::timeout, this, &FfmpegPanel::OnMediaProbeTimeout);

    const QString saved = settings_.value(QStringLiteral("ffmpeg/toolPath")).toString();
    if (!saved.isEmpty()) {
        tool_path_edit_->setText(saved);
    }
    history_.SetEntries(settings_.value(QStringLiteral("ffmpeg/history")).toStringList());

    RefreshToolInfo();
    RefreshExplanation();
    StartProbes();
}

FfmpegPanel::~FfmpegPanel() {
    // 关页 / 退出时别把探测进程留在后台（它只是读文件头，直接停）
    CancelMediaProbes();
}

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
    connect(output_widget_, &FfmpegOutputWidget::StatusMessage, this, &FfmpegPanel::StatusMessage);
    left_layout->addWidget(output_widget_, 1);

    // 指令字典区（右侧）：搜索 / 分类 / 详情 / 插入全在组件内。构造顺序要求它先建好，
    // 下面 InsertRequested 才能接到命令输入框上。
    dict_widget_ = new FfmpegDictionaryWidget(splitter);
    // 「插入到命令」的目标（命令编辑框）属于页面，组件只发片段，这里负责落地。
    connect(dict_widget_, &FfmpegDictionaryWidget::InsertRequested, this, &FfmpegPanel::InsertSnippet);

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
    tool_status_label_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::color::kTextSecondary)));
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
        tr("ffmpeg -i \"输入.mp4\" -c:v libx264 -crf 23 输出.mp4   （回车运行，↑↓ 翻历史）"));
    command_edit_->setToolTip(tr("只支持单独一条 ffmpeg 命令：不支持 | 、&& 、> 等 shell 操作符；"
                                 "含空格的路径请用引号包起来"));
    connect(command_edit_, &QLineEdit::returnPressed, this, &FfmpegPanel::OnRun);
    connect(command_edit_, &QLineEdit::textChanged, this, &FfmpegPanel::OnCommandTextChanged);
    // QLineEdit 自己不处理 ↑ / ↓，装上过滤器才能翻历史
    command_edit_->installEventFilter(this);
    layout->addWidget(command_edit_);

    QHBoxLayout* button_row = new QHBoxLayout();
    button_row->setSpacing(6);
    insert_input_button_ = new QPushButton(tr("插入当前媒体路径"), box);
    insert_input_button_->setToolTip(tr("把主窗口当前打开的媒体路径（带引号）插到光标处"));
    connect(insert_input_button_, &QPushButton::clicked, this, &FfmpegPanel::OnInsertInputPath);
    insert_output_button_ = new QPushButton(tr("插入输出路径…"), box);
    connect(insert_output_button_, &QPushButton::clicked, this, &FfmpegPanel::OnInsertOutputPath);
    templates_button_ = new QPushButton(tr("模板…"), box);
    templates_button_->setToolTip(tr("按场景给一整条命令（转码 / 只换封装 / 只留音轨 / 截片段 /\n"
                                     "压体积 / GIF / 抽帧），路径按当前素材自动填好"));
    connect(templates_button_, &QPushButton::clicked, this, &FfmpegPanel::OnShowTemplates);
    expand_button_ = new QPushButton(tr("展开编辑…"), box);
    expand_button_->setToolTip(tr("命令太长时摊成多行看：选项与它的值一行，路径一行。\n"
                                  "换行只是排版，运行时仍拼成一行"));
    connect(expand_button_, &QPushButton::clicked, this, &FfmpegPanel::OnOpenExpandedEditor);
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
    button_row->addWidget(templates_button_);
    button_row->addWidget(expand_button_);
    button_row->addStretch(1);
    button_row->addWidget(clear_button);
    button_row->addWidget(stop_button_);
    button_row->addWidget(run_button_);
    layout->addLayout(button_row);

    // ---- 跑完之后：产出清单 + 去结果 ----
    // 单独一行、默认隐藏：命令还没跑的时候这里没有东西可说，
    // 常驻一个空标签只会把命令区挤高。
    result_actions_ = new QWidget(box);
    QHBoxLayout* result_row = new QHBoxLayout(result_actions_);
    result_row->setContentsMargins(0, 0, 0, 0);
    result_row->setSpacing(6);
    result_label_ = new QLabel(result_actions_);
    result_label_->setTextInteractionFlags(Qt::TextSelectableByMouse); // 路径可选中复制
    open_dir_button_ = new QPushButton(tr("打开输出目录"), result_actions_);
    open_dir_button_->setToolTip(tr("在文件管理器里打开产出所在的目录"));
    connect(open_dir_button_, &QPushButton::clicked, this, &FfmpegPanel::OnOpenOutputDir);
    open_media_button_ = new QPushButton(tr("在 VideoEye 中打开"), result_actions_);
    connect(open_media_button_, &QPushButton::clicked, this, &FfmpegPanel::OnOpenOutputMedia);
    result_row->addWidget(result_label_, 1);
    result_row->addWidget(open_media_button_);
    result_row->addWidget(open_dir_button_);
    result_actions_->setVisible(false);
    layout->addWidget(result_actions_);

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
        text = tr("程序: %1\n来源: %2\n版本: %3").arg(tool_info_.path, tool_info_.origin, version);
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
    const QString run_tip = usable ? QString() : tr("先安装 ffmpeg 或在上方指定可执行程序路径，才能运行命令");
    run_button_->setEnabled(usable && !runner_->IsRunning());
    run_button_->setToolTip(run_tip);
    command_edit_->setToolTip(usable ? tr("只支持单独一条 ffmpeg 命令：不支持 | 、&& 、> 等 shell 操作符；"
                                          "含空格的路径请用引号包起来。↑ / ↓ 可翻最近跑过的命令")
                                     : run_tip);
}

void FfmpegPanel::OnShowInstallGuide() {
    const ffmpeg::FfmpegInstallGuide guide = ffmpeg::FfmpegToolLocator::InstallGuide();

    QString html = QStringLiteral("<h3 style='margin:2px'>%1</h3>").arg(tr("安装 ffmpeg（%1）").arg(guide.platform));
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
                        "不装 ffmpeg 也完全能用。")
                         .toHtmlEscaped());

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
            return; // 同一个程序的探测还在跑，让它跑完
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
    probe_queue_ = {QStringLiteral("-version"), QStringLiteral("-encoders"), QStringLiteral("-filters"),
                    QStringLiteral("-formats")};
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
        return; // 上一轮的残余输出，丢弃，别混进 buffer
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

    if (result.status == ffmpeg::FfmpegRunStatus::Failed || result.status == ffmpeg::FfmpegRunStatus::StartError) {
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

void FfmpegPanel::InsertSnippet(const QString& snippet) {
    // 空光标紧贴已有 token 时直接 insert() 会粘成 "in.mp4-c:v"；`-i ""` 插完光标还会
    // 落在引号外面，用户一输入路径就写到引号外了。规则都在 ComposeInsertion 里。
    QString base = command_edit_->text();
    int at = command_edit_->cursorPosition();
    if (command_edit_->hasSelectedText()) {
        // 有选区时按"替换选区"办，跟 QLineEdit::insert() 原本的行为一致
        at = command_edit_->selectionStart();
        base.remove(at, command_edit_->selectedText().size());
    }
    const auto composed = ffmpeg::ComposeInsertion(base, at, snippet);
    if (composed.text != command_edit_->text()) {
        command_edit_->setText(composed.text);
    }
    command_edit_->setCursorPosition(composed.cursor);
    command_edit_->setFocus();
}

void FfmpegPanel::RememberCommand(const QString& command) {
    if (!history_.Push(command)) {
        return;
    }
    settings_.setValue(QStringLiteral("ffmpeg/history"), history_.Entries());
}

bool FfmpegPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == command_edit_ && event->type() == QEvent::KeyPress && !history_.Entries().isEmpty()) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        const bool up = key_event->key() == Qt::Key_Up;
        const bool down = key_event->key() == Qt::Key_Down;
        // 没在翻历史时按 ↓ 什么都不该发生（也别把按键吃掉，留给可能存在的默认行为）
        if (up || (down && history_.browsing())) {
            const QString next = up ? history_.Previous(command_edit_->text()) : history_.Next();
            command_edit_->setText(next);
            command_edit_->end(false); // 光标放到末尾，跟 shell 的手感一致
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FfmpegPanel::OnInsertInputPath() {
    if (current_media_path_.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("还没有打开媒体文件。可以先在主窗口打开一个，或手动输入路径。"));
        return;
    }
    InsertSnippet(QStringLiteral("\"%1\"").arg(current_media_path_));
}

void FfmpegPanel::OnInsertOutputPath() {
    // 默认名跟着素材走：in.mkv → in_out.mkv。写死 _out.mp4 会悄悄换掉容器
    // （把 mkv 里的字幕/多音轨塞进 mp4 往往直接失败）。
    const QString start = ffmpeg::DefaultOutputPath(current_media_path_);
    const QString path = QFileDialog::getSaveFileName(this, tr("选择输出文件"), start);
    if (path.isEmpty()) {
        return;
    }
    InsertSnippet(QStringLiteral("\"%1\"").arg(path));
}

void FfmpegPanel::OnShowTemplates() {
    // 每次点开都新建菜单：模板表是静态的，没必要长期持有 QMenu（还省得跟主题样式纠缠）
    QMenu menu(this);
    menu.setToolTipsVisible(true); // 标题只有一行，说明放在悬停里
    for (const ffmpeg::FfmpegCommandTemplate& tmpl : ffmpeg::FfmpegTemplateLibrary()) {
        QAction* action = menu.addAction(tmpl.title);
        action->setToolTip(tmpl.summary);
        const QString id = tmpl.id;
        connect(action, &QAction::triggered, this, [this, id]() { ApplyTemplate(id); });
    }
    menu.exec(templates_button_->mapToGlobal(QPoint(0, templates_button_->height())));
}

void FfmpegPanel::OnOpenExpandedEditor() {
    FfmpegCommandEditorDialog dialog(command_edit_->text(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return; // 取消：命令原样留着
    }
    const QString command = dialog.Command();
    if (command != command_edit_->text()) {
        command_edit_->setText(command); // textChanged 会顺带刷新解释区
        command_edit_->setCursorPosition(static_cast<int>(command.size()));
    }
    if (dialog.RunRequested()) {
        // 对话框里点的是「运行」：直接跑，省掉"关窗再回车"这一步
        OnRun();
    }
}

void FfmpegPanel::ApplyTemplate(const QString& id) {
    const ffmpeg::FfmpegCommandTemplate* tmpl = ffmpeg::FindFfmpegTemplate(id);
    if (tmpl == nullptr) {
        return;
    }

    // 有当前素材就用它，没有就保留占位符（模板本身仍然可读，用户自己替换）
    const QString input = current_media_path_;
    const QString output = ffmpeg::TemplateOutputPath(*tmpl, input);
    const QStringList arguments = ffmpeg::InstantiateTemplate(*tmpl, input, output);
    const QString command = ffmpeg::ComposeTemplateCommand(arguments);
    if (command.isEmpty()) {
        return;
    }

    // 模板是**整条命令**，插到光标处只会得到半截能用的东西 —— 只能整体替换。
    // 但用户可能已经手写了半条，直接盖掉不讲一声不合适。
    const QString current = command_edit_->text();
    if (!current.trimmed().isEmpty() && current != command) {
        QMessageBox box(this);
        box.setWindowTitle(tr("替换当前命令？"));
        box.setIcon(QMessageBox::Question);
        box.setText(tr("命令框里已经有内容，插入模板会把它整条替换掉。"));
        box.setInformativeText(tr("模板内容：\n%1").arg(command));
        QPushButton* replace_button = box.addButton(tr("替换"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel); // 默认不破坏已有输入
        box.exec();
        if (box.clickedButton() != replace_button) {
            return;
        }
    }

    command_edit_->setText(command);
    // 模板里的默认值（片段时间、CRF）先选中，用户敲字就是改它
    const int focus = ffmpeg::FindFocusToken(command, tmpl->focus_literal);
    if (focus >= 0) {
        command_edit_->setSelection(focus, static_cast<int>(tmpl->focus_literal.size()));
    } else {
        command_edit_->setCursorPosition(static_cast<int>(command.size()));
    }

    QString note = tr("已插入模板：%1。%2").arg(tmpl->title, tmpl->summary);
    const QStringList missing = ffmpeg::MissingTemplateSlots(arguments);
    if (!missing.isEmpty()) {
        note += tr("（%1 还没填 —— 先打开一个素材，或手动把它们换成实际路径）").arg(missing.join(QStringLiteral(" ")));
    }
    emit StatusMessage(note);
}

void FfmpegPanel::OnOpenOutputDir() {
    const QString dir = ffmpeg::RevealDirectory(last_outputs_);
    if (dir.isEmpty()) {
        emit StatusMessage(tr("没有可打开的目录：这次命令没有本地输出文件"));
        return;
    }
    // 用 openUrl 而不是 QProcess 调 explorer：Windows / macOS / Linux 各一套命令，
    // 交给桌面环境自己决定最省事（项目里导出模块也是这么做的）。
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void FfmpegPanel::OnOpenOutputMedia() {
    if (last_outputs_.files.isEmpty()) {
        emit StatusMessage(tr("没有可打开的文件：这次运行没有产出"));
        return;
    }
    const QString path = last_outputs_.files.front();
    // 图片序列是一族文件，VideoEye 一次只能开一个 —— 明说是"第一帧"，
    // 不然用户会以为整个序列都进去了
    if (last_outputs_.has_sequence) {
        emit StatusMessage(tr("抽帧输出是图片序列，这里打开的是第一帧：%1").arg(QFileInfo(path).fileName()));
    }
    emit OpenMediaRequested(path);
}

void FfmpegPanel::RefreshResultActions() {
    const bool has_output = !last_outputs_.files.isEmpty();
    result_actions_->setVisible(has_output);
    if (!has_output) {
        return;
    }

    const QString first = QFileInfo(last_outputs_.files.front()).fileName();
    // 文件名可能很长，这一行还要留给两个按钮：截一下，完整路径进 tooltip
    const QString shown = first.size() > 40 ? first.left(37) + QStringLiteral("…") : first;
    QString text = last_run_finished_ ? tr("本次产出: %1").arg(shown)
                                      // 失败/被停止时也可能留下半个文件，说清楚比藏起来好
                                      : tr("已生成（未正常结束，可能不完整）: %1").arg(shown);
    if (last_outputs_.files.size() > 1) {
        text += tr(" 等 %1 个文件").arg(last_outputs_.files.size());
    }
    result_label_->setText(text);

    // 完整路径进 tooltip：命令区这一行放不下长路径，但用户经常要复制它
    result_label_->setToolTip(last_outputs_.files.join(QLatin1Char('\n')));
    open_dir_button_->setEnabled(!ffmpeg::RevealDirectory(last_outputs_).isEmpty());
    open_media_button_->setEnabled(true);
    open_media_button_->setToolTip(last_outputs_.has_sequence ? tr("打开第一帧（图片序列不能整组打开）")
                                                              : tr("用本程序打开这个文件"));
}

void FfmpegPanel::OnClearCommand() {
    command_edit_->clear();
    output_widget_->ClearOutput();
    // 清空命令 = 换了话题，上一轮的产出清单不该继续挂在那儿
    last_outputs_ = ffmpeg::FfmpegRunOutputs();
    if (result_actions_ != nullptr) {
        result_actions_->setVisible(false);
    }
}

void FfmpegPanel::OnCommandTextChanged() {
    RefreshExplanation();
    // 图谱描述的永远是**上一次运行**。命令一改就得挂提醒，否则用户会把旧图当成
    // 当前命令的效果 —— 这是这类"事后对比"最容易骗人的地方。
    if (graph_arguments_.isEmpty()) {
        return;
    }
    const auto parsed = ffmpeg::ParseCommandLine(command_edit_->text());
    const bool same = parsed.status == ffmpeg::CommandParseStatus::Ok && parsed.arguments == graph_arguments_;
    output_widget_->SetChangeGraphStaleHint(same ? QString()
                                                 : tr("命令已改动：图上是上一次运行的结果，重新运行后更新"));
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
    // 运行中改命令不会影响已经在跑的那个进程（参数早就递过去了），
    // 但"改了却没生效"比改不了更让人困惑
    expand_button_->setEnabled(!running);

    // 一开始跑就把上一次的产出清单收起来：那一行指的是**上一轮**的结果，
    // 留着会让用户以为本次已经产出了东西。
    if (running && result_actions_ != nullptr) {
        result_actions_->setVisible(false);
    }
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
                                "请把结果改成输出到文件（例如 out.mp4）。")
                                 .arg(pipe));
        return;
    }

    tool_info_ = ffmpeg::FfmpegToolLocator::Resolve(CurrentToolPath());
    if (tool_info_.path.isEmpty() || !tool_info_.exists) {
        QMessageBox warning_box(this);
        warning_box.setWindowTitle(tr("无法运行"));
        warning_box.setIcon(QMessageBox::Warning);
        warning_box.setText(tr("找不到可执行的 ffmpeg 程序%1。")
                                .arg(tool_info_.path.isEmpty() ? QString() : tr("（%1）").arg(tool_info_.path)));
        warning_box.setInformativeText(tr("本页是把命令交给原生 ffmpeg 程序执行，所以必须先有这个程序。\n"
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

    // ---- 运行前路径预检 ----
    //
    // 三类「必然失败」在启动 ffmpeg 之前就能看出来：输入不存在、输出目录还没建、
    // 输出写成了输入。放在覆盖确认**之前**是有意的 —— 否则用户会先被问一遍
    // "输出文件已存在，要覆盖吗"，答完才被告知"那是你的输入文件"，白绕一圈。
    //
    // 「输出 == 输入」不留"仍然运行"：ffmpeg 自己也会拒绝（实测带 -y 也拦得住，
    // 源文件不会被改动），放过去只是多一次必然失败的等待。
    // 其余问题留「仍然运行」：网络盘稍后才挂上、文件正被别的进程创建，都真实存在。
    QSet<QString> acknowledged; // 已经确认过"仍然运行"的问题，改名之后不必再问一遍
    const auto confirm_preflight = [this, &acknowledged](const QStringList& args) -> bool {
        const auto issues = ffmpeg::PreflightCommand(args);
        if (issues.empty()) {
            return true;
        }
        QStringList lines;
        bool blocks = false;
        bool has_fresh = false;
        for (const auto& issue : issues) {
            lines << QStringLiteral("· %1").arg(issue.message);
            if (issue.id == QLatin1String("same-as-input")) {
                blocks = true;
            }
            if (!acknowledged.contains(issue.id)) {
                has_fresh = true;
            }
        }

        QMessageBox box(this);
        box.setWindowTitle(blocks ? tr("输出和输入是同一个文件") : tr("命令可能跑不起来"));
        box.setIcon(QMessageBox::Warning);
        box.setText(lines.join(QLatin1Char('\n')));
        if (blocks) {
            box.setInformativeText(tr("ffmpeg 会拒绝执行这种命令（「same as Input #0 - exiting」），"
                                      "源文件不会被改动。请换一个输出文件名。"));
            box.addButton(tr("返回修改"), QMessageBox::AcceptRole);
            box.exec();
            return false;
        }
        if (!has_fresh) {
            return true; // 全是已经点过"仍然运行"的问题，别再弹一次
        }
        box.setInformativeText(tr("这些都是运行前就能看出来的问题，按现在的命令，ffmpeg 多半在开头就退出了。\n"
                                  "「仍然运行」不会改动命令内容。"));
        QPushButton* run_anyway = box.addButton(tr("仍然运行"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel); // 默认停在安全的那一侧
        box.exec();
        if (box.clickedButton() != run_anyway) {
            return false;
        }
        for (const auto& issue : issues) {
            acknowledged.insert(issue.id);
        }
        return true;
    };

    if (!confirm_preflight(parsed.arguments)) {
        return; // 命令原样留着，用户自己去改路径
    }

    // ---- 覆盖确认 ----
    //
    // 目标已存在时若放行，ffmpeg 会停在 stdin 上问 "Overwrite? [y/N]"：本页的 stdin
    // 是留着给「停止」写 q 的，没有别的东西会回答它，于是任务看起来永远"运行中"；
    // 点停止时那个 q 正好被当成"不覆盖"，ffmpeg 以退出码 0 收场 —— 界面上会显示
    // "已停止（正常收尾）"，其实一个字节都没写。所以必须在这里先问清楚。
    QStringList exec_arguments = parsed.arguments;
    bool overwrite_appended = false;
    if (!exec_arguments.contains(QStringLiteral("-y")) && !exec_arguments.contains(QStringLiteral("-n"))) {
        const QStringList existing = ffmpeg::ExistingOutputTargets(exec_arguments);
        if (!existing.isEmpty()) {
            const QString detail =
                existing.size() == 1
                    ? tr("输出文件已存在：\n%1").arg(existing.front())
                    : tr("有 %1 个输出文件已存在：\n%2").arg(existing.size()).arg(existing.join(QLatin1Char('\n')));
            QMessageBox box(this);
            box.setWindowTitle(tr("输出文件已存在"));
            box.setIcon(QMessageBox::Warning);
            box.setText(detail);
            box.setInformativeText(tr("ffmpeg 默认不覆盖已存在的文件。继续运行会给这次执行加上 -y，"
                                      "命令输入框里的内容不会被改动。"));
            QPushButton* overwrite_btn = box.addButton(tr("覆盖"), QMessageBox::AcceptRole);
            QPushButton* rename_btn =
                existing.size() == 1 ? box.addButton(tr("换个文件名…"), QMessageBox::ActionRole) : nullptr;
            box.addButton(QMessageBox::Cancel);
            box.exec();

            if (box.clickedButton() == overwrite_btn) {
                exec_arguments.append(QStringLiteral("-y"));
                overwrite_appended = true;
            } else if (rename_btn != nullptr && box.clickedButton() == rename_btn) {
                const QString chosen = QFileDialog::getSaveFileName(this, tr("选择输出文件"), existing.front());
                if (chosen.isEmpty()) {
                    return;
                }
                const int at = exec_arguments.indexOf(existing.front());
                if (at < 0) {
                    return; // 理论上不会发生：existing 就是从这份参数里挑出来的
                }
                exec_arguments[at] = chosen;
                // 回填输入框，让"看到的命令"和"真正执行的命令"保持一致。用户在保存
                // 对话框里已经确认过是否覆盖同名文件，所以这里按覆盖处理（补 -y），
                // 否则又会回到"卡在覆盖提示"的老路上。
                if (QFileInfo::exists(chosen) && !exec_arguments.contains(QStringLiteral("-y"))) {
                    exec_arguments.append(QStringLiteral("-y"));
                    overwrite_appended = true;
                }
                QString new_text =
                    ffmpeg::FfmpegProcessRunner::BuildDisplayCommand(parsed.program_token, exec_arguments);
                command_edit_->setText(new_text.trimmed());
            } else {
                return; // 取消：什么都不做，命令保持原样
            }
        }
    }

    // 改名之后输出路径变了，得再查一遍：用户可能在保存对话框里又选回输入文件本身。
    // 已经点过"仍然运行"的问题不会重复弹（acknowledged）。
    if (!confirm_preflight(exec_arguments)) {
        return;
    }

    current_arguments_ = exec_arguments;
    progress_.Reset();
    output_widget_->PrepareForRun();
    // 排在 PrepareForRun 之后：它会清空图谱（新一轮不能留着上一轮的结果），
    // 先排队的话，万一探测比清空先回来，那张刚画好的"工序预览"就被抹掉了。
    BeginMediaProbeForRun(program);
    output_widget_->AppendLog(
        QStringLiteral("$ %1").arg(ffmpeg::FfmpegProcessRunner::BuildDisplayCommand(program, exec_arguments)), false);
    if (overwrite_appended) {
        output_widget_->AppendLog(tr("(输出文件已存在：本次执行已自动加 -y 覆盖)"), false);
    }
    if (program != tool_info_.path) {
        output_widget_->AppendLog(tr("(命令里写了完整路径: 本次用它执行；本页设置的 %1 已被忽略)").arg(tool_info_.path),
                                  false);
    }
    SetRunUiState(true);
    const bool started = runner_->Start(program, exec_arguments);
    if (!started) {
        SetRunUiState(false);
    } else {
        // 只记真正发起过的命令：解析失败 / 用户取消覆盖确认的那些不该混进历史
        RememberCommand(command_edit_->text());
    }
    // 启动失败时不能再说"已启动" —— 否则状态栏会前后矛盾
    emit StatusMessage(started ? tr("已启动 ffmpeg") : tr("ffmpeg 启动失败"));
}

void FfmpegPanel::OnStop() {
    if (!runner_->IsRunning()) {
        return;
    }
    if (ffmpeg::FfmpegProcessRunner::CanQuitViaStdin(current_arguments_)) {
        output_widget_->AppendLog(tr("(用户请求停止: 已向 ffmpeg 发送 q，等它正常收尾；超时后会强制终止)"), false);
    } else {
        output_widget_->AppendLog(tr("(用户请求停止: 命令里有 -nostdin，ffmpeg 不读标准输入，"
                                     "无法优雅收尾，只能直接终止 —— 输出文件可能不完整)"),
                                  true);
    }
    runner_->Stop();
    emit StatusMessage(tr("正在停止 ffmpeg"));
}

bool FfmpegPanel::ConfirmShutdown() {
    if (runner_ == nullptr || !runner_->IsRunning()) {
        return true;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("ffmpeg 还在运行"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("有一条 ffmpeg 命令正在执行。"));
    box.setInformativeText(tr("直接退出会掐断它 —— mp4/mov 这类容器要先写完索引（moov box）才能播放，"
                              "收尾没做完就只有一个打不开的文件，已经转好的部分也白费。\n\n"
                              "「停止并退出」会先请它自己收尾（最多等 5 秒），收不住才强杀。"));
    QPushButton* stop_button = box.addButton(tr("停止并退出"), QMessageBox::AcceptRole);
    QPushButton* cancel_button = box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel_button); // 默认停在安全的那一侧
    box.exec();
    if (box.clickedButton() != stop_button) {
        return false;
    }

    output_widget_->AppendLog(tr("(关闭窗口: 已请 ffmpeg 收尾，最多等 5 秒)"), false);
    const bool stopped = runner_->StopAndWait(5000);
    output_widget_->AppendLog(stopped ? tr("(ffmpeg 已退出，输出文件的收尾是完整的)")
                                      : tr("(ffmpeg 没在宽限期内退出，已被强制终止 —— 输出文件可能不可用)"),
                              !stopped);
    output_widget_->FlushLog();
    return true;
}

void FfmpegPanel::OnRunOutput(const QString& text, bool is_error) {
    // 进度就从这条流里读：ffmpeg 的 `Duration:` 和 `time=` 都在 stderr，不用另起探测。
    if (progress_.Feed(text)) {
        output_widget_->ShowProgress(progress_.Snapshot());
    }
    output_widget_->AppendLog(text, is_error);
}

void FfmpegPanel::OnRunFinished(const ffmpeg::FfmpegRunResult& result) {
    SetRunUiState(false);
    // 状态行 / 退出码 / 耗时 / 失败切页 / 强杀警告全在输出区组件内；
    // 组件另经 StatusMessage 把终态文案发回页面统一展示。
    output_widget_->ShowRunResult(result);

    // 「产出」用磁盘实测，而不是"命令里写了什么就报什么" —— 覆盖被拒、失败早退
    // 都会让命令里的输出名根本没有落到盘上。
    last_run_finished_ = result.status == ffmpeg::FfmpegRunStatus::Finished;
    last_outputs_ = ffmpeg::CollectRunOutputs(current_arguments_);
    RefreshResultActions();
    // 产出逐个探一遍：图谱的"实测"一列只能这么来
    QueueOutputMediaProbes();
}

// ===================== 转换图谱：媒体探测 =====================
//
// 这一节只干一件事：把「输入 / 每个输出到底是什么参数」问出来，交给
// FfmpegChangeGraph 去对比。它**不参与**运行决策 —— 探测失败只让图谱少一列，
// 命令照跑，日志照记。

namespace {

/// 命令里第一个 -i 的值。多输入命令的合并是另一件事（图谱按第一个输入画）。
QString FirstInputTarget(const QStringList& arguments) {
    const ffmpeg::FfmpegCommandStructure structure = ffmpeg::AnalyzeCommandStructure(arguments);
    for (const ffmpeg::FfmpegArgPlacement& place : structure.args) {
        if (place.role == ffmpeg::FfmpegArgRole::InputUrl) {
            return arguments.at(place.index);
        }
    }
    return QString();
}

/// 值不值得探：本地文件与 URL 可以（URL 会真连，所以有 10 秒看门狗）；
/// `-f lavfi` 的虚拟源、`-`、`pipe:`、`concat:` 这类"不存在于磁盘"的目标不探 ——
/// 探了也只会得到一句"文件不存在"，纯噪声。
bool IsProbeableTarget(const QString& target) {
    if (target.isEmpty()) {
        return false;
    }
    if (ffmpeg::IsLocalPathTarget(target)) {
        return true;
    }
    return target.contains(QLatin1String("://"));
}

} // namespace

void FfmpegPanel::BeginMediaProbeForRun(const QString& program) {
    CancelMediaProbes();
    ++media_probe_generation_;
    media_probe_program_ = program;
    media_probe_timed_out_ = false;
    input_probe_ = ffmpeg::MediaProbeSnapshot{};
    output_probes_.clear();
    probed_output_arguments_.clear();
    graph_arguments_ = current_arguments_;

    const QString input = FirstInputTarget(current_arguments_);
    if (IsProbeableTarget(input)) {
        media_probe_jobs_.push_back({input, true, -1});
    }
    if (media_probe_jobs_.empty()) {
        return;
    }
    media_probe_cursor_ = 0;
    RunNextMediaProbe();
}

void FfmpegPanel::QueueOutputMediaProbes() {
    if (current_arguments_.isEmpty() || probed_output_arguments_ == current_arguments_) {
        return; // 同一组参数只探一次（重跑才重新探）
    }
    probed_output_arguments_ = current_arguments_;

    QStringList targets;
    const ffmpeg::FfmpegCommandStructure structure = ffmpeg::AnalyzeCommandStructure(current_arguments_);
    for (const ffmpeg::FfmpegArgPlacement& place : structure.args) {
        if (place.role == ffmpeg::FfmpegArgRole::OutputUrl) {
            targets << current_arguments_.at(place.index);
        }
    }
    if (targets.isEmpty()) {
        return;
    }

    // 与命令里的输出**同序**占位：BuildChangeGraphs 按下标取，缺一个就整列错位。
    output_probes_.assign(static_cast<size_t>(targets.size()), ffmpeg::MediaProbeSnapshot{});
    for (int i = 0; i < targets.size(); ++i) {
        const QString target = targets.at(i);
        ffmpeg::MediaProbeSnapshot& slot = output_probes_.at(static_cast<size_t>(i));
        if (ffmpeg::HasSequencePattern(target)) {
            // 图片序列没有"那一个文件"，逐字段对比无从谈起 —— 说明白，别画空节点。
            slot.error = tr("图片序列（%1）没有单一成品文件，不做逐字段对比").arg(target);
            continue;
        }
        if (!ffmpeg::IsLocalPathTarget(target)) {
            slot.error = tr("输出不是本地文件（URL / 管道），没有可探测的成品");
            continue;
        }
        if (!QFileInfo::exists(target)) {
            slot.error = tr("命令声明了这个输出，但磁盘上没有产出");
            continue;
        }
        media_probe_jobs_.push_back({target, false, i});
    }
    if (media_probe_jobs_.empty()) {
        RefreshChangeGraph();
        return;
    }
    media_probe_cursor_ = 0;
    RunNextMediaProbe();
}

void FfmpegPanel::RunNextMediaProbe() {
    if (media_probe_cursor_ >= media_probe_jobs_.size()) {
        media_probe_buffer_.clear();
        if (media_probe_timeout_ != nullptr) {
            media_probe_timeout_->stop();
        }
        RefreshChangeGraph();
        if (!output_probes_.empty()) {
            // 实测齐了才切过去看一眼；「错误输出」页不动（见 FocusChangeGraphTab）
            output_widget_->FocusChangeGraphTab();
        }
        return;
    }
    if (media_probe_program_.isEmpty() || media_probe_runner_ == nullptr) {
        return;
    }
    const MediaProbeJob& job = media_probe_jobs_.at(media_probe_cursor_);
    media_probe_buffer_.clear();
    media_probe_timed_out_ = false;
    media_probe_launch_generation_ = media_probe_generation_;
    // 启动失败会走 Finished(StartError)，游标在那里推进，别在这儿再推一次。
    if (media_probe_runner_->Start(media_probe_program_, ffmpeg::BuildMediaProbeArguments(job.path))) {
        if (media_probe_timeout_ != nullptr) {
            media_probe_timeout_->start(10000);
        }
    }
}

void FfmpegPanel::OnMediaProbeOutput(const QString& text, bool /*is_error*/) {
    if (media_probe_launch_generation_ != media_probe_generation_) {
        return; // 上一轮的残余输出
    }
    media_probe_buffer_ += text + QLatin1Char('\n');
}

void FfmpegPanel::OnMediaProbeFinished(const ffmpeg::FfmpegRunResult& result) {
    if (media_probe_timeout_ != nullptr) {
        media_probe_timeout_->stop();
    }
    if (media_probe_launch_generation_ != media_probe_generation_) {
        media_probe_buffer_.clear();
        return;
    }
    if (media_probe_cursor_ >= media_probe_jobs_.size()) {
        return;
    }
    const MediaProbeJob job = media_probe_jobs_.at(media_probe_cursor_);

    // ⚠️ `ffmpeg -i <file>` 恒定以退出码 1 结束（"At least one output file must be
    // specified"），所以这里**不看** status == Failed；只看能不能解析出流信息。
    ffmpeg::MediaProbeSnapshot snapshot = ffmpeg::ParseMediaProbe(media_probe_buffer_, job.path);
    if (!snapshot.parsed) {
        if (media_probe_timed_out_) {
            snapshot.error = tr("探测超时（10 秒没读完文件头），这一项留空");
        } else if (result.status == ffmpeg::FfmpegRunStatus::StartError) {
            snapshot.error = result.error_message.isEmpty() ? tr("探测程序没能启动")
                                                            : tr("探测程序没能启动：%1").arg(result.error_message);
        }
    }
    ffmpeg::FillLocalFileFacts(&snapshot);

    if (job.is_input) {
        input_probe_ = snapshot;
    } else if (job.output_index >= 0 && job.output_index < static_cast<int>(output_probes_.size())) {
        output_probes_.at(static_cast<size_t>(job.output_index)) = snapshot;
    }
    ++media_probe_cursor_;
    RunNextMediaProbe();
}

void FfmpegPanel::OnMediaProbeTimeout() {
    media_probe_timed_out_ = true;
    if (media_probe_runner_ != nullptr && media_probe_runner_->IsRunning()) {
        // 探测没有输出文件要收尾 —— 直接终止，不用给优雅退出的时间
        media_probe_runner_->Stop(0, 0);
        return;
    }
    RunNextMediaProbe();
}

void FfmpegPanel::CancelMediaProbes() {
    ++media_probe_generation_; // 在飞的回调一律作废
    media_probe_jobs_.clear();
    media_probe_cursor_ = 0;
    media_probe_buffer_.clear();
    if (media_probe_timeout_ != nullptr) {
        media_probe_timeout_->stop();
    }
    if (media_probe_runner_ != nullptr && media_probe_runner_->IsRunning()) {
        media_probe_runner_->Stop(0, 0);
    }
}

void FfmpegPanel::RefreshChangeGraph() {
    if (current_arguments_.isEmpty()) {
        return;
    }
    std::vector<ffmpeg::ChangeGraph> graphs =
        ffmpeg::BuildChangeGraphs(input_probe_, current_arguments_, output_probes_);
    output_widget_->ShowChangeGraphs(std::move(graphs));
    output_widget_->SetChangeGraphStaleHint(QString());
}

void FfmpegPanel::SetCurrentFile(const QString& path) {
    current_media_path_ = path;
    insert_input_button_->setEnabled(!path.isEmpty() && !runner_->IsRunning());
    insert_input_button_->setToolTip(path.isEmpty() ? tr("还没有打开媒体文件") : tr("插入: %1").arg(path));
}

} // namespace ui
} // namespace videoeye
