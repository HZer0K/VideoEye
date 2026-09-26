#include "ui/player/PlayerPanel.h"

#include "ui/theme/AppTheme.h"
#include "utils/Logger.h"

#include <chrono>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStyle>
#include <QSignalBlocker>
#include <QInputDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>
#include <QDateTime>
#include <QSettings>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QLinearGradient>
#include <QFont>
#include <QMetaObject>

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace videoeye {
namespace ui {

namespace {
constexpr int kControlBarHeight = 56;
constexpr int kVideoMinHeight = 160;
} // namespace

PlayerPanel::PlayerPanel(QWidget* parent)
    : QWidget(parent) {
    SetupUI();
    SetupConnections();
    audio_vis_timer_.start();
}

PlayerPanel::~PlayerPanel() = default;

// ============================================================================
// UI 构建
// ============================================================================

void PlayerPanel::SetupUI() {
    QVBoxLayout* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // --- 视频显示区 ---
    video_widget_ = new VideoWidget(this);
    video_widget_->setMinimumSize(320, kVideoMinHeight);
    video_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root_layout->addWidget(video_widget_);

    // --- 控制栏 ---
    control_bar_ = new QWidget(this);
    control_bar_->setObjectName("ControlBar");
    control_bar_->setFixedHeight(kControlBarHeight);

    QHBoxLayout* control_layout = new QHBoxLayout(control_bar_);
    control_layout->setContentsMargins(16, 8, 16, 8);
    control_layout->setSpacing(12);

    // 播放/暂停按钮 (圆形主按钮)
    play_pause_button_ = new QPushButton(control_bar_);
    play_pause_button_->setObjectName("playPauseButton");
    play_pause_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    play_pause_button_->setIconSize(QSize(16, 16));
    play_pause_button_->setToolTip(tr("播放/暂停"));
    control_layout->addWidget(play_pause_button_);

    // 停止按钮
    stop_button_ = new QPushButton(control_bar_);
    stop_button_->setObjectName("stopButton");
    stop_button_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    stop_button_->setIconSize(QSize(14, 14));
    stop_button_->setToolTip(tr("停止"));
    control_layout->addWidget(stop_button_);

    // 上一帧/下一帧 (仅 raw 序列模式显示)
    prev_frame_button_ = new QPushButton(tr("上一帧"), control_bar_);
    prev_frame_button_->setVisible(false);
    control_layout->addWidget(prev_frame_button_);

    next_frame_button_ = new QPushButton(tr("下一帧"), control_bar_);
    next_frame_button_->setVisible(false);
    control_layout->addWidget(next_frame_button_);

    // 定位方式 (关键帧 / 精确值) 位于顶部菜单: 播放设置 → seek方式

    // 进度条
    seek_slider_ = new QSlider(Qt::Horizontal, control_bar_);
    seek_slider_->setRange(0, 0);
    control_layout->addWidget(seek_slider_, 1);

    // 时间显示
    time_label_ = new QLabel(tr("00:00:00 / 00:00:00"), control_bar_);
    time_label_->setObjectName("TimeLabel");
    time_label_->setMinimumWidth(140);
    time_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    control_layout->addWidget(time_label_);

    // 时码显示（SMPTE HH:MM:SS:FF）：放在时间标签旁，方便与交付单上的时码对表。
    // 素材本身没有写时码时，按 00:00:00:00 起算的"相对时码"显示。
    timecode_label_ = new QLabel(tr("时码 --:--:--:--"), control_bar_);
    timecode_label_->setObjectName("TimecodeLabel");
    timecode_label_->setMinimumWidth(150);
    timecode_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timecode_label_->setToolTip(
        tr("按当前帧率换算的 SMPTE 时码；素材自带起始时码时（tmcd 轨 / timecode tag）以此为基准累加"));
    control_layout->addWidget(timecode_label_);

    // 音量控制
    volume_button_ = new QPushButton(control_bar_);
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

    volume_slider_ = new QSlider(Qt::Horizontal, control_bar_);
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
    mv_overlay_button_ = new QPushButton(tr("MV"), control_bar_);
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
    collapse_player_button_ = new QPushButton(control_bar_);
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

    root_layout->addWidget(control_bar_);
}

void PlayerPanel::SetupConnections() {
    connect(play_pause_button_, &QPushButton::clicked, this, &PlayerPanel::OnPlayPause);
    connect(stop_button_, &QPushButton::clicked, this, &PlayerPanel::OnStop);
    connect(prev_frame_button_, &QPushButton::clicked, this, &PlayerPanel::OnPrevRawFrame);
    connect(next_frame_button_, &QPushButton::clicked, this, &PlayerPanel::OnNextRawFrame);
    connect(mv_overlay_button_, &QPushButton::toggled, this, &PlayerPanel::OnMvOverlayToggled);

    connect(collapse_player_button_, &QPushButton::clicked, this, [this]() {
        SetPlayerAreaVisible(false);
    });

    // 进度条交互:
    //  - 拖动中由 sliderMoved 做"节流的关键帧预览" (画面跟手且不过度占用 UI 线程)
    //  - valueChanged 仅在非拖动时生效 (键盘方向键/程序化跳转), 按当前模式定位
    //  - 释放时由 sliderReleased 按当前选择的定位方式 (关键帧/精确值) 做最终定位
    connect(seek_slider_, &QSlider::sliderPressed, this, [this]() {
        slider_dragging_ = true;
        if (player_) player_->SetSeekDragging(true); // 拖动期间抑制音频, 避免杂音
    });
    connect(seek_slider_, &QSlider::sliderMoved, this, [this](int v) {
        // 拖动中: 实时关键帧预览 (画面跟手)。节流到 ~100ms 一次, 避免每像素都调 av_seek_frame。
        if (showing_raw_image_) { ShowRawFrame(v); return; }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - last_drag_seek_ms_ < 100) return;
        last_drag_seek_ms_ = now;
        if (player_) player_->Seek(v, model::SeekMode::NearestKeyframe);
    });
    connect(seek_slider_, &QSlider::valueChanged, this, [this](int v) {
        if (slider_dragging_) return; // 拖动中由 sliderMoved 处理预览, 此处跳过
        OnSeek(v); // 键盘/程序化跳转: 按当前模式定位
    });
    connect(seek_slider_, &QSlider::sliderReleased, this, [this]() {
        slider_dragging_ = false;
        if (player_) player_->SetSeekDragging(false); // 恢复音频
        OnSeek(seek_slider_->value()); // 释放时按当前定位方式真正 seek
    });

    // 音量控制
    connect(volume_slider_, &QSlider::valueChanged, this, &PlayerPanel::OnVolumeChanged);
    connect(volume_button_, &QPushButton::clicked, this, &PlayerPanel::OnMuteButtonClicked);
}

void PlayerPanel::SetCurrentSource(const QString& source) {
    current_source_ = source;
    // 换源: 清掉上一份文件的叠加层缓存与状态栏统计, 避免新文件沿用旧分辨率/编码。
    // 放在这里而不是 ResetVideoUI, 是因为后者经 queued 回调执行, 可能晚于新内容的显示。
    last_overlay_fps_.clear();
    last_overlay_resolution_.clear();
    last_overlay_codec_.clear();
    last_status_text_.clear();
    last_known_state_ = model::PlayerState::Stopped;
    UpdateOverlay();
    emit StreamStatsDisplayReady(QString());
}

void PlayerPanel::SetMediaPlayer(player::MediaPlayer* player) {
    if (player_ == player) return;
    player_ = player;
    if (!player_) return;

    connect(player_, &player::MediaPlayer::StateChanged, this, &PlayerPanel::OnStateChanged);
    connect(player_, &player::MediaPlayer::FrameReady, this, &PlayerPanel::OnFrameReady);
    connect(player_, &player::MediaPlayer::PositionChanged, this, &PlayerPanel::OnPositionChanged);
    connect(player_, &player::MediaPlayer::Error, this, &PlayerPanel::OnError);
    connect(player_, &player::MediaPlayer::OpenFailed, this, &PlayerPanel::OnOpenFailed);
    connect(player_, &player::MediaPlayer::PlaybackFinished, this, &PlayerPanel::OnPlaybackFinished);
    connect(player_, &player::MediaPlayer::MediaModeChanged, this, &PlayerPanel::OnMediaModeChanged);
    connect(player_, &player::MediaPlayer::AudioLevelReady, this, &PlayerPanel::OnAudioLevelReady);
    connect(player_, &player::MediaPlayer::AudioVisualizationReady,
            this, &PlayerPanel::OnAudioVisualizationForDisplay);
    // 实时码流统计: 与 AnalysisPanel 并列消费同一个信号, 此处用于叠加层 FPS 与状态栏常驻显示。
    // 注意 MediaPlayer 仅在 analysis_enabled_ 时每 10 帧发射一次, 未开启分析时无数据。
    connect(player_, &player::MediaPlayer::StreamStatsReady,
            this, &PlayerPanel::OnStreamStatsUpdate);
}

void PlayerPanel::UpdateOverlay() {
    if (!video_widget_) return;

    const bool is_playing = (last_known_state_ == model::PlayerState::Playing);
    ui::VideoOverlayInfo overlay;
    overlay.is_playing = is_playing;
    overlay.has_video = !audio_only_mode_ && !showing_raw_image_;
    overlay.status = last_status_text_;
    // 暂停时清掉 FPS: 统计值停在最后一帧会误导
    overlay.fps = is_playing ? last_overlay_fps_ : QString();
    overlay.resolution = last_overlay_resolution_;
    overlay.codec = last_overlay_codec_;
    video_widget_->SetOverlayInfo(overlay);
    video_widget_->SetCenterPlayButtonVisible(!is_playing && overlay.has_video);
}

void PlayerPanel::RefreshOverlayMediaInfo(const QImage* frame) {
    bool changed = false;

    // 编码: StreamInfo 在 Open 阶段填充, 取到即缓存 (取到前每帧重试, 应对填充时序)
    if (last_overlay_codec_.isEmpty() && player_) {
        const auto& v = player_->GetStreamInfo().video;
        QString codec = QString::fromStdString(v.format);
        if (codec.isEmpty()) {
            codec = QString::fromStdString(v.codec_id);
        }
        if (!codec.isEmpty()) {
            last_overlay_codec_ = codec;
            changed = true;
        }
    }

    // 分辨率: 以实际帧尺寸为准 (StreamInfo 的 width/height 依赖 MediaInfo, 可能为空)
    if (frame && !frame->isNull()) {
        const QString res = QStringLiteral("%1x%2").arg(frame->width()).arg(frame->height());
        if (res != last_overlay_resolution_) {
            last_overlay_resolution_ = res;
            changed = true;
        }
    }

    if (changed) {
        UpdateOverlay();
    }
}

// ============================================================================
// 播放区显隐
// ============================================================================

bool PlayerPanel::IsPlayerAreaVisible() const {
    return player_area_visible_;
}

void PlayerPanel::SetPlayerAreaVisible(bool visible, bool persist) {
    if (player_area_visible_ == visible) return;
    player_area_visible_ = visible;

    // 收起前记住分割比例: QSplitter 在子部件隐藏时会把空间全部让给另一个,
    // 不保存的话展开后播放区会缩成一条。
    if (!visible && splitter_) {
        saved_splitter_sizes_ = splitter_->sizes();
    }

    setVisible(visible);

    if (visible && splitter_ && saved_splitter_sizes_.size() == splitter_->count()) {
        splitter_->setSizes(saved_splitter_sizes_);
    }

    // 画面输出抑制: 收起后解码线程不再 present / sws_scale, 但解码、实时分析与
    // 音频继续跑 (分析页的码流/宏块等数据仍会更新)。
    if (player_) {
        player_->SetRenderingSuppressed(!visible);
    }

    if (persist) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/playerAreaVisible"), visible);
        emit StatusMessage(visible ? tr("已展开播放区")
                                   : tr("已收起播放区, 分析区占满 (F9 展开)"), 3000);
    }

    emit VisibilityChanged(visible);
}

