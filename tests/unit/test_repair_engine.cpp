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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/domain/model/RepairRequest.h"
#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairCapabilityContracts.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairExecutor.h"
#include "core/repair/RepairFragmentForensics.h"
#include "core/repair/RepairIndexRepair.h"
#include "core/repair/RepairMdatSelfCheck.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairPayloadHash.h"
#include "core/repair/RepairPlainMp4Rebuilder.h"
#include "core/repair/RepairPlanner.h"
#include "core/repair/RepairTolerantTranscode.h"
#include "core/repair/RepairUtils.h"
#include "core/reporting/RepairReportExporter.h"
#include "tests/support/MediaSequenceFingerprint.h"
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

// ---- 切片 A-1：正常媒体的序列级守恒门禁 ----
//
// ProbeMediaFile 只回答「能不能打开、有几帧、时长多少」；帧数一致完全可能掩盖
// 「包顺序被换」「PTS 间隔被改写」「解出来的画面不是同一批」。修复链自己的 S01
// remux 此前只有聚合级守恒，这里用 MediaSequenceFingerprint 逐包 / 逐帧配对补上。
// 判据本身（空事实 / 换序 / 回退 / 画面改写必须判红）由 test_export_roundtrip.cpp
// 的 CompareGuards* 用例守护，下面再留一条本地守护证明本测试接的是同一把尺子。
std::string JoinSequenceDiffs(const std::vector<std::string>& diffs) {
    std::string out;
    for (const auto& line : diffs) {
        if (!out.empty())
            out += "\n";
        out += line;
    }
    return out;
}

void ExpectRepairSequenceClean(const char* context, const std::vector<std::string>& diffs) {
    EXPECT_TRUE(diffs.empty()) << context << "\n" << JoinSequenceDiffs(diffs);
}

// 覆盖守护：取证必须真的拿到事实，否则「两边都空」会伪装成完全一致。
void AssertSequenceHasFacts(const videoeye_test::MediaSequence& seq, const char* tag) {
    ASSERT_TRUE(seq.opened) << tag << ": " << seq.error;
    ASSERT_TRUE(videoeye_test::HasDecodableVideo(seq)) << tag << ": " << seq.error;
    ASSERT_GE(seq.streams.size(), 2u) << tag << "：视频 + 音频两条轨都要参与逐流配对";
    ASSERT_GE(seq.total_packets, 40u) << tag << "：样本量太小，差异会被掩盖";
    for (const auto& stream : seq.streams) {
        if (stream.codec_type != "video")
            continue;
        ASSERT_GT(stream.display_pts_samples, 0u) << tag << "：没取到显示顺序事实，不得算「一致」";
        ASSERT_FALSE(stream.frame_sequence_hash.empty()) << tag << "：没取到逐帧亮度序列";
    }
}

