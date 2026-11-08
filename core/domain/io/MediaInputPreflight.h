#pragma once

// 本地输入预检 —— 「文件为什么打不开」的唯一事实源。
//
// 背景：QcRunner::IsAnalyzableFile 早就把「路径为空 / 不存在 / 目录 / 文件为空」
// 分开了（报告里这四件事的处置方式不同），但 ContainerStructureAnalyzer 是另一条
// 独立入口（结构页 / 离线语料校验），它的 FFmpeg 回退把**所有**打开失败都压成一句
// 「FFmpeg 无法打开文件」。于是零字节文件、不存在的路径、纯文本 XML 在结构页上
// 长得一模一样 —— 用户无法判断是素材坏了、路径写错了、还是这个格式不支持。
//
// 分层：本头在 core/domain/io（叶子），core/qc 与 core/analysis 都能单向依赖它；
// 反过来 domain 不许 include 业务层，所以预检必须落在这里，不能放到 QcRunner。
//
// ⚠️ 文案是 UI 契约：QcRunner 既有测试断言了这几句话，改文案要同步改测试。
// ⚠️ 走 ToNativePath：Windows 上 std::filesystem 直接收 std::string 会按 ANSI 代码页
//    解释，中文目录在部分机器上会被误判成「文件不存在」。

#include <filesystem>
#include <string>
#include <system_error>

#include "core/domain/io/Utf8Path.h"

namespace videoeye {

enum class InputPreflightKind {
    Ok,
    EmptyPath,
    NotFound,
    IsDirectory,
    EmptyFile,
};

struct InputPreflightResult {
    InputPreflightKind kind = InputPreflightKind::Ok;
    std::string reason; // Ok 时为空；失败时是面向用户的完整文案（含路径）

    bool ok() const {
        return kind == InputPreflightKind::Ok;
    }
};

// 稳定 ASCII 键：离线门禁按它分组比对，不要拿中文文案当机器口径。
inline const char* InputPreflightKindName(InputPreflightKind kind) {
    switch (kind) {
    case InputPreflightKind::Ok:
        return "ok";
    case InputPreflightKind::EmptyPath:
        return "empty_path";
    case InputPreflightKind::NotFound:
        return "not_found";
    case InputPreflightKind::IsDirectory:
        return "is_directory";
    case InputPreflightKind::EmptyFile:
        return "empty_file";
    }
    return "unknown";
}

inline InputPreflightResult PreflightLocalFile(const std::string& utf8_path) {
    InputPreflightResult out;
    if (utf8_path.empty()) {
        out.kind = InputPreflightKind::EmptyPath;
        out.reason = "路径为空";
        return out;
    }

    const auto native = ToNativePath(utf8_path);
    std::error_code ec;
    const auto status = std::filesystem::status(native, ec);
    if (ec || !std::filesystem::exists(status)) {
        out.kind = InputPreflightKind::NotFound;
        out.reason = "文件不存在: " + utf8_path;
        return out;
    }
    if (std::filesystem::is_directory(status)) {
        out.kind = InputPreflightKind::IsDirectory;
        out.reason = "路径是目录，单文件分析请给出具体文件: " + utf8_path;
        return out;
    }

    const auto size = std::filesystem::file_size(native, ec);
    // 与 IsAnalyzableFile 既有语义保持一致：拿不到大小时不拦（保守放行），
    // 真正的打开失败交给 FFmpeg 那一层按 open_failed 归类。
    if (!ec && size == 0) {
        out.kind = InputPreflightKind::EmptyFile;
        out.reason = "文件为空: " + utf8_path;
        return out;
    }
    return out;
}

} // namespace videoeye
