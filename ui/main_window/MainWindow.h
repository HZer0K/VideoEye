#pragma once

#include <QMainWindow>
#include <QWindow>
#include <QPushButton>
#include <QSlider>
#include <QProgressBar>
#include <QTextEdit>
#include <QString>
#include <QTabWidget>
#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QGroupBox>
#include <QSplitter>
#include <QRect>
#include <QProgressDialog>
#include <QElapsedTimer>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QButtonGroup>
#include <QPointer>
#include <deque>
#include <memory>
#include <QImage>

#include "core/player/MediaPlayer.h"
#include "ui/main_window/VulkanVideoWidget.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "core/analyzer/MediaInfoAnalyzer.h"
#include "core/analyzer/EbmlAnalyzer.h"

namespace videoeye {
namespace ui {

// 主窗口类
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
    void OnPrevRawFrame();
    void OnNextRawFrame();
    
    // 播放控制
    void OnPlayPause();
    void OnStop();
    void OnSeek(int value);
    void OnVolumeChanged(int value);
    void OnMuteButtonClicked();
    
    // 播放器信号处理
    void OnStateChanged(model::PlayerState state);
    void OnFrameReady(const QImage& frame);
    void OnPositionChanged(int position_ms, int duration_ms);
    void OnError(const QString& message);
    void OnPlaybackFinished();
    
    // 分析功能槽函数 (MediaPlayer信号)
    void OnStreamStatsUpdate(const analyzer::StreamStats& stats);
    void OnHistogramUpdate(const analyzer::HistogramData& hist);
    void OnMediaModeChanged(bool has_video);
    void OnAudioLevelReady(double level, double timestamp_seconds);
    void OnAudioVisualizationForDisplay(const model::AudioVisualizationFrame& frame);
    void OnVideoFrameExportProgress(int exported_frames);
    void OnVideoFrameExportFinished(const QString& output_dir);
    void OnVideoFrameExportError(const QString& message);

    // 运动矢量叠加
    void OnMvOverlayToggled(bool enabled);
    void OnMacroblockInfoForOverlay(const videoeye::model::MacroblockFrameAnalysis& analysis);

    // 音视频导出
    void OnExportVideo();
    void OnExportAudio();
    void OnMediaExportProgress(int percent);
    void OnMediaExportFinished(const QString& output_path);
    void OnMediaExportError(const QString& message);

private:
    // 初始化UI
    void SetupUI();
    void SetupAppBar();         // 顶部应用栏
    void SetupSidebar();        // 左侧导航栏
    void SetupContentArea();    // 右侧主内容区 (视频+控制+分析)
    void SetupMenuBar();
    void SetupStatusBar();
    void SetupConnections();
    void OnSidebarChanged(int index);  // 侧边栏切换
    void InitVulkan();                 // 创建并挂载 Vulkan 渲染 (失败自动回退 CPU)
    bool LoadRawImageFile(const QString& filename);
    bool PromptForPcmSettings(QString& demuxer_name, int& sample_rate, int& channels);
    bool ShowRawFrame(int frame_index);
    void UpdateRawNavigationState();
    void UpdateMinimumWindowSize();
    void RenderAudioVisualization(double timestamp_seconds);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    // WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE: 精确检测窗口拖动模态的开始/结束,
    // 通知渲染器在拖动期间抑制 swapchain 重建 (时间防抖对慢速拖动无效)
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    
    // 更新UI状态
    void UpdateUIState();
    void ResetVideoUI();  // 重置视频显示UI

    // GDI overlay popup: 拖动模态期间 DWM 对被拖动窗口只显示快照缩放,
    // 视频帧改由解码线程 GDI 直绘到独立顶层 popup 窗口 (覆盖在视频区域上)。
    void ShowGdiOverlayPopup();
    void HideGdiOverlayPopup();
    void UpdateGdiOverlayPopupGeometry();
    
    // 成员变量
    player::MediaPlayer* player_;

    // Vulkan 渲染 (MainWindow 拥有生命周期, 渲染器/上下文与 MediaPlayer 共享)
    std::unique_ptr<player::VulkanContext> vulkan_ctx_;
    std::unique_ptr<player::VulkanRenderer> vulkan_renderer_;
    
