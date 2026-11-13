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

extern "C" {
#include <libavutil/mastering_display_metadata.h>
}

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

// ---- 切片 P：音画起点语料 ----
//
// 造一份「合法但音画起点不同」的样本：把前 N 个视频包整个丢掉再重封装。
// 得到的文件容器自洽、能正常播放，只是视频从 N/fps 秒才开始（音频仍从 0 开始）。
// 这正是 S08 要面对的典型源：录音被打断 / 首段视频丢失 / 设备把音频先落盘。
// 不用「破坏字节」是因为 H.264 会做错误隐藏，样本行为不稳定；丢包是确定性的。
std::string CreateAvOffsetMedia(const std::string& directory, const std::string& name, int shift_video_frames) {
    const videoeye_test::TestMedia base = videoeye_test::CreateTestMedia(directory, name + "_base");
    if (!base.ok)
        return std::string("base media failed: ") + base.error;

    const std::string out_path = directory + "/" + name + ".mp4";
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, base.path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
        if (in != nullptr)
            avformat_close_input(&in);
        return "open base failed";
    }
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, nullptr, out_path.c_str()) < 0 || out == nullptr) {
        avformat_close_input(&in);
        return "alloc output failed";
    }
    std::vector<int> mapping(in->nb_streams, -1);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream* dst = avformat_new_stream(out, nullptr);
        if (dst == nullptr || avcodec_parameters_copy(dst->codecpar, in->streams[i]->codecpar) < 0) {
            avformat_free_context(out);
            avformat_close_input(&in);
            return "copy stream failed";
        }
        dst->codecpar->codec_tag = 0;
        dst->time_base = in->streams[i]->time_base;
        mapping[i] = dst->index;
    }
    if (avio_open(&out->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(out, nullptr) < 0) {
        avformat_free_context(out);
        avformat_close_input(&in);
        return "open output failed";
    }
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(in, packet) >= 0) {
        const int src_index = packet->stream_index;
        const AVRational src_tb = in->streams[src_index]->time_base;
        const bool is_video = in->streams[src_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        packet->stream_index = mapping[src_index];
        const AVRational dst_tb = out->streams[packet->stream_index]->time_base;
        av_packet_rescale_ts(packet, src_tb, dst_tb);
        if (is_video) {
            // 整体推后 shift_video_frames 帧：音频仍从 0 开始，于是源片
            // 自带一段 A/V 起点差（相当于 -itsoffset）。
            const int64_t ticks =
                av_rescale(shift_video_frames, dst_tb.den, static_cast<int64_t>(dst_tb.num) * base.fps);
            if (packet->pts != AV_NOPTS_VALUE)
                packet->pts += ticks;
            if (packet->dts != AV_NOPTS_VALUE)
                packet->dts += ticks;
        }
        av_interleaved_write_frame(out, packet);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    return out_path;
}

// ---- 切片 Q：方向信息（displaymatrix）语料 ----
//
// 造一份「容器声明了画面旋转」的样本：重封装时给视频流的 codecpar 挂上
// AV_PKT_DATA_DISPLAYMATRIX，mp4 muxer 会把它写成 tkhd 里的变换矩阵。
// 像素本身不转 —— 手机竖拍拍出来的就是这种形态（画面横着存、靠矩阵转正），
// 所以「只拷画面不拷矩阵」会让产物躺倒 90°。
std::string CreateRotatedMedia(const std::string& directory, const std::string& name, double degrees,
                               bool with_hdr = false) {
    const videoeye_test::TestMedia base = videoeye_test::CreateTestMedia(directory, name + "_base");
    if (!base.ok)
        return std::string("base media failed: ") + base.error;

    const std::string out_path = directory + "/" + name + ".mp4";
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, base.path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
        if (in != nullptr)
            avformat_close_input(&in);
        return "open base failed";
    }
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, nullptr, out_path.c_str()) < 0 || out == nullptr) {
        avformat_close_input(&in);
        return "alloc output failed";
    }
    std::vector<int> mapping(in->nb_streams, -1);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream* dst = avformat_new_stream(out, nullptr);
        if (dst == nullptr || avcodec_parameters_copy(dst->codecpar, in->streams[i]->codecpar) < 0) {
            avformat_free_context(out);
            avformat_close_input(&in);
            return "copy stream failed";
        }
        dst->codecpar->codec_tag = 0;
        dst->time_base = in->streams[i]->time_base;
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            std::int32_t matrix[9] = {};
            av_display_rotation_set(matrix, degrees);
            AVPacketSideData* side =
                av_packet_side_data_new(&dst->codecpar->coded_side_data, &dst->codecpar->nb_coded_side_data,
                                        AV_PKT_DATA_DISPLAYMATRIX, sizeof(matrix), 0);
            if (side != nullptr)
                std::memcpy(side->data, matrix, sizeof(matrix));
            if (with_hdr) {
                // BT.2020 / 1000 nit 的一组典型 HDR10 静态元数据。数值本身不重要，
                // 重要的是「源容器确实声明了它」这个事实。
                AVMasteringDisplayMetadata mastering{};
                mastering.display_primaries[0][0] = av_make_q(34000, 50000);
                mastering.display_primaries[0][1] = av_make_q(16000, 50000);
                mastering.display_primaries[1][0] = av_make_q(13250, 50000);
                mastering.display_primaries[1][1] = av_make_q(34500, 50000);
                mastering.display_primaries[2][0] = av_make_q(7500, 50000);
                mastering.display_primaries[2][1] = av_make_q(3000, 50000);
                mastering.white_point[0] = av_make_q(15635, 50000);
                mastering.white_point[1] = av_make_q(16450, 50000);
                mastering.min_luminance = av_make_q(50, 10000);
                mastering.max_luminance = av_make_q(1000, 1);
                mastering.has_primaries = 1;
                mastering.has_luminance = 1;
                AVPacketSideData* mastering_side =
                    av_packet_side_data_new(&dst->codecpar->coded_side_data, &dst->codecpar->nb_coded_side_data,
                                            AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof(mastering), 0);
                if (mastering_side != nullptr)
                    std::memcpy(mastering_side->data, &mastering, sizeof(mastering));
                AVContentLightMetadata light{};
                light.MaxCLL = 1000;
                light.MaxFALL = 400;
                AVPacketSideData* light_side =
                    av_packet_side_data_new(&dst->codecpar->coded_side_data, &dst->codecpar->nb_coded_side_data,
                                            AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof(light), 0);
                if (light_side != nullptr)
                    std::memcpy(light_side->data, &light, sizeof(light));
            }
        }
        mapping[i] = dst->index;
    }
    if (avio_open(&out->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(out, nullptr) < 0) {
        avformat_free_context(out);
        avformat_close_input(&in);
        return "open output failed";
    }
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(in, packet) >= 0) {
        const int src_index = packet->stream_index;
        const AVRational src_tb = in->streams[src_index]->time_base;
        packet->stream_index = mapping[src_index];
        const AVRational dst_tb = out->streams[packet->stream_index]->time_base;
        av_packet_rescale_ts(packet, src_tb, dst_tb);
        av_interleaved_write_frame(out, packet);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    return out_path;
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

// 第三类状态：既没确认丢失、也没确认保留（产物侧没实测）。
bool HasUnverified(const RepairTranscodeFidelity& fidelity, const std::string& needle) {
    for (const auto& item : fidelity.unverified_properties)
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

// ---- 切片 Q：方向矩阵 / HDR 的「三段事实」 ----
//
// 旧实现把「未继承」直接写死在 InspectTranscodeFidelity 里 —— 那是从「我们没拷」
// 推出来的，产物侧一次没量过。现在拷了也测了，结论必须按实测落：
//   未实测 → unverified（既不能说丢失，也不能说保留）
//   实测有且一致 → preserved
//   实测无 / 角度不同 → lost

TEST(RepairTranscodeFidelity, RotationMatrixWithoutOutputEvidenceStaysUnverified) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    std::uint8_t* raw = probe.AddSideData(AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t));
    ASSERT_NE(raw, nullptr);
    auto* matrix = reinterpret_cast<std::int32_t*>(raw);
    av_display_rotation_set(matrix, 90.0);
    ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    // 只探测源侧时：源侧事实在，但一条结论都不许下。
    EXPECT_TRUE(result.fidelity.has_display_matrix);
    EXPECT_GT(std::abs(result.fidelity.rotation_degrees), 80.0);
    EXPECT_TRUE(result.fidelity.lost_properties.empty()) << "产物侧还没测，不能说已丢失";
    EXPECT_TRUE(result.fidelity.preserved_properties.empty()) << "产物侧还没测，不能说已保留";
    EXPECT_FALSE(result.fidelity.all_properties_preserved);

    // 产物侧打不开 / 没测：进第三类，不许冒充任何一种结论。
    repair::FinalizeSideDataInheritance(result.fidelity, /*measured=*/false, false, 0.0, false);
    EXPECT_TRUE(HasUnverified(result.fidelity, "旋转 / 镜像"));
    EXPECT_TRUE(result.fidelity.lost_properties.empty());
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
    EXPECT_FALSE(result.fidelity.display_matrix_note.empty());
    EXPECT_NE(result.fidelity.display_matrix_note.find("未实测"), std::string::npos);
}

