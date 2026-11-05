#pragma once

// 修复报告导出（JSON / TXT / HTML）。
//
// 报告层只消费 domain 值类型，不认识 RepairEngine / FFmpeg / Qt —— 路径由调用方给，
// 这样"修复正本"与"报告表现"互不依赖。JSON 里 64 位整数一律写成字符串，
// 避免下游 JS / JSON 解析器按 double 截断字节偏移与大计数。

#include <string>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace reporting {

struct RepairReportWriteResult {
    bool json_ok = false;
    bool text_ok = false;
    bool html_ok = false;
    std::string error;

    bool AllOk() const {
        return json_ok && text_ok && html_ok;
    }
};

class RepairReportExporter {
public:
    static RepairReportWriteResult WriteAll(const model::RepairResult& result, const std::string& json_path,
                                            const std::string& text_path, const std::string& html_path);
};

} // namespace reporting
} // namespace videoeye