    // UI组件 - 整体布局
    QWidget* app_bar_;            // 顶部应用栏
    QListWidget* sidebar_;        // 左侧导航栏
    QStackedWidget* content_stack_;  // 内容区堆栈 (媒体信息 + 分析面板各页)
    QSplitter* main_splitter_;    // 水平分割器 (侧栏 | 主内容)
    QSplitter* content_splitter_; // 垂直分割器 (视频 | 分析)
    
    // 视频区
    VulkanVideoWidget* video_widget_;  // 视频显示 (Vulkan + 回退)

    // GDI overlay popup (拖动模态期间显示视频帧的独立顶层 Win32 窗口)。
    // 纯 Win32 窗口绕开 Qt backing store 绘制管线, 避免与解码线程 GDI 直绘竞争闪屏。
    WId gdi_overlay_hwnd_ = 0;
    
    // 控制栏
    QWidget* control_bar_;        // 播放控制容器
    QSlider* seek_slider_;        // 进度条
    bool slider_dragging_ = false; // 进度条是否正在拖动 (区分拖动预览与键盘跳转)
    qint64 last_drag_seek_ms_ = 0; // 拖动预览节流的上一帧 seek 时间 (ms, 避免每像素都调 av_seek_frame 卡 UI)
    int last_seek_value_ = -1;     // 去重: 上一次真正 seek 的目标值 (避免释放+尾随 valueChanged 双 seek)
    qint64 last_seek_time_ = 0;    // 去重: 上一次真正 seek 的时间 (ms)
    QPushButton* play_pause_button_; // 播放/暂停按钮
    QPushButton* stop_button_;    // 停止按钮
    QPushButton* prev_frame_button_;
    QPushButton* next_frame_button_;
    QLabel* time_label_;          // 时间显示
    QPushButton* volume_button_;  // 音量/静音切换按钮
    QSlider* volume_slider_;      // 音量滑块 (0-100)
    int last_volume_ = 100;       // 静音前的音量 (用于恢复)
    
    // 媒体信息
    QTextEdit* mediainfo_text_;   // MediaInfo 媒体信息文本框
    QLabel* current_media_label_; // 顶部显示当前媒体路径
    QString current_media_url_;
    
    // 分析面板
    ui::AnalysisPanel* analysis_panel_;  // 分析面板
    
    // 菜单和工具栏
    QMenuBar* menu_bar_;
    QStatusBar* status_bar_;
    QAction* export_frames_action_ = nullptr;
    QProgressDialog* export_progress_dialog_ = nullptr;
    int export_total_frames_ = 0;

    // 导出类型跟踪 (用于进度对话框取消时调用正确的取消接口)
    enum class ActiveExport { None, Frames, Media };
    ActiveExport active_export_ = ActiveExport::None;

    bool audio_only_mode_ = false;
    QImage album_cover_;
    std::deque<double> audio_level_history_;
    std::deque<double> spectrum_history_;
    QElapsedTimer audio_vis_timer_;
    qint64 audio_vis_last_render_ms_ = -1;
    double audio_vis_smoothed_ = 0.0;
    double audio_vis_target_ = 0.0;
    QVector<double> latest_spectrum_bins_;
    QVector<double> latest_waveform_points_;
    QVector<double> smoothed_spectrum_bins_;
    bool showing_raw_image_ = false;
    QString raw_image_path_;
    QString raw_pixel_format_;
    int raw_width_ = 0;
    int raw_height_ = 0;
    qint64 raw_frame_size_ = 0;
    int raw_total_frames_ = 0;
    int raw_current_frame_ = 0;
    
    // 叠加层缓存数据
    QString last_overlay_fps_;
    QString last_overlay_resolution_;
    QString last_overlay_codec_;

    // 运动矢量叠加
    QPushButton* mv_overlay_button_ = nullptr;  // MV 叠加开关 (checkable)
    bool mv_overlay_enabled_ = false;
};

} // namespace ui
} // namespace videoeye
