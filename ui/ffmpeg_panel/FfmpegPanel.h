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

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBrowser>
#include <QWidget>

#include <QString>
#include <QStringList>
#include <vector>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegProcessRunner.h"
#include "core/ffmpeg/FfmpegToolLocator.h"

namespace videoeye {
namespace ui {

class FfmpegPanel : public QWidget {
    Q_OBJECT

public:
    explicit FfmpegPanel(QWidget* parent = nullptr);
    ~FfmpegPanel() override;

signals:
    void StatusMessage(const QString& text);

public slots:
    /// 主窗口打开/切换媒体时同步过来，供「插入当前媒体路径」使用
    void SetCurrentFile(const QString& path);

private slots:
    void OnBrowseTool();
    void OnResetTool();
    void OnToolPathEdited();
    void OnShowInstallGuide();
    void OnOpenDownloadPage();
    void OnRedetectTool();
    void OnInsertInputPath();
    void OnInsertOutputPath();
    void OnClearCommand();
    void OnRun();
    void OnStop();
    void OnCopyLog();
    void OnSaveLog();
    void OnCommandTextChanged();

    void OnRunOutput(const QString& text, bool is_error);
    void OnRunFinished(const ffmpegtool::FfmpegRunResult& result);

    void OnProbeOutput(const QString& text, bool is_error);
    void OnProbeFinished(const ffmpegtool::FfmpegRunResult& result);

    void OnSearchTextChanged();
    void OnCategoryChanged();
    void OnDictionaryCurrentChanged();
    void OnInsertEntry();
    void OnExplanationCurrentChanged();

private:
    void BuildUi();
    QWidget* BuildCommandArea();
    QWidget* BuildOutputArea();
    QWidget* BuildDictionaryArea();

    void RefreshToolInfo();
    void StartProbes();
    void RunNextProbe();
    /// 拼装"没找到 ffmpeg"时的安装指引对话框正文（纯文本，便于复制）
    QString BuildInstallGuideText() const;

    void RefreshDictionary();
    void ShowEntryDetail(const ffmpegtool::FfmpegCatalogEntry* entry);
    void RefreshExplanation();
    void AppendLog(const QString& text, bool is_error);
    void SetRunUiState(bool running);
    QString CurrentToolPath() const;

    // ---- 程序区 ----
    QLineEdit* tool_path_edit_ = nullptr;
    QLabel* tool_status_label_ = nullptr;
    QPushButton* browse_button_ = nullptr;
    QPushButton* reset_button_ = nullptr;
    QWidget* tool_missing_bar_ = nullptr;    // 只在找不到 ffmpeg 时出现的补救入口
    QPushButton* guide_button_ = nullptr;
    QPushButton* download_button_ = nullptr;
    QPushButton* redetect_button_ = nullptr;

    // ---- 命令区 ----
    QLineEdit* command_edit_ = nullptr;
    QPushButton* insert_input_button_ = nullptr;
    QPushButton* insert_output_button_ = nullptr;
    QPushButton* run_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;

    // ---- 输出区 ----
    QTabWidget* output_tabs_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QPlainTextEdit* error_view_ = nullptr;
    QListWidget* explain_list_ = nullptr;
    QTextBrowser* explain_detail_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* exit_code_label_ = nullptr;
    QLabel* elapsed_label_ = nullptr;
    QPushButton* copy_button_ = nullptr;
    QPushButton* save_button_ = nullptr;

    // ---- 字典区 ----
    QLineEdit* search_edit_ = nullptr;
    QComboBox* category_combo_ = nullptr;
    QListWidget* dict_list_ = nullptr;
    QTextBrowser* dict_detail_ = nullptr;
    QPushButton* dict_insert_button_ = nullptr;

    // ---- 状态 ----
    ffmpegtool::FfmpegProcessRunner* runner_ = nullptr;
    ffmpegtool::FfmpegProcessRunner* probe_runner_ = nullptr;
    QStringList probe_queue_;
    QString probe_current_;
    QString probe_buffer_;

    ffmpegtool::FfmpegToolInfo tool_info_;
    QString tool_version_;
    QString current_media_path_;
    QString last_log_text_;
    std::vector<const ffmpegtool::FfmpegCatalogEntry*> dict_results_;
    ffmpegtool::FfmpegCommandExplanation explanation_;
    QSettings settings_;
};

}  // namespace ui
}  // namespace videoeye