TEST(RepairTranscodeFidelity, MeasuredOutputWithoutMatrixReportsLoss) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    std::uint8_t* raw = probe.AddSideData(AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t));
    ASSERT_NE(raw, nullptr);
    auto* matrix = reinterpret_cast<std::int32_t*>(raw);
    av_display_rotation_set(matrix, 90.0);
    ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    repair::FinalizeSideDataInheritance(result.fidelity, /*measured=*/true, /*output_has_matrix=*/false, 0.0, false);
    EXPECT_TRUE(HasLost(result.fidelity, "旋转 / 镜像"));
    // 文案必须说清这是产物实测结果，不是「我们没拷」的推断。
    EXPECT_TRUE(HasLost(result.fidelity, "产物实测无方向信息"));
    EXPECT_FALSE(result.fidelity.all_properties_preserved);
    EXPECT_TRUE(result.fidelity.unverified_properties.empty());
}

TEST(RepairTranscodeFidelity, RotationAngleChangeIsNotSilentlyTreatedAsPreserved) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    std::uint8_t* raw = probe.AddSideData(AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t));
    ASSERT_NE(raw, nullptr);
    auto* matrix = reinterpret_cast<std::int32_t*>(raw);
    av_display_rotation_set(matrix, 90.0);
    ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    // 产物有矩阵但角度不同：这不是「已继承」，必须报角度已改变。
    repair::FinalizeSideDataInheritance(result.fidelity, /*measured=*/true, /*output_has_matrix=*/true, 0.0, false);
    EXPECT_TRUE(HasLost(result.fidelity, "角度已改变"));
    EXPECT_FALSE(HasKept(result.fidelity, "旋转 / 镜像"));
    EXPECT_TRUE(result.fidelity.output_has_display_matrix);
    EXPECT_DOUBLE_EQ(result.fidelity.output_rotation_degrees, 0.0);
}

