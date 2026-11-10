#include <gtest/gtest.h>

// ============================================================
// 修复模块安全 / 结论可信度回归（R01-R13）
//
// 覆盖：
//   * R01 .part 源文件与产物别名、报告路径冲突、参考文件冲突、已有临时文件；
//   * R02 恶意 stsz.sample_count 不再申请全量 DTS 数组，声明数与文件容量交叉校验；
//   * R03 参考段时基 / 码流冲突被拒绝，缺证据时标 unverified；
//   * R06 发布阶段收到取消不发布；R09 V0 不冒充可播放；R10 预算覆盖诊断阶段；
//   * R07 逐轨守恒把 951/1000 判成 loss（旧的 5% 全轨容差会放过）；
//   * R08 同大小 + mtime 被复位的内容改写被内容摘要抓到；
//   * R12 后台引擎抛异常时控制器仍进入终态；
//   * R13 JSON / TXT / HTML 都展示同一批残留问题。
//
// 每个用例都对着"修复前的具体错误行为"写断言，避免"做了才有用、坏了看不出来"。
// ============================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/media/container/IsobmffParser.h"
#include "core/qt/RepairController.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairCapabilityContracts.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairFragmentForensics.h"
#include "core/repair/RepairInitSegment.h"
#include "core/repair/RepairInitSegmentBuilder.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairPlanner.h"
#include "core/repair/RepairSampleBounds.h"
#include "core/repair/RepairUtils.h"
#include "core/repair/RepairVerifier.h"
#include "core/reporting/RepairReportExporter.h"
#include "tests/support/TestMediaFactory.h"

namespace {

using namespace videoeye;
using namespace videoeye::model;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name =
        "videoeye_repair_safety_" + tag + "_" + std::to_string(stamp) + "_" + std::to_string(counter++);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(dir);
    return dir.string();
}

void WriteBytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    return request;
}

// 恶意 stsz：声明 UINT32_MAX 个样本却只给固定大小和极短的文件。
IsobmffTrack MakeTrackWithHugeDeclaration() {
    IsobmffTrack track;
    track.track_id = 1;
    track.handler = "vide";
    track.media_timescale = 1000;
    track.has_stsz = true;
    track.stsz.sample_count = UINT32_MAX;
    track.stsz.default_size = 1;
    track.has_stco = true;
    track.chunk_offsets = {8};
    track.has_stts = true;
    track.stts = {{UINT32_MAX, 1}};
    track.has_stsc = true;
    track.stsc = {{1, 4096, 1}};
    return track;
}

// 保持 MP4 盒结构与 moov 完好，只把 mdat 中段一部分载荷打成 0xFF。
// 目的是造一份「容器能打开、但码流已损坏」的真实媒体，而不是打不开的垃圾文件。
bool CorruptMdatPayload(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t type = bytes.find("mdat");
    if (type == std::string::npos || type < 4 || bytes.size() < 4096) {
        *error = "找不到 mdat 或文件过小";
        return false;
    }
    const auto read_be32 = [&](std::size_t at) -> uint32_t {
        return (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 2])) << 8) |
               static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 3]));
    };
    const uint32_t box_size = read_be32(type - 4);
    if (box_size < 1024 || type + 4 + box_size > bytes.size()) {
        *error = "mdat 盒大小异常";
        return false;
    }
    std::string damaged = bytes;
    const std::size_t begin = type + 4 + static_cast<std::size_t>(box_size) / 3;
    const std::size_t end = std::min(begin + 768, type + 4 + static_cast<std::size_t>(box_size));
    for (std::size_t i = begin; i < end; ++i)
        damaged[i] = static_cast<char>(0xFF);
    WriteBytes(path, damaged);
    return true;
}

// 视频 #0 + 音频 #1 的「要求验证轨道」契约。
std::vector<RepairTrackVerificationTarget> MakeTwoTrackTargets() {
    RepairTrackVerificationTarget video;
    video.track_id = 1;
    video.input_stream_index = 0;
    video.output_stream_index = 0;
    video.kind = "video";
    video.required = true;

    RepairTrackVerificationTarget audio = video;
    audio.track_id = 2;
    audio.input_stream_index = 1;
    audio.output_stream_index = 1;
    audio.kind = "audio";
    return {video, audio};
}

} // namespace

// ---------------- R01：路径别名与独占预留 ----------------

TEST(RepairSafety, PartSourceAliasIsRejectedBeforeAnyWrite) {
    const std::string dir = MakeTempDir("part_alias");
    const std::string source = dir + "/recording.mp4.part";
    WriteBytes(source, "SOURCE-BYTES");

    RepairRequest request = MakeRequest(source, dir);
    request.output_path = "recording.mp4"; // part_path 正好等于源文件
    const repair::RepairOutputPaths paths = repair::RepairOutputWriter::Resolve(request, "run-alias");

    EXPECT_FALSE(paths.valid) << paths.error;
    EXPECT_NE(paths.error.find("冲突"), std::string::npos) << paths.error;
    EXPECT_EQ(ReadText(source), "SOURCE-BYTES");
}

TEST(RepairSafety, ReportPathAndReferenceConflictsAreRejected) {
    const std::string dir = MakeTempDir("report_alias");
    const std::string source = dir + "/clip.mp4";
    WriteBytes(source, "SOURCE");

    RepairRequest request = MakeRequest(source, dir);
    request.output_path = "out.mp4";
    request.reference_path = dir + "/out.repair.json"; // 与 report_json_path 同路径
    WriteBytes(request.reference_path, "REFERENCE");
    const repair::RepairOutputPaths paths = repair::RepairOutputWriter::Resolve(request, "run-report");
    EXPECT_FALSE(paths.valid) << paths.error;

    // 报告路径本身也不能是源文件。
    const std::string dir2 = MakeTempDir("report_source");
    const std::string source2 = dir2 + "/x.repair.json";
    WriteBytes(source2, "SOURCE");
    RepairRequest request2 = MakeRequest(source2, dir2);
    request2.output_path = "x.mp4";
    EXPECT_FALSE(repair::RepairOutputWriter::Resolve(request2, "run-report2").valid);
}

TEST(RepairSafety, ExistingPartFileIsNotTruncatedOrDeleted) {
    const std::string dir = MakeTempDir("reserve");
    const std::string source = dir + "/clip.mp4";
    WriteBytes(source, "SOURCE");
    const repair::RepairOutputPaths paths =
        repair::RepairOutputWriter::Resolve(MakeRequest(source, dir), "run-reserve");
    ASSERT_TRUE(paths.valid) << paths.error;

    WriteBytes(paths.part_path, "PRECIOUS-USER-FILE");
    std::string error;
    EXPECT_FALSE(repair::RepairOutputWriter::Reserve(paths, &error));
    EXPECT_EQ(ReadText(paths.part_path), "PRECIOUS-USER-FILE") << "独占预留不得截断已有文件";

    // 换一个干净路径：只有第一次能成功，第二次必须失败。
    const repair::RepairOutputPaths fresh =
        repair::RepairOutputWriter::Resolve(MakeRequest(source, dir), "run-reserve-2");
    ASSERT_TRUE(fresh.valid);
    EXPECT_TRUE(repair::RepairOutputWriter::Reserve(fresh, &error)) << error;
    EXPECT_FALSE(repair::RepairOutputWriter::Reserve(fresh, &error));
    EXPECT_TRUE(std::filesystem::exists(fresh.part_path));
    repair::RepairOutputWriter::Discard(fresh.part_path);
}

TEST(RepairSafety, FilesystemAliasIsDetected) {
    const std::string dir = MakeTempDir("alias");
    const std::string source = dir + "/Alias.mp4";
    WriteBytes(source, "X");
    EXPECT_TRUE(repair::PathsAlias(source, dir + "/alias.mp4"));
}

TEST(RepairSafety, NotYetExistingArtifactAliasesAreNormalized) {
    // 产物在解析阶段还不存在，equivalent 帮不上忙：必须靠词法归一化 + 大小写折叠。
    // 旧实现只比 final_path == source_path，随手一个 "sub/../" 就能绕过去。
    const std::string dir = MakeTempDir("alias_future");
    const std::string a = dir + "/future.mp4";
    const std::string b = dir + "/nested/../future.mp4";
    ASSERT_FALSE(std::filesystem::exists(a));
    ASSERT_FALSE(std::filesystem::exists(b));
    EXPECT_TRUE(repair::PathsAlias(a, b)) << "尚不存在的产物路径也必须能识别别名";
    EXPECT_EQ(repair::PathAliasKey(a), repair::PathAliasKey(b));
}

