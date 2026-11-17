// 真实 HLS / DASH fMP4 包的探测层回归。
//
// 为什么需要它：合成清单（手写 m3u8/mpd + 空字节分片，见 test_streaming_qc_rules）
// 能测"清单解析对不对"，但分片本体是假的，于是 fMP4 分片探测层 ——
// SegmentQcAnalyzer::ProbeFmp4Segment 读 tfdt 起点时间、读 trun/tfhd 的 sample_flags
// 实证关键帧 —— 在单测里**从来没被真分片喂过**。这条路径恰恰是"某码率层切档后
// 首帧花屏"这类问题的唯一取证口（详见 SegmentInfo.h 的 KeyframeEvidence 注释）。
//
// 这些语料是 ffmpeg 真编出来的小包（tests/corpus/streaming/real_*），把探测层钉死：
//   * fMP4（real_{hls,dash}_fmp4）：分片识别成 FMP4、tfdt 起点跨片连续、关键帧 SyncFlag；
//   * TS（real_hls_ts / real_hls_ts_misaligned）：分片识别成 MPEG_TS、段首 VCL NAL 实证
//     关键帧 —— 对齐包全 SyncFlag，mid-GOP 切档负样本的 seg1 必须是 NotSync 并报 Error。
// 两者都证明探测层读到的是**实证**（SyncFlag/NotSync）而不是按约定假定的 Declared。
//
// 依赖边界：纯 C++（Hls/Dash/SegmentQc 分析器都不碰 Qt / FFmpeg），只用 std::filesystem
// 拼语料路径。缺语料时 SKIP —— 没跑过 generate_real_corpus.py 的机器不该红。

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"
#include "core/analysis/streaming/SegmentQcAnalyzer.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/StreamPackageInfo.h"

namespace fs = std::filesystem;

using videoeye::DashManifestAnalyzer;
using videoeye::HlsManifestAnalyzer;
using videoeye::SegmentQcAnalyzer;
using videoeye::SegmentQcOptions;
using videoeye::model::IssueSeverity;
using videoeye::model::KeyframeEvidence;
using videoeye::model::SegmentContainer;
using videoeye::model::SegmentInfo;
using videoeye::model::StreamingIssue;
using videoeye::model::StreamingPackageResult;
// 问题码是 namespace 而非类型，用别名（using-declaration 不能指名命名空间）
namespace StreamingIssueCode = videoeye::model::StreamingIssueCode;

