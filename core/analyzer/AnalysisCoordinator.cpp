#include "core/analyzer/AnalysisCoordinator.h"
#include "core/analyzer/TimelineAnalyzer.h"

#include <QMetaType>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "utils/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace videoeye {
namespace analyzer {
namespace {

constexpr int kProgressMinIntervalMs = 100;
constexpr int kProgressMinPackets = 2000;
constexpr int64_t kMaxLayoutScanBytes = 8 * 1024 * 1024;  // moov/mdat 顺序扫描上限

// 大端读取（MP4 box header）
uint32_t ReadBe32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool IsMp4Family(const std::string& format_name) {
    static const char* kNames[] = {"mov", "mp4", "m4v", "3gp", "3g2", "isom", "quicktime", "f4v"};
    for (const char* name : kNames) {
        if (format_name == name) return true;
    }
    return false;
}

// 扫描顶层 box 顺序，判断 moov 是否在 mdat 之后（未 faststart）
bool ScanMoovAfterMdat(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    int64_t offset = 0;
    while (offset < kMaxLayoutScanBytes) {
        unsigned char header[16] = {0};
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        const std::streamsize got = file.gcount();
        if (got < 8) return false;

        const uint32_t size32 = ReadBe32(header);
        const std::string box_type(reinterpret_cast<char*>(header + 4), 4);

        int64_t box_size = static_cast<int64_t>(size32);
        int64_t header_size = 8;
        if (size32 == 1) {  // 64 位 largesize
            if (got < 16) return false;
            const uint64_t large = (static_cast<uint64_t>(ReadBe32(header + 8)) << 32) |
                                   static_cast<uint64_t>(ReadBe32(header + 12));
            if (large < 16) return false;
            box_size = static_cast<int64_t>(large);
            header_size = 16;
        } else if (size32 == 0) {
            return false;  // 延伸到文件尾，无法继续遍历
        }

        if (box_size < header_size) return false;

        if (box_type == "moov") return false;
        if (box_type == "mdat") return true;

        offset += box_size;
    }
    return false;
}

struct Bucket {
    int64_t total_bytes = 0;
    int64_t video_bytes = 0;
    int64_t video_frames = 0;
};

// 帧类型探测：优先用解码器（准确但慢），否则用 codec parser（几乎零成本）。
// 两者都拿不到时，调用方退回 OnPacket()，只按 AV_PKT_FLAG_KEY 识别 I 帧。
struct FrameTypeProbe {
    AVCodecContext* parser_ctx = nullptr;    // 仅供 av_parser_parse2 使用的参数上下文
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    bool use_decoder = false;

    void Release() {
        if (parser) av_parser_close(parser);
        if (parser_ctx) avcodec_free_context(&parser_ctx);
        if (frame) av_frame_free(&frame);
        if (decoder) avcodec_free_context(&decoder);
        parser = nullptr;
        parser_ctx = nullptr;
        frame = nullptr;
        decoder = nullptr;
        use_decoder = false;
    }
};

}  // namespace

AnalysisCoordinator::AnalysisCoordinator(QObject* parent) : QObject(parent) {
    qRegisterMetaType<AnalysisResult>("videoeye::analyzer::AnalysisResult");
    qRegisterMetaType<AnalysisResult>("AnalysisResult");
}

AnalysisCoordinator::~AnalysisCoordinator() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}

quint64 AnalysisCoordinator::StartAnalysis(const std::string& file_path,
                                           const AnalysisOptions& options) {
    Cancel();
    if (worker_.joinable()) worker_.join();

    const quint64 gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    cancel_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    worker_ = std::thread(&AnalysisCoordinator::Run, this, gen, file_path, options);
    return gen;
}

void AnalysisCoordinator::Cancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

