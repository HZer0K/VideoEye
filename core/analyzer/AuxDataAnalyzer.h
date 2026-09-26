#pragma once

// 辅助数据轨（data stream）的 FFmpeg 侧采集（功能 9）。
//
// 与 Scte35Analyzer 的分工：
//   Scte35Analyzer 只认识 splice_info_section 的字节，不认识文件；
//   这一层负责"从 FFmpeg 里把 data 流找出来、喂进来、顺手把 metadata 收集齐"。
//
// 采集三样东西：
//   1) data stream 列表（SCTE-35 / tmcd 时码轨 / KLV / 私有 data），
//      含 handler name、language tag、包数与字节数 —— 用来发现"封装时丢了辅助轨"；
//   2) SCTE-35 cue 列表（时间、event id、out/in、duration）；
//   3) 容器级 / 流级 / 章节级 metadata 的 key-value 表。
//
// 头文件只前向声明 FFmpeg 类型，方便单测直接包含。

#include <cstdint>
#include <string>
#include <vector>

#include "core/analyzer/Scte35Analyzer.h"
#include "core/model/AuxiliaryDataInfo.h"

struct AVFormatContext;
struct AVPacket;
struct AVStream;
struct AVDictionary;

namespace videoeye {
namespace analyzer {

struct AuxDataOptions {
    // 采集 metadata（容器 / 流 / 章节）
    bool collect_metadata = true;
    // 解析 data 流里的 SCTE-35 cue
    bool analyze_scte35 = true;
    // 单条流最多记录多少条 cue（大文件直播流保护）
    uint32_t max_cues = 4096;
    uint32_t max_metadata_entries = 400;
    Scte35Options scte35_options;
};

class AuxDataAnalyzer {
public:
    void Reset(const AuxDataOptions& options = AuxDataOptions{});

    // find_stream_info 之后调用一次：登记 data 流 + 采集 metadata
    void RegisterStreams(const AVFormatContext* fmt);

    // 逐包回调：只关心已登记的 data 流（其它流直接忽略，成本可忽略）
    void OnPacket(const AVPacket* pkt, const AVStream* stream);

    void Finish();

    const model::AuxiliaryDataResult& result() const { return result_; }
    const AuxDataOptions& options() const { return options_; }

    // codec id -> data 流类型（未知返回 GenericData）
    static model::AuxStreamKind KindFromCodecId(int codec_id);

private:
    void CollectMetadata(const AVFormatContext* fmt);
    void AddMetadata(const char* scope, int stream_index, int chapter_index,
                     const struct AVDictionary* dict);

    AuxDataOptions options_;
    model::AuxiliaryDataResult result_;
    std::vector<int> data_stream_indices_;   // 需要在逐包阶段关注的流号
    bool registered_ = false;
};

}  // namespace analyzer
}  // namespace videoeye
