#include "core/repair/RepairAudioTranscode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "core/repair/RepairTranscodeTracks.h"

namespace videoeye {
namespace repair {
namespace {

// 候选偏好表。aac 排第一：它是 S08 全部目标容器（mp4 / mov / matroska / mpegts）
// 都接受的编码，而且 FFmpeg 原生 aac 编码器不依赖任何外部库 —— 换到别的构建
// 也几乎一定在。后面几个只是「aac 不可用或容器拒绝 aac」时的退路。
struct CandidateSpec {
    const char* encoder_name;
    const char* codec_name;
};

constexpr CandidateSpec kCandidates[] = {
    {"aac", "aac"}, {"libopus", "opus"}, {"ac3", "ac3"}, {"libmp3lame", "mp3"}, {"flac", "flac"},
};

bool RealEncoderAvailable(const char* name) {
    return avcodec_find_encoder_by_name(name) != nullptr;
}

const char* SampleFormatName(int format) {
    const char* name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(format));
    return name != nullptr ? name : "unknown";
}

} // namespace

std::vector<AudioReencodeCandidate> AudioReencodeCandidates(const std::string& format_name,
                                                            const AudioEncoderAvailable& available) {
    // format_name 目前不影响顺序：候选表本身就是「先试最通用的」。留着这个参数
    // 是为了让调用点显式说明「顺序是按目标容器算的」，将来真要按容器分叉时，
    // 改这里而不是改执行层。
    (void)format_name;
    const auto& is_available = available ? available : AudioEncoderAvailable(RealEncoderAvailable);
    std::vector<AudioReencodeCandidate> candidates;
    candidates.reserve(sizeof(kCandidates) / sizeof(kCandidates[0]));
    for (const auto& spec : kCandidates) {
        if (!is_available(spec.encoder_name))
            continue;
        candidates.push_back(AudioReencodeCandidate{spec.encoder_name, spec.codec_name});
    }
    return candidates;
}

bool ChooseAudioEncodeSettings(const AVCodec* encoder, const AVCodecParameters* source, AudioEncodeSettings* out,
                               std::string* error) {
    if (out != nullptr)
        *out = AudioEncodeSettings{};
    if (error != nullptr)
        error->clear();
    if (encoder == nullptr || source == nullptr || out == nullptr) {
        if (error != nullptr)
            *error = "缺少编码器或源参数";
        return false;
    }

    const void* configs = nullptr;
    int count = 0;
    AudioEncodeSettings settings;

    // ---- 采样格式：编码器声明表的**第一项**就是它的首选（FFmpeg 约定按偏好排序）。
    // 我们不去挑「看起来更好」的格式 —— 那要自己判断有损 / 无损，等于猜。
    configs = nullptr;
    count = 0;
    if (avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count) < 0 ||
        configs == nullptr || count <= 0) {
        if (error != nullptr)
            *error = std::string("编码器 ") + encoder->name + " 没有声明可用的采样格式";
        return false;
    }
    settings.sample_format = static_cast<const AVSampleFormat*>(configs)[0];

    // ---- 采样率：源采样率在支持列表里就原样保留（不重采样 = 不动音高 / 不动时长）。
    const int source_rate = source->sample_rate > 0 ? source->sample_rate : 48000;
    configs = nullptr;
    count = 0;
    const bool rates_known =
        avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, &count) >= 0 &&
        configs != nullptr && count > 0;
    if (!rates_known) {
        // 不声明支持列表 = 接受任意采样率（flac / pcm 这类）。
        settings.sample_rate = source_rate;
    } else {
        const int* rates = static_cast<const int*>(configs);
        const bool exact = std::find(rates, rates + count, source_rate) != rates + count;
        if (exact) {
            settings.sample_rate = source_rate;
        } else {
            // 取比例上最接近的（按 log2 距离，避免「44100 → 96000 和 → 32000 距离不等」
            // 这类用算术差判断会选错的情形）。
            int best = rates[0];
            double best_distance = std::abs(std::log2(static_cast<double>(best) / source_rate));
            for (int i = 1; i < count; ++i) {
                const double distance = std::abs(std::log2(static_cast<double>(rates[i]) / source_rate));
                if (distance < best_distance) {
                    best = rates[i];
                    best_distance = distance;
                }
            }
            settings.sample_rate = best;
            settings.sample_rate_changed = true;
            settings.note = "采样率 " + std::to_string(source_rate) + " → " + std::to_string(best) + "（" +
                            encoder->name + " 不接受 " + std::to_string(source_rate) + "）";
        }
    }

    // ---- 声道：优先原样保留声道数；编码器不支持时才退到立体声 / 单声道。
    configs = nullptr;
    count = 0;
    const bool layouts_known =
        avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &configs, &count) >= 0 &&
        configs != nullptr && count > 0;
    const int source_channels = source->ch_layout.nb_channels > 0 ? source->ch_layout.nb_channels : 2;
    if (!layouts_known) {
        settings.channels = source_channels;
        if (source->ch_layout.order == AV_CHANNEL_ORDER_NATIVE && source->ch_layout.u.mask != 0)
            settings.channel_mask = source->ch_layout.u.mask;
    } else {
        const AVChannelLayout* layouts = static_cast<const AVChannelLayout*>(configs);
        const AVChannelLayout* chosen = nullptr;
        for (int i = 0; i < count; ++i) {
            if (layouts[i].nb_channels == source_channels) {
                chosen = &layouts[i];
                break;
            }
        }
        if (chosen == nullptr) {
            // 声道数对不上：先保立体声（绝大多数播放场景），再保单声道，最后取第一个。
            const AVChannelLayout* stereo = nullptr;
            const AVChannelLayout* mono = nullptr;
            for (int i = 0; i < count; ++i) {
                if (layouts[i].nb_channels == 2 && stereo == nullptr)
                    stereo = &layouts[i];
                if (layouts[i].nb_channels == 1 && mono == nullptr)
                    mono = &layouts[i];
            }
            chosen = stereo != nullptr ? stereo : (mono != nullptr ? mono : &layouts[0]);
            settings.channel_layout_changed = true;
            const std::string note = "声道 " + std::to_string(source_channels) + " → " +
                                     std::to_string(chosen->nb_channels) + "（" + encoder->name + " 不支持 " +
                                     std::to_string(source_channels) + " 声道）";
            settings.note = settings.note.empty() ? note : settings.note + "；" + note;
        }
        settings.channels = chosen->nb_channels;
        if (chosen->order == AV_CHANNEL_ORDER_NATIVE)
            settings.channel_mask = chosen->u.mask;
    }

    *out = settings;
    return true;
}

