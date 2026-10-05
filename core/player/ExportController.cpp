#include "core/player/ExportController.h"

#include <algorithm>
#include <string>
#include <utility>

#include <QDir>
#include <QMetaObject>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace player {

ExportController::ExportController(task::TaskManager& tasks, std::function<QString()> current_url,
                                   QObject* parent)
    : QObject(parent), tasks_(tasks), current_url_(std::move(current_url)) {}

ExportController::~ExportController() {
    // 兜底: 正常路径（换媒体 / 关闭）都应该先调过 Shutdown()。这里不再给等待预算 ——
    // 走到析构说明宿主已经在收尾，退不出来的线程交给 QtWorkerOwner 脱管
    // (泄漏一个卡死的线程，好过让它在宿主销毁后继续跑)。
    shutdown_ = true;
    workers_.StopAll(0);
}

void ExportController::Shutdown(int stop_budget_ms) {
    // 先立"关机"标志: 线程收尾触发的 finished 回调绝不能再续跑排队的导出请求,
    // 否则会在 workers_ 正被拆掉的时候往里塞新线程。
    shutdown_ = true;
    CancelAllExports();
    // 两类导出线程统一再收一轮: 由 workers_ 持有, 超时也不会被遗忘
    // (实在退不出来的会在它析构时脱管, 而不是被销毁)。
    workers_.StopAll(stop_budget_ms);
}

// --- 视频帧导出 (委托给 VideoFrameExporter) ---

bool ExportController::IsFrameExportWorkerAlive() const {
    // 只看线程是否还归 workers_ 持有: 取消超时后线程转入"待回收"但**仍被持有**,
    // 直到真的 finished 才从表里移除 —— 正好等于"还在往输出目录里写文件"。
    return frame_export_thread_ != nullptr && workers_.IsActive(frame_export_thread_);
}

bool ExportController::IsMediaExportWorkerAlive() const {
    return media_export_thread_ != nullptr && workers_.IsActive(media_export_thread_);
}

void ExportController::StartVideoFrameExport(const QString& output_dir, const QString& format,
                                             int jpg_quality, int frame_interval) {
    // 先把参数校验做完再动旧任务: 参数不合法时不该顺手打断正在跑的导出。
    if (current_url_().isEmpty()) { emit VideoFrameExportError("No media opened"); return; }
    if (output_dir.isEmpty()) { emit VideoFrameExportError("Output directory is empty"); return; }
    QDir dir(output_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        emit VideoFrameExportError(QString("Failed to create output directory: %1").arg(output_dir));
        return;
    }

    // 旧抽帧线程还在跑(已取消但仍卡在 FFmpeg 里退不出来): 不许并行再起一个 ——
    // 两次任务写的是同一个输出目录, 文件名又只由帧序号决定, 会互相覆盖出半新半旧的产物。
    // 这里把请求排进 pending, 等旧线程真正结束的 finished 回调续跑, 用户不必再点一次。
    if (IsFrameExportWorkerAlive()) {
        pending_frame_export_ =
            PendingFrameExport{current_url_(), output_dir, format, jpg_quality, frame_interval};
        LOG_WARN("上一次抽帧导出尚未结束, 本次请求已排队等待其退出");
        RequestStopFrameExport();
        return;
    }

    StartVideoFrameExportNow(output_dir, format, jpg_quality, frame_interval);
}

