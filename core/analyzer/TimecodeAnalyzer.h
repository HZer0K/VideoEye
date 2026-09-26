#pragma once

// 时码与章节分析（功能 9）。
//
// 交付审核里时码是硬指标：母版要求 00:59:58:00 起板、广告插入点要报时码，
// 光看"第 3.5 秒"没法跟客户对表。本模块负责把时码从三条路里挖出来：
//   1) MOV/MP4 的 tmcd 时码轨（AV_CODEC_ID_TIMECODE）—— 首帧样本是 4 字节帧序号；
//   2) 容器 / 流 metadata 里的 timecode tag（FFmpeg 会把 tmcd 的起始时码
//      复制到视频流的 metadata 上，多数文件走这条路就能拿到）；
//   3) 章节时间线（AVFormatContext::chapters）—— 顺便查越界与重叠。
//
// 为什么 drop-frame 要单独查：29.97 素材用 non-drop 时码，一小时后显示时间
// 会比真实时间慢约 3.6 秒，广告插入点就会整体漂移。
//
// 头文件只前向声明 FFmpeg 类型；换算逻辑在 core/model/TimecodeInfo.cpp，可单测。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/model/TimecodeInfo.h"

struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace videoeye {
namespace analyzer {

struct TimecodeOptions {
    // 读 metadata 里的 timecode tag
    bool read_metadata = true;
    // 读 MOV/MP4 tmcd 时码轨的首包
    bool read_timecode_track = true;
    // 检查章节越界 / 重叠 / 倒序
    bool check_chapters = true;
    // 检查 drop-frame 与帧率是否匹配
    bool check_drop_frame = true;
    // 29.97 / 59.94 且时码本身没有 drop-frame 标记时，按 NTSC 惯例假定 drop frame
    bool assume_drop_frame_on_ntsc = true;
    // 帧率提示（0 = 从第一条视频流推断）
    double fps_hint = 0.0;
};

class TimecodeAnalyzer {
public:
    void Reset(const TimecodeOptions& options = TimecodeOptions{});

    // find_stream_info 之后调用：登记时码轨、读 metadata tag、读章节
    void RegisterStreams(const AVFormatContext* fmt, double media_duration_seconds);

    // 逐包回调：只取 tmcd 轨的首包（后面的是同一轨的连续样本，重复解析没意义）
    void OnPacket(const AVPacket* pkt, const AVStream* stream);

    void Finish();

    const model::TimecodeAnalysisResult& result() const { return result_; }
    const TimecodeOptions& options() const { return options_; }

    // tmcd 样本 -> 时码。样本是 4 字节大端帧序号（QuickTime timecode media）。
    // 纯逻辑，单测直接调用。
    static bool DecodeTmcdSample(const uint8_t* data, size_t size, double fps,
                                 bool drop_frame, model::Timecode& out);

private:
    void CheckChapters();
    void AddChapterIssue(model::ChapterIssueType type, int index, double start, double end,
                         const std::string& detail);

    TimecodeOptions options_;
    model::TimecodeAnalysisResult result_;
    int timecode_stream_index_ = -1;
    bool tmcd_decoded_ = false;
    double inferred_fps_ = 0.0;
};

}  // namespace analyzer
}  // namespace videoeye