// ---------------- R02：恶意样本计数 ----------------

TEST(RepairSafety, HugeDeclaredSampleCountDoesNotAllocateDtsArray) {
    const IsobmffTrack track = MakeTrackWithHugeDeclaration();
    constexpr uint64_t kFileSize = 1024;
    const auto started = std::chrono::steady_clock::now();
    const repair::RepairTrackBounds bounds =
        repair::AnalyzeTrackBounds(track, kFileSize, nullptr, repair::kDefaultMaxTrackSamples);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    EXPECT_TRUE(bounds.checked);
    EXPECT_TRUE(bounds.declaration_implausible) << "声明数必须由文件容量反推校验";
    EXPECT_EQ(bounds.declared_sample_count, UINT32_MAX);
    EXPECT_LE(bounds.sample_count, kFileSize + 1) << "不得按 UINT32_MAX 展开";
    EXPECT_LE(bounds.samples_examined, kFileSize + 1);
    EXPECT_LT(elapsed_ms, 2000) << "旧实现会逐样本遍历 42 亿次 / 申请 32 GiB";
}

TEST(RepairSafety, NormalSampleBoundsStillReportsOutOfBoundsWithTiming) {
    IsobmffTrack track = MakeTrackWithHugeDeclaration();
    track.stsz.sample_count = 4;
    track.stsz.default_size = 100;
    track.stts = {{4, 100}};
    constexpr uint64_t kFileSize = 250;
    const repair::RepairTrackBounds bounds = repair::AnalyzeTrackBounds(track, kFileSize);
    EXPECT_EQ(bounds.bad_sample_count, 2u);
    EXPECT_EQ(bounds.first_bad_sample, 2u);
    EXPECT_EQ(bounds.samples_examined, 4u);
    ASSERT_TRUE(bounds.salvage_seconds.has_value());
    EXPECT_NEAR(*bounds.salvage_seconds, 0.2, 0.001);
}

TEST(RepairSafety, SharedBudgetStopsSampleScan) {
    const IsobmffTrack track = MakeTrackWithHugeDeclaration();
    std::atomic<bool> cancel{true};
    const repair::RepairRunBudget budget(&cancel, 0);
    const repair::RepairTrackBounds bounds = repair::AnalyzeTrackBounds(track, 1024, &budget);
    EXPECT_TRUE(bounds.truncated_by_budget);
    EXPECT_EQ(bounds.samples_examined, 0u) << "诊断一开始就取消，不该再逐样本扫一遍";
    EXPECT_NE(bounds.note.find("预算"), std::string::npos) << bounds.note;
}

// ---- 切片 A-3：共享预算必须真的贯穿诊断内部环节 ----
//
// 只把 budget 写进函数签名不算数：逐个传入「已取消」的预算，要求每个长循环环节
// 都明确拒绝；再用「未取消」的预算做反向守护，防止有人把检查写成恒返回取消。
TEST(RepairSafety, SharedBudgetStopsPlanningForensicsAndSnapshotStages) {
    std::atomic<bool> cancel{true};
    const repair::RepairRunBudget stopped(&cancel, 0);
    const repair::RepairRunBudget live(nullptr, 0);

    model::RepairDiagnosis diagnosis;
    const auto plan = repair::RepairPlanner::MakePlan(RepairRequest{}, diagnosis, std::nullopt, 0, nullptr, &stopped);
    EXPECT_FALSE(plan.can_execute);
    EXPECT_EQ(plan.early_status, RepairBusinessStatus::Canceled);
    EXPECT_NE(plan.plan.infeasible_reason.find("规划阶段被取消"), std::string::npos) << plan.plan.infeasible_reason;

    const std::string dir = MakeTempDir("budget_stage");
    const auto media = videoeye_test::CreateTestMedia(dir, "budget_src");
    ASSERT_TRUE(media.ok) << media.error;
    IsobmffFile parsed;

    const auto ev = repair::RepairFragmentForensics::Analyze(media.path, parsed, nullptr, &stopped);
    EXPECT_FALSE(ev.analyzed) << "入口就取消时不该产出半张取证真值表";
    EXPECT_NE(ev.error.find("被取消"), std::string::npos) << ev.error;

    const auto init = repair::RepairInitSegmentReader::Read(media.path, {}, nullptr, &stopped);
    EXPECT_FALSE(init.ok);
    EXPECT_NE(init.error.find("被取消"), std::string::npos) << init.error;

    const auto synth = repair::RepairInitSegmentBuilder::Build(media.path, parsed, ev, nullptr, &stopped);
    EXPECT_FALSE(synth.ok);
    EXPECT_NE(synth.error.find("被取消"), std::string::npos) << synth.error;

    const auto snapshot = repair::RepairOutputWriter::MakeSnapshot(media.path, &stopped);
    EXPECT_TRUE(snapshot.content_hash.empty());
    EXPECT_EQ(snapshot.hash_method, "unavailable");

    std::string error;
    std::string method;
    EXPECT_FALSE(repair::RepairOutputWriter::SourceUnchanged(snapshot, &error, &method, &stopped));
    EXPECT_NE(error.find("未完成输入一致性核对"), std::string::npos) << error;

    // 反向守护：预算没命中时这些环节必须照常工作。
    const auto live_snapshot = repair::RepairOutputWriter::MakeSnapshot(media.path, &live);
    EXPECT_FALSE(live_snapshot.content_hash.empty());
    EXPECT_NE(live_snapshot.hash_method, "unavailable");
    std::string live_error;
    EXPECT_TRUE(repair::RepairOutputWriter::SourceUnchanged(live_snapshot, &live_error, nullptr, &live)) << live_error;
    const auto live_plan = repair::RepairPlanner::MakePlan(RepairRequest{}, diagnosis, std::nullopt, 0, nullptr, &live);
    EXPECT_EQ(live_plan.plan.infeasible_reason.find("被取消"), std::string::npos) << live_plan.plan.infeasible_reason;
}

// ---------------- R03 / R07：结论判据 ----------------

TEST(RepairSafety, ReferenceTimescaleMismatchIsRejected) {
    const auto mismatch = repair::EvaluateReferenceCompatibility(10240, 20480, "h264", "avc1,mp4a");
    EXPECT_EQ(mismatch.status, "rejected");
    EXPECT_NE(mismatch.reason.find("时基"), std::string::npos);

    const auto codec_clash = repair::EvaluateReferenceCompatibility(0, 10240, "h264", "hvc1");
    EXPECT_EQ(codec_clash.status, "rejected");

    const auto verified = repair::EvaluateReferenceCompatibility(10240, 10240, "h264", "avc1");
    EXPECT_EQ(verified.status, "verified");

    const auto unverified = repair::EvaluateReferenceCompatibility(0, 10240, "unknown", "avc1");
    EXPECT_EQ(unverified.status, "unverified") << "没有源侧独立证据时不能凭 track_id 判兼容";
}

TEST(RepairSafety, PerTrackConservationRejectsSmallLosses) {
    // 旧实现按全轨合计 + 5% 容差，951/1000 与 950/1000 都会被当成"无需报告"。
    EXPECT_EQ(repair::EvaluateTrackConservation(1000, 951), "loss");
    EXPECT_EQ(repair::EvaluateTrackConservation(1000, 950), "loss");
    EXPECT_EQ(repair::EvaluateTrackConservation(1000, 1000), "conserved");
    EXPECT_EQ(repair::EvaluateTrackConservation(1000, 1001), "excess");
    EXPECT_EQ(repair::EvaluateTrackConservation(0, 100), "unmapped");
    EXPECT_EQ(repair::ConservationRelation(1000, 951), "merged_or_lost");
    EXPECT_EQ(repair::ConservationRelation(1000, 1001), "split");
}

// ---------------- R04：真实多轨媒体的逐轨验证覆盖 ----------------

