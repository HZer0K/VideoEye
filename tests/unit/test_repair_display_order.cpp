// 切片 E：显示顺序证据层（POC → 显示序 → ctts）的正反例回归。
//
// 期望值来源（**不**是把实现输出抄一遍）：
//   * 推导部分：手写帧序列，期望显示序由 POC 语义直接给出
//     —— I P B B 解码序里 POC 是 0 6 2 4，显示序必然是 I B B P，
//     即第 1 个解码帧排第 3 位、第 2 个排第 1 位，偏移为 (显示序−解码序)×帧时长。
//   * 实测部分：用入库的真实 B 帧语料 tests/corpus/codec/real_x264_bframes_gop.mp4，
//     拿**编码器写进容器的 PTS**（显示时间戳）当参考输出：按 POC 推出的显示序
//     必须与按 PTS 排出来的顺序逐项一致。这条对不上就说明推导是自洽的巧合。
//     ❗ 不能用 TestMediaFactory 现场生成 B 帧样本：仓库锁定的预编译 FFmpeg 没有
//     libx264，会降级到 libopenh264，那个编码器不出 B 帧（会静默退化成空跑）。
//
// 明确不在本测试范围（代码里也不做，见 RepairDisplayOrder.h 文件头）：
//   * pic_order_cnt_type == 1、开放 GOP 前导图像 —— 只断言"给不可用 + 原因"；
//   * 帧率推定：没有 VUI / stts 证据时不生成 ctts，只给限制说明。

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairDisplayOrder.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairPlainMp4Rebuilder.h"
#include "core/repair/RepairSampleScan.h"
#include "core/reporting/RepairReportExporter.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

namespace fs = std::filesystem;
namespace model = videoeye::model;
using videoeye::IsobmffFile;
using videoeye::IsobmffParser;
using videoeye::IsobmffTrack;
using videoeye::repair::BuildCttsBox;
using videoeye::repair::DeriveDisplayOrder;
using videoeye::repair::MeasureDisplayOrder;
using videoeye::repair::MemoryByteSource;
using videoeye::repair::RepairAuRange;
using videoeye::repair::RepairDisplayFrame;
using videoeye::repair::RepairDisplayOrderPlan;
using videoeye::repair::RepairFrameDurationEvidence;
using videoeye::repair::ScanWindow;
using videoeye::repair::SummarizeDisplayOrder;

RepairFrameDurationEvidence ConstantDuration(uint32_t timescale, uint32_t delta) {
    RepairFrameDurationEvidence duration;
    duration.source = "vui_timing";
    duration.timescale = timescale;
    duration.sample_delta = delta;
    duration.constant = true;
    duration.detail = "单元测试：恒定样本时长";
    return duration;
}

RepairDisplayFrame Frame(uint64_t decode_index, int64_t poc, bool irap = false) {
    RepairDisplayFrame frame;
    frame.decode_index = decode_index;
    frame.poc = poc;
    frame.irap = irap;
    frame.idr = irap;
    return frame;
}

// 两个 GOP 的 I P B B：解码序 POC = 0 6 2 4 / 0 6 2 4。
std::vector<RepairDisplayFrame> TwoGopBFrames() {
    return {
        Frame(0, 0, true), Frame(1, 6), Frame(2, 2), Frame(3, 4),
        Frame(4, 0, true), Frame(5, 6), Frame(6, 2), Frame(7, 4),
    };
}

std::string CorpusPath(const char* dir_macro, const char* name) {
    return (fs::path(dir_macro) / name).string();
}

std::string MakeTempDir(const std::string& tag) {
    const fs::path dir =
        fs::temp_directory_path() /
        ("videoeye_display_" + tag + "_" +
         std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(tag + std::to_string(::rand())))));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.string();
}

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string ReadText(const std::string& path) {
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    return std::string(bytes.begin(), bytes.end());
}