void VerifyIntactSequencePreserved(const std::string& source_path, const char* tag) {
    const std::string dir = MakeTempDir(std::string("seq_") + tag);
    ASSERT_TRUE(std::filesystem::exists(source_path)) << tag << ": 基准语料缺失";

    const auto expected = videoeye_test::CaptureMediaSequence(source_path);
    AssertSequenceHasFacts(expected, tag);

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(source_path, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << tag << ": " << result.summary << " | " << result.error;
    ASSERT_EQ(result.status, RepairBusinessStatus::Repaired) << tag;
    EXPECT_FALSE(result.fidelity.timeline_modified) << tag << "：保码流路径不得改写时间轴";

    const auto actual = videoeye_test::CaptureMediaSequence(result.output_path);
    AssertSequenceHasFacts(actual, tag);
    ExpectRepairSequenceClean((std::string(tag) + "：修复产物逐包/逐帧序列").c_str(),
                              videoeye_test::CompareExactSequence(expected, actual));
}

// 基准用仓内 ffmpeg 生成的健康原片（H.264+AAC，含 B 帧，faststart）。
// ❗ 不要用 TestMediaSpec 默认的无 B 帧样本当逐帧基准：那个生成器尾部冲刷后
// 源只解出 39 帧，而保码流 remux 产物解出 40 帧 —— 两边逐包载荷序列完全一致，
// 是「解码可见性」边界而不是修复链丢了内容。拿它做基准会让门禁恒红。
TEST(RepairEngine, IntactMediaSequenceIsPreservedExactly) {
    const std::string source = std::string(VIDEOEYE_CONTAINER_CORPUS_DIR) + "/real_h264_aac_faststart.mp4";
    if (!std::filesystem::is_regular_file(source))
        GTEST_SKIP() << "仓内真实语料缺失: " << source;
    VerifyIntactSequencePreserved(source, "real_corpus");
}

// B 帧 + 30000/1001：解码顺序 ≠ 显示顺序，逐包 PTS 天然锯齿；判据必须看解码
// 输出的显示顺序，既不能把合法重排误报成缺陷，也不能漏掉真的丢帧 / 改帧率。
TEST(RepairEngine, BFrameFractionalRateSequenceIsPreservedExactly) {
    const std::string dir = MakeTempDir("seq_frac");
    videoeye_test::TestMediaSpec spec;
    spec.fps = 30000;
    spec.fps_den = 1001;
    spec.max_b_frames = 2;
    const auto media = videoeye_test::CreateTestMedia(dir, "frac_seq_src", spec);
    ASSERT_TRUE(media.ok) << media.error;
    VerifyIntactSequencePreserved(media.path, "bframe_ntsc");
}

// 守护：本门禁依赖的判据必须真的有判别力，不能因为将来改动指纹实现而恒绿。
// 拿真实取证事实换一下序，逐包载荷序列必须报错。
TEST(RepairEngine, SequenceConservationGateRejectsReorderedPackets) {
    const std::string dir = MakeTempDir("seq_guard");
    const auto media = videoeye_test::CreateTestMedia(dir, "seq_guard_src");
    ASSERT_TRUE(media.ok) << media.error;

    const auto base = videoeye_test::CaptureMediaSequence(media.path);
    AssertSequenceHasFacts(base, "guard");
    ExpectRepairSequenceClean("自比对应无差异", videoeye_test::CompareExactSequence(base, base));

    auto reordered = base;
    for (auto& stream : reordered.streams) {
        if (stream.codec_type != "video" || stream.packets.size() < 3)
            continue;
        std::swap(stream.packets[1].payload_hash, stream.packets[2].payload_hash);
        // CompareExactSequence 比的是滚动哈希字符串，不是逐包 payload_hash 数组：
        // 只换数组不重算等于没换，这条守护会恒绿。
        uint64_t rolling = videoeye_test::Fnv1a64(14695981039346656037ULL, nullptr, 0);
        for (const auto& p : stream.packets)
            rolling = videoeye_test::Fnv1a64(rolling, &p.payload_hash, sizeof(p.payload_hash));
        stream.payload_sequence_hash = videoeye_test::HexHash(rolling);
    }
    const auto diffs = videoeye_test::CompareExactSequence(base, reordered);
    EXPECT_FALSE(diffs.empty()) << "换序必须判红";
    EXPECT_NE(JoinSequenceDiffs(diffs).find("逐包载荷序列"), std::string::npos) << JoinSequenceDiffs(diffs);

    videoeye_test::MediaSequence empty;
    EXPECT_FALSE(videoeye_test::CompareExactSequence(base, empty).empty()) << "空产物必须判红";
}

// ---- 切片 A-2：名实相符守护 ----
//
// 能力契约与「未启用动作」的描述会进 JSON / TXT / HTML 三份报告，是错误结论率的
// 直接来源：一边跑着 S08，一边告诉用户「当前版本未实现」，比不写更糟。
TEST(RepairEngine, CapabilityContractsDescribeImplementedScopeAccurately) {
    const auto& all = repair::AllCapabilityBoundaries();
    ASSERT_EQ(all.size(), 3u);
    const auto& moov = all[0];
    const auto& params = all[1];
    const auto& gop = all[2];

    EXPECT_NE(moov.evidence.find("RepairPlainMp4Rebuilder"), std::string::npos)
        << "moov_rebuild 必须写明已有受限版无参考重建器";
    EXPECT_EQ(moov.evidence.find("不反推普通 MP4 的 moov"), std::string::npos) << moov.evidence;
    EXPECT_NE(params.evidence.find("RepairInitSegmentBuilder"), std::string::npos)
        << "参数集恢复必须写明已有 in-band 自建路径";

    EXPECT_NE(gop.evidence.find("已实现并可运行"), std::string::npos) << gop.evidence;
    EXPECT_EQ(gop.evidence.find("明确标记 unavailable"), std::string::npos) << gop.evidence;
    EXPECT_EQ(gop.evidence.find("尚无解码-编码链"), std::string::npos) << gop.evidence;
    EXPECT_NE(gop.timeline_impact.find("不插入"), std::string::npos) << "合成帧目前是空能力，契约不能声称会插入并记账";
}

TEST(RepairEngine, PreserveBitstreamPlanDoesNotClaimTranscodeIsUnimplemented) {
    const std::string dir = MakeTempDir("plan_truth");
    const auto media = videoeye_test::CreateTestMedia(dir, "plan_src");
    ASSERT_TRUE(media.ok) << media.error;

    const auto result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    bool saw_s08 = false;
    for (const auto& action : result.plan.actions) {
        if (action.action != RepairAction::S08_TolerantTranscode)
            continue;
        saw_s08 = true;
        EXPECT_FALSE(action.available) << "保码流模式下 S08 不应被选中执行";
        EXPECT_EQ(action.expected_effect.find("当前版本未实现"), std::string::npos) << action.expected_effect;
        EXPECT_NE(action.expected_effect.find("本次未启用"), std::string::npos) << action.expected_effect;
    }
    EXPECT_TRUE(saw_s08) << "覆盖守护：计划里必须真的列出 S08 的未启用状态，否则循环空转也绿";
}

// ---- 切片 A-4：损失区间结构化 + 三格式一致 ----
//
// 之前「跳过了多少」只藏在每段前的 skipped_before_seconds 和一个残留 finding 里，
// 报告读者无法逐段回答「哪段时间没了、有没有其它副本」。这里把它落成显式区间。
TEST(RepairEngine, LostRangesAreStructuredAndVisibleInAllReportFormats) {
    const std::string dir = MakeTempDir("lost_ranges");
    const auto media = videoeye_test::CreateTestMedia(dir, "lost_src");
    ASSERT_TRUE(media.ok) << media.error;
    const std::string broken = dir + "/lost_broken.mp4";
    std::string patch_error;
    ASSERT_TRUE(PatchStcoChunkOffset(media.path, broken, 0.5, &patch_error)) << patch_error;

    const RepairResult result = repair::RepairEngine::Run(MakeRequest(broken, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    ASSERT_GE(result.recovered_segments.size(), 2u) << result.summary;

    ASSERT_FALSE(result.lost_ranges.empty()) << "有跳段就必须有结构化损失区间";
    for (const auto& lost : result.lost_ranges) {
        EXPECT_GT(lost.DurationSeconds(), 0.0);
        EXPECT_EQ(lost.reason, "container.damaged_segment_skipped");
        EXPECT_FALSE(lost.recoverable) << "没有其它副本时不能写成可恢复";
        EXPECT_FALSE(lost.evidence.empty());
        EXPECT_FALSE(lost.byte_start.has_value()) << "没测到字节边界就留空，不填 0";
    }
    EXPECT_NE(result.summary.find("损失区间"), std::string::npos) << result.summary;

    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string json = ReadText(dir + "/r.json");
    const std::string text = ReadText(dir + "/r.txt");
    const std::string html = ReadText(dir + "/r.html");
    EXPECT_NE(json.find("\"lost_ranges\""), std::string::npos);
    EXPECT_NE(json.find("container.damaged_segment_skipped"), std::string::npos);
    // 切片 D：恢复点来源必须是结构化机读码，不能只有一句自由文本。
    EXPECT_EQ(result.recovered_segments.back().recovery_evidence, RepairRecoveryEvidence::TrialDecodeOk)
        << result.recovered_segments.back().recovery_point;
    EXPECT_NE(json.find("\"recovery_evidence\""), std::string::npos);
    EXPECT_NE(json.find("trial_decode_ok"), std::string::npos);
    // 切片 G：全局损失区间之外，必须能说出「哪条轨缺了哪一段」。
    // available=false 会被读成「逐轨都没有缺口」，所以这里要求真接了账本。
    ASSERT_TRUE(result.track_coverage.available) << result.track_coverage.unavailable_reason;
    ASSERT_FALSE(result.track_coverage.tracks.empty());
    EXPECT_TRUE(result.track_coverage.complete) << "本次执行走到了源末尾，覆盖不该只是前缀";
    EXPECT_GT(result.track_coverage.tracks.front().samples, 0u);
    EXPECT_NE(json.find("\"track_coverage\""), std::string::npos);
    EXPECT_NE(json.find("\"common_span_known\""), std::string::npos);
    for (const auto& body : {text, html}) {
        EXPECT_NE(body.find("音画共同覆盖"), std::string::npos) << "TXT / HTML 必须给出共同覆盖区间";
        EXPECT_NE(body.find("缺口 "), std::string::npos) << "TXT / HTML 必须逐轨交代缺口";
    }
    for (const auto& body : {text, html}) {
        EXPECT_NE(body.find("损失区间"), std::string::npos) << "TXT / HTML 必须显式展示损失区间";
        EXPECT_NE(body.find("无其它副本"), std::string::npos);
        EXPECT_NE(body.find("恢复点来源"), std::string::npos) << "TXT / HTML 必须说明恢复点来源";
    }
}

// 反向守护：正常文件没有跳段时不得凭空报损失（「无事实时不得谎报」）。
TEST(RepairEngine, HealthyMediaReportsNoLostRanges) {
    const std::string dir = MakeTempDir("lost_none");
    const auto media = videoeye_test::CreateTestMedia(dir, "lost_none_src");
    ASSERT_TRUE(media.ok) << media.error;
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_TRUE(result.lost_ranges.empty()) << result.summary;
    EXPECT_EQ(result.summary.find("损失区间"), std::string::npos) << result.summary;
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
    EXPECT_EQ(result.recovered_segments.front().recovery_evidence, RepairRecoveryEvidence::Unknown);
    EXPECT_TRUE(result.recovered_segments.back().resumed_after_damage);
    EXPECT_GT(result.recovered_segments.back().skipped_before_seconds, 0.0);
    // 切片 D：恢复点证据必须升级到「试解码确认」。容器关键帧标记只是声明，
    // 这一段从损坏点之后的关键帧续采，候选包必须真的能解出帧才允许写进来。
    EXPECT_EQ(result.recovered_segments.back().recovery_evidence, RepairRecoveryEvidence::TrialDecodeOk)
        << result.recovered_segments.back().recovery_point;

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
// F01 门禁：正常输入 40 帧必须 40 帧全出。旧实现第二帧起 av_frame_get_buffer 恒
// EINVAL，只写 1 帧仍返回成功，而"frame_count > 0"这种断言完全看不出来。
TEST(RepairEngine, RecoverContentModeTranscodesReadableFrames) {
    const std::string dir = MakeTempDir("p2_transcode");
    const auto media = videoeye_test::CreateTestMedia(dir, "transcode_src");
    ASSERT_TRUE(media.ok) << media.error;
    const auto source_probe = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source_probe.opened) << source_probe.error;
    // 以「实测可解码帧数」为真值：编码器尾部冲刷可能少吐一帧，不要用声明值 40。
    ASSERT_GT(source_probe.frame_count, 1);

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
    EXPECT_GE(transcode->packets_written, static_cast<uint64_t>(source_probe.frame_count));

    const auto probe = videoeye_test::ProbeMediaFile(result.output_path);
    EXPECT_TRUE(probe.opened) << probe.error;
    EXPECT_EQ(probe.frame_count, source_probe.frame_count) << "正常输入的每一帧都必须被重新编码，不能只写第一帧";
    // 源里可能有 1 个解不出画面的包（40 包 / 39 帧），容忍式转码按「可解码帧」
    // 重建，所以只能要求产物一帧一包，不能拿源包数硬比。
    EXPECT_EQ(probe.video_packet_count, probe.frame_count);
    EXPECT_NEAR(probe.video_duration_seconds, source_probe.video_duration_seconds, 0.20)
        << "F02：输出 PTS 跨度必须与源时长一致";
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

// 覆盖范围的「实测值」必须能被独立取证复算。
//
// FragmentedSegmentsProduceMeasuredForensics 只断言 bytes_scanned > 0，把实现换成
// 「顶层盒大小之和」或整个文件大小照样全绿；reached_eof 也只断言为 true，但那条
// 在缺 moov 时恰好是派生式 parse_ok && found_moov 会给出 false，所以仍有判别力。
// 这里把取证器独立调一遍当作真值，逐字段钉死口径，并显式校验三种值确实能区分
// （口径重合时用例自己会报「失去判别力」，不会假装通过）。
TEST(RepairEngine, FragmentCoverageIsMeasuredPayloadBytesNotBoxSizes) {
    const std::string dir = MakeTempDir("coverage_measured");
    const auto media = videoeye_test::CreateFragmentedTestMedia(dir, "coverage");
    ASSERT_TRUE(media.ok) << media.error;

    videoeye::IsobmffFile parsed;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(media.segment_path, parsed)) << parsed.error_message;
    const auto ev = videoeye::repair::RepairFragmentForensics::Analyze(media.segment_path, parsed, nullptr);
    ASSERT_TRUE(ev.analyzed) << ev.error;
    ASSERT_GT(ev.bytes_scanned, 0u);
    ASSERT_GT(ev.samples_scanned, 0u);

    uint64_t structural_bytes = 0;
    for (const auto& box : parsed.top_level)
        structural_bytes += box.size;
    const uint64_t file_size = FileSize(media.segment_path);
    EXPECT_NE(ev.bytes_scanned, structural_bytes) << "盒大小之和与实测载荷重合，用例失去判别力";
    EXPECT_NE(ev.bytes_scanned, file_size) << "文件大小与实测载荷重合，用例失去判别力";

    const RepairResult result =
        repair::RepairEngine::Run(MakeRequest(media.segment_path, dir + "/out"), nullptr, nullptr);
    const auto& coverage = result.diagnosis.coverage;
    EXPECT_EQ(coverage.bytes_scanned, ev.bytes_scanned)
        << "覆盖范围只能写逐段实扫的 mdat 载荷字节，不能拿盒大小 / 文件大小充数";
    EXPECT_NE(coverage.bytes_scanned, structural_bytes);
    EXPECT_NE(coverage.bytes_scanned, file_size);
    EXPECT_EQ(coverage.packets_scanned, ev.samples_scanned);
    EXPECT_EQ(coverage.reached_eof, ev.reached_eof);
    EXPECT_TRUE(coverage.reached_eof) << "缺 moov 的分片序列已实扫到文件尾；用 parse_ok && found_moov 派生会恒 false";
    EXPECT_FALSE(coverage.truncated_by_budget);
    EXPECT_EQ(coverage.time_scanned_seconds, ev.estimated_duration_seconds);
}

// ============================================================
// 2026-11-09 审查修复回归：F01..F08
//
// 门禁顺序按审查建议：先保证正常媒体的「逐轨帧数 / 包数 / 时长守恒」，再补
// 跨容器音频哈希、分数帧率、mdat 结构、预算、输出上限与写入失败路径。
// ============================================================

namespace {

// 造一份「ftyp + 单个 mdat 里全是最小 ADTS AAC 帧」的无 moov MP4。
// 每帧 7 字节 ADTS 头 + payload_bytes 载荷，帧长字段自洽，能过 ParseAdtsHeader。
bool WriteAdtsOnlyMp4(const std::string& path, int frames, int payload_bytes) {
    std::vector<uint8_t> bytes;
    auto push_u32 = [&bytes](uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value >> 24));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value));
    };
    auto push_4cc = [&bytes](const char* text) {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<uint8_t>(text[i]));
    };
    const uint32_t frame_len = static_cast<uint32_t>(7 + payload_bytes);
    const uint32_t mdat_size = 8u + static_cast<uint32_t>(frames) * frame_len;
    push_u32(28);
    push_4cc("ftyp");
    push_4cc("isom");
    push_u32(512);
    push_4cc("isom");
    push_4cc("iso2");
    push_4cc("mp41");
    push_u32(mdat_size);
    push_4cc("mdat");
    for (int n = 0; n < frames; ++n) {
        std::vector<uint8_t> frame(frame_len, 0);
        frame[0] = 0xFF;
        frame[1] = 0xF1; // MPEG-4 / layer 00 / protection_absent=1 → 7 字节头
        frame[2] = 0x50; // profile=1(AAC LC), sampling_frequency_index=4(44100)
        frame[3] = static_cast<uint8_t>(0x80 | ((frame_len >> 11) & 0x03)); // channel_config=2
        frame[4] = static_cast<uint8_t>((frame_len >> 3) & 0xFF);
        frame[5] = static_cast<uint8_t>(((frame_len & 0x07) << 5) | 0x1F);
        frame[6] = 0xFC;
        for (uint32_t i = 7; i < frame_len; ++i)
            frame[i] = static_cast<uint8_t>((i * 7 + n) & 0xFF);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void ExpectRealMdatType(const std::string& path) {
    videoeye::IsobmffFile parsed;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(path, parsed)) << parsed.error_message;
    ASSERT_TRUE(parsed.found_mdat) << "产物里必须有类型正确的 mdat 盒";
    ASSERT_GT(parsed.first_mdat_offset, 0u);
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good());
    in.seekg(static_cast<std::streamoff>(parsed.first_mdat_offset + 4), std::ios::beg);
    char type[5] = {};
    in.read(type, 4);
    EXPECT_EQ(std::string(type, 4), "mdat") << "盒类型必须写在下标 4..7，不能被 size 覆盖成 0";
}