std::string AudioEncodeSettingsText(const AudioEncodeSettings& settings) {
    std::string text = std::to_string(settings.sample_rate) + " Hz / " + std::to_string(settings.channels) +
                       " 声道 / " + SampleFormatName(settings.sample_format);
    if (!settings.note.empty())
        text += "（" + settings.note + "）";
    return text;
}

std::string AudioReencodeAttemptsText(const std::vector<AudioReencodeAttempt>& attempts) {
    if (attempts.empty())
        return "没有可尝试的编码器";
    std::string text;
    for (const auto& attempt : attempts) {
        if (!text.empty())
            text += "；";
        text += attempt.encoder_name + ": ";
        if (attempt.encoder_missing)
            text += "本构建没有该编码器";
        else if (attempt.settings_rejected)
            text += "参数开不起来" + (attempt.detail.empty() ? std::string() : "（" + attempt.detail + "）");
        else if (attempt.container_rejected)
            text += "容器拒绝" + (attempt.detail.empty() ? std::string() : "（" + attempt.detail + "）");
        else
            text += "未尝试";
    }
    return text;
}

AudioReencodeTarget SelectAudioReencodeTarget(const char* format_name, const AVCodecParameters* source,
                                              const AudioEncoderAvailable& available) {
    AudioReencodeTarget target;
    if (format_name == nullptr || source == nullptr)
        return target;

    for (const auto& candidate : AudioReencodeCandidates(format_name, available)) {
        AudioReencodeAttempt attempt;
        attempt.encoder_name = candidate.encoder_name;

        const AVCodec* codec = avcodec_find_encoder_by_name(candidate.encoder_name.c_str());
        if (codec == nullptr) {
            attempt.encoder_missing = true;
            target.attempts.push_back(std::move(attempt));
            continue;
        }

        AudioEncodeSettings settings;
        std::string settings_error;
        if (!ChooseAudioEncodeSettings(codec, source, &settings, &settings_error)) {
            attempt.settings_rejected = true;
            attempt.detail = settings_error;
            target.attempts.push_back(std::move(attempt));
            continue;
        }

        AVCodecContext* encoder = avcodec_alloc_context3(codec);
        if (encoder == nullptr) {
            attempt.settings_rejected = true;
            attempt.detail = "无法分配编码器上下文";
            target.attempts.push_back(std::move(attempt));
            continue;
        }
        encoder->sample_rate = settings.sample_rate;
        encoder->sample_fmt = static_cast<AVSampleFormat>(settings.sample_format);
        if (settings.channel_mask != 0) {
            encoder->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
            encoder->ch_layout.nb_channels = settings.channels;
            encoder->ch_layout.u.mask = settings.channel_mask;
        } else {
            av_channel_layout_default(&encoder->ch_layout, settings.channels);
        }
        // 立体声 96 kbps、单声道 96 kbps、多声道每声道 64 kbps。源若是 PCM，
        // 它的 bit_rate（1.5 Mbps 级）不能直接照搬 —— 那是未压缩码率。
        encoder->bit_rate = settings.channels <= 2 ? 96000 : 64000LL * settings.channels;
        // 目标容器要在头里写 extradata（mp4 的 esds / matroska 的 CodecPrivate），
        // 不开这个标志编码器不会在 open 时生成它，试写头就会因缺参数被拒。
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (avcodec_open2(encoder, codec, nullptr) < 0) {
            attempt.settings_rejected = true;
            attempt.detail = "编码器打不开（" + AudioEncodeSettingsText(settings) + "）";
            avcodec_free_context(&encoder);
            target.attempts.push_back(std::move(attempt));
            continue;
        }

        // 用**真实**的编码参数去试写头。只用参数表猜「容器应该能收」是假覆盖：
        // mov 的 esds / mkv 的 CodecPrivate 都对 extradata 有要求。
        AVCodecParameters* parameters = avcodec_parameters_alloc();
        std::string fit_detail;
        TranscodeContainerFit fit = TranscodeContainerFit::Unknown;
        if (parameters != nullptr && avcodec_parameters_from_context(parameters, encoder) >= 0) {
            parameters->codec_tag = 0;
            fit = ProbeTrackFitsContainer(format_name, parameters, &fit_detail);
        } else {
            fit_detail = "无法从编码器上下文取编码参数";
        }
        avcodec_parameters_free(&parameters);

        if (fit != TranscodeContainerFit::Accepted) {
            attempt.container_rejected = true;
            attempt.detail = fit_detail;
            avcodec_free_context(&encoder);
            target.attempts.push_back(std::move(attempt));
            continue;
        }

        target.found = true;
        target.encoder_name = candidate.encoder_name;
        target.codec_name = avcodec_get_name(codec->id);
        target.settings = settings;
        target.encoder = encoder;
        target.attempts.push_back(std::move(attempt));
        return target;
    }
    return target;
}

} // namespace repair
} // namespace videoeye
