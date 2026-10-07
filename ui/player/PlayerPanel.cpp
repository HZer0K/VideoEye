#include "ui/player/PlayerPanel.h"

#include "ui/player/ControlBarWidget.h"
#include "ui/player/AudioVizRenderer.h"
#include "ui/player/RawImageSequence.h"
#include "ui/theme/AppTheme.h"
#include "infrastructure/logging/Logger.h"

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
constexpr int kVideoMinHeight = 160;
} // namespace

PlayerPanel::PlayerPanel(QWidget* parent)
    : QWidget(parent) {
    SetupUI();
    // Raw 图像序列：SetupConnections() 要把控制栏的「上一帧/下一帧」按钮连到它，
    // 所以必须在 SetupConnections() 之前建好并注入依赖（否则连到 nullptr 会静默失效）。
    raw_seq_ = new RawImageSequence(this);
    raw_seq_->SetVideoWidget(video_widget_);
    raw_seq_->SetControlBar(control_bar_);
    SetupConnections();
    // 音频可视化渲染器：自管计时器/状态，渲染目标在 SetupUI 之后注入（video_widget_ 已建好）。
    audio_vis_ = new AudioVizRenderer(this);
    audio_vis_->SetVideoWidget(video_widget_);
    // 转发 Raw 组件信号（保持 MainWindow 对 PlayerPanel 的公开接口不变）。
    connect(raw_seq_, &RawImageSequence::StatusMessage, this, &PlayerPanel::StatusMessage);
    connect(raw_seq_, &RawImageSequence::RawImageInfoReady, this, &PlayerPanel::RawImageInfoReady);
    connect(raw_seq_, &RawImageSequence::RawFrameShown, this,
            [this](const QImage& frame, const QString& codec) {
                // overlay 的编码/分辨率缓存归面板所有，由这里写 last_overlay_codec_ 并刷新。
                last_overlay_codec_ = codec;
                RefreshOverlayMediaInfo(&frame);
            });
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

    // --- 控制栏（已抽为独立组件 ControlBarWidget，见 ui/player/ControlBarWidget.{h,cpp}）---
    control_bar_ = new ControlBarWidget(this);
    root_layout->addWidget(control_bar_);
}

