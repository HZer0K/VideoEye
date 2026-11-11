#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairReferenceMoovRebuilder.h"
#include "core/reporting/RepairReportExporter.h"
#include "tests/support/MediaSequenceFingerprint.h"
#include "tests/support/TestMediaFactory.h"

namespace {

// R3：tests/unit 下不得声明 videoeye::repair 具名子命名空间。
using namespace videoeye::repair;
namespace model = videoeye::model;
namespace reporting = videoeye::reporting;
namespace fs = std::filesystem;

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_refmoov_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void Put32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

// 与 scripts/generate_real_corpus.py 的 drop_top_level_box 一致：保留其它顶层盒，
// 尾部残留字节也照搬，产物就是「从完整片受控制造的缺 moov 损坏片」。
bool DropTopLevelBox(const std::string& source, const std::string& dest, const std::string& target,
                     std::string* error) {
    const std::vector<uint8_t> data = ReadFile(source);
    std::vector<uint8_t> out;
    std::uint64_t offset = 0;
    bool found = false;
    while (offset + 8 <= data.size()) {
        const std::uint64_t size32 = Be32(data.data() + offset);
        const std::string type(reinterpret_cast<const char*>(data.data() + offset + 4), 4);
        std::uint64_t size = size32;
        std::uint64_t header = 8;
        if (size32 == 1) {
            if (offset + 16 > data.size())
                break;
            size = 0;
            for (int i = 0; i < 8; ++i)
                size = (size << 8) | data[offset + 8 + i];
            header = 16;
        } else if (size32 == 0) {
            size = data.size() - offset;
        }
        if (size < header || offset + size > data.size()) {
            *error = "顶层盒越界: " + type;
            return false;
        }
        if (type == target) {
            found = true;
        } else {
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                       data.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }
        offset += size;
    }
    if (!found) {
        *error = "找不到顶层盒 " + target;
        return false;
    }
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.end());
    return WriteFile(dest, out);
}

std::string CorpusPath(const char* name) {
    return std::string(VIDEOEYE_CONTAINER_CORPUS_DIR) + "/" + name;
}

videoeye_test::TestMedia MakeMedia(const std::string& dir, const std::string& name,
                                   const videoeye_test::TestMediaSpec& spec = {}) {
    return videoeye_test::CreateTestMedia(dir, name, spec);
}

// 篡改音频轨 mdhd 的 timescale，模拟「参考样片时基被改写」的 R03 事故。
bool PatchAudioTimescale(const std::string& source, const std::string& dest, uint32_t value, std::string* error) {
    videoeye::IsobmffFile parsed;
    if (!videoeye::IsobmffParser::Parse(source, parsed) || !parsed.ok || parsed.tracks.size() != 2) {
        *error = "参考片无法解析或轨道数不是 2";
        return false;
    }
    std::vector<uint8_t> data = ReadFile(source);
    const videoeye::IsobmffBox* moov = nullptr;
    for (const auto& box : parsed.top_level) {
        if (box.type == "moov")
            moov = &box;
    }
    if (moov == nullptr) {
        *error = "没有 moov";
        return false;
    }
    std::vector<const videoeye::IsobmffBox*> traks;
    for (const auto& child : moov->children) {
        if (child.type == "trak")
            traks.push_back(&child);
    }
    if (traks.size() != 2) {
        *error = "trak 数不是 2";
        return false;
    }
    const videoeye::IsobmffBox* mdhd = nullptr;
    for (const auto& child : traks[1]->children) {
        if (child.type == "mdia") {
            for (const auto& sub : child.children) {
                if (sub.type == "mdhd")
                    mdhd = &sub;
            }
        }
    }
    if (mdhd == nullptr) {
        *error = "找不到音频 mdhd";
        return false;
    }
    const std::uint64_t at = mdhd->offset + mdhd->header_size + 12; // version0: +4+4+4
    if (at + 4 > data.size()) {
        *error = "mdhd timescale 字段越界";
        return false;
    }
    Put32(data.data() + at, value);
    return WriteFile(dest, data);
}

