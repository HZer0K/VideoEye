#pragma once

// 容器结构分析的后台编排：派发任务、作废上一次结果、把结果投递回 UI 线程。
//
// 为什么从 MediaPlayer 里拆出来: 这条链路带的知识和"播放"毫无关系，全是后台任务
// 的通用坑 ——
//   * 分析要跑 avformat_open_input / find_stream_info，网络源或异常设备上即使装了
//     中断回调也可能不响应，只能登记成 BlockingIo 任务（超时不 join，避免关窗挂死）；
//   * 终态必须由**任务体自己**声明（RunBlockingIoWithResult），写在 UI 回调里会出现
//     "body 正常返回记 Succeeded" 与 "排在队列里的失败消息后到再记 Failed" 互相改写；
//   * 换文件时旧任务的结果会迟到，投递前查一次 IsCurrent、落地时再查一次 CurrentId
//     （注意是 CurrentId 不是 IsCurrent —— 消息落地时 slot 必然已是终态，
//      IsCurrent 要求状态仍在 Running，会一律判 false，把正常结果也一起丢掉）；
//   * 播放器可能在分析期间被销毁，任务体必须持 QPointer 并且在每一次解引用前复查。
// 这些原先和播放状态机、12 个分析开关混在同一个类里。收进来之后，MediaPlayer 只
// 保留"要不要分析（开关）"和"转发两条信号"。
//
// 本类是 QObject: 结果要跨线程排回 UI 线程落地（QMetaObject::invokeMethod 需要一个
// 接收者上下文），且对外信号契约必须与拆分前一个字节不差 —— 所以这里声明同名信号，
// 由 MediaPlayer 原样转发。
//
// 与 TaskManager 的关系（沿用拆分前的约定，不要改）:
//   * slot / 令牌 / 任务 id 一律从**调用方**的 TaskManager 取，本类只持引用；
//   * 本类不拥有任务线程，任务体在 std::thread 上跑，宿主析构路径由 TaskManager
//     的 CancelAll / WaitForAll 收尾（MediaPlayer 的析构里已经先做了这一步）。

#include <QObject>

#include "core/domain/model/ContainerStructureInfo.h"
#include "infrastructure/concurrency/TaskManager.h"

namespace videoeye {
namespace player {

class ContainerInspectionController : public QObject {
    Q_OBJECT

public:
    // tasks: 调用方的任务调度器（slot / 令牌 / 终态都在那儿）。必须活过本对象。
    explicit ContainerInspectionController(task::TaskManager& tasks, QObject* parent = nullptr);

    ContainerInspectionController(const ContainerInspectionController&) = delete;
    ContainerInspectionController& operator=(const ContainerInspectionController&) = delete;

    // 派发一次容器结构分析（后台线程，结果经下面两条信号发出）。
    //
    // 同 slot 上只允许一个任务: 换新任务时旧任务被自动取消，旧结果作废。
    // 开关判定（IsContainerStructureEnabled）不在这里 —— 那是播放期的分析开关，
    // 归 AnalysisSession，由调用方决定该不该调本函数。
    void Start(const QString& url);

    // 换媒体时调用: 上一次的容器结构分析结果作废。
    // 任务体投递前会查 IsCurrent，被取消的任务不会把结果弹到新媒体的界面上。
    void Cancel();

signals:
    void ContainerStructureReady(const videoeye::model::ContainerStructureResult& result);
    // 结构分析**没出结果**时发出（解析失败 / 任务体抛异常）。
    // 与 Ready 互斥：Ready 出去时本信号一定不会来，反之亦然 —— 页面据此决定
    // 是画结构树还是显示一条错误。
    void ContainerStructureFailed(const QString& message);

private:
    // 后台任务 slot 名。同一 slot 上永远只有一个任务在跑
    // (见 infrastructure/concurrency/TaskManager.h)。
    static constexpr const char* kSlotContainerStructure = "container-structure";

    task::TaskManager& tasks_;
};

}  // namespace player
}  // namespace videoeye
