#pragma once

#include <QObject>
#include <QString>
#include <atomic>

#include "infrastructure/concurrency/TaskManager.h"

namespace videoeye {
namespace player {

// 视频帧导出器：将视频帧导出为 JPG/RGB/YUV 文件
class VideoFrameExporter : public QObject {
    Q_OBJECT

public:
    explicit VideoFrameExporter(QObject* parent = nullptr);

    // 取消令牌由所有者在**线程启动之前**注入。
    //
    // 为什么不能让 Export() 自己重置取消标志: 用户可能在 worker 线程还没跑起来时就点了
    // 取消 —— 原实现在 Export() 开头写 cancel_ = false，于是"立即取消"被吃掉，
    // 导出照常跑完全片。现在取消状态只由外部写（Cancel() / 令牌），Export() 只读。
    void SetCancelToken(const task::CancelToken& token) { cancel_token_ = token; }

    void Export(const QString& url, const QString& output_dir,
                const QString& format, int jpg_quality, int frame_interval);
    void Cancel();
    bool IsExporting() const { return exporting_.load(); }
    bool IsCanceled() const;

signals:
    void ExportStarted(int total_frames);
    void ExportProgress(int exported_frames);
    void ExportFinished(const QString& output_dir);
    void ExportCanceled(int exported_frames, const QString& output_dir);
    void ExportError(const QString& message);

private:
    // 外部直接置位（Cancel()）与 TaskManager 令牌取或：两者任一置位都算取消。
    std::atomic<bool> cancel_{false};
    task::CancelToken cancel_token_;
    std::atomic<bool> exporting_{false};
};

} // namespace player
} // namespace videoeye