TEST(RepairSafety, FullDecodeVerifiesEveryRetainedTrackOnRealMedia) {
    // 合成样本是 H.264 视频 + AAC 音频双轨。旧验证器固定挑"第一条视频轨"，
    // 音频轨根本没有独立解码结论，坏音频会被整体 Passed 盖住。
    const std::string dir = MakeTempDir("per_track");
    const auto media = videoeye_test::CreateTestMedia(dir, "per_track_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.verification_level = RepairVerificationLevel::V2_FullDecode;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " / " << result.error;
    ASSERT_GE(result.verification.per_track.size(), 2u) << "视频轨与音频轨都必须留下独立的验证记录";

    bool video_verified = false;
    bool audio_verified = false;
    for (const auto& track : result.verification.per_track) {
        if (track.kind == "video" && track.verified && track.frames > 0)
            video_verified = true;
        if (track.kind == "audio" && track.verified && track.frames > 0)
            audio_verified = true;
    }
    EXPECT_TRUE(video_verified) << result.verification.decode_scope;
    EXPECT_TRUE(audio_verified) << "音频轨没有实际解码过，不能算逐目标轨验证通过";
    EXPECT_FALSE(result.verification.structure_only);
    EXPECT_NE(result.verification.decode_scope.find("audio"), std::string::npos) << result.verification.decode_scope;
}

// ---------------- R08：同大小改写 ----------------

TEST(RepairSafety, SameSizeRewrittenSourceIsDetectedEvenWhenMtimeRestored) {
    const std::string dir = MakeTempDir("hash");
    const std::string source = dir + "/same.mp4";
    WriteBytes(source, std::string(4096, 'A'));
    const RepairInputSnapshot snapshot = repair::RepairOutputWriter::MakeSnapshot(source);
    ASSERT_FALSE(snapshot.content_hash.empty());
    const auto original_mtime = std::filesystem::last_write_time(source);

    WriteBytes(source, std::string(4096, 'B')); // 同大小、同 mtime（复位）
    std::filesystem::last_write_time(source, original_mtime);

    std::string error;
    std::string method;
    EXPECT_FALSE(repair::RepairOutputWriter::SourceUnchanged(snapshot, &error, &method)) << error;
    EXPECT_NE(error.find("摘要"), std::string::npos) << error;
    EXPECT_NE(method.find("hash"), std::string::npos) << method;
}

// ---------------- R06 / R09 / R10：编排语义 ----------------

TEST(RepairSafety, CancelAtPublishingDoesNotPublish) {
    const std::string dir = MakeTempDir("publish_cancel");
    const auto media = videoeye_test::CreateTestMedia(dir, "publish");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    std::atomic<bool> cancel{false};
    bool reached_publishing = false;
    const repair::RepairProgress progress = [&](int percent, const std::string&, const std::string&) {
        if (percent >= 97) {
            reached_publishing = true;
            cancel.store(true);
        }
    };
    const RepairResult result = repair::RepairEngine::Run(request, &cancel, progress);

    EXPECT_TRUE(reached_publishing);
    EXPECT_EQ(result.status, RepairBusinessStatus::Canceled) << ToString(result.status) << " " << result.summary;
    EXPECT_FALSE(result.output_published);
    EXPECT_TRUE(result.output_path.empty());
}

TEST(RepairSafety, V0NeverClaimsPlaybackVerified) {
    const std::string dir = MakeTempDir("v0");
    const auto media = videoeye_test::CreateTestMedia(dir, "v0");
    ASSERT_TRUE(media.ok) << media.error;
    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.verification_level = RepairVerificationLevel::V0_Container;

    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    EXPECT_TRUE(result.verification.structure_only);
    EXPECT_EQ(result.verification.decoded_frames, 0u);
    EXPECT_EQ(result.verification.status, RepairCheckStatus::Inconclusive);
    EXPECT_EQ(result.status, RepairBusinessStatus::PartiallyRecovered) << ToString(result.status);
    EXPECT_NE(result.fidelity.content_completeness, "complete_for_selected_checks");
}

TEST(RepairSafety, TimeBudgetCoversDiagnosisStageBeforePacketLoop) {
    const std::string dir = MakeTempDir("budget");
    const auto media = videoeye_test::CreateTestMedia(dir, "budget");
    ASSERT_TRUE(media.ok) << media.error;
    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.limits.max_seconds = 1;

    bool first_callback = true;
    const repair::RepairProgress progress = [&](int, const std::string&, const std::string&) {
        if (first_callback) {
            first_callback = false;
            // 诊断阶段就耗尽预算；旧实现只在写包循环开始计时，会一路修复成功。
            std::this_thread::sleep_for(std::chrono::milliseconds(1150));
        }
    };
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, progress);
    EXPECT_EQ(result.status, RepairBusinessStatus::TimedOut) << ToString(result.status) << " " << result.summary;
    EXPECT_FALSE(result.output_published);
}

// ---------------- R12：异常终态 ----------------

TEST(RepairSafety, ControllerReachesTerminalStateWhenEngineThrows) {
    static int argc = 1;
    static char arg0[] = "test_repair_safety";
    static char* argv[] = {arg0, nullptr};
    if (QCoreApplication::instance() == nullptr)
        new QCoreApplication(argc, argv); // NOLINT: 与项目其它 Qt 用例一致，存活到进程结束

    qt::RepairController controller;
    qt::RepairController::SetEngineRunnerForTesting(
        [](const RepairRequest&, const std::atomic<bool>*, const repair::RepairProgress&) -> RepairResult {
            throw std::runtime_error("engine exploded");
        });

    QEventLoop loop;
    bool finished = false;
    QObject::connect(&controller, &qt::RepairController::RepairFinished, &loop,
                     [&](quint64, const QString&, const QString&) {
                         finished = true;
                         loop.quit();
                     });
    QTimer::singleShot(4000, &loop, [&]() { loop.quit(); });

    RepairRequest request;
    request.source_path = "does-not-matter.mp4"; // runner 会直接抛异常
    ASSERT_NE(controller.StartRepair(request), 0u);
    loop.exec();

    EXPECT_TRUE(finished) << "后台异常时控制器必须发出 RepairFinished，不能永远停在'运行中'";
    EXPECT_EQ(controller.terminal_state(), task::TaskState::Failed);
    ASSERT_NE(controller.result(), nullptr);
    EXPECT_EQ(controller.result()->status, RepairBusinessStatus::Failed);
    qt::RepairController::SetEngineRunnerForTesting(nullptr);
}

// ---------------- R13：三格式同源 ----------------

TEST(RepairSafety, AllReportFormatsShowResidualFindingsAndExecutionDetails) {
    const std::string dir = MakeTempDir("reports");
    RepairResult result;
    result.result_id = "result-unit";
    result.status = RepairBusinessStatus::PartiallyRecovered;
    result.summary = "存在残留问题";
    result.output_published = true;
    result.output_path = dir + "/out.mp4";
    result.ffmpeg_version = "unit-test";

    DamageFinding residual;
    residual.code = "unit.residual_marker";
    residual.evidence = "验证后发现轨道 2 损坏";
    residual.limitations.push_back("不能证明画面干净");
    result.residual_findings.push_back(residual);

    RepairActionExecution execution;
    execution.action = RepairAction::S01_Remux;
    execution.attempted = true;
    execution.succeeded = true;
    execution.packets_written = 12;
    execution.detail = "重封装写入 12 个包";
    result.executions.push_back(execution);
    result.fidelity.limitations.push_back("未做逐帧画质评估");

    // R05：逐轨的损坏包 / 损坏帧 / 错误恢复帧也必须进人类可读报告。
    RepairTrackVerification damaged_track;
    damaged_track.kind = "video";
    damaged_track.output_stream_index = 0;
    damaged_track.status = RepairCheckStatus::Inconclusive;
    damaged_track.corrupt_packets = 7;
    damaged_track.corrupt_frames = 3;
    damaged_track.error_recovery_frames = 2;
    damaged_track.valid_frames = 11;
    damaged_track.detail = "逐轨损坏证据不应只在 JSON 里";
    result.verification.per_track.push_back(damaged_track);

    const std::string json_path = dir + "/r.repair.json";
    const std::string text_path = dir + "/r.repair.txt";
    const std::string html_path = dir + "/r.repair.html";
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, json_path, text_path, html_path);
    ASSERT_TRUE(written.AllOk()) << written.error;

    const std::string json = ReadText(json_path);
    const std::string text = ReadText(text_path);
    const std::string html = ReadText(html_path);
    for (const auto& body : {json, text, html}) {
        EXPECT_NE(body.find("unit.residual_marker"), std::string::npos) << "残留问题必须在每种格式里出现";
        EXPECT_NE(body.find("重封装写入 12 个包"), std::string::npos) << "执行明细不能只在 TXT 里";
        EXPECT_NE(body.find("未做逐帧画质评估"), std::string::npos) << "保真度限制不能只在 TXT 里";
        EXPECT_NE(body.find("逐轨损坏证据不应只在 JSON 里"), std::string::npos) << "逐轨验证明细必须在每种格式里出现";
    }
    for (const auto& body : {text, html})
        EXPECT_NE(body.find("corrupt_packets=7"), std::string::npos) << "逐轨损坏包计数必须进 TXT / HTML";
}

