#include "tests/support/MediaSequenceFingerprint.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

namespace videoeye_test {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

std::string AvError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return std::string(buf);
}

// 逐帧亮度采样：固定取 16x16 个样本点，避免大分辨率下 hashing 全画面拖慢测试，
// 又足以让「换了一帧 / 画面被改写」改变哈希。yuv420p 的 data[0] 是 Y 平面，
// packed RGB 的 data[0] 是整幅图，两种都能用同一个步长公式。
uint64_t HashFrameLuma(const AVFrame* frame) {
    if (frame->data[0] == nullptr || frame->width <= 0 || frame->height <= 0)
        return 0;
    const int step_x = std::max(1, frame->width / 16);
    const int step_y = std::max(1, frame->height / 16);
    uint64_t hash = kFnvOffset;
    for (int y = 0; y < frame->height; y += step_y) {
        const uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < frame->width; x += step_x)
            hash = Fnv1a64(hash, &row[x], 1);
    }
    return hash;
}

void DrainDecoder(AVCodecContext* decoder, AVFrame* frame, StreamSequence& out, uint64_t& rolling,
                  int64_t& last_display_pts) {
    int rc = 0;
    while ((rc = avcodec_receive_frame(decoder, frame)) == 0) {
        const uint64_t frame_hash = HashFrameLuma(frame);
        rolling = Fnv1a64(rolling, &frame_hash, sizeof(frame_hash));
        // avcodec_receive_frame 的输出顺序 = 显示顺序。B 帧重排正确时这里的
        // PTS 单调递增；包顺序里的 PTS 则必然是锯齿状，不能拿来判断错序。
        const int64_t display_pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
        if (display_pts != AV_NOPTS_VALUE) {
            ++out.display_pts_samples;
            if (last_display_pts != AV_NOPTS_VALUE && display_pts < last_display_pts)
                out.display_pts_monotonic = false;
            last_display_pts = display_pts;
        }
        ++out.decoded_frames;
        av_frame_unref(frame);
    }
    if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF && out.decode_error.empty())
        out.decode_error = "avcodec_receive_frame: " + AvError(rc);
}

// 每条视频流单独重开一次输入：包级遍历已经把整个文件顺序读完了，解码器需要
// 自己的 AVFormatContext，交错共用会让「按流配对」变得难以自证。
void DecodeVideoStream(const std::string& path, int stream_index, StreamSequence& out) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        out.decode_error = "avformat_open_input 失败（解码阶段）";
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        out.decode_error = "avformat_find_stream_info 失败（解码阶段）";
        avformat_close_input(&fmt);
        return;
    }
    if (stream_index < 0 || stream_index >= static_cast<int>(fmt->nb_streams)) {
        out.decode_error = "视频流下标越界";
        avformat_close_input(&fmt);
        return;
    }

    const AVCodec* codec = avcodec_find_decoder(fmt->streams[stream_index]->codecpar->codec_id);
    if (codec == nullptr) {
        out.decode_error = "找不到解码器";
        avformat_close_input(&fmt);
        return;
    }
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (decoder == nullptr || packet == nullptr || frame == nullptr) {
        out.decode_error = "解码器/包/帧分配失败";
    } else if (avcodec_parameters_to_context(decoder, fmt->streams[stream_index]->codecpar) < 0 ||
               avcodec_open2(decoder, codec, nullptr) < 0) {
        out.decode_error = "解码器初始化失败";
    } else {
        uint64_t rolling = kFnvOffset;
        int64_t last_display_pts = AV_NOPTS_VALUE;
        int read_rc = 0;
        while ((read_rc = av_read_frame(fmt, packet)) == 0) {
            if (packet->stream_index == stream_index) {
                // EAGAIN 只说明编码器/解码器内部满了：排空后必须把**同一个包**
                // 重送，否则会静默少解一帧，让两次取证都"一致"地少一帧。
                int send_rc = avcodec_send_packet(decoder, packet);
                while (send_rc == AVERROR(EAGAIN)) {
                    DrainDecoder(decoder, frame, out, rolling, last_display_pts);
                    send_rc = avcodec_send_packet(decoder, packet);
                }
                if (send_rc < 0 && out.decode_error.empty())
                    out.decode_error = "avcodec_send_packet: " + AvError(send_rc);
                DrainDecoder(decoder, frame, out, rolling, last_display_pts);
            }
            av_packet_unref(packet);
        }
        if (read_rc != AVERROR_EOF && out.decode_error.empty())
            out.decode_error = "av_read_frame（解码阶段）: " + AvError(read_rc);
        avcodec_send_packet(decoder, nullptr); // flush：B 帧尾部的帧不能漏
        DrainDecoder(decoder, frame, out, rolling, last_display_pts);
        out.frame_sequence_hash = HexHash(rolling);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&fmt);
}

