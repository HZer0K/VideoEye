#pragma once

// 「格式转换」页：把源文件转成别的容器 / 编码，面向"要交付、要能播"的场景。
//
// 与「FFmpeg 命令工作台」的分工：
//   * 工作台是**命令级**入口，用户自己写 argv，页面只负责跑与解释；
//   * 这一页是**任务级**入口，用户只选容器/编码/质量，命令由
//     core/ffmpeg/FormatConversionPlan 拼出来 —— 那些"容器收不收这种编码"的规则
//     在 core 里被单测钉住，页面不做二次判断。
//
// 页面只做四件事：收参数、逐字展示将执行的命令、驱动一个 ffmpeg 子进程、把结果
// 说清楚（成功 / 失败原因 / 停止是否干净 / 产出到底在不在盘上）。
//
// 与"当前打开的媒体"是**单向弱耦合**：主窗口通过 SetCurrentFile 告知，页面只在
// 输入框为空时自动填入，不抢用户已经选好的文件。

#include <QString>
#include <QStringList>
#include <QWidget>

#include "core/ffmpeg/FfmpegMediaProbe.h"
#include "core/ffmpeg/FfmpegProcessRunner.h"
#include "core/ffmpeg/FfmpegProgressTracker.h"
#include "core/ffmpeg/FormatConversionPlan.h"

// 前向声明必须在命名空间**外**：放进 videoeye::ui 会变成 videoeye::ui::QComboBox，
// 任何 include 本头的 .cpp 里写的 QComboBox 都会被这个名字遮蔽（踩过）。
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSettings;
class QSpinBox;
class QTimer;

namespace videoeye {
namespace ui {

class ConversionPanel : public QWidget {
    Q_OBJECT

public:
    explicit ConversionPanel(QWidget* parent = nullptr);
    ~ConversionPanel() override;

    /// 关窗口 / 退出前调用：有转换在跑时问用户，确认后**同步**停干净。
    /// 返回 false = 用户选择不退出。
    bool ConfirmShutdown();

signals:
    void StatusMessage(const QString& text);

    /// 用户点了「在 VideoEye 中打开」。路径交给主窗口 —— 打开媒体的实现
    /// （换源、复位、异步收尾）在 OpenSessionController 里，页面不持有。
    void OpenMediaRequested(const QString& path);

public slots:
    /// 主窗口打开 / 切换媒体时同步过来，供「用当前媒体」与空输入自动填充。
    void SetCurrentFile(const QString& path);

protected:
    /// 每次进本页重新解析一次 ffmpeg 路径：用户可能刚在「FFmpeg 命令」页改过设置。
    void showEvent(QShowEvent* event) override;

private slots:
    void OnBrowseInput();
    void OnUseCurrentFile();
    void OnBrowseOutput();
    void OnOutputEdited();
    void OnContainerChanged();
    void OnSettingsChanged();
    void OnStart();
    void OnStop();
    void OnCopyCommand();
    void OnOpenOutputDir();
    void OnOpenMedia();

    void OnRunOutput(const QString& text, bool is_error);
    void OnRunFinished(const ffmpeg::FfmpegRunResult& result);

    void OnProbeOutput(const QString& text, bool is_error);
    void OnProbeFinished(const ffmpeg::FfmpegRunResult& result);
    void OnProbeTimeout();

private:
    void BuildUi();
    QWidget* BuildHeader();

    /// 按当前容器重建编码下拉（不支持的项不出现），并尽量保住用户原来的选择。
    void RefreshCodecChoices();

    ffmpeg::ConversionProfile CurrentProfile() const;
    /// 把缩放 / 帧率归位到"保持原样"（流复制时 -vf 不被允许）。
    void ResetTransformChoices();
    /// 把只对特定编码有意义的选项归位：预设只留给 H.264/H.265，10-bit 只留给支持的编码。
    void NormalizeCodecOptions(ffmpeg::ConversionVideoCodec video);
    /// 读当前输入 / 输出 / 参数，刷新命令预览、提示行与「开始转换」可用性。
    void RebuildPlan();

    ffmpeg::ConversionSourceFacts CurrentSourceFacts() const;
    /// 用探测结果刷新"源信息"一行（容器 / 时长 / 分辨率 / 编码）。
    void RefreshSourceSummary();

    QString CurrentProgram() const;
    void RefreshToolState();
    void SetRunning(bool running);
    void StartProbe(const QString& path);
    void CancelProbe();

    void AppendLog(const QString& line, bool is_error);
    void ShowProgressLine();

    QSettings* settings_ = nullptr;

    // ---- 页头 ----
    QLabel* tool_label_ = nullptr;
    QLabel* source_summary_ = nullptr;

    // ---- 输入 ----
    QLineEdit* input_edit_ = nullptr;
    QPushButton* browse_input_button_ = nullptr;
    QPushButton* use_current_button_ = nullptr;

    // ---- 目标参数 ----
    QComboBox* container_combo_ = nullptr;
    QComboBox* video_combo_ = nullptr;
    QComboBox* audio_combo_ = nullptr;
    QComboBox* quality_combo_ = nullptr;
    QSpinBox* crf_spin_ = nullptr;
    QComboBox* rate_mode_combo_ = nullptr;
    QSpinBox* video_bitrate_spin_ = nullptr;
    QComboBox* preset_combo_ = nullptr;
    QComboBox* pixel_combo_ = nullptr;
    QComboBox* bitrate_combo_ = nullptr;
    QComboBox* scale_combo_ = nullptr;
    QComboBox* fps_combo_ = nullptr;
    QLineEdit* extra_edit_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QPushButton* browse_output_button_ = nullptr;
    /// 输出路径是否由程序自动生成（用户一改就置 false，此后不再被覆盖）
    bool auto_output_ = true;

    // ---- 命令预览 ----
    QPlainTextEdit* command_view_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QPushButton* copy_button_ = nullptr;

    // ---- 运行 ----
    QPushButton* start_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QLabel* status_label_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QWidget* result_row_ = nullptr;
    QPushButton* open_dir_button_ = nullptr;
    QPushButton* open_media_button_ = nullptr;

    // ---- 运行状态 ----
    /// 解析一次就缓存的 ffmpeg 程序路径（Resolve 要摸 PATH 与常见安装目录，
    /// 而 RebuildPlan 每次输入变化都会调用它，不该每次都去找一遍）。
    QString cached_program_;
    ffmpeg::FfmpegProcessRunner* runner_ = nullptr;
    ffmpeg::FfmpegProgressTracker progress_;
    QStringList current_arguments_;
    QString last_output_path_;
    QString run_stderr_; // 失败时交给 DiagnoseFailure 做"人话翻译"
    bool last_run_ok_ = false;

    // ---- 源探测（独立 runner + 世代号：换文件时让在飞的结果作废）----
    ffmpeg::FfmpegProcessRunner* probe_runner_ = nullptr;
    QTimer* probe_timer_ = nullptr;
    QString probe_buffer_;
    QString probe_path_;
    quint64 probe_generation_ = 0;
    quint64 probe_launch_generation_ = 0;
    ffmpeg::MediaProbeSnapshot source_probe_;

    QString current_media_path_;
};

} // namespace ui
} // namespace videoeye
