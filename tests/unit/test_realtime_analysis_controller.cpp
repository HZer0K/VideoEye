#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "core/player/RealtimeAnalysisController.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/pixfmt.h>
}

class videoeye::player::RealtimeAnalysisControllerTestAccess {
public:
    static void Feed(RealtimeAnalysisController& controller, const AVFrame* frame, double timestamp) {
        controller.FeedVisualDefectFrame(frame, timestamp, false);
    }

    static void Seek(RealtimeAnalysisController& controller, double target_ms) {
        controller.OnSeekDone(target_ms, model::SeekMode::ExactFrame);
    }

    static int SampleCount(const RealtimeAnalysisController& controller) {
        return controller.visual_defect_analyzer_.analyzed_samples();
    }

    static model::VisualDefectReport Snapshot(const RealtimeAnalysisController& controller) {
        return controller.visual_defect_analyzer_.Snapshot();
    }

    // 直接喂一个包：包批量投递是解码线程上的回调，这里绕过 PlaybackSession 只测攒批逻辑。
    static void FeedPacket(RealtimeAnalysisController& controller, const AVPacket& packet) {
        PacketContext ctx;
        ctx.packet = &packet;
        ctx.timestamp_seconds = 0.0;
        controller.OnPacket(ctx);
    }

    static void FlushOnStop(RealtimeAnalysisController& controller) {
        controller.FlushOnStop();
    }

    static void QueueMacroblock(RealtimeAnalysisController& controller, model::MacroblockFrameAnalysis analysis) {
        controller.QueueMacroblockAnalysis(std::move(analysis));
    }

    static void FlushMacroblock(RealtimeAnalysisController& controller) {
        controller.FlushPendingMacroblock();
    }

    static int PendingMacroblockFrame(const RealtimeAnalysisController& controller) {
        return controller.pending_macroblock_ ? controller.pending_macroblock_->frame_index : -1;
    }

    static void SetMacroblockEnabled(RealtimeAnalysisController& controller, bool enabled) {
        controller.analysis_.SetMacroblockAnalysisEnabled(enabled);
    }

    // 暂停补采走的就是这个方法（RefreshMacroblockFromLastFrame 从解码器取帧后调它）；
    // 这里直接喂帧，绕过"必须真有解码器"的限制。帧号由调用方给，方法内部不再自增。
    static bool AnalyzeMacroblock(RealtimeAnalysisController& controller, int frame_index, const AVFrame* frame,
                                  int frame_type, int codec_id) {
        return controller.AnalyzeMacroblockFrame(frame, frame_index, 0, 0.0, frame_type, codec_id);
    }

    static void RefreshMacroblock(RealtimeAnalysisController& controller) {
        controller.RefreshMacroblockFromLastFrame();
    }
};

namespace {

using videoeye::player::RealtimeAnalysisControllerTestAccess;

void FreeFrame(AVFrame* frame) {
    av_frame_free(&frame);
}

// 给测试帧挂一条 16x16 的前向运动矢量，让宏块分析有可统计的数据。
void AttachMotionVector(AVFrame* frame) {
    AVFrameSideData* side_data = av_frame_new_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS, sizeof(AVMotionVector));
    ASSERT_NE(side_data, nullptr);
    auto* mv = reinterpret_cast<AVMotionVector*>(side_data->data);
    *mv = AVMotionVector{};
    mv->dst_x = 8;
    mv->dst_y = 8;
    mv->w = 16;
    mv->h = 16;
    mv->source = -1;
    mv->motion_scale = 4;
    mv->motion_x = 8;
}

class RealtimeAnalysisControllerTest : public testing::Test {
protected:
    void SetUp() override {
        frame_.reset(av_frame_alloc());
        ASSERT_NE(frame_, nullptr);
        frame_->format = AV_PIX_FMT_GRAY8;
        frame_->width = 64;
        frame_->height = 36;
        ASSERT_GE(av_frame_get_buffer(frame_.get(), 32), 0);
        for (int row = 0; row < frame_->height; ++row) {
            std::fill_n(frame_->data[0] + row * frame_->linesize[0], frame_->width, 128);
        }
        videoeye::VisualDefectOptions options;
        options.sample_fps = 2.0;
        options.analysis_width = 64;
        options.capture_rgb = false;
        controller_.SetVisualDefectOptions(options);
        controller_.SetVisualDefectAnalysisEnabled(true);
    }