class TranscodeFaultGuard {
public:
    TranscodeFaultGuard() = default;
    ~TranscodeFaultGuard() {
        repair::SetTranscodeWriteFailureForTesting(0);
        repair::SetTranscodeTrailerFailureForTesting(false);
    }
    TranscodeFaultGuard(const TranscodeFaultGuard&) = delete;
    TranscodeFaultGuard& operator=(const TranscodeFaultGuard&) = delete;
};

} // namespace

// ---- F03：ADTS 头与裸访问单元必须是同一份哈希证据 ----
TEST(RepairPayloadHashTests, AdtsAndRawAccessUnitsProduceSameHash) {
    using videoeye::repair::Fnv1a64Begin;
    using videoeye::repair::Fnv1a64Update;
    using videoeye::repair::UpdateAudioPayloadHash;
    constexpr bool kAac = true; // codec_id == AV_CODEC_ID_AAC 时传 true

    std::vector<uint8_t> au1(64), au2(48);
    for (size_t i = 0; i < au1.size(); ++i)
        au1[i] = static_cast<uint8_t>(i * 3 + 1);
    for (size_t i = 0; i < au2.size(); ++i)
        au2[i] = static_cast<uint8_t>(i * 5 + 7);
    auto adts = [](const std::vector<uint8_t>& au, bool crc) {
        const size_t header = crc ? 9 : 7;
        const size_t total = header + au.size();
        std::vector<uint8_t> frame(total, 0);
        frame[0] = 0xFF;
        frame[1] = crc ? 0xF0 : 0xF1;
        frame[2] = 0x50;
        frame[3] = static_cast<uint8_t>(((total >> 11) & 0x03));
        frame[4] = static_cast<uint8_t>((total >> 3) & 0xFF);
        frame[5] = static_cast<uint8_t>(((total & 0x07) << 5) | 0x1F);
        frame[6] = 0xFC;
        std::copy(au.begin(), au.end(), frame.begin() + static_cast<std::ptrdiff_t>(header));
        return frame;
    };

    const uint64_t raw = Fnv1a64Update(Fnv1a64Update(Fnv1a64Begin(), au1.data(), au1.size()), au2.data(), au2.size());
    const auto one = adts(au1, false);
    const auto two = adts(au2, false);
    EXPECT_EQ(UpdateAudioPayloadHash(Fnv1a64Begin(), one.data(), one.size(), kAac),
              Fnv1a64Update(Fnv1a64Begin(), au1.data(), au1.size()));
    // 一个 PES 包装两个 ADTS 帧，必须与 MP4 侧「两个裸 AU 包」逐字节等价。
    std::vector<uint8_t> combined = one;
    combined.insert(combined.end(), two.begin(), two.end());
    EXPECT_EQ(UpdateAudioPayloadHash(Fnv1a64Begin(), combined.data(), combined.size(), kAac), raw);
    // 带 CRC 的 9 字节头同样只比访问单元。
    const auto with_crc = adts(au1, true);
    EXPECT_EQ(UpdateAudioPayloadHash(Fnv1a64Begin(), with_crc.data(), with_crc.size(), kAac),
              Fnv1a64Update(Fnv1a64Begin(), au1.data(), au1.size()));
}

