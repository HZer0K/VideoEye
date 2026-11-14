#include "core/repair/RepairTranscodeTracks.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace videoeye {
namespace repair {
namespace {

// 内存写头：只为拿「容器接不接受这条轨」的实测结论，落盘内容全部丢弃。
//
// 为什么用内存而不是临时文件：探测发生在每次转码的失败路径上，落盘意味着
// 一个新路径、一次清理、一次杀软占用风险；内存 IO 没有任何这些副作用。
// 实测结论与真实落盘写头一致（mp4/truehd、mov/flac、mov/opus 均拒绝）。
struct MemSink {
    int64_t pos = 0;
    int64_t written = 0;
};

int MemWrite(void* opaque, const std::uint8_t* /*buffer*/, int size) {
    auto* sink = static_cast<MemSink*>(opaque);
    if (size <= 0)
        return 0;
    sink->pos += size;
    if (sink->pos > sink->written)
        sink->written = sink->pos;
    return size;
}

// 封装器在写头阶段会 seek（回填 box 长度）。没有 seek 回调会被读成「写失败」，
// 那就把「容器拒绝这个编码」和「我们没有可 seek 的输出」混成一件事了。
int64_t MemSeek(void* opaque, int64_t offset, int whence) {
    auto* sink = static_cast<MemSink*>(opaque);
    if (whence == AVSEEK_SIZE)
        return sink->written;
    switch (whence) {
    case SEEK_SET:
        sink->pos = offset;
        break;
    case SEEK_CUR:
        sink->pos += offset;
        break;
    default:
        return AVERROR(ENOSYS);
    }
    return sink->pos;
}

std::string AvErrorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

model::RepairTranscodeTrackDecision MakeDecision(const TranscodeTrackProbe& probe, const char* action,
                                                 const char* reason_code, const std::string& reason) {
    model::RepairTranscodeTrackDecision decision;
    decision.input_stream_index = probe.input_stream_index;
    decision.kind = probe.kind;
    decision.source_codec = probe.source_codec;
    decision.action = action;
    decision.reason_code = reason_code;
    decision.reason = reason;
    return decision;
}

} // namespace

const char* TranscodeTrackKindName(AVMediaType type, bool attached_pic) {
    if (attached_pic)
        return "attached_pic";
    switch (type) {
    case AVMEDIA_TYPE_VIDEO:
        return "video";
    case AVMEDIA_TYPE_AUDIO:
        return "audio";
    case AVMEDIA_TYPE_SUBTITLE:
        return "subtitle";
    case AVMEDIA_TYPE_DATA:
        return "data";
    case AVMEDIA_TYPE_ATTACHMENT:
        return "attachment";
    default:
        return "other";
    }
}

int SelectPrimaryVideoIndex(const std::vector<TranscodeTrackProbe>& probes) {
    for (const auto& probe : probes) {
        if (probe.kind == "video")
            return probe.input_stream_index;
    }
    return -1;
}

TranscodeContainerFit ProbeTrackFitsContainer(const char* format_name, const AVCodecParameters* parameters,
                                              std::string* detail) {
    if (detail != nullptr)
        detail->clear();
    if (format_name == nullptr || parameters == nullptr) {
        if (detail != nullptr)
            *detail = "缺少封装器名或轨道参数";
        return TranscodeContainerFit::Unknown;
    }
    AVFormatContext* probe = nullptr;
    if (avformat_alloc_output_context2(&probe, nullptr, format_name, nullptr) < 0 || probe == nullptr) {
        if (detail != nullptr)
            *detail = std::string("无法创建 ") + format_name + " 输出上下文";
        return TranscodeContainerFit::Unknown;
    }

    TranscodeContainerFit fit = TranscodeContainerFit::Unknown;
    AVIOContext* avio = nullptr;
    MemSink sink;
    // avio_alloc_context 接管 buffer 所有权；失败时由这里释放。
    std::uint8_t* buffer = static_cast<std::uint8_t*>(av_malloc(8192));
    AVStream* stream = buffer != nullptr ? avformat_new_stream(probe, nullptr) : nullptr;
    if (stream != nullptr && avcodec_parameters_copy(stream->codecpar, parameters) >= 0) {
        // codec_tag 由封装器自己决定：源容器的 tag 在目标容器里往往非法，
        // 照抄会让 movenc 走进「MS 风格 tag」的分支，得出与真实转码不同的结论。
        stream->codecpar->codec_tag = 0;
        stream->time_base = parameters->sample_rate > 0 ? AVRational{1, parameters->sample_rate} : AVRational{1, 1000};
        avio = avio_alloc_context(buffer, 8192, 1, &sink, nullptr, MemWrite, MemSeek);
        if (avio != nullptr) {
            // 所有权转移给 avio：它可能自己换掉这块缓冲，所以只能通过
            // avio->buffer 释放。这里立刻把本地指针置空，避免下面再 free 一次
            // —— avio_context_free() 会把 avio 置 NULL，用「avio == nullptr」
            // 判所有权会直接变成 double free（实测 0xc0000374 堆损坏）。
            buffer = nullptr;
            probe->pb = avio;
            probe->flags |= AVFMT_FLAG_CUSTOM_IO;
            const int ret = avformat_write_header(probe, nullptr);
            if (ret >= 0) {
                av_write_trailer(probe);
                fit = TranscodeContainerFit::Accepted;
            } else {
                fit = TranscodeContainerFit::Rejected;
                // 封装器的具体理由走 av_log（例如「flac only supported in MP4」），
                // 返回值只有错误码。这里如实写「封装器返回 <错误码含义>」，不假装
                // 拿到了它的原话。
                if (detail != nullptr)
                    *detail = "封装器返回 " + AvErrorText(ret);
            }
            av_freep(&avio->buffer);
            avio_context_free(&avio);
            probe->pb = nullptr;
        } else if (detail != nullptr) {
            *detail = "无法分配内存输出上下文";
        }
    } else if (detail != nullptr) {
        *detail = "无法复制轨道参数到探测上下文";
    }
    if (buffer != nullptr)
        av_free(buffer);
    avformat_free_context(probe);
    return fit;
}

TranscodeTrackPlan PlanTranscodeTracks(const std::vector<TranscodeTrackProbe>& probes) {
    TranscodeTrackPlan plan;
    plan.primary_video_index = SelectPrimaryVideoIndex(probes);
    plan.decisions.reserve(probes.size());

    for (const auto& probe : probes) {
        if (probe.discarded) {
            plan.decisions.push_back(MakeDecision(probe, "drop", "stream_marked_discard",
                                                  "该轨在源侧已被标记为丢弃（discard=all），不会被读出，产物不含该轨"));
            continue;
        }
        if (probe.kind == "video" && probe.input_stream_index == plan.primary_video_index) {
            // 主视频轨：重编码的理由必须写清楚是「这条轨需要」还是「模式选择」。
            // 没有解码层损伤证据时不许暗示这是修复所必需 —— 旧文案「视频轨经过
            // 解码与重编码」读起来像在说该轨坏了。
            if (probe.decode_damage_detected) {
                plan.decisions.push_back(
                    MakeDecision(probe, "transcode", "video_reencode_decode_damage",
                                 "主视频轨：诊断层在该轨检测到编码层损伤，解码可读帧后重编码为 H.264"));
            } else {
                plan.decisions.push_back(
                    MakeDecision(probe, "transcode", "video_reencode_mode_choice",
                                 "主视频轨：该轨未检出编码层损伤证据；本次重编码来自救内容模式的选择，"
                                 "不是该轨必需的修复（可感知画面会有代际损失）"));
            }
            continue;
        }
        if (probe.kind == "attached_pic") {
            plan.decisions.push_back(MakeDecision(probe, "drop", "cover_art_not_rebuilt",
                                                  "封面图片（attached_pic）不是视频内容，S08 不重建该轨"));
            continue;
        }
        if (probe.kind == "video") {
            plan.decisions.push_back(MakeDecision(
                probe, "drop", "extra_video_single_chain",
                "第二视频轨：S08 只有一条解码-重编码链，产物未保留该轨（这是本模块的能力边界，不是容器限制）"));
            continue;
        }
        if (probe.kind == "audio") {
            const std::string detail = probe.container_fit_detail.empty()
                                           ? std::string()
                                           : std::string("（") + probe.container_fit_detail + "）";
            switch (probe.container_fit) {
            case TranscodeContainerFit::Accepted:
                plan.decisions.push_back(MakeDecision(probe, "copy", "audio_copy_container_ok",
                                                      "音频轨：目标容器试写头接受该编码，按原码流复制（不重编码）"));
                break;
            case TranscodeContainerFit::Rejected:
                // 切片 V：容器拒绝原编码不等于只能丢轨。能实测出一条「编码器存在 +
                // 参数能开 + 容器接受」的路径就转码，否则才 drop，并把每条候选
                // 被拒的原因一并报出 —— 「没有可用编码器」不能是空口断言。
                if (!probe.audio_reencode_allowed) {
                    plan.decisions.push_back(
                        MakeDecision(probe, "drop", "audio_drop_reencode_disabled",
                                     "音频轨：目标容器试写头拒绝该编码，且本次未允许重编码，产物未保留该轨" + detail));
                } else if (!probe.audio_reencode_encoder.empty()) {
                    plan.decisions.push_back(MakeDecision(probe, "transcode", "audio_reencode_container_rejected",
                                                          "音频轨：目标容器试写头拒绝原编码" + detail +
                                                              "，解码后重编码为 " + probe.audio_reencode_codec +
                                                              "（编码器 " + probe.audio_reencode_encoder +
                                                              "，目标参数 " + probe.audio_reencode_settings + "）"));
                    plan.decisions.back().target_codec = probe.audio_reencode_codec;
                } else {
                    // 逐候选的尝试记录必须跟着结论走：空着也要说清「试过什么」——
                    // 只写「没有可用编码器」等于把结论当证据。
                    const std::string attempts = probe.audio_reencode_note.empty() ? std::string("未逐条记录候选尝试")
                                                                                   : probe.audio_reencode_note;
                    plan.decisions.push_back(MakeDecision(probe, "drop", "audio_drop_no_fitting_encoder",
                                                          "音频轨：目标容器试写头拒绝该编码" + detail +
                                                              "，且没有任何可用的重编码目标（" + attempts +
                                                              "），产物未保留该轨"));
                }
                break;
            case TranscodeContainerFit::Unknown:
                plan.decisions.push_back(MakeDecision(
                    probe, "copy", "audio_copy_container_unprobed",
                    "音频轨：未能实测容器是否接受该编码，按原码流复制；若写头失败会在报告里给出失败原因" + detail));
                break;
            }
            continue;
        }
        if (probe.kind == "subtitle") {
            plan.decisions.push_back(MakeDecision(probe, "drop", "subtitle_not_carried",
                                                  "字幕轨：S08 输出链只重建视频与音频，产物未保留该轨"));
            continue;
        }
        if (probe.kind == "data") {
            plan.decisions.push_back(
                MakeDecision(probe, "drop", "data_not_carried", "数据轨：S08 输出链只重建视频与音频，产物未保留该轨"));
            continue;
        }
        if (probe.kind == "attachment") {
            plan.decisions.push_back(
                MakeDecision(probe, "drop", "attachment_not_carried",
                             "附件轨（字体 / 图片等）：S08 输出链只重建视频与音频，产物未保留该轨"));
            continue;
        }
        plan.decisions.push_back(MakeDecision(probe, "drop", "unclassified_not_carried",
                                              "未归类的轨道类型：S08 输出链无法承载，产物未保留该轨"));
    }

    for (const auto& decision : plan.decisions) {
        if (decision.action == "copy")
            ++plan.copy_count;
        else if (decision.action == "transcode")
            ++plan.transcode_count;
        else
            ++plan.drop_count;
    }
    // 唯一渲染在这里做一次：报告层拿 text 打印，不许自己拼。
    for (auto& decision : plan.decisions)
        decision.text = TranscodeTrackDecisionText(decision);

    // 覆盖自证：条数对得上还不够，下标必须唯一且与探针一致 —— 复制粘贴写错
    // 下标时条数照样相等，但会有一条轨被报两次、另一条彻底消失。
    std::set<int> seen;
    bool indices_match = true;
    for (const auto& decision : plan.decisions) {
        if (!seen.insert(decision.input_stream_index).second)
            indices_match = false;
    }
    for (const auto& probe : probes) {
        if (seen.count(probe.input_stream_index) == 0)
            indices_match = false;
    }
    plan.complete = indices_match && plan.decisions.size() == probes.size();

    plan.summary = "复制 " + std::to_string(plan.copy_count) + " 轨 / 重编码 " + std::to_string(plan.transcode_count) +
                   " 轨 / 未保留 " + std::to_string(plan.drop_count) + " 轨";
    if (!plan.complete) {
        plan.summary += "（决策表与源侧流数不一致：" + std::to_string(plan.decisions.size()) + " 条决策 / " +
                        std::to_string(probes.size()) + " 条输入轨，未列出的轨不能当成已保留）";
    }
    plan.note = "容器接受性由内存试写头逐轨实测（只放该音频轨一条流），不是查表或按扩展名推断；"
                "实测拒绝的音频轨优先转码成实测能收的编码（转码目标同样要过试写头），转不了才列为未保留 —— "
                "两种结果都在决策表里逐条写明，且都不会让整份产物写头失败";
    return plan;
}

std::string TranscodeTrackDecisionText(const model::RepairTranscodeTrackDecision& decision) {
    std::string text = "流 #" + std::to_string(decision.input_stream_index) + "（" + decision.kind;
    if (!decision.source_codec.empty())
        text += ", " + decision.source_codec;
    text += "）→ ";
    if (decision.action == "copy")
        text += "复制";
    else if (decision.action == "transcode")
        text += "重编码";
    else if (decision.action == "drop")
        text += "未保留";
    else
        text += decision.action.empty() ? "未判定" : decision.action;
    if (!decision.reason.empty())
        text += "：" + decision.reason;
    return text;
}

std::string TranscodeTrackOmittedText(const model::RepairTranscodeTrackDecision& decision) {
    std::string text = "流 #" + std::to_string(decision.input_stream_index) + "（" + decision.kind;
    if (!decision.source_codec.empty())
        text += ", " + decision.source_codec;
    text += "）";
    if (!decision.reason.empty())
        text += "：" + decision.reason;
    return text;
}

} // namespace repair
} // namespace videoeye