    void Feed(double timestamp) {
        RealtimeAnalysisControllerTestAccess::Feed(controller_, frame_.get(), timestamp);
    }

    bool WaitForSamples(int expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (RealtimeAnalysisControllerTestAccess::SampleCount(controller_) < expected) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    // 给工作线程留出"如果真收到帧就该处理完"的时间，用来断言"这一帧根本没被投递"。
    // 直接断言计数会与异步处理赛跑，测不出问题。
    void Settle() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    videoeye::player::AnalysisSession analysis_;
    videoeye::player::PlaybackSession playback_;
    videoeye::player::RealtimeAnalysisController controller_{analysis_, playback_};
    std::unique_ptr<AVFrame, decltype(&FreeFrame)> frame_{nullptr, FreeFrame};
};

TEST_F(RealtimeAnalysisControllerTest, BackwardTimestampResumesSamplingImmediately) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    Feed(10.0);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_DOUBLE_EQ(report.samples.back().timestamp_seconds, 10.0);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

TEST_F(RealtimeAnalysisControllerTest, BackwardSeekResetsSamplingClockAndFrameHistory) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    RealtimeAnalysisControllerTestAccess::Seek(controller_, 10000.0);
    Feed(10.0);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

TEST_F(RealtimeAnalysisControllerTest, ShortForwardSeekDoesNotCompareUnrelatedFrames) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    RealtimeAnalysisControllerTestAccess::Seek(controller_, 61000.0);
    Feed(61.0);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

TEST_F(RealtimeAnalysisControllerTest, SamplingIntervalStillAppliesAfterSeek) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    RealtimeAnalysisControllerTestAccess::Seek(controller_, 10000.0);
    Feed(10.0);
    ASSERT_TRUE(WaitForSamples(2));
    Feed(10.1);
    EXPECT_EQ(RealtimeAnalysisControllerTestAccess::SampleCount(controller_), 2);
    Feed(10.5);
    ASSERT_TRUE(WaitForSamples(3));
}

// 时间戳小幅倒退（小于一个采样间隔）不该被当成 seek: 这一帧本来就会被节流丢掉，
// 而重建工作线程 + 清空亮度历史会让闪烁 / 冻结这类靠历史累积的判定失效。
TEST_F(RealtimeAnalysisControllerTest, SmallBackwardJitterKeepsFrameHistory) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    Feed(59.99); // 倒退 10ms，远小于 2fps 的 0.5s 采样间隔
    Settle();
    EXPECT_EQ(RealtimeAnalysisControllerTestAccess::SampleCount(controller_), 1);
    Feed(60.5);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    // 帧历史没被清掉: 这一帧仍与 60.0s 的那帧比较（两张纯色图 → 差值≈0，而不是 NaN）
    EXPECT_FALSE(std::isnan(report.samples.back().frame_diff));
    EXPECT_LT(std::fabs(report.samples.back().frame_diff), 1e-6);
}

// 向前大跨度跳变（没有 seek 回调，比如时间戳直接跳进新分段）同样要清掉帧历史。
TEST_F(RealtimeAnalysisControllerTest, ForwardJumpResetsTimeline) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    Feed(65.0); // 跨度 5s > 2s 的跳变阈值
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_DOUBLE_EQ(report.samples.back().timestamp_seconds, 65.0);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

