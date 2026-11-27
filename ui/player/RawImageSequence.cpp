#include "ui/player/RawImageSequence.h"

#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>

#include <algorithm>

#include "ui/main_window/VideoWidget.h"
#include "ui/player/ControlBarWidget.h"

namespace videoeye {
namespace ui {

namespace {

// 从文件名猜连续播放帧率（如 clip_25fps.yuv / shot_1920x1080_30p.yuv）。
// 只接受 1..240 的取值：1920x1080 这类尺寸数字不会误命中（"p" 后必须是非数字边界）。
int InferRawFps(const QString& file_name) {
    static const QRegularExpression kFpsRe(R"((\d{1,3})\s*(?:fps|p)\b)", QRegularExpression::CaseInsensitiveOption);
    auto it = kFpsRe.globalMatch(file_name);
    while (it.hasNext()) {
        const int value = it.next().captured(1).toInt();
        if (value >= 1 && value <= 240) {
            return value;
        }
    }
    return 25;
}

} // namespace

RawImageSequence::RawImageSequence(QWidget* parent) : QObject(parent), dialog_parent_(parent) {
    // Raw 没有音频轨，连续播放只能靠自己的时钟。
    playback_timer_ = new QTimer(this);
    playback_timer_->setTimerType(Qt::PreciseTimer);
    connect(playback_timer_, &QTimer::timeout, this, &RawImageSequence::AdvancePlaybackFrame);
}

void RawImageSequence::SetVideoWidget(VideoWidget* widget) {
    video_widget_ = widget;
}

void RawImageSequence::SetControlBar(ControlBarWidget* bar) {
    control_bar_ = bar;
}

bool RawImageSequence::LoadRawImageFile(const QString& filename) {
    const QString suffix = QFileInfo(filename).suffix().toLower();
    if (suffix != "yuv" && suffix != "nv12" && suffix != "nv21" && suffix != "yv12" && suffix != "p010" &&
        suffix != "yuv420p10" && suffix != "yuy2" && suffix != "uyvy" && suffix != "gray" && suffix != "rgb" &&
        suffix != "bgr" && suffix != "raw") {
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
    const int width =
        QInputDialog::getInt(dialog_parent_, tr("图像宽度"), tr("请输入宽度(px):"), default_width, 1, 16384, 1, &ok);
    if (!ok) {
        return false;
    }
    const int height =
        QInputDialog::getInt(dialog_parent_, tr("图像高度"), tr("请输入高度(px):"), default_height, 1, 16384, 1, &ok);
    if (!ok) {
        return false;
    }

    const QString lower_name = QFileInfo(filename).fileName().toLower();
    const QStringList formats = {"YUV420P(I420)", "NV12", "NV21",  "YV12",  "P010", "YUV420P10LE",
                                 "YUY2",          "UYVY", "GRAY8", "RGB24", "BGR24"};

    QString default_format = "YUV420P(I420)";
    if (suffix == "nv12" || lower_name.contains("nv12")) {
        default_format = "NV12";
    } else if (suffix == "nv21" || lower_name.contains("nv21")) {
        default_format = "NV21";
    } else if (suffix == "yv12" || lower_name.contains("yv12")) {
        default_format = "YV12";
    } else if (suffix == "yuy2" || lower_name.contains("yuy2")) {
        default_format = "YUY2";
    } else if (suffix == "uyvy" || lower_name.contains("uyvy")) {
        default_format = "UYVY";
    } else if (suffix == "gray" || lower_name.contains("gray8")) {
        default_format = "GRAY8";
    } else if (suffix == "p010" || lower_name.contains("p010")) {
        default_format = "P010";
    } else if (lower_name.contains("yuv420p10")) {
        default_format = "YUV420P10LE";
    } else if (suffix == "rgb" || lower_name.contains("rgb24") || lower_name.contains("_rgb")) {
        default_format = "RGB24";
    } else if (suffix == "bgr" || lower_name.contains("bgr24") || lower_name.contains("_bgr")) {
        default_format = "BGR24";
    }

    const QString selected = QInputDialog::getItem(dialog_parent_, tr("原始图像格式"), tr("选择像素格式:"), formats,
                                                   formats.indexOf(default_format), false, &ok);
    if (!ok || selected.isEmpty()) {
        return false;
    }

    // 4:2:0 家族需要偶数宽高；packed 4:2:2 只需要偶数宽度；RGB/GRAY 任意尺寸。
    const bool is_420_8bit =
        selected == "YUV420P(I420)" || selected == "NV12" || selected == "NV21" || selected == "YV12";
    const bool is_420_10bit = selected == "P010" || selected == "YUV420P10LE";
    const bool is_422_packed = selected == "YUY2" || selected == "UYVY";

    qint64 frame_size = 0;
    if (selected == "RGB24" || selected == "BGR24") {
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3;
    } else if (selected == "GRAY8") {
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height);
    } else if (is_420_8bit) {
        if ((width % 2) != 0 || (height % 2) != 0) {
            QMessageBox::warning(dialog_parent_, tr("尺寸不支持"), tr("%1 仅支持偶数宽高。").arg(selected));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3 / 2;
    } else if (is_420_10bit) {
        if ((width % 2) != 0 || (height % 2) != 0) {
            QMessageBox::warning(dialog_parent_, tr("尺寸不支持"), tr("%1 仅支持偶数宽高。").arg(selected));
            return false;
        }
        // 每个样本 2 字节：Y(w*h) + U/V 各(w*h/4) → 3*w*h 字节
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3;
    } else if (is_422_packed) {
        if ((width % 2) != 0) {
            QMessageBox::warning(dialog_parent_, tr("尺寸不支持"), tr("%1 仅支持偶数宽度。").arg(selected));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 2;
    }

    const qint64 file_size = QFileInfo(filename).size();
    if (frame_size <= 0 || file_size < frame_size) {
        QMessageBox::warning(
            dialog_parent_, tr("打开失败"),
            tr("文件大小不足以组成一帧。\n单帧大小: %1 字节\n文件大小: %2 字节").arg(frame_size).arg(file_size));
        return false;
    }

    // 连续播放的时钟只能由帧率决定：默认值从文件名猜（如 _25fps / _30p），否则 25。
    // 单帧文件没有"连续播放"可言，不用拿这个对话框骚扰用户。
    const int total_frames = std::max(1, static_cast<int>(file_size / frame_size));
    int fps = 25;
    if (total_frames > 1) {
        fps = QInputDialog::getInt(dialog_parent_, tr("播放帧率"), tr("连续播放帧率(fps):"),
                                   InferRawFps(QFileInfo(filename).fileName()), 1, 240, 1, &ok);
        if (!ok) {
            return false;
        }
    }

    // 换文件时先停掉上一段序列的连续播放，避免旧定时器继续推进新文件。
    StopRawPlayback();

    raw_image_path_ = filename;
    raw_pixel_format_ = selected;
    raw_width_ = width;
    raw_height_ = height;
    raw_frame_size_ = frame_size;
    raw_total_frames_ = total_frames;
    raw_current_frame_ = 0;
    raw_fps_ = fps;

    const qint64 remain = file_size % frame_size;
    emit RawImageInfoReady(tr("Raw Image Sequence\n"
                              "File: %1\n"
                              "Format: %2\n"
                              "Size: %3x%4\n"
                              "Color Matrix: %5 (auto)\n"
                              "Frame Rate: %6 fps\n"
                              "Frame Size: %7 bytes\n"
                              "Total Frames: %8\n"
                              "Ignored Tail Bytes: %9")
                               .arg(filename)
                               .arg(selected)
                               .arg(width)
                               .arg(height)
                               .arg(height >= 720 ? QStringLiteral("BT.709") : QStringLiteral("BT.601"))
                               .arg(raw_fps_)
                               .arg(frame_size)
                               .arg(raw_total_frames_)
                               .arg(remain));

    return ShowRawFrame(0);
}

bool RawImageSequence::ShowRawFrame(int frame_index) {
    if (!showing_raw_image_ || raw_image_path_.isEmpty() || raw_frame_size_ <= 0 || frame_index < 0 ||
        frame_index >= raw_total_frames_) {
        return false;
    }

    // 三处失败都不弹模态框了：原始帧是逐帧/拖动连续到达的，弹窗会把 UI 直接卡死
    // （拖动滑块时一个 MessageBox 叠一个，窗口再也点不动）。原因统一走状态栏。
    QFile file(raw_image_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        emit StatusMessage(tr("无法读取文件: %1").arg(raw_image_path_), 0);
        return false;
    }

    const qint64 offset = static_cast<qint64>(frame_index) * raw_frame_size_;
    if (!file.seek(offset)) {
        emit StatusMessage(tr("无法定位到第 %1 帧。").arg(frame_index + 1), 0);
        return false;
    }

    const QByteArray data = file.read(raw_frame_size_);
    file.close();
    if (data.size() != raw_frame_size_) {
        emit StatusMessage(tr("读取第 %1 帧失败，期望 %2 字节，实际 %3 字节。")
                               .arg(frame_index + 1)
                               .arg(raw_frame_size_)
                               .arg(data.size()),
                           0);
        return false;
    }

    auto clamp_to_u8 = [](int value) -> uchar { return static_cast<uchar>(qBound(0, value, 255)); };
    // 裸流没有容器级色彩元数据。按分辨率自动选择矩阵（业界默认约定，swscale 亦如此）：
    // 高度 >= 720 视为 HD 走 BT.709，其余走 BT.601。选错只会偏色，不会崩。
    const bool bt709 = raw_height_ >= 720;
    auto yuv_to_rgb = [&](int Y, int U, int V, uchar* dst) {
        const int u = U - 128;
        const int v = V - 128;
        int r = 0;
        int g = 0;
        int b = 0;
        if (bt709) {
            r = Y + static_cast<int>(1.5748 * v);
            g = Y - static_cast<int>(0.1873 * u + 0.4681 * v);
            b = Y + static_cast<int>(1.8556 * u);
        } else {
            r = Y + static_cast<int>(1.402 * v);
            g = Y - static_cast<int>(0.344136 * u + 0.714136 * v);
            b = Y + static_cast<int>(1.772 * u);
        }
        dst[0] = clamp_to_u8(r);
        dst[1] = clamp_to_u8(g);
        dst[2] = clamp_to_u8(b);
    };

    QImage img(raw_width_, raw_height_, QImage::Format_RGB888);
    if (img.isNull()) {
        return false;
    }

    const uchar* src = reinterpret_cast<const uchar*>(data.constData());
    if (raw_pixel_format_ == "RGB24" || raw_pixel_format_ == "BGR24") {
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
    } else if (raw_pixel_format_ == "GRAY8") {
        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const uchar* src_row = src + static_cast<qint64>(j) * raw_width_;
            for (int i = 0; i < raw_width_; ++i) {
                const uchar y = src_row[i];
                row[i * 3 + 0] = y;
                row[i * 3 + 1] = y;
                row[i * 3 + 2] = y;
            }
        }
    } else if (raw_pixel_format_ == "YUV420P(I420)" || raw_pixel_format_ == "NV12" || raw_pixel_format_ == "NV21" ||
               raw_pixel_format_ == "YV12") {
        const qint64 y_size = static_cast<qint64>(raw_width_) * static_cast<qint64>(raw_height_);
        const uchar* y_plane = src;
        const uchar* uv_plane = y_plane + y_size;
        const int chroma_w = raw_width_ / 2;
        const int chroma_h = raw_height_ / 2;
        const qint64 chroma_plane_size = static_cast<qint64>(chroma_w) * chroma_h;
        // 三种排布：I420(U 平面, V 平面) / YV12(V 平面, U 平面) / NV12(UV 交织) / NV21(VU 交织)
        const bool interleaved = raw_pixel_format_ == "NV12" || raw_pixel_format_ == "NV21";
        const bool vu_first = raw_pixel_format_ == "YV12" || raw_pixel_format_ == "NV21";

        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            for (int i = 0; i < raw_width_; ++i) {
                const int Y = static_cast<int>(y_plane[static_cast<qint64>(j) * raw_width_ + i]);
                int c0 = 0;
                int c1 = 0;
                if (interleaved) {
                    const qint64 uv_idx = static_cast<qint64>(j / 2) * raw_width_ + (i / 2) * 2;
                    c0 = static_cast<int>(uv_plane[uv_idx]);
                    c1 = static_cast<int>(uv_plane[uv_idx + 1]);
                } else {
                    const qint64 uv_idx = static_cast<qint64>(j / 2) * chroma_w + (i / 2);
                    c0 = static_cast<int>(uv_plane[uv_idx]);
                    c1 = static_cast<int>(uv_plane[chroma_plane_size + uv_idx]);
                }
                const int U = vu_first ? c1 : c0;
                const int V = vu_first ? c0 : c1;
                yuv_to_rgb(Y, U, V, row + i * 3);
            }
        }
    } else if (raw_pixel_format_ == "P010" || raw_pixel_format_ == "YUV420P10LE") {
        const bool p010 = raw_pixel_format_ == "P010";
        const qint64 y_bytes = static_cast<qint64>(raw_width_) * static_cast<qint64>(raw_height_) * 2;
        const uchar* y_plane = src;
        const uchar* uv_plane = y_plane + y_bytes;
        const qint64 chroma_samples = static_cast<qint64>(raw_width_ / 2) * (raw_height_ / 2);
        // 10bit 存放在 16bit 小端样本的高 10 位（低 6 位为填充），>>2 归一到 8bit。
        auto sample10 = [](const uchar* p) -> int {
            return (static_cast<int>(p[0]) | (static_cast<int>(p[1]) << 8)) >> 2;
        };

        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            for (int i = 0; i < raw_width_; ++i) {
                const int Y = sample10(y_plane + (static_cast<qint64>(j) * raw_width_ + i) * 2);
                int U = 0;
                int V = 0;
                if (p010) {
                    // UV 交织：半高行，每行 w 个样本（U/V 成对），每个样本 2 字节
                    const qint64 uv_idx = (static_cast<qint64>(j / 2) * raw_width_ + (i / 2) * 2) * 2;
                    U = sample10(uv_plane + uv_idx);
                    V = sample10(uv_plane + uv_idx + 2);
                } else {
                    const qint64 uv_idx = static_cast<qint64>(j / 2) * (raw_width_ / 2) + (i / 2);
                    U = sample10(uv_plane + uv_idx * 2);
                    V = sample10(uv_plane + (chroma_samples + uv_idx) * 2);
                }
                yuv_to_rgb(Y, U, V, row + i * 3);
            }
        }
    } else if (raw_pixel_format_ == "YUY2" || raw_pixel_format_ == "UYVY") {
        const bool uyvy = raw_pixel_format_ == "UYVY";
        for (int j = 0; j < raw_height_; ++j) {
            uchar* row = img.scanLine(j);
            const uchar* src_row = src + static_cast<qint64>(j) * raw_width_ * 2;
            for (int i = 0; i < raw_width_; i += 2) {
                const int idx = i * 2;
                int y0 = 0;
                int y1 = 0;
                int u = 0;
                int v = 0;
                if (uyvy) {
                    u = static_cast<int>(src_row[idx + 0]);
                    y0 = static_cast<int>(src_row[idx + 1]);
                    v = static_cast<int>(src_row[idx + 2]);
                    y1 = static_cast<int>(src_row[idx + 3]);
                } else {
                    y0 = static_cast<int>(src_row[idx + 0]);
                    u = static_cast<int>(src_row[idx + 1]);
                    y1 = static_cast<int>(src_row[idx + 2]);
                    v = static_cast<int>(src_row[idx + 3]);
                }
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
    if (video_widget_) {
        video_widget_->SetFrame(img);
    }
    // Raw 序列没有编码可言，叠加层用像素格式占位 + 实际帧尺寸。
    // overlay 状态归面板所有：通过 RawFrameShown 让面板写 last_overlay_codec_ 并刷新叠加层。
    emit RawFrameShown(img, raw_pixel_format_);
    UpdateRawNavigationState();
    // 连续播放时每帧都刷状态栏会闪个不停，交回给面板的播放状态提示即可。
    if (!raw_playing_) {
        emit StatusMessage(tr("Raw 帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_), 0);
    }
    return true;
}

void RawImageSequence::UpdateRawNavigationState() {
    const bool raw_mode = showing_raw_image_ && raw_total_frames_ > 0;

    if (control_bar_) {
        if (auto* b = control_bar_->prevFrameButton()) {
            b->setVisible(raw_mode);
            b->setEnabled(raw_mode && raw_current_frame_ > 0);
        }
        if (auto* b = control_bar_->nextFrameButton()) {
            b->setVisible(raw_mode);
            b->setEnabled(raw_mode && raw_current_frame_ + 1 < raw_total_frames_);
        }
        if (auto* b = control_bar_->playPauseButton()) {
            // Raw 支持连续播放（单帧文件除外）；音量仍不适用。
            b->setEnabled(!raw_mode || raw_total_frames_ > 1);
        }
        if (auto* b = control_bar_->volumeButton()) {
            b->setEnabled(!raw_mode);
        }
        if (auto* s = control_bar_->volumeSlider()) {
            s->setEnabled(!raw_mode);
        }

        if (raw_mode) {
            QSignalBlocker blocker(control_bar_->seekSlider());
            control_bar_->seekSlider()->setRange(0, std::max(0, raw_total_frames_ - 1));
            control_bar_->seekSlider()->setValue(raw_current_frame_);
            control_bar_->seekSlider()->setEnabled(raw_total_frames_ > 1);
            control_bar_->timeLabel()->setText(
                raw_playing_ ? tr("帧 %1 / %2 (播放中)").arg(raw_current_frame_ + 1).arg(raw_total_frames_)
                             : tr("帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_));
        } else {
            control_bar_->seekSlider()->setEnabled(true);
            control_bar_->timeLabel()->setText(tr("00:00:00 / 00:00:00"));
        }
    }
}

void RawImageSequence::OnPrevRawFrame() {
    if (showing_raw_image_ && raw_current_frame_ > 0) {
        ShowRawFrame(raw_current_frame_ - 1);
    }
}

void RawImageSequence::OnNextRawFrame() {
    if (showing_raw_image_ && raw_current_frame_ + 1 < raw_total_frames_) {
        ShowRawFrame(raw_current_frame_ + 1);
    }
}

bool RawImageSequence::StartRawPlayback() {
    if (!showing_raw_image_ || raw_total_frames_ <= 1 || raw_frame_size_ <= 0) {
        return false;
    }
    if (raw_playing_) {
        return true;
    }
    // 停在末帧时再点播放：从头开始，避免"点了没反应"。
    if (raw_current_frame_ + 1 >= raw_total_frames_ && !ShowRawFrame(0)) {
        return false;
    }
    raw_playing_ = true;
    playback_timer_->start(std::max(1, 1000 / std::max(1, raw_fps_)));
    UpdateRawNavigationState();
    emit RawPlaybackStateChanged(true);
    emit StatusMessage(tr("开始连续播放 (%1 fps)").arg(raw_fps_), 2000);
    return true;
}

void RawImageSequence::StopRawPlayback() {
    if (!raw_playing_) {
        return;
    }
    raw_playing_ = false;
    if (playback_timer_) {
        playback_timer_->stop();
    }
    UpdateRawNavigationState();
    emit RawPlaybackStateChanged(false);
}

void RawImageSequence::AdvancePlaybackFrame() {
    if (!raw_playing_) {
        return;
    }
    // 停在末帧：不循环，避免掩盖序列尾部数据。
    if (raw_current_frame_ + 1 >= raw_total_frames_) {
        StopRawPlayback();
        return;
    }
    ShowRawFrame(raw_current_frame_ + 1);
}

void RawImageSequence::SetRawImageMode(bool on) {
    if (!on) {
        StopRawPlayback();
    }
    showing_raw_image_ = on;
}

void RawImageSequence::Reset() {
    StopRawPlayback();
    showing_raw_image_ = false;
    raw_image_path_.clear();
    raw_pixel_format_.clear();
    raw_width_ = 0;
    raw_height_ = 0;
    raw_frame_size_ = 0;
    raw_total_frames_ = 0;
    raw_current_frame_ = 0;
    raw_fps_ = 25;
}

} // namespace ui
} // namespace videoeye
