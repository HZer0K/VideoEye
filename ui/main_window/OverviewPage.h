#pragma once

// 首页（总览页）：把"打开一个文件后该看什么"收在一屏里。
//
// 原来的 Page 0 是一段不可换行的媒体信息纯文本 —— 用户打开文件后仍然不知道该
// 先去哪个分析页、这次扫描有没有问题。本页按"先回答结论，再回答细节"排：
//   1. 素材身份：文件名 + 完整路径 + 状态徽章（这一屏在说什么文件、扫到哪了）；
//   2. 分析状态：评分/结论/覆盖情况 + 问题计数 + 最重要的几条异常。
//      —— 异常排在文件摘要之前：用户先要知道"有没有问题、点哪里"，
//         容器/码率这些参数是"看完结论再核对"的次级信息；
//   3. 文件摘要：容器/时长/大小/码率 + 视频/音频一行式概要（来自 StreamInfo）；
//   4. 完整媒体信息：原来的纯文本整段保留，**默认折叠**（它很长，常驻展开会把
//      上面三块挤出首屏）。
//
// 各分析页的入口就是侧边栏（与本页异常条目跳转二选一即可），首页不再重复铺一版
// 跳转按钮。
//
// 数据全部由 MainWindow 喂进来（本页不持有 AnalysisPanel / MediaPlayer），
// 跳转意图以 NavigateToIssue 信号外抛（带问题身份，由 MainWindow 转给诊断页选中
// 对应那一行），由 MainWindow 切 stack + 侧栏。

#include <QString>
#include <QVector>
#include <QWidget>

#include "core/domain/model/DiagnosticIssue.h" // model::DiagnosticIssue / IssueSeverity
#include "core/domain/model/FrameData.h"       // model::StreamInfo
#include "ui/main_window/overview_analysis.h"  // OverviewAnalysisSummary

class QGroupBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTextEdit;

namespace videoeye {
namespace ui {

class OverviewPage : public QWidget {
    Q_OBJECT

public:
    explicit OverviewPage(QWidget* parent = nullptr);

    // 是否已登记了打开的文件（MainWindow 刷新分析状态时要带上，未打开时只显示空态）
    bool HasFile() const {
        return summary_.has_file;
    }

    // 完整媒体信息文本（FFmpeg 探测结果 / Raw 图像详情 / PCM 详情）
    void SetMediaInfoText(const QString& text);

    // 文件摘要：打开成功时由 MainWindow 从 player_->GetStreamInfo() 喂入；
    // 打开失败/未打开文件时调 ClearFileSummary()。
    void SetFileSummary(const model::StreamInfo& info);
    // 没有 StreamInfo 可用的打开路径（裸 PCM / Raw 图像）：只登记文件名，
    // 摘要各栏留「—」，媒体信息文本仍由 SetMediaInfoText 正常显示。
    void SetOpenedFileWithoutSummary(const QString& path);
    void ClearFileSummary();

    // 分析状态摘要。MainWindow 在扫描起止、报告变化、换文件时拉取
    // AnalysisPanel 的只读快照后调用；has_file=false 表示没有打开文件。
    //
    // 结构本体已移到 ui/main_window/overview_analysis.h（连同组装口径
    // BuildOverviewAnalysisSummary 一起），因为「什么算失败」「异常按什么顺序取前几条」
    // 是有边界的业务口径，住在 QWidget 里测不到。这里保留原名做别名，
    // 页面自己的绘制代码不用跟着改名。
    using AnalysisSummary = OverviewAnalysisSummary;
    void SetAnalysisSummary(const AnalysisSummary& summary);

signals:
    // 跳到「质量诊断」并直接定位到某条异常（首页异常表的点击意图）。
    // 带的是问题身份而不是行号：首页那份 top_issues 是按严重度重排过的**副本**，
    // 下标与报告里的 issues 并不对应，传下标会让用户落在另一条问题上
    // （这条 bug 只看下标永远发现不了，因为两边行数一样）。
    void NavigateToIssue(const QString& rule_id, double start_seconds, int stream_index);
    // 请求重新跑一次全文件扫描（由 MainWindow 转交诊断页）
    void RescanRequested();

private:
    void BuildUi();
    void RefreshUi();
    static QString SeverityColor(model::IssueSeverity severity);
    static QString FormatPosition(const model::DiagnosticIssue& issue);

    // 文件摘要
    QLabel* file_name_label_ = nullptr;
    QLabel* file_path_label_ = nullptr;
    QLabel* state_badge_ = nullptr;
    QGroupBox* summary_group_ = nullptr;
    QLabel* summary_container_ = nullptr;
    QLabel* summary_duration_ = nullptr;
    QLabel* summary_size_ = nullptr;
    QLabel* summary_bitrate_ = nullptr;
    QLabel* summary_video_ = nullptr;
    QLabel* summary_audio_ = nullptr;

    // 分析状态
    QGroupBox* analysis_group_ = nullptr;
    QLabel* empty_hint_ = nullptr;
    QLabel* verdict_label_ = nullptr;      // 富文本：评分 + 结论 + 分级计数
    QLabel* error_label_ = nullptr;        // 失败原因
    QTableWidget* issues_table_ = nullptr; // 最重要的异常（点击跳质量诊断页）
    QPushButton* rescan_button_ = nullptr;

    // 完整媒体信息
    QGroupBox* mediainfo_group_ = nullptr;
    QTextEdit* mediainfo_text_ = nullptr;

    AnalysisSummary summary_;
};

} // namespace ui
} // namespace videoeye
