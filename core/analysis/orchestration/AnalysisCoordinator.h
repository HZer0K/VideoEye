#pragma once

// 迁移期兼容层。
//
// 原先的 AnalysisCoordinator 同时承担两件事：跑分析（现在归 AnalysisEngine）与
// 用 Qt 信号调度（现在归 core/qt/QtAnalysisController）。这里保留一个类型别名，
// 让还在写 AnalysisCoordinator 的调用方继续编译；新代码请直接用下面两者之一：
//   * 需要 Qt 信号/后台线程 -> core/qt/QtAnalysisController.h
//   * 只想同步跑一次分析   -> core/analysis/orchestration/AnalysisEngine.h

#include "core/qt/QtAnalysisController.h"

namespace videoeye {
namespace analyzer {

using AnalysisCoordinator = ::videoeye::qt::QtAnalysisController;

} // namespace analyzer
} // namespace videoeye
