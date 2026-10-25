#pragma once

// 字幕轨分析（功能 9）。
//
// 支持范围（按交付审核的实际需求排的优先级）:
//   * 文本字幕 SRT / ASS-SSA / WebVTT / MOV tx3g：解析 cue 的起止时间，
//     检查重叠、空字幕、过短/过长、时间倒序、阅读速度；
//   * CEA-608/708：走 CeaCaptionDecoder 的**服务层状态机**（D2）——608 分 CC1-CC4
//     四个通道、pop-on/paint-on/roll-up 三种模式，708 按 DTVCC 包解到每个服务，
//     产出带真实起止时间的 cue，按通道/服务分别归属；
//   * 图形字幕（DVB / DVD / PGS）：只输出流 metadata 与包时间线（不做 OCR，见 D3 边界）。
//
// 依赖边界:
//   - 头文件只前向声明 FFmpeg 类型，纯文本解析全是静态函数（单测直接调用）；
//   - 只有 RegisterStreams / OnPacket 需要 avformat，且都在 .cpp 里。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/diagnostics/CeaCaptionDecoder.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/SubtitleCueInfo.h"

struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace videoeye {

// 解析出的裸 cue（还没有流/语言这些上下文）
struct ParsedSubtitleCue {
    bool valid = false;
    int cue_number = -1;
    double start_seconds = -1.0;
    double end_seconds = -1.0;
    std::string text;     // 展示用纯文本
    std::string raw_text; // 原始文本
};

class SubtitleAnalyzer {
public:
    void Reset(const SubtitleOptions& options = SubtitleOptions{});

    // find_stream_info 之后调用：登记字幕流（含语言 tag / handler name / disposition）
    void RegisterStreams(const AVFormatContext* fmt);

    // 逐包回调：文本字幕直接解析包载荷；图形字幕只累计包数与字节数
    void OnPacket(const AVPacket* pkt, const AVStream* stream);

    void Finish(double media_duration_seconds = 0.0);

    const model::SubtitleAnalysisResult& result() const {
        return result_;
    }
    const SubtitleOptions& options() const {
        return options_;
    }

    // ---- 纯逻辑（不依赖 FFmpeg，单测直接调用）----

    static model::SubtitleFormat FormatFromCodecId(int codec_id);
    static model::SubtitleKind KindFromFormat(model::SubtitleFormat format);

    // "00:00:01,000" / "0:01.500" / "1:02:03.250" -> 秒
    static bool ParseTimestamp(const std::string& text, double& seconds);

    static bool ParseSrtText(const std::string& text, std::vector<ParsedSubtitleCue>& out);
    static bool ParseWebVttText(const std::string& text, std::vector<ParsedSubtitleCue>& out);
    static bool ParseAssText(const std::string& text, std::vector<ParsedSubtitleCue>& out);
    static bool ParseAssDialogueLine(const std::string& line, ParsedSubtitleCue& out);

    // 去掉 ASS 的 {覆盖指令} 与 \N 换行
    static std::string StripAssOverrides(const std::string& in);
    // 去掉 WebVTT 的 <v Speaker> / <i> / <00:00:01.000> 之类标签
    static std::string StripVttTags(const std::string& in);

    // 从一个字幕包里解析 cue（按 codec 自动分派）；失败时退化为"整包即文本"
    static bool ParsePacketPayload(int codec_id, const uint8_t* data, size_t size, double pts_seconds,
                                   double duration_seconds, ParsedSubtitleCue& out);

    // CEA-608：把字节对里的可见字符抽出来（控制码对跳过）。
    // 只做基础北美字符集映射，802/708 的扩展字符集不处理。
    // ⚠️ 这是 D2 之前的旧口径，保留给"整包即文本"的兜底路径与既有单测；
    //    正式的隐藏字幕分析走 CeaCaptionDecoder（有状态机，能分通道/服务）。
    static std::string DecodeCea608Field(const uint8_t* data, size_t size);

    // 校验一组同流 cue（按出现顺序）。problem 追加到 issues，并回填 cue.has_issue。
    static void ValidateCues(std::vector<model::SubtitleCue>& cues, std::vector<model::SubtitleIssue>& issues,
                             const SubtitleOptions& options, double media_duration_seconds);

    // 可见字符数（按 UTF-8 码点算，不算空白）
    static size_t CountVisibleChars(const std::string& text);

private:
    // 每条隐藏字幕流一个解码器：多语言节目里 c608 与 c708 可能是两条独立轨，
    // 共用一份状态机必然把两条流的文本搅在一起。
    struct CaptionSlot {
        int stream_index = -1;
        CeaCaptionDecoder decoder;
    };
    CaptionSlot* FindOrCreateCaptionSlot(int stream_index);
    // 把 decoder 产出的 cue 转成通用 SubtitleCue，挂到流上（含通道/服务归属）
    void AppendCaptionCues(CaptionSlot& slot, double end_seconds);

    SubtitleOptions options_;
    model::SubtitleAnalysisResult result_;
    std::vector<CaptionSlot> caption_slots_;
};

} // namespace videoeye
