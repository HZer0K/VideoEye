#include "core/repair/RepairExecutor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairUtils.h"
#include "infrastructure/concurrency/Cancellation.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace videoeye {
namespace repair {
namespace {

using Clock = std::chrono::steady_clock;

std::string AvErrorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

void Emit(const RepairProgress& progress, int percent, const std::string& detail) {
    if (progress)
        progress(percent, stage::Executing(), detail);
}

bool IsMediaStream(const AVStream* stream) {
    const AVMediaType type = stream->codecpar->codec_type;
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
        return false;
    return stream->codecpar->codec_id != AV_CODEC_ID_NONE && stream->time_base.num > 0 && stream->time_base.den > 0;
}

std::optional<double> PacketSeconds(const AVPacket* packet, const AVStream* stream) {
    int64_t ts = packet->pts;
    if (ts == AV_NOPTS_VALUE)
        ts = packet->dts;
    if (ts == AV_NOPTS_VALUE)
        return std::nullopt;
    return static_cast<double>(ts) * stream->time_base.num / stream->time_base.den;
}

int32_t DurationMilliseconds(const AVFormatContext* fmt) {
    if (!fmt || fmt->duration <= 0)
        return 0;
    return static_cast<int32_t>((fmt->duration + 999) / 1000);
}

// 拼接用的临时源文件，执行结束（含任何提前 return）时删除。
class TempFileGuard {
public:
    explicit TempFileGuard(std::string path) : path_(std::move(path)) {
    }
    ~TempFileGuard() {
        if (path_.empty())
            return;
        std::error_code ec;
        std::filesystem::remove(ToNativePath(path_), ec);
    }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

private:
    std::string path_;
};

bool CopyFileInto(std::ofstream& out, const std::string& source_path, const std::atomic<bool>* cancel,
                  const RepairRunBudget* budget, bool* canceled, bool* timed_out) {
    std::ifstream in(ToNativePath(source_path), std::ios::binary);
    if (!in)
        return false;
    std::vector<char> buffer(4u * 1024u * 1024u);
    while (in) {
        if (budget != nullptr) {
            if (budget->Checkpoint()) {
                if (budget->Canceled())
                    *canceled = true;
                else
                    *timed_out = true;
                return false;
            }
        } else if (infrastructure::Checkpoint(cancel)) {
            *canceled = true;
            return false;
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0)
            break;
        out.write(buffer.data(), got);
        if (!out)
            return false;
    }
    return true;
}

// 生成"参考初始化段 + 源媒体分片"的临时文件，供 FFmpeg 按普通 fMP4 读取。
bool AssembleWithInitSegment(const std::string& source_path, const std::vector<uint8_t>& init_bytes,
                             const std::string& target_path, const std::atomic<bool>* cancel,
                             const RepairRunBudget* budget, bool* canceled, bool* timed_out, std::string* error) {
    // target_path 必须已由调用方用 CreateExclusiveFile 独占预留；这里只负责写，
    // 不能在未确认所有权的情况下 trunc/删除可能属于用户的文件（R01）。
    std::ofstream out(ToNativePath(target_path), std::ios::binary | std::ios::trunc);
    if (!out) {
        *error = "无法创建临时组装文件: " + target_path;
        return false;
    }
    if (!init_bytes.empty()) {
        out.write(reinterpret_cast<const char*>(init_bytes.data()), static_cast<std::streamsize>(init_bytes.size()));
        if (!out) {
            *error = "写入参考初始化段失败";
            return false;
        }
    }
    if (!CopyFileInto(out, source_path, cancel, budget, canceled, timed_out)) {
        if (!*canceled && !*timed_out)
            *error = "复制源媒体分片失败: " + source_path;
        return false;
    }
    out.flush();
    if (!out) {
        *error = "写入临时组装文件失败: " + target_path;
        return false;
    }
    return true;
}

// 逐流时间轴状态：只用于"有证据的重写"（重复 / 回退），不用于伪造缺失时间戳。
struct StreamTimingState {
    bool has_last_dts = false;
    int64_t last_dts = 0;
    int64_t last_pts = 0;
    int64_t nominal_duration = 0; // 源时间基下的单帧时长（用于重写步进）
};

int64_t NominalFrameDuration(const AVStream* stream) {
    const AVRational fps = stream->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0 || stream->time_base.num <= 0)
        return 0;
    const int64_t ticks = av_rescale_q(1, AVRational{fps.den, fps.num}, stream->time_base);
    return ticks > 0 ? ticks : 0;
}

bool CheckpointOrStop(const std::atomic<bool>* cancel, const RepairRunBudget* budget, bool* canceled, bool* timed_out) {
    if (budget != nullptr) {
        if (!budget->Checkpoint())
            return false;
        *canceled = budget->Canceled();
        *timed_out = !*canceled;
        return true;
    }
    if (infrastructure::Checkpoint(cancel)) {
        *canceled = true;
        return true;
    }
    return false;
}

// 跳段恢复：从损坏点之后按递增步长寻找可解码恢复点。
//
// 关键约束：
//  - 只接受时间戳明确大于损坏点之后的新包，避免 seek 原地打转；
//  - 有输出视频轨时优先要关键帧，宁可跳过恢复点前的少量音频，也不从半截 GOP 开始；
//  - 恢复失败由调用方记为"读取失败点之后未参与修复"，不能假装成功。
bool TryResync(AVFormatContext* in, double from_seconds, double duration_seconds, int first_video_input_index,
               const std::vector<int>& stream_map, const std::atomic<bool>* cancel, const RepairRunBudget* budget,
               AVPacket* packet, double* resync_seconds, std::string* detail, bool* canceled, bool* timed_out) {
    static const double kStepsSeconds[] = {0.2, 0.4, 0.8, 1.6, 3.2, 6.4, 12.8, 25.6, 51.2};
    const bool require_keyframe = first_video_input_index >= 0;
    const double end_limit = duration_seconds > 0.0 ? duration_seconds - 0.05 : std::numeric_limits<double>::infinity();

    for (double step : kStepsSeconds) {
        const double target = from_seconds + step;
        if (target >= end_limit)
            break;
        if (CheckpointOrStop(cancel, budget, canceled, timed_out))
            return false;

        const int64_t target_us = static_cast<int64_t>(target * AV_TIME_BASE);
        const int seek_ret = av_seek_frame(in, -1, target_us, AVSEEK_FLAG_BACKWARD);
        if (seek_ret < 0) {
            avformat_flush(in);
            continue;
        }
        avformat_flush(in);

        // 单次恢复点探测窗口有界：避免在损坏文件上无限读包。
        const int64_t probe_deadline_us = av_gettime() + 2 * 1000 * 1000;
        for (int probe = 0; probe < 128; ++probe) {
            if (av_gettime() > probe_deadline_us)
                break;
            if (CheckpointOrStop(cancel, budget, canceled, timed_out))
                return false;

            const int read_ret = av_read_frame(in, packet);
            if (read_ret < 0)
                break;

            const int in_index = packet->stream_index;
            if (in_index < 0 || in_index >= static_cast<int>(stream_map.size()) ||
                stream_map[static_cast<std::size_t>(in_index)] < 0) {
                av_packet_unref(packet);
                continue;
            }

            const AVStream* stream = in->streams[in_index];
            const std::optional<double> seconds = PacketSeconds(packet, stream);
            if (!seconds.has_value() || *seconds < from_seconds + 0.001) {
                av_packet_unref(packet);
                continue;
            }
            const bool is_video = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
            // 有视频轨时必须从关键帧恢复：音频包先跳过，避免从半截 GOP 开始解码。
            if (require_keyframe && (!is_video || (packet->flags & AV_PKT_FLAG_KEY) == 0)) {
                av_packet_unref(packet);
                continue;
            }

            *resync_seconds = *seconds;
            *detail = "av_seek_frame(" + FormatSeconds(target) + " s) 恢复到 " + FormatSeconds(*seconds) + " s";
            return true;
        }
        avformat_flush(in);
    }
    return false;
}

} // namespace