// ---------------- R04 / R05：逐轨验证结论可信度（真实媒体） ----------------
//
// 上一轮的逐轨验证只覆盖了「全部轨都成功」的正例。这里专打三条残留漏判：
//   ① 某条要求验证的轨失败时，失败分支没有写进 checks，整体状态仍能 Passed；
//   ② record.verified 无条件置 true，解码器没打开 / 0 帧 / 超时也自称「已验证」；
//   ③ targets 少报了一条产物里实际存在的轨，验证器直接信任，覆盖范围被静默缩小。

TEST(RepairSafety, OutOfRangeTrackFailureIsNotMaskedByOverallPassed) {
    const std::string dir = MakeTempDir("range_fail");
    const auto media = videoeye_test::CreateTestMedia(dir, "range_fail");
    ASSERT_TRUE(media.ok) << media.error;

    // 产物里两条轨都正常覆盖，另加一条执行器给错下标的「要求验证轨」。
    auto targets = MakeTwoTrackTargets();
    RepairTrackVerificationTarget dangling = targets.back();
    dangling.track_id = 3;
    dangling.output_stream_index = 999;
    targets.push_back(dangling);

    const repair::RepairVerifyOutcome outcome = repair::RepairVerifier::Verify(
        media.path, RepairVerificationLevel::V2_FullDecode, std::nullopt, nullptr, nullptr, targets, nullptr, 0.0);
    const RepairVerification& v = outcome.verification;
    ASSERT_EQ(v.per_track.size(), 3u) << v.evidence;
    EXPECT_EQ(v.per_track.back().status, RepairCheckStatus::Failed);
    EXPECT_FALSE(v.per_track.back().verified);
    EXPECT_NE(v.status, RepairCheckStatus::Passed) << "有一条要求验证的轨失败，整体结论不能仍是 Passed: " << v.evidence;
}

TEST(RepairSafety, TrackIsMarkedVerifiedOnlyWhenFramesWereDecoded) {
    const std::string dir = MakeTempDir("verified_flag");
    const auto media = videoeye_test::CreateTestMedia(dir, "verified_flag");
    ASSERT_TRUE(media.ok) << media.error;

    // 在第一个进度回调里耗尽预算：容器已经能打开，但逐轨解码一帧都不该跑。
    // 用超时而不是取消，避免 FFmpeg 的 IO 中断回调把 open 直接掐掉。
    std::atomic<bool> no_cancel{false};
    const repair::RepairRunBudget budget(&no_cancel, 1);
    bool slept = false;
    const repair::RepairProgress progress = [&](int, const std::string&, const std::string&) {
        if (!slept) {
            slept = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1150));
        }
    };

    const repair::RepairVerifyOutcome outcome =
        repair::RepairVerifier::Verify(media.path, RepairVerificationLevel::V2_FullDecode, std::nullopt, nullptr,
                                       progress, MakeTwoTrackTargets(), &budget, 0.0);
    const RepairVerification& v = outcome.verification;
    ASSERT_GE(v.per_track.size(), 2u) << v.evidence;
    for (const auto& track : v.per_track) {
        EXPECT_EQ(track.frames, 0u) << track.kind << " 预算已耗尽，不该解出帧";
        EXPECT_FALSE(track.verified) << track.kind << " 一帧都没解出来，不能自称已验证";
    }
}

TEST(RepairSafety, UncoveredRetainedTracksAreReportedAsInconclusive) {
    const std::string dir = MakeTempDir("coverage_gap");
    const auto media = videoeye_test::CreateTestMedia(dir, "coverage_gap");
    ASSERT_TRUE(media.ok) << media.error;

    // 执行器只上报了视频轨；产物里真实存在的音频轨不能被静默忽略。
    auto targets = MakeTwoTrackTargets();
    targets.pop_back();
    const repair::RepairVerifyOutcome outcome = repair::RepairVerifier::Verify(
        media.path, RepairVerificationLevel::V2_FullDecode, std::nullopt, nullptr, nullptr, targets, nullptr, 0.0);
    const RepairVerification& v = outcome.verification;

    bool audio_recorded = false;
    for (const auto& track : v.per_track) {
        if (track.kind != "audio")
            continue;
        audio_recorded = true;
        EXPECT_FALSE(track.verified) << "未覆盖的轨不能标成已验证";
        EXPECT_NE(track.status, RepairCheckStatus::Passed) << track.detail;
    }
    EXPECT_TRUE(audio_recorded) << "产物里有的音频轨必须在逐轨结果里留痕: " << v.decode_scope;
    EXPECT_NE(v.status, RepairCheckStatus::Passed) << "覆盖范围有缺口时不能报整体通过: " << v.evidence;
}

TEST(RepairSafety, CorruptPayloadIsNotReportedAsCleanPass) {
    const std::string dir = MakeTempDir("corrupt_payload");
    const auto media = videoeye_test::CreateTestMedia(dir, "corrupt_payload");
    ASSERT_TRUE(media.ok) << media.error;
    std::string error;
    ASSERT_TRUE(CorruptMdatPayload(media.path, &error)) << error;

    // targets 留空 = 让验证器按产物实际流枚举，避免覆盖守护介入，专看损坏证据。
    const repair::RepairVerifyOutcome outcome = repair::RepairVerifier::Verify(
        media.path, RepairVerificationLevel::V2_FullDecode, std::nullopt, nullptr, nullptr, {}, nullptr, 0.0);
    const RepairVerification& v = outcome.verification;

    const bool has_decode_evidence =
        v.decode_errors > 0 || v.corrupt_packets > 0 || v.corrupt_frames > 0 || v.error_recovery_frames > 0;
    EXPECT_TRUE(has_decode_evidence) << "损坏码流必须留下逐层证据: " << v.evidence;
    EXPECT_NE(v.status, RepairCheckStatus::Passed) << "带损坏证据的产物不能报整体通过: " << v.evidence;

    bool track_reports_damage = false;
    for (const auto& track : v.per_track) {
        if (track.errors > 0 || track.corrupt_packets > 0 || track.corrupt_frames > 0 ||
            track.error_recovery_frames > 0 || track.frames == 0)
            track_reports_damage = true;
    }
    EXPECT_TRUE(track_reports_damage) << "损坏必须落到具体轨道，不能只在全局计数里: " << v.decode_scope;
}

// ---------------- R03 残留：时间轴证据没有落到结论层 ----------------
//
// 上一轮只把「源侧 sidx/tfdt 估算时长 vs 产物实测时长」打印进 timeline_checks，
// 没有判定；引擎又硬编码 timeline_modified=false。结果就是原报告 R03 描述的
// 「全部帧都解出来了，播放时长却变了一半」仍然可以拿到 Passed / Repaired。

static bool HasVerificationCheck(const RepairVerification& v, const std::string& name) {
    for (const auto& check : v.checks) {
        if (check.name.find(name) != std::string::npos)
            return true;
    }
    return false;
}

static bool HasResidualCode(const RepairVerification& v, const std::string& code) {
    for (const auto& finding : v.residual_findings) {
        if (finding.code == code)
            return true;
    }
    return false;
}