// 换媒体时"清空 UI 列表"无条件发：开关只管收不收新数据，不管清空。
// 否则"A 文件采集过 -> 关开关 -> 换 B -> 再开开关"会把 B 的数据接在 A 的旧行后面。
TEST_F(RealtimeAnalysisControllerTest, ResetForNewMediaClearsListsRegardlessOfFeatureSwitches) {
    int video_resets = 0;
    int audio_resets = 0;
    int packet_resets = 0;
    int event_resets = 0;
    int sync_resets = 0;
    int timeline_resets = 0;
    int visual_defect_resets = 0;

    auto& bus = controller_.bus();
    QObject::connect(&bus, &videoeye::player::AnalysisBus::VideoFrameListReset, [&] { ++video_resets; });
    QObject::connect(&bus, &videoeye::player::AnalysisBus::AudioFrameListReset, [&] { ++audio_resets; });
    QObject::connect(&bus, &videoeye::player::AnalysisBus::PacketListReset, [&] { ++packet_resets; });
    QObject::connect(&bus, &videoeye::player::AnalysisBus::AnalysisEventListReset, [&] { ++event_resets; });
    QObject::connect(&bus, &videoeye::player::AnalysisBus::SyncSampleListReset, [&] { ++sync_resets; });
    QObject::connect(&bus, &videoeye::player::AnalysisBus::TimelineEventListReset, [&] { ++timeline_resets; });
    QObject::connect(&bus, &videoeye::player::AnalysisBus::VisualDefectReset, [&] { ++visual_defect_resets; });

    // 默认只有 VisualDefectAnalysis 打开，其余五个开关均为关闭状态
    controller_.ResetForNewMedia();

    EXPECT_EQ(video_resets, 1);
    EXPECT_EQ(audio_resets, 1);
    EXPECT_EQ(packet_resets, 1);
    EXPECT_EQ(event_resets, 1);
    EXPECT_EQ(sync_resets, 1);
    EXPECT_EQ(timeline_resets, 1);
    EXPECT_EQ(visual_defect_resets, 1);
}

// 包信息按批投递：攒满 256 条立即发一批；不足一批的尾部在停止时强制 flush，
// 不能一直留在缓冲里（否则界面上最后几十个包永远不出现）。
TEST_F(RealtimeAnalysisControllerTest, PacketInfoIsBatchedAndFlushedOnStop) {
    analysis_.SetPacketAnalysisEnabled(true);

    int batches = 0;
    int singles = 0;
    std::size_t last_batch_size = 0;
    auto& bus = controller_.bus();
    QObject::connect(&bus, &videoeye::player::AnalysisBus::PacketInfoReady,
                     [&](const videoeye::model::PacketInfo&) { ++singles; });
    QObject::connect(&bus, &videoeye::player::AnalysisBus::PacketInfoBatchReady,
                     [&](const std::vector<videoeye::model::PacketInfo>& packets) {
                         ++batches;
                         last_batch_size = packets.size();
                     });

    AVPacket packet{};
    for (int i = 0; i < 256; ++i) {
        RealtimeAnalysisControllerTestAccess::FeedPacket(controller_, packet);
    }
    // 单条契约信号保留（逐包一条），批量信号在攒满 256 条时发一批
    EXPECT_EQ(256, singles);
    EXPECT_EQ(1, batches);
    EXPECT_EQ(256u, last_batch_size);

    // 尾部不足一批：停止时强制发出去
    RealtimeAnalysisControllerTestAccess::FeedPacket(controller_, packet);
    EXPECT_EQ(1, batches);
    RealtimeAnalysisControllerTestAccess::FlushOnStop(controller_);
    EXPECT_EQ(2, batches);
    EXPECT_EQ(1u, last_batch_size);
}

// 宏块分析按 10Hz 合并投递：窗口内只保留最新一帧，暂停 / 播完时补发，
// 避免 1080p 逐帧跨线程拷贝数千条运动矢量。
TEST_F(RealtimeAnalysisControllerTest, MacroblockAnalysisIsCoalescedTo10Hz) {
    std::vector<int> emitted;
    auto& bus = controller_.bus();
    QObject::connect(
        &bus, &videoeye::player::AnalysisBus::MacroblockInfoReady,
        [&](const videoeye::model::MacroblockFrameAnalysis& analysis) { emitted.push_back(analysis.frame_index); });

    auto make = [](int frame_index) {
        videoeye::model::MacroblockFrameAnalysis analysis;
        analysis.frame_index = frame_index;
        return analysis;
    };

    // 第一帧立即投递（打开分析后不应等 100ms）
    RealtimeAnalysisControllerTestAccess::QueueMacroblock(controller_, make(0));
    ASSERT_EQ(std::vector<int>({0}), emitted);

    // 窗口内的后续帧被合并，只保留最新一帧
    for (int i = 1; i <= 5; ++i) {
        RealtimeAnalysisControllerTestAccess::QueueMacroblock(controller_, make(i));
    }
    EXPECT_EQ(1u, emitted.size());
    EXPECT_EQ(5, RealtimeAnalysisControllerTestAccess::PendingMacroblockFrame(controller_));

    // 暂停 / 播完补发合并窗口里的最后一帧，而不是丢掉
    RealtimeAnalysisControllerTestAccess::FlushMacroblock(controller_);
    ASSERT_EQ(2u, emitted.size());
    EXPECT_EQ(5, emitted.back());
    EXPECT_EQ(-1, RealtimeAnalysisControllerTestAccess::PendingMacroblockFrame(controller_));
}

