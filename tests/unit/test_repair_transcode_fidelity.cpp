#include <gtest/gtest.h>

// ============================================================
// 阶段 6 / 切片 J：S08 重编码的「源侧保真属性」探测
//
// 旧实现把解码帧统一重编码成 8-bit yuv420p，位深 / 色彩标记 / 旋转 /
// HDR 元数据 / 动态分辨率全部静默丢掉，报告里一个字都没有。这一组用例盯住：
//   * 没有证据时（首帧或流参数不可用）不得用空表冒充「属性均已保留」；
//   * 位深变化必须报损失，且**不许**把色彩标记照搬到低位深产物上；
//   * 位深不变时色彩 / SAR / 色度采样位置必须真的写进编码器 / 转换帧，
//     才允许进 preserved 列表（只改了清单没改产物 = 假覆盖）；
//   * 旋转 / HDR / 色度抽样逐项可判别；
//   * 结果真的进 RepairResult 与 JSON/TXT/HTML 三份报告，非转码路径写「未探测」。
//
// 切片 O 追加：**编码器名是实测事实，不是选型期望**。本项目锁定的 FFmpeg 是
// LGPL 变体（--disable-libx264 / --disable-libx265），「libx264 重编码」这句话
// 在本构建里恒为假；旧实现把它写进了能力契约与排除项，同一份报告的
// fidelity.limitations 却印着「编码器=libopenh264」。这一组用例钉住：
//   * 标签只能由 encoder_used / encoder_fallback 渲染，不许回退到首选名；
//   * profile 比对只在两端都实测到时成立，缺一侧就明说「未能比对」；
//   * 非转码路径不得出现编码器行（没转码就没有编码器事实）。
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include "core/domain/model/RepairRequest.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairTranscodeFidelity.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/TestMediaFactory.h"

namespace {

using namespace videoeye;
using namespace videoeye::model;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_fidelity_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    request.verification_level = RepairVerificationLevel::V1_FrameSample;
    request.write_report = true;
    return request;
}

// 纯内存构造 AVStream / AVFrame / AVCodecContext：不落盘、不解码，
// 让「探测函数本身」的判别力可以单独验证。
class FidelityProbe {
public:
    FidelityProbe() {
        format_ = avformat_alloc_context();
        if (format_ != nullptr)
            stream_ = avformat_new_stream(format_, nullptr);
        frame_ = av_frame_alloc();
        encoder_ = avcodec_alloc_context3(nullptr);
        target_frame_ = av_frame_alloc();
    }
    ~FidelityProbe() {
        avcodec_free_context(&encoder_);
        av_frame_free(&target_frame_);
        av_frame_free(&frame_);
        avformat_free_context(format_);
    }
    FidelityProbe(const FidelityProbe&) = delete;
    FidelityProbe& operator=(const FidelityProbe&) = delete;

    bool Ready() const {
        return stream_ != nullptr && frame_ != nullptr && encoder_ != nullptr && target_frame_ != nullptr;
    }
    AVStream* Stream() const {
        return stream_;
    }
    AVFrame* Frame() const {
        return frame_;
    }
    AVFrame* TargetFrame() const {
        return target_frame_;
    }
    AVCodecContext* Encoder() const {
        return encoder_;
    }
    AVCodecParameters* Par() const {
        return stream_->codecpar;
    }

    void SetSource(AVPixelFormat format, int width = 320, int height = 240) {
        frame_->format = format;
        frame_->width = width;
        frame_->height = height;
        Par()->format = format;
        Par()->width = width;
        Par()->height = height;
    }

    void SetColorTags(AVColorPrimaries primaries, AVColorTransferCharacteristic transfer, AVColorSpace space,
                      AVColorRange range, AVChromaLocation chroma) {
        Par()->color_primaries = primaries;
        Par()->color_trc = transfer;
        Par()->color_space = space;
        Par()->color_range = range;
        Par()->chroma_location = chroma;
    }

    void SetSar(int num, int den) {
        Par()->sample_aspect_ratio.num = num;
        Par()->sample_aspect_ratio.den = den;
    }

    std::uint8_t* AddSideData(AVPacketSideDataType type, std::size_t size) {
        AVPacketSideData* data =
            av_packet_side_data_new(&Par()->coded_side_data, &Par()->nb_coded_side_data, type, size, 0);
        return data != nullptr ? data->data : nullptr;
    }

private:
    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVCodecContext* encoder_ = nullptr;
    AVFrame* target_frame_ = nullptr;
};