// 流描述是唯一事实源：完整取证与轻量预筛都走这里，避免两处清单漂移。
void DescribeStreams(AVFormatContext* fmt, MediaSequence& sequence) {
    sequence.opened = true;
    sequence.duration_seconds = fmt->duration > 0 ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
    sequence.streams.resize(fmt->nb_streams);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* stream = fmt->streams[i];
        StreamSequence& out = sequence.streams[i];
        out.index = static_cast<int>(i);
        const char* type = av_get_media_type_string(stream->codecpar->codec_type);
        out.codec_type = type != nullptr ? type : "unknown";
        out.codec_name = avcodec_get_name(stream->codecpar->codec_id);
        out.time_base_num = stream->time_base.num;
        out.time_base_den = stream->time_base.den;
    }
}

} // namespace

uint64_t Fnv1a64(uint64_t state, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        state ^= bytes[i];
        state *= kFnvPrime;
    }
    return state;
}

std::string HexHash(uint64_t value) {
    char buf[32] = {0};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buf);
}

MediaSequence ProbeStreamLayout(const std::string& path) {
    MediaSequence sequence;
    AVFormatContext* fmt = nullptr;
    const int open_rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (open_rc < 0 || fmt == nullptr) {
        sequence.error = "avformat_open_input: " + AvError(open_rc);
        return sequence;
    }
    const int probe_rc = avformat_find_stream_info(fmt, nullptr);
    if (probe_rc < 0) {
        sequence.error = "avformat_find_stream_info: " + AvError(probe_rc);
        avformat_close_input(&fmt);
        return sequence;
    }
    DescribeStreams(fmt, sequence);
    avformat_close_input(&fmt);
    return sequence;
}