// 只裁掉 mdat 载荷尾部 N 字节，并同步改小 mdat 盒大小 —— 顶层盒链仍然合法，
// 不会在 ReadSourceTopLevel 就被「盒大小越界」拦下。这样唯一能拒绝它的就是
// 源侧逐 chunk 字节守恒（参考只给线索，样本边界必须在损坏文件里自证）。
// 直接删文件尾 N 字节的旧做法会让 mdat 盒头先越界，测不到这条链。
bool TrimMdatPayload(const std::string& source, const std::string& dest, std::uint32_t bytes, std::string* error) {
    std::vector<uint8_t> data = ReadFile(source);
    std::uint64_t offset = 0;
    while (offset + 8 <= data.size()) {
        const std::uint32_t size32 = Be32(data.data() + offset);
        const std::string type(reinterpret_cast<const char*>(data.data() + offset + 4), 4);
        if (size32 == 0 || size32 == 1 || offset + size32 > data.size()) {
            *error = "顶层盒不是 32 位定长盒或已越界";
            return false;
        }
        if (type == "mdat") {
            if (size32 <= 8 + bytes || offset + size32 != data.size()) {
                *error = "mdat 不是文件末盒，或剩余载荷不足以裁剪";
                return false;
            }
            Put32(data.data() + offset, size32 - bytes);
            data.resize(data.size() - bytes);
            return WriteFile(dest, data);
        }
        offset += size32;
    }
    *error = "找不到顶层 mdat";
    return false;
}

// A 阶段已记录：测试工厂给视频最后一帧写 stts delta=0，源侧解码只出 39 帧，
// 而任何保码流产物都会解出全部 40 帧（包序列完全一致）。这是生成器的尾部
// 冲刷边界，不是重建丢帧。因此这里不把「源 vs 产物逐帧亮度哈希」当判据，
// 换成三条更硬的事实：逐包载荷序列完全一致、相邻 PTS 间隔（秒）一致、
// 产物自身解码帧数 == 包数。任何真的丢包 / 换序 / 改帧率都会让它们判红。
std::vector<std::string> ComparePerTrackFacts(const videoeye_test::MediaSequence& expected,
                                              const videoeye_test::MediaSequence& actual) {
    std::vector<std::string> diffs;
    if (!expected.opened || !actual.opened || !expected.error.empty() || !actual.error.empty())
        return {"源或产物取证不完整"};
    if (expected.total_packets == 0 || actual.total_packets == 0)
        return {"存在零包文件，无法证明一致"};
    if (expected.streams.size() != actual.streams.size())
        return {"流数不一致: " + std::to_string(expected.streams.size()) + " vs " +
                std::to_string(actual.streams.size())};
    for (std::size_t i = 0; i < expected.streams.size(); ++i) {
        const auto& a = expected.streams[i];
        const auto& b = actual.streams[i];
        const std::string tag = "stream[" + std::to_string(i) + "] ";
        if (a.packets.empty() || b.packets.empty())
            diffs.push_back(tag + "存在空流");
        if (a.codec_type != b.codec_type)
            diffs.push_back(tag + "codec_type: " + a.codec_type + " vs " + b.codec_type);
        if (a.codec_name != b.codec_name)
            diffs.push_back(tag + "codec_name: " + a.codec_name + " vs " + b.codec_name);
        if (a.packets.size() != b.packets.size())
            diffs.push_back(tag + "包数: " + std::to_string(a.packets.size()) + " vs " +
                            std::to_string(b.packets.size()));
        if (a.payload_sequence_hash != b.payload_sequence_hash)
            diffs.push_back(tag + "逐包载荷序列不一致（顺序/内容被改写）");
        if (!b.dts_monotonic)
            diffs.push_back(tag + "产物 DTS 不单调");
        const auto delta_seconds = [](const videoeye_test::StreamSequence& s, std::size_t k) {
            const std::int64_t ticks = s.packets[k].pts - s.packets[k - 1].pts;
            return s.time_base_den > 0 ? ticks * static_cast<double>(s.time_base_num) / s.time_base_den
                                       : static_cast<double>(ticks);
        };
        const std::size_t n = std::min(a.packets.size(), b.packets.size());
        for (std::size_t k = 1; k < n; ++k) {
            if (!a.packets[k].has_pts || !b.packets[k].has_pts)
                continue;
            if (std::abs(delta_seconds(a, k) - delta_seconds(b, k)) > 0.001) {
                diffs.push_back(tag + "第 " + std::to_string(k) + " 个 PTS 间隔(秒)不一致");
                break;
            }
        }
    }
    return diffs;
}

