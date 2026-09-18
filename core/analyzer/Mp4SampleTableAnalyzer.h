#pragma once

// MP4/fMP4 容器一致性校验器
//
// 职责:
//   1) 基于 Bento4 解析 stbl 全表（stts/ctts/stss/stsz/stsc/stco/co64/elst）与
//      fMP4 的 moof/traf/tfhd/tfdt/trun，展开每个样本（offset/DTS/PTS/size/keyframe）；
//   2) 交叉校验各表是否自洽、chunk offset 是否越界、elst 是否造成首帧偏移、
//      分片序号与解码时间是否连续。
//
// 依赖边界（与 ColorHdrAnalyzer 一致）:
//   - 头文件不依赖 Bento4 / Qt，纯 C++ 可单测；
//   - .cpp 里 Bento4 相关代码用 HAVE_BENTO4 包裹，缺库时退化为空实现；
//   - 校验逻辑 Validate() 是纯函数，单测直接喂合成 Mp4SampleTableResult。

#include <cstdint>
#include <string>

#include "core/model/Mp4SampleInfo.h"

namespace videoeye {
namespace analyzer {

struct Mp4SampleTableOptions {
    // 单个轨道最多展开多少个样本（大文件保护；超出只做表级校验）
    uint32_t max_samples_per_track = 20000;
    // 是否逐样本展开（关闭时只做各表计数与分片校验，开销极低）
    bool expand_samples = true;
    // 音视频首个样本呈现时间差的告警阈值（毫秒）
    double av_start_tolerance_ms = 40.0;
    // 首帧被 elst 跳过的告警阈值（毫秒）
    double elst_shift_tolerance_ms = 33.0;
    // 负合成时间的告警阈值（毫秒，绝对值超过才告警）
    double negative_cts_tolerance_ms = 0.0;
    // 分片解码时间允许的误差（媒体时基单位，防止舍入误报）
    uint32_t fragment_time_tolerance = 1;
};

class Mp4SampleTableAnalyzer {
public:
    Mp4SampleTableAnalyzer();
    ~Mp4SampleTableAnalyzer();

    // 解析 + 校验。返回 true 表示文件被成功解析（不代表没有问题）。
    bool AnalyzeFile(const std::string& file_path,
                     model::Mp4SampleTableResult& out,
                     const Mp4SampleTableOptions& options = Mp4SampleTableOptions{});

    // 纯逻辑校验：在 out 上补齐 issues 与逐样本 flags。
    // 单测与"只改阈值重新评估"都走这里；幂等（每次调用先清空 issues）。
    static void Validate(model::Mp4SampleTableResult& out,
                         const Mp4SampleTableOptions& options = Mp4SampleTableOptions{});

    void Reset();

private:
    Mp4SampleTableOptions options_;
};

}  // namespace analyzer
}  // namespace videoeye
