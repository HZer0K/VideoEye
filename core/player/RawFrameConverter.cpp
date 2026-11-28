#include "core/player/RawFrameConverter.h"

// 仓库惯例：FFmpeg 公共头一律包在 extern "C" 里（它们自身不再带 extern "C"，
// 不包就会生成 C++ 修饰名，链接期全是"无法解析的外部符号 sws_*"）。
extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace videoeye {
namespace player {

namespace {

// 界面下拉框顺序（历史上的顺序，用户已经形成肌肉记忆，别重排）。
const std::vector<RawPixelFormatOption>& BuildOptionTable() {
    static const std::vector<RawPixelFormatOption> kOptions = {
        {RawPixelFormat::I420, "YUV420P(I420)", true}, {RawPixelFormat::NV12, "NV12", true},
        {RawPixelFormat::NV21, "NV21", true},          {RawPixelFormat::YV12, "YV12", true},
        {RawPixelFormat::P010, "P010", true},          {RawPixelFormat::YUV420P10LE, "YUV420P10LE", true},
        {RawPixelFormat::YUY2, "YUY2", true},          {RawPixelFormat::UYVY, "UYVY", true},
        {RawPixelFormat::GRAY8, "GRAY8", false},       {RawPixelFormat::RGB24, "RGB24", false},
        {RawPixelFormat::BGR24, "BGR24", false},
    };
    return kOptions;
}

AVPixelFormat ToAvPixelFormat(RawPixelFormat format) {
    switch (format) {
    case RawPixelFormat::I420:
    case RawPixelFormat::YV12:
        // 两者在 FFmpeg 里是同一个像素格式（Y,U,V 平面序），差别只在文件的平面先后，
        // 由 Convert 里换指针顺序解决 —— 数据本身不用搬。
        return AV_PIX_FMT_YUV420P;
    case RawPixelFormat::NV12:
        return AV_PIX_FMT_NV12;
    case RawPixelFormat::NV21:
        return AV_PIX_FMT_NV21;
    case RawPixelFormat::P010:
        return AV_PIX_FMT_P010LE;
    case RawPixelFormat::YUV420P10LE:
        return AV_PIX_FMT_YUV420P10LE;
    case RawPixelFormat::YUY2:
        return AV_PIX_FMT_YUYV422;
    case RawPixelFormat::UYVY:
        return AV_PIX_FMT_UYVY422;
    case RawPixelFormat::GRAY8:
        return AV_PIX_FMT_GRAY8;
    case RawPixelFormat::RGB24:
        return AV_PIX_FMT_RGB24;
    case RawPixelFormat::BGR24:
        return AV_PIX_FMT_BGR24;
    }
    return AV_PIX_FMT_NONE;
}

bool FormatNeedsMatrix(RawPixelFormat format) {
    for (const RawPixelFormatOption& option : BuildOptionTable()) {
        if (option.format == format) {
            return option.yuv;
        }
    }
    return false;
}

} // namespace

const std::vector<RawPixelFormatOption>& RawPixelFormatOptions() {
    return BuildOptionTable();
}

bool ParseRawPixelFormatLabel(const std::string& label, RawPixelFormat& out) {
    for (const RawPixelFormatOption& option : BuildOptionTable()) {
        if (label == option.label) {
            out = option.format;
            return true;
        }
    }
    return false;
}

std::size_t RawFrameSizeBytes(int width, int height, RawPixelFormat format, std::string* reason) {
    auto reject = [reason](const char* why) -> std::size_t {
        if (reason != nullptr) {
            *reason = why;
        }
        return 0;
    };

    if (width <= 0 || height <= 0) {
        return reject("宽度和高度都必须大于 0。");
    }

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);

    switch (format) {
    case RawPixelFormat::RGB24:
    case RawPixelFormat::BGR24:
        return w * h * 3;
    case RawPixelFormat::GRAY8:
        return w * h;
    case RawPixelFormat::I420:
    case RawPixelFormat::NV12:
    case RawPixelFormat::NV21:
    case RawPixelFormat::YV12:
        if ((w % 2) != 0 || (h % 2) != 0) {
            return reject("4:2:0 格式仅支持偶数宽高。");
        }
        return w * h * 3 / 2;
    case RawPixelFormat::P010:
    case RawPixelFormat::YUV420P10LE:
        if ((w % 2) != 0 || (h % 2) != 0) {
            return reject("4:2:0 10bit 格式仅支持偶数宽高。");
        }
        // 每个样本 2 字节：Y(w*h) + U/V 各(w*h/4) → 3*w*h 字节
        return w * h * 3;
    case RawPixelFormat::YUY2:
    case RawPixelFormat::UYVY:
        if ((w % 2) != 0) {
            return reject("打包 4:2:2 格式仅支持偶数宽度。");
        }
        return w * h * 2;
    }

    return reject("不支持的像素格式。");
}

RawFrameConverter::RawFrameConverter(int width, int height, RawPixelFormat format, bool bt709)
    : width_(width), height_(height), format_(format) {
    frame_bytes_ = RawFrameSizeBytes(width, height, format, nullptr);
    const AVPixelFormat src_format = ToAvPixelFormat(format);
    if (frame_bytes_ == 0 || src_format == AV_PIX_FMT_NONE) {
        return;
    }

    // 同尺寸只做色彩转换（不缩放），拼装/滤波标志在这里没有实际作用。
    sws_ = sws_getContext(width, height, src_format, width, height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr,
                          nullptr);
    if (sws_ == nullptr) {
        return;
    }

    // 显式设置范围与矩阵，不依赖 sws 内部的猜测（不同版本对"YUV420P 默认 srcRange"
    // 的处理有过差异）：
    //   * 范围：YUV 按视频惯例取 limited（16-235 / 64-940），RGB/GRAY 是全范围。
    //     旧实现把 Y 当全范围（Y=16 直接画成 16），黑电平因此发灰。
    //   * 矩阵：裸流没有容器色彩标注，按调用方给的分辨率惯例选 BT.601/709。
    // 设不上不是致命错误（某些纯 RGB 组合的上下文字段只读），继续用默认参数即可。
    const int* table = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    const int src_range = FormatNeedsMatrix(format) ? 0 : 1; // 0 = limited, 1 = full
    sws_setColorspaceDetails(sws_, table, src_range, table, 1, 0, 1 << 16, 1 << 16);

    // 行步长对齐到 4 字节（QImage 外部缓冲的硬要求），并在尾部预留 stride + 64 字节：
    // swscale 在窄帧上会多写一整行（实测 8x8 多写 24 字节、2x2 多写 42 字节），
    // 没有余量就是堆越界 —— 症状是"跑完不报错，销毁时随机崩"。
    stride_ = (static_cast<std::size_t>(width_) * 3 + 3) / 4 * 4;
    rgb_.assign(OutputBytes() + stride_ + 64, 0);
}

RawFrameConverter::~RawFrameConverter() {
    if (sws_ != nullptr) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
}

bool RawFrameConverter::Convert(const std::uint8_t* src, std::size_t src_size) {
    if (sws_ == nullptr || src == nullptr || rgb_.empty()) {
        return false;
    }
    if (src_size < frame_bytes_) {
        return false;
    }

    const std::size_t w = static_cast<std::size_t>(width_);
    const std::size_t h = static_cast<std::size_t>(height_);
    const std::size_t y_size = w * h;
    const std::size_t chroma_w = w / 2;
    const std::size_t chroma_size = chroma_w * (h / 2);

    // 按文件的平面顺序填指针。步长单位是字节；4:2:2 打包与单平面格式只有 data[0]。
    const std::uint8_t* planes[4] = {src, nullptr, nullptr, nullptr};
    int strides[4] = {static_cast<int>(w), 0, 0, 0};
    switch (format_) {
    case RawPixelFormat::I420:
        planes[1] = src + y_size;
        planes[2] = src + y_size + chroma_size;
        strides[1] = static_cast<int>(chroma_w);
        strides[2] = static_cast<int>(chroma_w);
        break;
    case RawPixelFormat::YV12:
        // 文件是 Y,V,U；sws 认 Y,U,V —— 只换指针顺序，不搬数据。
        planes[1] = src + y_size + chroma_size;
        planes[2] = src + y_size;
        strides[1] = static_cast<int>(chroma_w);
        strides[2] = static_cast<int>(chroma_w);
        break;
    case RawPixelFormat::NV12:
    case RawPixelFormat::NV21:
        // NV21 的 VU 顺序由 AV_PIX_FMT_NV21 自己解释。
        planes[1] = src + y_size;
        strides[1] = static_cast<int>(w);
        break;
    case RawPixelFormat::P010:
        planes[1] = src + y_size * 2;
        strides[0] = static_cast<int>(w * 2);
        strides[1] = static_cast<int>(w * 2);
        break;
    case RawPixelFormat::YUV420P10LE:
        planes[1] = src + y_size * 2;
        planes[2] = src + y_size * 2 + chroma_size * 2;
        strides[0] = static_cast<int>(w * 2);
        strides[1] = static_cast<int>(chroma_w * 2);
        strides[2] = static_cast<int>(chroma_w * 2);
        break;
    case RawPixelFormat::YUY2:
    case RawPixelFormat::UYVY:
        strides[0] = static_cast<int>(w * 2);
        break;
    case RawPixelFormat::GRAY8:
        strides[0] = static_cast<int>(w);
        break;
    case RawPixelFormat::RGB24:
    case RawPixelFormat::BGR24:
        strides[0] = static_cast<int>(w * 3);
        break;
    }

    std::uint8_t* dst_planes[4] = {rgb_.data(), nullptr, nullptr, nullptr};
    int dst_strides[4] = {static_cast<int>(stride_), 0, 0, 0};
    const int written = sws_scale(sws_, planes, strides, 0, height_, dst_planes, dst_strides);
    return written == height_;
}

std::uint8_t* RawFrameConverter::RgbData() {
    return rgb_.data();
}

const std::uint8_t* RawFrameConverter::RgbData() const {
    return rgb_.data();
}

} // namespace player
} // namespace videoeye
