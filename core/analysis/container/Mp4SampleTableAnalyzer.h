#pragma once

// MP4/fMP4 容器一致性校验器
//
// 职责:
//   1) 基于自研 utils::IsobmffParser 解析 stbl 全表
//      （stts/ctts/stss/stsz/stsc/stco/co64/elst）与 fMP4 的 moof/traf/tfhd/tfdt/trun，
//      展开每个样本（offset/DTS/PTS/size/keyframe）；
//   2) 交叉校验各表是否自洽、chunk offset 是否越界、elst 是否造成首帧偏移、
//      分片序号与解码时间是否连续。
//
// 依赖边界（与 ColorHdrAnalyzer 一致）:
//   - 头文件不依赖 Qt / FFmpeg / 任何第三方容器库，纯 C++ 可单测；
//   - 解析只依赖 core/media/container/IsobmffParser.h（纯 C++17 标准库）；
//   - 校验逻辑 Validate() 是纯函数，单测直接喂合成 Mp4SampleTableResult。

#include <cstdint>
#include <string>

#include "core/domain/model/Mp4SampleInfo.h"
#include "core/analysis/AnalysisOptions.h"

namespace videoeye {
namespace analyzer {

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
