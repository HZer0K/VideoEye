#pragma once

// 产物命名 / 原子发布 / 源文件一致性校验。
//
// 规则：
//   * 自动命名带 run id，绝不覆盖已有文件；
//   * 显式目标已存在时直接失败（不静默覆盖用户文件）；
//   * 先写 <final>.part，成功后 rename；失败一律清理 .part；
//   * 发布前重新核对源文件大小与 mtime —— 修复过程中源被改写时不得发布产物。

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

struct RepairOutputPaths {
    std::string directory;
    std::string extension;
    std::string final_path;
    std::string part_path;
    std::string report_json_path;
    std::string report_text_path;
    std::string report_html_path;
    bool valid = false;
    std::string error;
};

class RepairOutputWriter {
public:
    // run_id 必须已由调用方生成（同一 run 内所有产物共享它）。
    static RepairOutputPaths Resolve(const model::RepairRequest& request, const std::string& run_id);

    static bool SourceUnchanged(const model::RepairInputSnapshot& snapshot, std::string* error);

    static bool Publish(const std::string& part_path, const std::string& final_path, std::string* error);

    // 尽最大努力删除临时文件；不存在的错误码忽略。
    static void Discard(const std::string& path);
};

} // namespace repair
} // namespace videoeye
