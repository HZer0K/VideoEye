// MediaInfoAnalyzer::FormatFromContext 回归测试（对应评审 P1：媒体信息去重）。
//
// 根因: 打开链路已经完成 avformat_open_input + find_stream_info，媒体信息页却又
// 独立打开一遍（同一个文件被读第四遍）。修复后打开链路（OpenController::Prepare）
// 与 MediaInfoAnalyzer::Open 共用同一个格式化入口。
//
// 本测试用合成的 AVFormatContext（不读真实文件）验证：
//   1) 空指针保护：返回空串而不是崩溃；
//   2) 文本骨架：General 段 + 流数量 + 每种流的段落标题；
//   3) 传入的 filePath 出现在 Complete name（说明文本确实来自复用入口的入参）。

#include <gtest/gtest.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <string>

#include "core/analysis/orchestration/MediaInfoAnalyzer.h"

namespace {

class AVFormatContextGuard {
public:
    AVFormatContextGuard() : ctx_(avformat_alloc_context()) {}
    ~AVFormatContextGuard() { avformat_close_input(&ctx_); }

    AVFormatContext* get() { return ctx_; }

private:
    AVFormatContext* ctx_ = nullptr;
};

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST(MediaInfoFormatTest, NullContextReturnsEmptyText) {
    EXPECT_TRUE(videoeye::MediaInfoAnalyzer::FormatFromContext(nullptr, "x.mp4").empty());
}

TEST(MediaInfoFormatTest, FormatsGeneralAndStreamSectionsFromContext) {
    AVFormatContextGuard format;
    ASSERT_NE(format.get(), nullptr);

    AVStream* video = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(video, nullptr);
    ASSERT_NE(video->codecpar, nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->codecpar->codec_id = AV_CODEC_ID_H264;
    video->codecpar->width = 1920;
    video->codecpar->height = 1080;
    video->avg_frame_rate = AVRational{30, 1};

    AVStream* audio = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(audio, nullptr);
    ASSERT_NE(audio->codecpar, nullptr);
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio->codecpar->codec_id = AV_CODEC_ID_AAC;
    audio->codecpar->sample_rate = 48000;

    const std::string text =
        videoeye::MediaInfoAnalyzer::FormatFromContext(format.get(), "D:/media/sample.mp4");

    ASSERT_FALSE(text.empty());
    EXPECT_TRUE(Contains(text, "General"));
    EXPECT_TRUE(Contains(text, "Complete name              : D:/media/sample.mp4"));
    EXPECT_TRUE(Contains(text, "Stream count               : 2"));
    EXPECT_TRUE(Contains(text, "Video #0"));
    EXPECT_TRUE(Contains(text, "Audio #0"));
    EXPECT_TRUE(Contains(text, "Width                      : 1920"));
    EXPECT_TRUE(Contains(text, "Height                     : 1080"));
    EXPECT_TRUE(Contains(text, "Sample rate                : 48000 Hz"));
}