#include <gtest/gtest.h>

// ============================================================
// 视频修复引擎回归（M1）
//
// 覆盖：
//   * 完整 MP4 → 保留码流重封装 → V2 全量解码 + 载荷哈希一致 → Repaired
//   * 产物（moov 前置）尾部截断 → 样本表越界 → 尾部抢救 → PartiallyRecovered
//   * 尾部整段缺失（moov 丢失）→ 准确报告 NeedsReference，不伪造产物
//   * fMP4 媒体分片缺初始化段 → NeedsReference；提供同源初始化段后 → Repaired
//   * 非媒体垃圾文件 → 如实报告无可恢复内容
//   * 目标路径 == 源文件 → 拒绝写入，源文件不变
//   * 取消标志 → Canceled，不发布产物
//   * 报告：JSON / TXT / HTML 三份落盘，JSON 含 schema_version 与字符串化 64 位字段
//
// 用 TestMediaFactory 现场生成确定性 H.264+AAC MP4，避免把二进制样本放进仓库。
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>

#include "core/domain/model/RepairRequest.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairExecutor.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairPlanner.h"
#include "core/repair/RepairUtils.h"
#include "core/reporting/RepairReportExporter.h"
#include "tests/support/TestMediaFactory.h"

bool HasResidualFinding(const videoeye::model::RepairResult& result, const std::string& code);

namespace {

using namespace videoeye;
using namespace videoeye::model;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_repair_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(dir);
    return dir.string();
}

uint64_t FileSize(const std::string& path) {
    std::error_code ec;
    return std::filesystem::file_size(path, ec);
}

// 只复制 prefix_ratio 比例的前缀字节，模拟"尾部被截断的媒体"。
bool CopyPrefix(const std::string& source, const std::string& target, double prefix_ratio) {
    std::ifstream in(source, std::ios::binary);
    if (!in)
        return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t keep = static_cast<std::size_t>(bytes.size() * prefix_ratio);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(bytes.data(), static_cast<std::streamsize>(keep));
    return static_cast<bool>(out);
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool HasFinding(const RepairResult& result, const std::string& code) {
    for (const auto& finding : result.diagnosis.findings) {
        if (finding.code == code)
            return true;
    }
    return false;
}

std::string CheckDetail(const RepairResult& result, const std::string& name) {
    for (const auto& check : result.diagnosis.checks) {
        if (check.name == name)
            return check.detail;
    }
    return {};
}

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir,
                          RepairVerificationLevel level = RepairVerificationLevel::V2_FullDecode) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    request.verification_level = level;
    request.write_report = true;
    return request;
}

uint32_t ReadBe32(const std::vector<char>& bytes, std::size_t offset) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3]));
}

void WriteBe32(std::vector<char>& bytes, std::size_t offset, uint32_t value) {
    bytes[offset] = static_cast<char>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<char>(value & 0xFF);
}

// 在文件头垫一个同长度的 free 盒：盒结构合法，但 mdat 整体后移，
// stco/co64 里的 chunk 偏移因此全部指向了真实数据之前的位置。
// 这是"数据还在、索引错位"的最小可控形态（整体位移可以被解释与重建）。
bool PrependFreeBox(const std::string& source, const std::string& target, uint32_t pad) {
    if (pad < 8)
        return false;
    std::ifstream in(source, std::ios::binary);
    if (!in)
        return false;
    std::vector<char> body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<char> out(static_cast<std::size_t>(pad), '\0');
    WriteBe32(out, 0, pad);
    out[4] = 'f';
    out[5] = 'r';
    out[6] = 'e';
    out[7] = 'e';
    out.insert(out.end(), body.begin(), body.end());
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(file);
}

bool HasLimitationContaining(const RepairResult& result, const std::string& needle) {
    for (const auto& limitation : result.fidelity.limitations) {
        if (limitation.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// 去掉指定类型的顶层盒（保留其余字节），用来构造"moov 丢失、mdat 仍在"的样本。
bool StripTopLevelBox(const std::string& source, const std::string& target, const std::string& box_type) {
    std::ifstream in(source, std::ios::binary);
    if (!in)
        return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<char> out;
    std::size_t pos = 0;
    bool removed = false;
    while (pos + 8 <= bytes.size()) {
        uint32_t size = ReadBe32(bytes, pos);
        const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(pos) + 4,
                               bytes.begin() + static_cast<std::ptrdiff_t>(pos) + 8);
        std::size_t box_size = size;
        if (size == 1) {
            if (pos + 16 > bytes.size())
                return false;
            box_size = (static_cast<std::size_t>(ReadBe32(bytes, pos + 8)) << 32) | ReadBe32(bytes, pos + 12);
        }
        if (box_size < 8 || pos + box_size > bytes.size())
            return false;
        if (type == box_type) {
            removed = true;
        } else {
            out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                       bytes.begin() + static_cast<std::ptrdiff_t>(pos + box_size));
        }
        pos += box_size;
    }
    if (!removed)
        return false;
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(file);
}

// 把每条轨的 stco 中同一比例的条目改成远超文件大小的偏移，模拟"录制中途
// 存储区域损坏、但文件结构与后续数据仍在"。
//
// 必须两条轨都改：只坏一条轨时，demuxer 会让该轨提前 EOF，另一条轨仍能读到
// 文件尾，时间轴并未真正中断，无法验证"整段中断 → 跳段恢复"的行为。
bool PatchStcoChunkOffset(const std::string& source, const std::string& target, double ratio,
                          std::string* error = nullptr) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        if (error)
            *error = "open failed";
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // stco 布局: size(4) + type(4) + version/flags(4) + entry_count(4) + entries...
    std::size_t patched_tables = 0;
    std::string detail;
    std::size_t offset = 0;
    while (offset + 8 <= bytes.size()) {
        if (offset + 4 > bytes.size())
            break;
        const std::size_t type_offset = offset + 4;
        if (type_offset + 12 <= bytes.size() && bytes[type_offset] == 's' && bytes[type_offset + 1] == 't' &&
            bytes[type_offset + 2] == 'c' && bytes[type_offset + 3] == 'o') {
            const uint32_t size = ReadBe32(bytes, offset);
            const uint32_t entry_count = ReadBe32(bytes, type_offset + 8);
            if (size >= 16 && offset + size <= bytes.size() && entry_count >= 4 &&
                offset + 16 + static_cast<std::size_t>(entry_count) * 4 <= bytes.size()) {
                std::size_t index = static_cast<std::size_t>(entry_count * ratio);
                if (index >= entry_count)
                    index = entry_count - 1;
                WriteBe32(bytes, offset + 16 + index * 4, 0x7F000000u);
                ++patched_tables;
                detail += " stco#" + std::to_string(patched_tables) + "(count=" + std::to_string(entry_count) +
                          ",index=" + std::to_string(index) + ")";
                offset += size;
                continue;
            }
        }
        ++offset;
    }
    if (patched_tables == 0) {
        if (error)
            *error = "stco not found, bytes=" + std::to_string(bytes.size());
        return false;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error)
            *error = "output open failed: " + target;
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (error)
            *error = "write failed";
        return false;
    }
    if (error)
        *error = "ok tables=" + std::to_string(patched_tables) + detail;
    return true;
}

} // namespace

