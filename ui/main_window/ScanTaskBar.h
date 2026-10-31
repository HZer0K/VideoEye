#pragma once

// 扫描任务条：全文件扫描期间的跨页面控制条。
//
// 为什么需要它：扫描由「质量诊断」页编排，取消按钮也在那一页。用户在首页看到
// "分析中…"，文案却要他"去那一页取消"——一次任务的状态和控制分居两页，
// 于是任何页面上的等待都变成了"切过去找按钮"。这条常驻在应用栏下方，
// 任意页面都能看见阶段 / 进度，并且就地取消。
//
// 与 DiagnosticsPage 的关系：本控件只认"一次扫描的状态快照"，不知道扫描是谁在跑，
// 取消也只发信号（CancelRequested），由 MainWindow 转给面板。这样首页、诊断页、
// 报告页看到的是同一份状态，不会再出现"两个地方各说各话"。

#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;

namespace videoeye {
namespace ui {

class ScanTaskBar : public QWidget {
    Q_OBJECT

public:
    explicit ScanTaskBar(QWidget* parent = nullptr);

    // 扫描状态快照。running=false 时整条隐藏（并清掉"取消中"标记）。
    // 阶段文案为空时兜一个通用文案 —— 空白条比"正在分析"更容易被当成卡死。
    void SetScanState(bool running, double percent, const QString& stage);

    // 是否处于"取消已请求、后台还在收尾"。期间进度与阶段文案不再被覆盖，
    // 取消按钮保持置灰 —— 让用户看得出"请求已经被接住了"。
    bool IsCancelling() const {
        return cancelling_;
    }

signals:
    // 用户点了取消。只发意图，不在这里假装终态（终态由扫描真正的结束事件驱动）。
    void CancelRequested();

private:
    void OnCancelClicked();

    QLabel* stage_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    bool cancelling_ = false;
};

} // namespace ui
} // namespace videoeye
