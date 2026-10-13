#pragma once

// 播放控制栏：播放/暂停、停止、逐帧、进度条、时间/时码、音量、MV 叠加、收起。
//
// 从 PlayerPanel 抽出的独立视图组件。它只持有自己的 UI 与两个纯本地 handler
// （音量 / 静音，仅触碰本组件内的控件与 MediaPlayer::SetVolume），其余用户意图
// （播放/停止/逐帧/seek/MV/收起）通过子控件访问器交给 PlayerPanel 连接，保持
// PlayerPanel 作为协调层的角色不变。
//
// 窄宽收纳：一排放完所有控件约需 720px。窗口更窄时（本机 WM 拖拽中会无视最小
// 尺寸约束，见 MainWindow::resizeEvent），resizeEvent 按布局的 minimumSizeHint
// 判断放不放得下；放不下就把次要控件（时码 / 音量滑块 / MV 叠加）收进右上角
// 溢出按钮的菜单（控件本体移入 QWidgetAction，宽回来时再移回布局原位），
// 主控件（播放/停止/进度/时间/收起）始终留在栏上。

#include <QWidget>

class QHBoxLayout;
class QPushButton;
class QSlider;
class QLabel;
class QMenu;
class QToolButton;
class QWidgetAction;

namespace videoeye {
namespace player {
class MediaPlayer;
}

namespace ui {

class ControlBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit ControlBarWidget(QWidget* parent = nullptr);

    // 依赖注入：MediaPlayer 由 MainWindow 拥有，此处仅取裸指针供 SetVolume 使用
    void SetMediaPlayer(player::MediaPlayer* player);

    // 子控件访问器（供 PlayerPanel 连接信号 / 回写状态）
    QPushButton* playPauseButton() const { return play_pause_button_; }
    QPushButton* stopButton() const { return stop_button_; }
    QPushButton* prevFrameButton() const { return prev_frame_button_; }
    QPushButton* nextFrameButton() const { return next_frame_button_; }
    QPushButton* volumeButton() const { return volume_button_; }
    QSlider* volumeSlider() const { return volume_slider_; }
    QLabel* timecodeLabel() const { return timecode_label_; }
    QPushButton* mvOverlayButton() const { return mv_overlay_button_; }
    QPushButton* collapseButton() const { return collapse_player_button_; }
    QSlider* seekSlider() const { return seek_slider_; }
    QLabel* timeLabel() const { return time_label_; }

private slots:
    void OnVolumeChanged(int value);
    void OnMuteClicked();

protected:
    // 窄宽收纳：宽度放不下全部控件时，次要控件移进溢出菜单
    void resizeEvent(QResizeEvent* event) override;

private:
    void SetupUI();
    void SetupConnections();
    void BuildOverflowMenu();
    // 次要控件在"栏上 / 溢出菜单"之间搬移（幂等；布局原位在 SetupUI 里记录）
    void SetSecondaryInMenu(bool in_menu);

    player::MediaPlayer* player_ = nullptr;
    int last_volume_ = 100;

    QPushButton* play_pause_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QPushButton* prev_frame_button_ = nullptr;
    QPushButton* next_frame_button_ = nullptr;
    QPushButton* volume_button_ = nullptr;
    QSlider* volume_slider_ = nullptr;
    QLabel* timecode_label_ = nullptr;
    QPushButton* mv_overlay_button_ = nullptr;
    QPushButton* collapse_player_button_ = nullptr;
    QSlider* seek_slider_ = nullptr;
    QLabel* time_label_ = nullptr;

    // ---- 窄宽收纳 ----
    QToolButton* overflow_button_ = nullptr;   // "⋯" 溢出按钮（窄模式才可见）
    QMenu* overflow_menu_ = nullptr;
    QWidgetAction* timecode_action_ = nullptr;  // 时码标签
    QWidgetAction* volume_action_ = nullptr;    // 音量滑块
    QWidgetAction* mv_action_ = nullptr;        // MV 叠加开关
    bool secondary_in_menu_ = false;
    // "全部控件就位"时布局的最小宽度。只在控件都在栏上时量：收进菜单后布局变小，
    // 拿当时那个更小的值当阈值会让判定随状态翻转（收紧→变宽→放松→变窄→…）无限抖动。
    int full_min_width_ = 0;
    // 次要控件在 control_layout_ 里的原始下标（移回时按原序插回）
    int timecode_index_ = -1;
    int volume_index_ = -1;
    int mv_index_ = -1;

    // 控制栏根布局（窄宽收纳时要在它上面搬移控件）
    QHBoxLayout* control_layout_ = nullptr;
};

} // namespace ui
} // namespace videoeye