// 定位属于"位置相关"的边界：旧位置合并中的帧必须丢弃，且新位置第一帧立即投递。
TEST_F(RealtimeAnalysisControllerTest, SeekDiscardsPendingMacroblockAndReopensWindow) {
    std::vector<int> emitted;
    auto& bus = controller_.bus();
    QObject::connect(
        &bus, &videoeye::player::AnalysisBus::MacroblockInfoReady,
        [&](const videoeye::model::MacroblockFrameAnalysis& analysis) { emitted.push_back(analysis.frame_index); });

    auto make = [](int frame_index) {
        videoeye::model::MacroblockFrameAnalysis analysis;
        analysis.frame_index = frame_index;
        return analysis;
    };

    RealtimeAnalysisControllerTestAccess::QueueMacroblock(controller_, make(0));
    RealtimeAnalysisControllerTestAccess::QueueMacroblock(controller_, make(1));
    ASSERT_EQ(1u, emitted.size());
    ASSERT_EQ(1, RealtimeAnalysisControllerTestAccess::PendingMacroblockFrame(controller_));

    RealtimeAnalysisControllerTestAccess::Seek(controller_, 5000.0);
    EXPECT_EQ(-1, RealtimeAnalysisControllerTestAccess::PendingMacroblockFrame(controller_));

    // Seek 清空了时间窗，新位置第一帧应立即投递
    RealtimeAnalysisControllerTestAccess::QueueMacroblock(controller_, make(2));
    ASSERT_EQ(2u, emitted.size());
    EXPECT_EQ(2, emitted.back());
}

// 暂停中补采的分析帧必须立刻投递（不能压在 10Hz 合并窗口里等下一帧）。
// 同时验证开关关闭时不分析、不投递。
TEST_F(RealtimeAnalysisControllerTest, MacroblockFrameAnalysisRespectsSwitchAndEmitsImmediately) {
    int emitted = 0;
    videoeye::model::MacroblockFrameAnalysis last;
    auto& bus = controller_.bus();
    QObject::connect(&bus, &videoeye::player::AnalysisBus::MacroblockInfoReady,
                     [&](const videoeye::model::MacroblockFrameAnalysis& analysis) {
                         ++emitted;
                         last = analysis;
                     });

    // 开关关闭：直接返回 false，不发任何结果
    EXPECT_FALSE(
        RealtimeAnalysisControllerTestAccess::AnalyzeMacroblock(controller_, 7, frame_.get(), AV_PICTURE_TYPE_P, 27));
    EXPECT_EQ(0, emitted);

    // 开关打开：首帧窗口为空，应立即投递。帧号原样透传（与帧表共用一套编号，不再内部自增）。
    RealtimeAnalysisControllerTestAccess::SetMacroblockEnabled(controller_, true);
    AttachMotionVector(frame_.get());
    EXPECT_TRUE(
        RealtimeAnalysisControllerTestAccess::AnalyzeMacroblock(controller_, 7, frame_.get(), AV_PICTURE_TYPE_P, 27));
    ASSERT_EQ(1, emitted);
    EXPECT_EQ(7, last.frame_index);
    EXPECT_EQ(1, last.stats.distinct_block_count);
    EXPECT_TRUE(last.stats.intra_count_available);
}

// 没有打开媒体 / 没有解码器时，补采请求是安全的 no-op：不崩、不发空结果。
TEST_F(RealtimeAnalysisControllerTest, RefreshMacroblockWithoutDecoderIsSafeNoOp) {
    RealtimeAnalysisControllerTestAccess::SetMacroblockEnabled(controller_, true);
    int emitted = 0;
    auto& bus = controller_.bus();
    QObject::connect(&bus, &videoeye::player::AnalysisBus::MacroblockInfoReady,
                     [&](const videoeye::model::MacroblockFrameAnalysis&) { ++emitted; });

    RealtimeAnalysisControllerTestAccess::RefreshMacroblock(controller_);
    EXPECT_EQ(0, emitted);
}

} // namespace
