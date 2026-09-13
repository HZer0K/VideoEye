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
#include <QProgressDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <memory>

#include "core/player/MediaPlayer.h"
#include "core/analyzer/MediaInfoAnalyzer.h"
#include "core/analyzer/EbmlAnalyzer.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/player/PlayerPanel.h"

namespace videoeye {
namespace ui {

// 主窗口: 菜单 / 侧边栏 / 分析内容栈 / 导出。
//
// 播放相关的一切 (视频区、控制栏、Vulkan、GDI overlay、音频可视化、Raw 序列)
// 已剥离到 ui::PlayerPanel —— VideoEye 的核心定位是视频文件分析, 播放只是辅助
// 定位手段。MainWindow 仍拥有 MediaPlayer (分析侧共用), 以裸指针注入 PlayerPanel。
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
    void OnExportVideoFrames();
    void OnExportVideo();
    void OnExportAudio();

    // 音视频导出
    void OnMediaExportProgress(int percent);
    void OnMediaExportFinished(const QString& output_path);
    void OnMediaExportError(const QString& message);
    // 视频帧导出
    void OnVideoFrameExportProgress(int exported_frames);
    void OnVideoFrameExportFinished(const QString& output_dir);
    void OnVideoFrameExportError(const QString& message);

    // 播放区显隐 (实际逻辑在 PlayerPanel)
    void OnTogglePlayerArea(bool checked);

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
    // WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE: 精确检测窗口拖动模态的开始/结束,
    // 转发给 PlayerPanel (子 widget 收不到顶层窗口消息)。
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

    // 成员变量
    player::MediaPlayer* player_;  // MainWindow 拥有, 以裸指针注入 player_panel_

    // UI组件 - 整体布局
    QWidget* app_bar_;            // 顶部应用栏
    QListWidget* sidebar_;        // 左侧导航栏
    QStackedWidget* content_stack_;  // 内容区堆栈 (媒体信息 + 分析面板各页)
    QSplitter* main_splitter_;    // 水平分割器 (侧栏 | 主内容)
    QSplitter* content_splitter_; // 垂直分割器 (播放模块 | 分析)

    // 播放模块 (视频区 + 控制栏 + Vulkan + 音频可视化; 可整体收起)
    PlayerPanel* player_panel_ = nullptr;

    // 媒体信息
    QTextEdit* mediainfo_text_;   // MediaInfo 媒体信息文本框
    QLabel* current_media_label_; // 顶部显示当前媒体路径
    QLabel* stats_label_;         // 状态栏右端常驻: 实时 FPS/码率/关键帧
    QString current_media_url_;

    // 分析面板
    ui::AnalysisPanel* analysis_panel_;  // 分析面板

    // 菜单和工具栏
    QMenuBar* menu_bar_;
    QStatusBar* status_bar_;
    QAction* export_frames_action_ = nullptr;
    QAction* toggle_player_action_ = nullptr;  // 视图菜单: 显示播放区 (F9)
    QProgressDialog* export_progress_dialog_ = nullptr;
    int export_total_frames_ = 0;

    // 导出类型跟踪 (用于进度对话框取消时调用正确的取消接口)
    enum class ActiveExport { None, Frames, Media };
    ActiveExport active_export_ = ActiveExport::None;
};

} // namespace ui
} // namespace videoeye
