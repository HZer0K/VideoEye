#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace videoeye::model {

// 单帧渲染预算：画面从解码帧转成 QImage(Format_ARGB32) / AV_PIX_FMT_BGRA，
// 两边都是 4 字节/像素，所以"像素数"直接决定一次分配多大。
//
// 为什么需要它：图片格式允许的尺寸远超视频。30000x30000 的 PNG 是 9 亿像素，
// 转成 BGRA 要 3.6 GiB —— QImage 分配失败后 isNull() 只是安静地跳过绘制，
// 用户看到的是纯黑画面，既没有错误也没有线索（和"文件损坏"无法区分）。
// 上限取 64 Mi 像素（≈256 MiB/帧）：8K 视频（33 Mi）与 12K 以内的静图都能过，
// 再大就不是"看一眼"能用的场景，宁可明确拒绝并给出可行动的原因。
inline constexpr std::int64_t kMaxRenderImagePixels = 64ll * 1024 * 1024;

// 宽高任一非正视为"尺寸未知"，不判超限 —— 让解码器去报真正的错误，
// 别把"探测不出来"说成"太大"。
inline bool ExceedsImagePixelBudget(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    return static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height) > kMaxRenderImagePixels;
}

inline std::int64_t RenderFrameBytes(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height) * 4;
}

// 人类可读的"宽x高（N.N MiB 一帧）"，用于把拒绝原因写清楚。
inline std::string FormatPixelBudget(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    const std::int64_t bytes = RenderFrameBytes(width, height);
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::string text = std::to_string(width) + "x" + std::to_string(height);
    char tail[32] = {};
    std::snprintf(tail, sizeof(tail), "（%.1f MiB/帧）", mib);
    text += tail;
    return text;
}

} // namespace videoeye::model
