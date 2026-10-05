#include "core/player/ContainerInspectionController.h"

#include <QMetaObject>
#include <QPointer>
#include <QString>

#include <exception>
#include <utility>

#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace player {

ContainerInspectionController::ContainerInspectionController(task::TaskManager& tasks,
                                                             QObject* parent)
    : QObject(parent), tasks_(tasks) {}

void ContainerInspectionController::Cancel() {
    tasks_.Cancel(kSlotContainerStructure);
}

void ContainerInspectionController::Start(const QString& url) {
    // 以前这里是 QString url_copy, 直接整个喂给 ContainerStructureAnalyzer::Analyze。
    // 那个签名是域/分析侧的 std::string, 于是"QString 跨到无 Qt 那一层"这条依赖
    // 一直藏在调用方身上。边界转换在**这一行**明写出来: Qt 侧的值进了任务体就
    // 立刻落成本层的 std::string, 任务体再也不持有任何 Qt 类型。
    const std::string url_copy = url.toStdString();
    QPointer<ContainerInspectionController> self = this;

    // 交给统一的任务调度: 同 slot 上只允许一个任务, 换文件时旧任务被取消且结果作废,
    // 线程由 TaskManager 持有并在下次启动 / 析构时回收, 不再每次分析都新建并堆积线程。
    //
    // 用 RunBlockingIo: 这里要走 FFmpeg 的 avformat_open_input / find_stream_info,
    // 网络源或异常设备上即使装了 AVIOInterruptCB 也可能不响应, 关闭时不能 join 到底 ——
    // 代价是必须遵守 TaskKind::BlockingIo 的生命周期约定(见 infra/concurrency/TaskManager.h):
    // 本任务体只按值捕获 (QPointer self + std::string url_copy), 不持有裸引用,
    // 也不持有 Qt 类型, 符合约定。
    //
    // 用 WithResult 版本: 终态由**任务体自己**声明, TaskManager 只负责一次性写入。
    // 以前终态写在 UI 回调里, 于是"body 正常返回 -> Run 记 Succeeded" 与 "排在 UI 队列
    // 里的失败消息后到 -> 回调再记 Failed" 会互相改写, 终态取决于谁后跑完; 旧任务的
    // 失败消息甚至可能在新任务开始之后才落地。
    tasks_.RunBlockingIoWithResult(
        kSlotContainerStructure,
        [self, url_copy](task::TaskId id, task::CancelToken token) -> task::TaskState {
        // self 失效(播放器在分析期间被销毁)时必须立刻退出: 下面任何一次 self->...
        // 都是悬空访问 —— 包括"取消 + 销毁"这种两件事先后到达的时序。
        if (!self) return task::TaskState::Canceled;

        // 只发失败信号, 不写终态(终态是本任务体的返回值)。
        //
        // 落地时还要复查一次 current id: 排到 UI 队列之后可能已经换了文件 / 重新分析过,
        // 那时这条失败属于上一个任务, 不该弹到当前界面上。
        // 这里用 CurrentId 而不是 IsCurrent —— 消息落地时 slot 必然已经是终态,
        // IsCurrent 会一律判 false, 把正常的失败也一起丢掉。
        const auto report_failure = [self, id](const QString& msg) {
            if (!self) return;
            // 本任务体在 std::thread 上，直接 emit 会走直连把 UI 槽拖进后台线程，
            // 所以信号排回 UI 线程落地。
            QMetaObject::invokeMethod(
                self,
                [self, id, msg]() {
                    if (!self) return;
                    if (self->tasks_.CurrentId(kSlotContainerStructure) != id) {
                        LOG_INFO("容器结构分析的失败消息已过期, 丢弃");
                        return;
                    }
                    emit self->ContainerStructureFailed(msg);
                },
                Qt::QueuedConnection);
        };

        model::ContainerStructureResult cs_result;
        bool ok = false;
        QString failure_msg;
        try {
            videoeye::ContainerStructureAnalyzer analyzer;
            ok = analyzer.Analyze(url_copy, cs_result, token.flag());
        } catch (const std::exception& e) {
            LOG_ERROR("后台容器结构分析异常: " + std::string(e.what()));
            failure_msg = QString::fromStdString(e.what());
        } catch (...) {
            LOG_ERROR("后台容器结构分析发生未知异常");
            failure_msg = QStringLiteral("未知异常");
        }

        // 取消是用户动作, 不是分析出错 —— 优先级高于"没解析出来"。
        if (token.IsCanceled()) return task::TaskState::Canceled;
        // 播放器已被销毁: 结果无处可送, 也不能再碰 self。
        if (!self) return task::TaskState::Canceled;

        // 失败消息只在这里发一次。以前 catch 里发一次、!ok 分支再发一次,
        // 异常路径会连着弹出两条 ContainerStructureFailed。
        if (!ok) {
            if (failure_msg.isEmpty())
                failure_msg = QStringLiteral("容器结构分析未能产出结果");
            report_failure(failure_msg);
            return task::TaskState::Failed;
        }
        // 过期结果丢弃: 期间换了文件或关了播放器, 这次扫描的结果不能再覆盖新结果。
        // (以前靠 generation 比对, 现在统一由 TaskManager 的 IsCurrent 判定)
        if (!self->tasks_.IsCurrent(kSlotContainerStructure, id)) {
            LOG_INFO("容器结构分析结果已过期, 丢弃");
            return task::TaskState::Canceled;
        }
        // 落地时**再**校一次 current id: 上面的 IsCurrent 是投递前查的, 而从查完到这条
        // lambda 在 UI 线程上真正执行之间, 旧任务完全可能已经收尾、新任务也已经起步
        // (换文件 / 重新分析)。那时这条成功结果属于上一个任务, 弹出去就会把新任务的
        // 界面状态覆盖掉。用 CurrentId 而不是 IsCurrent —— 消息落地时 slot 必然已是
        // 终态, IsCurrent 会一律判 false, 连正常的成功结果也一起丢了。
        QMetaObject::invokeMethod(
            self,
            [self, id, result = std::move(cs_result)]() mutable {
                if (!self) return;
                if (self->tasks_.CurrentId(kSlotContainerStructure) != id) {
                    LOG_INFO("容器结构分析的成功结果已过期, 丢弃");
                    return;
                }
                emit self->ContainerStructureReady(result);
            },
            Qt::QueuedConnection);
        LOG_INFO("容器结构分析完成");
        return task::TaskState::Succeeded;
    });
    LOG_INFO("容器结构分析已派发到后台线程");
}

}  // namespace player
}  // namespace videoeye