TEST(RepairSafety, TimelineDurationMismatchIsNotReportedAsCleanPass) {
    const std::string dir = MakeTempDir("timeline_mismatch");
    const auto media = videoeye_test::CreateTestMedia(dir, "timeline_mismatch_src");
    ASSERT_TRUE(media.ok) << media.error;
    const auto probe = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(probe.opened) << probe.error;
    ASSERT_GT(probe.video_duration_seconds, 0.0) << probe.error;

    // 模拟错误时基的参考初始化段：源侧给出的时长基线是产物实测的两倍。
    const double wrong_expected = probe.video_duration_seconds * 2.0;
    const repair::RepairVerifyOutcome outcome =
        repair::RepairVerifier::Verify(media.path, RepairVerificationLevel::V2_FullDecode, std::nullopt, nullptr,
                                       nullptr, {}, nullptr, wrong_expected);
    const RepairVerification& v = outcome.verification;

    std::string checks;
    for (const auto& check : v.checks)
        checks += check.name + "=" + ToString(check.status) + "; ";
    EXPECT_TRUE(HasVerificationCheck(v, "timeline.duration_mismatch"))
        << "源侧估算时长与产物实测时长明显不符，必须留下时间轴检查证据: " << checks;
    EXPECT_TRUE(HasResidualCode(v, "timeline.duration_mismatch")) << "时长差必须作为残留问题上报: " << v.evidence;
    EXPECT_NE(v.status, RepairCheckStatus::Passed)
        << "时长差一倍不能报整体通过（帧数一样不代表播放时长没变）: " << v.evidence;
}

TEST(RepairSafety, UnverifiedReferenceIsNotClaimedAsRepairedOrUnchangedTimeline) {
    const std::string dir = MakeTempDir("fmp4_unverified");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "fmp4");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.segment_path, dir + "/out");
    request.reference_path = media.init_path;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << "参考段必须被接受，否则测不到 unverified 结论层: " << result.summary
                                         << " / " << result.error;
    ASSERT_EQ(result.fidelity.reference_compatibility, "unverified")
        << "本用例针对源侧缺少独立时基证据的场景，实际=" << result.fidelity.reference_compatibility;

    EXPECT_NE(result.status, RepairBusinessStatus::Repaired)
        << "参考段兼容性未核验时不能报 Repaired: " << result.summary;
    EXPECT_NE(result.fidelity.content_completeness, "complete_for_selected_checks")
        << "时间轴未核验时完整性结论必须受限: " << result.fidelity.content_completeness;

    const std::string json_path = dir + "/timeline.repair.json";
    const std::string text_path = dir + "/timeline.repair.txt";
    const std::string html_path = dir + "/timeline.repair.html";
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, json_path, text_path, html_path);
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string json = ReadText(json_path);
    const std::string text = ReadText(text_path);
    const std::string html = ReadText(html_path);
    for (const auto& body : {json, text, html}) {
        EXPECT_EQ(body.find("时间轴改写: no"), std::string::npos) << "未经核验的借用时基不能向用户声称时间轴未改写";
    }
    EXPECT_NE(json.find("timeline_status"), std::string::npos) << "JSON 必须输出三态时间轴结论";
    EXPECT_NE(text.find("时间轴结论: unknown"), std::string::npos) << "TXT 必须写明时间轴未核验";
    EXPECT_NE(text.find("时间轴实测"), std::string::npos) << "TXT 必须展示源侧 / 产物时长实测证据";
    EXPECT_NE(html.find("时间轴结论"), std::string::npos) << "HTML 必须展示三态时间轴结论";
    EXPECT_NE(html.find("时间轴实测"), std::string::npos) << "HTML 必须展示源侧 / 产物时长实测证据";
}

// 反向守护：时长一致时不能把「已比对」误判成「时基被改写」，否则正常的重封装
// 会被一律降级成 PartiallyRecovered。
TEST(RepairSafety, MatchingDurationIsNotMistakenForTimelineRewrite) {
    const std::string dir = MakeTempDir("timeline_match");
    const auto media = videoeye_test::CreateTestMedia(dir, "timeline_match_src");
    ASSERT_TRUE(media.ok) << media.error;
    const auto probe = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(probe.opened) << probe.error;
    ASSERT_GT(probe.video_duration_seconds, 0.0);

    const repair::RepairVerifyOutcome outcome =
        repair::RepairVerifier::Verify(media.path, RepairVerificationLevel::V2_FullDecode, std::nullopt, nullptr,
                                       nullptr, {}, nullptr, probe.video_duration_seconds);
    const RepairVerification& v = outcome.verification;
    EXPECT_TRUE(v.timeline_comparable) << "两侧都有实测时长，必须记录为已比对: " << v.evidence;
    EXPECT_FALSE(v.timeline_mismatch) << "时长一致不能误报时基改写: " << v.evidence;
    EXPECT_FALSE(HasVerificationCheck(v, "timeline.duration_mismatch"));
    EXPECT_EQ(v.status, RepairCheckStatus::Passed) << "正常重封装不该被降级: " << v.evidence;
}

// ---------------- R03 续：参考段逐轨时基 / sidx 独立证据 ----------------

namespace {

struct InitTrackView {
    uint32_t track_id = 0;
    uint32_t timescale = 0;
    std::string codec;
    std::size_t timescale_offset = 0; // mdhd.timescale 在文件里的绝对偏移
};

uint32_t ReadBe32At(const std::string& bytes, std::size_t at) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 3]));
}

void WriteBe32At(std::string* bytes, std::size_t at, uint32_t value) {
    (*bytes)[at] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[at + 1] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[at + 2] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[at + 3] = static_cast<char>(value & 0xFF);
}

// 用 IsobmffParser 拿逐轨语义，用字节扫描定位每条 mdhd 的 timescale 字段。
// 两者按 trak 出现顺序一一对应；数量不符就判为样本不可用，不静默猜测。
std::vector<InitTrackView> ReadInitTrackViews(const std::string& path, std::string* error) {
    std::vector<InitTrackView> views;
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    IsobmffFile parsed;
    IsobmffOptions options;
    options.skip_sample_tables = true;
    if (!IsobmffParser::Parse(path, parsed, options)) {
        *error = "无法解析参考初始化段: " + parsed.error_message;
        return views;
    }
    std::vector<std::size_t> offsets;
    for (std::size_t p = 4; p + 8 <= bytes.size(); ++p) {
        if (bytes.compare(p, 4, "mdhd") != 0)
            continue;
        const std::size_t box = p - 4;
        const uint32_t box_size = ReadBe32At(bytes, box);
        if (box_size < 24 || box + box_size > bytes.size())
            continue;
        const uint8_t version = static_cast<uint8_t>(bytes[p + 4]);
        // v0: version/flags(4) + creation(4) + modification(4) -> timescale
        // v1: 两个时间戳各 8 字节 -> timescale 在 payload+20
        const std::size_t ts = p + 4 + (version == 1 ? 20 : 12);
        if (ts + 4 > bytes.size())
            continue;
        offsets.push_back(ts);
    }
    if (offsets.size() != parsed.tracks.size()) {
        *error = "mdhd 盒数 " + std::to_string(offsets.size()) + " 与轨道数 " + std::to_string(parsed.tracks.size()) +
                 " 不一致";
        return views;
    }
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        InitTrackView view;
        view.track_id = parsed.tracks[i].track_id;
        view.codec = parsed.tracks[i].codec;
        view.timescale_offset = offsets[i];
        view.timescale = ReadBe32At(bytes, offsets[i]);
        views.push_back(std::move(view));
    }
    return views;
}

std::string DescribeViews(const std::vector<InitTrackView>& views) {
    std::string out;
    for (const auto& v : views)
        out +=
            "track " + std::to_string(v.track_id) + " ts=" + std::to_string(v.timescale) + " codec=" + v.codec + "; ";
    return out;
}

bool RewriteInitTimescales(const std::string& src, const std::string& dst, const std::vector<InitTrackView>& views,
                           const std::vector<std::size_t>& indices, uint32_t multiplier, std::string* error) {
    std::ifstream in(src, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        *error = "参考初始化段为空";
        return false;
    }
    for (std::size_t index : indices) {
        if (index >= views.size()) {
            *error = "轨道下标越界";
            return false;
        }
        const std::size_t at = views[index].timescale_offset;
        WriteBe32At(&bytes, at, ReadBe32At(bytes, at) * multiplier);
    }
    WriteBytes(dst, bytes);
    return true;
}