struct ProbeResult {
    repair::RepairTranscodeInheritance inherited;
    RepairTranscodeFidelity fidelity;
};

ProbeResult Inspect(FidelityProbe& probe, AVPixelFormat source_format, AVPixelFormat output_format,
                    bool apply_frame_chroma = true) {
    ProbeResult out;
    out.inherited = repair::ApplyTranscodeInheritance(probe.Encoder(), probe.Stream(), probe.Frame(),
                                                      repair::TranscodeBitDepth(source_format),
                                                      repair::TranscodeBitDepth(output_format));
    if (apply_frame_chroma) {
        out.inherited.chroma_location_copied =
            repair::ApplyTranscodeFrameProperties(probe.TargetFrame(), probe.Stream());
    }
    out.fidelity = repair::InspectTranscodeFidelity(probe.Stream(), probe.Frame(), output_format, out.inherited);
    return out;
}

bool HasLost(const RepairTranscodeFidelity& fidelity, const std::string& needle) {
    for (const auto& item : fidelity.lost_properties)
        if (item.find(needle) != std::string::npos)
            return true;
    return false;
}

// 同一条损失项里同时含两个关键词（避免被另一条同词项误判为命中）。
bool HasLostItemWith(const RepairTranscodeFidelity& fidelity, const std::string& a, const std::string& b) {
    for (const auto& item : fidelity.lost_properties)
        if (item.find(a) != std::string::npos && item.find(b) != std::string::npos)
            return true;
    return false;
}

bool HasKept(const RepairTranscodeFidelity& fidelity, const std::string& needle) {
    for (const auto& item : fidelity.preserved_properties)
        if (item.find(needle) != std::string::npos)
            return true;
    return false;
}

// ---- 反向守护：没有证据时不许自证 ----

TEST(RepairTranscodeFidelity, NoEvidenceStaysUninspected) {
    RepairTranscodeFidelity fidelity =
        repair::InspectTranscodeFidelity(nullptr, nullptr, AV_PIX_FMT_YUV420P, repair::RepairTranscodeInheritance{});
    EXPECT_FALSE(fidelity.inspected);
    EXPECT_FALSE(fidelity.all_properties_preserved);
    EXPECT_TRUE(fidelity.lost_properties.empty());
    EXPECT_TRUE(fidelity.preserved_properties.empty());
    EXPECT_TRUE(fidelity.source_pixel_format.empty());

    // 即使事后喂一个很大的动态分辨率计数，也不能把「没探测过」抬成「已保留」。
    repair::FinalizeTranscodeFidelity(fidelity, 7);
    EXPECT_FALSE(fidelity.inspected);
    EXPECT_FALSE(fidelity.all_properties_preserved);
    EXPECT_EQ(fidelity.resolution_changed_frames, 0u);
    EXPECT_TRUE(fidelity.lost_properties.empty());

    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    const RepairTranscodeFidelity no_frame =
        repair::InspectTranscodeFidelity(probe.Stream(), nullptr, AV_PIX_FMT_YUV420P, {});
    EXPECT_FALSE(no_frame.inspected) << "首帧缺失时同样没有位深 / 色彩证据";
}

// ---- 正常 8-bit 源：没有可报的损失，也不能凭空报 ----

TEST(RepairTranscodeFidelity, PlainEightBitSourceReportsNoLoss) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    ASSERT_TRUE(result.fidelity.inspected);
    EXPECT_EQ(result.fidelity.source_pixel_format, "yuv420p");
    EXPECT_EQ(result.fidelity.output_pixel_format, "yuv420p");
    EXPECT_EQ(result.fidelity.source_bit_depth, 8);
    EXPECT_EQ(result.fidelity.output_bit_depth, 8);
    EXPECT_EQ(result.fidelity.source_width, 320);
    EXPECT_EQ(result.fidelity.source_height, 240);
    EXPECT_FALSE(result.fidelity.has_display_matrix);
    EXPECT_FALSE(result.fidelity.has_hdr_metadata);
    EXPECT_TRUE(result.fidelity.lost_properties.empty());
    EXPECT_TRUE(result.fidelity.all_properties_preserved);
    EXPECT_FALSE(result.inherited.color_tags_copied) << "源没声明色彩标记，不该凭意图记账";
}

