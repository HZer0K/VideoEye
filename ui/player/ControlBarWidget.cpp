#include "ui/player/ControlBarWidget.h"

#include "core/player/MediaPlayer.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QStyle>
#include <QSize>
#include <algorithm>

namespace videoeye {
namespace ui {

namespace {
constexpr int kControlBarHeight = 56;
} // namespace

ControlBarWidget::ControlBarWidget(QWidget* parent)
    : QWidget(parent) {
    SetupUI();
    SetupConnections();
}

void ControlBarWidget::SetMediaPlayer(player::MediaPlayer* player) {
    player_ = player;
}

void ControlBarWidget::SetupUI() {
    setObjectName("ControlBar");
    setFixedHeight(kControlBarHeight);

    QHBoxLayout* control_layout = new QHBoxLayout(this);
    control_layout->setContentsMargins(16, 8, 16, 8);
    control_layout->setSpacing(12);

    // 播放/暂停按钮 (圆形主按钮)
    play_pause_button_ = new QPushButton(this);
    play_pause_button_->setObjectName("playPauseButton");
    play_pause_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    play_pause_button_->setIconSize(QSize(16, 16));
    play_pause_button_->setToolTip(tr("播放/暂停"));
    control_layout->addWidget(play_pause_button_);

    // 停止按钮
    stop_button_ = new QPushButton(this);
    stop_button_->setObjectName("stopButton");
    stop_button_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    stop_button_->setIconSize(QSize(14, 14));
    stop_button_->setToolTip(tr("停止"));
    control_layout->addWidget(stop_button_);

    // 上一帧/下一帧 (仅 raw 序列模式显示)
    prev_frame_button_ = new QPushButton(tr("上一帧"), this);
    prev_frame_button_->setVisible(false);
    control_layout->addWidget(prev_frame_button_);

    next_frame_button_ = new QPushButton(tr("下一帧"), this);
    next_frame_button_->setVisible(false);
    control_layout->addWidget(next_frame_button_);

    // 进度条
    seek_slider_ = new QSlider(Qt::Horizontal, this);
    seek_slider_->setRange(0, 0);
    control_layout->addWidget(seek_slider_, 1);

    // 时间显示
    time_label_ = new QLabel(tr("00:00:00 / 00:00:00"), this);
    time_label_->setObjectName("TimeLabel");
    time_label_->setMinimumWidth(140);
    time_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    control_layout->addWidget(time_label_);

    // 时码显示（SMPTE HH:MM:SS:FF）：放在时间标签旁，方便与交付单上的时码对表。
    // 素材本身没有写时码时，按 00:00:00:00 起算的"相对时码"显示。
    timecode_label_ = new QLabel(tr("时码 --:--:--:--"), this);
    timecode_label_->setObjectName("TimecodeLabel");
    timecode_label_->setMinimumWidth(150);
    timecode_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timecode_label_->setToolTip(
        tr("按当前帧率换算的 SMPTE 时码；素材自带起始时码时（tmcd 轨 / timecode tag）以此为基准累加"));
    control_layout->addWidget(timecode_label_);

    // 音量控制
    volume_button_ = new QPushButton(this);
    volume_button_->setObjectName("volumeButton");
    volume_button_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    volume_button_->setIconSize(QSize(16, 16));
    volume_button_->setFixedSize(32, 28);
    volume_button_->setToolTip(tr("静音/取消静音"));
    volume_button_->setStyleSheet(
        "QPushButton#volumeButton {"
        "  background-color: transparent; border: none;"
        "}"
        "QPushButton#volumeButton:hover {"
        "  background-color: #21262D; border-radius: 4px;"
        "}");
    control_layout->addWidget(volume_button_);

    volume_slider_ = new QSlider(Qt::Horizontal, this);
    volume_slider_->setObjectName("volumeSlider");
    volume_slider_->setRange(0, 100);
    volume_slider_->setValue(100);
    volume_slider_->setFixedWidth(80);
    volume_slider_->setToolTip(tr("音量: 100%"));
    volume_slider_->setStyleSheet(
        "QSlider#volumeSlider::groove:horizontal {"
        "  border: none; height: 4px; background: #30363D; border-radius: 2px;"
        "}"
        "QSlider#volumeSlider::sub-page:horizontal {"
        "  background: #58A6FF; border-radius: 2px;"
        "}"
        "QSlider#volumeSlider::handle:horizontal {"
        "  background: #C9D1D9; width: 12px; height: 12px;"
        "  margin: -5px 0; border-radius: 6px;"
        "}"
        "QSlider#volumeSlider::handle:horizontal:hover {"
        "  background: #FFFFFF;"
        "}");
    control_layout->addWidget(volume_slider_);

    // 运动矢量叠加开关
    mv_overlay_button_ = new QPushButton(tr("MV"), this);
    mv_overlay_button_->setObjectName("MvOverlayButton");
    mv_overlay_button_->setCheckable(true);
    mv_overlay_button_->setToolTip(tr("运动矢量叠加显示 (需软件解码)"));
    mv_overlay_button_->setFixedSize(40, 28);
    mv_overlay_button_->setStyleSheet(
        "QPushButton#MvOverlayButton {"
        "  background-color: #21262D; border: 1px solid #30363D;"
        "  border-radius: 4px; color: #8B949E; font-size: 11px; font-weight: bold;"
        "}"
        "QPushButton#MvOverlayButton:hover {"
        "  border-color: #58A6FF; color: #C9D1D9;"
        "}"
        "QPushButton#MvOverlayButton:checked {"
        "  background-color: #1F6FEB; border-color: #58A6FF; color: #FFFFFF;"
        "}");
    control_layout->addWidget(mv_overlay_button_);

    // 收起播放区 (分析为主: 收起后播放区隐藏, 分析区占满整个内容区)
    collapse_player_button_ = new QPushButton(this);
    collapse_player_button_->setObjectName("collapsePlayerButton");
    collapse_player_button_->setIcon(style()->standardIcon(QStyle::SP_TitleBarShadeButton));
    collapse_player_button_->setIconSize(QSize(16, 16));
    collapse_player_button_->setFixedSize(32, 28);
    collapse_player_button_->setToolTip(tr("收起播放区 (F9)"));
    collapse_player_button_->setStyleSheet(
        "QPushButton#collapsePlayerButton {"
        "  background-color: transparent; border: none;"
        "}"
        "QPushButton#collapsePlayerButton:hover {"
        "  background-color: #21262D; border-radius: 4px;"
        "}");
    control_layout->addWidget(collapse_player_button_);
}

void ControlBarWidget::SetupConnections() {
    connect(volume_slider_, &QSlider::valueChanged, this, &ControlBarWidget::OnVolumeChanged);
    connect(volume_button_, &QPushButton::clicked, this, &ControlBarWidget::OnMuteClicked);
}

void ControlBarWidget::OnVolumeChanged(int value) {
    if (player_) {
        player_->SetVolume(value);
    }
    volume_slider_->setToolTip(tr("音量: %1%").arg(value));

    // 更新音量按钮图标: 音量为 0 时显示静音图标
    if (value == 0) {
        volume_button_->setIcon(style()->standardIcon(QStyle::SP_MediaVolumeMuted));
    } else {
        volume_button_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
        // 从 0 调高音量时, 记住当前音量 (用于静音恢复)
        if (last_volume_ == 0) {
            last_volume_ = value;
        }
    }
}

void ControlBarWidget::OnMuteClicked() {
    const int current = volume_slider_->value();
    if (current > 0) {
        // 当前有音量 → 静音: 记住音量, 滑块归零
        last_volume_ = current;
        volume_slider_->setValue(0);
    } else {
        // 当前静音 → 恢复: 恢复上次音量 (至少 1, 避免 0 又变静音)
        const int restore = std::max(1, last_volume_);
        volume_slider_->setValue(restore);
    }
}

} // namespace ui
} // namespace videoeye