TEST(RepairEngine, IntactMp4IsRemuxedAndVerified) {
    const std::string dir = MakeTempDir("intact");
    const auto media = videoeye_test::CreateTestMedia(dir, "intact_src");
    ASSERT_TRUE(media.ok) << media.error;
    ASSERT_GT(media.frame_count, 0);

    const std::string output_dir = dir + "/out";
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(media.path, output_dir), nullptr, nullptr);

    ASSERT_EQ(result.status, RepairBusinessStatus::Repaired)
        << "summary=" << result.summary << " error=" << result.error;
    EXPECT_TRUE(result.output_published);
    ASSERT_FALSE(result.output_path.empty());
    EXPECT_TRUE(std::filesystem::exists(result.output_path));
    EXPECT_GT(FileSize(result.output_path), 0u);

    EXPECT_EQ(result.verification.status, RepairCheckStatus::Passed);
    EXPECT_GT(result.verification.decoded_frames, 0u);
    EXPECT_EQ(result.verification.seek_passes, result.verification.seek_checks);
    EXPECT_EQ(result.fidelity.video, RepairFidelityStatus::VerifiedMatch);
    EXPECT_EQ(result.fidelity.audio, RepairFidelityStatus::VerifiedMatch);
    EXPECT_FALSE(result.fidelity.video_reencoded);
    EXPECT_FALSE(result.fidelity.timeline_modified);
    EXPECT_EQ(result.fidelity.timeline_status, "unchanged")
        << "没有借用任何外部解码参数 / 时基时，时间轴结论才可以是 unchanged";
    EXPECT_EQ(result.fidelity.content_completeness, "complete_for_selected_checks");
    EXPECT_TRUE(result.verification.av_sync_checked);
    EXPECT_EQ(result.verification.av_sync_status, "within_tolerance");
    ASSERT_TRUE(result.fidelity.original_content_recovery_ratio.has_value());
    EXPECT_NEAR(*result.fidelity.original_content_recovery_ratio, 1.0, 0.05);
    // 诊断不再只看开头：完整媒体应完成首/中/尾三个抽样点。
    const std::string decode_detail = CheckDetail(result, "decode.sample");
    EXPECT_NE(decode_detail.find("首段"), std::string::npos) << decode_detail;
    EXPECT_NE(decode_detail.find("中段"), std::string::npos) << decode_detail;
    EXPECT_NE(decode_detail.find("尾段"), std::string::npos) << decode_detail;

    ASSERT_FALSE(result.executions.empty());
    EXPECT_EQ(result.executions.front().action, RepairAction::S01_Remux);
    EXPECT_TRUE(result.executions.front().succeeded);
    EXPECT_GT(result.executions.front().packets_written, 0u);

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
    EXPECT_NEAR(probe.duration_seconds, media.duration_seconds, 0.25);

    // ---- 报告三件套 ----
    ASSERT_FALSE(result.report_json_path.empty());
    const reporting::RepairReportWriteResult written = reporting::RepairReportExporter::WriteAll(
        result, result.report_json_path, result.report_text_path, result.report_html_path);
    EXPECT_TRUE(written.AllOk()) << written.error;
    EXPECT_TRUE(std::filesystem::exists(result.report_json_path));
    EXPECT_TRUE(std::filesystem::exists(result.report_text_path));
    EXPECT_TRUE(std::filesystem::exists(result.report_html_path));

    const std::string json = ReadText(result.report_json_path);
    EXPECT_NE(json.find("\"schema_version\": \"1.0\""), std::string::npos);
    EXPECT_NE(json.find("\"status\": \"repaired\""), std::string::npos);
    // 64 位字段以字符串写出，避免下游按 double 截断。
    EXPECT_NE(json.find("\"size_bytes\": \""), std::string::npos);
    EXPECT_NE(json.find("\"bytes_scanned\": \""), std::string::npos);
}

TEST(RepairEngine, TruncatedTailIsSalvagedAsPartial) {
    const std::string dir = MakeTempDir("tail");
    const auto media = videoeye_test::CreateTestMedia(dir, "tail_src");
    ASSERT_TRUE(media.ok) << media.error;

    // 第一次先做一次完整重封装：产物按 faststart 布局写（moov 在前）。
    // 然后截断它的尾部，就得到"索引还在、尾样本越过文件尾"的真实损坏样本。
    const std::string first_out = dir + "/first";
    const RepairResult first = repair::RepairEngine::Run(MakeRequest(media.path, first_out), nullptr, nullptr);
    ASSERT_EQ(first.status, RepairBusinessStatus::Repaired) << first.summary;
    ASSERT_TRUE(first.output_published);

    const std::string truncated = dir + "/truncated.mp4";
    ASSERT_TRUE(CopyPrefix(first.output_path, truncated, 0.70));
    ASSERT_GT(FileSize(truncated), 0u);

    const RepairRequest request = MakeRequest(truncated, dir + "/second", RepairVerificationLevel::V1_FrameSample);
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);

    EXPECT_TRUE(result.diagnosis.openable);
    EXPECT_TRUE(result.diagnosis.moov_present);
    EXPECT_TRUE(HasFinding(result, "track.sample_out_of_bounds") || HasFinding(result, "container.box_out_of_bounds"))
        << result.diagnosis.summary;
    EXPECT_TRUE(result.plan.salvage_end_seconds.has_value());

    EXPECT_TRUE(result.output_published) << "summary=" << result.summary << " error=" << result.error;
    EXPECT_TRUE(result.status == RepairBusinessStatus::PartiallyRecovered ||
                result.status == RepairBusinessStatus::Repaired)
        << "status=" << ToString(result.status) << " summary=" << result.summary;
    EXPECT_EQ(result.fidelity.content_completeness.empty() ? "" : result.fidelity.content_completeness, "partial");
    EXPECT_TRUE(result.verification.status == RepairCheckStatus::Passed ||
                result.verification.status == RepairCheckStatus::Inconclusive);
    EXPECT_GT(result.verification.decoded_frames, 0u);
    ASSERT_TRUE(result.fidelity.original_content_recovery_ratio.has_value());
    EXPECT_LT(*result.fidelity.original_content_recovery_ratio, 1.0) << "尾部被截断时恢复比例不能报 100%";

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened);
    EXPECT_GT(probe.frame_count, 0);
    EXPECT_LT(probe.duration_seconds, media.duration_seconds + 0.05);
}

TEST(RepairEngine, OutputByteCapPublishesPlayablePrefix) {
    const std::string dir = MakeTempDir("cap");
    const auto media = videoeye_test::CreateTestMedia(dir, "cap_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out", RepairVerificationLevel::V1_FrameSample);
    request.limits.max_output_bytes = std::max<uint64_t>(FileSize(media.path) / 3, 4 * 1024);
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);

    EXPECT_TRUE(result.output_published) << "summary=" << result.summary << " error=" << result.error;
    EXPECT_TRUE(std::filesystem::exists(result.output_path));
    EXPECT_GT(FileSize(result.output_path), 0u);
    EXPECT_EQ(result.status, RepairBusinessStatus::PartiallyRecovered) << "summary=" << result.summary;
    EXPECT_EQ(result.fidelity.content_completeness, "partial");
    EXPECT_TRUE(HasResidualFinding(result, "output.size_capped"));
    ASSERT_TRUE(result.fidelity.original_content_recovery_ratio.has_value());
    EXPECT_LT(*result.fidelity.original_content_recovery_ratio, 1.0);

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
}

