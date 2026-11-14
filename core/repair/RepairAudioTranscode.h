#pragma once

// 阶段 6 / 切片 V：音频轨「转码成目标容器能收的编码」。
//
// 切片 T 把音频轨落成两条路：容器接受就 copy，容器拒绝就 drop。drop 是诚实的，
// 但它把「这条音轨还在、只是编码不合适」变成「这条音轨没有了」—— 用户拿到一份
// 没有声音的产物。本例（mov + opus / mov + flac / mp4 + truehd）都是实测可复现的。
//
// 这里补第三条路：解码 → 重采样 → 重新编码成目标容器接受的编码。
//
// 判据边界（必须随结论一起报出去）：
//   * 选哪个编码器不是查表得来的：候选顺序是「偏好」，**能不能用由两次实测决定**
//     ——① 编码器在本构建里存在且能按源参数开起来；② 用它产出的编码参数去试写
//     目标容器的头（内存里写，不落文件）。两次都过才选它，否则试下一个。
//   * 重编码是有损的，采样率 / 声道也可能被迫改变：这些都逐项记进 settings.note，
//     报告里必须写出来，不能只说「音轨已转码」。
//   * 一个候选都过不了时才 drop，并把每个候选被拒的原因逐条报出 —— 不能只说
//     「没有可用编码器」。

#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace videoeye {
namespace repair {

struct AudioReencodeCandidate {
    std::string encoder_name; // avcodec_find_encoder_by_name 的入参（"aac" / "libopus" …）
    std::string codec_name;   // 编码后的编码名，报告用
};

using AudioEncoderAvailable = std::function<bool(const char* encoder_name)>;

// 纯函数：按目标容器给出**偏好顺序**的候选编码器，并滤掉本构建没有的。
// 偏好理由：aac 是唯一被 S08 全部目标容器（mp4 / mov / matroska / mpegts）
// 接受的编码；其余按各容器的常见程度排。
std::vector<AudioReencodeCandidate> AudioReencodeCandidates(const std::string& format_name,
                                                            const AudioEncoderAvailable& available);

// 编码器参数。只放纯值 —— 不持有 AVChannelLayout，避免值对象的析构语义陷阱；
// 执行层按 channels + channel_mask 自己构造布局。
struct AudioEncodeSettings {
    int sample_rate = 0;
    int channels = 0;
    uint64_t channel_mask = 0;              // 0 = 用 av_channel_layout_default 推
    int sample_format = AV_SAMPLE_FMT_NONE; // AVSampleFormat
    bool sample_rate_changed = false;
    bool channel_layout_changed = false;
    // 与源不同的每一项的实测说明（"采样率 44100 → 48000（flac 不接受 44100）"）。
    std::string note;
};

// 纯函数：按编码器**自己声明的**能力挑一组能开的参数。
// 返回 false 表示连采样格式都问不出来（调用方应换下一个候选编码器）。
bool ChooseAudioEncodeSettings(const AVCodec* encoder, const AVCodecParameters* source, AudioEncodeSettings* out,
                               std::string* error);

// 一组参数的渲染（报告里紧跟编码器名，写清是否改变了采样率 / 声道）。
std::string AudioEncodeSettingsText(const AudioEncodeSettings& settings);

// 一次候选尝试的结果。逐条记账是「为什么最后 drop 了」的唯一依据。
struct AudioReencodeAttempt {
    std::string encoder_name;
    bool encoder_missing = false;    // 本构建没有这个编码器
    bool settings_rejected = false;  // 参数选不出来 / 编码器打不开
    bool container_rejected = false; // 试写头被目标容器拒绝
    std::string detail;
};

std::string AudioReencodeAttemptsText(const std::vector<AudioReencodeAttempt>& attempts);

struct AudioReencodeTarget {
    bool found = false;
    std::string encoder_name;
    std::string codec_name;
    AudioEncodeSettings settings;
    // 已打开的编码器上下文，**所有权归调用方**（必须 avcodec_free_context）。
    // 打开它就为了拿真实的 extradata 去试写头 —— 参数表能开不等于容器能收。
    AVCodecContext* encoder = nullptr;
    std::vector<AudioReencodeAttempt> attempts;
};

// 依次试候选：编码器存在 → 参数能开 → 容器接受。第一个全过的即选中。
// available 为空时用 avcodec_find_encoder_by_name 的真实结果。
AudioReencodeTarget SelectAudioReencodeTarget(const char* format_name, const AVCodecParameters* source,
                                              const AudioEncoderAvailable& available = AudioEncoderAvailable());

} // namespace repair
} // namespace videoeye