TEST(RepairPayloadHashTests, NonAacAndFalseSyncAreNeverStripped) {
    using videoeye::repair::Fnv1a64Begin;
    using videoeye::repair::Fnv1a64Update;
    using videoeye::repair::UpdateAudioPayloadHash;
    std::vector<uint8_t> data(32);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(0xF0 + i);
    // 非 AAC 编码：原样累计（结论层会标为「无规范化能力」）。
    EXPECT_EQ(UpdateAudioPayloadHash(Fnv1a64Begin(), data.data(), data.size(), false),
              Fnv1a64Update(Fnv1a64Begin(), data.data(), data.size()));
    // 以 0xFF 0xF1 开头但帧长字段不自洽的裸 AU：不得误剥 7 字节。
    data[0] = 0xFF;
    data[1] = 0xF1;
    data[3] = 0x00;
    data[4] = 0x00;
    data[5] = 0x00;
    EXPECT_EQ(UpdateAudioPayloadHash(Fnv1a64Begin(), data.data(), data.size(), true),
              Fnv1a64Update(Fnv1a64Begin(), data.data(), data.size()));
}

// ---- F05：盒头的普通 / 扩展分支都要把 'mdat' 写到下标 4..7 ----
TEST(RepairMdatTests, MakeMdatHeaderWritesTypeInBothBranches) {
    uint8_t header[16] = {};
    EXPECT_EQ(videoeye::repair::MakeMdatHeader(100, header), 8u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(header + 4), 4), "mdat");
    EXPECT_EQ((static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
                  (static_cast<uint32_t>(header[2]) << 8) | header[3],
              108u);

    const uint64_t huge_payload = 0xFFFFFFFFull - 7ull; // 迫使进入 largesize 分支
    EXPECT_EQ(videoeye::repair::MakeMdatHeader(huge_payload, header), 16u);
    EXPECT_EQ(header[3], 1);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(header + 4), 4), "mdat");
    uint64_t large_size = 0;
    for (int i = 0; i < 8; ++i)
        large_size = (large_size << 8) | header[8 + i];
    EXPECT_EQ(large_size, huge_payload + 16u);
}

TEST(RepairMdatTests, SelfCheckRejectsZeroTypeAndOutOfRangeSamples) {
    videoeye::IsobmffFile file;
    file.ok = true;
    file.file_size = 108;
    file.found_mdat = true;
    videoeye::IsobmffBox box;
    box.offset = 0;
    box.size = 108;
    box.header_size = 8;
    box.type = std::string("\0\0\0\0", 4); // 旧 bug 的产物
    file.top_level.push_back(box);
    std::string error;
    EXPECT_FALSE(videoeye::repair::VerifyRebuiltMdat(file, 8, 108, 100, &error)) << error;
    EXPECT_NE(error.find("mdat"), std::string::npos);

    file.top_level[0].type = "mdat";
    EXPECT_TRUE(videoeye::repair::VerifyRebuiltMdat(file, 8, 108, 100, &error)) << error;
    EXPECT_FALSE(videoeye::repair::VerifyRebuiltMdat(file, 8, 200, 100, &error)) << "样本越界必须判红";
    EXPECT_FALSE(videoeye::repair::VerifyRebuiltMdat(file, 8, 108, 99, &error)) << "载荷长度不符必须判红";
}

TEST(RepairMdatTests, RebuiltPlainAndIndexMp4HaveRealMdat) {
    const std::string plain_source = RealCorpusSample("real_h264_inband_missing_moov.mp4");
    if (!std::filesystem::exists(plain_source))
        GTEST_SKIP() << "缺真实样本，先跑 tests/corpus/generate_real_corpus.py";
    const std::string dir = MakeTempDir("mdat_header");

    const std::string plain_out = dir + "/plain.mp4";
    const auto plain = repair::RepairPlainMp4Rebuilder::Rebuild(plain_source, plain_out, nullptr);
    ASSERT_TRUE(plain.ok) << plain.error;
    ExpectRealMdatType(plain_out);

    const auto media = videoeye_test::CreateTestMedia(dir, "index_src");
    ASSERT_TRUE(media.ok) << media.error;
    const std::string broken = dir + "/shifted.mp4";
    ASSERT_TRUE(PrependFreeBox(media.path, broken, 1024u));
    const std::string index_out = dir + "/index.mp4";
    const auto index = repair::RepairIndexRepair::Rebuild(broken, index_out, nullptr);
    ASSERT_TRUE(index.ok) << index.error;
    ExpectRealMdatType(index_out);
}

