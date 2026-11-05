#pragma once

// 一次修复请求。纯值对象：界面、CLI 或自动化都构造它，然后交给 RepairEngine。

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace model {

struct RepairLimits {
    // 单次修复的总时间预算（秒）。0 = 不设上限。
    int max_seconds = 0;
    // 超过预算时是否允许用户确认后继续。false 时引擎按 TimedOut 返回。
    bool user_confirmed_exceed = false;
    // 最多写入多少字节。0 = 不设上限。
    uint64_t max_output_bytes = 0;
};

struct RepairRequest {
    std::string request_id;

    std::string source_path;

    // 产物目录。为空时用源文件所在目录（若不可写则修复失败，不做静默降级）。
    std::string output_directory;

    // 显式产物路径。为空时自动命名：<stem>.repaired.<runid>.<ext>。
    std::string output_path;

    // 参考样片 / 初始化段：当源文件是缺少 ftyp+moov 初始化段的 fMP4 媒体分片
    // （styp/sidx/moof/mdat）时，从这里提取初始化段并在重封装前拼接。
    std::string reference_path;

    // 目标流（FFmpeg 流下标）。空 = 自动选择第一条视频轨；没有视频则选第一条音频轨。
    std::vector<int> target_streams;

    RepairMode mode = RepairMode::PreserveBitstream;
    RepairVerificationLevel verification_level = RepairVerificationLevel::V2_FullDecode;

    RepairLimits limits;

    bool write_report = true;
};

} // namespace model
} // namespace videoeye