TEST(RepairEngine, MissingMoovIsReportedAsNeedsReference) {
    const std::string dir = MakeTempDir("moov");
    const auto media = videoeye_test::CreateTestMedia(dir, "moov_src");
    ASSERT_TRUE(media.ok) << media.error;

    // TestMediaFactory 的 MP4 采用默认布局（moov 在文件尾）；截掉尾部即同时丢掉 moov。
    const std::string broken = dir + "/no_moov.mp4";
    ASSERT_TRUE(CopyPrefix(media.path, broken, 0.35));

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(broken, dir + "/out"), nullptr, nullptr);

    EXPECT_TRUE(HasFinding(result, "container.moov_missing") || HasFinding(result, "container.structure_unparsable"))
        << result.diagnosis.summary;
    EXPECT_EQ(result.status, RepairBusinessStatus::NeedsReference) << ToString(result.status);
    EXPECT_FALSE(result.output_published);
    EXPECT_TRUE(result.output_path.empty());
    EXPECT_FALSE(result.plan.feasible);
    EXPECT_FALSE(result.diagnosis.blocking_reasons.empty());
}

// ============================================================
// 真实语料修复回归（阶段一验收：真实 ffmpeg 产物 + 真实损伤）
//
// 上面几个用例的"健康原片"由 TestMediaFactory 现场生成；这里换成 ffmpeg 真实
// muxer 写出的 faststart MP4（moov 前置、含 B 帧与 AAC），再由
// tests/corpus/generate_real_corpus.py 派生三种损伤。两者实现路径不同，
// 能照出"只对测试工厂产物成立"的假设。
// ============================================================
namespace {

std::string RealCorpusSample(const char* name) {
    return (std::filesystem::path(VIDEOEYE_CONTAINER_CORPUS_DIR) / name).string();
}

} // namespace

TEST(RepairEngine, RealCorpusHealthyMp4RemuxesCleanly) {
    const std::string source = RealCorpusSample("real_h264_aac_faststart.mp4");
    if (!std::filesystem::exists(source)) {
        GTEST_SKIP() << "缺真实样本 real_h264_aac_faststart.mp4，先跑 tests/corpus/generate_real_corpus.py";
    }

    const std::string dir = MakeTempDir("real_healthy");
    const RepairResult result = repair::RepairEngine::Run(
        MakeRequest(source, dir + "/out", RepairVerificationLevel::V1_FrameSample), nullptr, nullptr);

    EXPECT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_TRUE(std::filesystem::exists(result.output_path));
    EXPECT_TRUE(result.status == RepairBusinessStatus::Repaired ||
                result.status == RepairBusinessStatus::PartiallyRecovered)
        << ToString(result.status);
    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << result.output_path;
    EXPECT_GT(probe.frame_count, 0);
}

TEST(RepairEngine, RealCorpusTailTruncatedMp4RecoversAfterDamage) {
    const std::string source = RealCorpusSample("real_h264_aac_tail_truncated.mp4");
    if (!std::filesystem::exists(source)) {
        GTEST_SKIP() << "缺真实样本 real_h264_aac_tail_truncated.mp4，先跑 tests/corpus/generate_real_corpus.py";
    }

    const std::string dir = MakeTempDir("real_tail");
    const RepairResult result = repair::RepairEngine::Run(
        MakeRequest(source, dir + "/out", RepairVerificationLevel::V1_FrameSample), nullptr, nullptr);

    EXPECT_TRUE(result.diagnosis.openable);
    EXPECT_TRUE(result.diagnosis.moov_present);
    EXPECT_TRUE(HasFinding(result, "track.sample_out_of_bounds") || HasFinding(result, "container.box_out_of_bounds"))
        << result.diagnosis.summary;
    EXPECT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_TRUE(result.status == RepairBusinessStatus::PartiallyRecovered ||
                result.status == RepairBusinessStatus::Repaired)
        << ToString(result.status);
    EXPECT_GT(result.verification.decoded_frames, 0u);
    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << result.output_path;
    EXPECT_GT(probe.frame_count, 0);
}

TEST(RepairEngine, RealCorpusMissingMoovMp4NeedsReference) {
    const std::string source = RealCorpusSample("real_h264_aac_missing_moov.mp4");
    if (!std::filesystem::exists(source)) {
        GTEST_SKIP() << "缺真实样本 real_h264_aac_missing_moov.mp4，先跑 tests/corpus/generate_real_corpus.py";
    }

    const std::string dir = MakeTempDir("real_moov");
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(source, dir + "/out"), nullptr, nullptr);

    EXPECT_TRUE(HasFinding(result, "container.moov_missing") || HasFinding(result, "container.structure_unparsable"))
        << result.diagnosis.summary;
    EXPECT_EQ(result.status, RepairBusinessStatus::NeedsReference) << ToString(result.status);
    EXPECT_FALSE(result.output_published);
    EXPECT_TRUE(result.output_path.empty());
}

// 中段 chunk offset 越界：必须跳段续采，产物至少两段 —— 只保留前半段是本轮修复前的缺陷。
TEST(RepairEngine, RealCorpusMidFileDamageResumesIntoSegments) {
    const std::string source = RealCorpusSample("real_h264_aac_mid_damaged.mp4");
    if (!std::filesystem::exists(source)) {
        GTEST_SKIP() << "缺真实样本 real_h264_aac_mid_damaged.mp4，先跑 tests/corpus/generate_real_corpus.py";
    }

    const std::string dir = MakeTempDir("real_mid");
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(source, dir + "/out"), nullptr, nullptr);

    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_EQ(result.status, RepairBusinessStatus::PartiallyRecovered) << ToString(result.status);
    ASSERT_GE(result.recovered_segments.size(), 2u) << result.summary;
    EXPECT_FALSE(result.recovered_segments.front().resumed_after_damage);
    EXPECT_TRUE(result.recovered_segments.back().resumed_after_damage);
    EXPECT_GT(result.recovered_segments.back().skipped_before_seconds, 0.0);

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << result.output_path;
    EXPECT_GT(probe.frame_count, 0);
}

TEST(RepairEngine, FragmentedSegmentsWithoutInitNeedReference) {
    const std::string dir = MakeTempDir("fmp4_noref");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "fmp4");
    ASSERT_TRUE(media.ok) << media.error;

    // 只有 moof/mdat 分片、没有 ftyp+moov：FFmpeg 打不开，也不能凭空重建解码参数。
    const RepairResult result =
        repair::RepairEngine::Run(MakeRequest(media.segment_path, dir + "/out"), nullptr, nullptr);

    EXPECT_TRUE(HasFinding(result, "container.init_segment_missing")) << result.diagnosis.summary;
    EXPECT_EQ(result.status, RepairBusinessStatus::NeedsReference) << ToString(result.status);
    EXPECT_FALSE(result.plan.feasible);
    EXPECT_FALSE(result.output_published);
    EXPECT_TRUE(result.output_path.empty());
}