TEST(RepairTranscodeFidelity, IdentityDisplayMatrixIsNotALoss) {
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P);
    std::uint8_t* raw = probe.AddSideData(AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t));
    ASSERT_NE(raw, nullptr);
    auto* matrix = reinterpret_cast<std::int32_t*>(raw);
    av_display_rotation_set(matrix, 0.0);
    ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);

    EXPECT_FALSE(result.fidelity.has_display_matrix) << "单位矩阵不含方向信息，不该凭空报损失";
    repair::FinalizeSideDataInheritance(result.fidelity, true, false, 0.0, false);
    EXPECT_FALSE(HasLost(result.fidelity, "旋转 / 镜像"));
    EXPECT_FALSE(HasUnverified(result.fidelity, "旋转 / 镜像"));
    EXPECT_TRUE(result.fidelity.display_matrix_note.empty()) << "源侧没有方向信息，不许印一行「未继承」";
    EXPECT_TRUE(result.fidelity.all_properties_preserved);
}

// HDR 元数据有位深前提：10 bit 源压成 8 bit 产物后不许照搬 HDR 标记 —— 那会让
// 播放器把一个 SDR 画面按 HDR 渲染，比不写标记更糟（与色彩标记同一条理由）。
TEST(RepairTranscodeFidelity, HdrInheritanceRequiresSameBitDepth) {
    EXPECT_TRUE(repair::TranscodeHdrInheritable(8, 8));
    EXPECT_TRUE(repair::TranscodeHdrInheritable(10, 10));
    EXPECT_FALSE(repair::TranscodeHdrInheritable(10, 8)) << "位深降级必须拦住";
    EXPECT_FALSE(repair::TranscodeHdrInheritable(0, 8)) << "源位深未知时不许当作可继承";

    // 拦下来之后，结论必须说清是「主动不沿用」而不是「muxer 丢了」。
    FidelityProbe probe;
    ASSERT_TRUE(probe.Ready());
    probe.SetSource(AV_PIX_FMT_YUV420P10LE);
    probe.AddSideData(AV_PKT_DATA_MASTERING_DISPLAY_METADATA, 8);
    probe.AddSideData(AV_PKT_DATA_CONTENT_LIGHT_LEVEL, 4);
    ProbeResult result = Inspect(probe, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV420P);
    ASSERT_TRUE(result.fidelity.has_hdr_metadata);
    ASSERT_FALSE(repair::TranscodeHdrInheritable(result.fidelity.source_bit_depth, result.fidelity.output_bit_depth));
    result.fidelity.hdr_inherit_blocked_by_bit_depth = true;
    repair::FinalizeSideDataInheritance(result.fidelity, /*measured=*/true, false, 0.0, /*output_has_hdr=*/false);
    EXPECT_TRUE(HasLost(result.fidelity, "位深已降级"));
    EXPECT_FALSE(HasLost(result.fidelity, "产物实测无该元数据")) << "原因不同，文案不能混用";
    EXPECT_EQ(result.fidelity.hdr_metadata_note, "位深降级，主动不沿用");

    // 矛盾检测：明明决定不拷，产物却带着 → 不能当成功，要报异常。
    FidelityProbe conflict;
    ASSERT_TRUE(conflict.Ready());
    conflict.SetSource(AV_PIX_FMT_YUV420P10LE);
    conflict.AddSideData(AV_PKT_DATA_MASTERING_DISPLAY_METADATA, 8);
    ProbeResult second = Inspect(conflict, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV420P);
    ASSERT_TRUE(second.fidelity.has_hdr_metadata);
    second.fidelity.hdr_inherit_blocked_by_bit_depth = true;
    repair::FinalizeSideDataInheritance(second.fidelity, /*measured=*/true, false, 0.0, /*output_has_hdr=*/true);
    EXPECT_TRUE(HasLost(second.fidelity, "位深已降级")) << "位深降级却仍在产物中，必须报出来而不是记成「已保留」";
    EXPECT_FALSE(HasKept(second.fidelity, "HDR 静态元数据"));
    EXPECT_EQ(second.fidelity.hdr_metadata_note, "位深降级却仍在产物中（异常）");
}

