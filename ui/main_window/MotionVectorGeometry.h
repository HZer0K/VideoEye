#pragma once

#include <cmath>
#include <cstddef>

#include "core/domain/model/MacroblockInfo.h"

namespace videoeye {
namespace ui {

// 运动矢量叠加层的坐标换算。
//
// 单独抽成纯函数是为了能做回归测试: FFmpeg AVMotionVector 的 dst_x/dst_y 是块"中心"，
// 箭头必须从中心出发、块矩形必须从中心向四周展开半个块。这两处一旦算错，整层叠加
// 会整体偏移半个宏块 —— 缩放后肉眼很难发现，但频道分析时箭头会指到错误的块上。
// 这里刻意不引入 Qt 类型，单测可以直接用普通数值断言。
struct MvOverlayViewport {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct MvArrowGeometry {
    double start_x = 0.0;
    double start_y = 0.0;
    double end_x = 0.0;
    double end_y = 0.0;
    double length = 0.0; // 截断到 max_length 之后的实际长度
};

struct MvBlockGeometry {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// 运动矢量的像素幅度 (motion_scale 缺失时按 1 处理, 避免除零)
inline double MvPixelMagnitude(const model::MotionVectorInfo& mv) {
    const double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
    const double px = mv.motion_x / scale;
    const double py = mv.motion_y / scale;
    return std::sqrt(px * px + py * py);
}

// 抽样步长：一帧的矢量条数超过上限时，按记录顺序等距取点。
// 返回 1 表示全量绘制；否则每间隔 stride 条画一条。上限为 0 时不做抽样。
inline std::size_t MvSampleStride(std::size_t count, std::size_t max_count) {
    if (max_count == 0 || count <= max_count)
        return 1;
    return (count + max_count - 1) / max_count;
}

// 箭头: 起点 = 块中心在画面里的位置, 终点 = 起点 + 放大后的运动矢量 (超过 max_length 截断)。
// frame_width / frame_height 非法时返回全 0, 调用方据此跳过绘制。
inline MvArrowGeometry ComputeMvArrow(const model::MotionVectorInfo& mv, const MvOverlayViewport& viewport,
                                      int frame_width, int frame_height, double amplify, double max_length) {
    MvArrowGeometry geo;
    if (frame_width <= 0 || frame_height <= 0)
        return geo;

    const double sx = viewport.width / frame_width;
    const double sy = viewport.height / frame_height;
    const double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;

    // dst 是块中心, 这里不再加半个块宽高。
    geo.start_x = viewport.left + mv.block_x * sx;
    geo.start_y = viewport.top + mv.block_y * sy;

    double dx = (mv.motion_x / scale) * sx * amplify;
    double dy = (mv.motion_y / scale) * sy * amplify;
    double length = std::sqrt(dx * dx + dy * dy);
    if (length > max_length && length > 0.0) {
        const double ratio = max_length / length;
        dx *= ratio;
        dy *= ratio;
        length = max_length;
    }

    geo.end_x = geo.start_x + dx;
    geo.end_y = geo.start_y + dy;
    geo.length = length;
    return geo;
}

// 块矩形: 以块中心为基准向四周展开半个块。
inline MvBlockGeometry ComputeMvBlock(const model::MotionVectorInfo& mv, const MvOverlayViewport& viewport,
                                      int frame_width, int frame_height) {
    MvBlockGeometry geo;
    if (frame_width <= 0 || frame_height <= 0)
        return geo;

    const double sx = viewport.width / frame_width;
    const double sy = viewport.height / frame_height;
    geo.left = viewport.left + (mv.block_x - mv.block_w / 2.0) * sx;
    geo.top = viewport.top + (mv.block_y - mv.block_h / 2.0) * sy;
    geo.width = mv.block_w * sx;
    geo.height = mv.block_h * sy;
    return geo;
}

} // namespace ui
} // namespace videoeye