// ---- F06：扫描循环必须真的响应取消与共享预算 ----
TEST(RepairPlainMp4RebuilderTests, ScanLoopHonorsCancelAndSharedBudget) {
    const std::string dir = MakeTempDir("plain_cancel");
    const std::string path = dir + "/adts_only.mp4";
    ASSERT_TRUE(WriteAdtsOnlyMp4(path, 4096, 8)); // 每帧 15 字节，pos 从不落在 1 MiB 边界前一字节

    std::atomic<bool> cancel_true{true};
    const auto canceled = repair::RepairPlainMp4Rebuilder::Probe(path, &cancel_true);
    EXPECT_FALSE(canceled.attemptable);
    EXPECT_NE(canceled.reason.find("已取消"), std::string::npos) << canceled.reason;

    const auto canceled_via_budget = repair::RepairPlainMp4Rebuilder::Probe(path, &cancel_true, nullptr);
    EXPECT_FALSE(canceled_via_budget.attemptable);

    std::atomic<bool> cancel_false{false};
    repair::RepairRunBudget budget(&cancel_false, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    const auto timed_out = repair::RepairPlainMp4Rebuilder::Probe(path, &cancel_false, &budget);
    EXPECT_FALSE(timed_out.attemptable);
    EXPECT_NE(timed_out.reason.find("预算"), std::string::npos) << timed_out.reason;

    // 反向守护：没取消、没预算时必须能扫出 ADTS 单轨，避免把「一律拒绝」当修复。
    const auto healthy = repair::RepairPlainMp4Rebuilder::Probe(path, nullptr);
    EXPECT_TRUE(healthy.attemptable) << healthy.reason;
    EXPECT_EQ(healthy.kind, "aac");
}

// ---- F03：默认 V2 下合法 MP4→TS / TS→MP4 不能被音频载荷哈希误拒 ----
TEST(RepairEngine, MpegTsRemuxWithDefaultV2VerifiesNormalizedAudio) {
    const std::string dir = MakeTempDir("f03_ts_audio");
    const auto media = videoeye_test::CreateTestMedia(dir, "ts_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest forward = MakeRequest(media.path, dir + "/out"); // 默认 V2 全量解码
    forward.output_container = RepairOutputContainer::Ts;
    const RepairResult ts_result = repair::RepairEngine::Run(forward, nullptr, nullptr);
    ASSERT_TRUE(ts_result.output_published) << ts_result.summary << " | " << ts_result.error;
    EXPECT_NE(ts_result.verification.status, RepairCheckStatus::Failed);
    EXPECT_EQ(ts_result.fidelity.audio, RepairFidelityStatus::VerifiedMatch)
        << "剥离 ADTS 头后 AAC 访问单元必须逐包一致";

    // 反向：TS 输入带 ADTS 头、MP4 输出是裸 AU，同样不能判不一致。
    RepairRequest backward = MakeRequest(ts_result.output_path, dir + "/back");
    backward.output_container = RepairOutputContainer::Mp4;
    const RepairResult mp4_result = repair::RepairEngine::Run(backward, nullptr, nullptr);
    ASSERT_TRUE(mp4_result.output_published) << mp4_result.summary << " | " << mp4_result.error;
    EXPECT_NE(mp4_result.verification.status, RepairCheckStatus::Failed);
    EXPECT_EQ(mp4_result.fidelity.audio, RepairFidelityStatus::VerifiedMatch);
}

// ---- F02：30000/1001、24000/1001 的帧数与时长守恒 ----
TEST(RepairEngine, RecoverContentKeepsFractionalFrameRateTimeline) {
    struct Case {
        int num;
        int den;
        const char* tag;
    };
    for (const Case& item : {Case{30000, 1001, "ntsc30"}, Case{24000, 1001, "ntsc24"}}) {
        const std::string dir = MakeTempDir(std::string("f02_") + item.tag);
        videoeye_test::TestMediaSpec spec;
        spec.fps = item.num;
        spec.fps_den = item.den;
        spec.frame_count = 40;
        const auto media = videoeye_test::CreateTestMedia(dir, "frac_src", spec);
        ASSERT_TRUE(media.ok) << item.tag << ": " << media.error;
        const auto src = videoeye_test::ProbeMediaFile(media.path);
        ASSERT_TRUE(src.opened) << item.tag << ": " << src.error;
        ASSERT_EQ(src.frame_count, spec.frame_count);

        RepairRequest request = MakeRequest(media.path, dir + "/out", RepairVerificationLevel::V1_FrameSample);
        request.mode = RepairMode::RecoverContent;
        const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
        ASSERT_TRUE(result.output_published) << item.tag << ": " << result.summary << " | " << result.error;
        const auto out = videoeye_test::ProbeMediaFile(result.output_path);
        ASSERT_TRUE(out.opened) << item.tag << ": " << out.error;
        EXPECT_EQ(out.frame_count, src.frame_count) << item.tag;
        EXPECT_NEAR(out.video_duration_seconds, src.video_duration_seconds, 0.20)
            << item.tag << "：旧时基 {1,fps.num} 会把 1001 s 压成 1 s";
        EXPECT_NEAR(out.duration_seconds, src.duration_seconds, 0.25) << item.tag;
    }
}

// ---- F04：保留音轨必须回放首帧探测期的包、默认参与逐轨验证 ----
TEST(RepairEngine, RecoverContentRetainsAudioAndVerifiesEveryTrack) {
    const std::string dir = MakeTempDir("f04_audio");
    videoeye_test::TestMediaSpec spec;
    spec.max_b_frames = 2; // 首帧解码被推迟，之前会出现音频包
    const auto media = videoeye_test::CreateTestMedia(dir, "audio_src", spec);
    ASSERT_TRUE(media.ok) << media.error;
    const auto src = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(src.opened) << src.error;
    ASSERT_GT(src.audio_packet_count, 0);

    RepairRequest request = MakeRequest(media.path, dir + "/out", RepairVerificationLevel::V2_FullDecode);
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const auto out = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(out.opened) << out.error;
    EXPECT_EQ(out.audio_streams, src.audio_streams);
    EXPECT_EQ(out.audio_packet_count, src.audio_packet_count) << "首帧探测期间的音频包必须被回放，不能静默丢弃";
    EXPECT_NEAR(out.audio_start_seconds, 0.0, 0.15) << "音频起点应与重建的视频起点对齐";

    bool audio_required = false;
    bool audio_verified = false;
    for (const auto& track : result.verification.per_track) {
        if (track.kind != "audio")
            continue;
        audio_required = true;
        audio_verified = audio_verified || track.verified;
        EXPECT_NE(track.status, RepairCheckStatus::NotRun) << "S08 保留音轨不能再被固定标为不验证";
    }
    EXPECT_TRUE(audio_required && audio_verified) << "全部保留媒体轨都必须有实测解码证据";
    EXPECT_TRUE(result.verification.av_sync_checked) << "音画同步检查不能排除 S08 音轨";
    EXPECT_LT(result.verification.av_sync_start_delta_seconds, 0.5);
}

// ---- F07：S08 与重封装路径一样遵守输出字节上限 ----
TEST(RepairEngine, RecoverContentRespectsMaxOutputBytes) {
    const std::string dir = MakeTempDir("f07_cap");
    const auto media = videoeye_test::CreateTestMedia(dir, "cap_src");
    ASSERT_TRUE(media.ok) << media.error;
    const auto src = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(src.opened) << src.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out", RepairVerificationLevel::V1_FrameSample);
    request.mode = RepairMode::RecoverContent;
    request.limits.max_output_bytes = 10000;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_TRUE(HasResidualFinding(result, "output.size_capped")) << "S08 命中上限必须留痕";
    EXPECT_EQ(result.fidelity.content_completeness, "partial");

    const auto out = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(out.opened) << out.error;
    EXPECT_GT(out.frame_count, 0) << "上限内仍应交付可播放前缀";
    EXPECT_LT(out.frame_count, src.frame_count) << "命中上限后必须停止继续写入";
}

// ---- F08：部分写入后的写包 / 收尾失败绝不能记为成功 ----
// 直接调 S08 执行器：走 RepairEngine 会在转码失败后自动回退保码流重封装，
// 那会把"转码失败"掩盖成一次成功的 remux，测不到我们要钉的那段收尾逻辑。
TEST(RepairTolerantTranscodeTests, WriteFailureAfterPartialWriteIsNotSuccess) {
    struct Case {
        int nth_write;
        bool trailer;
        const char* tag;
    };
    for (const Case& item : {Case{3, false, "packet"}, Case{0, true, "trailer"}}) {
        TranscodeFaultGuard guard;
        repair::SetTranscodeWriteFailureForTesting(item.nth_write);
        repair::SetTranscodeTrailerFailureForTesting(item.trailer);

        const std::string dir = MakeTempDir(std::string("f08_") + item.tag);
        const auto media = videoeye_test::CreateTestMedia(dir, "write_src");
        ASSERT_TRUE(media.ok) << item.tag << ": " << media.error;
        RepairRequest request = MakeRequest(media.path, dir, RepairVerificationLevel::V1_FrameSample);
        request.mode = RepairMode::RecoverContent;
        const auto paths = repair::RepairOutputWriter::Resolve(request, "transcode-fault-test");
        ASSERT_TRUE(paths.valid) << paths.error;
        model::RepairDiagnosis diagnosis;
        model::RepairPlan plan;
        const auto outcome =
            repair::RepairTolerantTranscode::Execute(request, diagnosis, plan, paths, nullptr, nullptr, nullptr);
        repair::RepairOutputWriter::Discard(paths.part_path);

        EXPECT_FALSE(outcome.ok) << item.tag << "：写过包也不能把写入 / 封装失败报成成功转码";
        EXPECT_FALSE(outcome.error.empty()) << item.tag;
        EXPECT_GT(outcome.packets_written, 0u) << item.tag << "：必须真的写过部分包，才测得到旧 bug";
        bool has_failure_finding = false;
        for (const auto& finding : outcome.residual_findings) {
            if (finding.code == "output.transcode_write_failed")
                has_failure_finding = true;
        }
        EXPECT_TRUE(has_failure_finding) << item.tag << "：写入失败要作为残留问题传出";
        for (const auto& execution : outcome.executions)
            EXPECT_FALSE(execution.action == RepairAction::S08_TolerantTranscode && execution.succeeded)
                << item.tag << "：不能伪造一条成功的转码执行记录";
    }
}

// ---- B 切片：样本层事实落到三份报告，且上三层计数确实由它派生 ----
TEST(RepairEngine, SampleLedgerIsPublishedInAllReportFormatsAndMatchesUpperLayers) {
    const std::string dir = MakeTempDir("sample_ledger");
    const auto media = videoeye_test::CreateTestMedia(dir, "ledger_src");
    ASSERT_TRUE(media.ok) << media.error;
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    ASSERT_TRUE(result.sample_ledger.available) << "保码流路径必须接入样本账本";
    EXPECT_GT(result.sample_ledger.total_samples, 0u) << "有产物内容时账本不得为空";
    EXPECT_EQ(result.sample_ledger.by_track_count, result.sample_ledger.total_samples);
    uint64_t track_sum = 0;
    for (const auto& track : result.track_conservation)
        track_sum += track.packets_written;
    EXPECT_EQ(track_sum, result.sample_ledger.total_samples) << "逐轨包数必须由样本层求和";
    const std::string first_problem =
        result.sample_conservation_problems.empty() ? std::string() : result.sample_conservation_problems.front();
    EXPECT_TRUE(result.sample_conservation_problems.empty()) << first_problem;

    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string json = ReadText(dir + "/r.json");
    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("样本账本"), std::string::npos);
        EXPECT_NE(body.find("样本层实测样本数"), std::string::npos);
        EXPECT_NE(body.find(std::to_string(result.sample_ledger.total_samples)), std::string::npos);
    }
    EXPECT_NE(json.find("\"sample_ledger\""), std::string::npos);
    EXPECT_NE(json.find("\"total_samples\""), std::string::npos);
    EXPECT_NE(json.find("\"conservation_problems\""), std::string::npos);
}