TEST(RepairTranscodeFidelity, HdrMetadataFollowsSameThreeStateRule) {
    auto make_probe = [](FidelityProbe& probe) {
        probe.SetSource(AV_PIX_FMT_YUV420P);
        probe.AddSideData(AV_PKT_DATA_MASTERING_DISPLAY_METADATA, 8);
        probe.AddSideData(AV_PKT_DATA_CONTENT_LIGHT_LEVEL, 4);
    };

    // 未实测 → 第三类。
    FidelityProbe unmeasured;
    ASSERT_TRUE(unmeasured.Ready());
    make_probe(unmeasured);
    ProbeResult a = Inspect(unmeasured, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);
    EXPECT_TRUE(a.fidelity.has_hdr_metadata);
    repair::FinalizeSideDataInheritance(a.fidelity, false, false, 0.0, false);
    EXPECT_TRUE(HasUnverified(a.fidelity, "HDR 静态元数据"));
    EXPECT_TRUE(a.fidelity.lost_properties.empty());

    // 实测有 → 保留。
    FidelityProbe measured;
    ASSERT_TRUE(measured.Ready());
    make_probe(measured);
    ProbeResult b = Inspect(measured, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);
    repair::FinalizeSideDataInheritance(b.fidelity, true, false, 0.0, /*output_has_hdr=*/true);
    EXPECT_TRUE(HasKept(b.fidelity, "HDR 静态元数据"));
    EXPECT_FALSE(HasLost(b.fidelity, "HDR 静态元数据"));
    EXPECT_TRUE(b.fidelity.all_properties_preserved);

    // 实测无 → 丢失。
    FidelityProbe missing;
    ASSERT_TRUE(missing.Ready());
    make_probe(missing);
    ProbeResult c = Inspect(missing, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P);
    repair::FinalizeSideDataInheritance(c.fidelity, true, false, 0.0, /*output_has_hdr=*/false);
    EXPECT_TRUE(HasLost(c.fidelity, "HDR 静态元数据"));
    EXPECT_FALSE(c.fidelity.all_properties_preserved);
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

// ---- 切片 P：音画起点必须保留，不能各自归零 ----

// 源片自带 0.48 s 的音画起点差（视频比音频晚 12 帧）。旧实现把视频按帧序号、
// 音频按各自首包都归零，产物两边都是 0 —— 差值被静默抹掉，报告一个字都不提。
TEST(RepairTranscodeFidelity, S08PreservesAudioVideoStartOffset) {
    const std::string dir = MakeTempDir("av_offset");
    const int shift_frames = 12;
    const std::string source = CreateAvOffsetMedia(dir, "av_offset", shift_frames);
    ASSERT_TRUE(fs::exists(source)) << source;
    const auto source_probe = videoeye_test::ProbeMediaFile(source);
    ASSERT_TRUE(source_probe.opened) << source_probe.error;
    const double source_offset = source_probe.video_start_seconds - source_probe.audio_start_seconds;
    ASSERT_GT(source_offset, 0.4) << "语料必须先真的带上起点差";

    RepairRequest request = MakeRequest(source, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    // 1. 结论层必须量到这个差值，并声明保留。
    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    ASSERT_TRUE(fidelity.av_start_offset_seconds.has_value()) << "两条轨都有起点，必须比对";
    EXPECT_NEAR(*fidelity.av_start_offset_seconds, source_offset, 0.03) << fidelity.av_sync_note;
    EXPECT_TRUE(fidelity.av_start_offset_preserved) << fidelity.av_sync_note;
    EXPECT_NE(fidelity.av_sync_note.find("相对关系保留"), std::string::npos) << fidelity.av_sync_note;

    // 2. 产物自身必须真的保住这个差值 —— 报告说保留、产物却对齐了就是假结论。
    const auto output_probe = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(output_probe.opened) << output_probe.error;
    const double output_offset = output_probe.video_start_seconds - output_probe.audio_start_seconds;
    EXPECT_NEAR(output_offset, source_offset, 0.03)
        << "产物音画起点差被改写了：source=" << source_offset << " output=" << output_offset;

    // 3. 三格式同源。
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(dir + "/r.json"), root, &error)) << error;
    const JsonValue* transcode = root.Find("fidelity");
    ASSERT_NE(transcode, nullptr);
    const JsonValue* node = transcode->Find("transcode");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->Find("av_start_offset_seconds")->IsNumber()) << "JSON 必须是数字而不是文案";
    EXPECT_TRUE(node->Find("av_start_offset_preserved")->BoolValueOr(false));
    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("音画起点差"), std::string::npos);
        EXPECT_NE(body.find("音画同步处理"), std::string::npos);
    }
}