struct Fixture {
    std::string dir;
    std::string reference;
    std::string damaged;
    std::string output;
    videoeye_test::TestMedia media;
};

Fixture MakeFixture(const std::string& tag, const videoeye_test::TestMediaSpec& spec = {}) {
    Fixture f;
    f.dir = MakeTempDir(tag);
    f.reference = f.dir + "/reference.mp4";
    f.damaged = f.dir + "/damaged_missing_moov.mp4";
    f.output = f.dir + "/rebuilt.mp4";
    f.media = MakeMedia(f.dir, "reference", spec);
    if (f.media.ok) {
        std::string error;
        DropTopLevelBox(f.media.path, f.damaged, "moov", &error);
    }
    return f;
}

} // namespace

TEST(RepairReferenceMoov, ReadProfileAcceptsHealthyTwoTrackReference) {
    Fixture f = MakeFixture("profile_ok");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    const RepairReferenceProfile profile = RepairReferenceMoovRebuilder::ReadProfile(f.media.path);
    ASSERT_TRUE(profile.ok) << profile.error;
    EXPECT_EQ(profile.compatibility, "verified");
    EXPECT_GT(profile.video.sample_count, 0u);
    EXPECT_GT(profile.audio.sample_count, 0u);
    EXPECT_GT(profile.video.width, 0u);
    EXPECT_GT(profile.video.height, 0u);
    EXPECT_EQ(profile.audio.sample_delta, 1024u);
    EXPECT_EQ(profile.audio.timescale, profile.audio.sample_rate);
    EXPECT_FALSE(profile.ftyp.empty());
    EXPECT_FALSE(profile.video.sample_entry.empty());
    EXPECT_FALSE(profile.audio.sample_entry.empty());
    EXPECT_FALSE(profile.chunks.empty());
    EXPECT_FALSE(profile.layout_fingerprint.empty());
    EXPECT_GE(profile.borrowed_parameters.size(), 4u);
    EXPECT_FALSE(profile.limitations.empty());

    std::uint64_t video_samples = 0;
    std::uint64_t audio_samples = 0;
    for (const auto& chunk : profile.chunks) {
        if (chunk.track == 0)
            video_samples += chunk.samples;
        else
            audio_samples += chunk.samples;
    }
    EXPECT_EQ(video_samples, profile.video.sample_count);
    EXPECT_EQ(audio_samples, profile.audio.sample_count);
}

TEST(RepairReferenceMoov, ReadProfileAcceptsBFrameReferenceButDoesNotBorrowCtts) {
    const std::string bframe = CorpusPath("real_h264_aac_faststart.mp4");
    ASSERT_TRUE(fs::exists(bframe)) << "缺少受控语料 " << bframe;
    const RepairReferenceProfile profile = RepairReferenceMoovRebuilder::ReadProfile(bframe);
    // 切片 H：含 ctts 的参考不再拒绝（否则「有 B 帧 + 缺 moov」是死路），
    // 但合成偏移一律不借 —— 那是「这份文件的帧」的属性。
    ASSERT_TRUE(profile.ok) << profile.error;
    EXPECT_TRUE(profile.video.has_ctts) << "该语料含 B 帧 ctts，profile 必须记录它";
    for (const auto& item : profile.borrowed_parameters)
        EXPECT_EQ(item.find("ctts"), std::string::npos) << "不得把 ctts 写进借用清单: " << item;
    bool noted = false;
    for (const auto& item : profile.limitations)
        if (item.find("不借用其合成偏移") != std::string::npos)
            noted = true;
    EXPECT_TRUE(noted) << "不借用这件事必须写进限制项";

    Fixture f = MakeFixture("profile_tamper");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    const std::string tampered = f.dir + "/tampered_timescale.mp4";
    std::string error;
    ASSERT_TRUE(PatchAudioTimescale(f.media.path, tampered, 96000, &error)) << error;
    const RepairReferenceProfile rejected_timescale = RepairReferenceMoovRebuilder::ReadProfile(tampered);
    EXPECT_FALSE(rejected_timescale.ok);
    EXPECT_NE(rejected_timescale.error.find("timescale"), std::string::npos) << rejected_timescale.error;
}

