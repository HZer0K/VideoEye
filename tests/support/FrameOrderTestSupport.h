#pragma once

// 帧级显示顺序测试的共用装置（H.264 / HEVC / VP9 / AV1 四套用例都要用）。
//
// 抽出来的理由不是"省几行"，而是**参考输出必须只有一个来源**：
//   * `RunFrameOrderAnalysis` 里那张"把其余维度全关掉"的开关表，与
//     AnalysisOptions 的每一次新增字段都要同步。抄两份的话，某天多一个
//     `analyze_new_thing = true` 的默认值，就只有一份用例会莫名其妙变慢/变红，
//     而另一份看起来还是对的。
//   * `DemuxForDisplayOrder` 是本项目里唯一"从容器取权威显示顺序"的地方。
//     各编码器的对账口径必须逐字相同，否则"HEVC 的结论与 H.264 一致"
//     这句话就不成立了。
//
// 与业务无关的测试数据（合成 SPS / slice 位流 / VP9 帧头）**不**放这里：
// 那些是要证明"实现与规范一致"的，各编码器的构造方式本来就该各写一份。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/analysis/orchestration/AnalysisEngine.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/FrameOrderInfo.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
}

namespace videoeye_test {

using videoeye::model::FrameOrderTimeline;

// 语料目录由构建系统注入（VIDEOEYE_CODEC_CORPUS_DIR），两个用到真实码流的
// 测试目标都要带这个定义。
inline const char* CodecCorpusDir() {
#ifdef VIDEOEYE_CODEC_CORPUS_DIR
    return VIDEOEYE_CODEC_CORPUS_DIR;
#else
    return "";
#endif
}

// 语料文件缺失时返回空串（GTEST_SKIP 不能写在非 void 函数里），由调用点跳过。
// 语料由 tests/corpus/generate_real_corpus.py 用 ffmpeg 生成并入库，
// 没生成过的机器上不该 FAIL；入库之后它随仓库一起走，这里就是必跑的。
inline std::string CodecSample(const char* name) {
    const std::filesystem::path path = std::filesystem::path(CodecCorpusDir()) / name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return std::string();
    return path.string();
}

inline void SkipIfMissing(const std::string& path, const char* name) {
    if (path.empty()) {
        GTEST_SKIP() << "缺真实样本 " << name << "，先跑 tests/corpus/generate_real_corpus.py";
    }
}

// 0,1,2,...n-1 —— 用来判断一份"显示顺序"参考有没有区分度（全等 = 没有重排）
inline std::vector<int64_t> Sequential(std::size_t n) {
    std::vector<int64_t> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<int64_t>(i);
    return out;
}

// 序列里最大的"向后跳步"幅度（相邻两项下降了多少）。给 0,3,2,4,1 得 3（4→1）。
inline int64_t MaxBackwardStep(const std::vector<int64_t>& values) {
    int64_t worst = 0;
    for (std::size_t i = 1; i < values.size(); ++i) {
        const int64_t back = values[i - 1] - values[i];
        if (back > worst)
            worst = back;
    }
    return worst;
}

// 容器自己给出的**显示顺序**：按"文件里的包顺序"记下每个包的 PTS。
//
// 这三个概念别搞混：
//   * 文件里的**包顺序** == 解码顺序（MP4 里 sample 按 DTS 递增排布）；
//   * 每个包的 **PTS** == 该帧的显示时刻，是**编码器**写进 ctts/stts 的，
//     不是任何解析器事后推出来的 —— 这正是它适合当参考输出的原因：
//     与被测代码零共享，且是"真实编码器认为的显示顺序"；
//   * 于是"把包按 PTS 排序"得到的下标序列，就是权威的 显示序 → 解码序 映射。
//
// ❗ 特意**不用**两条看起来更"直接"的路：
//   * AVFrame::coded_picture_number —— FFmpeg 7 起已移除，且它是解码器的内部记账；
//   * 裸流的 PTS —— 那是 libavformat 的 parser 自己按 POC 推的，
//     与被测逻辑同源，对得上也不能证明什么。
struct DemuxedOrderFacts {
    bool ok = false;
    // 第 i 项 = 文件里第 i 个视频包（解码顺序）的显示时间戳（秒）
    std::vector<double> pts_in_decode_order;
    // 第 i 项 = 容器（stss 同步样本表）标记的关键帧。与被测侧的 NAL 类型
    // 是两条完全独立的通道，先各自数一遍再对账。
    std::vector<bool> is_key_in_decode_order;