void ExportController::StartVideoFrameExportNow(const QString& output_dir, const QString& format,
                                                int jpg_quality, int frame_interval) {
    // 登记到统一任务调度 (任务 ID / 取消标志 / 终态)。
    //
    // 用 BeginHandle 而不是 Begin + Token 两步：句柄要一路传到 QtWorkerOwner，
    // 由它在 worker 线程退出的那一刻 EndHandle 还回终态 —— 于是"这条 QThread
    // 结束了"这件事也走同一个 TaskManager，不再有两种线程各有各的收尾方式。
    const task::TaskHandle export_handle =
        tasks_.BeginHandle(kSlotFrameExport, 0, task::TaskKind::BlockingIo);
    const task::TaskId task_id = export_handle.id;
    if (task_id == 0) {
        emit VideoFrameExportError("已有后台任务在运行, 请稍后再试");
        return;
    }

    const QString url = current_url_();
    const QString normalized_format = format.toLower();
    const int normalized_interval = std::max(1, frame_interval);

    // 取消令牌必须在线程启动**之前**注入 worker: 用户若在 worker 起来之前点了取消,
    // worker 一进门就能看到, 不会出现"取消被清空、导出照常跑完"的竞态。
    auto* exporter = new VideoFrameExporter();
    // 注入的就是句柄里那颗令牌 —— 全项目"取消"在这一条链路上只剩这一处来源:
    // 用户点取消 -> RequestStopFrameExport 置位 slot 令牌 -> 这里的 exporter 读到
    // -> Export() 收尾，同时 QtWorkerOwner 在线程结束时按同一颗令牌判 Canceled。
    exporter->SetCancelToken(export_handle.cancel);

    // 每次发起自增代际号: 下面所有转发信号都只接受当前代际,
    // 旧任务(被取消但线程还在排队终态)的信号会被丢弃, 不会串到本次新任务。
    const quint64 gen = ++frame_export_gen_;

    // 信号必须**先连好再起线程**: worker 一进门就可能因为参数不对而立刻报错,
    // 晚一步连就会漏掉这次终态(TaskManager 永远等不到 End)。
    // 转发前先做代际校验, 丢弃非当前任务的(已排队但迟到的)信号。
    connect(exporter, &VideoFrameExporter::ExportStarted, this, [this, gen](int total) {
        if (frame_export_gen_ != gen) return;
        emit VideoFrameExportStarted(total);
    });
    connect(exporter, &VideoFrameExporter::ExportProgress, this, [this, gen](int n) {
        if (frame_export_gen_ != gen) return;
        emit VideoFrameExportProgress(n);
    });
    connect(exporter, &VideoFrameExporter::ExportFinished, this, [this, gen](const QString& dir) {
        if (frame_export_gen_ != gen) return;
        emit VideoFrameExportFinished(dir);
    });
    connect(exporter, &VideoFrameExporter::ExportCanceled, this,
            [this, gen](int n, const QString& dir) {
                if (frame_export_gen_ != gen) return;
                emit VideoFrameExportCanceled(n, dir);
            });
    connect(exporter, &VideoFrameExporter::ExportError, this, [this, gen](const QString& msg) {
        if (frame_export_gen_ != gen) return;
        emit VideoFrameExportError(msg);
    });

    // 终态上报: 保证 WaitForIdle 看到的是终态而不是"还在跑"
    connect(exporter, &VideoFrameExporter::ExportFinished, this, [this, task_id](const QString&) {
        tasks_.End(kSlotFrameExport, task_id, task::TaskState::Succeeded);
    });
    connect(exporter, &VideoFrameExporter::ExportCanceled, this,
            [this, task_id](int, const QString&) {
                tasks_.End(kSlotFrameExport, task_id, task::TaskState::Canceled);
            });
    connect(exporter, &VideoFrameExporter::ExportError, this, [this, task_id](const QString&) {
        tasks_.End(kSlotFrameExport, task_id, task::TaskState::Failed);
    });

    // 线程退出后清掉"当前任务"标记。线程的 deleteLater 由 QtWorkerOwner 负责,
    // 这里只清指针, 不碰对象。作为 on_finished 在 start() 之前连接, 消除竞态
    // (否则线程秒级完成时清理回调还没连上, frame_exporter_ 会保留失效对象)。
    auto clear_marks = [this, exporter, task_id](QThread* finished_thread) {
        if (frame_export_thread_ == finished_thread) frame_export_thread_ = nullptr;
        if (frame_exporter_ == exporter) frame_exporter_ = nullptr;

        // 终态兜底: 线程都已经退出了, slot 却还是 Running, 说明终态没能落地
        // (终态信号还在 UI 队列里没排到、或 exporter 在退出途中把信号丢了)。
        // 不能再等下一次任务接管或播放器析构来强制收尾 —— 届时界面早就没有"已取消"
        // 的反馈了。
        //
        // 这里是**唯一**的兜底写入者: QtWorkerOwner 不碰 TaskManager(它不持调度器,
        // 句柄与终态都在本函数这一侧), 所以"QThread 任务怎么还终态"这件事只有一条路。
        // End 自带两道保护, 所以这行是幂等的: id 不匹配(已被新任务取代)直接丢弃,
        // 已经写过终态(终态信号抢先落地写了 Succeeded/Failed)也直接丢弃。
        if (tasks_.State(kSlotFrameExport) == task::TaskState::Running)
            tasks_.End(kSlotFrameExport, task_id,
                       tasks_.Token(kSlotFrameExport).IsCanceled() ? task::TaskState::Canceled
                                                                   : task::TaskState::Succeeded);

        // 线程真正结束了: 从这一行起,"两个抽帧任务同时写同一个输出目录"已不可能,
        // 于是把排队中的请求接上（用户"取消后立刻重试"就靠这里自动续跑）。
        if (!pending_frame_export_.has_value() || shutdown_) return;
        PendingFrameExport req = *pending_frame_export_;
        pending_frame_export_.reset();
        if (req.url != current_url_()) {
            // 排队期间换了媒体: 续跑会拿新文件往旧目录里导, 语义已经不对, 直接作废。
            LOG_INFO("排队中的抽帧导出请求已作废: 期间已切换媒体");
            return;
        }
        LOG_INFO("上一次抽帧导出已退出, 启动排队中的请求");
        StartVideoFrameExportNow(req.output_dir, req.format, req.jpg_quality, req.frame_interval);
    };

    // 任务体抛异常时的失败闭环。以前只靠 exporter 自己的终态信号，而 Export() 一旦抛异常
    // 就发不出任何信号：线程照常退出(quit 由 QtWorkerOwner 兜住)，但 TaskManager 任务永远
    // 停在 Running，界面既看不到"导出结束"也看不到"导出出错"，卡在一个谁也不认的半死状态里。
    // QtWorkerOwner 在**线程**上下文调 on_error，所以这里只把终态排回 UI 线程再落地。
    // 与 exporter 的 ExportError 是互补的两条路：那条是"导出失败后好好报错"，
    // 这条是"导出直接崩了、连报错都发不出来"——异常只有在这里才有出口。
    const auto on_frame_export_error = [this, task_id, gen](QThread*, const std::string& msg) {
        QMetaObject::invokeMethod(
            this,
            [this, task_id, gen, msg]() {
                // 只在"还在跑"时写失败终态：exporter 若自己发过终态（那条排在前面投递），
                // 再覆盖一次会把 Succeeded/Canceled 改写成 Failed。
                if (tasks_.State(kSlotFrameExport) != task::TaskState::Running) return;
                tasks_.End(kSlotFrameExport, task_id, task::TaskState::Failed);
                // 代际校验：期间换了媒体或点了取消，这次失败不该串到界面上。
                if (frame_export_gen_ == gen)
                    emit VideoFrameExportError(QString::fromStdString(msg));
            },
            Qt::QueuedConnection);
    };

    auto* thread = workers_.StartWorker(
        exporter,
        [exporter, url, output_dir, normalized_format, jpg_quality, normalized_interval]() {
            exporter->Export(url, output_dir, normalized_format, jpg_quality, normalized_interval);
        },
        export_handle,
        {} /* request_stop 留空: 取消就是置上面那颗令牌，exporter 自己读它 */,
        clear_marks,
        on_frame_export_error);

    frame_export_thread_ = thread;
    frame_exporter_ = exporter;
}