void PlayerPanel::RestoreVisibility() {
    QSettings settings;
    const QVariant stored = settings.value(QStringLiteral("ui/playerAreaVisible"));
    const bool visible = stored.isValid() ? stored.toBool() : true;
    LOG_INFO("恢复播放区显隐: 配置值=" + (stored.isValid() ? stored.toString().toStdString()
                                                           : std::string("<none>")) +
             " 解析为=" + (visible ? "显示" : "收起") +
             " (配置文件: " + settings.fileName().toStdString() + ")");
    SetPlayerAreaVisible(visible, false);
}

int PlayerPanel::MinimumHeightHint() const {
    if (!IsPlayerAreaVisible()) return 0;
    return (video_widget_ ? video_widget_->minimumHeight() : kVideoMinHeight) + kControlBarHeight;
}

// ============================================================================
// 播放控制
// ============================================================================

void PlayerPanel::OnPlayPause() {
    if (!player_) {
        return;
    }

    if (showing_raw_image_) {
        emit StatusMessage(tr("当前为图像文件，无法播放"), 0);
        return;
    }

    const auto state = player_->GetState();

    // 如果处于Idle/Stopped/Error状态，先打开媒体
    if ((state == model::PlayerState::Idle ||
         state == model::PlayerState::Stopped ||
         state == model::PlayerState::Error) &&
        !current_source_.isEmpty()) {
        if (!player_->Open(current_source_)) {
            emit StatusMessage(tr("打开失败: %1").arg(player_->GetLastError()), 0);
            return;
        }
    }

    // 根据当前状态切换播放/暂停
    if (state == model::PlayerState::Playing) {
        player_->Pause();
    } else {
        player_->Play();
    }
}

