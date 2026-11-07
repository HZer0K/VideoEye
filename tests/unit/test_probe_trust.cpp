// 探测可信度复核的回归守护。
//
// 这条规则存在的唯一理由：没有容器头、也没按约定命名的裸音频，会被 FFmpeg 挑一个
// 裸音频伪 demuxer 接走 —— 实测一个 44.1kHz/2ch 的裸 PCM 命名成 .bin 后由 `adp`
// 接走，codec 报 adpcm_dtk、采样率报 48kHz、**时长还被按文件大小估成 3.2s**。
// 打开成功、播放成功、进度条有值，只有声音是噪声，界面零报错。
//
// 所以这里两个方向都要钉死：
//   * 正向：.bin/.dat/无扩展名 + 裸音频伪 demuxer → 必须报可疑；
//   * 反向：所有"本来就该正常"的文件（.pcm、扩展名与 demuxer 一致、adp 自己的
//     .adp/.dtk、真正的 mp3/wav/aac 等容器、有视频轨）一律不许报警 ——
//     一个误报就会让提示失去可信度。

#include <gtest/gtest.h>

#include <string>

#include "core/player/ProbeTrust.h"

namespace {

using videoeye::player::ClassifyProbeTrust;
using videoeye::player::IsExpectedRawAudioExtension;
using videoeye::player::IsRawAudioDemuxerName;
using videoeye::player::ProbeFacts;
using videoeye::player::ProbeTrustMessage;
using videoeye::player::ProbeVerdict;
using videoeye::player::SplitDemuxerNames;

// 造一份"纯音频"的事实，再按用例覆盖个别字段。
ProbeFacts AudioOnly(const std::string& demuxer, const std::string& extension) {
    ProbeFacts facts;
    facts.demuxer_name = demuxer;
    facts.file_extension = extension;
    facts.has_audio = true;
    facts.has_video = false;
    return facts;
}

} // namespace

TEST(ProbeTrustTest, SplitsCommaSeparatedDemuxerNames) {
    const auto names = SplitDemuxerNames("matroska,webm");
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "matroska");
    EXPECT_EQ(names[1], "webm");

    EXPECT_TRUE(SplitDemuxerNames("").empty());
    EXPECT_EQ(SplitDemuxerNames("h264").size(), 1u);
}

TEST(ProbeTrustTest, RecognizesRawAudioDemuxerFamily) {
    // `adp` 是实测踩到的元凶，必须在名单里 —— 少它一条，整条规则就是死代码。
    EXPECT_TRUE(IsRawAudioDemuxerName("adp"));
    EXPECT_TRUE(IsRawAudioDemuxerName("adpcm_dtk"));
    EXPECT_TRUE(IsRawAudioDemuxerName("adpcm_ms"));
    EXPECT_TRUE(IsRawAudioDemuxerName("pcm_s16le"));
    EXPECT_TRUE(IsRawAudioDemuxerName("s16le"));
    EXPECT_TRUE(IsRawAudioDemuxerName("f32be"));
    EXPECT_TRUE(IsRawAudioDemuxerName("u8"));

    // 有容器/有魔数的 demuxer 不属于这一族，不能误伤。
    EXPECT_FALSE(IsRawAudioDemuxerName("wav"));
    EXPECT_FALSE(IsRawAudioDemuxerName("mp3"));
    EXPECT_FALSE(IsRawAudioDemuxerName("aac"));
    EXPECT_FALSE(IsRawAudioDemuxerName("mov,mp4,m4a,3gp,3g2,mj2"));
    EXPECT_FALSE(IsRawAudioDemuxerName(""));
}

// 正向：实测场景 —— 44.1kHz/2ch 裸 PCM 命名成 .bin，被 `adp` 接走并编出假时长。
// 注意这里**不提时长**：伪 demuxer 会给一个按文件大小估出来的值，靠 duration
// 判断真假是错的（第一版判据就是这么失效的）。
TEST(ProbeTrustTest, FlagsGenericExtensionGuessedAsRawAudio) {
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("adp", "bin")), ProbeVerdict::SuspectRawAudioMismatch);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("adpcm_dtk", "bin")), ProbeVerdict::SuspectRawAudioMismatch);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("s16le", "dat")), ProbeVerdict::SuspectRawAudioMismatch);
    // 无扩展名更难查：用户连"我该改名"的线索都没有。
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("adp", "")), ProbeVerdict::SuspectRawAudioMismatch);
}

// 反向：按约定命名的裸音频走专用打开路径，不该被提示打扰。
TEST(ProbeTrustTest, TrustsPcmExtension) {
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("s16le", "pcm")), ProbeVerdict::Trusted);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("adp", "pcm")), ProbeVerdict::Trusted);
}

// 反向：扩展名本身就是采样格式名（song.s16le），说明用户清楚自己在做什么。
TEST(ProbeTrustTest, TrustsExtensionMatchingDemuxerName) {
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("s16le", "s16le")), ProbeVerdict::Trusted);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("f32be", "f32be")), ProbeVerdict::Trusted);
}

// 反向：`adp` demuxer 自己的合法扩展名是 adp/dtk（见 libavformat/adp.c 的
// p.extensions）。一份真的 DTK 音频不该被劝"改名 .pcm"。
TEST(ProbeTrustTest, TrustsNativeExtensionsOfAdpDemuxer) {
    EXPECT_TRUE(IsExpectedRawAudioExtension("adp", "adp"));
    EXPECT_TRUE(IsExpectedRawAudioExtension("adp", "dtk"));
    EXPECT_FALSE(IsExpectedRawAudioExtension("adp", "bin"));
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("adp", "dtk")), ProbeVerdict::Trusted);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("adp", "adp")), ProbeVerdict::Trusted);
}

// 反向：有视频轨的文件不可能被裸音频伪 demuxer 抢走，这条规则不该插手。
TEST(ProbeTrustTest, TrustsFilesWithVideoStream) {
    ProbeFacts facts = AudioOnly("h264", "bin");
    facts.has_video = true;
    EXPECT_EQ(ClassifyProbeTrust(facts), ProbeVerdict::Trusted);
}

// 反向：真正的容器 demuxer（哪怕扩展名是 .bin）不报警 —— 魔数过关的容器
// 不该因为"文件名奇怪"就被怀疑。
TEST(ProbeTrustTest, TrustsRealContainerDemuxers) {
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("mp3", "bin")), ProbeVerdict::Trusted);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("wav", "wav")), ProbeVerdict::Trusted);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("aac", "aac")), ProbeVerdict::Trusted);
    EXPECT_EQ(ClassifyProbeTrust(AudioOnly("matroska,webm", "mka")), ProbeVerdict::Trusted);
}

// 提示必须可操作：说清"被判成了什么"，并给出 .pcm + 参数对话框这条出路。
TEST(ProbeTrustTest, MessageNamesDetectedFormatAndSuggestsFix) {
    ProbeFacts facts = AudioOnly("adp", "bin");
    facts.codec_name = "adpcm_dtk";
    const std::string message = ProbeTrustMessage(facts);
    EXPECT_NE(message.find("adp / adpcm_dtk"), std::string::npos)
        << "demuxer 与 codec 不同名时应拼成 'adp / adpcm_dtk'：" << message;
    EXPECT_NE(message.find(".pcm"), std::string::npos) << message;
    EXPECT_NE(message.find("噪声"), std::string::npos) << message;
}
