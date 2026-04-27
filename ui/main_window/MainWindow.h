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
#include <QGroupBox>
#include <QSplitter>
#include <QRect>

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
    
private:
    // 初始化UI
    void SetupUI();
    void SetupMenuBar();
    void SetupToolBar();
    void SetupStatusBar();
    void SetupConnections();
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

    QRect last_geometry_;
    bool enforcing_geometry_ = false;
};

} // namespace ui
} // namespace videoeye
