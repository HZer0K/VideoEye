#pragma once

#include <QObject>
#include <QString>
#include <atomic>

namespace videoeye {
namespace player {

// 视频帧导出器：将视频帧导出为 JPG/RGB/YUV 文件
class VideoFrameExporter : public QObject {
    Q_OBJECT

public:
    explicit VideoFrameExporter(QObject* parent = nullptr);

    void Export(const QString& url, const QString& output_dir,
                const QString& format, int jpg_quality, int frame_interval);
    void Cancel();
    bool IsExporting() const { return exporting_.load(); }

signals:
    void ExportStarted(int total_frames);
    void ExportProgress(int exported_frames);
    void ExportFinished(const QString& output_dir);
    void ExportCanceled(int exported_frames, const QString& output_dir);
    void ExportError(const QString& message);

private:
    std::atomic<bool> cancel_{false};
    std::atomic<bool> exporting_{false};
};

} // namespace player
} // namespace videoeye