    // 按 GOP 切开后的"显示序 → 解码序"映射。
    //
    // ❗ 必须**分段**排：POC 在每个随机接入点处被重置回 0，两个 GOP 的 POC 值域
    // 完全重叠。整条时间线一起排会得到 0,0,2,2,4,4... 这种两两并列的序列 ——
    // 那不是 POC 算错了，是排序的口径错了。
    std::vector<std::vector<int64_t>> DecodeIndexInDisplayOrderPerGop() const {
        std::vector<std::vector<int64_t>> out;
        std::vector<std::size_t> current;
        auto flush = [&]() {
            if (current.empty())
                return;
            std::sort(current.begin(), current.end(),
                      [this](std::size_t a, std::size_t b) { return pts_in_decode_order[a] < pts_in_decode_order[b]; });
            std::vector<int64_t> segment;
            segment.reserve(current.size());
            for (std::size_t index : current)
                segment.push_back(static_cast<int64_t>(index));
            out.push_back(std::move(segment));
            current.clear();
        };
        for (std::size_t i = 0; i < pts_in_decode_order.size(); ++i) {
            if (is_key_in_decode_order[i])
                flush();
            current.push_back(i);
        }
        flush();
        return out;
    }
};

inline DemuxedOrderFacts DemuxForDisplayOrder(const std::string& path) {
    DemuxedOrderFacts facts;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt)
        return facts;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return facts;
    }
    int video_stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = static_cast<int>(i);
            break;
        }
    }
    if (video_stream < 0) {
        avformat_close_input(&fmt);
        return facts;
    }
    AVStream* stream = fmt->streams[video_stream];

    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video_stream && pkt->pts != AV_NOPTS_VALUE) {
            facts.pts_in_decode_order.push_back(static_cast<double>(pkt->pts) * av_q2d(stream->time_base));
            facts.is_key_in_decode_order.push_back((pkt->flags & AV_PKT_FLAG_KEY) != 0);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    facts.ok = facts.pts_in_decode_order.size() >= 2;
    return facts;
}

// 只开帧顺序维度，其余全关：这些用例只验证 POC，别的维度既无关又拖慢。
// ❗ 新增 AnalysisOptions 开关时**只改这一处**，两个编码器的用例一起跟上。
inline videoeye::model::AnalysisResult RunFrameOrderAnalysis(const std::string& path) {
    videoeye::model::AnalysisResult result;
    videoeye::AnalysisCallbacks callbacks;
    callbacks.on_failed = [](const std::string&) {};
    callbacks.on_finished = [](bool, const videoeye::model::AnalysisResult&) {};

    videoeye::AnalysisOptions options;
    options.analyze_frame_order = true;
    options.analyze_bitrate_gop = false;
    options.analyze_audio_qc = false;
    options.analyze_color_hdr = false;
    options.analyze_bitstream = false;
    options.analyze_mp4_sample_table = false;
    options.analyze_streaming_package = false;
    options.analyze_subtitle = false;
    options.analyze_timecode = false;
    options.analyze_aux_data = false;
    options.analyze_parameter_timeline = false;

    videoeye::AnalysisEngine engine;
    engine.Run(path, options, callbacks, &result);
    return result;
}

// 把一条时间线按随机接入点切成若干段，每段内给出"显示序 → 解码序"映射。
inline std::vector<std::vector<int64_t>> DisplayOrderPerGop(const FrameOrderTimeline& t) {
    struct Ranked {
        int64_t poc = 0;
        std::size_t decode_index = 0;
    };
    std::vector<std::vector<int64_t>> out;
    std::vector<Ranked> current;
    auto flush = [&]() {
        if (current.empty())
            return;
        std::sort(current.begin(), current.end(), [](const Ranked& a, const Ranked& b) { return a.poc < b.poc; });
        std::vector<int64_t> segment;
        segment.reserve(current.size());
        for (const Ranked& item : current)
            segment.push_back(static_cast<int64_t>(item.decode_index));
        out.push_back(std::move(segment));
        current.clear();
    };
    for (std::size_t i = 0; i < t.frames.size(); ++i) {
        // 分段按**随机接入点**而不是 idr：HEVC 的开放 GOP 用 CRA 分段，
        // 它的 POC 并不复位。只看 idr 会把整个文件当成一段，
        // 与容器（按同步样本分段）逐项对不上。
        if (t.frames[i].irap || t.frames[i].idr)
            flush();
        current.push_back({t.frames[i].poc, i});
    }
    flush();
    return out;
}

} // namespace videoeye_test
