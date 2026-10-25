#pragma once

// 容器结构分析信号的对外契约：成功一条 + 失败一条。
//
// 为什么从 MediaPlayer 门面上摘下来（与 ExportBus 同理）:
//   * 这两条信号是"后台分析"的产物，不属于播放，却要让门面替它们背声明 + 背转发连接；
//   * UI 里关心它的只有 MainWindow 一处（转给 AnalysisPanel）。连总线之后，
//     门面不再是"打开 + 播放 + 分析 + 导出"四份契约叠在一起的那个上帝对象。
//
// 由谁持有: ContainerInspectionController（只有它知道结果过没过期），直接在本对象上
// 发射；MediaPlayer 经 container_bus() 把引用转给 UI。总线与控制器同线程（UI 线程）。
//
// ⚠️ 信号名与参数列表一个字节都不许改 —— 它们是 UI 的既有契约。
// ⚠️ 两条信号互斥：Ready 出去就一定不会有 Failed，反之亦然。

#include <QObject>
#include <QString>

#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {
namespace player {

class ContainerBus : public QObject {
    Q_OBJECT

public:
    explicit ContainerBus(QObject* parent = nullptr);

signals:
    void ContainerStructureReady(const videoeye::model::ContainerStructureResult& result);
    // 结构分析**没出结果**时发出（解析失败 / 任务体抛异常）。与 Ready 互斥。
    void ContainerStructureFailed(const QString& message);
};

} // namespace player
} // namespace videoeye
