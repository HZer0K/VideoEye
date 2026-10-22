#include "core/domain/model/CeaCaptionInfo.h"

namespace videoeye {
namespace model {

std::string Cea608Stats::ChannelText() const {
    std::string text;
    static const char* kNames[4] = {"CC1", "CC2", "CC3", "CC4"};
    for (int i = 0; i < 4; ++i) {
        if ((channel_mask & (1 << i)) == 0) continue;
        if (!text.empty()) text += ", ";
        text += kNames[i];
    }
    return text;
}

std::string Cea708Stats::ServiceText() const {
    std::string text;
    for (int service : services) {
        if (!text.empty()) text += ", ";
        text += "S" + std::to_string(service);
    }
    return text;
}

}  // namespace model
}  // namespace videoeye
