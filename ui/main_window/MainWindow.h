#pragma once

#include <QAction>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QString>
#include <QTextEdit>
#include <QTimer>
#include <chrono>
#include <memory>

#include "core/player/MediaPlayerFwd.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/ffmpeg_panel/FfmpegPanel.h"
#include "ui/main_window/ExportCoordinator.h"
#include "ui/main_window/OpenSessionController.h"
#include "ui/main_window/OverviewPage.h"
#include "ui/main_window/macroblock_coordination.h"
#include "ui/main_window/menu_bar.h"
#include "ui/player/PlayerPanel.h"

namespace videoeye {
namespace ui {

class MediaInfoCoordinator;

// 主窗口: 菜单 / 侧边栏 / 分析内容栈。
//
// 播放相关的一切 (视频区、控制栏、音频可视化、Raw 序列)
// 已剥离到 ui::PlayerPanel —— VideoEye 的核心定位是视频文件分析, 播放只是辅助
// 定位手段。MainWindow 仍拥有 MediaPlayer (分析侧共用), 以裸指针注入 PlayerPanel。
// 导出体系 (三个导出入口 + 十条播放器导出信号接线 + 进度框) 剥离到 ui::ExportCoordinator,
// 本窗口只注入两个状态查询并把它的 StatusMessage 转发给状态栏。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    bool OpenMedia(const QString& source, bool autoplay = true);

private slots:
    // 文件菜单
    void OnOpenFile();
    void OnOpenURL();
    void OnExit();

    // 帮助菜单: 支持的格式与编解码器（现场枚举本进程链接的 FFmpeg 能力）
    void OnShowFormatSupport();

    // 播放区显隐 (实际逻辑在 PlayerPanel)
    void OnTogglePlayerArea(bool checked);

private:
    // 初始化UI
    void SetupUI();
    void SetupAppBar();          // 顶部应用栏
    void SetupSidebar();         // 左侧导航栏
    void PopulateSidebarItems(); // 依据 content_stack_ 实际页面生成导航项（须在页面注册完后调用）
    void SetupContentArea();     // 右侧主内容区 (播放模块 + 分析)
    void SetupMenuBar();
    void SetupStatusBar();
    void SetupConnections();
    void OnSidebarChanged(int index); // 侧边栏切换
    // 构造打开会话控制器（分派 / 换媒体复位 / 异步收尾）。须在 SetupUI 之后:
    // 它要接 player_panel_ / overview_page_ / analysis_panel_ 三个此刻才存在的对象。
    void SetupOpenSession();
    // 按 pageId 跳转到内容栈页（首页「跳转到分析页」用）：切 stack 并同步侧栏选中
    void ShowPageById(const QString& page_id);
    // 拉取 AnalysisPanel 的诊断状态快照，刷新首页「分析状态」区
    void RefreshOverviewAnalysis();
    // 把各分析页的异常数 / 级别 / 扫描态写进侧栏 item 的徽标角色，
    // 由 SidebarItemDelegate 画成右侧数字徽标与扫描圆点（见 DiagnosticsStateChanged）
    void UpdateNavigationBadges();
    // 宏块分析的实际采集状态 = "用户主动启用" 或 "MV 叠加需要"，两者由本窗口分别记录。
    // 打开叠加时强制开启（并同步宏块页勾选框），关闭叠加后恢复到用户原先的选择。
    void ApplyMacroblockAnalysisState();
    bool PromptForPcmSettings(QString& demuxer_name, int& sample_rate, int& channels);
    void UpdateMinimumWindowSize();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // 成员变量
    player::MediaPlayer* player_;                   // MainWindow 拥有, 以裸指针注入 player_panel_

    // UI组件 - 整体布局
    QWidget* app_bar_;              // 顶部应用栏
    QListWidget* sidebar_;          // 左侧导航栏
    QStackedWidget* content_stack_; // 内容区堆栈 (媒体信息 + 分析面板各页)
    QSplitter* main_splitter_;      // 水平分割器 (侧栏 | 主内容)
    QSplitter* content_splitter_;   // 垂直分割器 (播放模块 | 分析)

    // 播放模块 (视频区 + 控制栏 + 音频可视化; 可整体收起)
    PlayerPanel* player_panel_ = nullptr;

    // 首页（总览）: 文件摘要 + 分析状态 + 最重要异常 + 完整媒体信息
    OverviewPage* overview_page_ = nullptr;

    // 顶部显示当前媒体路径
    QLabel* current_media_label_;
    QLabel* stats_label_; // 状态栏右端常驻: 实时 FPS/码率/关键帧

    // 打开会话: 分派(裸图像/PCM/通用媒体) + 换媒体复位 + 异步收尾 + 下游登记。
    // 待收尾登记与"当前媒体源"都由它持有 —— 这两个状态只有打开链路在用,
    // 挂在主窗口上会让"谁在改 current_media_url_"变成一笔糊涂账。
    OpenSessionController* open_session_ = nullptr;

    // 分析面板
    ui::AnalysisPanel* analysis_panel_; // 分析面板

    // 宏块分析状态由本窗口协调（见 ApplyMacroblockAnalysisState）：
    // 用户主动开启的分析与 MV 叠加需求分开记录，实际采集状态取两者的或。
    // 协调规则封装在不依赖 Qt 的 MacroblockCoordination 里（可单测）。
    MacroblockCoordination macroblock_;

    // FFmpeg 命令工作台 (原生 ffmpeg 的图形入口; 不依赖当前是否打开了媒体)
    ui::FfmpegPanel* ffmpeg_panel_ = nullptr;

    // 导出协调器: 三个导出入口 + 十条播放器导出信号接线 (构造时自连), 菜单动作
    // 接收者就是它。对话框 parent 与状态查询在构造时注入; 查询 lambda 读本窗口
    // 状态, 协调器不反向持有 MainWindow 的具体成员。
    ExportCoordinator* export_coordinator_ = nullptr;

    // 菜单和工具栏。菜单由 ui::BuildMenuBar 建（结构/快捷键在 menu_bar.h 里），
    // 本窗口只留需要反向同步勾选状态的那几个动作。
    ui::MenuBarActions menu_actions_;
    // 首次 showEvent 时才应用布局预设（此前 splitter 还没有真实尺寸）
    bool layout_preset_applied_ = false;

    // UI 健康度探针 (主线程阻塞检测, 仅用于定位卡顿)
    QTimer* ui_health_timer_ = nullptr;
    std::chrono::steady_clock::time_point ui_health_last_{};

    // 媒体信息后台解析 (避免大文件解析卡住主线程) —— 委托给独立协调器,
    // MainWindow 只贴占位文本并在 InfoReady 信号里刷新文本框本身。
    // unique_ptr 让本头文件不必 include 协调器头；析构点紧随 ~MainWindow 体。
    std::unique_ptr<MediaInfoCoordinator> media_info_coordinator_;
};

} // namespace ui
} // namespace videoeye
