#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

namespace videoeye {

// 可播放的视频轨：封面图片（APIC / AV_DISPOSITION_ATTACHED_PIC）不算。
// 带内嵌封面的 MP3/M4A 在 FFmpeg 里就是一条"一帧的 MJPEG 视频流"，
// 把它当主视频会让码率/GOP/色彩/码流分析全部指向一张封面图。
inline bool IsAnalysisVideoStream(const AVStream* stream) {
    return stream != nullptr && stream->codecpar != nullptr &&
           stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
           (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
}

// 选主轨：优先 av_find_best_stream（它认 AV_DISPOSITION_DEFAULT 与容器默认轨，
// 单靠"第一条同类型流"会选中封面图或字幕带的缩略图轨），
// 结果不合法时再线性兜底。
// decoder_ret 传 nullptr：不解码也要能分析元数据（不可解码的编码同样要出流摘要）。
inline int SelectAnalysisStream(AVFormatContext* format, AVMediaType type) {
    if (format == nullptr) return -1;
    const auto eligible = [type](const AVStream* stream) {
        return stream != nullptr && stream->codecpar != nullptr &&
               stream->codecpar->codec_type == type &&
               (type != AVMEDIA_TYPE_VIDEO || IsAnalysisVideoStream(stream));
    };
    const int best = av_find_best_stream(format, type, -1, -1, nullptr, 0);
    if (best >= 0 && best < static_cast<int>(format->nb_streams) &&
        eligible(format->streams[best])) {
        return best;
    }
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (eligible(format->streams[i])) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace videoeye