void ExportController::CancelVideoFrameExport() {
    // 用户主动取消: 排队中的请求一并作废 —— 否则旧线程退出后会"自作主张"接着跑一遍,
    // 用户看到的就是"点了取消, 界面还是导完了"。
    pending_frame_export_.reset();
    RequestStopFrameExport();
}

void ExportController::RequestStopFrameExport() {
    // 统一置取消标志(供外部查询 / 后续 Begin 取代旧任务)
    tasks_.Cancel(kSlotFrameExport);

    VideoFrameExporter* exporter = frame_exporter_;
    QThread* thread = frame_export_thread_;
    if (!exporter && !thread) return;

    if (exporter) exporter->Cancel();
    // **不在这里等线程退出**: 取消/重新导出/关窗都发生在 UI 线程, 一旦 FFmpeg 卡在不可
    // 中断的调用里, 等 5 秒就是 5 秒界面冻结。线程会在自己的 Export() 里看到取消标志后
    // 走到终态; 真退不出来也只是"这次取消晚点生效", 句柄仍归 workers_ 持有。
    const bool stopped = thread ? workers_.StopWorker(thread, 0) : true;
    if (stopped) {
        // 线程已结束(或本来就没有): 直接清标记。clear_marks 是排队投递的, 此刻可能还没跑。
        if (frame_exporter_ == exporter) frame_exporter_ = nullptr;
        if (frame_export_thread_ == thread) frame_export_thread_ = nullptr;
        return;
    }
    // 还在跑: **保留** frame_export_thread_/frame_exporter_, StartVideoFrameExport 要靠
    // IsFrameExportWorkerAlive() 判断旧任务还活着, 从而把新请求排队而不是并行写同一目录。
    // 线程真正结束时 clear_marks 会清掉这两个成员。
    //
    // 这里**不能**用 exporter->disconnect(this)。那会把它到本对象的连接整批断掉,
    // 其中包括三条终态上报(ExportFinished/Canceled/Error -> TaskManager::End)。
    // 断了它们, 旧线程随后即便正常收尾也写不进终态, slot 会一直挂在 Running 上,
    // 直到下次 Begin 取代它、或播放器析构时被强制收尾。
    // 防串台不靠断开连接: 每条 UI 转发 lambda 都有代际校验(frame_export_gen_),
    // 新任务一起动代际号就变了, 旧任务的进度/完成消息自然被丢弃; 而终态必须保留,
    // 因为"任务到底怎么结束的"是 TaskManager 的账, 与界面上显示哪一代无关。
}