// P1-8：没有参考样片时，从分片 in-band SPS/PPS 与 sidx 时基自建 ftyp+moov。
// 样本必须是"视频轨 + global_sidx + 参数集在码流里"，三者缺一都只能拒绝。
TEST(RepairEngine, FragmentedSegmentsSynthesizeInitFromInBandParameterSets) {
    const std::string dir = MakeTempDir("fmp4_synth");
    videoeye_test::TestMediaSpec spec;
    spec.with_audio = false;
    spec.repeat_parameter_sets = true;
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "synth", spec, true);
    ASSERT_TRUE(media.ok) << media.error;

    const RepairResult result = repair::RepairEngine::Run(
        MakeRequest(media.segment_path, dir + "/out", RepairVerificationLevel::V1_FrameSample), nullptr, nullptr);

    // 自建初始化段没有同源参考段可交叉验证，结论只能是 PartiallyRecovered。
    std::string blocking;
    for (const auto& reason : result.diagnosis.blocking_reasons)
        blocking += "[" + reason + "]";
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error << " | " << blocking;
    ASSERT_EQ(result.status, RepairBusinessStatus::PartiallyRecovered)
        << ToString(result.status) << " summary=" << result.summary;
    EXPECT_TRUE(HasResidualFinding(result, "init_segment.synthesized")) << "自建来源必须作为残留问题上报";
    EXPECT_EQ(result.fidelity.reference_compatibility, "synthesized");
    EXPECT_EQ(result.fidelity.timeline_status, "unchanged");

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
    EXPECT_GT(probe.width, 0);
}

// 反向守护：没有 sidx 时不能猜时基 —— 猜错会保留全部帧却改变播放速度。
TEST(RepairEngine, FragmentedSegmentsWithoutSidxStillNeedReference) {
    const std::string dir = MakeTempDir("fmp4_nosidx");
    videoeye_test::TestMediaSpec spec;
    spec.with_audio = false;
    spec.repeat_parameter_sets = true;
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "nosidx", spec, false);
    ASSERT_TRUE(media.ok) << media.error;

    const RepairResult result =
        repair::RepairEngine::Run(MakeRequest(media.segment_path, dir + "/out"), nullptr, nullptr);
    EXPECT_EQ(result.status, RepairBusinessStatus::NeedsReference) << ToString(result.status);
    EXPECT_FALSE(result.output_published);
    EXPECT_FALSE(result.diagnosis.blocking_reasons.empty());
    bool mentions_timescale = false;
    for (const auto& reason : result.diagnosis.blocking_reasons) {
        if (reason.find("timescale") != std::string::npos || reason.find("时基") != std::string::npos)
            mentions_timescale = true;
    }
    EXPECT_TRUE(mentions_timescale) << "拒绝原因必须指向缺失的时基证据";
}

// P1-9：文件头被垫入字节后 chunk 偏移整体错位，但 mdat 载荷仍完整。
// 执行前重建索引应恢复可读性，而不是把整份文件当成尾部截断只抢救前缀。
TEST(RepairEngine, ShiftedChunkOffsetsAreRebuiltBeforeRemux) {
    const std::string dir = MakeTempDir("index_rebuild");
    const auto media = videoeye_test::CreateTestMedia(dir, "index_src");
    ASSERT_TRUE(media.ok) << media.error;

    const std::string broken = dir + "/shifted.mp4";
    ASSERT_TRUE(PrependFreeBox(media.path, broken, 1024u));

    const RepairResult result = repair::RepairEngine::Run(
        MakeRequest(broken, dir + "/out", RepairVerificationLevel::V1_FrameSample), nullptr, nullptr);

    EXPECT_TRUE(HasFinding(result, "track.sample_table_inconsistent")) << result.diagnosis.summary;
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_TRUE(result.status == RepairBusinessStatus::Repaired ||
                result.status == RepairBusinessStatus::PartiallyRecovered)
        << ToString(result.status);
    EXPECT_TRUE(HasLimitationContaining(result, "重建")) << "索引重建必须写进报告";
    EXPECT_TRUE(result.diagnosis.findings.empty() == false);

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
    EXPECT_NEAR(probe.duration_seconds, media.duration_seconds, 0.30);
}

// 反向守护：索引没坏时不得触发重建（避免每次修复都整份拷贝一遍）。
TEST(RepairEngine, IntactIndexDoesNotTriggerRebuild) {
    const std::string dir = MakeTempDir("index_noop");
    const auto media = videoeye_test::CreateTestMedia(dir, "noop_src");
    ASSERT_TRUE(media.ok) << media.error;

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out"), nullptr, nullptr);
    EXPECT_FALSE(HasFinding(result, "track.sample_table_inconsistent"));
    EXPECT_FALSE(HasLimitationContaining(result, "重建"));
    ASSERT_EQ(result.status, RepairBusinessStatus::Repaired) << result.summary;
}

TEST(RepairEngine, FragmentedSegmentsRepairedWithReferenceInit) {
    const std::string dir = MakeTempDir("fmp4_ref");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "fmp4");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.segment_path, dir + "/out");
    request.reference_path = media.init_path;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);

    // R03 结论口径：测试工厂生成的分片序列不带 sidx，源侧没有独立时基证据，
    // 参考段只能标 unverified。产物仍然完整可解码，但「借来的时基是否改了播放时长」
    // 无法证明，所以业务状态必须是 PartiallyRecovered，不能再报 Repaired。
    // 这里不是放宽断言：下面几条把「不确定」的落地范围一起钉死。
    ASSERT_EQ(result.status, RepairBusinessStatus::PartiallyRecovered)
        << "summary=" << result.summary << " error=" << result.error;
    EXPECT_EQ(result.fidelity.reference_compatibility, "unverified");
    EXPECT_EQ(result.fidelity.timeline_status, "unknown");
    EXPECT_EQ(result.fidelity.content_completeness, "unknown");
    EXPECT_FALSE(result.fidelity.timeline_modified) << "引擎确实没有主动重写时间戳，但这不等于时间轴未变";
    bool has_unverified_reference = false;
    for (const auto& finding : result.residual_findings) {
        if (finding.code == "reference.parameters_unverified")
            has_unverified_reference = true;
    }
    EXPECT_TRUE(has_unverified_reference) << "未核验的借用参数必须作为残留问题上报";

    EXPECT_TRUE(result.output_published);
    ASSERT_FALSE(result.output_path.empty());
    EXPECT_TRUE(std::filesystem::exists(result.output_path));
    EXPECT_EQ(result.verification.status, RepairCheckStatus::Passed);
    EXPECT_GT(result.verification.decoded_frames, 0u);
    EXPECT_EQ(result.fidelity.video, RepairFidelityStatus::VerifiedMatch);

    ASSERT_FALSE(result.executions.empty());
    EXPECT_EQ(result.executions.front().action, RepairAction::S01_Remux);
    EXPECT_TRUE(result.executions.front().succeeded);

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
    EXPECT_NEAR(probe.duration_seconds, 1.6, 0.3);
}

TEST(RepairEngine, GarbageFileHasNoRecoverableContent) {
    const std::string dir = MakeTempDir("garbage");
    const std::string junk = videoeye_test::CreateCorruptMediaFile(dir, "garbage.mp4");
    ASSERT_FALSE(junk.empty());

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(junk, dir + "/out"), nullptr, nullptr);

    EXPECT_FALSE(result.output_published);
    EXPECT_EQ(result.status, RepairBusinessStatus::NoRecoverableContent) << ToString(result.status);
    EXPECT_FALSE(result.diagnosis.openable);
}

