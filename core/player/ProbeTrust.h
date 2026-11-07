#pragma once

// 打开之后的「探测可信度」复核。
//
// 背景（实测，两次结果不一样，以第二次为准）：
//   一个 44.1kHz/2ch 的裸 PCM 命名成 .bin 丢给 avformat_open_input，FFmpeg 会挑
//   `adp` 这个伪 demuxer 接走，codec 报成 adpcm_dtk、采样率报成 48kHz，**并且
//   按"文件大小 / 每块字节数"算出一个 3.2s 的假时长**。于是打开成功、播放成功、
//   进度条还有值，只有声音是噪声。界面零报错，用户只能说"声音不对"。
//
// 第一版判据曾用「duration <= 0」识别裸流，被这组实测数据证伪：伪 demuxer 自己
// 会编时长。所以现在的判据只看两件确定的事：
//   1. demuxer 是不是"没有容器、直接按样本读"的伪 demuxer（白名单很短）；
//   2. 扩展名是否属于该 demuxer 认的合法名字（.pcm，或和 demuxer 同名）。
//
// 判据保守的另一面：命中时只产出**提示**，不阻断打开。用户手上可能真就是一份
// 没有头信息的 DTK 音频，拦下来反而把能放的东西变成不能放。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace player {

enum class ProbeVerdict {
    Trusted,                 // 探测结果与文件特征自洽
    SuspectRawAudioMismatch, // 极可能是"无容器头的裸音频被猜成了别的编码"
};

struct ProbeFacts {
    std::string demuxer_name;   // AVInputFormat::name，可能逗号分隔（如 "matroska,webm"）
    std::string codec_name;     // avcodec_get_name(codecpar->codec_id)，只用于提示文案
    std::string file_extension; // 源文件后缀：小写、无点，可为空
    bool has_video = false;
    bool has_audio = false;
};

// 拆 AVInputFormat::name 的逗号列表（"h264" / "matroska,webm" / "adpcm_dtk"）。
inline std::vector<std::string> SplitDemuxerNames(const std::string& name) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin <= name.size()) {
        const std::size_t comma = name.find(',', begin);
        const std::size_t end = (comma == std::string::npos) ? name.size() : comma;
        if (end > begin) {
            out.push_back(name.substr(begin, end - begin));
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return out;
}

// 是不是"没有容器、直接按采样格式读样本"的伪 demuxer。
//
// 名单来自 FFmpeg 里 pcm.c / adpcm 家族以及 raw audio 的 demuxer 注册名：它们没有
// 可校验的魔数、probe 函数只做"像不像"的弱判断，正是误判的来源。
// `adp` 必须在内：它就是那个会把任意 4 字节对齐的 .bin 认成 adpcm_dtk 的 demuxer
// （p.extensions = "adp,dtk"），也是本次实测踩到的元凶。
inline bool IsRawAudioDemuxerName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    if (name.compare(0, 6, "adpcm_") == 0 || name == "adpcm") {
        return true;
    }
    if (name.compare(0, 4, "pcm_") == 0) {
        return true;
    }
    static const char* const kNames[] = {
        "adp",   "s8",    "u8",    "s16le", "s16be", "u16le", "u16be", "s24le", "s24be",    "s32le",
        "s32be", "f32le", "f32be", "f64le", "f64be", "alaw",  "mulaw", "vidc",  "ima_alaw",
    };
    for (const char* candidate : kNames) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

// 该伪 demuxer 认可的文件名。命中就说明用户按约定命名了，不必提示。
//
//   * .pcm / .lpcm —— 项目自己的裸 PCM 约定（通常走专用打开路径，这里兜底）；
//   * 与 demuxer 同名 —— 例如 song.s16le、clip.f32be；
//   * `adp` 自带 adp/dtk 两个扩展名（见 libavformat/adp.c 的 p.extensions）。
inline bool IsExpectedRawAudioExtension(const std::string& demuxer, const std::string& extension) {
    if (extension.empty()) {
        return false;
    }
    if (extension == "pcm" || extension == "lpcm") {
        return true;
    }
    if (extension == demuxer) {
        return true;
    }
    if (demuxer == "adp" && (extension == "adp" || extension == "dtk")) {
        return true;
    }
    return false;
}

// 复核一次探测结果。调用方只需在打开成功后调用，不需要任何额外 IO。
inline ProbeVerdict ClassifyProbeTrust(const ProbeFacts& facts) {
    // 只有"纯音频"才谈得上被裸音频伪 demuxer 抢走：真视频流的 demuxer 不可能是
    // pcm_*/adpcm_*/adp，而带视频又报错的场景属于另一类问题。
    if (!facts.has_audio || facts.has_video) {
        return ProbeVerdict::Trusted;
    }

    const std::vector<std::string> names = SplitDemuxerNames(facts.demuxer_name);
    const bool raw_audio = std::any_of(names.begin(), names.end(), IsRawAudioDemuxerName);
    if (!raw_audio) {
        return ProbeVerdict::Trusted;
    }

    const bool expected = std::any_of(names.begin(), names.end(), [&facts](const std::string& name) {
        return IsExpectedRawAudioExtension(name, facts.file_extension);
    });
    if (expected) {
        return ProbeVerdict::Trusted;
    }

    // 通用后缀（.bin/.dat/.dump）或无扩展名 + 裸音频伪 demuxer = 典型误判。
    return ProbeVerdict::SuspectRawAudioMismatch;
}

// 面向用户的提示文案（写进媒体信息面板与日志）。不阻断播放，只说明怎么修。
inline std::string ProbeTrustMessage(const ProbeFacts& facts) {
    std::string detected = facts.demuxer_name;
    if (!facts.codec_name.empty() && facts.codec_name != facts.demuxer_name) {
        detected += " / " + facts.codec_name;
    }
    return "探测结果可疑：该文件被识别为裸音频（" + detected +
           "）。这类格式没有容器头，采样率与时长都是按文件大小猜出来的，"
           "播放出来很可能只是噪声。如果它其实是裸 PCM，请改名为 .pcm 再打开，"
           "并在参数对话框里指定采样格式 / 采样率 / 声道。";
}

} // namespace player
} // namespace videoeye