void PlayerPanel::SetupConnections() {
    // 控制栏子控件的用户意图全部通过 ControlBarWidget 的访问器连回 PlayerPanel（协调层）；
    // 音量 / 静音两个纯本地 handler 留在 ControlBarWidget 内部，不在此处连接。
    connect(control_bar_->playPauseButton(), &QPushButton::clicked, this, &PlayerPanel::OnPlayPause);
    connect(control_bar_->stopButton(), &QPushButton::clicked, this, &PlayerPanel::OnStop);
    connect(control_bar_->prevFrameButton(), &QPushButton::clicked, raw_seq_, &RawImageSequence::OnPrevRawFrame);
    connect(control_bar_->nextFrameButton(), &QPushButton::clicked, raw_seq_, &RawImageSequence::OnNextRawFrame);
    connect(control_bar_->mvOverlayButton(), &QPushButton::toggled, this, &PlayerPanel::OnMvOverlayToggled);

    connect(control_bar_->collapseButton(), &QPushButton::clicked, this, [this]() {
        SetPlayerAreaVisible(false);
    });

    // 进度条交互:
    //  - 拖动中由 sliderMoved 做"节流的关键帧预览" (画面跟手且不过度占用 UI 线程)
    //  - valueChanged 仅在非拖动时生效 (键盘方向键/程序化跳转), 按当前模式定位
    //  - 释放时由 sliderReleased 按当前选择的定位方式 (关键帧/精确值) 做最终定位
    connect(control_bar_->seekSlider(), &QSlider::sliderPressed, this, [this]() {
        slider_dragging_ = true;
        if (player_) player_->SetSeekDragging(true); // 拖动期间抑制音频, 避免杂音
    });
    connect(control_bar_->seekSlider(), &QSlider::sliderMoved, this, [this](int v) {
        // 拖动中: 实时预览 (画面跟手)。两条分支都要节流到 ~100ms 一次 ——
        // 非 Raw 分支每像素调一次 av_seek_frame 早就卡，Raw 序列这边更卡：
        // 每像素 ShowRawFrame 要整帧读盘 + 做 YUV->RGB 转换，1080p 一次几十毫秒，
        // 一拖就滑不动了。
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - last_drag_seek_ms_ < 100) return;
        last_drag_seek_ms_ = now;
        if (raw_seq_->IsShowingRawImage()) { raw_seq_->ShowRawFrame(v); return; }
        if (player_) player_->Seek(v, model::SeekMode::NearestKeyframe);
    });
    connect(control_bar_->seekSlider(), &QSlider::valueChanged, this, [this](int v) {
        if (slider_dragging_) return; // 拖动中由 sliderMoved 处理预览, 此处跳过
        OnSeek(v); // 键盘/程序化跳转: 按当前模式定位
    });
    connect(control_bar_->seekSlider(), &QSlider::sliderReleased, this, [this]() {
        slider_dragging_ = false;
        if (player_) player_->SetSeekDragging(false); // 恢复音频
        OnSeek(control_bar_->seekSlider()->value()); // 释放时按当前定位方式真正 seek
    });
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
    // 把播放器指针透传给控制栏：音量 / 静音两个本地 handler 需要它来调 SetVolume。
    if (control_bar_) control_bar_->SetMediaPlayer(player);
    if (!player_) return;

    connect(player_, &player::MediaPlayer::StateChanged, this, &PlayerPanel::OnStateChanged);
    connect(player_, &player::MediaPlayer::FrameReady, this, &PlayerPanel::OnFrameReady);
    connect(player_, &player::MediaPlayer::PositionChanged, this, &PlayerPanel::OnPositionChanged);
    connect(player_, &player::MediaPlayer::Error, this, &PlayerPanel::OnError);
    connect(player_, &player::MediaPlayer::OpenFailed, this, &PlayerPanel::OnOpenFailed);
    // 点播放触发的异步打开完成后续播 (见 OnPlayPause / OnPlayerOpenFinished)。
    connect(player_, &player::MediaPlayer::OpenFinished, this, &PlayerPanel::OnPlayerOpenFinished);
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
    overlay.has_video = !audio_only_mode_ && !raw_seq_->IsShowingRawImage();
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
    // 控制栏高度由 ControlBarWidget 固定为 56 (见其 kControlBarHeight)，这里取真实高度，
    // 避免与组件内常量重复定义产生不一致。
    const int bar_height = control_bar_ ? control_bar_->height() : 56;
    return (video_widget_ ? video_widget_->minimumHeight() : kVideoMinHeight) + bar_height;
}

// ============================================================================
// 播放控制
// ============================================================================

void PlayerPanel::OnPlayPause() {
    if (!player_) {
        return;
    }

    if (raw_seq_->IsShowingRawImage()) {
        emit StatusMessage(tr("当前为图像文件，无法播放"), 0);
        return;
    }

    const auto state = player_->GetState();

    // 如果处于Idle/Stopped/Error状态，先打开媒体
    if ((state == model::PlayerState::Idle ||
         state == model::PlayerState::Stopped ||
         state == model::PlayerState::Error) &&
        !current_source_.isEmpty()) {
        // 事务式异步打开: 打开/探测放后台线程, 完成后经 OpenFinished 回 UI 再续播,
        // 不在 UI 线程阻塞。open_then_play_ 记住"这次是点播放触发的"。
        open_then_play_ = true;
        player_->OpenAsync(current_source_);
        return;
    }

    // 根据当前状态切换播放/暂停
    if (state == model::PlayerState::Playing) {
        player_->Pause();
    } else {
        player_->Play();
    }
}

void PlayerPanel::OnPlayerOpenFinished(bool ok) {
    if (!open_then_play_) {
        return;   // 不是"点播放"触发的打开 (如 OpenMedia 主路径), 不在此续播
    }
    open_then_play_ = false;
    if (!player_) {
        return;
    }
    if (!ok) {
        emit StatusMessage(tr("打开失败: %1").arg(player_->GetLastError()), 0);
        return;
    }
    player_->Play();
}

