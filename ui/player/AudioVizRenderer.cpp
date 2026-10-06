#include "ui/player/AudioVizRenderer.h"

#include <cmath>
#include <algorithm>
#include <deque>

#include <QPainter>
#include <QImage>
#include <QFont>
#include <QPainterPath>
#include <QRadialGradient>
#include <QLinearGradient>

#include "ui/main_window/VideoWidget.h"
#include "core/domain/model/AudioVisualizationFrame.h"

// MSVC 在 <cmath> 前未定义 _USE_MATH_DEFINES 时不导出 M_PI，这里兜底。
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace videoeye {
namespace ui {

AudioVizRenderer::AudioVizRenderer(QObject* parent)
    : QObject(parent) {
    // 与抽取前一致：计时器在构造时启动，RenderAudioVisualization 据此计算帧间隔。
    audio_vis_timer_.start();
}

void AudioVizRenderer::SetVideoWidget(VideoWidget* widget) {
    video_widget_ = widget;
}

void AudioVizRenderer::SetAlbumCover(const QImage& cover) {
    album_cover_ = cover;
}

void AudioVizRenderer::Reset() {
    audio_level_history_.clear();
    spectrum_history_.clear();
    audio_vis_last_render_ms_ = -1;
    audio_vis_smoothed_ = 0.0;
    audio_vis_target_ = 0.0;
    latest_spectrum_bins_.clear();
    latest_waveform_points_.clear();
    smoothed_spectrum_bins_.clear();
    // 注意：不清除 album_cover_，封面由 OnFrameReady / OnMediaModeChanged 显式管理。
}

void AudioVizRenderer::OnAudioLevelReady(double level, double timestamp_seconds) {
    audio_vis_target_ = std::clamp(level, 0.0, 1.0);
    if (!audio_vis_timer_.isValid()) {
        audio_vis_timer_.start();
        audio_vis_last_render_ms_ = -1;
    }

    static constexpr qint64 kFrameIntervalMs = 33;
    const qint64 now_ms = audio_vis_timer_.elapsed();
    if (audio_vis_last_render_ms_ >= 0 &&
        (now_ms - audio_vis_last_render_ms_) < kFrameIntervalMs) {
        return;
    }

    RenderAudioVisualization(timestamp_seconds);
}

void AudioVizRenderer::OnAudioVisualizationForDisplay(
        const model::AudioVisualizationFrame& frame) {
    latest_spectrum_bins_ = QVector<double>(frame.spectrum_bins.begin(),
                                            frame.spectrum_bins.end());
    latest_waveform_points_ = QVector<double>(frame.waveform_points.begin(),
                                              frame.waveform_points.end());

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

void AudioVizRenderer::RenderAudioVisualization(double timestamp_seconds) {
    if (!video_widget_) return;

    const double dt = (audio_vis_last_render_ms_ >= 0)
                          ? (static_cast<double>(audio_vis_timer_.elapsed() -
                                                 audio_vis_last_render_ms_) /
                             1000.0)
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

} // namespace ui
} // namespace videoeye