// 从整份文件里按 4CC 找盒体（测试够用：语料里 avcC 唯一）。
std::vector<uint8_t> ExtractBoxBody(const std::vector<uint8_t>& bytes, const char* type) {
    const std::size_t len = std::strlen(type);
    for (std::size_t i = 4; i + 4 + len <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, type, len) != 0)
            continue;
        const std::size_t start = i - 4;
        const uint32_t box_size =
            (static_cast<uint32_t>(bytes[start]) << 24) | (static_cast<uint32_t>(bytes[start + 1]) << 16) |
            (static_cast<uint32_t>(bytes[start + 2]) << 8) | static_cast<uint32_t>(bytes[start + 3]);
        if (box_size < 8 || start + box_size > bytes.size())
            continue;
        return std::vector<uint8_t>(bytes.begin() + static_cast<long>(i + 4),
                                    bytes.begin() + static_cast<long>(start + box_size));
    }
    return {};
}

const IsobmffTrack* FirstTrack(const IsobmffFile& parsed, const char* handler) {
    for (const auto& track : parsed.tracks) {
        if (track.handler == handler)
            return &track;
    }
    return nullptr;
}

// ctts 解析（version 1 有符号）。返回逐样本偏移，条数不守恒时返回空。
std::vector<int64_t> ParseCttsOffsets(const std::vector<uint8_t>& bytes, const char* type) {
    const std::size_t len = std::strlen(type);
    for (std::size_t i = 4; i + 4 + len <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, type, len) != 0)
            continue;
        const std::size_t start = i - 4;
        const std::size_t box_size =
            (static_cast<std::size_t>(bytes[start]) << 24) | (static_cast<std::size_t>(bytes[start + 1]) << 16) |
            (static_cast<std::size_t>(bytes[start + 2]) << 8) | static_cast<std::size_t>(bytes[start + 3]);
        if (box_size < 16 || start + box_size > bytes.size())
            continue;
        const uint8_t version = bytes[i + 4];
        EXPECT_EQ(version, 1) << "ctts 必须写 version 1，否则负偏移会被读成巨大的正数";
        const std::size_t count_pos = i + 8;
        const uint32_t entries =
            (static_cast<uint32_t>(bytes[count_pos]) << 24) | (static_cast<uint32_t>(bytes[count_pos + 1]) << 16) |
            (static_cast<uint32_t>(bytes[count_pos + 2]) << 8) | static_cast<uint32_t>(bytes[count_pos + 3]);
        std::vector<int64_t> offsets;
        std::size_t cursor = count_pos + 4;
        for (uint32_t e = 0; e < entries; ++e) {
            if (cursor + 8 > start + box_size)
                return {};
            const uint32_t sample_count =
                (static_cast<uint32_t>(bytes[cursor]) << 24) | (static_cast<uint32_t>(bytes[cursor + 1]) << 16) |
                (static_cast<uint32_t>(bytes[cursor + 2]) << 8) | static_cast<uint32_t>(bytes[cursor + 3]);
            const uint32_t raw =
                (static_cast<uint32_t>(bytes[cursor + 4]) << 24) | (static_cast<uint32_t>(bytes[cursor + 5]) << 16) |
                (static_cast<uint32_t>(bytes[cursor + 6]) << 8) | static_cast<uint32_t>(bytes[cursor + 7]);
            const int32_t signed_offset = static_cast<int32_t>(raw);
            for (uint32_t k = 0; k < sample_count; ++k)
                offsets.push_back(signed_offset);
            cursor += 8;
        }
        return offsets;
    }
    return {};
}

} // namespace

