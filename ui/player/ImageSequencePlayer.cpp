#include "ui/player/ImageSequencePlayer.h"

#include "core/domain/io/ImageFolderScan.h"
#include "core/domain/model/ImageBudget.h"
#include "ui/main_window/VideoWidget.h"
#include "ui/player/ControlBarWidget.h"
#include "ui/player/ImageFileOrientation.h"
#include "ui/player/ImageOrientationTransform.h"

#include <QFileInfo>
#include <QImageReader>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>

#include <algorithm>
#include <vector>

namespace videoeye {
namespace ui {

namespace {
// 未指定帧率时的默认值（与抽帧导出的默认 25fps 一致）。
constexpr int kDefaultFps = 25;
// 序列里至少要有这么多张："序列"才成立。只有一张时直接打开文件更好 ——
// 那条路径有完整分析，而这里只做预览。
constexpr int kMinSequenceFrames = 2;
} // namespace

ImageSequencePlayer::ImageSequencePlayer(QObject* parent) : QObject(parent) {
    tick_timer_ = new QTimer(this);
    tick_timer_->setSingleShot(true);
    connect(tick_timer_, &QTimer::timeout, this, &ImageSequencePlayer::OnTick);
}

void ImageSequencePlayer::SetVideoWidget(VideoWidget* widget) {
    video_widget_ = widget;
}

void ImageSequencePlayer::SetControlBar(ControlBarWidget* bar) {
    control_bar_ = bar;
}

bool ImageSequencePlayer::Load(const QString& folder, int fps) {
    Reset();
    const std::vector<std::string> files = io::ListImagesInFolder(folder.toStdString());
    if (files.empty()) {
        emit StatusMessage(tr("这个目录里没有可识别的图片: %1").arg(folder), 0);
        return false;
    }
    if (static_cast<int>(files.size()) < kMinSequenceFrames) {
        const QString only = QFileInfo(QString::fromStdString(files.front())).fileName();
        emit StatusMessage(tr("目录里只有 1 张图片，请直接打开这个文件: %1").arg(only), 0);
        return false;
    }
    for (const std::string& path : files) {
        files_.append(QString::fromStdString(path));
    }
    folder_ = folder;
    fps_ = fps > 0 ? fps : kDefaultFps;

    // 先解首帧：目录里全是 Qt 读不了的格式（webp/tiff/jxl…）时，必须在"打开成功"
    // 之前就说清楚，而不是给一个黑画面让用户以为序列是空的。
    QString reason;
    const QImage first = DecodeFrame(0, reason);
    if (first.isNull()) {
        Reset();
        emit StatusMessage(reason, 0);
        return false;
    }
    current_index_ = 0;
    if (video_widget_) {
        video_widget_->SetFrame(first);
        video_widget_->ResetZoom();
    }
    UpdateNavigation();
    emit FrameShown(first, tr("图片序列"));
    emit StatusMessage(tr("图片序列: %1 张 @ %2 fps").arg(files_.size()).arg(fps_), 3000);
    return true;
}

void ImageSequencePlayer::Reset() {
    Stop();
    files_.clear();
    folder_.clear();
    current_index_ = -1;
    fps_ = kDefaultFps;
    UpdateNavigation();
}

QImage ImageSequencePlayer::DecodeFrame(int index, QString& reason) const {
    reason.clear();
    if (index < 0 || index >= files_.size()) {
        reason = tr("帧号越界");
        return QImage();
    }
    const QString path = files_.at(index);
    QImageReader reader(path);
    // 方向由 core 的解析器统一给（Qt 的 autoTransform 依赖图片插件读 EXIF，
    // 本构建只部署了 qgif/qico/qjpeg，用在 TIFF/WebP 上等于假支持）。
    reader.setAutoTransform(false);
    const QSize size = reader.size();
    if (model::ExceedsImagePixelBudget(size.width(), size.height())) {
        reason = tr("%1 超出渲染预算: %2")
                     .arg(QFileInfo(path).fileName())
                     .arg(QString::fromStdString(model::FormatPixelBudget(size.width(), size.height())));
        return QImage();
    }
    QImage image = reader.read();
    if (image.isNull()) {
        // 最常见的原因是 Qt 没有这个格式的解码插件（webp/tiff/jxl/qoi/exr…）。
        // 明确报"不支持"而不是"损坏"，否则用户会去查文件而不是换工具。
        const QString detail = reader.errorString();
        reason = tr("Qt 无法解码 %1（序列预览只支持 Qt 内置格式）: %2")
                     .arg(QFileInfo(path).fileName(), detail.isEmpty() ? tr("未知原因") : detail);
        return QImage();
    }
    const QTransform transform = OrientationToTransform(ReadImageOrientationFromFile(path));
    if (!transform.isIdentity()) {
        image = image.transformed(transform, Qt::SmoothTransformation);
    }
    return image;
}

bool ImageSequencePlayer::ShowFrame(int index) {
    if (files_.isEmpty() || index < 0 || index >= files_.size()) {
        return false;
    }
    QString reason;
    const QImage image = DecodeFrame(index, reason);
    if (image.isNull()) {
        emit StatusMessage(reason, 0);
        return false;
    }
    current_index_ = index;
    if (video_widget_) {
        video_widget_->SetFrame(image);
    }
    emit FrameShown(image, tr("图片序列"));
    UpdateNavigation();
    if (!playing_) {
        emit StatusMessage(tr("图片序列 %1 / %2").arg(current_index_ + 1).arg(files_.size()), 0);
    }
    return true;
}

bool ImageSequencePlayer::Start() {
    if (files_.size() < kMinSequenceFrames) {
        return false;
    }
    if (playing_) {
        return true;
    }
    // 停在末帧时再点播放：从头开始，避免"点了没反应"。
    if (current_index_ + 1 >= files_.size() && !ShowFrame(0)) {
        return false;
    }
    playing_ = true;
    tick_timer_->start(std::max(1, 1000 / std::max(1, fps_)));
    UpdateNavigation();
    emit PlaybackStateChanged(true);
    emit StatusMessage(tr("开始播放图片序列 (%1 fps)").arg(fps_), 2000);
    return true;
}

void ImageSequencePlayer::Stop() {
    if (!playing_) {
        if (tick_timer_) {
            tick_timer_->stop();
        }
        return;
    }
    playing_ = false;
    if (tick_timer_) {
        tick_timer_->stop();
    }
    UpdateNavigation();
    emit PlaybackStateChanged(false);
}

void ImageSequencePlayer::OnTick() {
    if (!playing_) {
        return;
    }
    if (current_index_ + 1 >= files_.size()) {
        Stop(); // 末帧自停，不循环
        return;
    }
    if (ShowFrame(current_index_ + 1)) {
        tick_timer_->start(std::max(1, 1000 / std::max(1, fps_)));
        return;
    }
    // 中间某一帧读不出来：停止并保留上一帧画面，别一路报错刷屏。
    Stop();
}

void ImageSequencePlayer::UpdateNavigation() {
    if (control_bar_ == nullptr) {
        return;
    }
    const bool active = !files_.isEmpty();
    if (auto* b = control_bar_->prevFrameButton()) {
        b->setVisible(active);
        b->setEnabled(active && current_index_ > 0);
        b->setToolTip(active ? tr("上一帧（%1/%2）").arg(current_index_ + 1).arg(files_.size()) : QString());
    }
    if (auto* b = control_bar_->nextFrameButton()) {
        b->setVisible(active);
        b->setEnabled(active && current_index_ + 1 < files_.size());
        b->setToolTip(active ? tr("下一帧（%1/%2）").arg(current_index_ + 1).arg(files_.size()) : QString());
    }
    if (auto* b = control_bar_->playPauseButton()) {
        b->setEnabled(!active || files_.size() > 1);
    }
    if (auto* b = control_bar_->volumeButton()) {
        b->setEnabled(!active); // 图片序列没有音轨
    }
    if (auto* s = control_bar_->volumeSlider()) {
        s->setEnabled(!active);
    }
    if (auto* slider = control_bar_->seekSlider()) {
        if (active) {
            QSignalBlocker blocker(slider);
            slider->setRange(0, std::max(0, static_cast<int>(files_.size()) - 1));
            slider->setValue(std::max(0, current_index_));
            slider->setEnabled(files_.size() > 1);
        }
    }
    if (auto* label = control_bar_->timeLabel()) {
        if (active) {
            label->setText(playing_ ? tr("帧 %1 / %2 (播放中)").arg(current_index_ + 1).arg(files_.size())
                                    : tr("帧 %1 / %2").arg(current_index_ + 1).arg(files_.size()));
        } else {
            label->setText(tr("00:00:00 / 00:00:00"));
        }
    }
    // 循环按钮对"目录序列"没有意义（末帧自停是刻意的，见 Start()）。
    if (auto* loop = control_bar_->loopButton()) {
        loop->setVisible(false);
        loop->setChecked(false);
    }
}

} // namespace ui
} // namespace videoeye
