#pragma once

// 播放控制栏：播放/暂停、停止、逐帧、进度条、时间/时码、音量、MV 叠加、收起。
//
// 从 PlayerPanel 抽出的独立视图组件。它只持有自己的 UI 与两个纯本地 handler
// （音量 / 静音，仅触碰本组件内的控件与 MediaPlayer::SetVolume），其余用户意图
// （播放/停止/逐帧/seek/MV/收起）通过子控件访问器交给 PlayerPanel 连接，保持
// PlayerPanel 作为协调层的角色不变。
//
// 窄宽收纳：次要控件隐藏时，溢出菜单提供同步的时码、音量和 MV 操作。
// minimumSizeHint 按紧凑布局计算，允许父分栏缩窄到触发收纳。

#include <QWidget>

class QHBoxLayout;
class QPushButton;
class QSlider;
class QLabel;
class QMenu;
class QToolButton;

namespace videoeye {
namespace player {
class MediaPlayer;
}

namespace ui {

class ControlBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit ControlBarWidget(QWidget* parent = nullptr);
    QSize minimumSizeHint() const override;

    // 依赖注入：MediaPlayer 由 MainWindow 拥有，此处仅取裸指针供 SetVolume 使用
    void SetMediaPlayer(player::MediaPlayer* player);

    // 子控件访问器（供 PlayerPanel 连接信号 / 回写状态）
    QPushButton* playPauseButton() const {
        return play_pause_button_;
    }
    QPushButton* stopButton() const {
        return stop_button_;
    }
    QPushButton* prevFrameButton() const {
        return prev_frame_button_;
    }
    QPushButton* nextFrameButton() const {
        return next_frame_button_;
    }
    QPushButton* volumeButton() const {
        return volume_button_;
    }
    QSlider* volumeSlider() const {
        return volume_slider_;
    }
    QLabel* timecodeLabel() const {
        return timecode_label_;
    }
    QPushButton* mvOverlayButton() const {
        return mv_overlay_button_;
    }
    QPushButton* collapseButton() const {
        return collapse_player_button_;
    }
    QSlider* seekSlider() const {
        return seek_slider_;
    }
    QLabel* timeLabel() const {
        return time_label_;
    }

private slots:
    void OnVolumeChanged(int value);
    void OnMuteClicked();

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void SetupUI();
    void SetupConnections();
    void BuildOverflowMenu();
    void SetSecondaryInMenu(bool in_menu);
    int MinimumLayoutWidth(bool full) const;

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
    QToolButton* overflow_button_ = nullptr; // "⋯" 溢出按钮（窄模式才可见）
    QMenu* overflow_menu_ = nullptr;
    bool secondary_in_menu_ = false;

    QHBoxLayout* control_layout_ = nullptr;
};

} // namespace ui
} // namespace videoeye
