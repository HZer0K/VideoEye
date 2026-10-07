#pragma once

#include <QMainWindow>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QString>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QAction>
#include <QSplitter>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QTimer>
#include <chrono>
#include <memory>

#include "core/player/MediaPlayer.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/ffmpeg_panel/FfmpegPanel.h"
#include "ui/main_window/ExportCoordinator.h"
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

    // 播放区显隐 (实际逻辑在 PlayerPanel)
    void OnTogglePlayerArea(bool checked);

    // 事务式异步打开完成 (MediaPlayer::OpenFinished)。ok=true 表示已提交进播放会话。
    void OnPlayerOpenFinished(bool ok);

private:
    // 初始化UI
    void SetupUI();
    void SetupAppBar();         // 顶部应用栏
    void SetupSidebar();        // 左侧导航栏
    void PopulateSidebarItems();  // 依据 content_stack_ 实际页面生成导航项（须在页面注册完后调用）
    void SetupContentArea();    // 右侧主内容区 (播放模块 + 分析)
    void SetupMenuBar();
    void SetupStatusBar();
    void SetupConnections();
    void OnSidebarChanged(int index);  // 侧边栏切换
    bool PromptForPcmSettings(QString& demuxer_name, int& sample_rate, int& channels);
    void UpdateMinimumWindowSize();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // 成员变量
    player::MediaPlayer* player_;  // MainWindow 拥有, 以裸指针注入 player_panel_

    // UI组件 - 整体布局
    QWidget* app_bar_;            // 顶部应用栏
    QListWidget* sidebar_;        // 左侧导航栏
    QStackedWidget* content_stack_;  // 内容区堆栈 (媒体信息 + 分析面板各页)
    QSplitter* main_splitter_;    // 水平分割器 (侧栏 | 主内容)
    QSplitter* content_splitter_; // 垂直分割器 (播放模块 | 分析)

    // 播放模块 (视频区 + 控制栏 + 音频可视化; 可整体收起)
    PlayerPanel* player_panel_ = nullptr;

    // 媒体信息
    QTextEdit* mediainfo_text_;   // 媒体信息文本框 (FFmpeg 解析结果)
    QLabel* current_media_label_; // 顶部显示当前媒体路径
    QLabel* stats_label_;         // 状态栏右端常驻: 实时 FPS/码率/关键帧
    QString current_media_url_;
    // 异步打开的收尾信息: OpenAsync 发起时登记, OpenFinished 回来时消费。
    // 过期结果由 MediaPlayer 侧丢弃 (不发信号), 这里只需记住最近一次请求。
    QString pending_open_source_;
    bool pending_open_autoplay_ = false;

    // 分析面板
    ui::AnalysisPanel* analysis_panel_;  // 分析面板

    // FFmpeg 命令工作台 (原生 ffmpeg 的图形入口; 不依赖当前是否打开了媒体)
    ui::FfmpegPanel* ffmpeg_panel_ = nullptr;

    // 导出协调器: 三个导出入口 + 十条播放器导出信号接线 (构造时自连), 菜单动作
    // 接收者就是它。对话框 parent 与状态查询在构造时注入; 查询 lambda 读本窗口
    // 状态, 协调器不反向持有 MainWindow 的具体成员。
    ExportCoordinator* export_coordinator_ = nullptr;

    // 菜单和工具栏
    QMenuBar* menu_bar_;
    QStatusBar* status_bar_;
    QAction* export_frames_action_ = nullptr;
    QAction* toggle_player_action_ = nullptr;  // 视图菜单: 显示播放区 (F9)

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
