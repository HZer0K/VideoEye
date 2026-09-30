#pragma once

namespace videoeye {
namespace model {

// 进度条拖动的定位方式
enum class SeekMode {
    // 最近关键帧: av_seek_frame(AVSEEK_FLAG_BACKWARD) 定位到目标时间之前最近的关键帧 (I帧)。
    // 速度最快，但落点通常是 GOP 起点而非精确时间。这是绝大多数播放器的默认行为。
    NearestKeyframe,

    // 精确帧: 先回退到最近关键帧，再清空解码缓冲并从关键帧向前逐帧解码，
    // 丢弃目标位置之前的帧，直到精确到达目标时间戳。定位准确，但需额外解码，速度较慢。
    ExactFrame
};

} // namespace model
} // namespace videoeye