// ---- 10-bit：位深损失 + 不许照搬色彩标记 ----

TEST(RepairTranscodeFidelity, TenBitSourceReportsBitDepthLoss) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P10LE);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV420P);

    ASSERT_TRUE(result.fidelity.inspected);
    EXPECT_EQ(result.fidelity.source_bit_depth, 10);
    EXPECT_EQ(result.fidelity.output_bit_depth, 8);
    EXPECT_TRUE(HasLost(result.fidelity, "位深 10 → 8")) << "10-bit 被压成 8-bit 必须逐项写明";
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
}

TEST(RepairTranscodeFidelity, BitDepthChangeBlocksColorTagInheritance) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P10LE);
    probe.SetColorTags(AVCOL_PRI_BT2020, AVCOL_TRC_SMPTE2084, AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_MPEG,
                       AVCHROMA_LOC_LEFT);
    probe.SetSar(1, 1);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV420P);

    EXPECT_FALSE(result.inherited.color_tags_copied);
    EXPECT_EQ(probe.Encoder()->color_primaries, AVCOL_PRI_UNSPECIFIED)
        << "位深变了还把 bt2020/smpte2084 写进 8-bit 编码器，播放器会按 HDR 渲染";
    EXPECT_EQ(probe.Encoder()->colorspace, AVCOL_SPC_UNSPECIFIED);
    EXPECT_EQ(result.fidelity.color_primaries, "bt2020");
    EXPECT_EQ(result.fidelity.color_transfer, "smpte2084");
    EXPECT_TRUE(HasLost(result.fidelity, "色彩标记")) << "色彩标记未继承必须进损失清单";
    EXPECT_FALSE(HasKept(result.fidelity, "色彩标记")) << "没真写进编码器就不能进继承清单";
    // 位深守卫必须只拦色彩标记：色度采样位置与 SAR 都不依赖位深，
    // 一刀切全拦等于为了「安全」多丢两项本可保留的属性。
    EXPECT_EQ(probe.TargetFrame()->chroma_location, AVCHROMA_LOC_LEFT);
    EXPECT_TRUE(HasKept(result.fidelity, "色度采样位置"));
    EXPECT_TRUE(HasKept(result.fidelity, "像素宽高比")) << "SAR 与位深无关，应当原样继承";
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
}

// ---- 位深不变：真的写进编码器 / 转换帧才算继承 ----

TEST(RepairTranscodeFidelity, SameBitDepthInheritsColorTagsSarAndChromaLocation) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    probe.SetColorTags(AVCOL_PRI_BT709, AVCOL_TRC_BT709, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, AVCHROMA_LOC_LEFT);
    probe.SetSar(4, 3);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    EXPECT_TRUE(result.inherited.color_tags_copied);
    EXPECT_TRUE(result.inherited.chroma_location_copied);
    EXPECT_EQ(probe.Encoder()->colorspace, AVCOL_SPC_BT709);
    EXPECT_EQ(probe.Encoder()->color_trc, AVCOL_TRC_BT709);
    EXPECT_EQ(probe.Encoder()->sample_aspect_ratio.num, 4);
    EXPECT_EQ(probe.Encoder()->sample_aspect_ratio.den, 3);
    EXPECT_EQ(probe.TargetFrame()->chroma_location, AVCHROMA_LOC_LEFT);
    EXPECT_EQ(result.fidelity.sample_aspect_ratio, "4:3");
    EXPECT_EQ(result.fidelity.color_space, "bt709");
    EXPECT_TRUE(result.fidelity.lost_properties.empty());
    EXPECT_TRUE(result.fidelity.all_properties_preserved);
    EXPECT_TRUE(HasKept(result.fidelity, "色彩标记"));
    EXPECT_TRUE(HasKept(result.fidelity, "像素宽高比"));
    EXPECT_TRUE(HasKept(result.fidelity, "色度采样位置"));
}

TEST(RepairTranscodeFidelity, ClaimingChromaInheritanceWithoutWritingFrameIsRejected) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    probe.SetColorTags(AVCOL_PRI_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED,
                       AVCHROMA_LOC_LEFT);
    // 故意不调 ApplyTranscodeFrameProperties：模拟「清单说继承了、产物没写」。
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P, false);
    EXPECT_EQ(probe.TargetFrame()->chroma_location, AVCHROMA_LOC_UNSPECIFIED);
    EXPECT_TRUE(HasLostItemWith(result.fidelity, "色度采样位置", "未继承"));
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
}

