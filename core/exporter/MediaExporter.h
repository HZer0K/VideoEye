#pragma once

#include <QObject>
#include <QString>
#include <atomic>

namespace videoeye {
namespace exporter {

// 导出类型: 视频(含音轨) 或 纯音频
enum class ExportKind {
    Video,
    Audio
};

// 导出选项: 由 UI 构造, 传入 MediaExporter
struct ExportOptions {
    QString input_path;                 // 源文件
    QString output_path;                // 目标文件 (含扩展名, 决定容器)
    ExportKind kind = ExportKind::Video;
    QString format;                     // 目标格式扩展名: mp4/mkv/mov/avi/webm/ts/mp3/wav/m4a/flac/ogg
    bool reencode = false;              // 是否强制重新编码; false 时尽量直接 copy (无损且极快)
    int videoQuality = 1;               // 重编码画质: 0=高, 1=中, 2=低
    int audioBitrateKbps = 192;         // 重编码音频码率 (kbps)
    bool no_audio = false;              // 仅导出视频画面, 不包含音轨 (仅对 ExportKind::Video 生效)
    qint64 start_ms = -1;               // 片段起点; -1 = 从头
    qint64 end_ms = -1;                 // 片段终点; -1 = 到尾
};

// 音视频导出器 (remux / transcode)
// 在独立 QThread 中同步运行 Export(); 通过信号上报进度与结果。
class MediaExporter : public QObject {
    Q_OBJECT

public:
    explicit MediaExporter(QObject* parent = nullptr);

    // 同步执行导出 (应在 worker 线程中调用)
    void Export(const ExportOptions& opt);
    void Cancel();
    bool IsExporting() const { return exporting_.load(); }

signals:
    void ExportStarted(qint64 duration_ms);
    void ExportProgress(int percent);
    void ExportFinished(const QString& output_path);
    void ExportCanceled(const QString& output_path);
    void ExportError(const QString& message);

private:
    std::atomic<bool> cancel_{false};
    std::atomic<bool> exporting_{false};
};

} // namespace exporter
} // namespace videoeye
