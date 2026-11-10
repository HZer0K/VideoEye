#pragma once

// 产物命名 / 原子发布 / 源文件一致性校验。
//
// 规则：
//   * 自动命名带 run id，绝不覆盖已有文件；
//   * 显式目标已存在时直接失败（不静默覆盖用户文件）；
//   * 先写 <final>.part，成功后 rename；失败一律清理 .part；
//   * ⚠️ 最终产物 / 临时产物 / 拼接文件 / 三份报告**全部**纳入路径别名校验：
//     只比 final_path 时，源文件 recording.mp4.part 与显式目标 recording.mp4
//     会让 .part 临时产物正好指向源文件，avio_open 写模式会截断它；
//   * 写任何字节之前用独占创建把临时路径预留下来（已有临时文件一律拒绝）；
//   * 发布前用 size + mtime + 内容摘要复核源文件与参考文件。

#include <string>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairBudget.h"

namespace videoeye {
namespace repair {

struct RepairOutputPaths {
    std::string directory;
    std::string extension;
    std::string final_path;
    std::string part_path;
    // 参考初始化段 + 源分片的临时组装文件（RepairExecutor 使用）。
    std::string assembled_path;
    std::string report_json_path;
    std::string report_text_path;
    std::string report_html_path;
    bool valid = false;
    std::string error;
};

class RepairOutputWriter {
public:
    // run_id 必须已由调用方生成（同一 run 内所有产物共享它）。
    // 纯路径解析 + 冲突校验，不创建文件。
    static RepairOutputPaths Resolve(const model::RepairRequest& request, const std::string& run_id);

    // 独占创建 part 文件，预留本次 run 的写入权；已存在时返回 false。
    static bool Reserve(const RepairOutputPaths& paths, std::string* error);

    // 输入快照：size / mtime / 内容摘要。signature 由调用方按需补充。
    // budget：内容摘要是分块读，命中取消/超时时 content_hash 为空（method=unavailable），
    // 下游必须把它当成「没核对完」而不是「内容没变」。
    static model::RepairInputSnapshot MakeSnapshot(const std::string& path, const RepairRunBudget* budget = nullptr);

    // 与快照比对。method 回填本次实际使用的核对方法及其局限（可为空）。
    static bool SourceUnchanged(const model::RepairInputSnapshot& snapshot, std::string* error,
                                std::string* method = nullptr, const RepairRunBudget* budget = nullptr);

    static bool Publish(const std::string& part_path, const std::string& final_path, std::string* error);

    // 取消 / 超预算时保留已写前缀的目标路径：在 final 的扩展名前插入 ".partial"。
    // 例如 a.repaired.x.mp4 -> a.repaired.x.partial.mp4，绝不覆盖已有文件。
    static std::string PartialPath(const std::string& final_path);

    // 尽最大努力删除临时文件；不存在的错误码忽略。
    static void Discard(const std::string& path);
};

} // namespace repair
} // namespace videoeye
