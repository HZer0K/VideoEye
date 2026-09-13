#pragma once

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QSplitter>
#include <QElapsedTimer>
#include <QImage>
#include <QVector>
#include <QString>
#include <deque>
#include <memory>

#include "core/player/MediaPlayer.h"
#include "core/player/VulkanContext.h"
#include "core/player/VulkanRenderer.h"
#include "core/analyzer/StreamAnalyzer.h"
#include "core/model/MacroblockInfo.h"
#include "ui/main_window/VulkanVideoWidget.h"

namespace videoeye {
namespace ui {

// 播放区模块 (视频显示 + 控制栏 + Vulkan/GDI 渲染 + 音频可视化 + Raw 序列)。
//
// VideoEye 的核心定位是视频文件分析, 播放只是辅助定位手段, 因此播放相关的一切
// 从 MainWindow 中剥离到此处, MainWindow 只保留菜单/侧边栏/分析栈/导出。
//
// 职责边界:
//   - 拥有: 视频 widget、控制栏、VulkanContext/VulkanRenderer、GDI overlay popup
//   - 不拥有: MediaPlayer (分析侧共用, 生命周期归 MainWindow), 只持裸指针
//   - 与 MainWindow 通信一律走信号 (状态栏/错误提示), 不反向持有主窗口指针
//
// 已知约束:
//   - nativeEvent 只有顶层窗口能收到 WM_ENTERSIZEMOVE, 子 widget 收不到,
//     因此拖动状态由 MainWindow 通过 OnDragStateChanged() 转发进来。
class PlayerPanel : public QWidget {
    Q_OBJECT

public:
    explicit PlayerPanel(QWidget* parent = nullptr);
    ~PlayerPanel() override;

    // 依赖注入: MediaPlayer 由 MainWindow 拥有, 此处仅取裸指针
    void SetMediaPlayer(player::MediaPlayer* player);

    // Vulkan: 创建 context/renderer 并挂到 video_widget_ 与 MediaPlayer。
    // 真正的初始化延迟到 video_widget_ 首次 showEvent (见 InitVulkan 注释)。
    bool InitVulkan();
    void TryInitializeVulkan();

    // 播放区所在的分割器: 收起/展开时保存与还原上下比例
    void SetSplitter(QSplitter* splitter) { splitter_ = splitter; }

    // 播放区显隐 (VideoEye 以分析为主, 可收起给分析区让出空间)
    bool IsPlayerAreaVisible() const;
    void SetPlayerAreaVisible(bool visible, bool persist = true);
    void RestoreVisibility();

    // 供 MainWindow::UpdateMinimumWindowSize 计算窗口最小高度
    int MinimumHeightHint() const;

    // === 供 MainWindow 调用的业务接口 ===
    void StopPlayback();                       // 清理 UI 状态 + player_->Stop()
    void ResetVideoUI();
    bool LoadRawImageFile(const QString& filename);
    void SetRawImageMode(bool on);             // 打开 raw/pcm/媒体时同步模式
    bool IsShowingRawImage() const { return showing_raw_image_; }
    void SetMvOverlayEnabled(bool enabled);    // 宏块分析关闭时联动取消勾选
    // 当前媒体源: 停止后再次点播放需要重新 Open, 由 MainWindow 在 OpenMedia 时同步。
    // 同时负责换源时清掉上一份文件的叠加层缓存与状态栏统计。
    void SetCurrentSource(const QString& source);

signals:
    void StatusMessage(const QString& text, int timeout);
    void ErrorMessage(const QString& text);
    void VisibilityChanged(bool visible);
    void RawImageInfoReady(const QString& info);   // 写入媒体信息文本框
    // 实时码流统计文本 (FPS/码率/关键帧), 供 MainWindow 显示在状态栏常驻区。
    // 不用 StatusMessage 是因为后者走 showMessage 会被临时提示冲掉, 而统计需要常驻。
    // 传空串表示清空 (停止/换源)。
    void StreamStatsDisplayReady(const QString& text);

public slots:
    // MainWindow::nativeEvent 转发 (子 widget 收不到 WM_ENTERSIZEMOVE 等顶层消息)
    void OnDragStateChanged(bool dragging);   // 拖动模态开始/结束
    void OnWindowPosChanged();                // 拖动中几何变化: 更新 popup 并立即刷新
    void OnMacroblockInfoForOverlay(const model::MacroblockFrameAnalysis& analysis);
    void OnStreamStatsUpdate(const analyzer::StreamStats& stats);

private slots:
    // 播放控制
    void OnPlayPause();
    void OnStop();
    void OnSeek(int value);
    void OnVolumeChanged(int value);
    void OnMuteButtonClicked();
    void OnPrevRawFrame();
    void OnNextRawFrame();