// --- 音视频导出 (后台线程运行 MediaExporter) ---

void ExportController::StartMediaExport(const exporter::ExportOptions& opt) {
    // 旧任务还没退出时排队而非等待。旧导出写的是带 PID+UUID 的独立临时文件, 并行起来
    // 不会撞文件, 但两个转码任务抢同一批编解码线程只会互相拖慢; 更关键的是原来那套
    // "StopWorker(30s)" 是**在 UI 线程上同步等**, FFmpeg 一旦卡住界面就冻结 30 秒。
    if (IsMediaExportWorkerAlive()) {
        pending_media_export_ = opt;
        LOG_WARN("上一次媒体导出尚未结束, 本次请求已排队等待其退出");
        RequestStopMediaExport();
        return;
    }

    StartMediaExportNow(opt);
}

void ExportController::StartMediaExportNow(const exporter::ExportOptions& opt) {
    // 与抽帧侧同理：句柄交给 QtWorkerOwner 在线程退出时归还终态，slot / 令牌 / id
    // 仍然只有这一份，写入者只有一个。
    const task::TaskHandle export_handle =
        tasks_.BeginHandle(kSlotMediaExport, 0, task::TaskKind::BlockingIo);
    const task::TaskId task_id = export_handle.id;
    if (task_id == 0) { emit MediaExportError("已有后台任务在运行, 请稍后再试"); return; }
    auto fail = [this, task_id](const QString& msg) {
        tasks_.End(kSlotMediaExport, task_id, task::TaskState::Failed);
        emit MediaExportError(msg);
    };
    if (opt.input_path.isEmpty()) { fail("未打开媒体文件"); return; }
    if (opt.output_path.isEmpty()) { fail("未指定输出路径"); return; }
    // 注意: 不再预先删除目标文件。MediaExporter 先写同目录临时文件, 成功后才原子替换目标,
    // 因此导出失败/取消时原文件完好保留, 不会出现"旧文件被删、新导出又失败"的数据丢失。

    // 取消令牌在线程启动**之前**注入: 启动前的"立即取消"必须能被 worker 看到。
    auto* exporter = new exporter::MediaExporter();
    exporter->SetCancelToken(export_handle.cancel);

    // 同样先连信号再起线程: worker 可能一进门就因为参数不对而立刻报错。
    // 每次发起自增代际号: 转发信号只接受当前代际,
    // 旧任务(被取消但线程还在排队终态)的信号会被丢弃, 不会串到本次新任务。
    const quint64 gen = ++media_export_gen_;

    // 先连信号再起线程: worker 可能一进门就因为参数不对而立刻报错。
    // 转发前做代际校验, 丢弃非当前任务的迟到信号。
    connect(exporter, &exporter::MediaExporter::ExportStarted, this, [this, gen](qint64 d) {
        if (media_export_gen_ != gen) return;
        emit MediaExportStarted(d);
    });
    connect(exporter, &exporter::MediaExporter::ExportProgress, this, [this, gen](int p) {
        if (media_export_gen_ != gen) return;
        emit MediaExportProgress(p);
    });
    connect(exporter, &exporter::MediaExporter::ExportFinished, this,
            [this, gen](const QString& p) {
                if (media_export_gen_ != gen) return;
                emit MediaExportFinished(p);
            });
    connect(exporter, &exporter::MediaExporter::ExportCanceled, this,
            [this, gen](const QString& p) {
                if (media_export_gen_ != gen) return;
                emit MediaExportCanceled(p);
            });
    connect(exporter, &exporter::MediaExporter::ExportError, this, [this, gen](const QString& m) {
        if (media_export_gen_ != gen) return;
        emit MediaExportError(m);
    });

    // 终态上报 (登记在 quit 之前, 保证 WaitForIdle 看到的是终态而不是"还在跑")
    connect(exporter, &exporter::MediaExporter::ExportFinished, this,
            [this, task_id](const QString&) {
                tasks_.End(kSlotMediaExport, task_id, task::TaskState::Succeeded);
            });
    connect(exporter, &exporter::MediaExporter::ExportCanceled, this,
            [this, task_id](const QString&) {
                tasks_.End(kSlotMediaExport, task_id, task::TaskState::Canceled);
            });
    connect(exporter, &exporter::MediaExporter::ExportError, this, [this, task_id](const QString&) {
        tasks_.End(kSlotMediaExport, task_id, task::TaskState::Failed);
    });

    // 线程退出后清标记 (不覆盖后来启动的新导出: 两个指针都比过再清)。
    // 作为 on_finished 在 start() 之前连接, 消除竞态。对象回收由 QtWorkerOwner 负责。
    auto clear_marks = [this, exporter, task_id](QThread* finished_thread) {
        if (media_export_thread_ == finished_thread) media_export_thread_ = nullptr;
        if (media_exporter_ == exporter) media_exporter_ = nullptr;

        // 终态兜底, 与抽帧侧同理: 线程已退出却还没终态, 说明终态信号没落地。
        // 只补 Canceled 且只在本次确实被取消过时才补; End 的 id 匹配 + 终态一次性
        // 保证它不会盖掉已经写好的 Succeeded/Failed。
        // 与抽帧侧一致: 这里是唯一的兜底写入者, QtWorkerOwner 不持 TaskManager,
        // 不在别处抢着 End。
        if (tasks_.State(kSlotMediaExport) == task::TaskState::Running)
            tasks_.End(kSlotMediaExport, task_id,
                       tasks_.Token(kSlotMediaExport).IsCanceled() ? task::TaskState::Canceled
                                                                   : task::TaskState::Succeeded);

        // 线程真正结束了: 排队中的导出请求在这里续跑（"取消后立刻重导出"的路径）。
        // 重新走 StartMediaExportNow 而不是 StartMediaExport: 此刻旧线程已确认结束，
        // 不必再排队一轮。
        if (!pending_media_export_.has_value() || shutdown_) return;
        const exporter::ExportOptions req = *pending_media_export_;
        pending_media_export_.reset();
        LOG_INFO("上一次媒体导出已退出, 启动排队中的请求");
        StartMediaExportNow(req);
    };

    // 任务体抛异常时的失败闭环(与抽帧侧同理): MediaExporter::Export() 一抛就发不出终态
    // 信号，TaskManager 任务会永远停在 Running，界面也收不到 MediaExportError。
    // 这条是异常唯一的出口，必须一直连到 TaskManager::End(Failed)。
    const auto on_media_export_error = [this, task_id, gen](QThread*, const std::string& msg) {
        QMetaObject::invokeMethod(
            this,
            [this, task_id, gen, msg]() {
                if (tasks_.State(kSlotMediaExport) != task::TaskState::Running) return;
                tasks_.End(kSlotMediaExport, task_id, task::TaskState::Failed);
                if (media_export_gen_ == gen) emit MediaExportError(QString::fromStdString(msg));
            },
            Qt::QueuedConnection);
    };

    auto* thread = workers_.StartWorker(exporter, [exporter, opt]() { exporter->Export(opt); },
                                        export_handle,
                                        {} /* request_stop 留空: 取消走上面那颗令牌 */,
                                        clear_marks,
                                        on_media_export_error);
    media_export_thread_ = thread;
    media_exporter_ = exporter;
}