// ---- B 切片：分块大小不影响样本边界，两个 producer 的产物逐字节一致 ----
TEST(RepairPlainMp4RebuilderTests, ChunkSizeDoesNotChangeRebuiltOutputForBothProducers) {
    auto read_bytes = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    struct Case {
        const char* tag;
        std::string source;
        std::vector<std::size_t> chunks;
    };

    const std::string dir = MakeTempDir("plain_chunks");
    const std::string adts = dir + "/adts_only.mp4";
    ASSERT_TRUE(WriteAdtsOnlyMp4(adts, 3072, 8));

    const std::vector<Case> cases = {
        {"adts", adts, {0, 7, 13, 64, 4096, 65536}},
        {"h264", RealCorpusSample("real_h264_inband_missing_moov.mp4"), {0, 13, 4096, 65536}},
    };
    for (const auto& item : cases) {
        if (!std::filesystem::exists(item.source)) {
            GTEST_SKIP() << "缺受控语料 " << item.source;
        }
        std::vector<uint8_t> baseline;
        uint64_t baseline_samples = 0;
        for (const std::size_t chunk : item.chunks) {
            const std::string out = dir + "/" + item.tag + "_" + std::to_string(chunk) + ".mp4";
            const auto rebuilt = repair::RepairPlainMp4Rebuilder::Rebuild(item.source, out, nullptr, nullptr, chunk);
            ASSERT_TRUE(rebuilt.ok) << item.tag << " chunk=" << chunk << ": " << rebuilt.error;
            ASSERT_TRUE(rebuilt.sample_ledger.available);
            EXPECT_EQ(rebuilt.samples_rebuilt, rebuilt.sample_ledger.total_samples)
                << item.tag << " chunk=" << chunk << "：样本数必须由账本求和";
            EXPECT_GT(rebuilt.sample_ledger.total_bytes, 0u);
            const auto bytes = read_bytes(out);
            ASSERT_FALSE(bytes.empty());
            if (chunk == 0) {
                baseline = bytes;
                baseline_samples = rebuilt.samples_rebuilt;
            } else {
                EXPECT_EQ(bytes, baseline) << item.tag << " chunk=" << chunk << "：产物必须逐字节一致";
                EXPECT_EQ(rebuilt.samples_rebuilt, baseline_samples);
            }
        }
        EXPECT_GT(baseline_samples, 0u);
    }
}

