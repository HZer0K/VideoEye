#include "ui/main_window/ExportCoordinator.h"

#include "ui/dialogs/MediaExportDialog.h"
#include "core/exporter/MediaExporter.h"
#include "core/player/MediaPlayer.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressDialog>

#include <algorithm>

namespace videoeye {
namespace ui {

ExportCoordinator::ExportCoordinator(player::MediaPlayer* player, QWidget* dialog_parent,
                                     QObject* parent)
    : QObject(parent), player_(player), dialog_parent_(dialog_parent) {
    if (!player_) {
        return;
    }
    // 播放器导出信号自连 (原 MainWindow::SetupConnections 里的导出块整体迁来;
    // OpenFinished 等其余播放器信号仍归 MainWindow)。
    connect(player_, &player::MediaPlayer::VideoFrameExportStarted,
            this, &ExportCoordinator::OnVideoFrameExportStarted);
    connect(player_, &player::MediaPlayer::VideoFrameExportProgress,
            this, &ExportCoordinator::OnVideoFrameExportProgress);
    connect(player_, &player::MediaPlayer::VideoFrameExportFinished,
            this, &ExportCoordinator::OnVideoFrameExportFinished);
    connect(player_, &player::MediaPlayer::VideoFrameExportCanceled,
            this, &ExportCoordinator::OnVideoFrameExportCanceled);
    connect(player_, &player::MediaPlayer::VideoFrameExportError,
            this, &ExportCoordinator::OnVideoFrameExportError);
    connect(player_, &player::MediaPlayer::MediaExportStarted,
            this, &ExportCoordinator::OnMediaExportStarted);
    connect(player_, &player::MediaPlayer::MediaExportProgress,
            this, &ExportCoordinator::OnMediaExportProgress);
    connect(player_, &player::MediaPlayer::MediaExportFinished,
            this, &ExportCoordinator::OnMediaExportFinished);
    connect(player_, &player::MediaPlayer::MediaExportCanceled,
            this, &ExportCoordinator::OnMediaExportCanceled);
    connect(player_, &player::MediaPlayer::MediaExportError,
            this, &ExportCoordinator::OnMediaExportError);
}

void ExportCoordinator::SetSourceQueries(SourceQuery source_query, RawImageQuery raw_image_query) {
    source_query_ = std::move(source_query);
    raw_image_query_ = std::move(raw_image_query);
}

void ExportCoordinator::HideProgress() {
    if (export_progress_dialog_) {
        export_progress_dialog_->reset();
        export_progress_dialog_->hide();
    }
}

void ExportCoordinator::ShowProgress(const QString& title, int maximum, const QString& label) {
    // 首次出现的导出类型决定进度框标题/取消按钮/量程; 复用已有框时只改量程 ——
    // 与拆分前两处 Started 的 if/else 语义一致 (两分支之后三行本就相同)。
    if (!export_progress_dialog_) {
        export_progress_dialog_ = new QProgressDialog(title, tr("终止"), 0, maximum, dialog_parent_);
        export_progress_dialog_->setWindowModality(Qt::ApplicationModal);
        export_progress_dialog_->setAutoClose(false);
        export_progress_dialog_->setAutoReset(false);
        connect(export_progress_dialog_, &QProgressDialog::canceled,
                this, &ExportCoordinator::OnProgressCanceled);
    } else {
        export_progress_dialog_->setMaximum(maximum);
    }
    export_progress_dialog_->setValue(0);
    export_progress_dialog_->setLabelText(label);
    export_progress_dialog_->show();
}

void ExportCoordinator::OnProgressCanceled() {
    if (!player_) return;
    emit StatusMessage(tr("正在终止导出..."));
    if (active_export_ == ActiveExport::Media) player_->CancelMediaExport();
    else player_->CancelVideoFrameExport();
}

// ===========================================================================
// 导出入口
// ===========================================================================

void ExportCoordinator::OnExportVideoFrames() {
    if (!player_) {
        return;
    }
    const QString source = source_query_ ? source_query_() : QString();
    if (source.isEmpty()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("请先打开一个视频文件"));
        return;
    }
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("正在导出中，请先终止或等待完成"));
        return;
    }
    active_export_ = ActiveExport::Frames;

    const QString dir = QFileDialog::getExistingDirectory(dialog_parent_, tr("选择导出目录"), "");
    if (dir.isEmpty()) {
        return;
    }

    bool ok = false;
    const QStringList items = {
        "jpg",
        "yuv",
        "rgb"
    };
    const QString format = QInputDialog::getItem(dialog_parent_, tr("导出格式"),
                                                 tr("选择导出格式:"),
                                                 items, 0, false, &ok);
    if (!ok || format.isEmpty()) {
        return;
    }

    int quality = 90;
    if (format == "jpg") {
        quality = QInputDialog::getInt(dialog_parent_, tr("JPG质量"),
                                       tr("JPG质量(1-100):"),
                                       90, 1, 100, 1, &ok);
        if (!ok) {
            return;
        }
    }

    const int interval = QInputDialog::getInt(dialog_parent_, tr("抽帧间隔"),
                                              tr("每 N 帧导出 1 帧 (N>=1):"),
                                              1, 1, 1000000, 1, &ok);
    if (!ok) {
        return;
    }

    emit StatusMessage(tr("开始导出视频帧..."));
    player_->StartVideoFrameExport(dir, format, quality, interval);
}

