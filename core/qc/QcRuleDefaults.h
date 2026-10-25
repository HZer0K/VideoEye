#pragma once

#include <vector>

#include "core/domain/model/QcRule.h"

namespace videoeye {
namespace qc {

// 默认规则集（顺序即 UI 展示顺序）。
// 2026-10-25 从 domain 下沉到 QC 层：domain 只保留值类型与查找工具，
// 具体的规则 id / 阈值 / 判据 / 建议文案属于 QC 策略。
std::vector<model::QcRule> DefaultQcRules();

} // namespace qc
} // namespace videoeye
