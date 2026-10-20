#include "core/domain/model/Vp9BitstreamInfo.h"

#include <cmath>

namespace videoeye {
namespace model {
namespace {

// VP9 等级表的 vpcC 编码值（ISO/IEC 14496-30 表 1）
//
// vpcC / ffprobe / movenc 用的都是这套整数编码：10 = Level 1、11 = Level 1.1、
// 20 = Level 2 ... 62 = Level 6.2。这里统一存这个编码值，避免同一个 level 在
// 两条数据链里出现两种写法（曾经 av1C 的 level_idc 就是这样被两套口径用混的）。
struct Vp9LevelEntry {
    int code;               // vpcC 编码
    const char* name;       // 展示名
    int64_t luma_sample_rate;  // Annex A.2 的 LumaSampleRate 上限（samples/s）
    int64_t luma_pic_size;     // Annex A.2 的 LumaPicSize 上限（像素/帧）
};

const Vp9LevelEntry kVp9Levels[] = {
    {10, "1", 829440LL, 36864LL},
    {11, "1.1", 2764800LL, 73728LL},
    {20, "2", 4608000LL, 122880LL},
    {21, "2.1", 9216000LL, 245760LL},
    {30, "3", 20736000LL, 552960LL},
    {31, "3.1", 36864000LL, 983040LL},
    {40, "4", 83558400LL, 2228224LL},
    {41, "4.1", 160432128LL, 2228224LL},
    {50, "5", 311951360LL, 8912896LL},
    {51, "5.1", 588251136LL, 8912896LL},
    {52, "5.2", 1176502272LL, 8912896LL},
    {60, "6", 1176502272LL, 35651584LL},
    {61, "6.1", 2353004544LL, 35651584LL},
    {62, "6.2", 4706009088LL, 35651584LL},
};

std::string LevelNameByCode(int code) {
    for (const auto& entry : kVp9Levels) {
        if (entry.code == code) return entry.name;
    }
    return code > 0 ? ("Level " + std::to_string(code / 10) + "." + std::to_string(code % 10))
                    : "unknown";
}

const char* RawChromaSubsamplingName(int chroma_subsampling) {
    switch (chroma_subsampling) {
        case 0: return "4:4:4";
        case 1: return "4:2:2";
        case 2: return "4:2:0";
        case 3: return "4:4:0";
        default: return "unknown";
    }
}

}  // namespace

// ---- Vp9SequenceHeaderInfo ----

std::string Vp9SequenceHeaderInfo::ChromaSubsamplingName() const {
    // subsampling_x / subsampling_y -> vpcC 的 chroma_subsampling 编码
    //   0 = 4:4:4 (x=0,y=0) / 1 = 4:2:2 (x=1,y=0) / 2 = 4:2:0 (x=1,y=1) / 3 = 4:4:0 (x=0,y=1)
    const int code = (subsampling_x << 1) | subsampling_y;
    switch (code) {
        case 0: return "4:4:4";  // x=0 y=0
        case 1: return "4:4:0";  // x=0 y=1
        case 2: return "4:2:2";  // x=1 y=0
        case 3: return "4:2:0";  // x=1 y=1
        default: return "unknown";
    }
}

std::string Vp9SequenceHeaderInfo::ProfileName() const {
    switch (profile) {
        case 0: return "Profile 0 (8-bit 4:2:0)";
        case 1: return "Profile 1 (8-bit 4:2:2/4:4:4)";
        case 2: return "Profile 2 (10/12-bit 4:2:0)";
        case 3: return "Profile 3 (10/12-bit 4:2:2/4:4:4)";
        default: return "Profile " + std::to_string(profile);
    }
}

std::string Vp9SequenceHeaderInfo::LevelName() const { return LevelNameByCode(level); }

std::string Vp9SequenceHeaderInfo::ColorSpaceName() const {
    switch (color_space) {
        case Vp9ColorSpace::Unknown: return "unknown (CSP_UNKNOWN)";
        case Vp9ColorSpace::Bt601: return "BT.601";
        case Vp9ColorSpace::Bt709: return "BT.709";
        case Vp9ColorSpace::Smpte170: return "SMPTE 170M";
        case Vp9ColorSpace::Smpte240: return "SMPTE 240M";
        case Vp9ColorSpace::Bt2020Ncl: return "BT.2020 NCL";
        case Vp9ColorSpace::Bt2020Cl: return "BT.2020 CL";
        case Vp9ColorSpace::Srgb: return "sRGB";
        default: return "unknown";
    }
}

std::string Vp9SequenceHeaderInfo::Summary() const {
    std::string out = ProfileName();
    out += " / " + std::to_string(width()) + "x" + std::to_string(height());
    out += " / " + std::to_string(BitDepth()) + "bit";
    out += " / " + ChromaSubsamplingName();
    out += " / " + LevelName();
    out += " / " + ColorSpaceName();
    out += std::string(" / ") + (color_range_full ? "full range" : "studio range");
    return out;
}

// ---- Vp9CodecConfigInfo ----

std::string Vp9CodecConfigInfo::ProfileName() const {
    switch (profile) {
        case 0: return "Profile 0 (8-bit 4:2:0)";
        case 1: return "Profile 1 (8-bit 4:2:2/4:4:4)";
        case 2: return "Profile 2 (10/12-bit 4:2:0)";
        case 3: return "Profile 3 (10/12-bit 4:2:2/4:4:4)";
        default: return "Profile " + std::to_string(profile);
    }
}

std::string Vp9CodecConfigInfo::LevelName() const { return LevelNameByCode(level); }

std::string Vp9CodecConfigInfo::ChromaSubsamplingName() const {
    return RawChromaSubsamplingName(chroma_subsampling);
}

}  // namespace model
}  // namespace videoeye