// 给裸 moof/mdat 分片前置一个合法 version-0 sidx：提供源侧独立时基证据。
// 布局按 ISO/IEC 14496-12，且必须与 RepairFragmentForensics::ReadSidx 的读法一致。
bool PrependSidx(const std::string& src, const std::string& dst, uint32_t timescale, uint32_t duration_ticks,
                 std::string* error) {
    std::ifstream in(src, std::ios::binary);
    const std::string segment((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (segment.size() < 16 || segment.size() > UINT32_MAX) {
        *error = "分片长度不适合构造 sidx";
        return false;
    }
    std::string box(44, '\0');
    WriteBe32At(&box, 0, 44);
    box.replace(4, 4, "sidx");
    // [8] version=0, [9..11] flags=0
    WriteBe32At(&box, 12, 1);                                     // reference_ID
    WriteBe32At(&box, 16, timescale);                             // timescale
    WriteBe32At(&box, 20, 0);                                     // earliest_presentation_time
    WriteBe32At(&box, 24, 0);                                     // first_offset
    box[31] = 1;                                                  // reference_count = 1
    WriteBe32At(&box, 32, static_cast<uint32_t>(segment.size())); // reference_type=0 + size
    WriteBe32At(&box, 36, duration_ticks);                        // subsegment_duration
    // [40..43] SAP = 0
    WriteBytes(dst, box + segment);
    return true;
}

bool ConcatFiles(const std::vector<std::string>& sources, const std::string& dst) {
    std::string joined;
    for (const auto& src : sources) {
        std::ifstream in(src, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty())
            return false;
        joined += bytes;
    }
    WriteBytes(dst, joined);
    return !joined.empty();
}

} // namespace

// 旧判定只检查「反向冲突」：源侧是 H.264 时，只要参考段不出现 hvc/hevc/av01/vp09
// 就放行。参考段只声明 mp4a（根本没有视频解码配置）也会被写成 verified。
TEST(RepairSafety, MissingSourceCodecFamilyIsNotClaimedVerified) {
    const repair::RepairCompatibilityVerdict verdict =
        repair::EvaluateReferenceCompatibility(12800, 12800, "h264", "mp4a");
    EXPECT_NE(verdict.status, "verified")
        << "参考段只声明音频编码，不能自称已核对 H.264 源分片的解码参数: " << verdict.reason;
    EXPECT_EQ(verdict.status, "unverified") << verdict.reason;
}

// 旧实现只取 parsed.tracks 第一条轨的 media_timescale 作为「参考段时基」。
// 多轨参考段里改掉非首轨的 mdhd timescale，检查完全看不到。
TEST(RepairSafety, NonFirstTrackTimescaleTamperIsNotClaimedVerified) {
    const std::string dir = MakeTempDir("init_ts_tamper");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "fmp4");
    ASSERT_TRUE(media.ok) << media.error;
    std::string error;
    const std::vector<InitTrackView> views = ReadInitTrackViews(media.init_path, &error);
    ASSERT_GE(views.size(), 2u) << error << " / " << DescribeViews(views);
    const std::string dump = DescribeViews(views);

    const std::string tampered = dir + "/init_last_track_x2.mp4";
    ASSERT_TRUE(RewriteInitTimescales(media.init_path, tampered, views, {views.size() - 1}, 2, &error)) << error;

    repair::RepairSourceCompatibilityEvidence evidence;
    evidence.sidx_timescale = views.front().timescale;
    evidence.bitstream = "h264";
    evidence.estimated_duration_seconds = 1.6;
    for (const auto& view : views) {
        evidence.track_ids.push_back(view.track_id);
        // 源侧 trun 实测时长用**未篡改**的时基记账。借来的 mdhd 被改成 2 倍后，
        // 这条轨换算出的秒数会减半 —— 这正是要被逐轨核对抓住的信号。
        repair::RepairSourceTrackTiming timing;
        timing.track_id = view.track_id;
        timing.declared_duration_ticks = static_cast<uint64_t>(1.6 * view.timescale);
        evidence.track_timings.push_back(timing);
    }
    const repair::RepairInitSegment init =
        repair::RepairInitSegmentReader::Read(tampered, evidence.track_ids, evidence, nullptr);
    EXPECT_EQ(init.compatibility, "rejected")
        << "改掉非首轨 mdhd timescale 必须被逐轨 trun 时长核对抓出: " << init.compatibility_reason << " / " << dump;
    EXPECT_FALSE(init.ok) << "不兼容的参考段不能被借用来拼接产物: " << init.error;
    EXPECT_NE(init.compatibility_reason.find("mdhd"), std::string::npos)
        << "拒绝理由必须点明是哪个参数被改写，而不是笼统的「证据不足」: " << init.compatibility_reason;
}

// 原报告 R03 的现象证据：只把 mdhd timescale 加倍，字节仍是同一个完整 fMP4，
// 解码帧数一帧不少，播放时长却减半。这一条不经过我们的任何检查，专证危害真实存在。
TEST(RepairSafety, TamperedTimescaleActuallyHalvesPlaybackDuration) {
    const std::string dir = MakeTempDir("timescale_halves");
    videoeye_test::TestMediaSpec spec;
    spec.with_audio = false;
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "halve", spec);
    ASSERT_TRUE(media.ok) << media.error;
    std::string error;
    const std::vector<InitTrackView> views = ReadInitTrackViews(media.init_path, &error);
    ASSERT_EQ(views.size(), 1u) << error << " / " << DescribeViews(views);
    ASSERT_GT(views.front().timescale, 0u);

    const std::string baseline = dir + "/baseline.mp4";
    ASSERT_TRUE(ConcatFiles({media.init_path, media.segment_path}, baseline));
    const auto base_probe = videoeye_test::ProbeMediaFile(baseline);
    ASSERT_TRUE(base_probe.opened) << base_probe.error;
    ASSERT_GT(base_probe.video_duration_seconds, 0.0);

    const std::string tampered_init = dir + "/init_x2.mp4";
    ASSERT_TRUE(RewriteInitTimescales(media.init_path, tampered_init, views, {0}, 2, &error)) << error;
    const std::string tampered = dir + "/tampered.mp4";
    ASSERT_TRUE(ConcatFiles({tampered_init, media.segment_path}, tampered));
    const auto tampered_probe = videoeye_test::ProbeMediaFile(tampered);
    ASSERT_TRUE(tampered_probe.opened) << tampered_probe.error;

    EXPECT_EQ(tampered_probe.frame_count, base_probe.frame_count)
        << "只改时基不该影响可解码帧数 —— 这正是「全部帧都解出来」的假象";
    EXPECT_NEAR(tampered_probe.video_duration_seconds, base_probe.video_duration_seconds / 2.0, 0.02)
        << "mdhd timescale 加倍使播放时长减半: base=" << base_probe.video_duration_seconds
        << "s tampered=" << tampered_probe.video_duration_seconds << "s";
}

// 带 sidx 的单轨同源参考段：这是上一轮结论里点名缺的 verified 正例，
// 也用来证明「借用参考段但仍能诚实给出 timeline_status=unchanged」的路径真的能跑通。
TEST(RepairSafety, SingleTrackSidxReferenceIsVerifiedWithUnchangedTimeline) {
    const std::string dir = MakeTempDir("sidx_verified");
    videoeye_test::TestMediaSpec spec;
    spec.with_audio = false;
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "single", spec);
    ASSERT_TRUE(media.ok) << media.error;
    std::string error;
    const std::vector<InitTrackView> views = ReadInitTrackViews(media.init_path, &error);
    ASSERT_EQ(views.size(), 1u) << error << " / " << DescribeViews(views);
    const uint32_t timescale = views.front().timescale;
    ASSERT_GT(timescale, 0u);

    const std::string segment = dir + "/seg_with_sidx.m4s";
    ASSERT_TRUE(PrependSidx(media.segment_path, segment, timescale, static_cast<uint32_t>(1.6 * timescale), &error))
        << error;

    RepairRequest request = MakeRequest(segment, dir + "/out");
    request.reference_path = media.init_path;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << "带 sidx 的同源分片必须能修: " << result.summary << " / " << result.error;
    EXPECT_EQ(result.fidelity.reference_compatibility, "verified") << result.summary;
    EXPECT_EQ(result.fidelity.timeline_status, "unchanged") << result.fidelity.timeline_note;
    EXPECT_EQ(result.status, RepairBusinessStatus::Repaired) << result.summary;
    EXPECT_GT(result.verification.decoded_frames, 0u);

    // 逐轨时基核对是结论的依据，必须和残留问题一样在三种格式里同时可见（R13）。
    const std::string json_path = dir + "/sidx.repair.json";
    const std::string text_path = dir + "/sidx.repair.txt";
    const std::string html_path = dir + "/sidx.repair.html";
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, json_path, text_path, html_path);
    ASSERT_TRUE(written.AllOk()) << written.error;
    for (const auto& body : {ReadText(json_path), ReadText(text_path), ReadText(html_path)}) {
        EXPECT_NE(body.find("逐轨 mdhd 时基核对"), std::string::npos) << "逐轨时基核对证据不能只在某一种格式里出现";
        EXPECT_NE(body.find("mdhd timescale="), std::string::npos) << "报告必须逐项列出借用的时基";
    }
}

