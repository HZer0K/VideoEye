#include "ui/player/RawImageSequence.h"

#include <QInputDialog>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>

#include <algorithm>

#include "ui/main_window/VideoWidget.h"
#include "ui/player/ControlBarWidget.h"

namespace videoeye {
namespace ui {

RawImageSequence::RawImageSequence(QWidget* parent)
    : QObject(parent), dialog_parent_(parent) {}

void RawImageSequence::SetVideoWidget(VideoWidget* widget) {
    video_widget_ = widget;
}

void RawImageSequence::SetControlBar(ControlBarWidget* bar) {
    control_bar_ = bar;
}

bool RawImageSequence::LoadRawImageFile(const QString& filename) {
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
    const int width = QInputDialog::getInt(dialog_parent_, tr("图像宽度"),
                                           tr("请输入宽度(px):"),
                                           default_width, 1, 16384, 1, &ok);
    if (!ok) {
        return false;
    }
    const int height = QInputDialog::getInt(dialog_parent_, tr("图像高度"),
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

    const QString selected = QInputDialog::getItem(dialog_parent_, tr("原始图像格式"),
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
            QMessageBox::warning(dialog_parent_, tr("尺寸不支持"),
                                 tr("%1 仅支持偶数宽高。").arg(selected));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 3 / 2;
    } else if (selected == "YUY2") {
        if ((width % 2) != 0) {
            QMessageBox::warning(dialog_parent_, tr("尺寸不支持"),
                                 tr("YUY2 仅支持偶数宽度。"));
            return false;
        }
        frame_size = static_cast<qint64>(width) * static_cast<qint64>(height) * 2;
    }

    const qint64 file_size = QFileInfo(filename).size();
    if (frame_size <= 0 || file_size < frame_size) {
        QMessageBox::warning(dialog_parent_, tr("打开失败"),
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

bool RawImageSequence::ShowRawFrame(int frame_index) {
    if (!showing_raw_image_ || raw_image_path_.isEmpty() || raw_frame_size_ <= 0 ||
        frame_index < 0 || frame_index >= raw_total_frames_) {
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
        emit StatusMessage(
            tr("读取第 %1 帧失败，期望 %2 字节，实际 %3 字节。")
                .arg(frame_index + 1)
                .arg(raw_frame_size_)
                .arg(data.size()), 0);
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
    if (video_widget_) {
        video_widget_->SetFrame(img);
    }
    // Raw 序列没有编码可言，叠加层用像素格式占位 + 实际帧尺寸。
    // overlay 状态归面板所有：通过 RawFrameShown 让面板写 last_overlay_codec_ 并刷新叠加层。
    emit RawFrameShown(img, raw_pixel_format_);
    UpdateRawNavigationState();
    emit StatusMessage(tr("Raw 帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_), 0);
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
            b->setEnabled(!raw_mode);
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
            control_bar_->timeLabel()->setText(tr("帧 %1 / %2").arg(raw_current_frame_ + 1).arg(raw_total_frames_));
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

void RawImageSequence::SetRawImageMode(bool on) {
    showing_raw_image_ = on;
}

void RawImageSequence::Reset() {
    showing_raw_image_ = false;
    raw_image_path_.clear();
    raw_pixel_format_.clear();
    raw_width_ = 0;
    raw_height_ = 0;
    raw_frame_size_ = 0;
    raw_total_frames_ = 0;
    raw_current_frame_ = 0;
}

} // namespace ui
} // namespace videoeye