void ExportCoordinator::OnExportVideo() {
    const QString source = source_query_ ? source_query_() : QString();
    if (!player_ || source.isEmpty()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("请先打开一个视频文件"));
        return;
    }
    if (raw_image_query_ && raw_image_query_()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("当前为图像文件，无法导出视频"));
        return;
    }
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("正在导出中，请先终止或等待完成"));
        return;
    }
    active_export_ = ActiveExport::Media;
    ui::MediaExportDialog dlg(dialog_parent_, exporter::ExportKind::Video, source, player_->GetDuration());
    if (dlg.exec() != QDialog::Accepted) return;
    emit StatusMessage(tr("开始导出视频..."));
    player_->StartMediaExport(dlg.GetOptions());
}

void ExportCoordinator::OnExportAudio() {
    const QString source = source_query_ ? source_query_() : QString();
    if (!player_ || source.isEmpty()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("请先打开一个视频文件"));
        return;
    }
    if (raw_image_query_ && raw_image_query_()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("当前为图像文件，无法导出音频"));
        return;
    }
    if (export_progress_dialog_ && export_progress_dialog_->isVisible()) {
        QMessageBox::information(dialog_parent_, tr("提示"), tr("正在导出中，请先终止或等待完成"));
        return;
    }
    active_export_ = ActiveExport::Media;
    ui::MediaExportDialog dlg(dialog_parent_, exporter::ExportKind::Audio, source, player_->GetDuration());
    if (dlg.exec() != QDialog::Accepted) return;
    emit StatusMessage(tr("开始导出音频..."));
    player_->StartMediaExport(dlg.GetOptions());
}

// ===========================================================================
// 视频帧导出
// ===========================================================================

void ExportCoordinator::OnVideoFrameExportStarted(int total_frames) {
    export_total_frames_ = total_frames;
    // 总数未知 (<=0) 时量程取 0, 进度框进入 busy 档。
    ShowProgress(tr("正在导出视频帧..."), total_frames > 0 ? total_frames : 0,
                 tr("正在导出视频帧..."));
}

void ExportCoordinator::OnVideoFrameExportProgress(int exported_frames) {
    if (export_progress_dialog_) {
        if (export_total_frames_ > 0) {
            export_progress_dialog_->setMaximum(export_total_frames_);
            export_progress_dialog_->setValue(std::min(exported_frames, export_total_frames_));
        } else {
            export_progress_dialog_->setMaximum(0);
            export_progress_dialog_->setValue(0);
        }
        export_progress_dialog_->setLabelText(tr("已导出 %1 帧").arg(exported_frames));
    }
    emit StatusMessage(tr("已导出 %1 帧").arg(exported_frames));
}

void ExportCoordinator::OnVideoFrameExportFinished(const QString& output_dir) {
    HideProgress();
    emit StatusMessage(tr("导出完成: %1").arg(output_dir));
}

void ExportCoordinator::OnVideoFrameExportCanceled(int exported_frames, const QString& output_dir) {
    HideProgress();
    const QString msg = tr("已取消导出：已导出 %1 帧\n输出目录：%2").arg(exported_frames).arg(output_dir);
    emit StatusMessage(msg);
    QMessageBox::information(dialog_parent_, tr("导出已取消"), msg);
}

void ExportCoordinator::OnVideoFrameExportError(const QString& message) {
    HideProgress();
    emit StatusMessage(tr("导出失败: %1").arg(message));
    QMessageBox::warning(dialog_parent_, tr("导出失败"), message);
}

// ===========================================================================
// 音视频导出
// ===========================================================================

void ExportCoordinator::OnMediaExportStarted(qint64 duration_ms) {
    Q_UNUSED(duration_ms);
    ShowProgress(tr("正在导出..."), 100, tr("正在导出音视频..."));
}

void ExportCoordinator::OnMediaExportProgress(int percent) {
    // 音视频导出不刷状态栏 (帧号语义不适用), 只动进度框。
    if (export_progress_dialog_) {
        export_progress_dialog_->setMaximum(100);
        export_progress_dialog_->setValue(percent);
        export_progress_dialog_->setLabelText(tr("正在导出音视频... %1%").arg(percent));
    }
}

void ExportCoordinator::OnMediaExportFinished(const QString& output_path) {
    HideProgress();
    const QString msg = tr("导出完成: %1").arg(output_path);
    emit StatusMessage(msg);
    QMessageBox::information(dialog_parent_, tr("导出完成"), msg);
}

void ExportCoordinator::OnMediaExportCanceled(const QString& output_path) {
    HideProgress();
    const QString msg = tr("已取消导出：%1").arg(output_path);
    emit StatusMessage(msg);
    QMessageBox::information(dialog_parent_, tr("导出已取消"), msg);
}

void ExportCoordinator::OnMediaExportError(const QString& message) {
    HideProgress();
    emit StatusMessage(tr("导出失败: %1").arg(message));
    QMessageBox::warning(dialog_parent_, tr("导出失败"), message);
}

} // namespace ui
} // namespace videoeye