// 反向守护：源片没有起点差时不得凭空造一个。这条钉的是「锚点取容器呈现起点」
// —— 拿首包/首帧的原始 pts 当锚点会稳定多出一帧（AAC priming 是负的）。
TEST(RepairTranscodeFidelity, S08DoesNotInventAvOffsetWhenSourceHasNone) {
    const std::string dir = MakeTempDir("av_aligned");
    const std::string source = CreateAvOffsetMedia(dir, "av_aligned", 0);
    ASSERT_TRUE(fs::exists(source)) << source;
    const auto source_probe = videoeye_test::ProbeMediaFile(source);
    ASSERT_TRUE(source_probe.opened) << source_probe.error;
    ASSERT_NEAR(source_probe.video_start_seconds - source_probe.audio_start_seconds, 0.0, 0.01);

    RepairRequest request = MakeRequest(source, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    ASSERT_TRUE(fidelity.av_start_offset_seconds.has_value());
    EXPECT_NEAR(*fidelity.av_start_offset_seconds, 0.0, 0.01) << fidelity.av_sync_note;
    EXPECT_NE(fidelity.av_sync_note.find("相对关系保留"), std::string::npos);

    const auto output_probe = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(output_probe.opened) << output_probe.error;
    // 一帧以内：不允许因为「把 priming 负值当起点」而稳定多出一帧。
    EXPECT_NEAR(output_probe.video_start_seconds - output_probe.audio_start_seconds, 0.0, 0.03)
        << "video=" << output_probe.video_start_seconds << " audio=" << output_probe.audio_start_seconds;
    EXPECT_NEAR(output_probe.video_start_seconds, 0.0, 0.03) << "产物视频不该一开始就晚一帧";
}

// 单轨视频没有音画同步可言：不许写「音画同步已保持」这类没有对象的结论。
TEST(RepairTranscodeFidelity, VideoOnlySourceClaimsNoAvSync) {
    const std::string dir = MakeTempDir("video_only_sync");
    videoeye_test::TestMediaSpec spec;
    spec.with_audio = false;
    const auto media = videoeye_test::CreateTestMedia(dir, "video_only", spec);
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    EXPECT_FALSE(fidelity.av_start_offset_seconds.has_value()) << "没有音频轨就没有起点差";
    EXPECT_TRUE(fidelity.av_sync_note.empty()) << fidelity.av_sync_note;
    EXPECT_FALSE(fidelity.av_start_offset_preserved);
}

// ==== 端到端：S08 真的把方向矩阵 / HDR 元数据带进产物 ====

// 手机竖拍的片子是「画面横着存 + 容器声明 90°」：只重编码像素、不拷矩阵，
// 产物就会躺倒。这条钉三件事：拷贝动作、产物自身实测、报告三格式同源。
TEST(RepairTranscodeFidelity, S08InheritsRotationMatrixAndVerifiesInOutput) {
    const std::string dir = MakeTempDir("rot_inherit");
    const std::string source = CreateRotatedMedia(dir, "rot", 90.0);
    ASSERT_TRUE(fs::exists(source)) << source;
    const auto source_probe = videoeye_test::ProbeMediaFile(source);
    ASSERT_TRUE(source_probe.opened) << source_probe.error;
    ASSERT_TRUE(source_probe.has_display_matrix) << "语料必须先真的带上方向信息";
    ASSERT_GT(std::abs(source_probe.video_rotation_degrees), 80.0);

    RepairRequest request = MakeRequest(source, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    EXPECT_TRUE(fidelity.display_matrix_attached) << "没挂到输出流就不可能进产物";
    EXPECT_TRUE(fidelity.side_data_output_measured);
    EXPECT_TRUE(fidelity.output_has_display_matrix) << fidelity.display_matrix_note;
    EXPECT_NEAR(fidelity.output_rotation_degrees, source_probe.video_rotation_degrees, 0.5)
        << fidelity.display_matrix_note;

    // 产物自己必须真的带角度 —— 报告说继承、产物却躺倒就是假结论。
    const auto output_probe = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(output_probe.opened) << output_probe.error;
    ASSERT_TRUE(output_probe.has_display_matrix) << "产物丢了方向信息，画面会躺倒";
    EXPECT_NEAR(output_probe.video_rotation_degrees, source_probe.video_rotation_degrees, 0.5);

    EXPECT_TRUE(HasKept(fidelity, "旋转 / 镜像"));
    EXPECT_FALSE(HasLost(fidelity, "旋转 / 镜像"));
    EXPECT_FALSE(HasUnverified(fidelity, "旋转 / 镜像"));

    // 三格式同源：行文案由修复层渲染，报告层不另拼一份。
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("旋转 / 镜像矩阵"), std::string::npos);
        EXPECT_NE(body.find("产物实测"), std::string::npos) << "行文案必须说明产物侧实测结果";
        EXPECT_EQ(body.find("转码属性未实测"), std::string::npos) << "已经实测过，不该再报「未实测」";
    }
    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(dir + "/r.json"), root, &error)) << error;
    const JsonValue* transcode = root.Find("fidelity");
    ASSERT_NE(transcode, nullptr);
    const JsonValue* node = transcode->Find("transcode");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->Find("display_matrix_attached")->BoolValueOr(false));
    EXPECT_TRUE(node->Find("side_data_output_measured")->BoolValueOr(false));
    EXPECT_TRUE(node->Find("output_has_display_matrix")->BoolValueOr(false));
}

