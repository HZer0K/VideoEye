#pragma once

// 「FFmpeg 命令工作台」页。
//
// 定位: 原生 ffmpeg 的图形化入口 —— 写命令、看输出、查参数。它不是终端:
//   * 只执行单独一条 ffmpeg 命令，不支持管道 / 串联 / 重定向；
//   * 命令文本由 FfmpegCommandParser 切成参数数组后交给 QProcess，不经 shell；
//   * 输出到 stdout 的媒体数据（pipe:1）会被拦下，因为日志框只装得下文本。
//
// 页面不依赖"当前打开的媒体"：没加载素材也能打开这一页直接用。
// 真正干活的三块在 core/ffmpeg/：解析（Parser）、执行（ProcessRunner）、
// 解释（Catalog + Explainer）。
//
// 页面由三块拼成：命令区 / 程序区（本类）、输出与解释区（FfmpegOutputWidget）、
// 指令字典区（FfmpegDictionaryWidget）—— 后两区的内部规则归各自组件。

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QWidget>

#include <QString>
#include <QStringList>
#include <vector>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandHistory.h"
#include "core/ffmpeg/FfmpegMediaProbe.h"
#include "core/ffmpeg/FfmpegProcessRunner.h"
#include "core/ffmpeg/FfmpegProgressTracker.h"
#include "core/ffmpeg/FfmpegRunOutputs.h"
#include "core/ffmpeg/FfmpegToolLocator.h"

// 前向声明必须在命名空间**外**：放到 videoeye::ui 里会变成 videoeye::ui::QTimer，
// 任何 include 本头的 .cpp 里写的 QTimer 都会被这个名字遮蔽（实测踩过一次）。
class QTimer;

namespace videoeye {
namespace ui {

class FfmpegDictionaryWidget;   // 前向声明：指令字典区 (见 FfmpegDictionaryWidget.{h,cpp})
class FfmpegOutputWidget;       // 前向声明：输出与解释区 (见 FfmpegOutputWidget.{h,cpp})
class FfmpegCommandInputAssist; // 前向声明：命令框的实时解释与补全 (见 FfmpegCommandInputAssist.{h,cpp})

class FfmpegPanel : public QWidget {
    Q_OBJECT

public:
    explicit FfmpegPanel(QWidget* parent = nullptr);
    ~FfmpegPanel() override;

signals:
    void StatusMessage(const QString& text);

    /// 用户点了「在 VideoEye 中打开」。路径由主窗口接走 —— 页面不持有
    /// 「打开媒体」这件事的实现（换源、复位、异步收尾都在 OpenSessionController 里）。
    void OpenMediaRequested(const QString& path);

public slots:
    /// 主窗口打开/切换媒体时同步过来，供「插入当前媒体路径」使用
    void SetCurrentFile(const QString& path);

public:
    /// 关窗口 / 退出程序前调用。有 ffmpeg 在跑时问用户，并在确认后**同步**停干净
    /// （走 q → terminate → kill，最多约 8.5 秒）。返回 false = 用户选择不退出。
    ///
    /// 为什么必须拦一道：全仓只有 ~FfmpegProcessRunner 里的 kill() 兜底，直接关窗
    /// 会得到一个缺 moov box 的 mp4 —— 转了几十分钟的产出，最后一步白费。
    bool ConfirmShutdown();

protected:
    /// 命令输入框里按 ↑ / ↓ 翻历史（QLineEdit 自己不管这两个键）。
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void OnBrowseTool();
    void OnResetTool();
    void OnToolPathEdited();
    void OnShowInstallGuide();
    void OnOpenDownloadPage();
    void OnRedetectTool();
    void OnInsertInputPath();
    void OnInsertOutputPath();
    /// 弹出场景模板菜单（一键换成整条命令）
    void OnShowTemplates();
    /// 打开多行编辑器（长命令摊开看/改）；里面点「运行」就直接跑
    void OnOpenExpandedEditor();
    void OnOpenOutputDir();
    void OnOpenOutputMedia();
    void OnClearCommand();
    void OnRun();
    void OnStop();
    void OnCommandTextChanged();

