#pragma once

// QC 模板 -> 分析参数 的映射。
//
// 单独成一个文件是因为"模板该配什么"和"模板怎么存/怎么序列化"是两件事：
// profile 侧的 KV 结构尽可以保持稳定，而 OptionsForDepth 会随分析器新增维度而变。
// 拆开后 QcProfile.h 不再 include AnalysisOptions.h —— 只想读写模板的模块
// （序列化的单元测试、模板编辑器）不必把整套分析参数拖进编译图。

#include "core/analysis/AnalysisOptions.h"
#include "core/qc/QcProfile.h"

namespace videoeye {
namespace qc {

// 分析强度 -> AnalysisOptions。基础档位与"具体要分析哪些维度"无关，
// 后者由调用方决定（UI 有自己的开关面板）。
videoeye::AnalysisOptions OptionsForDepth(QcAnalysisDepth depth);

}  // namespace qc
}  // namespace videoeye