TEST(RepairEngine, RefusesToOverwriteSourcePath) {
    const std::string dir = MakeTempDir("samepath");
    const auto media = videoeye_test::CreateTestMedia(dir, "same_src");
    ASSERT_TRUE(media.ok) << media.error;
    const uint64_t before = FileSize(media.path);

    RepairRequest request = MakeRequest(media.path, dir);
    request.output_path = media.path; // 显式指回源文件
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);

    EXPECT_EQ(result.status, RepairBusinessStatus::Failed);
    EXPECT_NE(result.error.find("拒绝"), std::string::npos) << result.error;
    EXPECT_FALSE(result.output_published);
    EXPECT_EQ(FileSize(media.path), before);
}

TEST(RepairEngine, CancelBeforeExecutionLeavesNoArtifacts) {
    const std::string dir = MakeTempDir("cancel");
    const auto media = videoeye_test::CreateTestMedia(dir, "cancel_src");
    ASSERT_TRUE(media.ok) << media.error;

    std::atomic<bool> cancel{false};
    auto progress = [&cancel](int, const std::string&, const std::string&) { cancel.store(true); };
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out"), &cancel, progress);

    EXPECT_EQ(result.status, RepairBusinessStatus::Canceled) << ToString(result.status);
    EXPECT_FALSE(result.output_published);
    EXPECT_TRUE(result.output_path.empty());
    const std::filesystem::path out_dir = std::filesystem::path(dir) / "out";
    if (std::filesystem::exists(out_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(out_dir)) {
            EXPECT_NE(entry.path().extension().string(), ".part");
        }
    }
}

// 在末尾追加一个孤立的 styp（24 字节，没有 moof/mdat），模拟下载中断留下的残段。
// 这是纯结构破坏，不改动任何已有分片的载荷，便于验证"不完整分片"检查的判别力。
bool AppendOrphanStyp(const std::string& source, const std::string& target) {
    std::ifstream in(source, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty())
        return false;
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const unsigned char stub[24] = {0, 0, 0, 24, 's', 't', 'y', 'p', 'm', 's', 'd', 'h',
                                    0, 0, 0, 0,  'm', 's', 'd', 'h', 'm', 's', 'i', 'x'};
    out.write(reinterpret_cast<const char*>(stub), sizeof(stub));
    return static_cast<bool>(out);
}

const RepairCheck* FindCheck(const RepairDiagnosis& diagnosis, const std::string& name) {
    for (const auto& check : diagnosis.checks) {
        if (check.name == name)
            return &check;
    }
    return nullptr;
}

// 分片序列必须给出**实测**证据：以前只写一句"分片里没有解码参数"的假设，
// 计数还把 traf 数当成分片数；覆盖率也把盒大小之和虚报成"已扫描字节"。
TEST(RepairEngine, FragmentedSegmentsProduceMeasuredForensics) {
    const std::string dir = MakeTempDir("forensics");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "forensics");
    ASSERT_TRUE(media.ok) << media.error;

    const RepairResult result =
        repair::RepairEngine::Run(MakeRequest(media.segment_path, dir + "/out"), nullptr, nullptr);
    EXPECT_EQ(result.status, RepairBusinessStatus::NeedsReference) << ToString(result.status);

    EXPECT_TRUE(HasFinding(result, "container.fragment_sequence")) << result.diagnosis.summary;
    // TestMediaFactory 把媒体分片写成 .ts，正好覆盖"扩展名与内容不符"。
    EXPECT_TRUE(HasFinding(result, "container.extension_mismatch"));

    ASSERT_FALSE(result.diagnosis.expected_tracks.empty());
    uint64_t declared = 0;
    for (const auto& track : result.diagnosis.expected_tracks) {
        EXPECT_GT(track.sample_count, 0u);
        EXPECT_NE(track.provenance.find("moof/trun"), std::string::npos) << track.provenance;
        declared += track.sample_count;
    }
    EXPECT_EQ(result.diagnosis.coverage.packets_scanned, declared);
    EXPECT_GT(result.diagnosis.coverage.bytes_scanned, 0u);
    EXPECT_TRUE(result.diagnosis.coverage.reached_eof);
    for (const auto& limitation : result.diagnosis.coverage.limitations)
        EXPECT_EQ(limitation.find("未逐字节扫描"), std::string::npos) << limitation;

    const RepairCheck* integrity = FindCheck(result.diagnosis, "container.fragment_integrity");
    ASSERT_NE(integrity, nullptr);
    EXPECT_EQ(integrity->status, RepairCheckStatus::Passed) << integrity->detail;
    EXPECT_NE(FindCheck(result.diagnosis, "codec.in_band_parameter_sets"), nullptr);
    EXPECT_NE(FindCheck(result.diagnosis, "timing.fragment_timeline"), nullptr);
    // 覆盖守护：取证器若退化成"什么 NAL 都没扫"，上面的数字仍可能填得出来。
    bool nal_evidence = false;
    for (const auto& finding : result.diagnosis.findings) {
        if (finding.code.rfind("codec.", 0) == 0 && finding.evidence.find("扫描") != std::string::npos)
            nal_evidence = true;
    }
    EXPECT_TRUE(nal_evidence) << "必须给出逐 NAL 实测证据，而不是一句硬编码假设";

    bool has_spec = false;
    for (const auto& reason : result.diagnosis.blocking_reasons) {
        if (reason.find("重建初始化段所需规格") != std::string::npos)
            has_spec = true;
    }
    EXPECT_TRUE(has_spec) << "结论必须写明需要什么规格的初始化段";
    EXPECT_NE(result.diagnosis.summary.find("实测"), std::string::npos) << result.diagnosis.summary;
}

// 反向守护：末段只剩 styp 时必须精确报"结构不完整"，且不能把整份文件
// 误判成"没有可恢复内容"（前面的分片仍然是好的）。
TEST(RepairEngine, OrphanTailSegmentIsDetectedWithoutLosingTheRest) {
    const std::string dir = MakeTempDir("orphan");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "orphan");
    ASSERT_TRUE(media.ok) << media.error;
    const std::string broken = dir + "/orphan_tail.ts";
    ASSERT_TRUE(AppendOrphanStyp(media.segment_path, broken));

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(broken, dir + "/out"), nullptr, nullptr);

    EXPECT_EQ(result.status, RepairBusinessStatus::NeedsReference) << ToString(result.status);
    EXPECT_TRUE(HasFinding(result, "container.fragment_incomplete")) << result.diagnosis.summary;
    const RepairCheck* integrity = FindCheck(result.diagnosis, "container.fragment_integrity");
    ASSERT_NE(integrity, nullptr);
    EXPECT_EQ(integrity->status, RepairCheckStatus::Failed) << integrity->detail;
    EXPECT_FALSE(result.diagnosis.expected_tracks.empty());
}

// 源侧样本守恒的阈值必须被钉死：这条降级判据在"一切正常"时永远不触发，
// 一旦写错也不会有任何测试变红，属于典型的"坏了看不出来"的机制。
TEST(RepairEngine, SampleConservationThresholdRejectsRealLossOnly) {
    // 没有源侧基线（普通 MP4 路径）时绝不误报。
    EXPECT_FALSE(repair::SampleLossExceedsTolerance(0, 0));
    EXPECT_FALSE(repair::SampleLossExceedsTolerance(0, 100));
    // 完整或更多写入包数都不算缺失。
    EXPECT_FALSE(repair::SampleLossExceedsTolerance(1000, 1000));
    EXPECT_FALSE(repair::SampleLossExceedsTolerance(1000, 1200));
    // 5% 容差：允许容器合并/拆分少量访问单元，超过才降级。
    EXPECT_FALSE(repair::SampleLossExceedsTolerance(1000, 960));
    EXPECT_FALSE(repair::SampleLossExceedsTolerance(1000, 950));
    EXPECT_TRUE(repair::SampleLossExceedsTolerance(1000, 949));
    EXPECT_TRUE(repair::SampleLossExceedsTolerance(100, 0));
    // 大数走加法而非浮点乘法，不能溢出。
    EXPECT_TRUE(repair::SampleLossExceedsTolerance(UINT64_MAX, 0));
}

