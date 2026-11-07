#include "core/repair/RepairOutputWriter.h"

#include <chrono>
#include <filesystem>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/repair/RepairUtils.h"

namespace videoeye {
namespace repair {
namespace {

// 源容器之外的格式统一输出为 mp4：未指定容器时沿用源扩展名（mov/m4v），其它统一 mp4。
std::string OutputExtension(const model::RepairRequest& request) {
    switch (request.output_container) {
    case model::RepairOutputContainer::Mkv:
        return "mkv";
    case model::RepairOutputContainer::Ts:
        return "ts";
    case model::RepairOutputContainer::Mp4:
        return "mp4";
    case model::RepairOutputContainer::Auto:
        break;
    }
    const std::string ext = LowerExtension(request.source_path);
    if (ext == "mov" || ext == "m4v")
        return ext;
    return "mp4";
}

std::filesystem::path ParentDirectory(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    return parent.empty() ? std::filesystem::path(".") : parent;
}

bool Exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(ToNativePath(path), ec);
}

// 报告与最终产物共享 stem，但扩展名不同；用 path::stem 而不是在整条路径串上
// find_last_of('.')，否则目录名里带点会把报告写到错误的位置。
void FillReportPaths(const std::string& final_path, RepairOutputPaths* paths) {
    const std::filesystem::path fp(final_path);
    const std::filesystem::path dir = fp.parent_path();
    const std::string stem = fp.stem().u8string();
    paths->report_json_path = (dir / (stem + ".repair.json")).u8string();
    paths->report_text_path = (dir / (stem + ".repair.txt")).u8string();
    paths->report_html_path = (dir / (stem + ".repair.html")).u8string();
}

std::vector<std::string> AllArtifacts(const RepairOutputPaths& paths) {
    return {paths.final_path,       paths.part_path,        paths.assembled_path,
            paths.report_json_path, paths.report_text_path, paths.report_html_path};
}

std::vector<std::string> ProtectedInputs(const model::RepairRequest& request) {
    std::vector<std::string> inputs = {request.source_path};
    if (!request.reference_path.empty())
        inputs.push_back(request.reference_path);
    return inputs;
}

// 所有产物都不与受保护输入冲突，产物之间也不互相冲突，且此刻都不存在。
std::string ValidateArtifacts(const RepairOutputPaths& paths, const model::RepairRequest& request) {
    const std::vector<std::string> artifacts = AllArtifacts(paths);
    const std::vector<std::string> inputs = ProtectedInputs(request);

    for (const auto& artifact : artifacts) {
        if (artifact.empty())
            continue;
        for (const auto& input : inputs) {
            if (PathsAlias(artifact, input)) {
                return "产物路径与受保护的输入文件冲突，拒绝写入（避免截断/删除源文件）: " + artifact + " <-> " + input;
            }
        }
        if (Exists(artifact) && artifact != paths.final_path) {
            return "派生产物已存在，拒绝覆盖或清理用户文件: " + artifact;
        }
    }
    for (std::size_t i = 0; i < artifacts.size(); ++i) {
        for (std::size_t j = i + 1; j < artifacts.size(); ++j) {
            if (!artifacts[i].empty() && !artifacts[j].empty() && PathsAlias(artifacts[i], artifacts[j])) {
                return "产物路径之间互相冲突: " + artifacts[i] + " <-> " + artifacts[j];
            }
        }
    }
    return {};
}

} // namespace

RepairOutputPaths RepairOutputWriter::Resolve(const model::RepairRequest& request, const std::string& run_id) {
    RepairOutputPaths paths;
    if (request.source_path.empty()) {
        paths.error = "源文件路径为空";
        return paths;
    }

    std::filesystem::path directory = request.output_directory.empty()
                                          ? ParentDirectory(request.source_path)
                                          : std::filesystem::path(request.output_directory);
    std::error_code ec;
    if (!std::filesystem::exists(ToNativePath(directory.u8string()), ec)) {
        std::filesystem::create_directories(ToNativePath(directory.u8string()), ec);
        if (ec) {
            paths.error = "无法创建产物目录: " + directory.u8string() + " (" + ec.message() + ")";
            return paths;
        }
    }
    if (!std::filesystem::is_directory(ToNativePath(directory.u8string()), ec)) {
        paths.error = "产物目录不是可写目录: " + directory.u8string();
        return paths;
    }

    paths.directory = directory.u8string();
    paths.extension = OutputExtension(request);
    const std::string stem = FileStem(request.source_path);

    auto build = [&](const std::string& final_path) {
        RepairOutputPaths candidate = paths;
        candidate.final_path = final_path;
        candidate.part_path = final_path + ".part";
        candidate.assembled_path = candidate.part_path + ".src.mp4";
        FillReportPaths(final_path, &candidate);
        return candidate;
    };

    if (!request.output_path.empty()) {
        std::string final_path = request.output_path;
        if (!std::filesystem::path(final_path).is_absolute())
            final_path = (directory / final_path).u8string();
        if (Exists(final_path)) {
            paths.error = "目标文件已存在，拒绝覆盖: " + final_path;
            return paths;
        }
        RepairOutputPaths candidate = build(final_path);
        const std::string conflict = ValidateArtifacts(candidate, request);
        if (!conflict.empty()) {
            paths.error = conflict;
            return paths;
        }
        candidate.valid = true;
        return candidate;
    }

    // 自动命名：run_id 里已有随机后缀，但派生产物也可能撞上已有文件，
    // 所以整组一起重试，而不是只看 final_path。
    for (int suffix = 0; suffix < 100; ++suffix) {
        const std::string unique = run_id + (suffix == 0 ? std::string() : "-" + std::to_string(suffix));
        const std::string name = stem + ".repaired." + unique + "." + paths.extension;
        RepairOutputPaths candidate = build((directory / name).u8string());
        if (Exists(candidate.final_path))
            continue;
        // 自动命名下任何冲突都应该换个后缀重试，而不是把一次遗留的 .part
        // 变成永久无法修复的错误。
        if (!ValidateArtifacts(candidate, request).empty())
            continue;
        candidate.valid = true;
        return candidate;
    }
    paths.error = "无法为产物生成不冲突的文件名（已尝试 100 个后缀）";
    return paths;
}

bool RepairOutputWriter::Reserve(const RepairOutputPaths& paths, std::string* error) {
    if (!paths.valid || paths.part_path.empty()) {
        if (error)
            *error = "产物路径未通过校验，不能预留临时文件";
        return false;
    }
    if (!CreateExclusiveFile(paths.part_path)) {
        if (error)
            *error = "无法独占预留临时产物（已存在或不可创建）: " + paths.part_path;
        return false;
    }
    return true;
}

model::RepairInputSnapshot RepairOutputWriter::MakeSnapshot(const std::string& path) {
    model::RepairInputSnapshot snapshot;
    snapshot.path = path;
    std::error_code ec;
    const std::filesystem::path native = ToNativePath(path);
    if (!std::filesystem::exists(native, ec))
        return snapshot;
    snapshot.size_bytes = std::filesystem::file_size(native, ec);
    if (ec)
        return snapshot;
    const auto mtime = std::filesystem::last_write_time(native, ec);
    if (!ec) {
        // file_time_type 的 epoch 在 C++17 里没有可移植的 unix 偏移；这里只需要一个
        // 稳定的"是否变化"刻度，不需要真的对齐 Unix 时间。
        snapshot.mtime_epoch_seconds =
            static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count());
    }
    snapshot.content_hash = ComputeContentHash(path, &snapshot.hash_method, &snapshot.hashed_bytes);
    return snapshot;
}

