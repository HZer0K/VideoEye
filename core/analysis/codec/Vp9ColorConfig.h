#pragma once

// VP9 color_config()（规范 6.2.2）的唯一位布局实现。
//
// 为什么抽出来：这段有**两条**调用路径 —— 位流面板要拿它填 Vp9SequenceHeaderInfo
// （颜色空间 / 位深 / 子采样），帧顺序追踪要拿它算"intra_only 帧的
// refresh_frame_flags 从第几位开始"。抄成两份的代价在历史上已经付过一次：
//
// ❗ 这段的坑：ColorRange 对**所有 profile** 都无条件存在，而 profile 1/3
// 在它前面/后面各多一位 reserved_zero。曾经把"有没有 ColorRange"和
// "profile 是不是 1/3"绑成同一个 has_range_flag，于是两个方向各错 1 位：
//   * 非 RGB + profile 0/2：少读 ColorRange → frame_size 提前 1 位，
//     高度读成 32822（> 32768）→ 整份作废；
//   * RGB + profile 1/3：多读一个不存在的 reserved_zero → 192x108 读成
//     383x215（191<<1|1、107<<1|1）。
// 两个分支必须一起改，只补 ColorRange 会把 RGB 分支继续留在错的状态。
//
// 位序：f(n) 是 MSB-first（VP8 才是 LSB-first），可直接用 BitReader。

#include "core/domain/model/Vp9BitstreamInfo.h"
#include "core/media/codec/BitReader.h"

namespace videoeye {

struct Vp9ColorConfigInfo {
    int bit_depth = 8;
    int color_space = 0; // 0..7，enum Vp9ColorSpace 的数值
    bool full_range = false;
    int subsampling_x = 1;
    int subsampling_y = 1;
};

// 返回 false = reserved_zero 非 0（违反规范）或位流被截断，两种情况都不该继续。
inline bool ParseVp9ColorConfig(BitReader& reader, int profile, Vp9ColorConfigInfo& out) {
    out = Vp9ColorConfigInfo();

    // ten_or_twelve_bit：只有 profile 2/3 有
    out.bit_depth = (profile == 2 || profile == 3) ? (reader.ReadBit() ? 12 : 10) : 8;

    out.color_space = static_cast<int>(reader.ReadBits(3));
    const bool is_444_profile = (profile == 1 || profile == 3);
    const bool is_rgb = (out.color_space == static_cast<int>(model::Vp9ColorSpace::Srgb));

    if (!is_rgb) {
        if (is_444_profile) {
            // reserved_zero —— profile 1/3 的非 RGB 分支独有，必须为 0
            if (reader.ReadBit() != 0)
                return false;
        }
        out.full_range = reader.ReadBit() != 0; // 所有 profile 都有
        if (is_444_profile) {
            out.subsampling_x = static_cast<int>(reader.ReadBit());
            out.subsampling_y = static_cast<int>(reader.ReadBit());
            // reserved_zero —— 同上
            if (reader.ReadBit() != 0)
                return false;
        } else {
            out.subsampling_x = 1;
            out.subsampling_y = 1;
        }
    } else {
        // RGB 分支：只有 ColorRange，没有 subsampling、也没有 reserved_zero
        out.full_range = reader.ReadBit() != 0;
        out.subsampling_x = 0;
        out.subsampling_y = 0;
    }
    return !reader.HasError();
}

} // namespace videoeye
