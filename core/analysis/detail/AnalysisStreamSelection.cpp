#include "core/analysis/detail/AnalysisStreamSelection.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace videoeye {

bool IsAnalysisVideoStream(const AVStream* stream) {
    return stream != nullptr && stream->codecpar != nullptr && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
           (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
}

int SelectAnalysisStream(AVFormatContext* format, int type) {
    if (format == nullptr)
        return -1;
    const auto media_type = static_cast<AVMediaType>(type);
    const auto eligible = [media_type](const AVStream* stream) {
        return stream != nullptr && stream->codecpar != nullptr && stream->codecpar->codec_type == media_type &&
               (media_type != AVMEDIA_TYPE_VIDEO || IsAnalysisVideoStream(stream));
    };
    const int best = av_find_best_stream(format, media_type, -1, -1, nullptr, 0);
    if (best >= 0 && best < static_cast<int>(format->nb_streams) && eligible(format->streams[best])) {
        return best;
    }
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (eligible(format->streams[i]))
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace videoeye
