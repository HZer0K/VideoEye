#pragma once

// 抽帧导出与音视频导出的全部编排：发起、排队、代际过滤、取消、线程生命周期。
//
// 为什么从 MediaPlayer 里拆出来: 这两条链路各自带一整套"容易写错但跟播放毫无关系"的知识 ——
//   * 抽帧的产物文件名只由帧序号决定，两个任务并行就会互相覆盖，所以新请求必须**排队**
//     等旧线程真的退出（不能只看"已请求停止"）；
//   * 取消、换文件、关窗三种"上下文变了"的时刻都必须推进代际号，否则旧任务迟到的
//     进度/完成/错误信号会把新任务的界面状态顶掉；
//   * 线程句柄绝不能丢（丢弃 = 宿主析构时 QThread 还在跑 = 崩溃），所以统一由
//     QtWorkerOwner 持有，"请求停止"和"真的结束"是两件事。
// 这些知识原先散在 MediaPlayer 的 10 个成员和 7 个方法里，跟播放状态机、分析开关混在一起，
// 谁也不敢动。收进来之后，MediaPlayer 只保留"转发入口 + 转发信号"。
//
// 本类是 QObject：worker 是 QObject（要发进度信号），信号连接需要一个接收者，
// 而导出信号的对外契约（名字与参数）必须与拆分前一个字节不差 —— 所以这里声明同名信号，
// 由 MediaPlayer 原样转发给 UI。
//
// 与 TaskManager 的关系（沿用拆分前的约定，不要改）:
//   * slot / 令牌 / 任务 id 一律从**调用方**的 TaskManager 取，本类只持引用；
//   * 终态的写入者从头到尾只有一个（本类）：exporter 的三条终态信号、任务体抛异常的
//     on_error、线程退出时的兜底，三条路都写同一个 slot，靠 End 的 id 匹配 + 一次性保证幂等。
//
// 生命周期: 析构前应先调 Shutdown()（立关机标志 + 取消全部 + 限时收线程）；
// 析构函数会再兜一次，交给 QtWorkerOwner 脱管而不是让线程在宿主销毁后继续跑。

#include <functional>
#include <optional>

#include <QObject>
#include <QString>
#include <QThread>

#include "core/domain/task/TaskProtocol.h"
#include "core/exporter/MediaExporter.h"
#include "core/player/VideoFrameExporter.h"
#include "core/qt/QtWorkerOwner.h"
#include "infrastructure/concurrency/TaskManager.h"

namespace videoeye {
namespace player {

class ExportController : public QObject {
    Q_OBJECT

public:
    // tasks: 调用方的任务调度器（slot / 令牌 / 终态都在那儿）。必须活过本对象。
    // current_url: 取"当前打开的媒体"。不直接存 QString 是因为排队请求续跑前要拿**此刻**
    //   的 url 与发起时的比对，换了媒体就得作废（见 StartVideoFrameExportNow 的续跑分支）。
    ExportController(task::TaskManager& tasks, std::function<QString()> current_url,
                     QObject* parent = nullptr);
    ~ExportController() override;

    ExportController(const ExportController&) = delete;
    ExportController& operator=(const ExportController&) = delete;

    // 关机 / 换媒体前调用: 立"不再续跑排队请求"的标志，取消两类导出，限时收线程。
    // 代际号一并推进，旧任务迟到的信号会被代际校验丢弃。
    void Shutdown(int stop_budget_ms);

    // 抽帧导出。若上一次抽帧的线程还没退出来，本次请求**排队**而不是立即启动：
    // 两次任务写同一个输出目录、文件名只由帧序号决定，并行跑会互相覆盖产物。
    // 排队请求会在旧线程真正结束的 finished 回调里自动续跑（用户不必再点一次）。
    void StartVideoFrameExport(const QString& output_dir, const QString& format,
                               int jpg_quality = 90, int frame_interval = 1);
    // 只置取消标志并请求线程退出，**不等待**：调用方在 UI 线程，绝不能被卡住。
    void CancelVideoFrameExport();

    // 音视频导出 (remux / transcode)。排队语义同 StartVideoFrameExport。
    void StartMediaExport(const exporter::ExportOptions& opt);
    void CancelMediaExport();

    // 取消**全部**导出（抽帧 + 音视频），并作废各自的排队请求与代际号。
    //
    // 谁该调它: 任何"媒体上下文要换了"的时刻 —— 打开新文件、关闭/析构。
    // 以前这件事散在 UI 里做（MainWindow::OpenMedia 只调了抽帧那一半），于是
    // "媒体导出进行中打开新文件"会留下一个仍在跑、且终态信号还能串回界面的旧任务。
    void CancelAllExports();

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

private:
    bool IsFrameExportWorkerAlive() const;
    bool IsMediaExportWorkerAlive() const;
    void StartVideoFrameExportNow(const QString& output_dir, const QString& format,
                                  int jpg_quality, int frame_interval);
    void StartMediaExportNow(const exporter::ExportOptions& opt);
    // 只请求旧线程停止（置取消标志 / 断开信号 / 不限时地"看一眼"是否已结束），
    // 绝不阻塞等待。排队请求由它保留；用户主动取消请走 CancelXxxExport()。
    void RequestStopFrameExport();
    void RequestStopMediaExport();

    // 抽帧 / 媒体导出各占一个后台任务 slot：同一 slot 上永远只有一个任务在跑
    // (见 infrastructure/concurrency/TaskManager.h)。
    static constexpr const char* kSlotFrameExport = "frame-export";
    static constexpr const char* kSlotMediaExport = "media-export";

    // 排队的导出请求（旧任务线程还没退出时用）。用 optional 而非裸指针:
    // 无请求、有请求、被覆盖都只有一处状态，不存在"忘了置空"。
    struct PendingFrameExport {
        QString url;          // 发起时打开的媒体: 续跑前若已换文件, 本次请求作废
        QString output_dir;
        QString format;
        int jpg_quality = 90;
        int frame_interval = 1;
    };

    task::TaskManager& tasks_;
    std::function<QString()> current_url_;

    // 抽帧 / 媒体导出的 worker 线程全部归 workers_ 所有: 取消超时也不许丢句柄
    // (丢弃 = 宿主析构时 QThread 还在跑 = 崩溃)，见 core/qt/QtWorkerOwner.h。
    // 下面两组裸指针只是"当前任务"的标记，线程结束后由 finished 回调清空，
    // 所有权不在它们身上。
    qt::QtWorkerOwner workers_;

    QThread* frame_export_thread_ = nullptr;
    VideoFrameExporter* frame_exporter_ = nullptr;
    // 每次发起抽帧导出自增的代际号: 转发信号时只接受当前代际,
    // 旧任务已排队到 UI 线程的终态信号(进度/完成/取消/错误)会因此被丢弃, 不会串到新任务。
    quint64 frame_export_gen_ = 0;

    std::optional<PendingFrameExport> pending_frame_export_;
    std::optional<exporter::ExportOptions> pending_media_export_;
    // 析构开始后不再续跑排队请求: 此时线程正在被 StopAll 收尾，
    // 若 finished 回调又起一个新线程，就等于在析构途中往 workers_ 里塞新 Entry。
    bool shutdown_ = false;

    QThread* media_export_thread_ = nullptr;
    exporter::MediaExporter* media_exporter_ = nullptr;
    quint64 media_export_gen_ = 0;
};

}  // namespace player
}  // namespace videoeye