// ---- 推导：段内按 POC 排序，跨段不比较 POC ----
TEST(RepairDisplayOrder, DeriveAssignsDisplayOrderInsideRapSegments) {
    const RepairDisplayOrderPlan plan = DeriveDisplayOrder(TwoGopBFrames(), ConstantDuration(12800, 512));
    ASSERT_TRUE(plan.available) << plan.unavailable_reason;
    ASSERT_EQ(plan.frames.size(), 8u);
    EXPECT_EQ(plan.rap_segments, 2u);
    EXPECT_EQ(plan.confidence, "verified");

    // 段 0：I(0) B(2) B(4) P(6) → 解码序 0,2,3,1；段 1 同理整体后移 4。
    const std::vector<uint64_t> expected_display{0, 3, 1, 2, 4, 7, 5, 6};
    for (std::size_t i = 0; i < expected_display.size(); ++i) {
        EXPECT_EQ(plan.frames[i].display_index, expected_display[i]) << "解码序 " << i;
        EXPECT_EQ(plan.frames[i].segment, i < 4 ? 0u : 1u);
    }
    EXPECT_EQ(plan.reordered_frames, 6u);
    EXPECT_EQ(plan.max_reorder, 2u);
    // 跨段不比较 POC：第二段的第一帧（IDR, POC 0）绝不能排到第一段前面。
    EXPECT_LT(plan.frames[0].display_index, plan.frames[4].display_index);
}

TEST(RepairDisplayOrder, DeriveRefusesLeadingPicturesAndEmptyInput) {
    const RepairDisplayOrderPlan empty = DeriveDisplayOrder({}, ConstantDuration(12800, 512));
    EXPECT_FALSE(empty.available);
    EXPECT_FALSE(empty.unavailable_reason.empty()) << "不可用必须带原因，不能空着让人猜";

    std::vector<RepairDisplayFrame> frames = TwoGopBFrames();
    frames[2].leading = true; // RASL：POC 按定义小于所属 CRA，混进排序会把整段算错
    const RepairDisplayOrderPlan plan = DeriveDisplayOrder(frames, ConstantDuration(12800, 512));
    EXPECT_FALSE(plan.available);
    EXPECT_NE(plan.unavailable_reason.find("前导图像"), std::string::npos) << plan.unavailable_reason;
}

// ---- 推导：ctts 偏移有符号，且负值必须真的出现 ----
TEST(RepairDisplayOrder, DeriveProducesSignedCompositionOffsets) {
    const RepairDisplayOrderPlan plan = DeriveDisplayOrder(TwoGopBFrames(), ConstantDuration(12800, 512));
    ASSERT_TRUE(plan.available) << plan.unavailable_reason;
    ASSERT_EQ(plan.composition_offsets.size(), plan.frames.size());
    EXPECT_TRUE(plan.ctts_required);

    const std::vector<int64_t> expected{0, 1024, -512, -512, 0, 1024, -512, -512};
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(plan.composition_offsets[i], expected[i]) << "解码序 " << i;

    const std::vector<uint8_t> box = BuildCttsBox(plan);
    ASSERT_FALSE(box.empty());
    const std::vector<int64_t> parsed = ParseCttsOffsets(box, "ctts");
    ASSERT_EQ(parsed.size(), plan.frames.size()) << "ctts 条数必须与样本数守恒";
    for (std::size_t i = 0; i < parsed.size(); ++i)
        EXPECT_EQ(parsed[i], expected[i]) << "ctts 第 " << i << " 条";
}

// ---- 没有恒定帧时长证据时不许生成 ctts ----
TEST(RepairDisplayOrder, DeriveWithoutConstantDurationSkipsCtts) {
    RepairFrameDurationEvidence variable = ConstantDuration(12800, 512);
    variable.constant = false;
    const RepairDisplayOrderPlan plan = DeriveDisplayOrder(TwoGopBFrames(), variable);
    ASSERT_TRUE(plan.available) << plan.unavailable_reason;
    EXPECT_TRUE(plan.composition_offsets.empty());
    EXPECT_FALSE(plan.ctts_required);
    EXPECT_TRUE(BuildCttsBox(plan).empty());
    bool mentions = false;
    for (const auto& text : plan.limitations)
        mentions = mentions || text.find("ctts") != std::string::npos;
    EXPECT_TRUE(mentions) << "不生成 ctts 必须写进限制，不能静默";
}