// ---- 旋转 / 镜像、HDR、色度抽样各自可判别 ----

TEST(RepairTranscodeFidelity, RotationMatrixIsReportedAsLoss) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    std::uint8_t* raw = probe.AddSideData(AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t));
    ASSERT_NE(raw, nullptr);
    auto* matrix = reinterpret_cast<std::int32_t*>(raw);
    av_display_rotation_set(matrix, 90.0);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    EXPECT_TRUE(result.fidelity.has_display_matrix);
    EXPECT_GT(std::abs(result.fidelity.rotation_degrees), 80.0);
    EXPECT_TRUE(HasLost(result.fidelity, "旋转 / 镜像"));
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
}

TEST(RepairTranscodeFidelity, IdentityDisplayMatrixIsNotALoss) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    std::uint8_t* raw = probe.AddSideData(AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t));
    ASSERT_NE(raw, nullptr);
    auto* matrix = reinterpret_cast<std::int32_t*>(raw);
    av_display_rotation_set(matrix, 0.0);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    EXPECT_FALSE(result.fidelity.has_display_matrix) << "单位矩阵不含方向信息，不该凭空报损失";
    EXPECT_FALSE(HasLost(result.fidelity, "旋转 / 镜像"));
}

TEST(RepairTranscodeFidelity, HdrSideDataIsReportedAsLoss) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    ASSERT_NE(probe.AddSideData(AV_PKT_DATA_MASTERING_DISPLAY_METADATA, 8), nullptr);
    ASSERT_NE(probe.AddSideData(AV_PKT_DATA_CONTENT_LIGHT_LEVEL, 4), nullptr);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    EXPECT_TRUE(result.fidelity.has_hdr_metadata);
    EXPECT_TRUE(HasLost(result.fidelity, "HDR 静态元数据"));
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
}

TEST(RepairTranscodeFidelity, ChromaSubsamplingLossIsReported) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV422P);
    const ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P);

    EXPECT_TRUE(HasLost(result.fidelity, "色度抽样 yuv422p → yuv420p"));
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
    // 两者都是 8 bit，不能顺带把位深损失也报出来。
    EXPECT_FALSE(HasLost(result.fidelity, "位深"));
}

// ---- 动态分辨率收尾 ----

TEST(RepairTranscodeFidelity, FinalizeCountsDynamicResolutionFrames) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    RepairTranscodeFidelity fidelity = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P).fidelity;
    ASSERT_TRUE(fidelity.all_properties_preserved);

    repair::FinalizeTranscodeFidelity(fidelity, 3);
    EXPECT_EQ(fidelity.resolution_changed_frames, 3u);
    EXPECT_TRUE(HasLost(fidelity, "动态分辨率：3 帧"));
    EXPECT_FALSE(fidelity.all_properties_preserved);

    // 覆盖守护：0 帧时结论必须仍是「全部保留」，不能因为收尾函数跑过就翻 false。
    RepairTranscodeFidelity clean = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P).fidelity;
    repair::FinalizeTranscodeFidelity(clean, 0);
    EXPECT_TRUE(clean.all_properties_preserved);
    EXPECT_TRUE(clean.lost_properties.empty());
}

// ---- 逐帧缩放状态：动态分辨率计数必须真的来自逐帧实测 ----

struct FrameHolder {
    AVFrame* frame = av_frame_alloc();
    ~FrameHolder() {
        av_frame_free(&frame);
    }
};

void SetFrame(AVFrame* frame, int width, int height, AVPixelFormat format) {
    frame->width = width;
    frame->height = height;
    frame->format = format;
}

