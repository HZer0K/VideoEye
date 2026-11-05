#include "core/repair/RepairOutputWriter.h"

#include <filesystem>

#include "core/repair/RepairUtils.h"

namespace videoeye {
namespace repair {
namespace {

// 源容器之外的格式统一输出为 mp4：M1 的执行器只挂了 MP4/MOV 多路复用器。
std::string OutputExtension(const std::string& source_path) {
    const std::string ext = LowerExtension(source_path);
    if (ext == "mov" || ext == "m4v")
        return ext;
    return "mp4";
}

std::filesystem::path ParentDirectory(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    return parent.empty() ? std::filesystem::path(".") : parent;
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
    if (!std::filesystem::exists(directory, ec)) {
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            paths.error = "无法创建产物目录: " + directory.string() + " (" + ec.message() + ")";
            return paths;
        }
    }
    if (!std::filesystem::is_directory(directory, ec)) {
        paths.error = "产物目录不是可写目录: " + directory.string();
        return paths;
    }

    paths.directory = directory.string();
    paths.extension = OutputExtension(request.source_path);
    const std::string stem = FileStem(request.source_path);

    std::string final_path;
    if (!request.output_path.empty()) {
        final_path = request.output_path;
        if (!std::filesystem::path(final_path).is_absolute())
            final_path = (directory / final_path).string();
        if (std::filesystem::exists(final_path, ec)) {
            paths.error = "目标文件已存在，拒绝覆盖: " + final_path;
            return paths;
        }
    } else {
        std::string name = stem + ".repaired." + run_id + "." + paths.extension;
        final_path = (directory / name).string();
        int suffix = 1;
        while (std::filesystem::exists(final_path, ec) && suffix < 100) {
            name = stem + ".repaired." + run_id + "-" + std::to_string(suffix) + "." + paths.extension;
            final_path = (directory / name).string();
            ++suffix;
        }
    }

    if (SamePath(request.source_path, final_path)) {
        paths.error = "目标路径与源文件相同，拒绝写入（避免破坏源文件）";
        return paths;
    }

    paths.final_path = final_path;
    paths.part_path = final_path + ".part";

    const std::string base = final_path.substr(0, final_path.find_last_of('.'));
    paths.report_json_path = base + ".repair.json";
    paths.report_text_path = base + ".repair.txt";
    paths.report_html_path = base + ".repair.html";

    paths.valid = true;
    return paths;
}

bool RepairOutputWriter::SourceUnchanged(const model::RepairInputSnapshot& snapshot, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(snapshot.path, ec)) {
        if (error)
            *error = "发布前校验失败：源文件已不存在";
        return false;
    }
    const uint64_t size = std::filesystem::file_size(snapshot.path, ec);
    if (ec) {
        if (error)
            *error = "发布前校验失败：无法读取源文件大小 (" + ec.message() + ")";
        return false;
    }
    if (size != snapshot.size_bytes) {
        if (error)
            *error =
                "发布前校验失败：源文件大小从 " + std::to_string(snapshot.size_bytes) + " 变为 " + std::to_string(size);
        return false;
    }
    return true;
}

bool RepairOutputWriter::Publish(const std::string& part_path, const std::string& final_path, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(part_path, ec)) {
        if (error)
            *error = "临时产物不存在: " + part_path;
        return false;
    }
    if (std::filesystem::exists(final_path, ec)) {
        if (error)
            *error = "目标文件已存在，拒绝覆盖: " + final_path;
        return false;
    }
    std::filesystem::rename(part_path, final_path, ec);
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
    std::filesystem::remove(path, ec);
}

} // namespace repair
} // namespace videoeye
