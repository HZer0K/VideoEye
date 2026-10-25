#include "ui/player/ControlBarWidget.h"

#include "core/player/MediaPlayer.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QSize>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QWidgetAction>
#include <algorithm>

namespace videoeye {
namespace ui {

namespace {
constexpr int kControlBarHeight = 56;
} // namespace

ControlBarWidget::ControlBarWidget(QWidget* parent) : QWidget(parent) {
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
    control_layout_ = control_layout;
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
    volume_button_->setStyleSheet("QPushButton#volumeButton {"
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
    volume_slider_->setStyleSheet("QSlider#volumeSlider::groove:horizontal {"
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
    mv_overlay_button_->setStyleSheet("QPushButton#MvOverlayButton {"
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
    collapse_player_button_->setStyleSheet("QPushButton#collapsePlayerButton {"
                                           "  background-color: transparent; border: none;"
                                           "}"
                                           "QPushButton#collapsePlayerButton:hover {"
                                           "  background-color: #21262D; border-radius: 4px;"
                                           "}");
    control_layout->addWidget(collapse_player_button_);

    // 次要控件在布局里的原始下标：窄宽收纳移出、恢复原位都靠它（不写死位置）
    timecode_index_ = control_layout->indexOf(timecode_label_);
    volume_index_ = control_layout->indexOf(volume_slider_);
    mv_index_ = control_layout->indexOf(mv_overlay_button_);

    BuildOverflowMenu();
}

void ControlBarWidget::BuildOverflowMenu() {
    // "⋯" 溢出按钮：默认隐藏，窄宽收纳模式才出现
    overflow_button_ = new QToolButton(this);
    overflow_button_->setObjectName("OverflowButton");
    overflow_button_->setText(QStringLiteral("\xE2\x8B\xAF")); // ⋯
    overflow_button_->setToolTip(tr("次要控件（窄宽度时收进这里）"));
    overflow_button_->setPopupMode(QToolButton::InstantPopup);
    overflow_button_->setFixedSize(32, 28);
    overflow_button_->setVisible(false);
    control_layout_->addWidget(overflow_button_);

    overflow_menu_ = new QMenu(this);
    // 三个 QWidgetAction 装的是控件本体（不是镜像）：菜单显示时控件 reparent 进
    // 菜单，关闭后归还；宽回来时 releaseWidget + 按原下标插回布局。信号连接在
    // 控件上，不随父窗口变化失效（PlayerPanel 连的是控件指针）。
    timecode_action_ = new QWidgetAction(this);
    timecode_action_->setToolTip(tr("时码（窄宽度时收进菜单）"));
    overflow_menu_->addAction(timecode_action_);
    volume_action_ = new QWidgetAction(this);
    volume_action_->setToolTip(tr("音量（窄宽度时收进菜单）"));
    overflow_menu_->addAction(volume_action_);
    mv_action_ = new QWidgetAction(this);
    mv_action_->setToolTip(tr("运动矢量叠加（窄宽度时收进菜单）"));
    overflow_menu_->addAction(mv_action_);
    overflow_button_->setMenu(overflow_menu_);
}

void ControlBarWidget::SetSecondaryInMenu(bool in_menu) {
    if (in_menu == secondary_in_menu_)
        return;
    if (!control_layout_ || !overflow_button_)
        return;
    secondary_in_menu_ = in_menu;

    // QLayout::removeWidget 只解除布局管理，不会隐藏控件 —— 不显式 hide 的话，
    // 摘下来的控件会以最后一次的几何位置继续画在控制栏上（菜单里那个是另外一份绘制），
    // 表现为"收进菜单了但栏上还留着残影"。
    QWidget* secondary[] = {timecode_label_, volume_slider_, mv_overlay_button_};
    if (in_menu) {
        // 栏上 -> 菜单：先摘下再交给 QWidgetAction（所有权随 setDefaultWidget 转移）
        for (QWidget* widget : secondary) {
            control_layout_->removeWidget(widget);
            widget->setVisible(false);
        }
        timecode_action_->setDefaultWidget(timecode_label_);
        volume_action_->setDefaultWidget(volume_slider_);
        mv_action_->setDefaultWidget(mv_overlay_button_);
        overflow_button_->setVisible(true);
    } else {
        // 菜单 -> 栏上：取回所有权，按原始下标升序插回（顺序即恢复原布局）
        timecode_action_->releaseWidget(timecode_label_);
        volume_action_->releaseWidget(volume_slider_);
        mv_action_->releaseWidget(mv_overlay_button_);
        control_layout_->insertWidget(timecode_index_, timecode_label_);
        control_layout_->insertWidget(volume_index_, volume_slider_);
        control_layout_->insertWidget(mv_index_, mv_overlay_button_);
        for (QWidget* widget : secondary) {
            widget->setVisible(true);
        }
        overflow_button_->setVisible(false);
    }
}

void ControlBarWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int width = event->size().width();
    if (width <= 0)
        return;

    // 阈值只在不处于收纳态时量一次 —— 见 full_min_width_ 的注释。
    // 若直接用"当前状态的 minimumSizeHint"当阈值，收纳会把阈值一起变小，于是下一次
    // resizeEvent 又判定"放得下"而移回栏上，父布局随之再次变窄：两个状态互相翻转，
    // 拖窗口时会无限抖动（甚至递归到栈溢出）。
    if (!secondary_in_menu_)
        full_min_width_ = minimumSizeHint().width();
    if (full_min_width_ <= 0)
        return; // 布局还没建好，先不动

    // 本机 WM 拖拽中会无视窗口最小尺寸约束，宽度可能被拖到 1100 以下，
    // 这时次要控件必须自己收进菜单，而不是互相重叠 / 被裁掉。
    SetSecondaryInMenu(width < full_min_width_);
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