// 切片 H 端到端：有 B 帧的参考（含 ctts）+ 缺 moov 的同源损坏片，
// 重建后必须写出由**源侧码流 POC** 实测的 ctts，且逐包 PTS 序列与原片一致。
TEST(RepairReferenceMoov, RebuildWithBFrameReferenceWritesMeasuredCtts) {
    const std::string reference = CorpusPath("real_h264_aac_faststart.mp4");
    ASSERT_TRUE(fs::exists(reference)) << "缺少受控语料 " << reference;
    const std::string dir = MakeTempDir("bframes");
    const std::string damaged = dir + "/damaged_missing_moov.mp4";
    const std::string output = dir + "/rebuilt.mp4";
    std::string error;
    ASSERT_TRUE(DropTopLevelBox(reference, damaged, "moov", &error)) << error;

    const auto result = RepairReferenceMoovRebuilder::Rebuild(damaged, reference, output, nullptr);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.profile.video.has_ctts) << "参考含 ctts 这一事实必须带进结果";
    ASSERT_TRUE(result.display_order.analyzed) << "显示顺序必须真的做过测量";
    ASSERT_TRUE(result.display_order.available) << result.display_order.unavailable_reason;
    EXPECT_GT(result.display_order.reordered_frames, 0u) << "该语料有 B 帧重排";
    EXPECT_TRUE(result.display_order.ctts_required);
    EXPECT_TRUE(result.display_order.ctts_written) << "有重排就必须写 ctts";

    videoeye::IsobmffFile rebuilt;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(output, rebuilt));
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error_message;
    bool video_has_ctts = false;
    for (const auto& track : rebuilt.tracks)
        if (track.handler == "vide")
            video_has_ctts = track.has_ctts;
    EXPECT_TRUE(video_has_ctts) << "重建产物的视频轨必须有 ctts";

    // 最硬的判据：逐包 PTS 间隔与原片一致。ctts 缺一条 / 偏移算错都会让它判红。
    const auto expected = videoeye_test::CaptureMediaSequence(reference);
    const auto actual = videoeye_test::CaptureMediaSequence(output);
    const auto diffs = ComparePerTrackFacts(expected, actual);
    for (const auto& diff : diffs)
        ADD_FAILURE() << diff;
    EXPECT_TRUE(diffs.empty());
}

TEST(RepairReferenceMoov, ProbeAcceptsMissingMoovWithSameSourceReference) {
    Fixture f = MakeFixture("probe_ok");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    ASSERT_TRUE(fs::exists(f.damaged));
    const auto probe = RepairReferenceMoovRebuilder::ProbeWithReference(f.damaged, f.media.path, nullptr);
    ASSERT_TRUE(probe.attemptable) << probe.reason;
    EXPECT_FALSE(probe.codec_summary.empty());
    EXPECT_FALSE(probe.limitations.empty());

    // 源文件仍含 moov 时不能走重建路径。
    const auto intact = RepairReferenceMoovRebuilder::ProbeWithReference(f.media.path, f.media.path, nullptr);
    EXPECT_FALSE(intact.attemptable);
    EXPECT_NE(intact.reason.find("moov"), std::string::npos) << intact.reason;
}