void PlayerPanel::ResetVideoUI() {
    video_widget_->Clear();
    // 清除旧的运动矢量数据, 避免新文件打开前显示残留箭头
    video_widget_->SetMotionVectors(videoeye::model::MacroblockFrameAnalysis{});
    control_bar_->seekSlider()->setValue(0);
    control_bar_->seekSlider()->setRange(0, 0);
    control_bar_->timeLabel()->setText(tr("00:00:00 / 00:00:00"));
    // 换源/停止: 时码回到未知态（帧率与起始时码都要等新文件打开后重新解析）
    timecode_fps_ = 0.0;
    timecode_fps_resolved_ = false;
    timecode_start_valid_ = false;
    timecode_start_ = model::Timecode{};
    if (control_bar_) control_bar_->timecodeLabel()->setText(tr("时码 --:--:--:--"));
}

void PlayerPanel::OnStop() {
    // 清理状态（不立即清理视频UI，让 OnStateChanged 统一处理）
    open_then_play_ = false;   // 停止即作废"打开后续播"意图
    audio_vis_->Reset();
    audio_vis_->SetAlbumCover(QImage());  // 停止即清空封面
    audio_only_mode_ = false;
    raw_seq_->Reset();
    raw_seq_->UpdateRawNavigationState();

    // 停止播放器，会触发 OnStateChanged(Stopped) 来清理视频UI
    if (player_) {
        player_->Stop();
    }
}

void PlayerPanel::StopPlayback() {
    OnStop();
}

void PlayerPanel::OnSeek(int value) {
    if (raw_seq_->IsShowingRawImage()) {
        raw_seq_->ShowRawFrame(value);
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

// 音量 / 静音 handler 已迁移至 ControlBarWidget::OnVolumeChanged / OnMuteClicked，
// 此处不再保留（PlayerPanel 只通过控制栏访问器回写状态，不直接操作音量控件）。

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
        control_bar_->playPauseButton()->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    } else {
        control_bar_->playPauseButton()->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
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
        audio_vis_->SetAlbumCover(frame);
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

    QSignalBlocker blocker(control_bar_->seekSlider());
    control_bar_->seekSlider()->setRange(0, duration_ms);
    control_bar_->seekSlider()->setValue(position_ms);

    control_bar_->timeLabel()->setText(QString("%1 / %2")
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
    if (control_bar_ == nullptr || control_bar_->timecodeLabel() == nullptr) return;
    RefreshTimecodeFps();

    if (timecode_fps_ <= 0.0) {
        control_bar_->timecodeLabel()->setText(tr("时码 --:--:--:--"));
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
    control_bar_->timecodeLabel()->setText(tr("时码 %1").arg(QString::fromStdString(tc.ToString())));
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

void PlayerPanel::OnStreamStatsUpdate(const model::StreamStats& stats) {
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
    // 清空音频可视化运行态（含电平/频谱历史），但不动专辑封面。
    audio_vis_->Reset();
    if (audio_only_mode_) {
        // 保留已有的专辑封面，不清除（后续 OnFrameReady 会刷新）
        video_widget_->Clear();
    } else {
        audio_vis_->SetAlbumCover(QImage());
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
    audio_vis_->OnAudioLevelReady(level, timestamp_seconds);
}

void PlayerPanel::OnAudioVisualizationForDisplay(const model::AudioVisualizationFrame& frame) {
    if (!audio_only_mode_) return;
    audio_vis_->OnAudioVisualizationForDisplay(frame);
}


// ============================================================================
// Raw 图像序列
// ============================================================================

bool PlayerPanel::LoadRawImageFile(const QString& filename) {
    return raw_seq_->LoadRawImageFile(filename);
}

void PlayerPanel::SetRawImageMode(bool on) {
    raw_seq_->SetRawImageMode(on);
}

bool PlayerPanel::IsShowingRawImage() const {
    return raw_seq_->IsShowingRawImage();
}

// ============================================================================
// 运动矢量叠加
// ============================================================================

void PlayerPanel::OnMvOverlayToggled(bool enabled) {
    mv_overlay_enabled_ = enabled;

    if (enabled) {
        // 开启 MV 叠加: 自动启用宏块分析 (会触发软件解码切换)
        if (player_) {
            player_->SetAnalysisFeature(model::AnalysisFeature::Macroblock, true);
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
    if (control_bar_ && control_bar_->mvOverlayButton()) {
        control_bar_->mvOverlayButton()->setChecked(enabled);
    }
}

} // namespace ui
} // namespace videoeye