TEST(RepairTranscodeFidelity, ScaleStateCountsSizeChangesAndRetriesFailedRebuild) {
    repair::TranscodeScaleState state;
    state.output_width = 320;
    state.output_height = 240;
    state.source_width = 320;
    state.source_height = 240;
    state.source_format = AV_PIX_FMT_YUV420P;

    FrameHolder a, b, c, d, e;
    ASSERT_NE(a.frame, nullptr);
    SetFrame(a.frame, 320, 240, AV_PIX_FMT_YUV420P);
    SetFrame(b.frame, 640, 480, AV_PIX_FMT_YUV420P);
    SetFrame(c.frame, 640, 480, AV_PIX_FMT_YUV420P);
    SetFrame(d.frame, 640, 480, AV_PIX_FMT_YUV420P);
    SetFrame(e.frame, 320, 240, AV_PIX_FMT_YUV420P);

    EXPECT_FALSE(state.Observe(a.frame));
    EXPECT_EQ(state.resolution_changed_frames, 0u) << "与产物同尺寸的帧不算动态分辨率";
    EXPECT_TRUE(state.Observe(b.frame));
    EXPECT_EQ(state.resolution_changed_frames, 1u);
    // 模拟 sws 重建失败：不 Commit，下一帧同尺寸必须再次尝试而不是假装已适配。
    EXPECT_TRUE(state.Observe(c.frame));
    EXPECT_EQ(state.resolution_changed_frames, 2u);
    state.Commit(c.frame);
    EXPECT_EQ(state.source_height, 480) << "sws_scale 的 srcSliceH 必须取重建后的源高";
    EXPECT_FALSE(state.Observe(d.frame));
    EXPECT_EQ(state.resolution_changed_frames, 3u) << "尺寸仍与产物不同，每一帧都要计数";
    EXPECT_TRUE(state.Observe(e.frame));
    EXPECT_EQ(state.resolution_changed_frames, 3u);
}

TEST(RepairTranscodeFidelity, ScaleStateRebuildsOnPixelFormatChangeWithoutCountingIt) {
    repair::TranscodeScaleState state;
    state.output_width = 320;
    state.output_height = 240;
    state.source_width = 320;
    state.source_height = 240;
    state.source_format = AV_PIX_FMT_YUV420P;
    FrameHolder f;
    ASSERT_NE(f.frame, nullptr);
    SetFrame(f.frame, 320, 240, AV_PIX_FMT_YUV422P);

    EXPECT_TRUE(state.Observe(f.frame)) << "像素格式变了也必须重建 sws（行宽不同）";
    EXPECT_EQ(state.resolution_changed_frames, 0u) << "尺寸没变就不该记成动态分辨率";
    EXPECT_FALSE(state.Observe(nullptr));
    EXPECT_EQ(state.resolution_changed_frames, 0u);
}

// ---- 端到端：字段真的进结果和三份报告 ----

TEST(RepairTranscodeFidelity, S08PublishesFidelityInAllReportFormats) {
    const std::string dir = MakeTempDir("s08");
    const auto media = videoeye_test::CreateTestMedia(dir, "s08_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    ASSERT_TRUE(result.fidelity.video_reencoded);

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    EXPECT_TRUE(fidelity.inspected) << "S08 走过重编码，必须留下源侧属性实测";
    EXPECT_FALSE(fidelity.source_pixel_format.empty());
    EXPECT_FALSE(fidelity.output_pixel_format.empty());
    EXPECT_GT(fidelity.source_bit_depth, 0);
    EXPECT_GT(fidelity.output_bit_depth, 0);
    EXPECT_GT(fidelity.source_width, 0);

    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string json_text = ReadText(dir + "/r.json");
    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(json_text, root, &error)) << error;
    const JsonValue* report_fidelity = root.Find("fidelity");
    ASSERT_NE(report_fidelity, nullptr);
    const JsonValue* transcode = report_fidelity->Find("transcode");
    ASSERT_NE(transcode, nullptr) << "JSON 必须同源落 transcode 子对象";
    EXPECT_TRUE(transcode->Find("inspected")->BoolValueOr(false));
    EXPECT_FALSE(transcode->Find("source_pixel_format")->StringValueOr().empty());
    EXPECT_TRUE(transcode->Has("lost_properties"));
    EXPECT_TRUE(transcode->Has("preserved_properties"));
    EXPECT_TRUE(transcode->Has("all_properties_preserved"));

    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("转码保真探测"), std::string::npos) << "TXT / HTML 必须写明探测状态";
        EXPECT_NE(body.find("已实测"), std::string::npos);
        EXPECT_NE(body.find("源像素格式 / 位深"), std::string::npos);
        EXPECT_NE(body.find(fidelity.source_pixel_format), std::string::npos);
    }
}

