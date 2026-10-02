#pragma once

#include <cstdint>
#include <string>

namespace videoeye {
namespace model {

struct AnalysisEvent {
    int index = 0;
    std::string severity;
    std::string type;
    int stream_index = -1;
    int64_t pts = 0;
    double timestamp_seconds = 0.0;
    std::string summary;
    std::string detail;
};

} // namespace model
} // namespace videoeye
