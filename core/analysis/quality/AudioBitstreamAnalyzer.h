#pragma once

// 音频位流参数分析（D1）：在 demux 逐包扫描里解析音频帧头，不解码 PCM。
//
// 数据来源:
//   * AVCodecParameters: 容器声明的采样率/声道/布局 + extradata（AAC ASC / OpusHead）；
//   * AVPacket: ADTS / LOAS / AC-3 / E-AC-3 / Opus 的逐帧头。
//
// 产出 model::AudioBitstreamResult:
//   * 每条音频流的位流参数（采样率、声道、profile、dialnorm、acmod、
//     Opus pre-skip/gain、Atmos/JOC）；
//   * "位流声明 vs 容器声明"的不一致清单；
//   * 帧头解析失败/中途变参数的证据。
//
// 依赖边界与 AudioQcAnalyzer 一致：解析器本身不依赖 FFmpeg；
// 只有本文件的 RegisterStreams/OnPacket 碰 avformat。

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/codec/AudioBitstreamParser.h"
#include "core/domain/model/AudioBitstreamInfo.h"

struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace videoeye {

class AudioBitstreamAnalyzer {
public:
    void Reset(const AudioBitstreamOptions& options = AudioBitstreamOptions{});
    const AudioBitstreamOptions& options() const { return options_; }

    // find_stream_info 之后调用：登记所有音频流并读取容器声明/extradata。
    void RegisterStreams(const AVFormatContext* fmt);

    // 逐包回调：只处理已登记的音频流。
    void OnPacket(const AVPacket* pkt, const AVStream* stream);

    const model::AudioBitstreamResult& Finish();
    const model::AudioBitstreamResult& result() const { return result_; }

private:
    struct StreamState {
        model::AudioBitstreamStream info;
        uint32_t parsed_limit = 0;

        // AAC / LATM
        bool asc_ready = false;
        AacAudioSpecificConfig asc;
        bool latm_config_ready = false;
        LatmHeader latm_config;

        // 参数变化检测（只在第一次有效头之后比较）
        std::vector<int> dependent_substream_ids;

        bool has_header = false;
        int last_sample_rate = 0;
        int last_channels = 0;
        int last_frame_bytes = 0;
        int last_audio_object_type = -1;
        int last_acmod = -1;
        int last_bit_rate_kbps = 0;
        int frame_size_changes = 0;

        // 避免同一类不一致重复写很多条
        bool reported_sample_rate_change = false;
        bool reported_channel_change = false;
        bool reported_bitrate_change = false;

        void ObserveFrame(int sample_rate, int channels, int frame_bytes,
                          int audio_object_type, int acmod, int bit_rate_kbps);
    };

    StreamState* FindState(int stream_index);
    void ParseAacPacket(StreamState& state, const AVPacket* pkt);
    void ParseLatmPacket(StreamState& state, const AVPacket* pkt);
    void ParseAc3Packet(StreamState& state, const AVPacket* pkt, bool force_eac3);
    void ParseOpusPacket(StreamState& state, const AVPacket* pkt);
    void ApplyAacConfig(StreamState& state, const AacAudioSpecificConfig& asc,
                        model::AudioBitstreamCodec codec);
    void ApplyLatmConfig(StreamState& state, const LatmHeader& header);
    void ApplyAc3Header(StreamState& state, const Ac3Header& header);
    void ApplyOpusHead(StreamState& state, const OpusHead& head);
    void ApplyOpusToc(StreamState& state, const OpusToc& toc);
    void AddInconsistency(StreamState& state, const std::string& text);

    AudioBitstreamOptions options_;
    model::AudioBitstreamResult result_;
    std::vector<StreamState> states_;
    bool registered_ = false;
    bool finished_ = false;
};

}  // namespace videoeye