void ExportController::CancelMediaExport() {
    // 用户主动取消: 排队中的请求一并作废(否则旧线程退出后界面还是会把导出跑完)。
    pending_media_export_.reset();
    RequestStopMediaExport();
}

void ExportController::CancelAllExports() {
    // 先推进代际号再请求停止: 旧任务随后投递到 UI 线程的终态/进度信号会被代际校验丢弃
    // (它们属于"上一个媒体"的上下文, 覆盖到新媒体的界面状态就是错的)。
    // 这一步也顺带让旧任务的 clear_marks 续跑逻辑失去意义 —— pending 紧接着就被清空。
    ++frame_export_gen_;
    ++media_export_gen_;
    CancelVideoFrameExport();
    CancelMediaExport();
}

void ExportController::RequestStopMediaExport() {
    // 统一置取消标志
    tasks_.Cancel(kSlotMediaExport);

    exporter::MediaExporter* exporter = media_exporter_;
    QThread* thread = media_export_thread_;
    if (!exporter && !thread) return;
    if (exporter) exporter->Cancel();

    // 同抽帧: 只请求停止, 不在 UI 线程等待。原实现等 30 秒, FFmpeg 卡住时界面直接假死。
    const bool stopped = thread ? workers_.StopWorker(thread, 0) : true;
    if (stopped) {
        if (media_exporter_ == exporter) media_exporter_ = nullptr;
        if (media_export_thread_ == thread) media_export_thread_ = nullptr;
        return;
    }
    // 仍在跑: 保留标记供 IsMediaExportWorkerAlive() 判定, 把新请求排队。
    //
    // 与抽帧侧同理, 这里不能用 exporter->disconnect(this): 它会连三条终态上报一起断掉,
    // 旧线程随后收尾时写不进终态, slot 永久停在 Running。串台由 media_export_gen_ 的
    // 代际校验拦住, 不需要靠断开连接。
}

}  // namespace player
}  // namespace videoeye