    void OnRunOutput(const QString& text, bool is_error);
    void OnRunFinished(const ffmpeg::FfmpegRunResult& result);

    void OnProbeOutput(const QString& text, bool is_error);
    void OnProbeFinished(const ffmpeg::FfmpegRunResult& result);

    // ---- 转换图谱用的一次性媒体探测 ----
    void OnMediaProbeOutput(const QString& text, bool is_error);
    void OnMediaProbeFinished(const ffmpeg::FfmpegRunResult& result);
    void OnMediaProbeTimeout();

private:
    void BuildUi();
    QWidget* BuildCommandArea();
    // 指令字典区已抽为 FfmpegDictionaryWidget（见同目录），页面只把它放进 splitter。

    void RefreshToolInfo();
    void StartProbes();
    void RunNextProbe();
    /// 作废当前这一轮探测：让还在飞的回调失效（结果一律丢弃）
    void ForgetCurrentProbe();
    /// 停掉探测并清掉队列（ffmpeg 不可用 / 页面关闭时用）
    void CancelProbes();
    /// 上一轮是被"换程序"打断的 → 用新路径重来一轮
    void MaybeRestartProbes();
    /// 拼装"没找到 ffmpeg"时的安装指引对话框正文（纯文本，便于复制）
    QString BuildInstallGuideText() const;

    /// 转发给输出区组件；命令文本属于页面，组件不持有命令输入框
    void RefreshExplanation();
    void SetRunUiState(bool running);
    QString CurrentToolPath() const;
    /// 把片段按「不粘 token、光标落在引号里」的规则插进命令输入框
    void InsertSnippet(const QString& snippet);
    /// 用场景模板整体替换命令框内容（模板是整条命令，逐片段插入没有意义）
    void ApplyTemplate(const QString& id);
    /// 刷新「跑完之后去哪儿看」那一行（产出清单只在有产出时出现）
    void RefreshResultActions();
    /// 记住最近跑过的命令（去重、截断后回写设置）
    void RememberCommand(const QString& command);

    // ---- 转换图谱（声明 + 实测）----
    //
    // 为什么单独一套 runner / 世代号：能力探测（-version/-encoders/…）与这里的
    // "-i 读一遍素材"是两件毫不相干的事，共用一套会把两边的结果串在一起 ——
    // 换程序时能力探测会被整轮作废，媒体探测不该跟着一起没了；反过来也一样。
    //
    // 探测一律**不阻塞**：跑命令前探输入、跑完后逐输出探，探测失败只影响图谱，
    // 绝不影响命令本身的执行与日志。
    void BeginMediaProbeForRun(const QString& program);
    void QueueOutputMediaProbes();
    void RunNextMediaProbe();
    void CancelMediaProbes();
    /// 用现有快照重建图谱并推进输出区（探测每完成一项就刷一次）
    void RefreshChangeGraph();

    // ---- 程序区 ----
    QLineEdit* tool_path_edit_ = nullptr;
    QLabel* tool_status_label_ = nullptr;
    QPushButton* browse_button_ = nullptr;
    QPushButton* reset_button_ = nullptr;
    QWidget* tool_missing_bar_ = nullptr; // 只在找不到 ffmpeg 时出现的补救入口
    QPushButton* guide_button_ = nullptr;
    QPushButton* download_button_ = nullptr;
    QPushButton* redetect_button_ = nullptr;

    // ---- 命令区 ----
    QLineEdit* command_edit_ = nullptr;
    // 输入框的实时解释 / 补全：光标停在哪就说明哪个参数、并按前缀给候选。
    // 逻辑全在 core/ffmpeg/FfmpegCommandInputHint，组件只管排版与键盘。
    FfmpegCommandInputAssist* input_assist_ = nullptr;
    QPushButton* insert_input_button_ = nullptr;
    QPushButton* insert_output_button_ = nullptr;
    QPushButton* templates_button_ = nullptr; // 「模板…」：场景级的一整条命令
    QPushButton* expand_button_ = nullptr;    // 「展开编辑…」：多行查看与修改