// ============================================================
// 切片 K：真实受控损坏语料上的结论层守护
//
// A–J 每个切片都给自己的字段写了单测，但「真实语料跑完整条链之后，报告还能不能
// 逐轨说明恢复了什么、损失了什么」一直没人验。这里用 generate_real_corpus.py 造的
// 受控损坏语料（有原片基准）钉死四条：
//   1. 保码流路径：逐轨覆盖 / 共同区间 / 时基依据必须有可核对的值；
//   2. 截断路径：不得把「只扫到前缀」标成完整覆盖；
//   3. S08 转码路径：有账本就必须上抛逐轨覆盖，且**不许伪造输入侧时间区间**
//      （产物时间轴按推定帧率从零重建，与损失区间不同轴，不能相减）；
//   4. 未测量 / 未取得证据 / 未接入统计这三态必须在三种格式里显式出行，
//      且「有账本但没统计」不许被写成「没有账本」。
//
// 语料缺失时这里用 ASSERT 而不是 GTEST_SKIP：结论层守护一旦静默跳过，
// 整条链就又回到「只有零件级证据」的状态，而且没人会发现。
// ============================================================

struct ConclusionLayerProbe {
    RepairResult result;
    std::string dir;
    std::string json;
    std::string text;
    std::string html;
};

void WriteConclusionReports(ConclusionLayerProbe* probe, const std::string& prefix) {
    const auto written = reporting::RepairReportExporter::WriteAll(probe->result, probe->dir + "/" + prefix + ".json",
                                                                   probe->dir + "/" + prefix + ".txt",
                                                                   probe->dir + "/" + prefix + ".html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    probe->json = ReadText(probe->dir + "/" + prefix + ".json");
    probe->text = ReadText(probe->dir + "/" + prefix + ".txt");
    probe->html = ReadText(probe->dir + "/" + prefix + ".html");
}

ConclusionLayerProbe RunConclusionLayer(const char* sample, RepairMode mode, const char* tag) {
    ConclusionLayerProbe probe;
    probe.dir = MakeTempDir(std::string("k_") + tag);
    RepairRequest request =
        MakeRequest(RealCorpusSample(sample), probe.dir + "/out", RepairVerificationLevel::V1_FrameSample);
    request.mode = mode;
    probe.result = repair::RepairEngine::Run(request, nullptr, nullptr);
    if (probe.result.output_published)
        WriteConclusionReports(&probe, "r");
    return probe;
}

bool ConclusionTextHas(const ConclusionLayerProbe& probe, const char* needle) {
    // TXT 是 "key: value"，HTML 是 <th>key</th><td>value</td> —— 带冒号的整串
    // 在 HTML 里永远找不到。要求 TXT 出现完整行、HTML 同时出现 key 与 value，
    // 既不放松「三格式同源」，也不会因排版差异假红。
    const std::string full(needle);
    const std::size_t sep = full.find(": ");
    if (sep == std::string::npos)
        return probe.text.find(full) != std::string::npos && probe.html.find(full) != std::string::npos;
    const std::string key = full.substr(0, sep);
    const std::string value = full.substr(sep + 2);
    return probe.text.find(full) != std::string::npos && probe.html.find(key) != std::string::npos &&
           probe.html.find(value) != std::string::npos;
}

bool ConclusionTextLacks(const ConclusionLayerProbe& probe, const char* needle) {
    return probe.text.find(needle) == std::string::npos && probe.html.find(needle) == std::string::npos;
}

void RequireRealCorpus(const char* sample) {
    ASSERT_TRUE(std::filesystem::exists(RealCorpusSample(sample)))
        << "缺受控损坏语料 " << sample << "，先跑 tests/corpus/generate_real_corpus.py";
}

TEST(RepairEngine, RealCorpusPreserveModeReportsFullConclusionLayer) {
    RequireRealCorpus("real_h264_aac_faststart.mp4");
    RequireRealCorpus("real_h264_aac_mid_damaged.mp4");

    struct Case {
        const char* tag;
        const char* sample;
        bool expect_loss;
    };
    const Case cases[] = {
        {"k_preserve_healthy", "real_h264_aac_faststart.mp4", false},
        {"k_preserve_mid", "real_h264_aac_mid_damaged.mp4", true},
    };
    for (const auto& c : cases) {
        const ConclusionLayerProbe probe = RunConclusionLayer(c.sample, RepairMode::PreserveBitstream, c.tag);
        ASSERT_TRUE(probe.result.output_published)
            << c.tag << ": " << probe.result.summary << " | " << probe.result.error;
        // 真实语料是音视频混合文件，逐轨覆盖必须真的接了账本且两轨都在。
        EXPECT_TRUE(probe.result.sample_ledger.available) << c.tag;
        ASSERT_TRUE(probe.result.track_coverage.available)
            << c.tag << ": " << probe.result.track_coverage.unavailable_reason;
        ASSERT_EQ(probe.result.track_coverage.tracks.size(), 2u) << c.tag;
        for (const auto& track : probe.result.track_coverage.tracks) {
            EXPECT_GT(track.samples, 0u) << c.tag;
            EXPECT_TRUE(track.span_known) << c.tag << "：保码流路径样本带容器时间戳";
            EXPECT_EQ(track.samples_without_timestamp, 0u) << c.tag;
            EXPECT_FALSE(track.codec.empty()) << c.tag;
        }
        EXPECT_TRUE(probe.result.track_coverage.common_span_known) << c.tag;
        EXPECT_GT(probe.result.track_coverage.common_end_seconds, probe.result.track_coverage.common_start_seconds)
            << c.tag << "：音画共同覆盖不能是空区间";
        EXPECT_EQ(probe.result.timeline_basis.mode, "container_pts") << c.tag;
        EXPECT_FALSE(probe.result.fidelity.transcode.inspected) << c.tag << "：保码流路径不许声称做过转码属性探测";
        EXPECT_EQ(probe.result.lost_ranges.empty(), !c.expect_loss) << c.tag;

        // 三格式同源：JSON 有结构，TXT/HTML 有同一批可读结论。
        EXPECT_NE(probe.json.find("\"track_coverage\""), std::string::npos) << c.tag;
        EXPECT_NE(probe.json.find("\"timeline_basis\""), std::string::npos) << c.tag;
        EXPECT_NE(probe.json.find("\"display_order\""), std::string::npos) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "时基依据: 容器时间戳")) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "音画共同覆盖")) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "轨 0（video")) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "转码保真探测: 未探测")) << c.tag;
        // 显示顺序本策略没测，必须显式说「未测量」，不能整节消失。
        EXPECT_TRUE(ConclusionTextHas(probe, "显示顺序: 未测量")) << c.tag;
        EXPECT_TRUE(ConclusionTextLacks(probe, "本次执行没有样本账本")) << c.tag;
    }
}

TEST(RepairEngine, RealCorpusTailTruncationNeverMarksCoverageComplete) {
    RequireRealCorpus("real_h264_aac_tail_truncated.mp4");
    const ConclusionLayerProbe probe =
        RunConclusionLayer("real_h264_aac_tail_truncated.mp4", RepairMode::PreserveBitstream, "k_tail");
    ASSERT_TRUE(probe.result.output_published) << probe.result.summary << " | " << probe.result.error;
    ASSERT_TRUE(probe.result.track_coverage.available);
    ASSERT_EQ(probe.result.track_coverage.tracks.size(), 2u);
    // 尾部被截断 = 没走到源末尾。此时逐轨 span 只是前缀，标 complete 会让
    // 「扫了前半段」被读成「整段都检查过、没有缺口」。
    EXPECT_FALSE(probe.result.track_coverage.complete) << probe.result.summary;
    EXPECT_TRUE(ConclusionTextHas(probe, "逐轨覆盖只统计到已扫描前缀"));
    EXPECT_NE(probe.json.find("\"complete\": false"), std::string::npos);
}