TEST(RepairTranscodeFidelity, PreserveBitstreamReportsFidelityAsNotInspected) {
    const std::string dir = MakeTempDir("no_transcode");
    const auto media = videoeye_test::CreateTestMedia(dir, "plain_src");
    ASSERT_TRUE(media.ok) << media.error;
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    ASSERT_FALSE(result.fidelity.video_reencoded);

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    EXPECT_FALSE(fidelity.inspected);
    EXPECT_FALSE(fidelity.all_properties_preserved);
    EXPECT_TRUE(fidelity.lost_properties.empty());
    EXPECT_TRUE(fidelity.preserved_properties.empty());

    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(dir + "/r.json"), root, &error)) << error;
    const JsonValue* report_fidelity = root.Find("fidelity");
    ASSERT_NE(report_fidelity, nullptr);
    const JsonValue* transcode = report_fidelity->Find("transcode");
    ASSERT_NE(transcode, nullptr);
    EXPECT_FALSE(transcode->Find("inspected")->BoolValueOr(true));
    EXPECT_FALSE(transcode->Find("all_properties_preserved")->BoolValueOr(true));

    const std::string text = ReadText(dir + "/r.txt");
    const std::string html = ReadText(dir + "/r.html");
    // 必须钉在「转码保真探测」这一行的值上：只找泛化的「未探测」会被
    // 另一行「源侧属性结论」蒙混过去（变异 M9 第一轮就是这样判绿的）。
    EXPECT_NE(text.find("转码保真探测: 未探测"), std::string::npos) << text;
    EXPECT_NE(html.find("转码保真探测</th><td>未探测"), std::string::npos) << html;
}

// ---- 切片 O：编码器 / profile 事实的渲染 ----

RepairTranscodeFidelity EncoderFacts(const std::string& used, const std::string& preferred, bool fallback) {
    RepairTranscodeFidelity fidelity;
    fidelity.encoder_used = used;
    fidelity.encoder_preferred = preferred;
    fidelity.encoder_fallback = fallback;
    return fidelity;
}

TEST(RepairTranscodeFidelity, EncoderLabelNeverInventsAnEncoder) {
    // 没走到编码器（未转码 / 打开失败）：空串，不许补一个默认名。
    EXPECT_TRUE(repair::TranscodeEncoderLabel(RepairTranscodeFidelity()).empty());
    EXPECT_TRUE(repair::TranscodeEncoderLabel(EncoderFacts("", "libx264", true)).empty());

    // 首选可用：就是首选本身，不能画蛇添足写「已回退」。
    const std::string direct = repair::TranscodeEncoderLabel(EncoderFacts("libx264", "libx264", false));
    EXPECT_EQ(direct, "libx264");
    EXPECT_EQ(direct.find("回退"), std::string::npos);

    // 回退：必须写出实际编码器 + 首选是谁 + 为什么不是首选。
    const std::string fallback = repair::TranscodeEncoderLabel(EncoderFacts("libopenh264", "libx264", true));
    EXPECT_NE(fallback.find("libopenh264"), std::string::npos) << fallback;
    EXPECT_NE(fallback.find("libx264"), std::string::npos) << fallback;
    EXPECT_NE(fallback.find("不可用"), std::string::npos) << fallback;
    // 反向守护：回退时绝不能把首选名当成实际使用的编码器。
    EXPECT_EQ(fallback.find("libx264 重编码"), std::string::npos) << "首选名只能以「首选 ... 不可用」的形式出现";

    // preferred 缺失也不能渲染成「首选  不可用」这种空档。
    const std::string unknown_preferred = repair::TranscodeEncoderLabel(EncoderFacts("libopenh264", "", true));
    EXPECT_NE(unknown_preferred.find("未知"), std::string::npos) << unknown_preferred;
}

TEST(RepairTranscodeFidelity, ProfileNoteRequiresMeasuredBothEnds) {
    // 源侧没声明 profile：整条不输出，不做「大概也是 High」的推断。
    RepairTranscodeFidelity fidelity;
    fidelity.output_profile = "High";
    EXPECT_TRUE(repair::TranscodeProfileNote(fidelity).empty());

    // 产物侧拿不到编码器回报：明说「未能比对」，不能默认写成一致。
    fidelity.source_profile = "High";
    fidelity.output_profile.clear();
    const std::string missing = repair::TranscodeProfileNote(fidelity);
    EXPECT_NE(missing.find("未能比对"), std::string::npos) << missing;
    EXPECT_EQ(missing.find("一致"), std::string::npos) << missing;

    // 两端都有且相同 / 不同，要能区分开。
    fidelity.output_profile = "High";
    EXPECT_NE(repair::TranscodeProfileNote(fidelity).find("一致"), std::string::npos);
    fidelity.output_profile = "Constrained Baseline";
    const std::string changed = repair::TranscodeProfileNote(fidelity);
    EXPECT_NE(changed.find("已改变"), std::string::npos) << changed;
    EXPECT_NE(changed.find("Constrained Baseline"), std::string::npos) << changed;
}

