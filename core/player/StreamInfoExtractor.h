#pragma once

#include <QString>
#include "core/model/FrameData.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace player {

// 从 AVFormatContext 提取流元数据信息
class StreamInfoExtractor {
public:
    // 提取结果（含时长）
    struct ExtractResult {
        model::StreamInfo info;
        int duration_ms = 0;
    };

    ExtractResult Extract(AVFormatContext* format_ctx,
                          int video_stream_index,
                          int audio_stream_index,
                          const QString& url) const;

    // 辅助：根据 codec 参数估算原始 PCM 文件时长
    static int64_t EstimateRawPcmDurationMs(const QString& url,
                                            const AVCodecParameters* codecpar);

    // 辅助：从 AVFormatContext 获取时长（毫秒）
    static int64_t DurationMsFromFormat(const AVFormatContext* format_ctx);

    // 辅助：将 int64_t 毫秒安全转为 int
    static int ClampMsToInt(int64_t ms);
};

} // namespace player
} // namespace videoeye