// ---- 反向守护：偏移条数与帧数不符时，一个字节都不许写 ----
TEST(RepairDisplayOrder, BuildCttsBoxRejectsMismatchedEntryCount) {
    RepairDisplayOrderPlan plan = DeriveDisplayOrder(TwoGopBFrames(), ConstantDuration(12800, 512));
    ASSERT_TRUE(plan.ctts_required);
    plan.composition_offsets.pop_back();
    EXPECT_TRUE(BuildCttsBox(plan).empty()) << "条数对不上却写出 ctts，等于把样本表写成错位";

    RepairDisplayOrderPlan unavailable = DeriveDisplayOrder({}, ConstantDuration(12800, 512));
    unavailable.ctts_required = true; // 故意把标志掰成"要写"
    EXPECT_TRUE(BuildCttsBox(unavailable).empty()) << "不可用的结论不能靠标志位凑出 ctts";
}

TEST(RepairDisplayOrder, SummarizeNeverClaimsCttsWithoutRealWrite) {
    const RepairDisplayOrderPlan plan = DeriveDisplayOrder(TwoGopBFrames(), ConstantDuration(12800, 512));
    ASSERT_TRUE(plan.ctts_required);

    const model::RepairDisplayOrderSummary not_written = SummarizeDisplayOrder(plan, false);
    EXPECT_TRUE(not_written.analyzed);
    EXPECT_TRUE(not_written.available);
    EXPECT_TRUE(not_written.ctts_required);
    EXPECT_FALSE(not_written.ctts_written) << "计划要写 ≠ 真的写了";

    const model::RepairDisplayOrderSummary written = SummarizeDisplayOrder(plan, true);
    EXPECT_TRUE(written.ctts_written);
    EXPECT_EQ(written.frames, 8u);
    EXPECT_EQ(written.max_reorder, 2u);
}

// ---- 实测：真实 B 帧语料，与容器 PTS 排出来的显示序逐项对账 ----
TEST(RepairDisplayOrder, MeasureReadsRealBFramesAndMatchesContainerOrder) {
    const std::string path = CorpusPath(VIDEOEYE_CODEC_CORPUS_DIR, "real_x264_bframes_gop.mp4");
    if (!fs::exists(path))
        GTEST_SKIP() << "缺真实 B 帧样本，先跑 tests/corpus/generate_real_corpus.py";

    IsobmffFile parsed;
    ASSERT_TRUE(IsobmffParser::Parse(path, parsed)) << parsed.error_message;
    const IsobmffTrack* video = FirstTrack(parsed, "vide");
    ASSERT_NE(video, nullptr);
    ASSERT_EQ(video->chunk_offsets.size(), 1u) << "该语料应为单 chunk，测试按累加尺寸换偏移";
    ASSERT_FALSE(video->stsz.sizes.empty());

    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
    for (const auto& box : parsed.top_level) {
        if (box.type == "mdat") {
            payload_offset = box.offset + box.header_size;
            payload_size = box.size - box.header_size;
        }
    }
    ASSERT_GT(payload_size, 0u);

    std::vector<RepairAuRange> ranges;
    uint64_t relative = video->chunk_offsets.front() - payload_offset;
    for (uint32_t size : video->stsz.sizes) {
        ranges.push_back(RepairAuRange{relative, size});
        relative += size;
    }

    const std::vector<uint8_t> file_bytes = ReadFileBytes(path);
    ASSERT_GT(file_bytes.size(), payload_offset + payload_size);
    std::vector<uint8_t> payload(file_bytes.begin() + static_cast<long>(payload_offset),
                                 file_bytes.begin() + static_cast<long>(payload_offset + payload_size));

    ScanWindow window;
    window.file_offset = payload_offset;
    window.size = payload_size;
    window.track_id = video->track_id;
    window.stream_index = 0;
    window.segment_index = 0;
    window.config_id = "corpus";
    MemoryByteSource source(payload, window);

    RepairFrameDurationEvidence duration;
    duration.source = "stts_constant";
    duration.timescale = video->media_timescale;
    duration.sample_delta = video->stts.empty() ? 0u : video->stts.front().sample_delta;
    duration.constant = true;
    duration.detail = "单元测试：取源 stts 首条样本时长";

    const RepairDisplayOrderPlan plan =
        MeasureDisplayOrder(source, ranges, ExtractBoxBody(file_bytes, "avcC"), "h264", duration);
    ASSERT_TRUE(plan.available) << plan.unavailable_reason;
    ASSERT_EQ(plan.frames.size(), video->stsz.sizes.size());
    EXPECT_EQ(plan.confidence, "verified");
    EXPECT_GT(plan.reordered_frames, 0u) << "B 帧语料必须真的发生重排，否则这条用例是空跑";
    EXPECT_TRUE(plan.ctts_required);

    // 参考输出：容器 PTS 的升序即真实显示序。
    std::vector<int64_t> pts;
    {
        AVFormatContext* ctx = nullptr;
        ASSERT_EQ(avformat_open_input(&ctx, path.c_str(), nullptr, nullptr), 0);
        ASSERT_EQ(avformat_find_stream_info(ctx, nullptr), 0);
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(ctx, packet) >= 0) {
            if (packet->stream_index == 0)
                pts.push_back(packet->pts);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        avformat_close_input(&ctx);
    }
    ASSERT_EQ(pts.size(), plan.frames.size());

    std::vector<std::size_t> order(pts.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&pts](std::size_t lhs, std::size_t rhs) { return pts[lhs] < pts[rhs]; });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        EXPECT_EQ(plan.frames[order[rank]].display_index, rank)
            << "按 PTS 排第 " << rank << " 位的帧（解码序 " << order[rank] << "）显示序不符";
    }
}