void AnalysisCoordinator::Run(quint64 generation, std::string file_path, AnalysisOptions options) {
    AnalysisResult result;
    result.file_path = file_path;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file_path.c_str(), nullptr, nullptr) < 0) {
        running_.store(false, std::memory_order_release);
        emit AnalysisFailed(generation, QString::fromStdString("无法打开文件: " + file_path));
        return;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        running_.store(false, std::memory_order_release);
        emit AnalysisFailed(generation, QString::fromStdString("无法解析流信息: " + file_path));
        return;
    }

    result.container_format = fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "";
    result.duration_seconds = (fmt->duration > 0)
                                  ? static_cast<double>(fmt->duration) / static_cast<double>(AV_TIME_BASE)
                                  : 0.0;
    result.file_size_bytes = fmt->pb ? avio_size(fmt->pb) : 0;
    result.overall_bitrate_bps = (fmt->bit_rate > 0) ? fmt->bit_rate : 0;
    result.seekable = (fmt->pb == nullptr) ? false : ((fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0);

    if (options.detect_container_layout && IsMp4Family(result.container_format)) {
        result.moov_after_mdat = ScanMoovAfterMdat(file_path);
    }

    // ---- 流摘要 ----
    const double file_duration = result.duration_seconds;
    result.streams.reserve(fmt->nb_streams);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        StreamDigest digest;
        digest.index = static_cast<int>(i);
        digest.media_type = static_cast<int>(st->codecpar->codec_type);
        const AVCodecID codec_id = st->codecpar->codec_id;
        const char* codec_name = avcodec_get_name(codec_id);
        digest.codec_name = codec_name ? codec_name : "";
        const char* profile = avcodec_profile_name(codec_id, st->codecpar->profile);
        digest.profile_name = profile ? profile : "";
        digest.width = st->codecpar->width;
        digest.height = st->codecpar->height;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
            digest.avg_fps = av_q2d(st->avg_frame_rate);
        }
        digest.sample_rate = st->codecpar->sample_rate;
        digest.channels = st->codecpar->ch_layout.nb_channels;
        digest.bitrate_bps = (st->codecpar->bit_rate > 0) ? st->codecpar->bit_rate : 0;
        const double tb = av_q2d(st->time_base);
        if (st->start_time != AV_NOPTS_VALUE && tb > 0.0) {
            digest.start_seconds = static_cast<double>(st->start_time) * tb;
        }
        if (st->duration != AV_NOPTS_VALUE && tb > 0.0) {
            digest.duration_seconds = static_cast<double>(st->duration) * tb;
        } else {
            digest.duration_seconds = file_duration;
        }
        result.streams.push_back(std::move(digest));
    }

    // ---- 码率与 GOP 深度分析准备 ----
    int video_stream_index = -1;
    if (options.analyze_bitrate_gop) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = static_cast<int>(i);
                break;
            }
        }
    }

    BitrateGopAnalyzer bitrate_gop;
    FrameTypeProbe probe;
    if (options.analyze_bitrate_gop && video_stream_index >= 0) {
        bitrate_gop.Reset(options.bitrate_gop_options);
        AVStream* vst = fmt->streams[video_stream_index];

        if (options.decode_frame_types) {
            const AVCodec* codec = avcodec_find_decoder(vst->codecpar->codec_id);
            if (codec != nullptr) {
                probe.decoder = avcodec_alloc_context3(codec);
                if (probe.decoder != nullptr &&
                    avcodec_parameters_to_context(probe.decoder, vst->codecpar) >= 0) {
                    probe.decoder->pkt_timebase = vst->time_base;
                    probe.frame = av_frame_alloc();
                    if (probe.frame != nullptr &&
                        avcodec_open2(probe.decoder, codec, nullptr) == 0) {
                        probe.use_decoder = true;
                    }
                }
                if (!probe.use_decoder) {
                    // 打开失败 → 退回 parser
                    if (probe.frame) { av_frame_free(&probe.frame); }
                    if (probe.decoder) { avcodec_free_context(&probe.decoder); }
                }
            }
        }
        if (!probe.use_decoder) {
            probe.parser = av_parser_init(vst->codecpar->codec_id);
            if (probe.parser != nullptr) {
                // 我们喂的是完整访问单元（一个包 = 一帧），告诉解析器不要做拼接
                probe.parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
                probe.parser_ctx = avcodec_alloc_context3(nullptr);
                if (probe.parser_ctx != nullptr) {
                    avcodec_parameters_to_context(probe.parser_ctx, vst->codecpar);
                }
            }
        }
    }

    // ---- 逐包扫描 ----
    const double interval = (options.sample_interval_seconds > 0.0) ? options.sample_interval_seconds : 1.0;
    std::map<int64_t, Bucket> buckets;
    std::vector<int64_t> frames_since_key(fmt->nb_streams, 0);
    double last_key_ts = -1.0;
    bool has_key = false;

    AVPacket* pkt = av_packet_alloc();
    int64_t packet_index = 0;
    int64_t last_pos = 0;
    TimelineAnalyzer timeline_analyzer;

    auto last_progress = std::chrono::steady_clock::now();
    emit ProgressReported(generation, 0.0, QStringLiteral("扫描数据包"));

    while (true) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            result.completed = false;
            break;
        }
        const int ret = av_read_frame(fmt, pkt);
        if (ret < 0) break;  // EOF 或错误

        if (pkt->stream_index < 0 ||
            pkt->stream_index >= static_cast<int>(fmt->nb_streams)) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* st = fmt->streams[pkt->stream_index];
        StreamDigest& digest = result.streams[pkt->stream_index];
        const double tb = av_q2d(st->time_base);
        const bool is_video = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        const bool has_pts = (pkt->pts != AV_NOPTS_VALUE);
        const double ts = has_pts ? static_cast<double>(pkt->pts) * tb : -1.0;

        // 时间轴与同步诊断（demux 层：PTS/DTS/B 帧/首帧偏移/关键帧索引）
        {
            model::PacketTiming timing;
            timing.index = static_cast<int>(packet_index);
            timing.stream_index = pkt->stream_index;
            timing.media_type = static_cast<int>(st->codecpar->codec_type);
            timing.pts_ms = has_pts ? ts * 1000.0 : model::kNoTimestamp;
            timing.dts_ms = (pkt->dts != AV_NOPTS_VALUE)
                                ? static_cast<double>(pkt->dts) * tb * 1000.0
                                : model::kNoTimestamp;
            timing.duration_ms = (pkt->duration > 0)
                                     ? static_cast<double>(pkt->duration) * tb * 1000.0
                                     : model::kNoTimestamp;
            timing.pos = pkt->pos;
            timing.size = pkt->size;
            timing.flags = pkt->flags;
            timing.key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            timeline_analyzer.OnPacket(timing);
        }

        result.total_packets += 1;
        result.total_bytes += pkt->size;
        digest.packet_count += 1;
        digest.byte_count += pkt->size;
        if (pkt->size > result.max_packet_bytes) result.max_packet_bytes = pkt->size;
        if (pkt->dts == AV_NOPTS_VALUE) result.packets_missing_dts += 1;

        if (is_video) {
                result.video_packets += 1;
            digest.frame_count += 1;
            frames_since_key[pkt->stream_index] += 1;

            if (pkt->flags & AV_PKT_FLAG_KEY) {
                result.key_frame_count += 1;
                digest.key_frame_count += 1;
                if (has_key && ts >= 0.0 && last_key_ts >= 0.0) {
                    const double gap = ts - last_key_ts;
                    if (gap > 0.0) {
                        result.gop_intervals_seconds.push_back(gap);
                        result.gop_frame_sizes.push_back(
                            static_cast<int>(frames_since_key[pkt->stream_index]));
                        if (gap > result.max_gop_interval_seconds) {
                            result.max_gop_interval_seconds = gap;
                        }
                    }
                }
                if (frames_since_key[pkt->stream_index] > result.max_gop_frames) {
                    result.max_gop_frames = static_cast<int>(frames_since_key[pkt->stream_index]);
                }
                frames_since_key[pkt->stream_index] = 0;
                if (ts >= 0.0) {
                    last_key_ts = ts;
                    has_key = true;
                }
            }

            // 码率与 GOP 深度分析（仅第一条视频流）
            if (options.analyze_bitrate_gop && pkt->stream_index == video_stream_index) {
                const double sample_ts = has_pts
                                             ? ts
                                             : ((pkt->dts != AV_NOPTS_VALUE)
                                                    ? static_cast<double>(pkt->dts) * tb
                                                    : 0.0);
                model::FrameType frame_type = model::FrameType::Unknown;
                bool is_idr = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                bool fed = false;

                if (probe.use_decoder) {
                    if (avcodec_send_packet(probe.decoder, pkt) == 0) {
                        while (avcodec_receive_frame(probe.decoder, probe.frame) >= 0) {
                            frame_type = static_cast<model::FrameType>(probe.frame->pict_type);
                            // 注: FFmpeg 8.x 已移除 AVFrame::key_frame，统一用 AV_FRAME_FLAG_KEY
                            is_idr = ((probe.frame->flags & AV_FRAME_FLAG_KEY) != 0);
                            const double frame_ts =
                                (probe.frame->pts != AV_NOPTS_VALUE)
                                    ? static_cast<double>(probe.frame->pts) * tb
                                    : sample_ts;
                            bitrate_gop.OnFrame(pkt->stream_index, frame_ts, pkt->size,
                                                frame_type, is_idr);
                            fed = true;
                            av_frame_unref(probe.frame);
                        }
                    }
                } else if (probe.parser != nullptr) {
                    uint8_t* out_data = nullptr;
                    int out_size = 0;
                    av_parser_parse2(probe.parser, probe.parser_ctx, &out_data, &out_size,
                                     pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
                    if (probe.parser->pict_type != AV_PICTURE_TYPE_NONE) {
                        frame_type = static_cast<model::FrameType>(probe.parser->pict_type);
                        if (probe.parser->key_frame >= 1) {
                            is_idr = true;
                        } else if (probe.parser->key_frame == 0) {
                            is_idr = false;
                        }
                        fed = true;
                    }
                }

                if (fed) {
                    bitrate_gop.OnFrame(pkt->stream_index, sample_ts, pkt->size, frame_type,
                                        is_idr);
                } else {
                    bitrate_gop.OnPacket(pkt->stream_index, sample_ts, pkt->size, is_idr);
                }
            }

            // 逐秒桶：码率 + 帧率
            if (ts >= 0.0) {
                const int64_t bucket_index = static_cast<int64_t>(std::floor(ts / interval));
                Bucket& bucket = buckets[bucket_index];
                bucket.video_bytes += pkt->size;
                bucket.video_frames += 1;
            }
        }

        if (ts >= 0.0) {
            const int64_t bucket_index = static_cast<int64_t>(std::floor(ts / interval));
            buckets[bucket_index].total_bytes += pkt->size;
        }

        last_pos = (pkt->pos > 0) ? pkt->pos : last_pos;
        av_packet_unref(pkt);

        // 进度上报（限频）
        ++packet_index;
        if (options.max_packets > 0 && packet_index >= options.max_packets) break;
        if (packet_index % kProgressMinPackets == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress).count() >=
                kProgressMinIntervalMs) {
                last_progress = now;
                double percent = 0.0;
                if (result.file_size_bytes > 0 && last_pos > 0) {
                    percent = 100.0 * static_cast<double>(last_pos) /
                              static_cast<double>(result.file_size_bytes);
                } else if (result.duration_seconds > 0.0 && ts > 0.0) {
                    percent = 100.0 * ts / result.duration_seconds;
                }
                percent = std::clamp(percent, 0.0, 99.0);
                emit ProgressReported(generation, percent,
                                      QStringLiteral("扫描数据包 %1 个").arg(packet_index));
            }
        }
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    probe.Release();

    // ---- 汇总 ----
    if (result.total_packets > 0) {
        result.avg_packet_bytes = static_cast<double>(result.total_bytes) /
                                  static_cast<double>(result.total_packets);
    }
    const double measured_duration = (result.duration_seconds > 0.0)
                                         ? result.duration_seconds
                                         : (buckets.empty() ? 0.0
                                                            : (static_cast<double>(buckets.rbegin()->first) + 1.0) * interval);
    if (result.overall_bitrate_bps <= 0 && measured_duration > 0.0) {
        result.overall_bitrate_bps = static_cast<int64_t>(result.total_bytes * 8 / measured_duration);
    }
    for (auto& digest : result.streams) {
        if (digest.bitrate_bps <= 0 && digest.duration_seconds > 0.0) {
            digest.bitrate_bps = static_cast<int64_t>(digest.byte_count * 8 / digest.duration_seconds);
        }
        if (digest.duration_seconds <= 0.0) digest.duration_seconds = measured_duration;
    }

    const double kbps_per_byte_per_sec = 8.0 / 1000.0 / interval;
    result.total_bitrate_kbps.name = "total_bitrate";
    result.total_bitrate_kbps.unit = "kbps";
    result.video_bitrate_kbps.name = "video_bitrate";
    result.video_bitrate_kbps.unit = "kbps";
    result.video_fps.name = "video_fps";
    result.video_fps.unit = "fps";
    result.total_bitrate_kbps.Reserve(buckets.size());
    result.video_bitrate_kbps.Reserve(buckets.size());
    result.video_fps.Reserve(buckets.size());
    for (const auto& [index, bucket] : buckets) {
        const double t = static_cast<double>(index) * interval;
        result.total_bitrate_kbps.Add(t, static_cast<double>(bucket.total_bytes) * kbps_per_byte_per_sec);
        result.video_bitrate_kbps.Add(t, static_cast<double>(bucket.video_bytes) * kbps_per_byte_per_sec);
        result.video_fps.Add(t, static_cast<double>(bucket.video_frames) / interval);
    }

    // ---- 时间轴与同步汇总 ----
    timeline_analyzer.Finish();
    result.timeline = timeline_analyzer.result();

    if (result.max_gop_frames == 0 && !result.gop_frame_sizes.empty()) {
        result.max_gop_frames = *std::max_element(result.gop_frame_sizes.begin(),
                                                  result.gop_frame_sizes.end());
    }

    // 码率与 GOP 深度分析收尾（必须在 probe.Release() 之后、发信号之前）
    if (options.analyze_bitrate_gop && video_stream_index >= 0) {
        result.bitrate_gop = bitrate_gop.Finish();
        LOG_INFO("码率与 GOP 分析: frames=" + std::to_string(result.bitrate_gop.total_frames) +
                 " gops=" + std::to_string(result.bitrate_gop.gops.size()) +
                 " anomalies=" + std::to_string(result.bitrate_gop.anomalies.size()));
    }

    LOG_INFO("全文件分析完成: packets=" + std::to_string(result.total_packets) +
             " duration=" + std::to_string(result.duration_seconds) +
             " completed=" + std::to_string(result.completed ? 1 : 0));

    running_.store(false, std::memory_order_release);
    emit ProgressReported(generation, 100.0, QStringLiteral("分析完成"));
    emit AnalysisFinished(generation, result.completed, result);
}

} // namespace analyzer
} // namespace videoeye