    // ---- 跑完之后 ----
    // 只放"去结果"的动作：产出清单由 CollectRunOutputs 实测得出，没产出就不出现，
    // 免得用户点开一个空目录。
    QWidget* result_actions_ = nullptr;
    QLabel* result_label_ = nullptr;
    QPushButton* open_dir_button_ = nullptr;
    QPushButton* open_media_button_ = nullptr;
    ffmpeg::FfmpegRunOutputs last_outputs_;
    /// 上一次运行是否正常结束（决定"产出"还是"未正常结束的残留"）
    bool last_run_finished_ = false;
    QPushButton* run_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;

    // ---- 输出与解释区 ----
    // 已抽为独立组件 FfmpegOutputWidget：状态行 / 三个页签 / 日志缓存全在组件内，
    // 页面只管在运行时驱动它（PrepareForRun / AppendLog / ShowRunResult）并转发文案。
    FfmpegOutputWidget* output_widget_ = nullptr;

    // ---- 指令字典区 ----
    // 已抽为独立组件 FfmpegDictionaryWidget：搜索 / 分类 / 列表 / 详情 / 插入全在
    // 组件内，页面只把它放进 splitter 并接 InsertRequested 插到命令输入框。
    FfmpegDictionaryWidget* dict_widget_ = nullptr;

    // ---- 状态 ----
    //
    // 一轮探测（-version → -encoders → -filters → -formats）必须绑定**同一个**
    // ffmpeg 程序：中途换了路径，剩下几步如果继续用新路径跑，就会出现
    // "旧程序的版本号 + 新程序的编码器列表"。probe_program_ 是这一轮锁定的程序，
    // probe_generation_ 是这一轮的序号 —— 换程序时让旧序号失效，结果一律丢弃。
    ffmpeg::FfmpegProcessRunner* runner_ = nullptr;
    ffmpeg::FfmpegProcessRunner* probe_runner_ = nullptr;
    QString probe_program_;
    QStringList probe_queue_;
    QString probe_current_;
    QString probe_buffer_;
    int probe_generation_ = 0;
    int probe_launch_generation_ = 0;
    bool probe_restart_pending_ = false;

    ffmpeg::FfmpegToolInfo tool_info_;

    // ---- 转换图谱 ----
    struct MediaProbeJob {
        QString path;
        bool is_input = false;
        int output_index = -1; // 对应命令里的第几个输出
    };
    ffmpeg::FfmpegProcessRunner* media_probe_runner_ = nullptr;
    QTimer* media_probe_timeout_ = nullptr;
    /// 每轮探测一个世代号：换命令 / 关了页面，在飞的回调一律作废
    quint64 media_probe_generation_ = 0;
    quint64 media_probe_launch_generation_ = 0;
    std::vector<MediaProbeJob> media_probe_jobs_;
    size_t media_probe_cursor_ = 0;
    QString media_probe_buffer_;
    QString media_probe_program_;
    bool media_probe_timed_out_ = false;
    ffmpeg::MediaProbeSnapshot input_probe_;
    std::vector<ffmpeg::MediaProbeSnapshot> output_probes_;
    /// 图谱对应的那组参数（命令框被改过时用来判断"图上是不是旧结果"）
    QStringList graph_arguments_;
    /// 已经排过输出探测的那组参数，避免重复排队
    QStringList probed_output_arguments_;
    /// 本次运行的参数。停止时要用它判断能不能靠写 q 让 ffmpeg 优雅收尾。
    QStringList current_arguments_;
    /// 本次运行交给哪个工具。停止策略与文案按它分：
    /// ffmpeg 可以靠 stdin 的 q 收尾，ffprobe / ffplay 不读 stdin，只能直接终止。
    ffmpeg::FfmpegToolKind current_tool_ = ffmpeg::FfmpegToolKind::Ffmpeg;
    /// 上一次运行期间从 stderr 读到的进度（Duration + time=）
    ffmpeg::FfmpegProgressTracker progress_;
    /// ↑/↓ 翻阅用；内容存进 QSettings("ffmpeg/history")
    ffmpeg::FfmpegCommandHistory history_;
    QString tool_version_;
    QString current_media_path_;
    QSettings settings_;
};

} // namespace ui
} // namespace videoeye