TEST(RepairReferenceMoov, RebuildPreservesPerTrackSequenceAndTimeline) {
    Fixture f = MakeFixture("rebuild_ok");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    const auto result = RepairReferenceMoovRebuilder::Rebuild(f.damaged, f.media.path, f.output, nullptr);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.rebuilt_tracks, 2u);
    EXPECT_EQ(result.samples_rebuilt, result.profile.video.sample_count + result.profile.audio.sample_count);
    EXPECT_GT(result.samples_rebuilt, 0u);
    EXPECT_TRUE(result.sample_ledger.available);
    EXPECT_EQ(result.sample_ledger.total_samples, result.samples_rebuilt);
    EXPECT_EQ(result.sample_ledger.by_track_count, result.samples_rebuilt);
    EXPECT_TRUE(result.assist.used);
    EXPECT_EQ(result.assist.compatibility, "verified");
    EXPECT_FALSE(result.assist.layout_evidence.empty());
    EXPECT_FALSE(result.assist.borrowed_parameters.empty());
    EXPECT_FALSE(result.assist.limitations.empty());

    videoeye::IsobmffFile rebuilt;
    ASSERT_TRUE(videoeye::IsobmffParser::Parse(f.output, rebuilt));
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error_message;
    EXPECT_TRUE(rebuilt.found_moov);
    EXPECT_EQ(rebuilt.tracks.size(), 2u);

    const auto expected = videoeye_test::CaptureMediaSequence(f.media.path);
    const auto actual = videoeye_test::CaptureMediaSequence(f.output);
    const auto diffs = ComparePerTrackFacts(expected, actual);
    for (const auto& diff : diffs)
        ADD_FAILURE() << diff;
    EXPECT_TRUE(diffs.empty());
    // 产物自身必须逐帧可解码，不能靠"包序列一致"掩盖解不出画面。
    const auto decoded = videoeye_test::CaptureMediaSequence(f.output, true);
    ASSERT_FALSE(decoded.streams.empty());
    EXPECT_TRUE(decoded.error.empty()) << decoded.error;
    for (const auto& stream : decoded.streams) {
        if (stream.codec_type != "video")
            continue;
        EXPECT_EQ(stream.decoded_frames, stream.packets.size());
        EXPECT_FALSE(stream.frame_sequence_hash.empty());
        EXPECT_TRUE(stream.decode_error.empty()) << stream.decode_error;
    }
}

TEST(RepairReferenceMoov, RebuildRejectsForeignReferenceAndTruncatedPayload) {
    Fixture f = MakeFixture("rebuild_foreign");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    videoeye_test::TestMediaSpec other_spec;
    other_spec.frame_count = 24;
    other_spec.width = 96;
    other_spec.height = 54;
    const auto other = MakeMedia(f.dir, "foreign_reference", other_spec);
    ASSERT_TRUE(other.ok) << other.error;
    const auto foreign = RepairReferenceMoovRebuilder::ProbeWithReference(f.damaged, other.path, nullptr);
    EXPECT_FALSE(foreign.attemptable);
    EXPECT_FALSE(foreign.reason.empty());

    const std::string truncated = f.dir + "/truncated.mp4";
    std::vector<uint8_t> bytes = ReadFile(f.damaged);
    ASSERT_GT(bytes.size(), 64u);
    bytes.resize(bytes.size() - 32);
    ASSERT_TRUE(WriteFile(truncated, bytes));
    const auto cut = RepairReferenceMoovRebuilder::ProbeWithReference(truncated, f.media.path, nullptr);
    EXPECT_FALSE(cut.attemptable);
    EXPECT_NE(cut.reason.find("完整解释"), std::string::npos) << cut.reason;
}

// 反向守护 + 判别力锚点：顶层盒链合法、只是载荷被裁短时，必须靠源侧
// chunk 字节守恒拒绝；不能因为参考片里有 stsz 就照抄样本大小当真值。
TEST(RepairReferenceMoov, RebuildRejectsPayloadTrimmedSource) {
    Fixture f = MakeFixture("payload_trim");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    const std::string trimmed = f.dir + "/payload_trimmed.mp4";
    std::string error;
    ASSERT_TRUE(TrimMdatPayload(f.damaged, trimmed, 24, &error)) << error;

    const auto probe = RepairReferenceMoovRebuilder::ProbeWithReference(trimmed, f.media.path, nullptr);
    EXPECT_FALSE(probe.attemptable) << probe.reason;
    EXPECT_NE(probe.reason.find("字节不守恒"), std::string::npos)
        << "必须由源侧 chunk 边界守恒拒绝，而不是靠盒头越界: " << probe.reason;

    const auto rebuilt =
        RepairReferenceMoovRebuilder::Rebuild(trimmed, f.media.path, f.dir + "/trimmed_out.mp4", nullptr);
    EXPECT_FALSE(rebuilt.ok) << rebuilt.error;
    EXPECT_NE(rebuilt.error.find("字节不守恒"), std::string::npos) << rebuilt.error;
}

