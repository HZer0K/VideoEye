#pragma once

// ============================================================================
// domain 结果类型的 Qt 元类型声明
// ============================================================================
//
// 这些 Q_DECLARE_METATYPE 以前散落在 core/domain/model/*.h 里。domain 层改建 std::
// 容器/字符串之后它们没理由留在那 —— 元类型是"能不能进 QVariant / 能不能跨线程排队传送"
// 的问题，属于 Qt 侧的接线，不是领域模型自己的性质。留着它们就意味着 VideoEyeDomain
// 永远要 <QMetaType>，永远要 PUBLIC 链 Qt6::Core。
//
// 谁需要 include 本文件：
//   * 声明了带这些类型的 signal / slot 的 Q_OBJECT 类 —— moc 生成的 moc_*.cpp 只包含
//     这个类的头文件，所以在 .cpp 里 include 是没用的，必须写在头文件里；
//   * 写 connect() 连接这些类型参与的信号的地方。
//
// 为什么不必担心漏掉：漏了的话 Qt 会在运行时报
//   "QObject::connect: Cannot queue arguments of type 'X' (Make sure 'X' is registered
//    using qRegisterMetaType().)" —— 而不是静默出错。这是运行时错误，所以本文件
//   被 core/player 与 ui/ 里所有相关 Q_OBJECT 头文件显式 include。

#include <QMetaType>

#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/EbmlInfo.h"
#include "core/domain/model/MacroblockInfo.h"
#include "core/domain/model/Mp4BoxInfo.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"

Q_DECLARE_METATYPE(videoeye::model::AnalysisEvent)
Q_DECLARE_METATYPE(videoeye::model::AudioVisualizationFrame)
Q_DECLARE_METATYPE(videoeye::model::ContainerFormat)
Q_DECLARE_METATYPE(videoeye::model::ContainerElement)
Q_DECLARE_METATYPE(videoeye::model::ContainerStreamInfo)
Q_DECLARE_METATYPE(videoeye::model::ContainerStructureResult)
Q_DECLARE_METATYPE(videoeye::model::EbmlAnalysisResult)
Q_DECLARE_METATYPE(videoeye::model::MacroblockFrameAnalysis)
Q_DECLARE_METATYPE(videoeye::model::Mp4BoxNode)
Q_DECLARE_METATYPE(videoeye::model::Mp4BoxAnalysisResult)
Q_DECLARE_METATYPE(videoeye::model::PacketInfo)
Q_DECLARE_METATYPE(videoeye::model::SyncSample)
Q_DECLARE_METATYPE(videoeye::model::TimelineEvent)
Q_DECLARE_METATYPE(videoeye::model::TrackBoxTables)
