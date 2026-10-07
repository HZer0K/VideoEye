#pragma once

// 「FFmpeg 命令工作台」页左侧下半区的「输出与解释」区：状态行（状态 / 退出码 / 耗时 /
// 复制日志 / 保存日志）+ 三个页签（命令解释 / 运行日志 / 错误输出）。
//
// 从 FfmpegPanel 抽出的独立组件 —— 页面里"命令一改就重算解释""运行输出按到达顺序
// 合并两路、stderr 行加 [err] 前缀""失败自动切到错误页""优雅停止与被强杀分开报"
// 这些规则全压在这里。组件就是 QGroupBox 本身（外面不再包一层 QWidget），保证拆分
// 前后视觉层级不变。
//
// 对外接缝：
//   - 数据进来：RefreshExplanation(command_text) 的命令文本由页面给（组件不持有命令
//     输入框）；运行生命周期由页面驱动（PrepareForRun / AppendLog / ShowRunResult）。
//   - 消息出去：StatusMessage(text) —— 组件不直接写状态栏，由页面统一展示。
//   - 明文日志缓存留在组件内部：复制/保存用的是同一份累积文本，「清空」由页面下指令。

#include <QGroupBox>

#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>

#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegProcessRunner.h"

namespace videoeye {
namespace ui {

class FfmpegOutputWidget : public QGroupBox {
    Q_OBJECT

public:
    explicit FfmpegOutputWidget(QWidget* parent = nullptr);

    /// 按当前命令文本重算「命令解释」；空命令静默清空，解析失败只显示错误说明。
    void RefreshExplanation(const QString& command_text);
    /// 页面的「清空」：日志与状态行回到初始态。
    void ClearOutput();
    /// 新一轮运行开始：清日志、状态置「运行中」、切到「运行日志」页。
    void PrepareForRun();
    /// 一行输出。is_error=true 表示来自 stderr（「运行日志」里加 [err] 前缀）。
    void AppendLog(const QString& text, bool is_error);
    /// 运行结束：回填状态 / 退出码 / 耗时；失败切「错误输出」页并发出 StatusMessage。
    void ShowRunResult(const ffmpeg::FfmpegRunResult& result);

signals:
    void StatusMessage(const QString& text);

private slots:
    void OnExplanationCurrentChanged();
    void OnCopyLog();
    void OnSaveLog();

private:
    void SetupUi();
    /// setText + 颜色一次写完（三处状态更新共用；color 为空 = 清掉样式）
    void SetStatusText(const QString& text, const QString& color);

    // ---- 状态行 ----
    QLabel* status_label_ = nullptr;
    QLabel* exit_code_label_ = nullptr;
    QLabel* elapsed_label_ = nullptr;
    QPushButton* copy_button_ = nullptr;
    QPushButton* save_button_ = nullptr;

    // ---- 三个页签 ----
    QTabWidget* output_tabs_ = nullptr;
    QListWidget* explain_list_ = nullptr;
    QTextBrowser* explain_detail_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QPlainTextEdit* error_view_ = nullptr;

    // ---- 状态 ----
    QString last_log_text_;
    ffmpeg::FfmpegCommandExplanation explanation_;
};

}  // namespace ui
}  // namespace videoeye