RepairExecutionOutcome RepairExecutor::Execute(const model::RepairRequest& request,
                                               const model::RepairDiagnosis& diagnosis, const model::RepairPlan& plan,
                                               const RepairOutputPaths& paths, const RepairInitSegment* reference_init,
                                               const std::atomic<bool>* cancel, const RepairProgress& progress,
                                               const RepairRunBudget* budget) {
    (void)diagnosis;
    const std::string& part_path = paths.part_path;
    RepairExecutionOutcome outcome;
    outcome.part_path = part_path;
    outcome.assembled_path = paths.assembled_path;
    if (budget != nullptr && budget->Checkpoint()) {
        outcome.canceled = budget->Canceled();
        outcome.timed_out = !outcome.canceled;
        return outcome;
    }
    outcome.ffmpeg_version = av_version_info();

    // 缺少初始化段的 fMP4 媒体分片：先拼出 init + 分片的临时文件，之后整条
    // 重封装链路与普通 MP4 完全一致。
    const bool use_reference_init = reference_init != nullptr && reference_init->ok && !reference_init->bytes.empty();
    std::string input_path = request.source_path;
    std::unique_ptr<TempFileGuard> assembled_guard;
    if (use_reference_init) {
        Emit(progress, 76, "前置参考初始化段并组装源媒体分片");
        const std::string assembled_path = paths.assembled_path;
        if (!CreateExclusiveFile(assembled_path)) {
            outcome.error = "无法独占创建临时组装文件（已存在或不可创建）: " + assembled_path;
            return outcome;
        }
        assembled_guard = std::make_unique<TempFileGuard>(assembled_path);
        bool canceled_copy = false;
        bool timed_out_copy = false;
        std::string assemble_error;
        if (!AssembleWithInitSegment(request.source_path, reference_init->bytes, assembled_path, cancel, budget,
                                     &canceled_copy, &timed_out_copy, &assemble_error)) {
            if (canceled_copy) {
                outcome.canceled = true;
                return outcome;
            }
            if (timed_out_copy) {
                outcome.timed_out = true;
                return outcome;
            }
            outcome.error = assemble_error;
            return outcome;
        }
        input_path = assembled_path;
    }

    Emit(progress, 76, "打开源文件");

    AVFormatContext* in = avformat_alloc_context();
    ffmpeg_io::AvInterruptState interrupt;
    interrupt.cancel = cancel ? const_cast<std::atomic<bool>*>(cancel) : nullptr;
    if (in)
        ffmpeg_io::AttachInterrupt(in, interrupt, ffmpeg_io::kOpenTimeoutUs);

    const int open_ret = in ? avformat_open_input(&in, input_path.c_str(), nullptr, nullptr) : AVERROR(ENOMEM);
    if (open_ret < 0) {
        outcome.error = use_reference_init ? "使用参考初始化段组装后仍无法打开源文件: " + AvErrorText(open_ret) +
                                                 "（参考初始化段可能与源分片不匹配）"
                                           : "无法打开源文件: " + AvErrorText(open_ret);
        return outcome;
    }
    int64_t probe_timeout_us = ffmpeg_io::kProbeTimeoutUs;
    if (budget != nullptr && budget->HasDeadline()) {
        const int64_t left = budget->RemainingUs();
        if (left <= 0) {
            outcome.timed_out = true;
            avformat_close_input(&in);
            return outcome;
        }
        probe_timeout_us = std::min<int64_t>(probe_timeout_us, left);
    }
    interrupt.deadline_us.store(av_gettime() + probe_timeout_us);
    if (avformat_find_stream_info(in, nullptr) < 0) {
        // 探测失败时仍允许尝试复制：坏文件经常就是探测阶段失败，但包本身可读。
    }
    interrupt.deadline_us.store(0);

    if (budget != nullptr ? budget->Checkpoint() : infrastructure::Checkpoint(cancel)) {
        outcome.canceled = budget != nullptr ? budget->Canceled() : true;
        outcome.timed_out = budget != nullptr && !outcome.canceled;
        avformat_close_input(&in);
        return outcome;
    }

    // R04 契约：保留轨 = 实际写进产物的全部音视频轨（用户 target_streams 不裁剪输出，
    // 避免静默丢内容）；要求验证轨 = 全部保留轨，target_streams 用来强制标记关注轨。
    // 封面图片（attached_pic）不算视频内容验证对象。
    std::vector<int> stream_map(in->nb_streams, -1);
    std::vector<const AVStream*> mapped_streams;
    int first_video_input_index = -1;
    std::set<int> requested;
    for (int index : request.target_streams)
        requested.insert(index);
    // S09 单轨抢救：只剔除诊断阶段拿到"全部样本越界"证据的轨道。
    const std::set<int> dropped_inputs(plan.dropped_input_streams.begin(), plan.dropped_input_streams.end());
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* stream = in->streams[i];
        if (dropped_inputs.count(static_cast<int>(i)) > 0) {
            const char* type_name = av_get_media_type_string(stream->codecpar->codec_type);
            outcome.omitted_items.push_back("流 #" + std::to_string(i) + " (" + (type_name ? type_name : "unknown") +
                                            ") 经诊断判定无可用样本，单轨抢救已省略该轨");
            // 告诉 demuxer 直接丢弃该流，避免为了跳过它继续读坏样本数据。
            in->streams[i]->discard = AVDISCARD_ALL;
            continue;
        }
        if (!IsMediaStream(stream)) {
            const char* type_name = av_get_media_type_string(stream->codecpar->codec_type);
            outcome.omitted_items.push_back("流 #" + std::to_string(i) + " (" + (type_name ? type_name : "unknown") +
                                            ") 不是音视频轨，产物未保留（字幕 / 数据 / 章节可能丢失）");
            continue;
        }
        const bool attached_pic = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        const bool is_video = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        model::RepairTrackVerificationTarget target;
        target.track_id = stream->id > 0 ? static_cast<uint32_t>(stream->id) : 0;
        target.input_stream_index = static_cast<int>(i);
        target.output_stream_index = static_cast<int>(mapped_streams.size());
        target.kind = attached_pic ? "attached_pic" : (is_video ? "video" : "audio");
        target.required = requested.count(static_cast<int>(i)) > 0 || (request.target_streams.empty() && !attached_pic);
        if (attached_pic)
            target.note = "封面图片不是视频内容轨，不参与可播放性结论";
        outcome.verification_targets.push_back(std::move(target));

        stream_map[i] = static_cast<int>(mapped_streams.size());
        mapped_streams.push_back(stream);
        if (first_video_input_index < 0 && is_video && !attached_pic)
            first_video_input_index = static_cast<int>(i);
    }
    for (int index : requested) {
        if (index < 0 || index >= static_cast<int>(in->nb_streams) || stream_map[static_cast<std::size_t>(index)] < 0)
            outcome.omitted_items.push_back("请求指定的流 #" + std::to_string(index) +
                                            " 不是可保留的音视频轨，无法按要求验证");
    }

    if (mapped_streams.empty()) {
        outcome.error = "源文件没有可复制的视频/音频轨";
        avformat_close_input(&in);
        return outcome;
    }

    const std::string ext = LowerExtension(part_path);
    const char* format_name = ext == "mov" ? "mov" : "mp4";

    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, format_name, part_path.c_str()) < 0 || !out) {
        outcome.error = "无法创建输出封装上下文 (" + std::string(format_name) + ")";
        avformat_close_input(&in);
        return outcome;
    }

    // R11：格式级 metadata 尽量保留；MP4 重封装不保留章节，必须逐项说明。
    if (in->metadata && av_dict_count(in->metadata) > 0) {
        if (av_dict_copy(&out->metadata, in->metadata, 0) < 0)
            outcome.omitted_items.push_back("格式级 metadata 复制失败，产物未保留");
    }
    if (in->nb_chapters > 0)
        outcome.omitted_items.push_back("源容器含 " + std::to_string(in->nb_chapters) + " 个章节，MP4 重封装未保留");
    if (in->nb_streams > mapped_streams.size())
        outcome.omitted_items.push_back("共 " + std::to_string(in->nb_streams - mapped_streams.size()) +
                                        " 条非音视频轨未保留");

    bool opened_io = false;
    bool header_written = false;
    std::string failure;
    double last_resync_seconds = -1.0;
    double last_resync_from = -1.0;
    std::vector<uint64_t> per_output_packets(mapped_streams.size(), 0);
    // 这些统计在 do 块内累加、在 do 块之后的执行记录里使用，必须提升到函数作用域。
    uint64_t resync_count = 0;
    double skipped_total_seconds = 0.0;

    do {
        for (std::size_t i = 0; i < mapped_streams.size(); ++i) {
            AVStream* src = const_cast<AVStream*>(mapped_streams[i]);
            AVStream* dst = avformat_new_stream(out, nullptr);
            if (!dst) {
                failure = "无法创建输出轨道";
                break;
            }
            if (avcodec_parameters_copy(dst->codecpar, src->codecpar) < 0) {
                failure = "无法复制轨道编码参数";
                break;
            }
            dst->codecpar->codec_tag = 0; // 让封装器选择合法的 tag
            // R11：语言标签 / disposition / 轨道级 metadata 一并带上。
            av_dict_copy(&dst->metadata, src->metadata, 0);
            dst->disposition = src->disposition;
            dst->time_base = src->time_base;
            dst->avg_frame_rate = src->avg_frame_rate;
            dst->sample_aspect_ratio = src->sample_aspect_ratio;
            if (src->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                outcome.output_video_stream_index = static_cast<int>(i);
            }
        }
        if (!failure.empty())
            break;

        // 修复产物优先写 moov 在前：即使以后尾部再被截断，索引仍然可读。
        if (out->priv_data) {
            if (av_opt_set(out->priv_data, "movflags", "+faststart", 0) < 0) {
                // 不是所有封装器都支持该选项；失败时退回默认布局，不阻断修复。
            }
        }

        if (avio_open(&out->pb, part_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            failure = "无法创建临时产物: " + part_path;
            break;
        }
        opened_io = true;

        if (avformat_write_header(out, nullptr) < 0) {
            failure = "写入封装头失败（可能是轨道参数不被 MP4 接受）";
            break;
        }
        header_written = true;

        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            failure = "无法分配包缓冲";
            break;
        }

        const auto started = Clock::now();
        const double duration_seconds = in->duration > 0 ? static_cast<double>(in->duration) / AV_TIME_BASE : 0.0;
        const int64_t max_seconds = request.limits.max_seconds;
        const uint64_t max_bytes = request.limits.max_output_bytes;
        const std::optional<double> salvage_end = plan.salvage_end_seconds;
        double last_seconds = 0.0;
        uint64_t packets_seen = 0;
        bool write_failed = false;
        std::string write_error;

        // 时间轴状态：源侧修复 + 输出侧单调保护。
        std::vector<StreamTimingState> stream_timing(in->nb_streams);
        std::vector<StreamTimingState> output_timing(mapped_streams.size());

        // 多段恢复 / 时间轴修复的实测记录。
        std::vector<model::RepairRecoveredSegment> segments;
        model::RepairRecoveredSegment current_segment;
        bool current_segment_active = false;
        bool next_segment_resumed = false;
        std::string next_segment_recovery_point;
        double next_segment_skipped_before = 0.0;
        bool merge_active = false;
        double merge_shift_seconds = 0.0;
        double removed_gap_total = 0.0;
        bool gap_removed = false;
        uint64_t timestamp_regressions = 0;
        uint64_t timestamp_duplicates = 0;
        uint64_t missing_timestamps = 0;
        uint64_t rewritten_timestamps = 0;
        // 跳段恢复已经读到的恢复点包，需要在下一轮直接处理而不是重新读包。
        bool packet_ready = false;

        // 逐输入流已读包数与最后时间戳：用于识别"某个轨提前结束"这种无报错的
        // 中间损坏（例如 MP4 的样本偏移越界时 demuxer 直接对该轨返回 EOF）。
        std::vector<uint64_t> stream_read_packets(in->nb_streams, 0);
        std::vector<std::optional<double>> stream_last_seconds(in->nb_streams);
        // 已经写入产物的时间上界，以及跳段恢复后需要丢弃的重叠包上界。
        std::vector<double> stream_covered_seconds(in->nb_streams, -1.0);
        std::vector<double> stream_skip_until_seconds(in->nb_streams, -1.0);

        // 提前结束判定：声明帧数没读完，且最后时间戳明显短于该轨声明时长。
        // 两个条件同时成立才认定损坏，避免把"时就短"或声明不准的文件误判成损坏。
        auto detect_premature_end = [&](double* from_seconds, std::string* detail) {
            bool premature = false;
            double frontier = std::numeric_limits<double>::max();
            for (unsigned i = 0; i < in->nb_streams; ++i) {
                if (stream_map[i] < 0)
                    continue;
                const AVStream* stream = in->streams[i];
                double declared = duration_seconds;
                if (stream->duration > 0)
                    declared = static_cast<double>(stream->duration) * stream->time_base.num / stream->time_base.den;
                const double last = stream_last_seconds[i].value_or(0.0);
                const bool short_by_time = declared > 0.0 && last + 0.15 < declared;
                const bool short_by_count =
                    stream->nb_frames > 0 && stream_read_packets[i] < static_cast<uint64_t>(stream->nb_frames);
                if (!(short_by_time && short_by_count))
                    continue;
                premature = true;
                frontier = std::min(frontier, last);
                if (detail != nullptr) {
                    if (!detail->empty())
                        *detail += "; ";
                    *detail += "流 #" + std::to_string(i) + " 已读 " + std::to_string(stream_read_packets[i]) + "/" +
                               std::to_string(stream->nb_frames) + " 帧，止于 " + FormatSeconds(last) + " s" +
                               "（声明时长 " + FormatSeconds(declared) + " s）";
                }
            }
            if (premature && from_seconds != nullptr)
                *from_seconds = frontier;
            return premature;
        };

        for (;;) {
            if (budget != nullptr) {
                if (budget->Checkpoint()) {
                    outcome.canceled = budget->Canceled();
                    outcome.timed_out = !outcome.canceled;
                    break;
                }
            } else {
                if (infrastructure::Checkpoint(cancel)) {
                    outcome.canceled = true;
                    break;
                }
                if (max_seconds > 0 &&
                    std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count() >= max_seconds) {
                    outcome.timed_out = true;
                    break;
                }
            }

            if (!packet_ready) {
                const int read_ret = av_read_frame(in, packet);
                // demuxer 对越界样本不一定报错：常见表现是只让该轨提前 EOF，
                // 其余轨继续读完后再整体返回 EOF。因此 EOF 也要做"提前结束"判定。
                bool premature_eof = false;
                std::string premature_detail;
                double resync_from_seconds = last_seconds;
                if (read_ret == AVERROR_EOF && packets_seen > 0)
                    premature_eof = detect_premature_end(&resync_from_seconds, &premature_detail);
                if (read_ret == AVERROR_EOF && !premature_eof)
                    break;
                if (read_ret < 0) {
                    if (packets_seen == 0) {
                        failure = "读取首个包失败: " + AvErrorText(read_ret);
                        write_failed = true;
                        break;
                    }

                    if (!premature_eof)
                        outcome.read_error = true;

                    // S04：损坏点之后继续找恢复点。尾部截断（salvage_end 已确认）不再尝试，
                    // 因为越界边界之后没有可读内容。
                    double resync_seconds = 0.0;
                    std::string resync_detail;
                    bool canceled_resync = false;
                    bool timed_out_resync = false;
                    const double resync_from = premature_eof ? resync_from_seconds : last_seconds;
                    const bool resumed =
                        request.allow_segment_salvage && !salvage_end.has_value() &&
                        TryResync(in, resync_from, duration_seconds, first_video_input_index, stream_map, cancel,
                                  budget, packet, &resync_seconds, &resync_detail, &canceled_resync, &timed_out_resync);
                    if (canceled_resync) {
                        outcome.canceled = true;
                        break;
                    }
                    if (timed_out_resync) {
                        outcome.timed_out = true;
                        break;
                    }

                    if (!resumed) {
                        if (premature_eof)
                            outcome.premature_end_unrecovered = true;
                        model::DamageFinding finding;
                        finding.code =
                            premature_eof ? "container.ended_before_declared_end" : "container.read_error_after_data";
                        finding.detector = "repair.executor";
                        finding.layer = model::DamageLayer::Container;
                        finding.severity = model::IssueSeverity::Warning;
                        finding.confidence = model::DamageConfidence::Confirmed;
                        finding.location.precision = model::DamagePrecision::Bounded;
                        finding.location.estimated_seconds = resync_from;
                        if (premature_eof) {
                            finding.location.provenance = "av_read_frame EOF vs stream duration/nb_frames";
                            finding.evidence = "轨道提前结束且未找到后续恢复点: " + premature_detail;
                            finding.limitations.push_back("提前结束点之后的内容未参与修复");
                        } else {
                            finding.location.provenance = "av_read_frame after packets";
                            finding.evidence = "已读取 " + std::to_string(packets_seen) + " 个包后在 " +
                                               FormatSeconds(last_seconds) +
                                               " s 附近读取失败: " + AvErrorText(read_ret);
                            finding.limitations.push_back("读取失败点之后的载荷未参与修复");
                        }
                        finding.candidate_actions.push_back(model::RepairAction::S02_TailSalvage);
                        finding.candidate_actions.push_back(model::RepairAction::S04_SegmentSalvage);
                        outcome.residual_findings.push_back(std::move(finding));
                        break;
                    }

                    // 同一损坏点反复恢复到同一位置 = 没有任何进展（例如某条轨的索引已失效），
                    // 必须终止，否则会在"EOF → seek → 再 EOF"之间无限循环。
                    if (last_resync_seconds >= 0.0 && resync_seconds <= last_resync_seconds + 1e-6 &&
                        resync_from <= last_resync_from + 1e-6) {
                        failure = "跳段恢复未取得进展";
                        break;
                    }
                    last_resync_seconds = resync_seconds;
                    last_resync_from = resync_from;
                    ++resync_count;
                    if (current_segment_active) {
                        segments.push_back(current_segment);
                        current_segment_active = false;
                    }
                    next_segment_resumed = true;
                    next_segment_recovery_point = resync_detail;
                    next_segment_skipped_before = std::max(0.0, resync_seconds - resync_from);
                    skipped_total_seconds += next_segment_skipped_before;

                    // 跳段前已写入的内容继续有效：恢复后按轨丢弃重叠包，避免同一段
                    // 音频/视频被写成两份（尤其是只有单轨提前结束、其它轨已读完的情况）。
                    for (unsigned i = 0; i < in->nb_streams; ++i) {
                        if (stream_covered_seconds[static_cast<std::size_t>(i)] >= 0.0)
                            stream_skip_until_seconds[static_cast<std::size_t>(i)] =
                                stream_covered_seconds[static_cast<std::size_t>(i)];
                    }

                    if (request.gap_policy == model::RepairGapPolicy::MergeContiguous) {
                        const double previous_end = outcome.output_end_seconds.value_or(0.0);
                        const double gap = resync_seconds - previous_end;
                        // 只有确实存在空洞时才平移；重叠（resync 点早于已写内容）时
                        // 时间轴本来就是连续的，平移反而会把音画错位。
                        if (gap > 0.001) {
                            removed_gap_total += gap;
                            gap_removed = true;
                            merge_active = true;
                            merge_shift_seconds = -gap;
                        }
                    }

                    model::DamageFinding skipped;
                    skipped.code = "container.damaged_segment_skipped";
                    skipped.detector = "repair.executor";
                    skipped.layer = model::DamageLayer::Container;
                    skipped.severity = model::IssueSeverity::Info;
                    skipped.confidence = model::DamageConfidence::Confirmed;
                    skipped.location.precision = model::DamagePrecision::Bounded;
                    skipped.location.estimated_seconds = resync_from;
                    skipped.location.provenance = "av_seek_frame resync";
                    skipped.evidence = "在 " + FormatSeconds(resync_from) + " s 附近无法继续读取" +
                                       (premature_eof ? "（" + premature_detail + "）" : "") + "；已跳段并于 " +
                                       FormatSeconds(resync_seconds) + " s 继续恢复（" + resync_detail + "）";
                    skipped.limitations.push_back("被跳过区间的载荷未参与修复，内容无法找回");
                    skipped.candidate_actions.push_back(model::RepairAction::S04_SegmentSalvage);
                    outcome.residual_findings.push_back(std::move(skipped));

                    packet_ready = true;
                }
            }

            packet_ready = false;

            const int in_index = packet->stream_index;
            int out_index = -1;
            if (in_index >= 0 && in_index < static_cast<int>(stream_map.size()))
                out_index = stream_map[in_index];
            if (out_index < 0) {
                av_packet_unref(packet);
                continue;
            }

            const AVStream* src_stream = in->streams[in_index];
            AVStream* dst_stream = out->streams[out_index];

            const std::optional<double> input_seconds = PacketSeconds(packet, src_stream);
            ++stream_read_packets[static_cast<std::size_t>(in_index)];
            if (input_seconds.has_value()) {
                stream_last_seconds[static_cast<std::size_t>(in_index)] =
                    std::max(stream_last_seconds[static_cast<std::size_t>(in_index)].value_or(-1.0), *input_seconds);
                last_seconds = std::max(last_seconds, *input_seconds);
            }

            // 跳段恢复后，丢弃已经被前一段覆盖的时间区间，避免重叠内容重复写入。
            if (input_seconds.has_value() && stream_skip_until_seconds[static_cast<std::size_t>(in_index)] >= 0.0 &&
                *input_seconds <= stream_skip_until_seconds[static_cast<std::size_t>(in_index)] + 0.000001) {
                av_packet_unref(packet);
                continue;
            }

            if (salvage_end.has_value() && input_seconds.has_value() && *input_seconds >= *salvage_end + 0.001) {
                // 到达诊断确认的越界边界：停止继续读尾部，保留已经写入的可读前缀。
                outcome.salvage_truncated = true;
                av_packet_unref(packet);
                break;
            }

            if (in_index == first_video_input_index) {
                outcome.input_video_payload_hash =
                    Fnv1a64Update(outcome.input_video_payload_hash.value_or(Fnv1a64Begin()), packet->data,
                                  static_cast<std::size_t>(std::max(packet->size, 0)));
                ++outcome.input_video_packet_count;
            }

            // ---- S03 时间轴修复：只处理重复 / 回退，缺失只统计不伪造 ----
            int64_t dts = packet->dts;
            int64_t pts = packet->pts;
            if (request.repair_timeline) {
                StreamTimingState& timing = stream_timing[static_cast<std::size_t>(in_index)];
                if (dts == AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE)
                    dts = pts; // 有 PTS 无 DTS：补齐 DTS，不改动实际展示时间
                if (dts == AV_NOPTS_VALUE && pts == AV_NOPTS_VALUE) {
                    ++missing_timestamps;
                } else {
                    const int64_t step = timing.nominal_duration > 0 ? timing.nominal_duration : 1;
                    if (timing.has_last_dts && dts <= timing.last_dts) {
                        if (dts == timing.last_dts)
                            ++timestamp_duplicates;
                        else
                            ++timestamp_regressions;
                        // 只把回退点推进到"上一帧 + 一帧时长"，不改变整体速度。
                        const int64_t delta = timing.last_dts + step - dts;
                        dts += delta;
                        if (pts != AV_NOPTS_VALUE)
                            pts += delta;
                        ++rewritten_timestamps;
                    }
                    timing.last_dts = dts;
                    timing.has_last_dts = true;
                    if (pts != AV_NOPTS_VALUE)
                        timing.last_pts = pts;
                }
                if (packet->duration > 0)
                    timing.nominal_duration = packet->duration;
                else if (timing.nominal_duration == 0)
                    timing.nominal_duration = NominalFrameDuration(src_stream);
                packet->dts = dts;
                packet->pts = pts;
            }

            av_packet_rescale_ts(packet, src_stream->time_base, dst_stream->time_base);

            // ---- 缺口策略：连续播放时把后续片段平移到前一段末尾 ----
            if (merge_active && merge_shift_seconds != 0.0) {
                const int64_t shift_ticks =
                    av_rescale_q(static_cast<int64_t>(std::llround(merge_shift_seconds * AV_TIME_BASE)), AV_TIME_BASE_Q,
                                 dst_stream->time_base);
                if (packet->dts != AV_NOPTS_VALUE)
                    packet->dts += shift_ticks;
                if (packet->pts != AV_NOPTS_VALUE)
                    packet->pts += shift_ticks;
            }

            // 输出侧逐流单调保护：seek 恢复与时间戳重写都可能在片段边界留下 1 tick 回退。
            {
                StreamTimingState& out_timing = output_timing[static_cast<std::size_t>(out_index)];
                if (packet->dts == AV_NOPTS_VALUE && packet->pts != AV_NOPTS_VALUE)
                    packet->dts = packet->pts;
                if (packet->dts != AV_NOPTS_VALUE) {
                    if (out_timing.has_last_dts && packet->dts <= out_timing.last_dts) {
                        const int64_t delta = out_timing.last_dts + 1 - packet->dts;
                        packet->dts += delta;
                        if (packet->pts != AV_NOPTS_VALUE)
                            packet->pts += delta;
                    }
                    out_timing.last_dts = packet->dts;
                    out_timing.has_last_dts = true;
                }
            }

            packet->stream_index = out_index;
            packet->pos = -1;

            // av_interleaved_write_frame 会接管并 unref packet，所以先把需要统计的
            // 字段取出来 —— 写完再读 packet->size 拿到的是 0。
            const int written_size = packet->size;
            const int write_ret = av_interleaved_write_frame(out, packet);
            if (write_ret < 0) {
                write_failed = true;
                write_error = AvErrorText(write_ret);
                av_packet_unref(packet);
                break;
            }

            ++packets_seen;
            ++outcome.packets_written;
            if (out_index >= 0 && out_index < static_cast<int>(per_output_packets.size()))
                ++per_output_packets[static_cast<std::size_t>(out_index)];
            outcome.bytes_written += static_cast<uint64_t>(std::max(written_size, 0));

            if (input_seconds.has_value()) {
                stream_covered_seconds[static_cast<std::size_t>(in_index)] =
                    std::max(stream_covered_seconds[static_cast<std::size_t>(in_index)], *input_seconds);
                double output_seconds = *input_seconds;
                if (merge_active && merge_shift_seconds != 0.0)
                    output_seconds += merge_shift_seconds;

                if (!current_segment_active) {
                    current_segment = model::RepairRecoveredSegment{};
                    current_segment.index = static_cast<uint32_t>(segments.size());
                    current_segment.input_start_seconds = *input_seconds;
                    current_segment.output_start_seconds = output_seconds;
                    current_segment.resumed_after_damage = next_segment_resumed;
                    current_segment.recovery_point = next_segment_recovery_point;
                    current_segment.skipped_before_seconds = next_segment_skipped_before;
                    current_segment_active = true;
                    next_segment_resumed = false;
                    next_segment_recovery_point.clear();
                    next_segment_skipped_before = 0.0;
                }
                current_segment.input_end_seconds = std::max(current_segment.input_end_seconds, *input_seconds);
                current_segment.output_end_seconds = std::max(current_segment.output_end_seconds, output_seconds);
                ++current_segment.packets_written;

                if (!outcome.output_start_seconds.has_value())
                    outcome.output_start_seconds = output_seconds;
                outcome.output_end_seconds =
                    std::max(outcome.output_end_seconds.value_or(output_seconds), output_seconds);
            }

            if (max_bytes > 0 && outcome.bytes_written >= max_bytes) {
                outcome.timed_out = true; // 预算意义上的停止
                av_packet_unref(packet);
                break;
            }

            if (packets_seen % 200 == 0) {
                int percent = 78;
                if (duration_seconds > 0)
                    percent = 76 + static_cast<int>(std::min(12.0, 12.0 * last_seconds / duration_seconds));
                Emit(progress, percent, "已写入 " + std::to_string(packets_seen) + " 个包");
            }

            av_packet_unref(packet);
        }

        if (current_segment_active)
            segments.push_back(current_segment);

        av_packet_free(&packet);
        outcome.packets_read = packets_seen;
        outcome.recovered_segments = segments;
        outcome.timeline_repair.attempted = request.repair_timeline;
        outcome.timeline_repair.gap_removed = gap_removed;
        outcome.timeline_repair.removed_gap_seconds = removed_gap_total;
        outcome.timeline_repair.timestamp_regressions = timestamp_regressions;
        outcome.timeline_repair.timestamp_duplicates = timestamp_duplicates;
        outcome.timeline_repair.missing_timestamps = missing_timestamps;
        outcome.timeline_repair.rewritten_timestamps = rewritten_timestamps;
        if (rewritten_timestamps > 0 && gap_removed)
            outcome.timeline_repair.status = "rebuilt";
        else if (rewritten_timestamps > 0 || gap_removed)
            outcome.timeline_repair.status = "adjusted";
        else
            outcome.timeline_repair.status = "unchanged";
        if (gap_removed) {
            outcome.timeline_repair.note = "删除 " + FormatSeconds(removed_gap_total) +
                                           " s 损坏区间，后续片段已平移到前一段末尾；片段内部未改变播放速度";
        } else if (rewritten_timestamps > 0) {
            outcome.timeline_repair.note = "重写 " + std::to_string(rewritten_timestamps) +
                                           " 个重复/回退时间戳，逐流恢复单调；未改变片段内部播放速度";
        } else if (missing_timestamps > 0) {
            outcome.timeline_repair.note =
                "检测到 " + std::to_string(missing_timestamps) + " 个缺失时间戳，缺少可靠依据，未伪造";
        }

        if (write_failed && failure.empty()) {
            failure = "写入数据包失败: " + write_error;
            // 拼接路径最常见的 EINVAL 不是磁盘问题，而是参考段的编码参数/时间基
            // 与源分片不一致，导致 demuxer 给出的时间戳非单调或无法写入。
            if (use_reference_init)
                failure += "；已前置参考初始化段，请确认它与源分片同源、编码参数与时间基一致";
        }

        if (av_write_trailer(out) < 0 && failure.empty()) {
            failure = "写入封装尾失败（产物可能不完整）";
        }
    } while (false);

    if (opened_io)
        avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);

    if (!failure.empty()) {
        outcome.error = failure;
        return outcome;
    }
    if (outcome.canceled || outcome.timed_out) {
        // 取消 / 超预算时产物仍可能可读，但本次结果不由执行器宣布成功。
        outcome.ok = outcome.packets_written > 0;
        return outcome;
    }
    if (outcome.packets_written == 0) {
        outcome.error = "没有写入任何数据包（没有可恢复内容）";
        return outcome;
    }

    outcome.ok = true;

    for (const auto& target : outcome.verification_targets) {
        model::RepairTrackConservation record;
        record.track_id = target.track_id;
        record.input_stream_index = target.input_stream_index;
        record.output_stream_index = target.output_stream_index;
        record.kind = target.kind;
        if (target.output_stream_index >= 0 && target.output_stream_index < static_cast<int>(per_output_packets.size()))
            record.packets_written = per_output_packets[static_cast<std::size_t>(target.output_stream_index)];
        record.status = "unknown"; // expected_samples 需引擎用源侧基线逐轨填
        record.relation = "unknown";
        record.note = target.note;
        outcome.track_conservation.push_back(std::move(record));
    }

    model::RepairActionExecution remux;
    remux.action = model::RepairAction::S01_Remux;
    remux.attempted = true;
    remux.succeeded = true;
    remux.packets_read = outcome.packets_read;
    remux.packets_written = outcome.packets_written;
    remux.bytes_written = outcome.bytes_written;
    remux.output_start_seconds = outcome.output_start_seconds;
    remux.output_end_seconds = outcome.output_end_seconds;
    remux.input_video_payload_hash = outcome.input_video_payload_hash;
    remux.input_video_packet_count = outcome.input_video_packet_count;
    remux.detail = "重封装写入 " + std::to_string(outcome.packets_written) + " 个包 / " +
                   std::to_string(outcome.bytes_written) + " 字节";
    if (use_reference_init)
        remux.detail = "已前置参考初始化段 " + std::to_string(reference_init->init_size) + " 字节；" + remux.detail;
    if (resync_count > 0)
        remux.detail += "；跳段恢复 " + std::to_string(resync_count) + " 次，产物由 " +
                        std::to_string(outcome.recovered_segments.size()) + " 段组成";
    else if (outcome.read_error)
        remux.detail += "；尾部读取失败后停止";
    outcome.executions.push_back(std::move(remux));

    if (plan.salvage_end_seconds.has_value()) {
        model::RepairActionExecution salvage;
        salvage.action = model::RepairAction::S02_TailSalvage;
        salvage.attempted = outcome.salvage_truncated || outcome.read_error;
        salvage.succeeded = outcome.ok;
        salvage.packets_read = outcome.packets_read;
        salvage.packets_written = outcome.packets_written;
        salvage.bytes_written = outcome.bytes_written;
        salvage.output_start_seconds = outcome.output_start_seconds;
        salvage.output_end_seconds = outcome.output_end_seconds;
        salvage.input_video_payload_hash = outcome.input_video_payload_hash;
        salvage.input_video_packet_count = outcome.input_video_packet_count;
        salvage.detail = "诊断边界 " + FormatSeconds(*plan.salvage_end_seconds) + " s；实际于 " +
                         FormatSeconds(outcome.output_end_seconds.value_or(0.0)) + " s 结束";
        outcome.executions.push_back(std::move(salvage));
    }

    if (request.allow_segment_salvage) {
        model::RepairActionExecution segment;
        segment.action = model::RepairAction::S04_SegmentSalvage;
        segment.attempted = resync_count > 0;
        segment.succeeded = segment.attempted && outcome.ok;
        segment.packets_read = outcome.packets_read;
        segment.packets_written = outcome.packets_written;
        segment.bytes_written = outcome.bytes_written;
        segment.output_start_seconds = outcome.output_start_seconds;
        segment.output_end_seconds = outcome.output_end_seconds;
        segment.detail = "产物由 " + std::to_string(outcome.recovered_segments.size()) + " 段组成；跳段恢复 " +
                         std::to_string(resync_count) + " 次；跳过区间的原始时长 " +
                         FormatSeconds(skipped_total_seconds) +
                         " s；处理策略: " + model::DisplayName(request.gap_policy);
        outcome.executions.push_back(std::move(segment));
    }

    if (request.repair_timeline) {
        model::RepairActionExecution timeline;
        timeline.action = model::RepairAction::S03_TimelineRepair;
        timeline.attempted = true;
        timeline.succeeded = outcome.ok;
        timeline.detail = std::string("状态 ") + outcome.timeline_repair.status + "；重写 " +
                          std::to_string(outcome.timeline_repair.rewritten_timestamps) + " 个时间戳（重复 " +
                          std::to_string(outcome.timeline_repair.timestamp_duplicates) + " / 回退 " +
                          std::to_string(outcome.timeline_repair.timestamp_regressions) + "）；缺失 " +
                          std::to_string(outcome.timeline_repair.missing_timestamps) + " 个（未伪造）";
        if (!outcome.timeline_repair.note.empty())
            timeline.detail += "；" + outcome.timeline_repair.note;
        outcome.executions.push_back(std::move(timeline));
    }

    if (!plan.dropped_input_streams.empty()) {
        model::RepairActionExecution single;
        single.action = model::RepairAction::S09_SingleTrack;
        single.attempted = true;
        single.succeeded = outcome.ok;
        single.detail =
            "已省略 " + std::to_string(plan.dropped_input_streams.size()) + " 条无可用样本的轨道，仅输出仍可恢复的轨道";
        outcome.executions.push_back(std::move(single));
    }

    Emit(progress, 89, "重封装完成");
    return outcome;
}

} // namespace repair
} // namespace videoeye
