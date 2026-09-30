#pragma once

// 分析链路里最底层的公共值类型。
//
// 单独成一个文件是为了让"只要拼状体、不要结果/参数"的使用者（比如界面预览、
// 单纯做 demux 摘要的工具）不必拖着整个 AnalysisResult 的模型清单编译。

#include <cstdint>
#include <string>

namespace videoeye {
namespace analyzer {

// 单条流的静态摘要（demux 层，不解码）
struct StreamDigest {
    int index = -1;
    int media_type = -1;          // AVMediaType
    std::string codec_name;
    std::string profile_name;
    int width = 0;
    int height = 0;
    double avg_fps = 0.0;
    int sample_rate = 0;
    int channels = 0;
    int64_t bitrate_bps = 0;
    int64_t packet_count = 0;
    int64_t byte_count = 0;
    int64_t frame_count = 0;      // 视频按包估算
    int64_t key_frame_count = 0;
    double start_seconds = 0.0;
    double duration_seconds = 0.0;

    bool IsVideo() const { return media_type == 0; }  // AVMEDIA_TYPE_VIDEO
    bool IsAudio() const { return media_type == 1; }  // AVMEDIA_TYPE_AUDIO
};

} // namespace analyzer
} // namespace videoeye
