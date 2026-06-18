#pragma once

#include <QMainWindow>
#include <QWindow>
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
#include <QImage>

#include "core/player/MediaPlayer.h"
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
    
private:
    // 初始化UI
    void SetupUI();
    void SetupMenuBar();
    void SetupToolBar();
    void SetupStatusBar();
    void SetupConnections();
    bool LoadRawImageFile(const QString& filename);
    bool PromptForPcmSettings(QString& demuxer_name, int& sample_rate, int& channels);
    bool ShowRawFrame(int frame_index);
    void UpdateRawNavigationState();
    void UpdateMinimumWindowSize();
    void RenderAudioVisualization(double timestamp_seconds);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    
    // 更新UI状态
    void UpdateUIState();
    void ResetVideoUI();  // 重置视频显示UI
    
    // 成员变量
    player::MediaPlayer* player_;
    
    // UI组件
    QLabel* video_label_;           // 视频显示
    QSplitter* splitter_;           // 主分割器
    QWidget* bottom_widget_;        // 下半区容器
    QGroupBox* control_group_;      // 播放控制容器
    QTabWidget* tab_widget_;        // 标签页
    QSlider* seek_slider_;          // 进度条
    QPushButton* play_pause_button_; // 播放/暂停按钮
    QPushButton* stop_button_;      // 停止按钮
    QPushButton* prev_frame_button_;
    QPushButton* next_frame_button_;
    QLabel* time_label_;            // 时间显示
    QTextEdit* mediainfo_text_;     // MediaInfo 媒体信息文本框
    QLabel* current_media_label_;   // 顶部显示当前媒体路径
    QString current_media_url_;
    
    // 分析面板
    ui::AnalysisPanel* analysis_panel_;  // 分析面板
    
    // 菜单和工具栏
    QMenuBar* menu_bar_;
    QToolBar* tool_bar_;
    QStatusBar* status_bar_;
    QAction* export_frames_action_ = nullptr;
    QProgressDialog* export_progress_dialog_ = nullptr;
    int export_total_frames_ = 0;

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
};

} // namespace ui
} // namespace videoeye
