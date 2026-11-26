#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QVector>
#include <deque>

#include "core/domain/model/AudioVisualizationFrame.h"

namespace videoeye {
namespace ui {

class VideoWidget;

// 纯音频模式下的音频可视化渲染器：把瞬时音量/频谱/波形画到 VideoWidget 上。
// 状态与渲染逻辑从 PlayerPanel 抽出，PlayerPanel 仅做委托与模式门控（audio_only_mode_）。
class AudioVizRenderer : public QObject {
public:
    explicit AudioVizRenderer(QObject* parent = nullptr);

    // 渲染目标（VideoWidget 由 PlayerPanel 在构造后注入）。
    void SetVideoWidget(VideoWidget* widget);

    // 专辑封面（仅纯音频模式使用，由 OnFrameReady 注入）。
    void SetAlbumCover(const QImage& cover);

    // 收到瞬时音量等级（MediaPlayer 信号），驱动一帧渲染。
    void OnAudioLevelReady(double level, double timestamp_seconds);
    // 收到完整频谱/波形帧（MediaPlayer 信号），做平滑后供渲染使用。
    void OnAudioVisualizationForDisplay(const model::AudioVisualizationFrame& frame);

    // 清空所有运行状态（停止/切换媒体时调用）。不清除专辑封面。
    void Reset();

private:
    void RenderAudioVisualization(double timestamp_seconds);

    VideoWidget* video_widget_ = nullptr;
    QImage album_cover_;
    // 封面缩放与倒影只在封面或目标尺寸变化时重算：旧实现每帧都做一次
    // SmoothTransformation 缩放 + mirrored()，纯音频模式下就是稳定的一笔额外开销。
    QImage scaled_cover_cache_;
    QImage reflection_cache_;
    int cover_cache_size_ = -1;
    // 渲染缓冲区：尺寸不变时复用。用双缓冲轮换 —— VideoWidget 持有的上一帧与
    // 本次要绘制的那张共用同一份数据，QPainter 会触发 detach（整图复制）；
    // 交替使用两张后，绘制目标永远没有额外引用。
    QImage render_buffers_[2];
    int render_buffer_index_ = 0;
    std::deque<double> audio_level_history_;
    std::deque<double> spectrum_history_;
    QElapsedTimer audio_vis_timer_;
    qint64 audio_vis_last_render_ms_ = -1;
    double audio_vis_smoothed_ = 0.0;
    double audio_vis_target_ = 0.0;
    QVector<double> latest_spectrum_bins_;
    QVector<double> latest_waveform_points_;
    QVector<double> smoothed_spectrum_bins_;
};

} // namespace ui
} // namespace videoeye
