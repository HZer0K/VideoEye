#include "core/domain/model/AudioBitstreamInfo.h"

#include <string>

namespace videoeye {
namespace model {

const char* ToString(AudioBitstreamCodec codec) {
    switch (codec) {
    case AudioBitstreamCodec::AacAdts:
        return "AAC (ADTS)";
    case AudioBitstreamCodec::AacLatm:
        return "AAC (LATM/LOAS)";
    case AudioBitstreamCodec::AacRaw:
        return "AAC (raw + ASC)";
    case AudioBitstreamCodec::Ac3:
        return "AC-3";
    case AudioBitstreamCodec::Eac3:
        return "E-AC-3";
    case AudioBitstreamCodec::Opus:
        return "Opus";
    case AudioBitstreamCodec::Mp3:
        return "MP3";
    case AudioBitstreamCodec::Flac:
        return "FLAC";
    case AudioBitstreamCodec::Other:
        return "其它";
    case AudioBitstreamCodec::Unknown:
    default:
        return "未知";
    }
}

const char* AudioBitstreamCodecCode(AudioBitstreamCodec codec) {
    switch (codec) {
    case AudioBitstreamCodec::AacAdts:
        return "aac_adts";
    case AudioBitstreamCodec::AacLatm:
        return "aac_latm";
    case AudioBitstreamCodec::AacRaw:
        return "aac_raw";
    case AudioBitstreamCodec::Ac3:
        return "ac3";
    case AudioBitstreamCodec::Eac3:
        return "eac3";
    case AudioBitstreamCodec::Opus:
        return "opus";
    case AudioBitstreamCodec::Mp3:
        return "mp3";
    case AudioBitstreamCodec::Flac:
        return "flac";
    case AudioBitstreamCodec::Other:
        return "other";
    case AudioBitstreamCodec::Unknown:
    default:
        return "unknown";
    }
}

const char* AudioObjectTypeName(int audio_object_type) {
    switch (audio_object_type) {
    case 1:
        return "AAC Main";
    case 2:
        return "AAC LC";
    case 3:
        return "AAC SSR";
    case 4:
        return "AAC LTP";
    case 5:
        return "HE-AAC (SBR)";
    case 6:
        return "AAC Scalable";
    case 17:
        return "ER AAC LC";
    case 23:
        return "ER AAC LD";
    case 29:
        return "HE-AACv2 (SBR+PS)";
    case 39:
        return "AAC ELD";
    case 42:
        return "USAC / xHE-AAC";
    default:
        return "AAC";
    }
}

const char* OpusModeName(int mode) {
    switch (mode) {
    case 0:
        return "SILK";
    case 1:
        return "Hybrid";
    case 2:
        return "CELT";
    default:
        return "未知";
    }
}

const char* OpusBandwidthName(int bandwidth) {
    switch (bandwidth) {
    case 0:
        return "NB";
    case 1:
        return "MB";
    case 2:
        return "WB";
    case 3:
        return "SWB";
    case 4:
        return "FB";
    default:
        return "未知";
    }
}

int AcmodChannelCount(int acmod) {
    // acmod 0 是 1+1（Ch1/Ch2 两个独立单声道），其余按 1/0..3/2 递增。
    static const int kChannels[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    if (acmod < 0 || acmod > 7)
        return 0;
    return kChannels[acmod];
}

std::string DescribeAcmod(int acmod, bool lfeon) {
    if (acmod < 0 || acmod > 7)
        return std::string();
    static const char* kLayout[8] = {
        "1+1 (dual mono)", "1/0 (mono)", "2/0 (stereo)", "3/0", "2/1", "3/1", "2/2", "3/2",
    };
    std::string text = kLayout[acmod];
    if (lfeon) {
        text += " + LFE";
        const int channels = AcmodChannelCount(acmod) + 1;
        switch (channels) {
        case 2:
            text += " (2.1)";
            break;
        case 3:
            text += " (3.1)";
            break;
        case 4:
            text += " (4.1)";
            break;
        case 5:
            text += " (5.1)";
            break;
        case 6:
            text += " (6.1)";
            break;
        default:
            break;
        }
    } else {
        const int channels = AcmodChannelCount(acmod);
        if (channels == 1)
            text += " (mono)";
        else if (channels == 2)
            text += " (stereo)";
        else if (channels == 3)
            text += " (3.0)";
        else if (channels == 4)
            text += " (4.0)";
        else if (channels == 5)
            text += " (5.0)";
    }
    return text;
}

std::string AudioBitstreamResult::ToString() const {
    std::string text = "音频位流: streams=" + std::to_string(streams.size()) +
                       " parsed=" + std::to_string(parsed_stream_count) +
                       " atmos=" + std::to_string(atmos_stream_count);
    if (!analyzed)
        text += " (未执行)";
    return text;
}

} // namespace model
} // namespace videoeye
