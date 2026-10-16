#pragma once

// 导出信号的对外契约：抽帧导出 5 条 + 音视频导出 5 条。
//
// 为什么从 MediaPlayer 门面上摘下来:
//   * 这十条信号跟"播放"毫无关系，却要让门面替它们背声明、背十条转发连接，
//     MediaPlayer.h 因此长期是"打开 + 播放 + 分析 + 导出"四份契约叠在一起；
//   * UI 里真正关心导出的只有 ExportCoordinator 一处。它现在连的是这根总线，
//     不再经过门面 —— 门面少十条信号，UI 侧的意图也更清楚。
//
// 由谁持有: ExportController（导出编排的唯一状态持有者，"该不该发 / 发第几代"
// 只有它知道），直接在本对象上发射；MediaPlayer 经 export_bus() 把引用转给 UI。
// 总线与控制器同线程（UI 线程），连接的投递方式与改之前一致。
//
// ⚠️ 信号名与参数列表一个字节都不许改 —— 它们是 UI 的既有契约。

#include <QObject>
#include <QString>

namespace videoeye {
namespace player {

class ExportBus : public QObject {
    Q_OBJECT

public:
    explicit ExportBus(QObject* parent = nullptr);

signals:
    void VideoFrameExportStarted(int total_frames);
    void VideoFrameExportProgress(int exported_frames);
    void VideoFrameExportFinished(const QString& output_dir);
    void VideoFrameExportCanceled(int exported_frames, const QString& output_dir);
    void VideoFrameExportError(const QString& message);

    void MediaExportStarted(qint64 duration_ms);
    void MediaExportProgress(int percent);
    void MediaExportFinished(const QString& output_path);
    void MediaExportCanceled(const QString& output_path);
    void MediaExportError(const QString& message);
};

}  // namespace player
}  // namespace videoeye
