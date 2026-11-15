#pragma once

// 产物命名 / 原子发布 / 源文件一致性校验。
//
// 规则：
//   * 未指定产物目录时在源文件旁新建 "<源文件名>_repaired/" 专用根目录，
//     并在其下按 run 再建 "<时间戳-run id>/" 独立子文件夹（每次修复各一个）；
//     最终产物、临时产物、拼接文件、三份报告全部落在该次 run 的子文件夹里；
//   * 显式指定产物目录时直接落在那里，不额外嵌套任何子层；
//   * 自动命名带 run id，绝不覆盖已有文件；
//   * 显式目标已存在时直接失败（不静默覆盖用户文件）；
//   * 先写 <final>.part，成功后 rename；失败一律清理 .part；
//   * ⚠️ 最终产物 / 临时产物 / 拼接文件 / 三份报告**全部**纳入路径别名校验：
//     只比 final_path 时，源文件 recording.mp4.part 与显式目标 recording.mp4
//     会让 .part 临时产物正好指向源文件，avio_open 写模式会截断它；
//   * 写任何字节之前用独占创建把临时路径预留下来（已有临时文件一律拒绝）；
//   * 终态若本次 run 一个文件都没留下，只回收「本次新建且仍为空」的目录，
//     不删用户已有的目录、不删非空目录、不碰用户显式指定的目录；
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

    // ---- 终态空目录回收（仅 output_directory 为空时填写）----
    // Resolve 在诊断之前就会把默认落点的目录建出来；预检失败 / NeedsReference /
    // 取消等终态若一个文件都没留下，就要把空文件夹收回去。只记录本次真正新建的层：
    // 用户已有的目录、用户显式指定的目录都不进回收范围，回收时还会再确认目录为空。
    bool created_run_directory = false;
    std::string created_root_directory; // 空 = 根目录已存在，不属于本次
};

class RepairOutputWriter {
public:
    // 默认产物根目录（UTF-8）：源文件旁的 "<源文件名>_repaired"。
    // 它是各次 run 的收纳根目录，**不是**产物落点；界面提示用它，避免两处各拼一份字符串。
    static std::string DefaultDirectory(const std::string& source_path);

    // 某次 run 的实际产物目录（UTF-8）：<DefaultDirectory>/<时间戳-run id>。
    // 由 Resolve 与界面提示共用；run_id 会先做清洗，非安全字符一律替换，
    // 所以 ".." / 路径分隔符既不可能把产物挪出根目录，也不可能借文件名逃出去。
    static std::string RunDirectory(const std::string& source_path, const std::string& run_id);

    // run_id 必须已由调用方生成（同一 run 内所有产物共享它）。
    // 纯路径解析 + 冲突校验，不创建文件；同一 (source_path, run_id) 解析结果是确定的，
    // 重复调用复用同一个 run 目录，文件名冲突仍由后缀重试负责。
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

    // 终态收尾：只回收 Resolve 本次新建、且此刻仍为空的默认落点目录（run 子目录，
    // 以及它空了的根目录）。任何产物 / 报告 / 用户文件都会让 is_empty 判否，一个都不碰。
    // 报告在引擎返回之后由界面异步落盘，WriteAll 会自己保证父目录存在。
    static void DiscardEmptyRunDirectories(const RepairOutputPaths& paths);
};

} // namespace repair
} // namespace videoeye