// 端到端：诊断 → 规划 → 参考辅助重建 → 重封装 → V2 验证 → 三格式报告。
// 只在单元测试里验证过重建器不算接入主链路；报告必须说清「借了哪些参数、
// 布局线索怎么在损坏文件里自证」，否则读者会把推断内容当成原片实测。
TEST(RepairReferenceMoov, EngineUsesReferenceAssistAndReportsItInAllFormats) {
    Fixture f = MakeFixture("engine");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    model::RepairRequest request;
    request.source_path = f.damaged;
    request.reference_path = f.media.path;
    request.output_directory = f.dir + "/out";
    request.verification_level = model::RepairVerificationLevel::V2_FullDecode;
    request.write_report = true;
    const model::RepairResult result = RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_TRUE(result.reference_assist.used) << result.summary;
    EXPECT_EQ(result.reference_assist.compatibility, "verified");
    EXPECT_FALSE(result.reference_assist.borrowed_parameters.empty());
    EXPECT_FALSE(result.reference_assist.layout_evidence.empty());
    EXPECT_FALSE(result.reference_assist.timescale_check.empty());
    EXPECT_TRUE(result.sample_ledger.available);
    EXPECT_GT(result.sample_ledger.total_samples, 0u);

    const auto written =
        reporting::RepairReportExporter::WriteAll(result, f.dir + "/r.json", f.dir + "/r.txt", f.dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string json = ReadText(f.dir + "/r.json");
    const std::string text = ReadText(f.dir + "/r.txt");
    const std::string html = ReadText(f.dir + "/r.html");
    EXPECT_NE(json.find("\"reference_assist\""), std::string::npos);
    EXPECT_NE(json.find("\"borrowed_parameters\""), std::string::npos);
    EXPECT_NE(json.find("\"layout_evidence\""), std::string::npos);
    for (const auto& body : {text, html}) {
        EXPECT_NE(body.find("参考样片辅助重建"), std::string::npos) << "TXT / HTML 必须显式说明参考辅助";
        EXPECT_NE(body.find("借用:"), std::string::npos) << "TXT / HTML 必须列出借用参数";
        EXPECT_NE(body.find("源侧布局核验"), std::string::npos);
    }
}

// 反向守护：不同源参考样片不允许被静默接受（不得把「打不开」报成「已修复」）。
TEST(RepairReferenceMoov, EngineRejectsForeignReference) {
    Fixture f = MakeFixture("engine_foreign");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    videoeye_test::TestMediaSpec other;
    other.frame_count = 24;
    other.width = 96;
    other.height = 54;
    const auto foreign = MakeMedia(f.dir, "foreign", other);
    ASSERT_TRUE(foreign.ok) << foreign.error;
    model::RepairRequest request;
    request.source_path = f.damaged;
    request.reference_path = foreign.path;
    request.output_directory = f.dir + "/out";
    request.write_report = false;
    const model::RepairResult result = RepairEngine::Run(request, nullptr, nullptr);
    EXPECT_FALSE(result.output_published) << "不同源参考样片不得发布产物: " << result.summary;
    EXPECT_FALSE(result.reference_assist.used);
    EXPECT_FALSE(result.error.empty());
}

TEST(RepairReferenceMoov, RebuildIsChunkSizeIndependent) {
    Fixture f = MakeFixture("rebuild_chunk");
    ASSERT_TRUE(f.media.ok) << f.media.error;
    const std::string small = f.dir + "/chunk_4k.mp4";
    const std::string large = f.dir + "/chunk_64k.mp4";
    const auto a = RepairReferenceMoovRebuilder::Rebuild(f.damaged, f.media.path, small, nullptr, nullptr, 4096);
    const auto b = RepairReferenceMoovRebuilder::Rebuild(f.damaged, f.media.path, large, nullptr, nullptr, 65536);
    ASSERT_TRUE(a.ok) << a.error;
    ASSERT_TRUE(b.ok) << b.error;
    EXPECT_EQ(ReadFile(small), ReadFile(large));
    EXPECT_EQ(a.samples_rebuilt, b.samples_rebuilt);
}
