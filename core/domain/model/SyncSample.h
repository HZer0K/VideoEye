#pragma once

#include <QMetaType>

namespace videoeye {
namespace model {

struct SyncSample {
    int index = 0;
    double audio_timestamp_seconds = 0.0;
    double video_timestamp_seconds = 0.0;
    double diff_ms = 0.0;
    bool audio_anchor = false;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::SyncSample)
