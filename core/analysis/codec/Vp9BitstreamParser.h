#pragma once

#include <cstdint>
#include <string>

#include "core/domain/model/BitstreamInfo.h"
#include "core/media/codec/BitReader.h"

namespace videoeye {

// ==========================================================================
// VP9 码流解析器（WebM / MP4(vpcC) / IVF）
//
// 与其它 codec parser 最大的差别：**VP9 没有参数集**。
// 没有 SPS/PPS/VPS 这种能单独取出来的单元 —— 「该按什么分辨率/位深/色彩去解」
// 全部写在**每个关键帧的 uncompressed header**（规范 6.2）里。所以本解析器的
// 输入永远是「一帧（或一个访问单元）的字节」，而不是配置记录。
//
// 位序：uncompressed header 的 f(n) 是 **MSB-first**（与 H.264/HEVC 的 RBSP 一致），
// 可以直接用 videoeye::BitReader。（VP8 才是 LSB-first，别混。）
// 可以自查：`0x82` 开头即 profile 0 关键帧 —— 二进制 1000 0010 解出来就是
// frame_marker=2 / profile=0 / show_existing=0 / keyframe / show=1 / no error resilient。
//
// 解析深度：到 render_size() 为止。
// 再往后的 loop_filter / quantization / segmentation / tile_info 不全在此期间：
// tile_info 起进入了 bool-decoder（算术）域，而 header_size_in_bytes 又在其后 ——
// 为了拿「QP」去实现一整套布尔解码器不划算，所以 base_q_index 恒为 0（未解析）。
// 需要逐帧 QP 时请看 slice header 那条线（H.264/HEVC）。
//
// 这就是 C1 的存在理由：WebM 是一等容器、MP4 侧也已经能显示 vp09 的 codec 名，
// 但**位流侧没有对应解析器** —— 这条流一度完全落不到码流面板里。
// ==========================================================================
class Vp9BitstreamParser {
public:
    // 从一帧 VP9 数据里解序列头。必须传入**关键帧**（见 LooksLikeKeyFrame）：
    // 非关键帧的 header 里没有 color_config / frame_size，解不出任何 Morphable
    // 显示配置，此时返回 present=false。
    static model::Vp9SequenceHeaderInfo ParseFromFrame(const uint8_t* data, size_t size);

    // 数据里是否长得像 VP9 关键帧：frame_marker=2、frame_type=0、帧同步码 0x49 0x83 0x42。
    // 用于在投入整段解析之前廉价地筛掉错帧 / 乱摊位置 / 非 key frame 的包。
    static bool LooksLikeKeyFrame(const uint8_t* data, size_t size);

    // Profile 名称（单元测试亦直接调用）
    static std::string GetProfileName(int profile);

    // VP9 的 color_space 是「旧式 MPEG 矩阵式三元组」，换成 CICP（ISO/IEC 23001-8）
    // 三元组后才能和 AVCodecParameters 的 color_primaries / trc / space 直接比对。
    // 映射照 FFmpeg libvpx 的写法（vpx Li encoding 时的助教映射）。
    // 返回 false 表示未知（UCF i.e. CSP_UNKNOWN）——调用方应跳过这项比对。
    static bool ColorSpaceToCicp(int color_space, int& color_primaries,
                                 int& transfer_characteristics, int& matrix_coefficients);

    // 按 Annex A.2 的表由「帧尺寸 + 帧率」反查 Level，返回 vpcC 的同款编码值
    // （10=Level 1、11=1.1、20=2 ... 62=6.2，0 = 未知/超上限）。
    // fps <= 0 时只用 LumaPicSize 判据（给的是下界：实际 level 只会更高不会更低）。
    static int ComputeLevel(int width, int height, double fps);

    // vpcC 编码值 -> "2.1" 这样的展示串（0 -> "unknown"）
    static std::string GetLevelName(int level_code);
};

}  // namespace videoeye