// 手工真实样本诊断入口（只读）：设置 VE_REPAIR_REAL_SAMPLE=<path> 后单独运行本用例，
// 打印分层诊断证据。常规 ctest 下未设置环境变量会直接跳过，不产生任何文件。
//
//   VE_REPAIR_REAL_SAMPLE=D:/.../sample.ts \
//       test_repair_engine --gtest_filter=RepairEngine.ManualRealSampleInspection
TEST(RepairEngine, ManualRealSampleInspection) {
    const char* sample = std::getenv("VE_REPAIR_REAL_SAMPLE");
    if (sample == nullptr || *sample == '\0')
        GTEST_SKIP() << "设置 VE_REPAIR_REAL_SAMPLE 指向真实样本后重跑本用例";

    // VE_REPAIR_REAL_REFERENCE 可选：给一份（可能不匹配的）初始化段，跑完整修复
    // 链路，用来验证"源侧样本守恒"能不能挡住 demuxer 的静默丢包。
    const char* reference = std::getenv("VE_REPAIR_REAL_REFERENCE");
    const bool with_reference = reference != nullptr && *reference != '\0';

    RepairRequest request;
    request.source_path = sample;
    request.output_directory = std::filesystem::temp_directory_path().string();
    request.write_report = false;
    request.verification_level = RepairVerificationLevel::V0_Container;
    if (with_reference) {
        request.reference_path = reference;
        const auto out_dir = std::filesystem::temp_directory_path() / "videoeye_real_repair";
        std::filesystem::create_directories(out_dir);
        request.output_directory = out_dir.string();
    }

    const auto started = std::chrono::steady_clock::now();
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::cout << "\n===== REAL SAMPLE INSPECTION =====\n"
              << "source: " << sample << "\n"
              << "elapsed_s: " << seconds << "\n"
              << "status: " << ToString(result.status) << "\n"
              << "summary: " << result.summary << "\n"
              << "error: " << result.error << "\n"
              << "openable=" << result.diagnosis.openable << " container_known=" << result.diagnosis.container_known
              << " moov_present=" << result.diagnosis.moov_present << " fragmented=" << result.diagnosis.fragmented
              << "\n"
              << "input: size=" << result.input.size_bytes << " signature=" << result.input.signature << "\n"
              << "coverage: bytes=" << result.diagnosis.coverage.bytes_scanned
              << " packets=" << result.diagnosis.coverage.packets_scanned
              << " reached_eof=" << result.diagnosis.coverage.reached_eof << "\n";
    for (const auto& check : result.diagnosis.checks) {
        std::cout << "CHECK [" << ToString(check.status) << "] " << check.name << ": " << check.detail << " ("
                  << check.cost_ms << " ms)\n";
    }
    for (const auto& finding : result.diagnosis.findings) {
        std::cout << "FINDING " << finding.code << " [" << ToString(finding.layer) << "/"
                  << ToString(finding.confidence) << "]: " << finding.evidence << "\n";
    }
    for (const auto& reason : result.diagnosis.blocking_reasons)
        std::cout << "BLOCKER: " << reason << "\n";
    std::cout << "plan.feasible=" << result.plan.feasible << " reason=" << result.plan.infeasible_reason
              << "\noutput_published=" << result.output_published << "\n";
    if (with_reference) {
        uint64_t expected = 0;
        for (const auto& track : result.diagnosis.expected_tracks)
            expected += track.sample_count;
        const uint64_t written = result.executions.empty() ? 0 : result.executions.front().packets_written;
        std::cout << "reference: " << reference << "\n"
                  << "expected_samples=" << expected << " packets_written=" << written << "\n"
                  << "verification=" << ToString(result.verification.status)
                  << " content_completeness=" << result.fidelity.content_completeness
                  << " video_fidelity=" << ToString(result.fidelity.video) << "\n";
        for (const auto& finding : result.residual_findings)
            std::cout << "RESIDUAL " << finding.code << ": " << finding.evidence << "\n";
        // 只清理本用例自己创建的临时目录，绝不碰系统 temp 里的其它内容。
        std::error_code ec;
        std::filesystem::remove_all(request.output_directory, ec);
    }
    std::cout << "===== END =====\n";
}

bool HasResidualFinding(const RepairResult& result, const std::string& code) {
    for (const auto& finding : result.residual_findings) {
        if (finding.code == code)
            return true;
    }
    return false;
}

const RepairActionExecution* FindExecution(const RepairResult& result, RepairAction action) {
    for (const auto& execution : result.executions) {
        if (execution.action == action)
            return &execution;
    }
    return nullptr;
}

// 「中间损坏后只能保留前半段」是本轮要解决的核心问题：
// 把样本表里的一个中间 chunk offset 改成文件外偏移（结构仍合法，读到那里才会失败），
// 要求修复流程跳过损坏点、继续提取后续内容，并把跳段范围写进产物记录。
TEST(RepairEngine, MidFileChunkOffsetDamageResumesAfterReadError) {
    const std::string dir = MakeTempDir("midfile");
    const auto media = videoeye_test::CreateTestMedia(dir, "mid_src");
    ASSERT_TRUE(media.ok) << media.error;

    const std::string broken = dir + "/mid_broken.mp4";
    std::string patch_error;
    ASSERT_TRUE(PatchStcoChunkOffset(media.path, broken, 0.5, &patch_error))
        << "测试样本没有可改写的 stco: path=" << media.path << " size=" << std::filesystem::file_size(media.path)
        << " reason=" << patch_error;

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(broken, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_EQ(result.status, RepairBusinessStatus::PartiallyRecovered) << ToString(result.status);

    // 至少两段：损坏点之前 + 跳段恢复之后。只保留前半段会让这里保持 1。
    ASSERT_GE(result.recovered_segments.size(), 2u) << result.summary;
    EXPECT_TRUE(result.recovered_segments.front().resumed_after_damage == false);
    EXPECT_TRUE(result.recovered_segments.back().resumed_after_damage);
    EXPECT_GT(result.recovered_segments.back().skipped_before_seconds, 0.0);

    const RepairActionExecution* segment = FindExecution(result, RepairAction::S04_SegmentSalvage);
    ASSERT_NE(segment, nullptr);
    EXPECT_TRUE(segment->attempted) << segment->detail;
    EXPECT_TRUE(segment->succeeded) << segment->detail;

    // 诊断阶段确认的越界边界之外不应再继续读；跳段记录必须落在残留问题里。
    EXPECT_TRUE(HasResidualFinding(result, "container.damaged_segment_skipped")) << result.summary;

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
    // 保留原时间间隔：不做缺口合并。
    EXPECT_FALSE(result.timeline_repair.gap_removed);
}

// 同样的损坏，选择「删除损坏区间后连续播放」：产物时间轴必须被主动平移，
// 且报告要明确这是缺口合并，而不是播放速度被改变。
TEST(RepairEngine, MergeGapPolicyProducesContinuousTimeline) {
    const std::string dir = MakeTempDir("mergegap");
    const auto media = videoeye_test::CreateTestMedia(dir, "merge_src");
    ASSERT_TRUE(media.ok) << media.error;

    const std::string broken = dir + "/merge_broken.mp4";
    std::string patch_error;
    ASSERT_TRUE(PatchStcoChunkOffset(media.path, broken, 0.5, &patch_error)) << patch_error;

    RepairRequest request = MakeRequest(broken, dir + "/out");
    request.gap_policy = RepairGapPolicy::MergeContiguous;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);

    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    ASSERT_GE(result.recovered_segments.size(), 2u) << result.summary;
    EXPECT_TRUE(result.timeline_repair.gap_removed);
    EXPECT_GT(result.timeline_repair.removed_gap_seconds, 0.0);
    EXPECT_EQ(result.fidelity.timeline_status, "adjusted");
    EXPECT_TRUE(result.fidelity.timeline_modified);

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
    // 跳过的损坏区间不应出现在产物时长里。
    EXPECT_LT(probe.duration_seconds, media.duration_seconds - 0.05);
}