TEST(RepairEngine, RealCorpusTranscodePathReportsTrackCoverageWithoutFakeSpans) {
    RequireRealCorpus("real_h264_aac_faststart.mp4");
    RequireRealCorpus("real_h264_aac_mid_damaged.mp4");

    struct Case {
        const char* tag;
        const char* sample;
    };
    const Case cases[] = {
        {"k_recover_healthy", "real_h264_aac_faststart.mp4"},
        {"k_recover_mid", "real_h264_aac_mid_damaged.mp4"},
    };
    for (const auto& c : cases) {
        const ConclusionLayerProbe probe = RunConclusionLayer(c.sample, RepairMode::RecoverContent, c.tag);
        ASSERT_TRUE(probe.result.output_published)
            << c.tag << ": " << probe.result.summary << " | " << probe.result.error;
        EXPECT_TRUE(probe.result.fidelity.transcode.inspected) << c.tag;
        EXPECT_TRUE(probe.result.fidelity.video_reencoded) << c.tag;
        EXPECT_TRUE(probe.result.sample_ledger.available) << c.tag;
        EXPECT_GT(probe.result.sample_ledger.total_samples, 0u) << c.tag;

        // 核心回归：S08 接了样本账本，逐轨覆盖就不许再说「未接入」。
        ASSERT_TRUE(probe.result.track_coverage.available)
            << c.tag << "：有 " << probe.result.sample_ledger.total_samples
            << " 个样本却把逐轨覆盖整块丢掉，报告会写成「没有账本」";
        EXPECT_TRUE(probe.result.track_coverage.complete) << c.tag << "：正常跑到源末尾，逐轨覆盖不该被标成未完成";
        ASSERT_EQ(probe.result.track_coverage.tracks.size(), 2u) << c.tag;
        for (const auto& track : probe.result.track_coverage.tracks) {
            EXPECT_GT(track.samples, 0u) << c.tag;
            EXPECT_FALSE(track.codec.empty()) << c.tag;
            EXPECT_FALSE(track.kind.empty()) << c.tag;
            // S08 样本没有输入侧时间戳：不许填假 span，也不许把「没有时间戳」
            // 的样本算成缺口（0 缺口是「未判定」，不是「无损」）。
            EXPECT_FALSE(track.span_known) << c.tag;
            EXPECT_EQ(track.samples_without_timestamp, track.samples) << c.tag;
            EXPECT_EQ(track.gap_count, 0u) << c.tag;
        }
        EXPECT_FALSE(probe.result.track_coverage.common_span_known) << c.tag;
        EXPECT_FALSE(probe.result.track_coverage.common_span_note.empty()) << c.tag;
        bool axis_limited = false;
        for (const auto& item : probe.result.track_coverage.limitations)
            if (item.find("不可与损失区间相减") != std::string::npos)
                axis_limited = true;
        EXPECT_TRUE(axis_limited) << c.tag << "：产物时间轴与输入时间轴不同轴，必须写明不可相减";

        EXPECT_EQ(probe.result.timeline_basis.mode, "unknown") << c.tag;
        EXPECT_EQ(probe.result.fidelity.timeline_status, "modified") << c.tag;

        EXPECT_NE(probe.json.find("\"track_coverage\""), std::string::npos) << c.tag;
        EXPECT_NE(probe.json.find("\"samples_without_timestamp\": \""), std::string::npos) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "覆盖区间未知")) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "未取得源侧时基证据")) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "显示顺序: 未测量")) << c.tag;
        EXPECT_TRUE(ConclusionTextHas(probe, "不可与损失区间相减")) << c.tag;
        // 反向守护：账本明明在，任何格式都不能写「本次执行没有样本账本」。
        EXPECT_TRUE(ConclusionTextLacks(probe, "本次执行没有样本账本")) << c.tag;
    }
}

TEST(RepairEngine, CoverageUnavailableReasonDistinguishesMissingLedgerFromMissingStats) {
    RequireRealCorpus("real_h264_aac_faststart.mp4");
    ConclusionLayerProbe probe =
        RunConclusionLayer("real_h264_aac_faststart.mp4", RepairMode::PreserveBitstream, "k_reason");
    ASSERT_TRUE(probe.result.output_published);

    // 有账本但没上抛统计：只准说「没统计」，不准说「没有账本」。
    probe.result.track_coverage = RepairTrackCoverageSummary{};
    probe.result.track_coverage.available = false;
    probe.result.sample_ledger.available = true;
    WriteConclusionReports(&probe, "has_ledger");
    EXPECT_TRUE(ConclusionTextHas(probe, "有样本账本但未产出逐轨覆盖统计"));
    EXPECT_TRUE(ConclusionTextLacks(probe, "本次执行没有样本账本"));

    // 反向：确实没账本时才允许这句，且原因要能区分开。
    probe.result.sample_ledger.available = false;
    WriteConclusionReports(&probe, "no_ledger");
    EXPECT_TRUE(ConclusionTextHas(probe, "本次执行没有样本账本"));
    EXPECT_TRUE(ConclusionTextLacks(probe, "有样本账本但未产出逐轨覆盖统计"));
}

TEST(RepairEngine, TimelineUnknownAndDisplayOrderUnmeasuredAreExplicitInAllFormats) {
    RequireRealCorpus("real_h264_aac_faststart.mp4");
    ConclusionLayerProbe probe =
        RunConclusionLayer("real_h264_aac_faststart.mp4", RepairMode::PreserveBitstream, "k_unknown");
    ASSERT_TRUE(probe.result.output_published);

    // 真实语料是 container_pts：先钉住「有证据」时的正向文案，防止下面的
    // 改动只是把两个分支都写成同一句。
    ASSERT_EQ(probe.result.timeline_basis.mode, "container_pts");
    EXPECT_TRUE(ConclusionTextHas(probe, "时基依据: 容器时间戳"));
    EXPECT_TRUE(ConclusionTextLacks(probe, "未取得源侧时基证据"));

    probe.result.timeline_basis.mode = "unknown";
    WriteConclusionReports(&probe, "unknown");
    EXPECT_TRUE(ConclusionTextHas(probe, "未取得源侧时基证据（unknown）")) << probe.text;
    EXPECT_TRUE(ConclusionTextHas(probe, "不能作为源片时间轴的证据")) << probe.text;
    EXPECT_NE(probe.json.find("\"mode\": \"unknown\""), std::string::npos);
    EXPECT_TRUE(ConclusionTextLacks(probe, "时基依据: 容器时间戳"));

    // 显示顺序：analyzed=false 时必须有「未测量」行，不能被静默省略成
    // 「看起来测过且没问题」。
    probe.result.display_order.analyzed = false;
    probe.result.display_order.available = false;
    WriteConclusionReports(&probe, "unmeasured");
    EXPECT_TRUE(ConclusionTextHas(probe, "显示顺序: 未测量")) << probe.text;
    EXPECT_TRUE(ConclusionTextHas(probe, "不能据此断言显示序与源片一致")) << probe.text;
    EXPECT_NE(probe.json.find("\"analyzed\": false"), std::string::npos);
}