// 端到端：双轨分片自带 sidx，参考段只把**音频轨**的 mdhd timescale 加倍。
// 修复前首轨仍与 sidx 一致 → verified → 产物照常发布、结论照常 Repaired，
// 而音频轨实际播放时长已经减半（上一条用例已证明这个危害真实存在）。
TEST(RepairSafety, TamperedNonFirstTrackTimescaleDoesNotPublishCleanResult) {
    const std::string dir = MakeTempDir("sidx_dual_tamper");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "dual");
    ASSERT_TRUE(media.ok) << media.error;
    std::string error;
    const std::vector<InitTrackView> views = ReadInitTrackViews(media.init_path, &error);
    ASSERT_EQ(views.size(), 2u) << error << " / " << DescribeViews(views);
    const uint32_t timescale = views.front().timescale;
    ASSERT_GT(timescale, 0u);

    const std::string segment = dir + "/seg_with_sidx.m4s";
    ASSERT_TRUE(PrependSidx(media.segment_path, segment, timescale, static_cast<uint32_t>(1.6 * timescale), &error))
        << error;
    const std::string tampered_init = dir + "/init_audio_x2.mp4";
    ASSERT_TRUE(RewriteInitTimescales(media.init_path, tampered_init, views, {1}, 2, &error)) << error;

    RepairRequest request = MakeRequest(segment, dir + "/out");
    request.reference_path = tampered_init;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    EXPECT_FALSE(result.output_published)
        << "逐轨时基已被改动，不能拿这份参考段发布「全帧可解」的产物: " << result.summary;
    EXPECT_NE(result.fidelity.reference_compatibility, "verified") << result.summary;
    EXPECT_NE(result.status, RepairBusinessStatus::Repaired) << result.summary;
    EXPECT_NE(result.error.find("mdhd"), std::string::npos)
        << "失败原因必须说明是哪条轨的时基被判定不兼容: " << result.error;
}

// ---------------- R05 续：真实带标记帧 + 「恒 0」结论诚实性 ----------------
//
// 修复前：AV_FRAME_FLAG_CORRUPT / decode_error_flags 分类只有代码路径 + 报告层断言，
// 没有一份真实媒体能让它触发；更糟的是 FFmpeg 8.x 的 H.264/AAC 对字节损坏只返回
// API 错误、不设帧标记，报告里「损坏 0」会被误读成「画面干净」。
// 本轮用 MPEG-4 Part 2 的错误隐藏构造真实正例，并把「标记能力」单独落成三态证据。

namespace {

struct ReportBodies {
    std::string json;
    std::string text;
    std::string html;
};

ReportBodies WriteReportsFor(const std::string& dir, const RepairResult& result) {
    const std::string base = dir + "/capability_probe.repair";
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, base + ".json", base + ".txt", base + ".html");
    EXPECT_TRUE(written.AllOk()) << written.error;
    return {ReadText(base + ".json"), ReadText(base + ".txt"), ReadText(base + ".html")};
}

void AppendBe32(std::string* out, uint32_t value) {
    out->push_back(static_cast<char>((value >> 24) & 0xFF));
    out->push_back(static_cast<char>((value >> 16) & 0xFF));
    out->push_back(static_cast<char>((value >> 8) & 0xFF));
    out->push_back(static_cast<char>(value & 0xFF));
}

// 结构合法、只有 ftyp + mdat、没有 moov 的 ISOBMFF：解析器能走完顶层盒，
// 明确落到 container.moov_missing（截断文件会落到 structure_unparsable，不稳定）。
bool WriteMoovlessIsobmff(const std::string& path) {
    std::string bytes;
    AppendBe32(&bytes, 24);
    bytes += "ftyp";
    bytes += "isom";
    AppendBe32(&bytes, 0);
    bytes += "isom"
             "iso2"
             "mp41";
    AppendBe32(&bytes, 8 + 4096);
    bytes += "mdat";
    for (int i = 0; i < 4096; ++i)
        bytes.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
    WriteBytes(path, bytes);
    return std::filesystem::file_size(path) == bytes.size();
}

const RepairCapabilityBoundary* FindBoundary(const RepairDiagnosis& diagnosis, const std::string& code) {
    for (const auto& boundary : diagnosis.capability_boundaries) {
        if (boundary.code == code)
            return &boundary;
    }
    return nullptr;
}

} // namespace

TEST(RepairSafety, DecodeErrorFlagsMarkedFramesAreRealEvidence) {
    const std::string dir = MakeTempDir("marked_frames");
    const auto media = videoeye_test::CreateConcealmentMedia(dir, "concealment");
    ASSERT_TRUE(media.ok) << media.error;
    ASSERT_GT(media.marked_frames, 0u) << "样本自检必须拿到真实带标记帧: " << media.error;
    ASSERT_GT(media.decoded_frames, 0u);

    const repair::RepairVerifyOutcome outcome = repair::RepairVerifier::Verify(
        media.path, RepairVerificationLevel::V2_FullDecode, std::nullopt, nullptr, nullptr, {}, nullptr, 0.0);
    const RepairVerification& v = outcome.verification;
    EXPECT_GT(v.error_recovery_frames, 0u) << "真实 decode_error_flags 帧必须被分类出来: " << v.evidence;
    EXPECT_EQ(v.corrupt_frames, 0u) << "FFmpeg 8.1 的 MPEG-4 只设 decode_error_flags，不设 CORRUPT（现象证据）";
    EXPECT_EQ(v.frame_marker_evidence, "observed") << v.evidence;
    EXPECT_EQ(v.marked_frames, v.corrupt_frames + v.error_recovery_frames) << "带标记帧数必须守恒";
    EXPECT_NE(v.status, RepairCheckStatus::Passed) << v.evidence;
    EXPECT_TRUE(HasResidualCode(v, "decode.error_concealment")) << v.evidence;
    EXPECT_FALSE(v.frame_marker_limitations.empty()) << "「恒 0」的成因必须随结论一起输出";
    EXPECT_NE(v.evidence.find("帧标记=observed"), std::string::npos) << v.evidence;

    bool track_has_markers = false;
    for (const auto& track : v.per_track) {
        if (track.error_recovery_frames > 0)
            track_has_markers = true;
    }
    EXPECT_TRUE(track_has_markers) << "逐轨也必须看得到错误恢复帧: " << v.decode_scope;

    RepairResult result;
    result.status = RepairBusinessStatus::PartiallyRecovered;
    result.verification = v;
    const auto bodies = WriteReportsFor(dir, result);
    EXPECT_NE(bodies.json.find("frame_marker_evidence"), std::string::npos);
    EXPECT_NE(bodies.json.find("marked_frames"), std::string::npos);
    EXPECT_NE(bodies.json.find("frame_marker_limitations"), std::string::npos);
    for (const auto& body : {bodies.text, bodies.html}) {
        EXPECT_NE(body.find("帧标记证据"), std::string::npos) << "三态证据必须进人类可读报告";
        EXPECT_NE(body.find("observed"), std::string::npos);
        EXPECT_NE(body.find("corrupt_frames=0 不能推出画面干净"), std::string::npos)
            << "「恒 0」的成因必须在 TXT / HTML 里同源可见";
    }
}