    // MediaPlayer 信号
    void OnStateChanged(model::PlayerState state);
    void OnFrameReady(const QImage& frame);
    void OnPositionChanged(int position_ms, int duration_ms);
    void OnMediaModeChanged(bool has_video);
    void OnError(const QString& message);
    // 打开阶段失败: 不弹模态框, 只在状态栏提示 (文件仍会进入分析模块展示错误原因)
    void OnOpenFailed(const QString& message);
    void OnPlaybackFinished();
    void OnAudioLevelReady(double level, double timestamp_seconds);
    void OnAudioVisualizationForDisplay(const model::AudioVisualizationFrame& frame);

    // 运动矢量叠加
    void OnMvOverlayToggled(bool enabled);

private:
    void SetupUI();
    void SetupConnections();

    // GDI overlay popup: 拖动模态期间 DWM 只对被拖动窗口显示快照缩放,
    // 视频帧改由解码线程 GDI 直绘到独立顶层 popup 窗口覆盖显示。
    void ShowGdiOverlayPopup();
    void HideGdiOverlayPopup();
    void UpdateGdiOverlayPopupGeometry();

    // 集中构造并下发叠加层信息 (状态/分辨率/编码/FPS), 避免多处各拼一份导致字段漏传。
    void UpdateOverlay();
    // 刷新分辨率与编码: 编码从 StreamInfo 取一次即缓存, 分辨率随实际帧尺寸变化更新。
    void RefreshOverlayMediaInfo(const QImage* frame = nullptr);

    bool ShowRawFrame(int frame_index);
    void UpdateRawNavigationState();
    void RenderAudioVisualization(double timestamp_seconds);

    // === 非拥有依赖 ===
    player::MediaPlayer* player_ = nullptr;
    QSplitter* splitter_ = nullptr;   // 非拥有: 由 MainWindow 创建的 content_splitter_

    // === 拥有: Vulkan 渲染 (失败时自动回退 CPU) ===
    std::unique_ptr<player::VulkanContext> vulkan_ctx_;
    std::unique_ptr<player::VulkanRenderer> vulkan_renderer_;

    // === UI ===
    VulkanVideoWidget* video_widget_ = nullptr;
    QWidget* control_bar_ = nullptr;
    QPushButton* play_pause_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QPushButton* prev_frame_button_ = nullptr;
    QPushButton* next_frame_button_ = nullptr;
    QPushButton* volume_button_ = nullptr;
    QSlider* volume_slider_ = nullptr;
    QPushButton* mv_overlay_button_ = nullptr;
    QPushButton* collapse_player_button_ = nullptr;
    QSlider* seek_slider_ = nullptr;
    QLabel* time_label_ = nullptr;

    // 进度条交互状态
    bool slider_dragging_ = false;
    qint64 last_drag_seek_ms_ = 0;
    int last_seek_value_ = -1;
    qint64 last_seek_time_ = 0;
    int last_volume_ = 100;

    // GDI overlay popup (纯 Win32 顶层窗口, 绕开 Qt backing store 绘制管线)
    WId gdi_overlay_hwnd_ = 0;

    // 当前媒体源 (停止后再次点播放需重新 Open)
    QString current_source_;

    // 模式标志
    bool audio_only_mode_ = false;
    bool showing_raw_image_ = false;
    bool mv_overlay_enabled_ = false;

    // 叠加层缓存
    QString last_overlay_fps_;
    QString last_overlay_resolution_;
    QString last_overlay_codec_;
    QString last_status_text_;   // OnStateChanged 生成, UpdateOverlay 复用
    model::PlayerState last_known_state_ = model::PlayerState::Stopped;

    // Raw 序列
    QString raw_image_path_;
    QString raw_pixel_format_;
    int raw_width_ = 0;
    int raw_height_ = 0;
    qint64 raw_frame_size_ = 0;
    int raw_total_frames_ = 0;
    int raw_current_frame_ = 0;

    // 音频可视化状态
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

    // 显隐状态
    QList<int> saved_splitter_sizes_;
    // 显式状态标志: 构造期 widget 的 isVisible() 恒为 false (父窗口尚未 show),
    // 直接用 isVisible() 会导致启动时恢复"收起"被误判为无变化而跳过渲染抑制。
    bool player_area_visible_ = true;
};

} // namespace ui
} // namespace videoeye
