// FFmpeg 中断/超时工具（core/ffmpeg_io）单元测试（复查 P1-3）
//
// 这套机制是"取消能不能及时生效"的唯一开关：它决定 avformat_open_input /
// avformat_find_stream_info / av_read_frame 在阻塞 IO 里能否被踢出来。
// 之前它住在 core/analysis/orchestration，导出层够不着，于是导出的取消在网络输入下
// 完全失效（Cancel() 只置了个标志，线程照样卡着）。下沉成叶子模块后，
// analysis / playback / exporter 共用同一份实现 —— 这里锁定它的语义。

#include <gtest/gtest.h>

#include <atomic>

// 注意: 不要在这里裸 include <libavutil/...> —— FFmpeg 的头必须在 extern "C" 里包含，
// 否则 av_gettime 会按 C++ 名字修饰生成引用，链接期找不到定义。
// FfmpegInterrupt.h 已经把需要的东西（含 libavutil/time.h）包在 extern "C" 里了。
#include "core/ffmpeg_io/FfmpegInterrupt.h"

namespace {

using namespace videoeye;

// 没有任何中断条件时绝不能误报中断 —— 否则正常的长素材会被随机掐断。
TEST(FfmpegInterrupt, IdleStateDoesNotInterrupt) {
    ffmpeg_io::AvInterruptState state;
    EXPECT_EQ(0, ffmpeg_io::AvIoInterruptCallback(&state));
}

// 取消标志置位即中断（这是"点取消能立刻生效"的那条路径）。
TEST(FfmpegInterrupt, CancelFlagInterrupts) {
    std::atomic<bool> cancel{false};
    ffmpeg_io::AvInterruptState state;
    state.cancel = &cancel;

    EXPECT_EQ(0, ffmpeg_io::AvIoInterruptCallback(&state));
    cancel.store(true, std::memory_order_release);
    EXPECT_EQ(1, ffmpeg_io::AvIoInterruptCallback(&state));
}

// 到期即中断：没有这条，不可达的 URL 会把 UI 线程永远挂住。
TEST(FfmpegInterrupt, ExpiredDeadlineInterrupts) {
    ffmpeg_io::AvInterruptState state;
    state.deadline_us = av_gettime() - 1;   // 已经过期
    EXPECT_EQ(1, ffmpeg_io::AvIoInterruptCallback(&state));

    state.deadline_us = av_gettime() + 60'000'000;   // 还早
    EXPECT_EQ(0, ffmpeg_io::AvIoInterruptCallback(&state));
}

// 扫描阶段把 deadline 清零 → 只剩取消响应。
// 这条是"长素材的正常读取不会被打开期的早期超时误杀"的保证。
TEST(FfmpegInterrupt, ZeroedDeadlineDisablesTimeout) {
    ffmpeg_io::AvInterruptState state;
    state.deadline_us = av_gettime() + 60'000'000;
    EXPECT_EQ(0, ffmpeg_io::AvIoInterruptCallback(&state));

    state.deadline_us = 0;
    EXPECT_EQ(0, ffmpeg_io::AvIoInterruptCallback(&state));
}

// AttachInterrupt: 装上回调、按 timeout_us 折算截止时间、opaque 指向传入的 state。
TEST(FfmpegInterrupt, AttachInterruptWiresCallbackAndDeadline) {
    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_TRUE(fmt != nullptr);

    ffmpeg_io::AvInterruptState state;
    ffmpeg_io::AttachInterrupt(fmt, state, ffmpeg_io::kOpenTimeoutUs);

    EXPECT_TRUE(fmt->interrupt_callback.callback == &ffmpeg_io::AvIoInterruptCallback);
    EXPECT_TRUE(fmt->interrupt_callback.opaque == &state);
    EXPECT_GT(state.deadline_us, av_gettime());
    EXPECT_LE(state.deadline_us, av_gettime() + ffmpeg_io::kOpenTimeoutUs + 1'000'000);
    // 还没到期 -> 不中断
    EXPECT_EQ(0, ffmpeg_io::AvIoInterruptCallback(fmt->interrupt_callback.opaque));

    // timeout_us <= 0 表示"不设超时"，只保留取消响应
    ffmpeg_io::AttachInterrupt(fmt, state, 0);
    EXPECT_EQ(0, state.deadline_us);

    // 空上下文不能崩（MediaPlayer 里 avformat_alloc_context 失败时就是这条路径），
    // 且不得改动调用方的状态。
    state.deadline_us = 12345;
    ffmpeg_io::AttachInterrupt(nullptr, state, ffmpeg_io::kOpenTimeoutUs);
    EXPECT_EQ(12345, state.deadline_us);

    avformat_free_context(fmt);
}

}  // namespace
