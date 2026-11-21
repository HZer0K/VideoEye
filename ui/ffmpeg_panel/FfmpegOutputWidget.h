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
//     输入框）；运行生命周期由页面驱动（PrepareForRun / AppendLog / ShowProgress /
//     ShowRunResult）。
//   - 消息出去：StatusMessage(text) —— 组件不直接写状态栏，由页面统一展示。
//   - 明文日志缓存留在组件内部：复制/保存用的是同一份累积文本，「清空」由页面下指令。
//     过滤只作用于两个日志页签的显示，不改这份缓存。
//
// 日志是**攒批**刷进视图的（50ms 一次）：-loglevel debug 下 ffmpeg 一秒能吐上百行，
// 每行都 appendPlainText + 滚到底会让界面持续抖动。要立刻看到已写入的内容
// （结束、复制、保存、测试）就先调 FlushLog()。
//
// 过滤要能"撤回"，所以行同时进两个地方：视图（当前显示）与历史（全量）。
// 只往视图里追加是不够的 —— 敲一个字母就再也变不回来。

#include <QElapsedTimer>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTextBrowser>

#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegProcessRunner.h"
#include "core/ffmpeg/FfmpegProgressTracker.h"

class QTimer;

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
    /// VideoEye 自己的一句话（不是 ffmpeg 输出，不加 [err] 前缀）：同时写进「运行日志」
    /// 与「错误输出」——失败后用户正停在错误页，结论要在他眼皮底下，而不是另一个页签里。
    /// **不受日志过滤影响**：过滤词是上一轮留下的，不能让它把结论也筛掉。
    void AppendNotice(const QString& text);
    /// 运行中刷新「进度」：百分比 / 速度 / 剩余时间在状态行，帧率与已写入大小在悬停提示。
    /// 总时长未知（流式输入）时只有已处理时长，不编百分比。
    void ShowProgress(const ffmpeg::FfmpegProgressSnapshot& snapshot);
    /// 运行结束：回填状态 / 退出码 / 耗时；失败切「错误输出」页并发出 StatusMessage。
    void ShowRunResult(const ffmpeg::FfmpegRunResult& result);
    /// 把攒着的日志立刻写进视图（结束 / 复制 / 保存前必须调，否则会漏最后几行）。
    void FlushLog();
    /// 日志过滤：子串匹配、不区分大小写；空串 = 不过滤（视图按全量历史重建）。
    /// 只影响两个日志页签的**显示**，复制 / 保存拿到的仍是完整日志。
    void SetLogFilter(const QString& text);

signals:
    void StatusMessage(const QString& text);

private slots:
    void OnExplanationCurrentChanged();
    void OnCopyLog();
    void OnSaveLog();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void SetupUi();
    /// setText + 颜色一次写完（三处状态更新共用；color 为空 = 清掉样式）
    void SetStatusText(const QString& text, const QString& color);
    /// 进度标签的默认悬停说明（百分比与剩余时间的口径）。
    static QString ProgressHint();
    /// 当前生效的过滤词（已 trim）。
    QString ActiveFilter() const;
    /// 按当前过滤词把两个日志页签整体重建（改过滤词、历史被裁剪时用）。
    void ApplyLogFilter();
    /// 过滤计数（匹配 N / 共 M 行）跟随当前页签刷新。
    void UpdateFilterCount();
    /// 把命中位置标成高亮；只作用于视图现有内容。
    void RefreshHighlights();

    // ---- 状态行 ----
    QLabel* status_label_ = nullptr;
    QLabel* progress_label_ = nullptr;
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

    // ---- 日志过滤（只在两个日志页签上出现；命令解释页没有日志可过滤）----
    QWidget* filter_bar_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QLabel* filter_count_label_ = nullptr;
    QTimer* filter_debounce_ = nullptr; // 逐字输入时不必每个字符都重建 20000 行

    // ---- 日志攒批 ----
    QTimer* log_flush_timer_ = nullptr; // 50ms 单次触发
    QStringList pending_log_lines_;     // 「运行日志」待写入的行（含 [err] 前缀）
    QStringList pending_error_lines_;   // 「错误输出」待写入的行（原始文本）
    QElapsedTimer run_elapsed_;         // 运行中刷新「耗时」用，终值以结果里的为准

    // ---- 日志历史：过滤要能"撤回"，就不能只往视图里追加 ----
    // 视图有条数上限，历史跟着对齐；两者分开是为了过滤时能整体重建。
    QStringList log_history_;
    QStringList error_history_;
    int log_matched_ = 0;   // 过滤生效时「运行日志」的命中行数（增量维护，不每次重扫）
    int error_matched_ = 0; // 同上，对应「错误输出」

    // ---- 状态 ----
    QString last_log_text_;
    ffmpeg::FfmpegCommandExplanation explanation_;
};

} // namespace ui
} // namespace videoeye