void PlayerPanel::ResetVideoUI() {
    video_widget_->Clear();
    // 清除旧的运动矢量数据, 避免新文件打开前显示残留箭头
    video_widget_->SetMotionVectors(videoeye::model::MacroblockFrameAnalysis{});
    seek_slider_->setValue(0);
    seek_slider_->setRange(0, 0);
    time_label_->setText(tr("00:00:00 / 00:00:00"));
    // 换源/停止: 时码回到未知态（帧率与起始时码都要等新文件打开后重新解析）
    timecode_fps_ = 0.0;
    timecode_fps_resolved_ = false;
    timecode_start_valid_ = false;
    timecode_start_ = model::Timecode{};
    if (timecode_label_) timecode_label_->setText(tr("时码 --:--:--:--"));
}

void PlayerPanel::OnStop() {
    // 清理状态（不立即清理视频UI，让 OnStateChanged 统一处理）
    audio_level_history_.clear();
    spectrum_history_.clear();
    audio_only_mode_ = false;
    audio_vis_last_render_ms_ = -1;
    audio_vis_smoothed_ = 0.0;
    audio_vis_target_ = 0.0;
    latest_spectrum_bins_.clear();
    latest_waveform_points_.clear();
    smoothed_spectrum_bins_.clear();
    album_cover_ = QImage();
    showing_raw_image_ = false;
    raw_image_path_.clear();
    raw_pixel_format_.clear();
    raw_width_ = 0;
    raw_height_ = 0;
    raw_frame_size_ = 0;
    raw_total_frames_ = 0;
    raw_current_frame_ = 0;
    UpdateRawNavigationState();

    // 停止播放器，会触发 OnStateChanged(Stopped) 来清理视频UI
    if (player_) {
        player_->Stop();
    }
}

void PlayerPanel::StopPlayback() {
    OnStop();
}

void PlayerPanel::OnSeek(int value) {
    if (showing_raw_image_) {
        ShowRawFrame(value);
        return;
    }
    if (!player_) return;
    // 去重: 释放进度条时 sliderReleased 与尾随的 valueChanged 会用相同目标值
    // 在短时间内各调一次 OnSeek, 这里跳过重复的那次, 避免双 seek。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (value == last_seek_value_ && now - last_seek_time_ < 100) return;
    last_seek_value_ = value;
    last_seek_time_ = now;
    // 按当前选择的定位方式 (关键帧 / 精确帧) 执行 seek
    player_->Seek(value, player_->GetSeekMode());
}