TEST(RepairTranscodeFidelity, S08ReportsMeasuredEncoderInsteadOfPreferredName) {
    const std::string dir = MakeTempDir("s08_encoder");
    const auto media = videoeye_test::CreateTestMedia(dir, "s08_encoder_src");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    ASSERT_TRUE(result.fidelity.video_reencoded);

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    // 走完重编码就必须有实测编码器名 —— 这是唯一可以写进报告的事实源。
    ASSERT_FALSE(fidelity.encoder_used.empty()) << "重编码完成却没有编码器名";
    EXPECT_EQ(fidelity.encoder_preferred, "libx264");
    EXPECT_NE(fidelity.encoder_note.find(fidelity.encoder_used), std::string::npos) << fidelity.encoder_note;

    // 有回退就必须说清原因；没回退就不许留「不可用」这种话。
    const bool lgpl_note_present =
        std::any_of(result.fidelity.limitations.begin(), result.fidelity.limitations.end(),
                    [](const std::string& item) { return item.find("LGPL") != std::string::npos; });
    if (fidelity.encoder_fallback) {
        EXPECT_NE(fidelity.encoder_note.find("不可用"), std::string::npos) << fidelity.encoder_note;
        EXPECT_TRUE(lgpl_note_present) << "回退是许可证取舍，必须写进限制项";
    } else {
        EXPECT_FALSE(lgpl_note_present) << "首选可用时不得声称发生了回退";
    }

    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;

    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(dir + "/r.json"), root, &error)) << error;
    const JsonValue* report_fidelity = root.Find("fidelity");
    ASSERT_NE(report_fidelity, nullptr);
    const JsonValue* transcode = report_fidelity->Find("transcode");
    ASSERT_NE(transcode, nullptr);
    EXPECT_EQ(transcode->Find("encoder_used")->StringValueOr(), fidelity.encoder_used);
    EXPECT_EQ(transcode->Find("encoder_preferred")->StringValueOr(), "libx264");
    EXPECT_EQ(transcode->Find("encoder_fallback")->BoolValueOr(!fidelity.encoder_fallback), fidelity.encoder_fallback);

    const std::string text = ReadText(dir + "/r.txt");
    const std::string html = ReadText(dir + "/r.html");
    // 必须钉在「行」上：限制项里也有一条以「重编码编码器: 」开头的文本
    // （"- 限制 / 未知项: 重编码编码器: ..."），只做裸 find 会被它蒙过去
    // （判别力变异 M4 第一轮就是这样判绿的）。TXT 的行是 "<标题>: <值>\n"。
    const std::string expected_row = "\n重编码编码器: " + fidelity.encoder_note + "\n";
    EXPECT_NE(text.find(expected_row), std::string::npos) << text;
    EXPECT_NE(html.find(fidelity.encoder_note), std::string::npos) << "HTML 必须同源渲染编码器行";
    // 反向守护：报告里任何地方都不得再把首选名当成既成事实。
    EXPECT_EQ(text.find("libx264 重编码"), std::string::npos) << text;
    EXPECT_EQ(html.find("libx264 重编码"), std::string::npos) << html;
    // 内部一致性：报告行里的编码器名必须就是结果里那个，不是另抄一份。
    EXPECT_NE(text.find(fidelity.encoder_used), std::string::npos) << text;
}

TEST(RepairTranscodeFidelity, NonTranscodePathHasNoEncoderFact) {
    const std::string dir = MakeTempDir("no_encoder_fact");
    const auto media = videoeye_test::CreateTestMedia(dir, "plain_no_fact_src");
    ASSERT_TRUE(media.ok) << media.error;
    const RepairResult result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out"), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    ASSERT_FALSE(result.fidelity.video_reencoded);

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    EXPECT_TRUE(fidelity.encoder_used.empty()) << "没转码就没有编码器事实";
    EXPECT_TRUE(fidelity.encoder_note.empty());
    EXPECT_TRUE(fidelity.profile_note.empty());

    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_EQ(body.find("重编码编码器"), std::string::npos) << "保码流路径不得凭空出现编码器行";
    }
}

} // namespace