TEST(RepairTranscodeFidelity, S08InheritsHdrStaticMetadata) {
    const std::string dir = MakeTempDir("hdr_inherit");
    const std::string source = CreateRotatedMedia(dir, "hdr", 0.0, /*with_hdr=*/true);
    ASSERT_TRUE(fs::exists(source)) << source;
    const auto source_probe = videoeye_test::ProbeMediaFile(source);
    ASSERT_TRUE(source_probe.opened) << source_probe.error;
    ASSERT_TRUE(source_probe.has_hdr_metadata) << "语料必须先真的带上 HDR 静态元数据";

    RepairRequest request = MakeRequest(source, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    EXPECT_TRUE(fidelity.has_hdr_metadata);
    EXPECT_TRUE(fidelity.hdr_metadata_attached);
    EXPECT_TRUE(fidelity.side_data_output_measured);
    EXPECT_TRUE(fidelity.output_has_hdr_metadata) << fidelity.hdr_metadata_note;

    const auto output_probe = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(output_probe.opened) << output_probe.error;
    ASSERT_TRUE(output_probe.has_hdr_metadata) << "产物没带上 HDR 静态元数据";

    EXPECT_TRUE(HasKept(fidelity, "HDR 静态元数据"));
    EXPECT_FALSE(HasLost(fidelity, "HDR 静态元数据"));
    EXPECT_FALSE(HasUnverified(fidelity, "HDR 静态元数据"));

    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("HDR 静态元数据"), std::string::npos);
    }
}

// 源片没有方向信息时，别凭空印一行「源未声明」以外的任何东西，
// 更不能因此让「属性均已保留」变成 false。
TEST(RepairTranscodeFidelity, SourceWithoutSideDataClaimsNothing) {
    const std::string dir = MakeTempDir("no_side_data");
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "plain");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    EXPECT_FALSE(fidelity.has_display_matrix);
    EXPECT_FALSE(fidelity.has_hdr_metadata);
    EXPECT_FALSE(fidelity.display_matrix_attached);
    EXPECT_FALSE(fidelity.hdr_metadata_attached);
    EXPECT_TRUE(fidelity.side_data_output_measured);
    EXPECT_TRUE(fidelity.display_matrix_note.empty()) << fidelity.display_matrix_note;
    EXPECT_TRUE(fidelity.hdr_metadata_note.empty()) << fidelity.hdr_metadata_note;
    EXPECT_TRUE(fidelity.unverified_properties.empty());
    EXPECT_FALSE(HasLost(fidelity, "旋转 / 镜像"));
    EXPECT_FALSE(HasLost(fidelity, "HDR 静态元数据"));
}

} // namespace