MediaSequence CaptureMediaSequence(const std::string& path, bool decode_frames, uint64_t max_packets) {
    MediaSequence sequence;
    AVFormatContext* fmt = nullptr;
    const int open_rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (open_rc < 0 || fmt == nullptr) {
        sequence.error = "avformat_open_input: " + AvError(open_rc);
        return sequence;
    }
    const int probe_rc = avformat_find_stream_info(fmt, nullptr);
    if (probe_rc < 0) {
        sequence.error = "avformat_find_stream_info: " + AvError(probe_rc);
        avformat_close_input(&fmt);
        return sequence;
    }

    DescribeStreams(fmt, sequence);

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        sequence.error = "av_packet_alloc 失败";
        avformat_close_input(&fmt);
        return sequence;
    }

    std::vector<uint64_t> rolling(sequence.streams.size(), kFnvOffset);
    std::vector<int64_t> last_pts(sequence.streams.size(), AV_NOPTS_VALUE);
    std::vector<int64_t> last_dts(sequence.streams.size(), AV_NOPTS_VALUE);

    int read_rc = 0;
    while ((read_rc = av_read_frame(fmt, packet)) == 0) {
        if (sequence.total_packets >= max_packets) {
            sequence.error = "超过 max_packets 护栏，取证不完整";
            av_packet_unref(packet);
            break;
        }
        const int index = packet->stream_index;
        if (index >= 0 && index < static_cast<int>(sequence.streams.size())) {
            StreamSequence& out = sequence.streams[static_cast<size_t>(index)];
            PacketFacts facts;
            facts.has_pts = packet->pts != AV_NOPTS_VALUE;
            facts.has_dts = packet->dts != AV_NOPTS_VALUE;
            facts.pts = facts.has_pts ? packet->pts : 0;
            facts.dts = facts.has_dts ? packet->dts : 0;
            facts.duration = packet->duration;
            facts.size = packet->size;
            facts.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            facts.payload_hash = Fnv1a64(kFnvOffset, packet->data, static_cast<size_t>(packet->size));

            if (facts.has_pts) {
                if (last_pts[static_cast<size_t>(index)] != AV_NOPTS_VALUE &&
                    facts.pts < last_pts[static_cast<size_t>(index)])
                    out.pts_monotonic = false;
                last_pts[static_cast<size_t>(index)] = facts.pts;
            }
            if (facts.has_dts) {
                if (last_dts[static_cast<size_t>(index)] != AV_NOPTS_VALUE &&
                    facts.dts < last_dts[static_cast<size_t>(index)])
                    out.dts_monotonic = false;
                last_dts[static_cast<size_t>(index)] = facts.dts;
            }
            if (facts.keyframe)
                ++out.keyframe_count;

            uint64_t& state = rolling[static_cast<size_t>(index)];
            state = Fnv1a64(state, &facts.payload_hash, sizeof(facts.payload_hash));
            out.packets.push_back(facts);
            ++sequence.total_packets;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    // 非零且非 EOF 说明中途读失败：不能被当成"完整取证"，否则残缺包序列
    // 会两边都少一截，看起来还是一致。
    if (read_rc != AVERROR_EOF && sequence.error.empty())
        sequence.error = "av_read_frame: " + AvError(read_rc);

    for (size_t i = 0; i < sequence.streams.size(); ++i)
        sequence.streams[i].payload_sequence_hash = HexHash(rolling[i]);

    if (decode_frames) {
        for (auto& out : sequence.streams) {
            if (out.codec_type != "video")
                continue;
            DecodeVideoStream(path, out.index, out);
        }
    }

    avformat_close_input(&fmt);
    return sequence;
}

std::vector<std::string> CompareExactSequence(const MediaSequence& expected, const MediaSequence& actual) {
    std::vector<std::string> diffs;
    const std::string prefix = "序列对比 ";
    // 空事实守护：任何一侧没打开 / 没包 / 出错，都不允许返回「无差异」。
    if (!expected.opened || !actual.opened)
        diffs.push_back(prefix + "源或产物未能打开");
    if (!expected.error.empty())
        diffs.push_back(prefix + "源取证不完整: " + expected.error);
    if (!actual.error.empty())
        diffs.push_back(prefix + "产物取证不完整: " + actual.error);
    if (expected.total_packets == 0 || actual.total_packets == 0)
        diffs.push_back(prefix + "存在零包文件，无法证明一致");
    if (!diffs.empty())
        return diffs;

    if (expected.streams.size() != actual.streams.size()) {
        diffs.push_back(prefix + "流数不一致: " + std::to_string(expected.streams.size()) + " vs " +
                        std::to_string(actual.streams.size()));
        return diffs;
    }

    for (size_t i = 0; i < expected.streams.size(); ++i) {
        const StreamSequence& a = expected.streams[i];
        const StreamSequence& b = actual.streams[i];
        const std::string tag = prefix + "stream[" + std::to_string(i) + "] ";
        if (a.packets.empty() || b.packets.empty()) {
            diffs.push_back(tag + "存在空流，无法逐包配对");
            continue;
        }
        if (a.codec_type != b.codec_type)
            diffs.push_back(tag + "codec_type: " + a.codec_type + " vs " + b.codec_type);
        if (a.codec_name != b.codec_name)
            diffs.push_back(tag + "codec_name: " + a.codec_name + " vs " + b.codec_name);
        if (a.packets.size() != b.packets.size())
            diffs.push_back(tag + "包数: " + std::to_string(a.packets.size()) + " vs " +
                            std::to_string(b.packets.size()));
        if (a.payload_sequence_hash != b.payload_sequence_hash)
            diffs.push_back(tag + "逐包载荷序列不一致（顺序/内容被改写）");
        if (!a.dts_monotonic)
            diffs.push_back(tag + "源侧 DTS 不单调");
        if (!b.dts_monotonic)
            diffs.push_back(tag + "产物 DTS 不单调");
        if (a.codec_type == "video") {
            // 包顺序 PTS 不是判据（B 帧），改看解码输出的显示顺序；同时要求
            // 真的取到过显示时间戳，否则「没测到」会伪装成「顺序正确」。
            if (a.display_pts_samples == 0 || b.display_pts_samples == 0)
                diffs.push_back(tag + "缺少显示顺序 PTS 事实，无法证明帧顺序正确");
            if (!a.display_pts_monotonic)
                diffs.push_back(tag + "源侧显示顺序 PTS 不单调");
            if (!b.display_pts_monotonic)
                diffs.push_back(tag + "产物显示顺序 PTS 不单调");
        }

        // 相邻 PTS 差按**秒**比，而不是按时基刻度：跨容器 remux 时
        // av_packet_rescale_ts 合法地改写刻度（MKV 1/1000 -> MP4 1/12800），
        // 刻度不同但时间值相同不是缺陷。反过来，时基写错导致真实间隔变化
        // 会在这里被抓到。手工构造的守护序列没有时基，退回刻度比较。
        const auto delta_seconds = [](const StreamSequence& s, size_t k) {
            const int64_t ticks = s.packets[k].pts - s.packets[k - 1].pts;
            if (s.time_base_den <= 0)
                return static_cast<double>(ticks);
            return ticks * static_cast<double>(s.time_base_num) / s.time_base_den;
        };
        const size_t n = std::min(a.packets.size(), b.packets.size());
        for (size_t k = 1; k < n; ++k) {
            if (!a.packets[k].has_pts || !b.packets[k].has_pts)
                continue;
            const double da = delta_seconds(a, k);
            const double db = delta_seconds(b, k);
            // 1 ms 容差吸收不同时基的量化误差；真正的间隔错会差一个数量级。
            if (std::abs(da - db) > 0.001) {
                diffs.push_back(tag + "第 " + std::to_string(k) + " 个 PTS 间隔(秒): " + std::to_string(da) + " vs " +
                                std::to_string(db));
                break;
            }
        }
        if (!a.frame_sequence_hash.empty() || !b.frame_sequence_hash.empty()) {
            if (a.frame_sequence_hash != b.frame_sequence_hash)
                diffs.push_back(tag + "逐帧亮度序列不一致（包相同但解出的画面不同）");
            if (a.decoded_frames != b.decoded_frames)
                diffs.push_back(tag + "解出帧数: " + std::to_string(a.decoded_frames) + " vs " +
                                std::to_string(b.decoded_frames));
        }
        if (!b.decode_error.empty())
            diffs.push_back(tag + "产物解码报错: " + b.decode_error);
    }
    return diffs;
}

bool FramesIdentical(const MediaSequence& expected, const MediaSequence& actual) {
    if (expected.streams.size() != actual.streams.size())
        return false;
    bool saw_video = false;
    for (size_t i = 0; i < expected.streams.size(); ++i) {
        const StreamSequence& a = expected.streams[i];
        const StreamSequence& b = actual.streams[i];
        if (a.codec_type != "video")
            continue;
        if (a.frame_sequence_hash.empty() || b.frame_sequence_hash.empty())
            return false;
        if (a.frame_sequence_hash != b.frame_sequence_hash)
            return false;
        if (a.decoded_frames == 0 || a.decoded_frames != b.decoded_frames)
            return false;
        saw_video = true;
    }
    return saw_video;
}

std::vector<std::string> CompareSubsequence(const MediaSequence& expected, const MediaSequence& actual) {
    std::vector<std::string> diffs;
    const std::string prefix = "区间裁剪 ";
    if (!expected.opened || !actual.opened || !expected.error.empty() || !actual.error.empty()) {
        diffs.push_back(prefix + "源或产物取证不完整");
        return diffs;
    }

    const auto find_video = [](const MediaSequence& s) -> const StreamSequence* {
        for (const auto& stream : s.streams)
            if (stream.codec_type == "video")
                return &stream;
        return nullptr;
    };
    const StreamSequence* source = find_video(expected);
    const StreamSequence* clipped = find_video(actual);
    if (source == nullptr || clipped == nullptr) {
        diffs.push_back(prefix + "缺少视频流，无法做子序列配对");
        return diffs;
    }
    if (source->packets.empty() || clipped->packets.empty()) {
        diffs.push_back(prefix + "存在零包视频流，无法证明裁剪");
        return diffs;
    }
    if (clipped->packets.size() >= source->packets.size())
        diffs.push_back(prefix + "产物包数未少于源，裁剪没有发生");
    if (!clipped->display_pts_monotonic)
        diffs.push_back(prefix + "产物显示顺序 PTS 不单调");
    if (!clipped->dts_monotonic)
        diffs.push_back(prefix + "产物 DTS 不单调");
    if (clipped->decoded_frames == 0)
        diffs.push_back(prefix + "产物没有解出任何帧");
    if (!clipped->decode_error.empty())
        diffs.push_back(prefix + "产物解码报错: " + clipped->decode_error);

    // 朴素连续子序列匹配：包数在测试样本里最多几百，够用且能给出精确位置。
    size_t found_at = std::string::npos;
    for (size_t start = 0; start + clipped->packets.size() <= source->packets.size(); ++start) {
        bool match = true;
        for (size_t k = 0; k < clipped->packets.size(); ++k) {
            if (source->packets[start + k].payload_hash != clipped->packets[k].payload_hash) {
                match = false;
                break;
            }
        }
        if (match) {
            found_at = start;
            break;
        }
    }
    if (found_at == std::string::npos)
        diffs.push_back(prefix + "产物包序列不是源序列的连续子序列（丢包 / 重排 / 换了内容）");
    return diffs;
}

bool HasDecodableVideo(const MediaSequence& sequence) {
    if (!sequence.opened || !sequence.error.empty() || sequence.total_packets == 0)
        return false;
    for (const auto& stream : sequence.streams) {
        if (stream.codec_type == "video" && !stream.packets.empty() && stream.decoded_frames > 0 &&
            stream.decode_error.empty())
            return true;
    }
    return false;
}

} // namespace videoeye_test
