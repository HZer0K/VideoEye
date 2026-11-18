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
struct AVCodecContext;
struct AVSubtitle;

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
    SubtitleAnalyzer() = default;
    ~SubtitleAnalyzer();
    SubtitleAnalyzer(const SubtitleAnalyzer&) = delete;
    SubtitleAnalyzer& operator=(const SubtitleAnalyzer&) = delete;

    void Reset(const SubtitleOptions& options = SubtitleOptions{});

    // 本地源路径。只有"纯字幕文件"（无音视频流）才会用到：
    // 这类文件的 demuxer 会丢掉空事件/畸形事件，必须整文件重解析才能检出。
    // 网络源传空串即可。
    void SetSourcePath(std::string path);

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
    // MicroDVD：{起始帧}{结束帧}文本，帧号按 frame_rate 换算；{n}{n} 空文本是"清屏"，不算 cue
    static bool ParseMicroDvdText(const std::string& text, double frame_rate, std::vector<ParsedSubtitleCue>& out);
    // 文本里有没有合法 UTF-8 编码（用来识别 GBK/CP1251 等旧编码字幕）
    static bool IsValidUtf8(const std::string& text);

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
    // skip_garbled_check：流本身已确认不是 UTF-8 时不再逐条报乱码 —— 真正的修复动作是
    // 整体转码，50 条 cue 逐条报"含 U+FFFD"只会把编码问题变成噪声。
    static void ValidateCues(std::vector<model::SubtitleCue>& cues, std::vector<model::SubtitleIssue>& issues,
                             const SubtitleOptions& options, double media_duration_seconds,
                             bool skip_garbled_check = false);

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

    // 非原生格式（MicroDVD / SAMI / RealText / STL ...）走 FFmpeg 字幕解码器：
    // 各 demuxer 已经把时间轴写进 PTS，解码器负责把载荷还原成干净文本。
    // 自己按字节猜格式既不可靠，也覆盖不了 STL 这种二进制容器。
    struct TextDecoderSlot {
        int stream_index = -1;
        AVCodecContext* context = nullptr;
    };
    AVCodecContext* EnsureTextDecoder(const AVStream* stream);
    void AppendDecodedSubtitle(const AVSubtitle& subtitle, double pts_seconds, double duration_seconds,
                               model::SubtitleStreamInfo& info);
    void ReleaseTextDecoders();

    // ---- 视频内嵌 CC（A53） ----
    // MPEG-2 的 user_data 与 H.264/HEVC 的 SEI 里都可能带 A53 cc_data，这类 CC
    // 不在独立字幕轨里，只能从视频包中提取后喂给同一套 CeaCaptionDecoder。
    void HandleVideoPacket(const AVPacket* pkt, const AVStream* stream);
    model::SubtitleStreamInfo* EnsureEmbeddedCaptionStream(const AVStream* stream);
    void PushEmbeddedTriplets(int stream_index, const AVStream* stream, const uint8_t* data, size_t size, double ts);

    // 纯字幕文件的整文件重解析（只对本地文件 + 无音视频流 + 单条文本字幕流生效）
    bool TryParseWholeFile(const AVFormatContext* fmt);
    // packet_ts >= 0 时顺带记录 cue 时间与包 PTS 的偏差，供时间轴偏移校验使用
    void AppendParsedCue(const ParsedSubtitleCue& parsed, model::SubtitleStreamInfo& info, double packet_ts = -1.0);

    SubtitleOptions options_;
    model::SubtitleAnalysisResult result_;
    std::vector<CaptionSlot> caption_slots_;
    std::vector<TextDecoderSlot> text_decoders_;

    // 视频流的内嵌 CC 提取状态（按视频流索引）
    struct EmbeddedVideoSlot {
        int stream_index = -1;
        int codec_id = 0;
        int nal_prefix_bytes = 0;    // MP4 的 AVC/HEVC 是长度前缀；0 = Annex B
        bool top_field_first = true; // MPEG-2 picture coding extension 里的标志
    };
    std::vector<EmbeddedVideoSlot> video_slots_;

    // 文本流 cue 时间与包 PTS 的最小偏差：只要有一条 cue 的时间对得上 PTS，
    // 就说明时间轴没有整体平移；最小偏差都很明显才是容器偏移。
    struct TimelineProbe {
        int stream_index = -1;
        bool has_sample = false;
        double min_abs_delta = 0.0;
        double sample_delta = 0.0;
    };
    std::vector<TimelineProbe> timeline_probes_;
    // 已确认不是合法 UTF-8 的流：乱码检查交给流级 non_utf8 规则，不再逐条报
    std::vector<int> non_utf8_streams_;

    std::string source_path_;
    // 已整文件解析完毕的流：逐包阶段跳过，避免同一批 cue 入两遍
    std::vector<int> whole_file_streams_;
    // 明确无法解析的流：不再尝试逐包解析，避免把二进制载荷当文本
    std::vector<int> unsupported_streams_;
};

} // namespace videoeye
