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
#include <string>

#include "core/player/RawFrameConverter.h"
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

RawImageSequence::~RawImageSequence() = default;

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

    // 帧大小与"这个尺寸合不合法"交给 core/player 判：与转换器共用同一份口径，
    // 免得这里放过、转换器那边又拒绝（表现成"打开了但一片黑"）。
    player::RawPixelFormat format_kind = player::RawPixelFormat::I420;
    if (!player::ParseRawPixelFormatLabel(selected.toStdString(), format_kind)) {
        return false;
    }
    std::string size_reason;
    const qint64 frame_size = static_cast<qint64>(player::RawFrameSizeBytes(width, height, format_kind, &size_reason));
    if (frame_size <= 0) {
        QMessageBox::warning(dialog_parent_, tr("尺寸不支持"), QString::fromStdString(size_reason));
        return false;
    }

    const qint64 file_size = QFileInfo(filename).size();
    if (file_size < frame_size) {
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

    return OpenRawSequence(filename, width, height, selected, fps);
}

bool RawImageSequence::OpenRawSequence(const QString& path, int width, int height, const QString& format_label,
                                       int fps) {
    player::RawPixelFormat format_kind = player::RawPixelFormat::I420;
    if (!player::ParseRawPixelFormatLabel(format_label.toStdString(), format_kind)) {
        emit StatusMessage(tr("不支持的像素格式: %1").arg(format_label), 0);
        return false;
    }

    std::string size_reason;
    const qint64 frame_size = static_cast<qint64>(player::RawFrameSizeBytes(width, height, format_kind, &size_reason));
    if (frame_size <= 0) {
        emit StatusMessage(QString::fromStdString(size_reason), 0);
        return false;
    }

    const qint64 file_size = QFileInfo(path).size();
    if (file_size < frame_size) {
        emit StatusMessage(tr("文件大小不足以组成一帧（单帧 %1 字节，文件 %2 字节）。").arg(frame_size).arg(file_size),
                           0);
        return false;
    }

    // 上下文建不起来 = 这台机器的 FFmpeg 用不了该像素格式。宁可当场报错，
    // 也别先上屏再说 —— 那样用户看到的是一片黑，分不清是格式选错还是文件坏了。
    auto converter = std::make_unique<player::RawFrameConverter>(width, height, format_kind, height >= 720);
    if (!converter->Valid()) {
        emit StatusMessage(tr("无法为该尺寸与像素格式建立色彩转换: %1x%2 %3").arg(width).arg(height).arg(format_label),
                           0);
        return false;
    }

    // 换序列：旧句柄、旧上下文一起换掉（旧定时器也不能继续推进新文件）。
    StopRawPlayback();
    CloseRawFile();
    // 画面上的 QImage 只是转换器内部缓冲的视图，缓冲要换掉之前必须先清屏，
    // 否则 paintEvent 会读到已经释放的内存。
    if (video_widget_ != nullptr) {
        video_widget_->Clear();
    }
    converter_ = std::move(converter);

    raw_image_path_ = path;
    raw_pixel_format_ = format_label;
    raw_width_ = width;
    raw_height_ = height;
    raw_frame_size_ = frame_size;
    raw_total_frames_ = std::max(1, static_cast<int>(file_size / frame_size));
    raw_current_frame_ = 0;
    raw_fps_ = std::max(1, fps);
    // 弹窗路径是 PlayerPanel::SetRawImageMode(true) 先进模式；直接调本函数（测试 /
    // 将来的"最近打开"）时也得进，否则 ShowRawFrame 的第一道守卫会把它挡回去。
    showing_raw_image_ = true;

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
                               .arg(path)
                               .arg(format_label)
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
    if (!EnsureRawFileOpen()) {
        return false;
    }

    const qint64 offset = static_cast<qint64>(frame_index) * raw_frame_size_;
    // 顺序读（下一帧 / 连续播放）时游标本来就在这一帧开头，那次 seek 是白送的系统调用。
    if (raw_next_offset_ != offset) {
        if (!raw_file_->seek(offset)) {
            // 游标位置已经不可信，关掉让下次重开 —— 别在错位置上继续读出一堆垃圾像素。
            CloseRawFile();
            emit StatusMessage(tr("无法定位到第 %1 帧。").arg(frame_index + 1), 0);
            return false;
        }
        raw_next_offset_ = -1;
    }

    frame_buffer_.resize(static_cast<std::size_t>(raw_frame_size_));
    const qint64 read_bytes = raw_file_->read(reinterpret_cast<char*>(frame_buffer_.data()), raw_frame_size_);
    if (read_bytes != raw_frame_size_) {
        CloseRawFile();
        emit StatusMessage(tr("读取第 %1 帧失败，期望 %2 字节，实际 %3 字节。")
                               .arg(frame_index + 1)
                               .arg(raw_frame_size_)
                               .arg(read_bytes),
                           0);
        return false;
    }
    raw_next_offset_ = offset + raw_frame_size_;

    // 裸字节 -> RGB888 交给 core/player/RawFrameConverter（sws_scale）。原先这里逐像素
    // 手写 BT.601/709 系数：1080p 一帧两百万次浮点乘法，而且把 Y 当**全范围**处理，
    // 16/235 的黑白电平该压的地方没压（黑电平发灰）。范围与矩阵现在由 sws 显式设置，
    // 口径与 `ffmpeg -i in.yuv -f rawvideo out.rgb` 一致。
    if (converter_ == nullptr || !converter_->Convert(frame_buffer_.data(), frame_buffer_.size())) {
        emit StatusMessage(tr("第 %1 帧色彩转换失败。").arg(frame_index + 1), 0);
        return false;
    }

    // QImage 只在这里包一层视图，不拷贝：缓冲归转换器（常驻，尾部带 sws 需要的余量），
    // 下一次 Convert 会覆盖它。上屏链路上的消费者都不长期持有这份像素 —— VideoWidget
    // 存 QImage 后立刻按控件尺寸缩放，叠加层只读宽高（见 RawFrameShown 的接收方）。
    QImage img(converter_->RgbData(), raw_width_, raw_height_, static_cast<int>(converter_->Stride()),
               QImage::Format_RGB888);
    if (img.isNull()) {
        emit StatusMessage(tr("无法封装 %1x%2 的图像。").arg(raw_width_).arg(raw_height_), 0);
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

bool RawImageSequence::EnsureRawFileOpen() {
    if (raw_file_ != nullptr && raw_file_->isOpen()) {
        return true;
    }
    auto file = std::make_unique<QFile>(raw_image_path_);
    if (!file->open(QIODevice::ReadOnly)) {
        emit StatusMessage(tr("无法读取文件: %1").arg(raw_image_path_), 0);
        return false;
    }
    raw_file_ = std::move(file);
    raw_next_offset_ = -1; // 新句柄的游标在 0，别让上一段序列的位置假设留下来
    return true;
}

void RawImageSequence::CloseRawFile() {
    if (raw_file_ != nullptr) {
        raw_file_->close();
        raw_file_.reset();
    }
    raw_next_offset_ = -1;
}

void RawImageSequence::SetRawImageMode(bool on) {
    if (!on) {
        StopRawPlayback();
        // 退出 Raw 模式后不会再读这个文件，句柄留着只会挡住删除/改名（Windows 上是独占的）。
        CloseRawFile();
    }
    showing_raw_image_ = on;
}

void RawImageSequence::Reset() {
    StopRawPlayback();
    CloseRawFile();
    if (video_widget_ != nullptr) {
        video_widget_->Clear(); // 同上：先撤掉视图，再释放它引用的缓冲
    }
    converter_.reset();
    frame_buffer_.clear();
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
