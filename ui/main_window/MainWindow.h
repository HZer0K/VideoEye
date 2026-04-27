#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QProgressBar>
#include <QTextEdit>
#include <QString>
#include <QTabWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QGroupBox>
#include <QSplitter>
#include <QRect>
#include <QProgressDialog>
#include <QElapsedTimer>
#include <deque>

#include "core/player/MediaPlayer.h"
#include "ui/analysis_panel/AnalysisPanel.h"

namespace videoeye {
namespace ui {

// 主窗口类
class MainWindow : public QMainWindow {
    Q_OBJECT
    
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();
    
private slots:
    // 文件菜单
    void OnOpenFile();
    void OnOpenURL();
    void OnExit();
    void OnExportVideoFrames();
    void OnPrevRawFrame();
    void OnNextRawFrame();
    
    // 播放控制
    void OnPlay();
    void OnPause();
    void OnStop();
    void OnSeek(int value);
    
    // 播放器信号处理
    void OnStateChanged(model::PlayerState state);
    void OnFrameReady(const QImage& frame);
    void OnPositionChanged(int position_ms, int duration_ms);
    void OnError(const QString& message);
    void OnPlaybackFinished();
    
    // 分析功能槽函数 (MediaPlayer信号)
    void OnStreamStatsUpdate(const analyzer::StreamStats& stats);
    void OnHistogramUpdate(const analyzer::HistogramData& hist);
    void OnFaceDetectionUpdate(const std::vector<analyzer::FaceInfo>& faces);
    void OnMediaModeChanged(bool has_video);
    void OnAudioLevelReady(double level, double timestamp_seconds);
    void OnVideoFrameExportProgress(int exported_frames);
    void OnVideoFrameExportFinished(const QString& output_dir);
    void OnVideoFrameExportError(const QString& message);
    
private:
    // 初始化UI
    void SetupUI();
    void SetupMenuBar();
    void SetupToolBar();
    void SetupStatusBar();
    void SetupConnections();
    bool LoadRawImageFile(const QString& filename);
    bool ShowRawFrame(int frame_index);
    void UpdateRawNavigationState();
    void UpdateMinimumWindowSize();
    void EnforceSplitterSizes();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    
    // 更新UI状态
    void UpdateUIState();
    
    // 成员变量
    player::MediaPlayer* player_;
    
    // UI组件
    QLabel* video_label_;           // 视频显示
    QSplitter* splitter_;           // 主分割器
    QWidget* bottom_widget_;        // 下半区容器
    QGroupBox* control_group_;      // 播放控制容器
    QTabWidget* tab_widget_;        // 标签页
    QSlider* seek_slider_;          // 进度条
    QPushButton* play_button_;      // 播放按钮
    QPushButton* pause_button_;     // 暂停按钮
    QPushButton* stop_button_;      // 停止按钮
    QPushButton* prev_frame_button_;
    QPushButton* next_frame_button_;
    QLabel* time_label_;            // 时间显示
    QTextEdit* info_text_;          // 详细信息文本框
    QLabel* current_media_label_;   // 顶部显示当前媒体路径
    QString current_media_url_;
    
    // 分析面板
    ui::AnalysisPanel* analysis_panel_;  // 分析面板
    
    // 菜单和工具栏
    QMenuBar* menu_bar_;
    QToolBar* tool_bar_;
    QStatusBar* status_bar_;
    QAction* stream_analysis_action_ = nullptr;
    QAction* frame_analysis_action_ = nullptr;
    QAction* histogram_action_ = nullptr;
    QAction* face_detection_action_ = nullptr;
    QAction* export_frames_action_ = nullptr;
    QProgressDialog* export_progress_dialog_ = nullptr;
    int export_total_frames_ = 0;

    QRect last_geometry_;
    bool enforcing_geometry_ = false;
    bool audio_only_mode_ = false;
    std::deque<double> audio_level_history_;
    QElapsedTimer audio_vis_timer_;
    qint64 audio_vis_last_render_ms_ = -1;
    double audio_vis_smoothed_ = 0.0;
    double audio_vis_target_ = 0.0;
    bool showing_raw_image_ = false;
    QString raw_image_path_;
    QString raw_pixel_format_;
    int raw_width_ = 0;
    int raw_height_ = 0;
    qint64 raw_frame_size_ = 0;
    int raw_total_frames_ = 0;
    int raw_current_frame_ = 0;
};

} // namespace ui
} // namespace videoeye