// ---- 取消必须真的打断逐 AU 循环 ----
TEST(RepairDisplayOrder, MeasureHonorsCancel) {
    std::vector<uint8_t> payload(64, 0);
    ScanWindow window;
    window.size = payload.size();
    MemoryByteSource source(payload, window);
    std::atomic<bool> canceled{true};
    const RepairDisplayOrderPlan plan = MeasureDisplayOrder(source, {RepairAuRange{0, 16}, RepairAuRange{16, 16}}, {},
                                                            "h264", ConstantDuration(12800, 512), &canceled, nullptr);
    EXPECT_FALSE(plan.available);
    EXPECT_NE(plan.unavailable_reason.find("取消"), std::string::npos) << plan.unavailable_reason;
}

// ---- 接线：无参考重建器必须把自己的显示顺序结论如实报出来 ----
TEST(RepairDisplayOrder, PlainRebuilderReportsDisplayOrderForInbandMp4) {
    const std::string source = CorpusPath(VIDEOEYE_CONTAINER_CORPUS_DIR, "real_h264_inband_missing_moov.mp4");
    if (!fs::exists(source))
        GTEST_SKIP() << "缺真实样本，先跑 tests/corpus/generate_real_corpus.py";
    const std::string dir = MakeTempDir("rebuild");
    const std::string out = dir + "/rebuilt.mp4";

    const auto rebuilt = videoeye::repair::RepairPlainMp4Rebuilder::Rebuild(source, out, nullptr);
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error;
    EXPECT_TRUE(rebuilt.display_order.analyzed) << "视频轨必须做显示顺序测量";
    ASSERT_TRUE(rebuilt.display_order.available) << rebuilt.display_order.unavailable_reason;
    EXPECT_EQ(rebuilt.display_order.confidence, "verified");
    EXPECT_EQ(rebuilt.display_order.duration_source, "vui_timing");

    // 该语料由 x264 编成、没有 B 帧：显示序应与解码序一致，因此不写 ctts。
    EXPECT_FALSE(rebuilt.display_order.ctts_required);
    EXPECT_FALSE(rebuilt.display_order.ctts_written);
    IsobmffFile product;
    ASSERT_TRUE(IsobmffParser::Parse(out, product)) << product.error_message;
    ASSERT_EQ(product.tracks.size(), 1u);
    EXPECT_FALSE(product.tracks[0].has_ctts) << "没有重排却写了 ctts，等于凭空插入显示延迟";
}

