#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// FFmpeg 的缩放/色彩转换上下文（仅实现文件里用到，头里保持不透明 —— 与 S16Converter 同一手法）
struct SwsContext;

namespace videoeye {
namespace player {

// 裸数据（无容器，.yuv/.nv12/.rgb 等）的像素排布。
//
// 枚举顺序 **不是** 界面下拉框的顺序（见 RawPixelFormatOptions）：4:2:0 的四种排布
// 排在一起，方便对着"同一份 YUV 换排布"读。
enum class RawPixelFormat {
    I420,        // Y 平面 + U 平面 + V 平面
    NV12,        // Y 平面 + UV 交织
    NV21,        // Y 平面 + VU 交织
    YV12,        // Y 平面 + V 平面 + U 平面（与 I420 只差两个色度平面的先后）
    P010,        // 10bit：Y 平面 + UV 交织（16bit 样本，10 位有效数据在高位）
    YUV420P10LE, // 10bit：Y 平面 + U 平面 + V 平面
    YUY2,        // 打包 4:2:2：Y0 U Y1 V
    UYVY,        // 打包 4:2:2：U Y0 V Y1
    GRAY8,       // 单平面灰度
    RGB24,       // 打包 8bit RGB
    BGR24,       // 打包 8bit BGR
};

// 一种可选格式：界面显示名 + 是不是需要颜色矩阵的 YUV。
struct RawPixelFormatOption {
    RawPixelFormat format;
    const char* label; // 弹窗显示名，也是 ParseRawPixelFormatLabel 的键
    bool yuv;          // true 表示按视频惯例（limited range + 矩阵）换算；RGB/GRAY 是全范围
};

// 支持的格式表 —— "下拉框显示什么 / 按什么排布解析 / 用哪个 FFmpeg 像素格式"的唯一事实源。
// 顺序即下拉框顺序（历史上就是这个顺序，别随手排）。
const std::vector<RawPixelFormatOption>& RawPixelFormatOptions();

// 显示名 → 枚举。找不到返回 false（名字是对外契约，不要在这里做模糊匹配）。
bool ParseRawPixelFormatLabel(const std::string& label, RawPixelFormat& out);

// 单帧字节数。返回 0 表示这个格式不支持该尺寸（4:2:0 要偶数宽高、4:2:2 要偶数宽），
// reason 非空时写入可直接展示的原因。
std::size_t RawFrameSizeBytes(int width, int height, RawPixelFormat format, std::string* reason);

// 裸帧 → RGB888 的转换器。
//
// 为什么要有它：RawImageSequence 原先在界面层逐像素手写 BT.601/709 系数（每个像素
// 2 次浮点乘法 + 3 次 clamp，1080p 一帧 200 万次），并且把 Y 当**全范围**处理 ——
// 16/235 该压到 0/255 的地方没压。同一份逻辑换 sws_scale 之后：SIMD 完成、范围与
// 矩阵交给库（也就是 `ffmpeg -i in.yuv -f rawvideo out.rgb` 的口径），而且 SwsContext
// 可以跨帧复用，不再每帧重建。
//
// 单帧内部状态（SwsContext 有缓存）不是线程安全的：一个序列一个转换器，只在 UI 线程用。
class RawFrameConverter {
public:
    // bt709=false 时用 BT.601。裸流没有容器级色彩标注，调用方按分辨率决定
    // （>=720 行按惯例视为 HD → BT.709），与转换器无关。
    RawFrameConverter(int width, int height, RawPixelFormat format, bool bt709);
    ~RawFrameConverter();

    RawFrameConverter(const RawFrameConverter&) = delete;
    RawFrameConverter& operator=(const RawFrameConverter&) = delete;

    // sws 上下文建不起来（尺寸非法/像素格式不支持）时为 false，此时 Convert 一律失败。
    bool Valid() const {
        return sws_ != nullptr;
    }

    int Width() const {
        return width_;
    }
    int Height() const {
        return height_;
    }

    // 输入一帧应有的字节数（与 RawFrameSizeBytes 同口径；尺寸非法时为 0）。
    std::size_t FrameBytes() const {
        return frame_bytes_;
    }

    // 输出行步长：width*3 向上对齐到 4 字节（QImage 的 Format_RGB888 用外部缓冲时要求
    // 行首 4 字节对齐；同时给 sws 一个整齐的步长）。
    std::size_t Stride() const {
        return stride_;
    }
    // 可见输出字节数 = stride * height（不含尾部余量，见 RgbData 的说明）。
    std::size_t OutputBytes() const {
        return stride_ * static_cast<std::size_t>(height_);
    }

    // 转换一帧到内部 RGB 缓冲。
    //
    // ❗ 结果留在转换器自己的缓冲里，由 RgbData()/Stride() 取用；**下一次 Convert 会覆盖它**。
    // 需要长期持有就得自己拷一份。
    //
    // 为什么不让调用方传目标缓冲：swscale 在窄帧上会**多写一整行**（实测 8x8 多写 24 字节、
    // 2x2 多写 42 字节，宽 >= 16 则不多写），把这种尾部余量要求甩给调用方，等于让每个
    // 调用点都可能踩一次堆越界 —— QImage 的缓冲根本没法留余量。所以缓冲由本类自己管，
    // 尾部预留 stride + 64 字节（与 FFmpeg 的输入 padding 同量级）。
    bool Convert(const std::uint8_t* src, std::size_t src_size);

    // 内部缓冲视图（生命周期 = 本对象；内容在下次 Convert 时变化）。
    std::uint8_t* RgbData();
    const std::uint8_t* RgbData() const;

private:
    SwsContext* sws_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    RawPixelFormat format_ = RawPixelFormat::I420;
    std::size_t frame_bytes_ = 0;
    std::size_t stride_ = 0;
    std::vector<std::uint8_t> rgb_;
};

} // namespace player
} // namespace videoeye