namespace {

const fs::path kStreamingCorpus = VIDEOEYE_STREAMING_CORPUS_DIR;

fs::path Manifest(const char* pkg, const char* manifest) {
    return kStreamingCorpus / pkg / manifest;
}

// 一条轨道（HLS 的一个 media playlist / DASH 的一个 representation）的分片，
// 逐片校验"探测层真的在真分片上跑出了实证结果"。
void ExpectRealFmp4Probe(const SegmentInfo& s, const char* track) {
    EXPECT_TRUE(s.exists) << track << ": 分片不在磁盘上（resolved_path=" << s.resolved_path << "）";
    EXPECT_GT(s.file_size_bytes, 0) << track;
    EXPECT_TRUE(s.probed) << track << ": 没探测过";
    EXPECT_EQ(SegmentContainer::FMP4, s.container) << track << ": 未识别成 fMP4/CMAF";
    EXPECT_FALSE(s.container_parse_failed) << track << ": " << s.container_error;
    // tfdt 起点时间：合成清单的空分片永远拿不到，只有真 fMP4 才有
    EXPECT_TRUE(s.has_decode_time) << track << ": 缺 tfdt 起点时间";
    EXPECT_GT(s.timescale, 0u) << track;
    // 段内实测时长：跨分片连续性实证的输入
    EXPECT_TRUE(s.has_media_duration) << track << ": 缺段内实测时长";
    EXPECT_GT(s.media_duration_seconds, 0.0) << track;
}

// 视频分片额外要求：关键帧是**实证**出来的（SyncFlag），不是按约定假定的（Declared）。
// 这是本测试最核心的一条 —— 它证明 ProbeFmp4Segment 真读到了 sample_flags。
void ExpectVerifiedKeyframe(const SegmentInfo& s, const char* track) {
    EXPECT_TRUE(s.starts_with_keyframe) << track << ": 视频分片首帧不是随机访问点";
    EXPECT_EQ(KeyframeEvidence::SyncFlag, s.keyframe_evidence)
        << track << ": 关键帧证据不是 SyncFlag（实证），探测层没读到 sample_flags";
}

// 一条轨道内分片的 tfdt 时间轴必须连续：上一段起点 + 实测时长 ≈ 本段起点。
// 清单声明的 EXTINF/duration 可能被篡改，这条用分片里实测的 tfdt 自证。
void ExpectContinuousTimeline(const std::vector<SegmentInfo>& segments, const char* track) {
    const SegmentInfo* prev = nullptr;
    for (const SegmentInfo& s : segments) {
        if (s.partial)
            continue;
        ASSERT_TRUE(s.probed) << track;
        ASSERT_TRUE(s.has_decode_time) << track << ": 缺 tfdt，无法判连续性";
        if (prev != nullptr) {
            EXPECT_GT(s.decode_time, prev->decode_time) << track << ": tfdt 未严格递增";
            if (prev->has_media_duration) {
                const double expected_start = prev->start_seconds + prev->media_duration_seconds;
                EXPECT_NEAR(expected_start, s.start_seconds, 0.05) << track << ": 时间轴断裂";
            }
        }
        prev = &s;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// HLS fMP4（CMAF）单码率包：media.m3u8 + init.mp4 + seg0/1.m4s（音视频混封）
// ---------------------------------------------------------------------------

TEST(StreamingRealPackage, HlsFmp4ProbesRealSegments) {
    const fs::path manifest = Manifest("real_hls_fmp4", "media.m3u8");
    if (!fs::exists(manifest))
        GTEST_SKIP() << "缺真实包 real_hls_fmp4，先跑 tests/corpus/generate_real_corpus.py";

    StreamingPackageResult pkg;
    ASSERT_TRUE(HlsManifestAnalyzer{}.AnalyzeFile(manifest.string(), pkg)) << pkg.error_message;
    ASSERT_TRUE(pkg.valid) << pkg.error_message;
    EXPECT_TRUE(pkg.IsHls());
    EXPECT_FALSE(pkg.truncated);
    ASSERT_EQ(1u, pkg.playlists.size()) << "单码率 HLS 应只有一个 media playlist";

    ASSERT_EQ(SegmentQcAnalyzer::StageStatus::kDone, SegmentQcAnalyzer::Analyze(pkg, SegmentQcOptions{}));

    // 包级：分片全在磁盘、全探测过（小包，max_probe_segments 默认 8 够用）
    EXPECT_GE(pkg.TotalSegments(), 2u);
    EXPECT_EQ(pkg.TotalSegments(), pkg.ProbedSegments()) << "探测未覆盖全部分片";
    EXPECT_TRUE(pkg.SegmentProbeComplete());
    EXPECT_GE(pkg.ladder.size(), 1u) << "Analyze 应建出 ladder";

    const std::vector<SegmentInfo>& segs = pkg.playlists[0].segments;
    ASSERT_GE(segs.size(), 2u);
    for (const SegmentInfo& s : segs) {
        ExpectRealFmp4Probe(s, "hls");
        ExpectVerifiedKeyframe(s, "hls"); // 混封分片含视频，首帧必须是实证的随机访问点
    }
    ExpectContinuousTimeline(segs, "hls");
}

// ---------------------------------------------------------------------------
// DASH fMP4 包：manifest.mpd + 视频/音频两路（各带 init + 分片，分离封装）
// ---------------------------------------------------------------------------

TEST(StreamingRealPackage, DashFmp4ProbesRealSegments) {
    const fs::path manifest = Manifest("real_dash_fmp4", "manifest.mpd");
    if (!fs::exists(manifest))
        GTEST_SKIP() << "缺真实包 real_dash_fmp4，先跑 tests/corpus/generate_real_corpus.py";

    StreamingPackageResult pkg;
    ASSERT_TRUE(DashManifestAnalyzer{}.AnalyzeFile(manifest.string(), pkg)) << pkg.error_message;
    ASSERT_TRUE(pkg.valid) << pkg.error_message;
    EXPECT_TRUE(pkg.IsDash());
    EXPECT_FALSE(pkg.truncated);

    ASSERT_EQ(SegmentQcAnalyzer::StageStatus::kDone, SegmentQcAnalyzer::Analyze(pkg, SegmentQcOptions{}));

    EXPECT_GE(pkg.TotalSegments(), 2u);
    EXPECT_EQ(pkg.TotalSegments(), pkg.ProbedSegments()) << "探测未覆盖全部分片";
    EXPECT_TRUE(pkg.SegmentProbeComplete());

    // 视频 + 音频两路 Representation
    ASSERT_EQ(2u, pkg.representations.size()) << "应含视频/音频两个 Representation";
    // ladder 是**视频码率阶梯**：BuildLadder 的 DASH 分支只收 content_type=="video"
    // 的档（音频不是阶梯上的一档）。本包单视频路 → 恰好 1 档。
    EXPECT_EQ(1u, pkg.ladder.size());

    const videoeye::model::DashRepresentationInfo* video = nullptr;
    for (const auto& r : pkg.representations) {
        if (r.content_type == "video" || r.width > 0)
            video = &r;
    }
    ASSERT_NE(nullptr, video) << "找不到视频 Representation";
    ASSERT_GE(video->segments.size(), 2u);

    // 视频路：探测实证 + 关键帧 SyncFlag + tfdt 连续
    for (const SegmentInfo& s : video->segments) {
        ExpectRealFmp4Probe(s, "dash-video");
        ExpectVerifiedKeyframe(s, "dash-video");
    }
    ExpectContinuousTimeline(video->segments, "dash-video");

    // 音频路：关键帧是视频概念，这里只要求探测真跑过（分片存在、识别成 fMP4、有 tfdt）
    bool saw_audio = false;
    for (const auto& r : pkg.representations) {
        if (&r == video)
            continue; // 恰好两路，非视频那一路即音频
        saw_audio = true;
        for (const SegmentInfo& s : r.segments) {
            if (s.partial)
                continue;
            EXPECT_TRUE(s.probed) << "dash-audio";
            EXPECT_EQ(SegmentContainer::FMP4, s.container) << "dash-audio";
            EXPECT_TRUE(s.exists) << "dash-audio";
        }
    }
    EXPECT_TRUE(saw_audio) << "找不到音频 Representation";
}

// ---------------------------------------------------------------------------
// HLS MPEG-TS 单码率包（关键帧对齐）：media.m3u8 + seg0/1.ts
// ---------------------------------------------------------------------------
//
// TS 分片的首帧实证走 TsStructureAnalyzer（逐包扫到首个视频 VCL NAL），与 fMP4 的
// sample_flags 是两条取证口。以前 TS 关键帧只有清单惯例给的 Declared，"切档后首帧花屏"
// 永远抓不到；这里钉死：真 TS 分片必须被识别成 MPEG_TS、解析成功、段首 IDR 实证为 SyncFlag。

TEST(StreamingRealPackage, HlsTsProbesRealSegments) {
    const fs::path manifest = Manifest("real_hls_ts", "media.m3u8");
    if (!fs::exists(manifest))
        GTEST_SKIP() << "缺真实包 real_hls_ts，先跑 tests/corpus/generate_real_corpus.py";

    StreamingPackageResult pkg;
    ASSERT_TRUE(HlsManifestAnalyzer{}.AnalyzeFile(manifest.string(), pkg)) << pkg.error_message;
    ASSERT_TRUE(pkg.valid) << pkg.error_message;
    ASSERT_EQ(1u, pkg.playlists.size()) << "单码率 HLS 应只有一个 media playlist";

    ASSERT_EQ(SegmentQcAnalyzer::StageStatus::kDone, SegmentQcAnalyzer::Analyze(pkg, SegmentQcOptions{}));
    EXPECT_TRUE(pkg.SegmentProbeComplete());

    const std::vector<SegmentInfo>& segs = pkg.playlists[0].segments;
    ASSERT_GE(segs.size(), 2u);
    for (const SegmentInfo& s : segs) {
        EXPECT_TRUE(s.exists) << s.resolved_path;
        EXPECT_TRUE(s.probed);
        EXPECT_EQ(SegmentContainer::MPEG_TS, s.container) << "未识别成 MPEG-TS";
        EXPECT_FALSE(s.container_parse_failed) << s.container_error;
        EXPECT_TRUE(s.starts_with_keyframe);
        EXPECT_EQ(KeyframeEvidence::SyncFlag, s.keyframe_evidence)
            << "TS 段首 IDR 应实证为 SyncFlag，而不是沿用清单约定的 Declared";
    }
    EXPECT_FALSE(pkg.HasIssue(StreamingIssueCode::kSegmentStartsNonSync));
}

// ---------------------------------------------------------------------------
// HLS MPEG-TS 单码率包（mid-GOP 切档负样本）：seg0 从 IDR 起头，seg1 被 split_by_time
// 切在 GOP 中间（段首是 P 帧）。这正是"某码率层切档后首帧花屏"的实物。
// ---------------------------------------------------------------------------

TEST(StreamingRealPackage, HlsTsMisalignedDetectsNonSyncFirstFrame) {
    const fs::path manifest = Manifest("real_hls_ts_misaligned", "media.m3u8");
    if (!fs::exists(manifest))
        GTEST_SKIP() << "缺真实包 real_hls_ts_misaligned，先跑 tests/corpus/generate_real_corpus.py";

    StreamingPackageResult pkg;
    ASSERT_TRUE(HlsManifestAnalyzer{}.AnalyzeFile(manifest.string(), pkg)) << pkg.error_message;
    ASSERT_TRUE(pkg.valid) << pkg.error_message;
    ASSERT_EQ(1u, pkg.playlists.size());

    ASSERT_EQ(SegmentQcAnalyzer::StageStatus::kDone, SegmentQcAnalyzer::Analyze(pkg, SegmentQcOptions{}));

    const std::vector<SegmentInfo>& segs = pkg.playlists[0].segments;
    ASSERT_GE(segs.size(), 2u);
    for (const SegmentInfo& s : segs) {
        EXPECT_EQ(SegmentContainer::MPEG_TS, s.container);
        EXPECT_FALSE(s.container_parse_failed) << s.container_error;
    }
    // seg0 从 IDR 起头 → SyncFlag；seg1 段首是 P 帧 → NotSync（实证，不是约定）
    EXPECT_EQ(KeyframeEvidence::SyncFlag, segs[0].keyframe_evidence);
    EXPECT_TRUE(segs[0].starts_with_keyframe);
    EXPECT_EQ(KeyframeEvidence::NotSync, segs[1].keyframe_evidence)
        << "mid-GOP 切档的 seg1 段首是 P 帧，必须实证为 NotSync";
    EXPECT_FALSE(segs[1].starts_with_keyframe);

    // 取证落地：必须报 Error 级 kSegmentStartsNonSync，并指出坏分片
    ASSERT_TRUE(pkg.HasIssue(StreamingIssueCode::kSegmentStartsNonSync));
    bool found = false;
    for (const StreamingIssue& issue : pkg.issues) {
        if (issue.code != StreamingIssueCode::kSegmentStartsNonSync)
            continue;
        found = true;
        EXPECT_EQ(IssueSeverity::Error, issue.severity);
        EXPECT_GE(issue.occurrence_count, 1);
        EXPECT_NE(issue.detail.find(segs[1].uri), std::string::npos) << "应指出第一个坏分片";
    }
    EXPECT_TRUE(found);
}
