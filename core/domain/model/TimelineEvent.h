#pragma once

#include <string>

namespace videoeye {
namespace model {

struct TimelineEvent {
    int index = 0;
    std::string category;
    double timestamp_seconds = 0.0;
    std::string label;
    std::string detail;
};

} // namespace model
} // namespace videoeye