// ============================================================
// 端到端：ctts 真的写进产物
//
// 为什么需要自建码流：现有真实语料里**没有**"既有 B 帧、SPS 又带 VUI timing、
// 参数集还在 in-band"的样本 —— x264 产 B 帧但不写 VUI，libopenh264 写 VUI 但不出
// B 帧，而重建器缺 VUI 时按规矩拒绝猜时基（那条路已经有用例守着）。
// 所以这里按 H.264 语法手写最小码流：只要能过 SPS/PPS/slice_header 解析即可，
// 不含可解码的图像数据。它证明的是"ctts 真的落进 moov 且偏移正确"，
// 显示顺序的语义正确性由上面基于真实 B 帧语料的用例负责。
// ============================================================
namespace {

class BitWriter {
public:
    void U(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i)
            Push(((value >> i) & 1u) != 0);
    }
    void Ue(uint32_t value) {
        const uint32_t code = value + 1;
        int width = 0;
        while ((1u << width) <= code)
            ++width;
        for (int i = 1; i < width; ++i)
            Push(false);
        U(code, width);
    }
    void Se(int32_t value) {
        Ue(value <= 0 ? static_cast<uint32_t>(-2 * value) : static_cast<uint32_t>(2 * value - 1));
    }
    void Trailing() {
        Push(true);
        while ((bits_ % 8) != 0)
            Push(false);
    }
    std::vector<uint8_t> Bytes() const {
        return bytes_;
    }

private:
    void Push(bool bit) {
        if ((bits_ % 8) == 0)
            bytes_.push_back(0);
        if (bit)
            bytes_.back() |= static_cast<uint8_t>(0x80u >> (bits_ % 8));
        ++bits_;
    }

    std::vector<uint8_t> bytes_;
    std::size_t bits_ = 0;
};