// 非 ISOBMFF 容器没有盒/样本表证据，但只要能探测到可读媒体轨，就必须允许
// 通用重封装兜底；结构级结论缺失只能降级报告，不能冒充阻断理由。
TEST(RepairPlanner, NonIsobmffContainerFallsBackToGenericRemux) {
    RepairDiagnosis diagnosis;
    diagnosis.openable = true;
    diagnosis.container_known = false;

    const auto outcome = repair::RepairPlanner::MakePlan(RepairRequest{}, diagnosis, std::nullopt, 0, nullptr);
    ASSERT_TRUE(outcome.can_execute) << outcome.plan.infeasible_reason;
    EXPECT_EQ(outcome.early_status, RepairBusinessStatus::Repaired);

    bool has_remux = false;
    for (const auto& action : outcome.plan.actions) {
        if (action.action == RepairAction::S01_Remux)
            has_remux = true;
    }
    EXPECT_TRUE(has_remux);
    bool boundary_documented = false;
    for (const auto& note : outcome.plan.global_preconditions) {
        if (note.find("通用重封装") != std::string::npos)
            boundary_documented = true;
    }
    EXPECT_TRUE(boundary_documented) << "必须把结构级检查缺失写进计划前置条件";

    // 没有可读轨道时仍应判为没有可恢复内容，而不是执行空重封装。
    const auto no_stream = repair::RepairPlanner::MakePlan(RepairRequest{}, diagnosis, std::nullopt, -1, nullptr);
    EXPECT_FALSE(no_stream.can_execute);
    EXPECT_EQ(no_stream.early_status, RepairBusinessStatus::NoRecoverableContent);
}

// 缺失时间戳的合成必须是显式开关：关闭时计划仍写明"不伪造"。
TEST(RepairPlanner, MissingTimestampSynthesisIsExplicit) {
    RepairDiagnosis diagnosis;
    diagnosis.container_known = true;
    diagnosis.moov_present = true;

    auto find_timeline = [](const videoeye::repair::RepairPlanOutcome& outcome) -> const RepairActionPlan* {
        for (const auto& action : outcome.plan.actions) {
            if (action.action == RepairAction::S03_TimelineRepair)
                return &action;
        }
        return nullptr;
    };

    const auto conservative = repair::RepairPlanner::MakePlan(RepairRequest{}, diagnosis, std::nullopt, 0, nullptr);
    const RepairActionPlan* action = find_timeline(conservative);
    ASSERT_NE(action, nullptr);
    EXPECT_NE(action->description.find("不"), std::string::npos) << action->description;

    RepairRequest request;
    request.fill_missing_timestamps = true;
    const auto filled = repair::RepairPlanner::MakePlan(request, diagnosis, std::nullopt, 0, nullptr);
    action = find_timeline(filled);
    ASSERT_NE(action, nullptr);
    EXPECT_NE(action->description.find("外推合成"), std::string::npos) << action->description;
    EXPECT_NE(action->risk.find("外推"), std::string::npos) << action->risk;
}

// 单轨抢救必须是"有证据的降级"：只有诊断明确某条轨全部样本不可读、
// 且还有其它轨道可用时，规划器才允许剔除该轨。
TEST(RepairPlanner, SingleTrackSalvageRequiresUnsalvageableEvidence) {
    RepairDiagnosis diagnosis;
    diagnosis.container_known = true;
    diagnosis.moov_present = true;
    RepairTrackSalvageInfo video;
    video.track_id = 1;
    video.input_stream_index = 0;
    video.kind = "video";
    video.salvageable = true;
    video.evidence = "0/40 越界";
    RepairTrackSalvageInfo audio;
    audio.track_id = 2;
    audio.input_stream_index = 1;
    audio.kind = "audio";
    audio.salvageable = false;
    audio.evidence = "全部 40 个样本越过文件尾";
    diagnosis.track_salvage = {video, audio};

    RepairRequest request;
    const auto outcome = repair::RepairPlanner::MakePlan(request, diagnosis, std::nullopt, 0, nullptr);
    ASSERT_TRUE(outcome.can_execute) << outcome.plan.infeasible_reason;
    EXPECT_EQ(outcome.plan.dropped_input_streams.size(), 1u);
    EXPECT_EQ(outcome.plan.dropped_input_streams.front(), 1);
    const RepairActionPlan* single = nullptr;
    for (const auto& action : outcome.plan.actions) {
        if (action.action == RepairAction::S09_SingleTrack)
            single = &action;
    }
    ASSERT_NE(single, nullptr);
    EXPECT_TRUE(single->available);

    // 用户关闭单轨抢救：不剔除任何轨道。
    RepairRequest conservative;
    conservative.allow_single_track = false;
    const auto conservative_outcome =
        repair::RepairPlanner::MakePlan(conservative, diagnosis, std::nullopt, 0, nullptr);
    EXPECT_TRUE(conservative_outcome.plan.dropped_input_streams.empty());

    // 没有"全部不可读"的证据：不许剔除。
    diagnosis.track_salvage[1].salvageable = true;
    diagnosis.track_salvage[1].evidence = "2/40 越界，仍有可用样本";
    const auto no_evidence = repair::RepairPlanner::MakePlan(request, diagnosis, std::nullopt, 0, nullptr);
    EXPECT_TRUE(no_evidence.plan.dropped_input_streams.empty());
}

