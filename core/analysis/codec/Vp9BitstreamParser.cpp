#include "core/analysis/codec/Vp9BitstreamParser.h"

#include <cmath>

namespace videoeye {
namespace {

// Annex A.2 的等级上限表。与 Vp9BitstreamInfo.cpp 里那份保持一致 ——
// 那边是从「编码值」查名字，这边是从「尺寸/采样率」查编码值，两份必须同步改。
struct Vp9LevelLimit {
    int code;
    int64_t luma_sample_rate; // samples/s
    int64_t luma_pic_size;    // pixels/frame
};

const Vp9LevelLimit kVp9LevelLimits[] = {
    {10, 829440LL, 36864LL},        {11, 2764800LL, 73728LL},       {20, 4608000LL, 122880LL},
    {21, 9216000LL, 245760LL},      {30, 20736000LL, 552960LL},     {31, 36864000LL, 983040LL},
    {40, 83558400LL, 2228224LL},    {41, 160432128LL, 2228224LL},   {50, 311951360LL, 8912896LL},
    {51, 588251136LL, 8912896LL},   {52, 1176502272LL, 8912896LL},  {60, 1176502272LL, 35651584LL},
    {61, 2353004544LL, 35651584LL}, {62, 4706009088LL, 35651584LL},
};

// VP9 的同步码 0x49 0x83 0x42（规范 6.2 的 frame_sync_code）
constexpr uint32_t kFrameSyncCode = 0x498342u;

// 合理图像尺寸上限：32K x 32K 之外一律当作垃圾码流拒绝，别把整份结果开出
// 负的/溢出的分辨率去参与容器比对。
constexpr int kMaxPictureSize = 32768;

} // namespace

bool Vp9BitstreamParser::LooksLikeKeyFrame(const uint8_t* data, size_t size) {
    // 完整开销：frame_marker(2) + profile(2) + [reserved(1)] + show_existing(1)
    //           + frame_type(1) + show_frame(1) + error_resilient(1) = 9~10 bit，
    //           再跟 24 bit 的 sync code → 最多 34 bit ≈ 5 字节。
    // 用 6 字节门槛是不想把一个"刚好够放着 xxxx"的输入当成候选再去读字节。
    if (data == nullptr || size < 6)
        return false;

    BitReader reader;
    reader.Reset(data, size);

    if (reader.ReadBits(2) != 0x02)
        return false; // frame_marker 必须恒为 2
    // 两个 ReadBit() 必须分成两条语句：写进同一个表达式里，两者的求值顺序在 C++ 里
    // 是未指定的，profile 1/2 会被随机读反，profile 3 的 reserved 位也会读错位置。
    const int profile_low = static_cast<int>(reader.ReadBit());
    const int profile_high = static_cast<int>(reader.ReadBit());
    const int profile = (profile_high << 1) | profile_low;
    if (profile > 3)
        return false;
    if (profile == 3 && reader.ReadBit() != 0)
        return false; // reserved_zero 必须恒为 0
    if (reader.ReadBit() != 0)
        return false; // show_existing_frame == 1 → 不是序列头来源
    if (reader.ReadBit() != 0)
        return false; // frame_type != KEY_FRAME
    if (reader.HasError())
        return false;
    reader.SkipBits(2); // show_frame / error_resilient_mode
    return !reader.HasError() && reader.ReadBits(24) == kFrameSyncCode;
}

model::Vp9SequenceHeaderInfo Vp9BitstreamParser::ParseFromFrame(const uint8_t* data, size_t size) {
    model::Vp9SequenceHeaderInfo sh;
    if (data == nullptr || size < 6)
        return sh;

    BitReader reader;
    reader.Reset(data, size);

    // ---- frame header prefix ----
    const uint32_t frame_marker = reader.ReadBits(2);
    if (frame_marker != 0x02)
        return sh; // 不是 VP9 帧（或已经错位）
    const int profile_low = static_cast<int>(reader.ReadBit());
    const int profile_high = static_cast<int>(reader.ReadBit());
    sh.profile = (profile_high << 1) | profile_low;
    if (sh.profile > 3)
        return sh;
    if (sh.profile == 3) {
        const uint32_t reserved_zero = reader.ReadBit();
        if (reserved_zero != 0)
            return sh; // 违反规范：profile 3 的保留位必须是 0
    }

    if (reader.ReadBit()) { // show_existing_frame
        // 只"重放"一张已解码帧：这帧头里既没有尺寸也没有色彩配置，
        // 拿它当序列头来源会得到一堆 0。
        reader.SkipBits(3); // frame_to_show_map_idx
        return sh;
    }

    const bool key_frame = !reader.ReadBit(); // frame_type: 0 = KEY_FRAME
    sh.key_frame = key_frame;
    // 规范 6.2 的顺序是 frame_type -> show_frame -> error_resilient_mode。
    // 少读 show_frame 一位，后面 24 bit 的 frame_sync_code 就会整体错位 1 bit。
    sh.show_frame = reader.ReadBit() != 0;
    sh.error_resilient_mode = reader.ReadBit() != 0;

    if (!key_frame) {
        // 非关键帧：没有 color_config / frame_size / render_size。
        // 唯一的例外是 intra_only 的帧（FrameIsIntra），但它大多出现在
        // alt-ref 里，同样没有 resolution —— 这里一律不解析，交给调用方
        // 继续等到关键帧（见 InBandParameterSetProbe 的攒包策略）。
        return sh;
    }

    // ---- 关键帧 ----
    const uint32_t sync_code = reader.ReadBits(24);
    if (sync_code != kFrameSyncCode)
        return sh; // 同步码不对，说明流的起点找错了

    // ---- color_config()（规范 6.2.2）----
    if (sh.profile == 2 || sh.profile == 3) {
        sh.bit_depth = reader.ReadBit() ? 12 : 10; // ten_or_twelve_bit
    } else {
        sh.bit_depth = 8;
    }

    sh.color_space = static_cast<model::Vp9ColorSpace>(static_cast<int>(reader.ReadBits(3)));
    const bool compos_srgb = (sh.color_space == model::Vp9ColorSpace::Srgb);
    const bool has_range_flag = (sh.profile == 1 || sh.profile == 3);
    if (!compos_srgb) {
        if (has_range_flag) {
            sh.color_range_full = reader.ReadBit() != 0;
        }
        if (sh.profile == 1 || sh.profile == 3) {
            sh.subsampling_x = static_cast<int>(reader.ReadBit());
            sh.subsampling_y = static_cast<int>(reader.ReadBit());
            reader.SkipBits(1); // reserved_zero
        } else {
            sh.subsampling_x = 1;
            sh.subsampling_y = 1;
        }
    } else {
        if (has_range_flag) {
            sh.color_range_full = reader.ReadBit() != 0;
        }
        sh.subsampling_x = 0;
        sh.subsampling_y = 0;
        if (has_range_flag) {
            reader.SkipBits(1); // reserved_zero
        }
    }
    if (reader.HasError())
        return sh;

    // ---- frame_size()（6.2.4）----
    sh.frame_width = static_cast<int>(reader.ReadBits(16)) + 1;
    sh.frame_height = static_cast<int>(reader.ReadBits(16)) + 1;
    if (reader.HasError())
        return sh;
    if (sh.frame_width <= 0 || sh.frame_height <= 0 || sh.frame_width > kMaxPictureSize ||
        sh.frame_height > kMaxPictureSize) {
        // 尺寸越界只可能来自垃圾/错位数据：宁可整份作废，也不要产出一个
        // 能污染容器比对表的荒谬分辨率。
        sh.frame_width = 0;
        sh.frame_height = 0;
        return sh;
    }

    // ---- render_size()（6.2.5）----
    sh.render_size_different = reader.ReadBit() != 0;
    if (sh.render_size_different) {
        sh.render_width = static_cast<int>(reader.ReadBits(16)) + 1;
        sh.render_height = static_cast<int>(reader.ReadBits(16)) + 1;
        if (sh.render_width <= 0 || sh.render_width > kMaxPictureSize || sh.render_height <= 0 ||
            sh.render_height > kMaxPictureSize) {
            sh.render_size_different = false;
            sh.render_width = 0;
            sh.render_height = 0;
        }
    }
    if (reader.HasError())
        return sh;

    sh.present = true;
    return sh;
}

std::string Vp9BitstreamParser::GetProfileName(int profile) {
    switch (profile) {
    case 0:
        return "Profile 0 (8-bit 4:2:0)";
    case 1:
        return "Profile 1 (8-bit 4:2:2/4:4:4)";
    case 2:
        return "Profile 2 (10/12-bit 4:2:0)";
    case 3:
        return "Profile 3 (10/12-bit 4:2:2/4:4:4)";
    default:
        return "Profile " + std::to_string(profile);
    }
}

bool Vp9BitstreamParser::ColorSpaceToCicp(int color_space, int& color_primaries, int& transfer_characteristics,
                                          int& matrix_coefficients) {
    // 取值照 libvpx 的 vpx_codec_enc_config / FFmpeg libvpx enc 的 color_space 映射。
    // 注意 BT.601 与 SMPTE 170M 在 VP9 里是**两个不同的枚举**（170M 是 BT.601 的
    // NTSC 变体），但它们换到 CICP 后是同一组值 —— 这里保留 FFmpeg 的写法。
    switch (color_space) {
    case static_cast<int>(model::Vp9ColorSpace::Bt709): // = MXF ???
        color_primaries = 1;                            // AVCOL_PRI_BT709
        transfer_characteristics = 1;                   // AVCOL_TRC_BT709
        matrix_coefficients = 1;                        // AVCOL_SPC_BT709
        return true;
    case static_cast<int>(model::Vp9ColorSpace::Bt601):
        color_primaries = 5;          // AVCOL_PRI_BT470BG
        transfer_characteristics = 6; // AVCOL_TRC_SMPTE170M
        matrix_coefficients = 6;      // AVCOL_SPC_SMPTE170M
        return true;
    case static_cast<int>(model::Vp9ColorSpace::Smpte170):
        color_primaries = 6;          // AVCOL_PRI_SMPTE170M
        transfer_characteristics = 6; // AVCOL_TRC_SMPTE170M
        matrix_coefficients = 6;      // AVCOL_SPC_SMPTE170M
        return true;
    case static_cast<int>(model::Vp9ColorSpace::Smpte240):
        color_primaries = 7;          // AVCOL_PRI_SMPTE240M
        transfer_characteristics = 7; // AVCOL_TRC_SMPTE240M
        matrix_coefficients = 7;      // AVCOL_SPC_SMPTE240M
        return true;
    case static_cast<int>(model::Vp9ColorSpace::Bt2020Ncl):
        color_primaries = 9;           // AVCOL_PRI_BT2020
        transfer_characteristics = 16; // AVCOL_TRC_BT2020_10
        matrix_coefficients = 9;       // AVCOL_SPC_BT2020_NCL
        return true;
    case static_cast<int>(model::Vp9ColorSpace::Bt2020Cl):
        color_primaries = 9;           // AVCOL_PRI_BT2020
        transfer_characteristics = 16; // AVCOL_TRC_BT2020_10
        matrix_coefficients = 10;      // AVCOL_SPC_BT2020_CL
        return true;
    case static_cast<int>(model::Vp9ColorSpace::Srgb):
        color_primaries = 1;           // AVCOL_PRI_BT709
        transfer_characteristics = 13; // AVCOL_TRC_IEC61966_2_1
        matrix_coefficients = 0;       // AVCOL_SPC_RGB
        return true;
    default:
        return false; // Unknown：容器侧可能没写、也可能写了 —— 无法判定不一致
    }
}

int Vp9BitstreamParser::ComputeLevel(int width, int height, double fps) {
    if (width <= 0 || height <= 0)
        return 0;
    const int64_t pic_size = static_cast<int64_t>(width) * height;
    const bool have_fps = (fps > 0.0f) && std::isfinite(fps);
    const int64_t sample_rate = have_fps ? static_cast<int64_t>(std::llround(pic_size * fps)) : 0;

    for (const auto& limit : kVp9LevelLimits) {
        if (pic_size > limit.luma_pic_size)
            continue;
        // 不知道帧率时只按图像尺寸判据 —— 给的是下界，真实 level 只能更高
        if (have_fps && sample_rate > limit.luma_sample_rate)
            continue;
        return limit.code;
    }
    return 0; // 超出 6.2 的上限
}

std::string Vp9BitstreamParser::GetLevelName(int level_code) {
    if (level_code <= 0)
        return "unknown";
    // 编码规则：十位以上 = 主版本号，个位 = 次版本号（10 -> "1", 11 -> "1.1"）
    const int major = level_code / 10;
    const int minor = level_code % 10;
    if (minor == 0)
        return std::to_string(major);
    return std::to_string(major) + "." + std::to_string(minor);
}

} // namespace videoeye