// 反向守护：一个「解出帧但没有任何帧标记」的结果，报告不得把 0 写成画面干净。
TEST(RepairSafety, CorruptFlagZeroIsNotClaimedAsCleanPicture) {
    const std::string dir = MakeTempDir("marker_zero");
    RepairResult result;
    result.status = RepairBusinessStatus::PartiallyRecovered;
    result.verification.level = RepairVerificationLevel::V2_FullDecode;
    result.verification.decoded_frames = 20;
    result.verification.valid_frames = 20;
    result.verification.corrupt_frames = 0;
    result.verification.error_recovery_frames = 0;
    result.verification.decode_errors = 3;
    result.verification.frame_marker_evidence = "not_observed";
    result.verification.frame_marker_limitations.push_back(
        "AV_FRAME_FLAG_CORRUPT 在项目锁定的 FFmpeg 8.1 可用解码器上实测恒 0；corrupt_frames=0 不能推出画面干净");

    const auto bodies = WriteReportsFor(dir, result);
    EXPECT_NE(bodies.json.find("frame_marker_evidence"), std::string::npos);
    EXPECT_NE(bodies.json.find("not_observed"), std::string::npos);
    for (const auto& body : {bodies.json, bodies.text, bodies.html})
        EXPECT_NE(body.find("corrupt_frames=0 不能推出画面干净"), std::string::npos)
            << "「损坏 0 = 干净」的误读必须被显式说明挡住";
    for (const auto& body : {bodies.text, bodies.html}) {
        EXPECT_NE(body.find("不能据此断言画面干净"), std::string::npos) << "not_observed 必须带否定说明";
        EXPECT_NE(body.find("帧标记证据"), std::string::npos);
    }
}

// ---------------- 未实现能力的边界契约 ----------------

// 覆盖守护：三条契约的四维度都必须有内容；顺序变了不会有编译错误，单独钉死。
TEST(RepairSafety, CapabilityBoundariesAreFullyDefined) {
    const auto& all = repair::AllCapabilityBoundaries();
    ASSERT_EQ(all.size(), 3u) << "缺 moov 重建 / 参数集恢复 / GOP 转码抢救必须各自成条";
    EXPECT_EQ(all[0].code, "moov_rebuild");
    EXPECT_EQ(all[1].code, "parameter_set_recovery");
    EXPECT_EQ(all[2].code, "decodable_gop_transcode");
    for (const auto& boundary : all) {
        EXPECT_EQ(boundary.status, "unsupported") << boundary.code;
        EXPECT_FALSE(boundary.preconditions.empty()) << boundary.code << " 缺前提";
        EXPECT_FALSE(boundary.unrecoverable_scope.empty()) << boundary.code << " 缺不可恢复范围";
        EXPECT_FALSE(boundary.timeline_impact.empty()) << boundary.code << " 缺时间轴变化";
        EXPECT_FALSE(boundary.quality_risks.empty()) << boundary.code << " 缺质量风险";
        EXPECT_FALSE(boundary.evidence.empty()) << boundary.code << " 缺判定依据";
        EXPECT_TRUE(boundary.trigger_findings.empty()) << "静态模板不得带触发证据";
    }
    EXPECT_STREQ(repair::BoundaryCodeForAction(RepairAction::S08_TolerantTranscode), "decodable_gop_transcode");
}

// 反向守护：没有事实依据时不得输出空壳契约；命中时 trigger_findings 要可追溯。
TEST(RepairSafety, CapabilityBoundariesOnlyAttachWhenEvidenceExists) {
    RepairDiagnosis empty;
    EXPECT_TRUE(repair::BuildCapabilityBoundaries(empty).empty()) << "无事实不得谎报未实现能力";

    DamageFinding moov;
    moov.code = "container.moov_missing";
    RepairDiagnosis only_moov;
    only_moov.findings.push_back(moov);
    const auto matched = repair::BuildCapabilityBoundaries(only_moov);
    ASSERT_EQ(matched.size(), 1u);
    EXPECT_EQ(matched[0].code, "moov_rebuild");
    ASSERT_EQ(matched[0].trigger_findings.size(), 1u);
    EXPECT_EQ(matched[0].trigger_findings[0], "container.moov_missing");

    DamageFinding a;
    a.code = "codec.parameter_sets_absent";
    DamageFinding b;
    b.code = "container.init_segment_missing";
    DamageFinding c;
    c.code = "decode.sample_failed";
    RepairDiagnosis multi;
    multi.findings = {a, b, c};
    const auto matched_multi = repair::BuildCapabilityBoundaries(multi);
    ASSERT_EQ(matched_multi.size(), 2u);
    EXPECT_EQ(matched_multi[0].code, "parameter_set_recovery");
    EXPECT_EQ(matched_multi[0].trigger_findings.size(), 2u);
    EXPECT_EQ(matched_multi[1].code, "decodable_gop_transcode");
    EXPECT_EQ(matched_multi[1].trigger_findings.size(), 1u);
}

TEST(RepairSafety, CapabilityBoundariesAreVisibleInAllReportFormats) {
    const std::string dir = MakeTempDir("boundary_reports");
    DamageFinding moov;
    moov.code = "container.moov_missing";
    RepairDiagnosis diagnosis;
    diagnosis.findings.push_back(moov);

    RepairResult result;
    result.status = RepairBusinessStatus::NeedsReference;
    result.diagnosis.capability_boundaries = repair::BuildCapabilityBoundaries(diagnosis);
    RepairActionPlan action;
    action.action = RepairAction::S08_TolerantTranscode;
    action.available = false;
    action.unavailable_reason = "需要解码-编码链与画质评估";
    action.capability_boundary = "decodable_gop_transcode";
    result.plan.actions.push_back(action);

    const auto bodies = WriteReportsFor(dir, result);
    EXPECT_NE(bodies.json.find("capability_boundaries"), std::string::npos);
    for (const auto& body : {bodies.json, bodies.text, bodies.html}) {
        EXPECT_NE(body.find("moov_rebuild"), std::string::npos) << "命中的契约必须三格式同源可见";
        EXPECT_NE(body.find("decodable_gop_transcode"), std::string::npos) << "规划层的契约引用必须可见";
    }
    for (const char* key : {"unrecoverable_scope", "timeline_impact", "quality_risks", "trigger_findings",
                            "container.moov_missing", "preconditions"})
        EXPECT_NE(bodies.json.find(key), std::string::npos) << key << " 必须进 JSON";
    for (const auto& body : {bodies.text, bodies.html}) {
        for (const char* label : {"前提: ", "不可恢复范围: ", "时间轴变化: ", "质量风险: ", "判定依据: "})
            EXPECT_NE(body.find(label), std::string::npos) << label << " 四个维度必须逐条展示";
        EXPECT_NE(body.find("边界契约: decodable_gop_transcode"), std::string::npos);
        EXPECT_NE(body.find("mdat 本身被截断"), std::string::npos) << "不可恢复范围必须写清物理边界";
    }
}

// 端到端：真实诊断必须把契约挂上，而不是只在纯函数里成立。
TEST(RepairSafety, RealRunsAttachMatchingCapabilityBoundaries) {
    const std::string dir = MakeTempDir("boundaries_real");

    const std::string moovless = dir + "/no_moov.mp4";
    ASSERT_TRUE(WriteMoovlessIsobmff(moovless));
    const RepairResult a = repair::RepairEngine::Run(MakeRequest(moovless, dir + "/out_a"), nullptr, nullptr);
    EXPECT_EQ(a.status, RepairBusinessStatus::NeedsReference) << a.summary;
    ASSERT_NE(FindBoundary(a.diagnosis, "moov_rebuild"), nullptr)
        << "缺 moov 的诊断必须挂载重建契约: " << a.diagnosis.summary;
    const auto bodies = WriteReportsFor(dir, a);
    for (const auto& body : {bodies.json, bodies.text, bodies.html})
        EXPECT_NE(body.find("moov_rebuild"), std::string::npos) << "端到端结果里契约必须可见";

    const auto frag = videoeye_test::CreateFragmentedTestMedia(dir, "frag_boundary");
    ASSERT_TRUE(frag.ok) << frag.error;
    const RepairResult b = repair::RepairEngine::Run(MakeRequest(frag.segment_path, dir + "/out_b"), nullptr, nullptr);
    const RepairCapabilityBoundary* params = FindBoundary(b.diagnosis, "parameter_set_recovery");
    ASSERT_NE(params, nullptr) << "缺初始化段必须挂载参数集恢复契约: " << b.diagnosis.summary;
    bool has_init_trigger = false;
    for (const auto& code : params->trigger_findings) {
        if (code == "container.init_segment_missing")
            has_init_trigger = true;
    }
    EXPECT_TRUE(has_init_trigger) << "契约必须写明是哪条证据触发的";
}