void PlayerPanel::OnVolumeChanged(int value) {
    if (player_) {
        player_->SetVolume(value);
    }
    // 更新 tooltip
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

void PlayerPanel::OnMuteButtonClicked() {
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

// ============================================================================
// MediaPlayer 信号
// ============================================================================

void PlayerPanel::OnStateChanged(model::PlayerState state) {
    QString state_text;
    switch (state) {
        case model::PlayerState::Idle:
            state_text = tr("空闲");
            break;
        case model::PlayerState::Loading:
            state_text = tr("加载中");
            break;
        case model::PlayerState::Playing:
            state_text = tr("播放中");
            break;
        case model::PlayerState::Paused:
            state_text = tr("已暂停");
            break;
        case model::PlayerState::Stopped:
            state_text = tr("已停止");
            // 停止状态下清理UI，确保最后一帧被清除
            QMetaObject::invokeMethod(this, [this]() {
                ResetVideoUI();
            }, Qt::QueuedConnection);
            break;
        case model::PlayerState::Error:
            state_text = tr("错误");
            break;
    }

    emit StatusMessage(state_text, 0);

    // 更新播放/暂停按钮的图标
    if (state == model::PlayerState::Playing) {
        play_pause_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    } else {
        play_pause_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    }

    // 更新视频区叠加层 (状态/分辨率/编码/FPS 统一在 UpdateOverlay 下发)
    last_known_state_ = state;
    last_status_text_ = state_text;
    UpdateOverlay();

    // 停止/空闲时状态栏不再有实时统计意义
    if (state != model::PlayerState::Playing) {
        emit StreamStatsDisplayReady(QString());
    }
}

void PlayerPanel::OnFrameReady(const QImage& frame) {
    // 主线程逐帧绘制耗时统计 (每 100 帧汇总一次, 用于定位播放期卡顿)
    const auto frame_begin = std::chrono::steady_clock::now();

    // 播放区已收起: 不做 CPU 回退绘制。解码线程侧已通过
    // MediaPlayer::SetRenderingSuppressed 停止发帧, 此处为显式兜底。
    if (!IsPlayerAreaVisible()) {
        return;
    }

    // 如果播放器已停止，忽略帧更新
    if (player_ && player_->GetState() == model::PlayerState::Stopped) {
        return;
    }

    // 在纯音频模式下，将封面存储为专辑封面而不是直接显示
    if (audio_only_mode_) {
        album_cover_ = frame;
        return;
    }

    video_widget_->SetFrame(frame);

    // 顺带刷新叠加层的分辨率/编码 (首帧或分辨率变化时才真正下发)
    RefreshOverlayMediaInfo(&frame);
}

void PlayerPanel::OnPositionChanged(int position_ms, int duration_ms) {
    // 如果播放器已停止，忽略位置更新
    if (player_ && player_->GetState() == model::PlayerState::Stopped) {
        return;
    }

    QSignalBlocker blocker(seek_slider_);
    seek_slider_->setRange(0, duration_ms);
    seek_slider_->setValue(position_ms);

    time_label_->setText(QString("%1 / %2")
        .arg(theme::font::formatTime(position_ms))
        .arg(theme::font::formatTime(duration_ms)));

    UpdateTimecodeLabel(position_ms);
}

void PlayerPanel::SetStartTimecode(const QString& timecode, double fps) {
    timecode_start_ = model::TimecodeFromString(timecode.toStdString(), fps);
    timecode_start_valid_ = timecode_start_.valid;
    if (fps > 0.0) timecode_fps_ = fps;
    timecode_fps_resolved_ = false;   // 帧率可能被覆盖, 下次刷新时重新解析
}

void PlayerPanel::RefreshTimecodeFps() {
    if (timecode_fps_ > 0.0 || player_ == nullptr) return;
    // StreamInfo 在 Open 阶段填充, 取到之前每次回调都重试一次
    const std::string& fps_text = player_->GetStreamInfo().video.frame_rate;
    double parsed = 0.0;
    if (PlayerPanel::ParseFrameRateText(fps_text, parsed) && parsed > 0.0) {
        timecode_fps_ = parsed;
        timecode_fps_resolved_ = true;
    }
}

bool PlayerPanel::ParseFrameRateText(const std::string& text, double& fps_out) {
    // StreamInfo 里是 "25.000 fps" / "29.970 (30000/1001) fps" 之类的人类可读串,
    // 这里只取第一个可解析的浮点数。
    std::string digits;
    bool seen_digit = false;
    for (char c : text) {
        if ((c >= '0' && c <= '9') || c == '.') {
            digits.push_back(c);
            if (c >= '0' && c <= '9') seen_digit = true;
        } else if (seen_digit) {
            break;
        }
    }
    if (!seen_digit) return false;
    try {
        fps_out = std::stod(digits);
    } catch (const std::exception&) {
        return false;
    }
    return fps_out > 0.0;
}

void PlayerPanel::UpdateTimecodeLabel(int position_ms) {
    if (timecode_label_ == nullptr) return;
    RefreshTimecodeFps();

    if (timecode_fps_ <= 0.0) {
        timecode_label_->setText(tr("时码 --:--:--:--"));
        return;
    }

    const bool drop_frame = model::IsDropFrameRate(timecode_fps_);
    int64_t start_frames = 0;
    if (timecode_start_valid_) {
        start_frames = timecode_start_.ToFrameCount(timecode_fps_);
        if (start_frames < 0) start_frames = 0;
    } else {
        // 素材没写起始时码: 按 00:00:00:00 起算, drop frame 标记跟着帧率走
    }
    const double position_seconds = static_cast<double>(position_ms) / 1000.0;
    const int64_t elapsed_frames =
        static_cast<int64_t>(std::llround(position_seconds * timecode_fps_));

    const model::Timecode tc = model::TimecodeFromFrameCount(start_frames + elapsed_frames,
                                                             timecode_fps_, drop_frame);
    timecode_label_->setText(tr("时码 %1").arg(QString::fromStdString(tc.ToString())));
}

void PlayerPanel::OnError(const QString& message) {
    QMessageBox::critical(this, tr("错误"), message);
    emit ErrorMessage(tr("错误: %1").arg(message));
}

void PlayerPanel::OnOpenFailed(const QString& message) {
    // 打开失败不弹模态框: 文件仍会被加载到分析模块 (媒体信息/文件结构/诊断扫描),
    // 错误原因经状态栏展示, 用户可在对应分析页查看详情。
    emit StatusMessage(tr("无法播放: %1").arg(message), 0);
    emit ErrorMessage(tr("无法播放: %1").arg(message));
}

void PlayerPanel::OnPlaybackFinished() {
    OnStop();
    emit StatusMessage(tr("播放完成"), 0);
}

void PlayerPanel::OnStreamStatsUpdate(const analyzer::StreamStats& stats) {
    // StreamAnalyzer 起步阶段统计窗口未满, current_fps 会先报若干个 0。
    // 此时不下发, 避免状态栏与叠加层闪现 "FPS 0.0 / 码率 0"。
    if (stats.current_fps <= 0.0) return;

    // 叠加层 FPS
    last_overlay_fps_ = QStringLiteral("%1 fps").arg(stats.current_fps, 0, 'f', 1);
    UpdateOverlay();

    // 状态栏常驻显示: 实时 FPS / 当前与峰值码率 / 关键帧计数。
    // 走 StreamStatsDisplayReady 而非 StatusMessage, 避免每 10 帧冲掉临时提示。
    emit StreamStatsDisplayReady(
        tr("FPS %1 | 码率 %2 kbps (峰值 %3) | 关键帧 %4")
            .arg(stats.current_fps, 0, 'f', 1)
            .arg(stats.current_bitrate_bps / 1000)
            .arg(stats.peak_bitrate_bps / 1000)
            .arg(stats.key_frame_count));
}

void PlayerPanel::OnMediaModeChanged(bool has_video) {
    audio_only_mode_ = !has_video;
    audio_level_history_.clear();
    spectrum_history_.clear();
    audio_vis_last_render_ms_ = -1;
    audio_vis_smoothed_ = 0.0;
    audio_vis_target_ = 0.0;
    latest_spectrum_bins_.clear();
    latest_waveform_points_.clear();
    smoothed_spectrum_bins_.clear();
    if (audio_only_mode_) {
        // 保留已有的专辑封面，不清除 album_cover_
        video_widget_->Clear();
    } else {
        album_cover_ = QImage();
    }

    UpdateOverlay();
}

// ============================================================================
// 音频可视化 (纯音频模式下在视频区绘制)
// ============================================================================

void PlayerPanel::OnAudioLevelReady(double level, double timestamp_seconds) {
    if (!audio_only_mode_) {
        return;
    }

    audio_vis_target_ = std::clamp(level, 0.0, 1.0);
    if (!audio_vis_timer_.isValid()) {
        audio_vis_timer_.start();
        audio_vis_last_render_ms_ = -1;
    }

    static constexpr qint64 kFrameIntervalMs = 33;
    const qint64 now_ms = audio_vis_timer_.elapsed();
    if (audio_vis_last_render_ms_ >= 0 && (now_ms - audio_vis_last_render_ms_) < kFrameIntervalMs) {
        return;
    }

    RenderAudioVisualization(timestamp_seconds);
}

void PlayerPanel::OnAudioVisualizationForDisplay(const model::AudioVisualizationFrame& frame) {
    if (!audio_only_mode_) return;
    latest_spectrum_bins_ = frame.spectrum_bins;
    latest_waveform_points_ = frame.waveform_points;

    // Smooth spectrum with fast attack, slow decay for persistence
    const int n = static_cast<int>(latest_spectrum_bins_.size());
    if (smoothed_spectrum_bins_.size() != n) {
        smoothed_spectrum_bins_.resize(n);
        smoothed_spectrum_bins_.fill(0.0);
    }
    for (int i = 0; i < n; ++i) {
        const double target = latest_spectrum_bins_[i];
        const double alpha = (target > smoothed_spectrum_bins_[i]) ? 0.7 : 0.15;
        smoothed_spectrum_bins_[i] += (target - smoothed_spectrum_bins_[i]) * alpha;
    }
}

void PlayerPanel::RenderAudioVisualization(double timestamp_seconds) {
    const double dt = (audio_vis_last_render_ms_ >= 0)
                          ? (static_cast<double>(audio_vis_timer_.elapsed() - audio_vis_last_render_ms_) / 1000.0)
                          : (0.033);
    audio_vis_last_render_ms_ = audio_vis_timer_.elapsed();

    // Smoothed overall level
    const double tau_attack = 0.04;
    const double tau_release = 0.18;
    const double tau = (audio_vis_target_ > audio_vis_smoothed_) ? tau_attack : tau_release;
    const double alpha = 1.0 - std::exp(-dt / std::max(1e-6, tau));
    audio_vis_smoothed_ += (audio_vis_target_ - audio_vis_smoothed_) * std::clamp(alpha, 0.0, 1.0);

    const int w = std::max(1, video_widget_->width());
    const int h = std::max(1, video_widget_->height());

    QImage img(w, h, QImage::Format_ARGB32);

    // --- Background: radial gradient pulsing with audio ---
    {
        QPainter bg(&img);
        bg.setRenderHint(QPainter::Antialiasing, true);
        const int pulse = static_cast<int>(audio_vis_smoothed_ * 30);
        const double cx = w / 2.0;
        const double cy = h * 0.38;
        const double radius = std::max(w, h) * 0.8;
        QRadialGradient rg(cx, cy, radius);
        rg.setColorAt(0.0, QColor(20 + pulse, 15 + pulse / 2, 50 + pulse));
        rg.setColorAt(0.5, QColor(10, 8, 28));
        rg.setColorAt(1.0, QColor(4, 3, 12));
        bg.fillRect(0, 0, w, h, rg);
    }

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // --- Album cover ---
    const int cover_size = std::clamp(std::min(w, h) * 38 / 100, 80, 380);
    const int cover_cx = w / 2;
    const int cover_cy = h * 38 / 100;
    const int cover_x = cover_cx - cover_size / 2;
    const int cover_y = cover_cy - cover_size / 2;

    if (!album_cover_.isNull()) {
        QImage scaled = album_cover_.scaled(cover_size, cover_size,
                                            Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation);
        const int sx = cover_x + (cover_size - scaled.width()) / 2;
        const int sy = cover_y + (cover_size - scaled.height()) / 2;

        // Glow behind cover, pulsing with audio
        const int glow_r = 15 + static_cast<int>(audio_vis_smoothed_ * 25);
        const int glow_a = 50 + static_cast<int>(audio_vis_smoothed_ * 100);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(100, 160, 255, glow_a));
        painter.drawRoundedRect(sx - glow_r / 2, sy - glow_r / 2,
                                scaled.width() + glow_r, scaled.height() + glow_r, 18, 18);

        // Rounded cover
        QPainterPath clip;
        clip.addRoundedRect(QRect(sx, sy, scaled.width(), scaled.height()), 12, 12);
        painter.save();
        painter.setClipPath(clip);
        painter.drawImage(sx, sy, scaled);
        painter.restore();

        // Border
        painter.setPen(QPen(QColor(255, 255, 255, 35), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(sx, sy, scaled.width(), scaled.height(), 12, 12);

        // Reflection
        const int refl_h = std::min(scaled.height() / 4, 50);
        if (refl_h > 0) {
            QImage ref = scaled.mirrored(false, true).copy(0, 0, scaled.width(), refl_h);
            painter.setOpacity(0.12);
            painter.drawImage(sx, sy + scaled.height() + 3, ref);
            QLinearGradient fg(0, sy + scaled.height() + 3, 0, sy + scaled.height() + 3 + refl_h);
            fg.setColorAt(0.0, QColor(0, 0, 0, 0));
            fg.setColorAt(1.0, QColor(0, 0, 0, 255));
            painter.setOpacity(1.0);
            painter.fillRect(sx, sy + scaled.height() + 3, ref.width(), refl_h, fg);
        }
    } else {
        // Placeholder
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 40, 65, 200));
        painter.drawRoundedRect(cover_x, cover_y, cover_size, cover_size, 12, 12);
        painter.setPen(QColor(100, 120, 160, 140));
        QFont f; f.setPixelSize(cover_size / 3);
        painter.setFont(f);
        painter.drawText(QRect(cover_x, cover_y, cover_size, cover_size),
                         Qt::AlignCenter, QStringLiteral("\u266B"));
    }

    // --- Circular spectrum bars around cover ---
    const int n_bins = static_cast<int>(smoothed_spectrum_bins_.size());
    if (n_bins > 0) {
        const double cx = w / 2.0;
        const double cy = cover_cy;
        const double inner_r = cover_size / 2.0 + 18;
        const double max_bar_len = cover_size * 0.42;

        const int bar_count = std::min(n_bins, 64);
        for (int i = 0; i < bar_count; ++i) {
            double val = std::clamp(smoothed_spectrum_bins_[i] * 8.0, 0.0, 1.0);
            // Apply logarithmic perception: boost quiet frequencies
            val = std::pow(val, 0.6);
            const double bar_len = val * max_bar_len;
            if (bar_len < 2.0) continue;

            const double angle = -M_PI / 2.0 + 2.0 * M_PI * i / bar_count;
            const double cos_a = std::cos(angle);
            const double sin_a = std::sin(angle);

            const double x1 = cx + inner_r * cos_a;
            const double y1 = cy + inner_r * sin_a;
            const double x2 = cx + (inner_r + bar_len) * cos_a;
            const double y2 = cy + (inner_r + bar_len) * sin_a;

            // Gradient color along the bar: base color to tip color
            const double hue = 190.0 + (static_cast<double>(i) / bar_count) * 170.0;
            QColor c_base = QColor::fromHsvF(std::fmod(hue / 360.0, 1.0), 0.55, 0.85, 0.75);
            QColor c_tip  = QColor::fromHsvF(std::fmod(hue / 360.0, 1.0), 0.75, 1.0, 0.95);

            QLinearGradient bar_grad(x1, y1, x2, y2);
            bar_grad.setColorAt(0.0, c_base);
            bar_grad.setColorAt(1.0, c_tip);

            QPen pen(QBrush(bar_grad), 3.2, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(pen);
            painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
        }
    }

    // --- Real waveform at bottom area ---
    const int wf_n = static_cast<int>(latest_waveform_points_.size());
    if (wf_n > 2) {
        const int wf_top = cover_cy + cover_size / 2 + cover_size * 0.5;
        const int wf_bot = h - 10;
        const int wf_h = wf_bot - wf_top;
        if (wf_h > 20) {
            const double mid_y = wf_top + wf_h / 2.0;
            const double amp = wf_h * 0.42;

            // Draw filled waveform area
            QPainterPath wf_path;
            wf_path.moveTo(0.0, mid_y);
            for (int i = 0; i < wf_n; ++i) {
                const double x = static_cast<double>(i) / (wf_n - 1) * w;
                const double y = mid_y - latest_waveform_points_[i] * amp;
                wf_path.lineTo(x, y);
            }
            wf_path.lineTo(static_cast<double>(w), mid_y);
            wf_path.lineTo(0.0, mid_y);

            QLinearGradient wf_fill(0, wf_top, 0, wf_bot);
            const double hue_shift = audio_vis_smoothed_ * 30.0;
            wf_fill.setColorAt(0.0, QColor::fromHsvF((220.0 + hue_shift) / 360.0, 0.6, 0.95, 0.35));
            wf_fill.setColorAt(0.5, QColor::fromHsvF((260.0 + hue_shift) / 360.0, 0.5, 0.7, 0.2));
            wf_fill.setColorAt(1.0, QColor::fromHsvF((220.0 + hue_shift) / 360.0, 0.6, 0.95, 0.35));
            painter.setPen(Qt::NoPen);
            painter.setBrush(wf_fill);
            painter.drawPath(wf_path);

            // Draw waveform line on top
            QPainterPath wf_line;
            for (int i = 0; i < wf_n; ++i) {
                const double x = static_cast<double>(i) / (wf_n - 1) * w;
                const double y = mid_y - latest_waveform_points_[i] * amp;
                if (i == 0) wf_line.moveTo(x, y);
                else wf_line.lineTo(x, y);
            }
            QPen line_pen(QColor::fromHsvF((200.0 + hue_shift) / 360.0, 0.5, 1.0, 0.8));
            line_pen.setWidthF(1.8);
            painter.setPen(line_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(wf_line);

            // Mirror waveform (below center)
            QPainterPath wf_mirror;
            wf_mirror.moveTo(0.0, mid_y);
            for (int i = 0; i < wf_n; ++i) {
                const double x = static_cast<double>(i) / (wf_n - 1) * w;
                const double y = mid_y + latest_waveform_points_[i] * amp;
                wf_mirror.lineTo(x, y);
            }
            wf_mirror.lineTo(static_cast<double>(w), mid_y);
            wf_mirror.lineTo(0.0, mid_y);

            QLinearGradient mirror_fill(0, wf_top, 0, wf_bot);
            mirror_fill.setColorAt(0.0, QColor::fromHsvF((280.0 + hue_shift) / 360.0, 0.5, 0.85, 0.18));
            mirror_fill.setColorAt(1.0, QColor::fromHsvF((300.0 + hue_shift) / 360.0, 0.4, 0.6, 0.08));
            painter.setPen(Qt::NoPen);
            painter.setBrush(mirror_fill);
            painter.drawPath(wf_mirror);

            // Center line
            painter.setPen(QPen(QColor(255, 255, 255, 25), 1.0));
            painter.drawLine(QPointF(0, mid_y), QPointF(w, mid_y));
        }
    } else {
        // Fallback: simple level bars if no waveform data yet
        static constexpr size_t kMaxHistory = 120;
        if (audio_level_history_.size() >= kMaxHistory) audio_level_history_.pop_front();
        audio_level_history_.push_back(audio_vis_smoothed_);

        const int n = static_cast<int>(audio_level_history_.size());
        const int bar_area_y = cover_cy + cover_size / 2 + cover_size * 0.5;
        const int bar_area_h = h - bar_area_y - 10;
        if (bar_area_h > 10 && n > 1) {
            const double bar_w = static_cast<double>(w) / n;
            for (int i = 0; i < n; ++i) {
                const double v = std::clamp(audio_level_history_[static_cast<size_t>(i)], 0.0, 1.0);
                const int amp = static_cast<int>(v * (bar_area_h * 0.8));
                if (amp < 1) continue;
                const int x0 = static_cast<int>(i * bar_w);
                const int bw = std::max(1, static_cast<int>(bar_w) - 1);
                const int my = bar_area_y + bar_area_h / 2;
                const double hue = 180.0 + (static_cast<double>(i) / n) * 120.0;
                QLinearGradient g(x0, my - amp, x0, my + amp);
                g.setColorAt(0.0, QColor::fromHsvF(hue / 360.0, 0.6, 0.95, 0.9));
                g.setColorAt(1.0, QColor::fromHsvF(hue / 360.0, 0.4, 0.7, 0.4));
                painter.setPen(Qt::NoPen);
                painter.setBrush(g);
                painter.drawRoundedRect(x0, my - amp, bw, amp * 2, 1.5, 1.5);
            }
        }
    }

    // --- Info text ---
    painter.setPen(QColor(200, 200, 220, 140));
    QFont info_font;
    info_font.setPixelSize(12);
    painter.setFont(info_font);
    painter.drawText(QRect(10, 6, w - 20, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QString("t=%1s  level=%2")
                         .arg(timestamp_seconds, 0, 'f', 3)
                         .arg(audio_vis_smoothed_, 0, 'f', 3));

    video_widget_->SetFrame(img);
}

// ============================================================================
// Raw 图像序列
// ============================================================================

bool PlayerPanel::LoadRawImageFile(const QString& filename) {
    const QString suffix = QFileInfo(filename).suffix().toLower();
    if (suffix != "yuv" && suffix != "nv12" && suffix != "rgb" &&
        suffix != "bgr" && suffix != "yuy2" && suffix != "raw") {
        return false;
    }

    int default_width = 1920;
    int default_height = 1080;
    {
        const QString name = QFileInfo(filename).fileName().toLower();
        QRegularExpression re_wh(R"((\d{2,5})\s*[xX]\s*(\d{2,5}))");
        QRegularExpressionMatch m = re_wh.match(name);
        if (m.hasMatch()) {
            bool ok_w = false;
            bool ok_h = false;
            const int w = m.captured(1).toInt(&ok_w);
            const int h = m.captured(2).toInt(&ok_h);
            if (ok_w && ok_h && w > 0 && h > 0) {
                default_width = w;
                default_height = h;
            }
        } else {
            QRegularExpression re_w_h(R"(w(\d{2,5}).*h(\d{2,5}))", QRegularExpression::CaseInsensitiveOption);
            m = re_w_h.match(name);
            if (m.hasMatch()) {
                bool ok_w = false;
                bool ok_h = false;
                const int w = m.captured(1).toInt(&ok_w);
                const int h = m.captured(2).toInt(&ok_h);
                if (ok_w && ok_h && w > 0 && h > 0) {
                    default_width = w;
                    default_height = h;
                }
            }
        }
    }

    bool ok = false;
    const int width = QInputDialog::getInt(this, tr("图像宽度"),
                                           tr("请输入宽度(px):"),
                                           default_width, 1, 16384, 1, &ok);
    if (!ok) {
        return false;
    }
    const int height = QInputDialog::getInt(this, tr("图像高度"),
                                            tr("请输入高度(px):"),
                                            default_height, 1, 16384, 1, &ok);
    if (!ok) {
        return false;
    }

    const QString lower_name = QFileInfo(filename).fileName().toLower();
    const QStringList formats = {
        "YUV420P(I420)",
        "NV12",
        "YUY2",
        "RGB24",
        "BGR24"
    };

    QString default_format = "YUV420P(I420)";
    if (suffix == "nv12" || lower_name.contains("nv12")) {
        default_format = "NV12";
    } else if (suffix == "yuy2" || lower_name.contains("yuy2")) {
        default_format = "YUY2";
    } else if (suffix == "rgb" || lower_name.contains("rgb24") || lower_name.contains("_rgb")) {
        default_format = "RGB24";
    } else if (suffix == "bgr" || lower_name.contains("bgr24") || lower_name.contains("_bgr")) {
        default_format = "BGR24";
    }

    const QString selected = QInputDialog::getItem(this, tr("原始图像格式"),
                                                   tr("选择像素格式:"),
                                                   formats,
                                                   formats.indexOf(default_format),
                                                   false, &ok);
    if (!ok || selected.isEmpty()) {
        return false;
    }

    qint64 frame_size = 0;
    if (selected == "RGB24" || selected == "BGR24") {
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3;
    } else if (selected == "YUV420P(I420)" || selected == "NV12") {
        if ((width % 2) != 0 || (height % 2) != 0) {
            QMessageBox::warning(this, tr("尺寸不支持"),
                                 tr("%1 仅支持偶数宽高。").arg(selected));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3 / 2;
    } else if (selected == "YUY2") {
        if ((width % 2) != 0) {
            QMessageBox::warning(this, tr("尺寸不支持"),
                                 tr("YUY2 仅支持偶数宽度。"));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 2;
    }

    const qint64 file_size = QFileInfo(filename).size();
    if (frame_size <= 0 || file_size < frame_size) {
        QMessageBox::warning(this, tr("打开失败"),
                             tr("文件大小不足以组成一帧。\n单帧大小: %1 字节\n文件大小: %2 字节")
                                 .arg(frame_size)
                                 .arg(file_size));
        return false;
    }

    raw_image_path_ = filename;
    raw_pixel_format_ = selected;
    raw_width_ = width;
    raw_height_ = height;
    raw_frame_size_ = frame_size;
    raw_total_frames_ = std::max(1, static_cast<int>(file_size / frame_size));
    raw_current_frame_ = 0;

    const qint64 remain = file_size % frame_size;
    emit RawImageInfoReady(
        tr("Raw Image Sequence\n"
           "File: %1\n"
           "Format: %2\n"
           "Size: %3x%4\n"
           "Frame Size: %5 bytes\n"
           "Total Frames: %6\n"
           "Ignored Tail Bytes: %7")
            .arg(filename)
            .arg(selected)
            .arg(width)
            .arg(height)
            .arg(frame_size)
            .arg(raw_total_frames_)
            .arg(remain));

    return ShowRawFrame(0);
}

bool PlayerPanel::ShowRawFrame(int frame_index) {
    if (!showing_raw_image_ || raw_image_path_.isEmpty() || raw_frame_size_ <= 0 ||
        frame_index < 0 || frame_index >= raw_total_frames_) {
        return false;
    }

    QFile file(raw_image_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("打开失败"), tr("无法读取文件: %1").arg(raw_image_path_));
        return false;
    }

    const qint64 offset = static_cast<qint64>(frame_index) * raw_frame_size_;
    if (!file.seek(offset)) {
        QMessageBox::warning(this, tr("定位失败"),
                             tr("无法定位到第 %1 帧。").arg(frame_index + 1));
        return false;
    }

    const QByteArray data = file.read(raw_frame_size_);
    file.close();
    if (data.size() != raw_frame_size_) {
        QMessageBox::warning(this, tr("读取失败"),
                             tr("读取第 %1 帧失败，期望 %2 字节，实际 %3 字节。")
                                 .arg(frame_index + 1)
                                 .arg(raw_frame_size_)
                                 .arg(data.size()));
        return false;
    }

    auto clamp_to_u8 = [](int value) -> uchar {
        return static_cast<uchar>(qBound(0, value, 255));
    };
    auto yuv_to_rgb = [&](int Y, int U, int V, uchar* dst) {
        const int u = U - 128;
        const int v = V - 128;
        const int r = Y + static_cast<int>(1.402 * v);
        const int g = Y - static_cast<int>(0.344136 * u + 0.714136 * v);
        const int b = Y + static_cast<int>(1.772 * u);
        dst[0] = clamp_to_u8(r);
        dst[1] = clamp_to_u8(g);
        dst[2] = clamp_to_u8(b);
    };

    QImage img(raw_width_, raw_height_, QImage::Format_RGB888);
    if (img.isNull()) {
        return false;
    }

    if (raw_pixel_format_ == "RGB24" || raw_pixel_format_ == "BGR24") {
        const uchar* src = reinterpret_cast<const uchar*>(data.constData());
        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const uchar* src_row = src + static_cast<qint64>(j) * raw_width_ * 3;
            for (int i = 0; i < raw_width_; ++i) {
                const int src_idx = i * 3;
                if (raw_pixel_format_ == "RGB24") {
                    row[src_idx + 0] = src_row[src_idx + 0];
                    row[src_idx + 1] = src_row[src_idx + 1];
                    row[src_idx + 2] = src_row[src_idx + 2];
                } else {
                    row[src_idx + 0] = src_row[src_idx + 2];
                    row[src_idx + 1] = src_row[src_idx + 1];
                    row[src_idx + 2] = src_row[src_idx + 0];
                }
            }
        }
    } else if (raw_pixel_format_ == "YUV420P(I420)" || raw_pixel_format_ == "NV12") {
        const qint64 y_size = static_cast<qint64>(raw_width_) * static_cast<qint64>(raw_height_);
        const uchar* y_plane = reinterpret_cast<const uchar*>(data.constData());
        const uchar* uv_plane = y_plane + y_size;
        const qint64 uv_size = y_size / 4;
        const uchar* u_plane = (raw_pixel_format_ == "NV12") ? nullptr : uv_plane;
        const uchar* v_plane = (raw_pixel_format_ == "NV12") ? nullptr : (uv_plane + uv_size);

        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const int uv_j_i420 = (j / 2) * (raw_width_ / 2);
            const int uv_j_nv12 = (j / 2) * raw_width_;
            for (int i = 0; i < raw_width_; ++i) {
                const int y_idx = j * raw_width_ + i;
                int U = 0;
                int V = 0;
                if (raw_pixel_format_ == "NV12") {
                    const int uv_idx = uv_j_nv12 + (i / 2) * 2;
                    U = static_cast<int>(uv_plane[uv_idx]);
                    V = static_cast<int>(uv_plane[uv_idx + 1]);
                } else {
                    const int uv_idx = uv_j_i420 + (i / 2);
                    U = static_cast<int>(u_plane[uv_idx]);
                    V = static_cast<int>(v_plane[uv_idx]);
                }
                yuv_to_rgb(static_cast<int>(y_plane[y_idx]), U, V, row + i * 3);
            }
        }
    } else if (raw_pixel_format_ == "YUY2") {
        const uchar* src = reinterpret_cast<const uchar*>(data.constData());
        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const uchar* src_row = src + static_cast<qint64>(j) * raw_width_ * 2;
            for (int i = 0; i < raw_width_; i += 2) {
                const int idx = i * 2;
                const int y0 = static_cast<int>(src_row[idx + 0]);
                const int u = static_cast<int>(src_row[idx + 1]);
                const int y1 = static_cast<int>(src_row[idx + 2]);
                const int v = static_cast<int>(src_row[idx + 3]);
                yuv_to_rgb(y0, u, v, row + i * 3);
                if (i + 1 < raw_width_) {
                    yuv_to_rgb(y1, u, v, row + (i + 1) * 3);
                }
            }
        }
    } else {
        return false;
    }

    raw_current_frame_ = frame_index;
    video_widget_->SetFrame(img);
    // Raw 序列没有编码可言, 叠加层用像素格式占位 + 实际帧尺寸
    last_overlay_codec_ = raw_pixel_format_;
    RefreshOverlayMediaInfo(&img);
    UpdateRawNavigationState();
    emit StatusMessage(tr("Raw 帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_), 0);
    return true;
}

void PlayerPanel::UpdateRawNavigationState() {
    const bool raw_mode = showing_raw_image_ && raw_total_frames_ > 0;

    if (prev_frame_button_) {
        prev_frame_button_->setVisible(raw_mode);
        prev_frame_button_->setEnabled(raw_mode && raw_current_frame_ > 0);
    }
    if (next_frame_button_) {
        next_frame_button_->setVisible(raw_mode);
        next_frame_button_->setEnabled(raw_mode && raw_current_frame_ + 1 < raw_total_frames_);
    }
    if (play_pause_button_) {
        play_pause_button_->setEnabled(!raw_mode);
    }
    if (volume_button_) {
        volume_button_->setEnabled(!raw_mode);
    }
    if (volume_slider_) {
        volume_slider_->setEnabled(!raw_mode);
    }

    if (raw_mode) {
        QSignalBlocker blocker(seek_slider_);
        seek_slider_->setRange(0, std::max(0, raw_total_frames_ - 1));
        seek_slider_->setValue(raw_current_frame_);
        seek_slider_->setEnabled(raw_total_frames_ > 1);
        time_label_->setText(tr("帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_));
    } else {
        seek_slider_->setEnabled(true);
        time_label_->setText(tr("00:00:00 / 00:00:00"));
    }
}

void PlayerPanel::OnPrevRawFrame() {
    if (showing_raw_image_ && raw_current_frame_ > 0) {
        ShowRawFrame(raw_current_frame_ - 1);
    }
}

void PlayerPanel::OnNextRawFrame() {
    if (showing_raw_image_ && raw_current_frame_ + 1 < raw_total_frames_) {
        ShowRawFrame(raw_current_frame_ + 1);
    }
}

void PlayerPanel::SetRawImageMode(bool on) {
    showing_raw_image_ = on;
}

// ============================================================================
// 运动矢量叠加
// ============================================================================

void PlayerPanel::OnMvOverlayToggled(bool enabled) {
    mv_overlay_enabled_ = enabled;

    if (enabled) {
        // 开启 MV 叠加: 自动启用宏块分析 (会触发软件解码切换)
        if (player_) {
            player_->SetMacroblockAnalysisEnabled(true);
        }
        video_widget_->SetMvOverlayMode(ui::MvOverlayMode::Arrows);
        emit StatusMessage(tr("运动矢量叠加已开启"), 3000);
    } else {
        video_widget_->SetMvOverlayMode(ui::MvOverlayMode::Off);
        emit StatusMessage(tr("运动矢量叠加已关闭"), 3000);
    }
}

void PlayerPanel::OnMacroblockInfoForOverlay(
        const videoeye::model::MacroblockFrameAnalysis& analysis) {
    if (!mv_overlay_enabled_) return;
    video_widget_->SetMotionVectors(analysis);
}

void PlayerPanel::SetMvOverlayEnabled(bool enabled) {
    if (mv_overlay_button_) {
        mv_overlay_button_->setChecked(enabled);
    }
}

} // namespace ui
} // namespace videoeye
