#pragma once

// VP9 码流解析结果：序列头（关键帧 uncompressed header）与 vpcC 配置记录。
//
// 与 H264BitstreamInfo.h / HevcBitstreamInfo.h 同层：只依赖标准库，
// 由 core/domain/model/BitstreamInfo.h 作为 umbrella 统一 include。
//
// VP9 与其它 codec 最大的差别：**没有独立的参数集**。
// 没有 SPS/PPS/VPS 这种可以单独取出来的单元 —— 图像尺寸、位深、色彩描述
// 全写在**每个关键帧的 uncompressed header 里**。所以本文件的
// Vp9SequenceHeaderInfo 语义上是"从第一个关键帧解出的显示配置"，
// 而不是"一份可以独立复用的参数集"。

#include <cstdint>
#include <string>

namespace videoeye {
namespace model {

// VP9 颜色空间枚举（规范 6.2.2 color_config()，CICP 之前的老式写法）
enum class Vp9ColorSpace : int {
    Unknown = 0,
    Bt601 = 1,
    Bt709 = 2,
    Smpte170 = 3,
    Smpte240 = 4,
    Bt2020Ncl = 5, // BT.2020 非恒定亮度
    Bt2020Cl = 6,  // BT.2020 恒定亮度
    Srgb = 7,
};

// --------------------------------------------------------------------------
// VP9 序列头（从第一个关键帧的 uncompressed header 解出）
// --------------------------------------------------------------------------
struct Vp9SequenceHeaderInfo {
    bool present = false;

    // ---- profile_tier / 基本能力 ----
    int profile = 0;   // 0..3
    int bit_depth = 8; // 8 / 10 / 12

    // ---- color_config()（6.2.2）----
    Vp9ColorSpace color_space = Vp9ColorSpace::Unknown;
    bool color_range_full = false; // 1 = full range（studio swing 为 0）
    int subsampling_x = 1;         // 0 = 水平无下采样
    int subsampling_y = 1;

    // ---- frame_size() / render_size()（6.2.4 / 6.2.5）----
    int frame_width = 0; // 编码尺寸（frame_size）
    int frame_height = 0;
    bool render_size_different = false;
    int render_width = 0; // 拉伸后的显示尺寸
    int render_height = 0;

    // ---- 帧头其它诊断字段 ----
    bool key_frame = false;
    bool show_frame = true; // 0 = 这帧不立即显示（alt-ref 之类）
    bool error_resilient_mode = false;
    bool intra_only = false; // 非关键帧里的 intra only 帧；本解析器只到
                             // render_size()，这一位在其后才出现，故恒为 false
    int refresh_frame_flags = 0;
    int base_q_index = 0;         // quantization_params(): base_q_idx（0..255）
    bool lossless = false;        // base_q_idx==0 且 Lossless 标志组合见 has_lossless
    int header_size_in_bytes = 0; // uncompressed header 之后的压缩头部长度

    // ---- 由上面字段派生的 swallowed 值 ----
    int level = 0;                         // 由图像尺寸/采样率按 Annex A.2 表推出；0 = 未知
    bool has_resolution_from_vpcc = false; // true = 尺寸来自 vpcC，非关键帧解出

    // 派生值
    // 显示尺寸：render_size_different 时以 render_size 为准（这是播放器该铺的尺寸）
    int width() const {
        if (render_size_different && render_width > 0)
            return render_width;
        return frame_width;
    }
    int height() const {
        if (render_size_different && render_height > 0)
            return render_height;
        return frame_height;
    }

    int FrameWidth() const {
        return frame_width;
    }
    int FrameHeight() const {
        return frame_height;
    }
    int BitDepth() const {
        return bit_depth > 0 ? bit_depth : 8;
    }

    // 4:2:0 / 4:2:2 / 4:4:4 / 4:4:0
    std::string ChromaSubsamplingName() const;
    // "Profile 0" ... "Profile 3"
    std::string ProfileName() const;
    std::string LevelName() const;
    std::string ColorSpaceName() const;
    std::string Summary() const;
};

// --------------------------------------------------------------------------
// vpcC（MP4 的 VP Codec Configuration Box，ISO/IEC 14496-30）
//
// WebM/Matroska 的 VP9 **没有 codec private**（序列参数全在码流里），MP4 则有
// vpcC。它的定位与 av1C / vvcC 相同：extradata 只有它、没有裸关键帧时的兜底
// 数据源。分辨率/帧率一概不在里面 —— 只有 profile/level/位深/色度/range/色彩。
// --------------------------------------------------------------------------
struct Vp9CodecConfigInfo {
    bool present = false;

    int profile = 0;
    int level = 0; // vpcC 的 level 字段（1..7 级，实际记 10*映射见 GetLevelName）
    int bit_depth = 8;
    int chroma_subsampling = 2; // 0=4:4:4 1=4:2:2 2=4:2:0 3=4:4:0
    bool video_full_range_flag = false;
    int color_primaries = 0;
    int transfer_characteristics = 0;
    int matrix_coefficients = 0;
    bool color_description_present = false; // 非全 0 的 CICP 三元组才算 present

    std::string ProfileName() const;
    std::string LevelName() const;
    std::string ChromaSubsamplingName() const;
};

} // namespace model
} // namespace videoeye