bool RepairOutputWriter::SourceUnchanged(const model::RepairInputSnapshot& snapshot, std::string* error,
                                         std::string* method) {
    std::error_code ec;
    const std::filesystem::path native = ToNativePath(snapshot.path);
    if (!std::filesystem::exists(native, ec)) {
        if (error)
            *error = "发布前校验失败：输入文件已不存在: " + snapshot.path;
        if (method)
            *method = "unavailable";
        return false;
    }
    const uint64_t size = std::filesystem::file_size(native, ec);
    if (ec) {
        if (error)
            *error = "发布前校验失败：无法读取输入文件大小 (" + ec.message() + ")";
        if (method)
            *method = "unavailable";
        return false;
    }
    if (size != snapshot.size_bytes) {
        if (error)
            *error = "发布前校验失败：输入文件大小从 " + std::to_string(snapshot.size_bytes) + " 变为 " +
                     std::to_string(size) + ": " + snapshot.path;
        if (method)
            *method = "size";
        return false;
    }

    bool mtime_checked = false;
    if (snapshot.mtime_epoch_seconds != 0) {
        const auto mtime = std::filesystem::last_write_time(native, ec);
        if (!ec) {
            const int64_t now_seconds = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count());
            if (now_seconds != snapshot.mtime_epoch_seconds) {
                if (error)
                    *error = "发布前校验失败：输入文件修改时间已变化: " + snapshot.path;
                if (method)
                    *method = "size+mtime";
                return false;
            }
            mtime_checked = true;
        }
    }

    // 内容摘要是唯一能挡住"同大小、mtime 被复位"的原位改写的证据。
    bool hash_checked = false;
    std::string hash_method;
    if (!snapshot.content_hash.empty() && snapshot.hash_method != "unavailable") {
        uint64_t hashed_bytes = 0;
        const std::string now_hash = ComputeContentHash(snapshot.path, &hash_method, &hashed_bytes);
        if (now_hash.empty() || now_hash != snapshot.content_hash) {
            if (error)
                *error = "发布前校验失败：输入文件内容摘要已变化: " + snapshot.path;
            if (method)
                *method = "size+mtime+hash";
            return false;
        }
        hash_checked = true;
    }

    std::string description = "size";
    if (mtime_checked)
        description += "+mtime";
    if (hash_checked) {
        description += "+" + hash_method;
        if (hash_method == "fnv1a64_head_tail")
            description += "(仅头尾 4MiB，中间字节未覆盖)";
    } else {
        description += "(内容摘要不可用，无法排除同大小原位改写)";
    }
    if (method)
        *method = description;
    return true;
}

bool RepairOutputWriter::Publish(const std::string& part_path, const std::string& final_path, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(ToNativePath(part_path), ec)) {
        if (error)
            *error = "临时产物不存在: " + part_path;
        return false;
    }
    if (std::filesystem::exists(ToNativePath(final_path), ec)) {
        if (error)
            *error = "目标文件已存在，拒绝覆盖: " + final_path;
        return false;
    }
    std::filesystem::rename(ToNativePath(part_path), ToNativePath(final_path), ec);
    if (ec) {
        if (error)
            *error = "发布产物失败: " + ec.message();
        return false;
    }
    return true;
}

void RepairOutputWriter::Discard(const std::string& path) {
    if (path.empty())
        return;
    std::error_code ec;
    std::filesystem::remove(ToNativePath(path), ec);
}

std::string RepairOutputWriter::PartialPath(const std::string& final_path) {
    const std::filesystem::path fp(final_path);
    const std::filesystem::path dir = fp.parent_path();
    const std::string stem = fp.stem().u8string();
    const std::string ext = fp.extension().u8string();
    return (dir / (stem + ".partial" + ext)).u8string();
}

} // namespace repair
} // namespace videoeye