// 执行层的单轨行为：plan 指定剔除某条输入流后，产物必须真的不含该流，
// 且剩余轨道仍能解码。
TEST(RepairExecutor, DroppedInputStreamIsOmittedFromOutput) {
    const std::string dir = MakeTempDir("dropout");
    const auto media = videoeye_test::CreateTestMedia(dir, "drop_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out", RepairVerificationLevel::V1_FrameSample);
    request.write_report = false;
    const auto paths = repair::RepairOutputWriter::Resolve(request, "execdrop");
    ASSERT_TRUE(paths.valid) << paths.error;
    std::string reserve_error;
    ASSERT_TRUE(repair::RepairOutputWriter::Reserve(paths, &reserve_error)) << reserve_error;

    RepairDiagnosis diagnosis;
    diagnosis.container_known = true;
    diagnosis.moov_present = true;
    RepairPlan plan;
    plan.feasible = true;
    plan.dropped_input_streams = {1}; // 测试样本：0=视频，1=音频
    plan.dropped_track_ids = {2};

    const auto outcome =
        repair::RepairExecutor::Execute(request, diagnosis, plan, paths, nullptr, nullptr, {}, nullptr);
    ASSERT_TRUE(outcome.ok) << outcome.error;
    EXPECT_FALSE(outcome.omitted_items.empty());

    const auto probe = videoeye_test::ProbeMediaFile(outcome.part_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_EQ(probe.video_streams, 1);
    EXPECT_EQ(probe.audio_streams, 0);
    EXPECT_GT(probe.frame_count, 0);
    repair::RepairOutputWriter::Discard(outcome.part_path);
}

// ============================================================
// P2：高成本能力
// ============================================================

// P2-14a：产物容器可切换为 MKV / MPEG-TS，产物必须能被 FFmpeg 重新打开并解码。
TEST(RepairEngine, OutputContainerSelectionProducesPlayableMkvAndTs) {
    const std::string dir = MakeTempDir("p2_container");
    const auto media = videoeye_test::CreateTestMedia(dir, "container_src");
    ASSERT_TRUE(media.ok) << media.error;

    struct Case {
        RepairOutputContainer container;
        const char* extension;
    };
    for (const Case& item : {Case{RepairOutputContainer::Mkv, "mkv"}, Case{RepairOutputContainer::Ts, "ts"}}) {
        RepairRequest request = MakeRequest(media.path, dir + "/out_" + std::string(item.extension),
                                            RepairVerificationLevel::V1_FrameSample);
        request.output_container = item.container;
        const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);

        ASSERT_TRUE(result.output_published) << item.extension << ": " << result.summary << " | " << result.error;
        ASSERT_FALSE(result.output_path.empty());
        EXPECT_EQ(std::filesystem::path(result.output_path).extension().string(), "." + std::string(item.extension));
        EXPECT_EQ(result.output_container, item.extension);
        EXPECT_TRUE(result.status == RepairBusinessStatus::Repaired ||
                    result.status == RepairBusinessStatus::PartiallyRecovered)
            << item.extension << ": " << ToString(result.status);

        const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
        EXPECT_TRUE(probe.opened) << item.extension << ": " << probe.error;
        EXPECT_GT(probe.frame_count, 0) << item.extension;
    }
}

// P2-14b：取消时保留已写前缀 —— 产物以 *.partial.<ext> 发布，状态仍然是 Canceled，
// 内容完整性标注为 partial（不能当作修复完成）。
TEST(RepairEngine, CancelKeepsWrittenPrefixWhenRequested) {
    const std::string dir = MakeTempDir("p2_partial");
    const auto media = videoeye_test::CreateTestMedia(dir, "partial_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out", RepairVerificationLevel::V1_FrameSample);
    request.keep_partial_on_cancel = true;
    std::atomic<bool> cancel{false};
    auto progress = [&cancel](int percent, const std::string&, const std::string&) {
        if (percent >= 89)
            cancel.store(true);
    };
    const RepairResult result = repair::RepairEngine::Run(request, &cancel, progress);

    ASSERT_EQ(result.status, RepairBusinessStatus::Canceled) << ToString(result.status) << " " << result.summary;
    ASSERT_TRUE(result.output_published) << result.summary;
    EXPECT_NE(result.output_path.find(".partial."), std::string::npos) << result.output_path;
    EXPECT_TRUE(std::filesystem::exists(result.output_path));
    EXPECT_EQ(result.fidelity.content_completeness, "partial");
    EXPECT_EQ(result.fidelity.timeline_status, "unknown");
    EXPECT_TRUE(HasLimitationContaining(result, "未完成验证")) << "未验证前缀必须在报告里写明";

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
}

// P2-13：救内容模式对可解码的视频帧做容忍式重编码，产物可播放且画质结论有量化口径。
TEST(RepairEngine, RecoverContentModeTranscodesReadableFrames) {
    const std::string dir = MakeTempDir("p2_transcode");
    const auto media = videoeye_test::CreateTestMedia(dir, "transcode_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out", RepairVerificationLevel::V1_FrameSample);
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);

    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_EQ(result.status, RepairBusinessStatus::PartiallyRecovered) << ToString(result.status);
    EXPECT_TRUE(result.fidelity.video_reencoded);
    EXPECT_TRUE(result.fidelity.timeline_modified);
    EXPECT_TRUE(HasLimitationContaining(result, "重编码"));

    const RepairActionExecution* transcode = FindExecution(result, RepairAction::S08_TolerantTranscode);
    ASSERT_NE(transcode, nullptr);
    EXPECT_TRUE(transcode->attempted) << transcode->detail;
    EXPECT_TRUE(transcode->succeeded) << transcode->detail;
    EXPECT_GT(transcode->packets_written, 0u);

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
}

// P2-12：普通 MP4 丢失 moov、但单个 mdat 是纯 H.264 AVCC（参数集与 VUI timing
// 都在码流里）时，无参考扫描 mdat 重建 ftyp+moov/样本表，产物应可直接解码。
// 样本用真实 ffmpeg 产物（real_h264_inband_missing_moov.mp4）：x264 的 SPS 带 VUI
// timing，关键帧重复携带 SPS/PPS —— 这是重建器能推断时基的唯一合法证据。
TEST(RepairEngine, RealCorpusInbandMp4WithoutMoovIsRebuiltFromMdat) {
    const std::string source = RealCorpusSample("real_h264_inband_missing_moov.mp4");
    if (!std::filesystem::exists(source)) {
        GTEST_SKIP() << "缺真实样本 real_h264_inband_missing_moov.mp4，先跑 tests/corpus/generate_real_corpus.py";
    }

    const std::string dir = MakeTempDir("real_plain_moov");
    const RepairResult result = repair::RepairEngine::Run(
        MakeRequest(source, dir + "/out", RepairVerificationLevel::V1_FrameSample), nullptr, nullptr);

    std::string blocking;
    for (const auto& reason : result.diagnosis.blocking_reasons)
        blocking += "[" + reason + "]";
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error << " | " << blocking;
    EXPECT_TRUE(result.status == RepairBusinessStatus::Repaired ||
                result.status == RepairBusinessStatus::PartiallyRecovered)
        << ToString(result.status);
    EXPECT_TRUE(HasFinding(result, "container.mdat_unindexed")) << result.diagnosis.summary;
    EXPECT_TRUE(HasLimitationContaining(result, "moov")) << "无参考重建必须写进报告";

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_GT(probe.frame_count, 0);
}

// 反向守护：mdat 里音视频交织时无法在不猜边界的前提下拆成单轨，必须如实要求参考样片。
TEST(RepairEngine, InterleavedPlainMdatWithoutMoovStillNeedsReference) {
    const std::string dir = MakeTempDir("p2_plain_interleaved");
    const auto media = videoeye_test::CreateTestMedia(dir, "interleaved_src");
    ASSERT_TRUE(media.ok) << media.error;

    const std::string broken = dir + "/no_moov.mp4";
    ASSERT_TRUE(StripTopLevelBox(media.path, broken, "moov"));

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(broken, dir + "/out"), nullptr, nullptr);

    EXPECT_EQ(result.status, RepairBusinessStatus::NeedsReference) << ToString(result.status) << " " << result.summary;
    EXPECT_FALSE(result.output_published);
    EXPECT_FALSE(result.diagnosis.blocking_reasons.empty());
}
