#pragma once

#include <QMetaType>
#include <cstdint>

namespace videoeye {
namespace model {

struct PacketInfo {
    int index = 0;
    int stream_index = -1;
    int stream_type = -1;
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 0;
    int size = 0;
    int flags = 0;
    int64_t pos = -1;
    double timestamp_seconds = 0.0;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::PacketInfo)