// RBSP → EBSP：连续两个 0 之后出现 00..03 要插入 0x03，否则会被当起始码。
std::vector<uint8_t> EscapeRbsp(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    int zeros = 0;
    for (uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

std::vector<uint8_t> MakeSps() {
    BitWriter writer;
    writer.U(66, 8);  // profile_idc = baseline
    writer.U(0, 8);   // constraint flags
    writer.U(30, 8);  // level_idc
    writer.Ue(0);     // seq_parameter_set_id
    writer.Ue(0);     // log2_max_frame_num_minus4 → 4 位
    writer.Ue(0);     // pic_order_cnt_type
    writer.Ue(0);     // log2_max_pic_order_cnt_lsb_minus4 → 4 位
    writer.Ue(1);     // max_num_ref_frames
    writer.U(0, 1);   // gaps_in_frame_num_value_allowed_flag
    writer.Ue(19);    // pic_width_in_mbs_minus1  = 320/16-1
    writer.Ue(10);    // pic_height_in_map_units_minus1 = 176/16-1
    writer.U(1, 1);   // frame_mbs_only_flag
    writer.U(1, 1);   // direct_8x8_inference_flag
    writer.U(0, 1);   // frame_cropping_flag
    writer.U(1, 1);   // vui_parameters_present_flag
    writer.U(0, 1);   // aspect_ratio_info_present_flag
    writer.U(0, 1);   // overscan_info_present_flag
    writer.U(0, 1);   // video_signal_type_present_flag
    writer.U(0, 1);   // chroma_loc_info_present_flag
    writer.U(1, 1);   // timing_info_present_flag
    writer.U(1, 32);  // num_units_in_tick
    writer.U(50, 32); // time_scale → 25 fps，每帧 2 tick
    writer.U(1, 1);   // fixed_frame_rate_flag
    writer.U(0, 1);   // nal_hrd_parameters_present_flag
    writer.U(0, 1);   // vcl_hrd_parameters_present_flag
    writer.U(0, 1);   // pic_struct_present_flag
    writer.U(0, 1);   // bitstream_restriction_flag
    writer.Trailing();
    std::vector<uint8_t> nal{0x67};
    const std::vector<uint8_t> escaped = EscapeRbsp(writer.Bytes());
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

std::vector<uint8_t> MakePps() {
    BitWriter writer;
    writer.Ue(0);   // pic_parameter_set_id
    writer.Ue(0);   // seq_parameter_set_id
    writer.U(0, 1); // entropy_coding_mode_flag
    writer.U(0, 1); // bottom_field_pic_order_in_frame_present_flag
    writer.Ue(0);   // num_slice_groups_minus1
    writer.Ue(0);   // num_ref_idx_l0_default_active_minus1
    writer.Ue(0);   // num_ref_idx_l1_default_active_minus1
    writer.U(0, 1); // weighted_pred_flag
    writer.U(0, 2); // weighted_bipred_idc
    writer.Se(0);   // pic_init_qp_minus26
    writer.Se(0);   // pic_init_qs_minus26
    writer.Se(0);   // chroma_qp_index_offset
    writer.U(0, 1); // deblocking_filter_control_present_flag
    writer.U(0, 1); // constrained_intra_pred_flag
    writer.U(0, 1); // redundant_pic_cnt_present_flag
    writer.Trailing();
    std::vector<uint8_t> nal{0x68};
    const std::vector<uint8_t> escaped = EscapeRbsp(writer.Bytes());
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

// slice_type：2 = I、0 = P、1 = B。追踪器只读到 poc_lsb 为止，
// 后面的参考列表 / 残差语法对 POC 没有影响，这里不写（也写不出可解码的图像）。
std::vector<uint8_t> MakeSlice(bool idr, uint32_t slice_type, uint32_t frame_num, uint32_t poc_lsb) {
    BitWriter writer;
    writer.Ue(0); // first_mb_in_slice
    writer.Ue(slice_type);
    writer.Ue(0); // pic_parameter_set_id
    writer.U(frame_num, 4);
    if (idr)
        writer.Ue(0); // idr_pic_id
    writer.U(poc_lsb, 4);
    writer.Trailing();
    std::vector<uint8_t> nal{static_cast<uint8_t>(idr ? 0x65 : 0x41)};
    const std::vector<uint8_t> escaped = EscapeRbsp(writer.Bytes());
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

void AppendNal(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal) {
    const uint32_t size = static_cast<uint32_t>(nal.size());
    out->push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
    out->push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(size & 0xFF));
    out->insert(out->end(), nal.begin(), nal.end());
}

void AppendU32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(value & 0xFF));
}

// ftyp + mdat（无 moov）：两个 GOP 的 I P B B，解码序 POC = 0 6 2 4。
bool WriteReorderedAvccMp4(const std::string& path) {
    std::vector<uint8_t> payload;
    const std::vector<uint8_t> sps = MakeSps();
    const std::vector<uint8_t> pps = MakePps();
    for (int gop = 0; gop < 2; ++gop) {
        AppendNal(&payload, sps);
        AppendNal(&payload, pps);
        AppendNal(&payload, MakeSlice(true, 2, 0, 0));  // I  POC 0
        AppendNal(&payload, MakeSlice(false, 0, 1, 6)); // P  POC 6
        AppendNal(&payload, MakeSlice(false, 1, 2, 2)); // B  POC 2
        AppendNal(&payload, MakeSlice(false, 1, 3, 4)); // B  POC 4
    }

    std::vector<uint8_t> file;
    AppendU32(&file, 20);
    file.insert(file.end(), {'f', 't', 'y', 'p'});
    file.insert(file.end(), {'i', 's', 'o', 'm'});
    AppendU32(&file, 512);
    file.insert(file.end(), {'i', 's', 'o', 'm'});
    AppendU32(&file, static_cast<uint32_t>(payload.size() + 8));
    file.insert(file.end(), {'m', 'd', 'a', 't'});
    file.insert(file.end(), payload.begin(), payload.end());

    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

} // namespace

TEST(RepairDisplayOrder, RebuildWritesCttsForReorderedStream) {
    const std::string dir = MakeTempDir("ctts");
    const std::string source = dir + "/reordered.mp4";
    ASSERT_TRUE(WriteReorderedAvccMp4(source)) << "无法写出自建码流";

    const std::string out = dir + "/rebuilt.mp4";
    const auto rebuilt = videoeye::repair::RepairPlainMp4Rebuilder::Rebuild(source, out, nullptr);
    ASSERT_TRUE(rebuilt.ok) << rebuilt.error;

    ASSERT_TRUE(rebuilt.display_order.available) << rebuilt.display_order.unavailable_reason;
    EXPECT_EQ(rebuilt.display_order.confidence, "verified");
    EXPECT_EQ(rebuilt.display_order.timescale, 50u);
    EXPECT_EQ(rebuilt.display_order.sample_delta, 2u);
    EXPECT_EQ(rebuilt.display_order.frames, 8u);
    EXPECT_EQ(rebuilt.display_order.rap_segments, 2u);
    EXPECT_EQ(rebuilt.display_order.reordered_frames, 6u);
    EXPECT_TRUE(rebuilt.display_order.ctts_required);
    ASSERT_TRUE(rebuilt.display_order.ctts_written) << "有重排却没写 ctts，产物会按解码序播放";

    IsobmffFile product;
    ASSERT_TRUE(IsobmffParser::Parse(out, product)) << product.error_message;
    ASSERT_EQ(product.tracks.size(), 1u);
    ASSERT_TRUE(product.tracks[0].has_ctts) << "重建产物里没有 ctts 盒";

    // 期望偏移 = (显示序 − 解码序) × 每帧 2 tick：I 0 / P +4 / B −2 / B −2，逐 GOP 重复。
    std::vector<int64_t> parsed;
    for (const auto& entry : product.tracks[0].ctts) {
        for (uint32_t i = 0; i < entry.sample_count; ++i)
            parsed.push_back(entry.sample_offset);
    }
    const std::vector<int64_t> expected{0, 4, -2, -2, 0, 4, -2, -2};
    ASSERT_EQ(parsed.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(parsed[i], expected[i]) << "ctts 第 " << i << " 条";
}

// ---- 接线：引擎结果 + 三格式报告都要能解释"显示顺序推没推、ctts 写没写" ----
TEST(RepairDisplayOrder, EngineReportsDisplayOrderInAllFormats) {
    const std::string source = CorpusPath(VIDEOEYE_CONTAINER_CORPUS_DIR, "real_h264_inband_missing_moov.mp4");
    if (!fs::exists(source))
        GTEST_SKIP() << "缺真实样本，先跑 tests/corpus/generate_real_corpus.py";
    const std::string dir = MakeTempDir("engine_report");

    model::RepairRequest request;
    request.source_path = source;
    request.output_directory = dir + "/out";
    request.verification_level = model::RepairVerificationLevel::V1_FrameSample;
    request.write_report = true;

    const model::RepairResult result = videoeye::repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_TRUE(result.display_order.analyzed) << "引擎必须把重建器的显示顺序结论带出来";
    ASSERT_TRUE(result.display_order.available) << result.display_order.unavailable_reason;
    EXPECT_FALSE(result.display_order.ctts_written) << "该样本无 B 帧，写 ctts 就是凭空插延迟";

    const auto written =
        videoeye::reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;

    const std::string json = ReadText(dir + "/r.json");
    EXPECT_NE(json.find("\"display_order\""), std::string::npos) << "JSON 缺显示顺序结论";
    EXPECT_NE(json.find("\"ctts_written\""), std::string::npos);
    EXPECT_NE(json.find("\"reordered_frames\""), std::string::npos);
    for (const char* name : {"/r.txt", "/r.html"}) {
        const std::string body = ReadText(dir + name);
        EXPECT_NE(body.find("显示顺序"), std::string::npos) << name << " 缺显示顺序段落";
        EXPECT_NE(body.find("POC"), std::string::npos) << name << " 缺推导依据";
    }
}
