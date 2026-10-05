#include "core/reporting/QcReportExporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "infrastructure/serialization/Json.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace reporting {

// JsonValue 还住在历史命名空间 videoeye::utils —— utils/ 目录已经拆进
// infrastructure/，命名空间没跟着改（全仓 200+ 处引用，单独一轮做）。
using videoeye::JsonValue;

namespace {

// JSON schema 的版本号。只要字段语义有破坏性变化就加一，
// CI 侧据此决定要不要兼容旧结构。
constexpr int kReportSchemaVersion = 1;

const char* kRowGroupSeparator = "----------------------------------------";

std::string Fixed(double value, int precision) {
    if (!std::isfinite(value)) return std::string();
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(precision) + "f").c_str(), value);
    return std::string(buf);
}

std::string Int64Text(long long value) { return std::to_string(value); }

JsonValue JNumber(double value) { return JsonValue(value); }

JsonValue JText(const std::string& text) { return JsonValue(text); }

bool WriteUtf8File(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("无法写入报告文件: " + path);
        return false;
    }
    file << content;
    return file.good();
}

std::string Html(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '\"': out += "&quot;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

// CSV 转义（RFC 4180）：字段整体用双引号包住，内部的双引号翻倍。
//
// 另外对以 = + - @ 开头的字段前置一个单引号：Excel / LibreOffice / Google Sheets
// 会把这样的单元格当成公式执行（=cmd|'/c ...、@SUM(...) 等），导出的报告会被
// 打开它的表格软件执行的载荷，前置 ' 之后它只是普通文本。
std::string CsvField(const std::string& text) {
    std::string out;
    if (!text.empty()) {
        const char first = text.front();
        if (first == '=' || first == '+' || first == '-' || first == '@')
            out += '\'';
    }
    out += '"';
    for (char c : text) {
        if (c == '"') out += "\"\"";
        else out += c;   // 逗号/换行/CR 本来就落在引号里面，属于 RFC 4180 的合法转义
    }
    out += '"';
    return out;
}

// CSV 里换行会把一行拆成两行，而 detail/suggestion 里有换行（如 FFmpeg 命令行示例）。
std::string SingleLine(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

JsonValue BuildIssueObject(const model::DiagnosticIssue& issue) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("rule_id", JText(issue.rule_id));
    object.Set("title", JText(issue.title));
    object.Set("severity", JText(SeverityCode(issue.severity)));
    object.Set("severity_text", JText(issue.SeverityText()));
    object.Set("category", JText(CategoryCode(issue.category)));
    object.Set("category_text", JText(issue.CategoryText()));
    object.Set("detail", JText(issue.detail));
    object.Set("suggestion", JText(issue.suggestion));
    object.Set("range", JText(issue.range.ToString()));
    object.Set("stream_index", JNumber(static_cast<double>(issue.stream_index)));
    object.Set("metric_value", JNumber(issue.metric_value));
    object.Set("threshold", JNumber(issue.threshold));
    object.Set("occurrence_count", JNumber(static_cast<double>(issue.occurrence_count)));
    return object;
}

JsonValue BuildStreamObject(const model::StreamDigest& stream) {
    JsonValue object = JsonValue::MakeObject();
    const char* type = stream.IsVideo() ? "video" : (stream.IsAudio() ? "audio" : "other");
    object.Set("index", JNumber(static_cast<double>(stream.index)));
    object.Set("type", JText(type));
    object.Set("codec_name", JText(stream.codec_name));
    object.Set("profile_name", JText(stream.profile_name));
    object.Set("width", JNumber(static_cast<double>(stream.width)));
    object.Set("height", JNumber(static_cast<double>(stream.height)));
    object.Set("fps", JNumber(stream.avg_fps));
    object.Set("sample_rate", JNumber(static_cast<double>(stream.sample_rate)));
    object.Set("channels", JNumber(static_cast<double>(stream.channels)));
    object.Set("bitrate_bps", JNumber(static_cast<double>(stream.bitrate_bps)));
    object.Set("packet_count", JNumber(static_cast<double>(stream.packet_count)));
    object.Set("byte_count", JNumber(static_cast<double>(stream.byte_count)));
    object.Set("start_seconds", JNumber(stream.start_seconds));
    object.Set("duration_seconds", JNumber(stream.duration_seconds));
    return object;
}

JsonValue BuildRuleObject(const model::QcRule& rule) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("id", JText(rule.id));
    object.Set("name", JText(rule.name));
    object.Set("category", JText(CategoryCode(rule.category)));
    object.Set("severity", JText(SeverityCode(rule.severity)));
    object.Set("op", JText(std::string([&rule]() {
        switch (rule.op) {
            case model::QcRuleOp::MaxExceeded: return "max_exceeded";
            case model::QcRuleOp::MinBelow:    return "min_below";
            case model::QcRuleOp::NonZero:     return "non_zero";
        }
        return "unknown";
    }())));
    object.Set("threshold", JNumber(rule.threshold));
    object.Set("unit", JText(rule.unit));
    object.Set("enabled", JsonValue(rule.enabled));
    object.Set("description", JText(rule.description));
    object.Set("suggestion", JText(rule.suggestion));
    return object;
}

JsonValue BuildMetricsObject(const model::AnalysisResult& result) {
    JsonValue metrics = JsonValue::MakeObject();

    if (const model::StreamDigest* video = result.FirstVideoStream()) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("codec_name", JText(video->codec_name));
        node.Set("profile_name", JText(video->profile_name));
        node.Set("width", JNumber(static_cast<double>(video->width)));
        node.Set("height", JNumber(static_cast<double>(video->height)));
        node.Set("fps", JNumber(video->avg_fps));
        node.Set("bitrate_bps", JNumber(static_cast<double>(video->bitrate_bps)));
        node.Set("frame_count", JNumber(static_cast<double>(video->frame_count)));
        node.Set("key_frame_count", JNumber(static_cast<double>(video->key_frame_count)));
        metrics.Set("video", node);
    }
    if (const model::StreamDigest* audio = result.FirstAudioStream()) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("codec_name", JText(audio->codec_name));
        node.Set("sample_rate", JNumber(static_cast<double>(audio->sample_rate)));
        node.Set("channels", JNumber(static_cast<double>(audio->channels)));
        node.Set("bitrate_bps", JNumber(static_cast<double>(audio->bitrate_bps)));
        node.Set("duration_seconds", JNumber(audio->duration_seconds));
        metrics.Set("audio", node);
    }

    JsonValue bitrate = JsonValue::MakeObject();
    bitrate.Set("avg_kbps", JNumber(result.bitrate_gop.avg_bitrate_kbps));
    bitrate.Set("peak_kbps", JNumber(result.bitrate_gop.peak_bitrate_kbps));
    bitrate.Set("min_kbps", JNumber(result.bitrate_gop.min_bitrate_kbps));
    bitrate.Set("p95_kbps", JNumber(result.bitrate_gop.p95_bitrate_kbps));
    bitrate.Set("median_kbps", JNumber(result.bitrate_gop.median_bitrate_kbps));
    bitrate.Set("peak_to_mean_ratio", JNumber(result.bitrate_gop.peak_to_mean_ratio));
    bitrate.Set("window_seconds", JNumber(result.bitrate_gop.window_seconds));
    metrics.Set("bitrate", bitrate);

    JsonValue gop = JsonValue::MakeObject();
    gop.Set("count", JNumber(static_cast<double>(result.bitrate_gop.gops.size())));
    gop.Set("mean_seconds", JNumber(result.bitrate_gop.gop_duration_mean));
    gop.Set("max_seconds", JNumber(result.bitrate_gop.gop_duration_max));
    gop.Set("stddev_seconds", JNumber(result.bitrate_gop.gop_duration_stddev));
    gop.Set("min_frames", JNumber(static_cast<double>(result.bitrate_gop.gop_frames_min)));
    gop.Set("max_frames", JNumber(static_cast<double>(result.bitrate_gop.gop_frames_max)));
    gop.Set("key_interval_mean_seconds", JNumber(result.bitrate_gop.key_interval_mean));
    gop.Set("key_interval_irregularity", JNumber(result.bitrate_gop.key_interval_irregularity));
    gop.Set("long_gop_count", JNumber(static_cast<double>(result.bitrate_gop.long_gop_count)));
    gop.Set("frame_types_known", JsonValue(result.bitrate_gop.frame_types_known));
    gop.Set("i_frame_count", JNumber(static_cast<double>(result.bitrate_gop.i_frame_count)));
    gop.Set("p_frame_count", JNumber(static_cast<double>(result.bitrate_gop.p_frame_count)));
    gop.Set("b_frame_count", JNumber(static_cast<double>(result.bitrate_gop.b_frame_count)));
    metrics.Set("gop", gop);

    if (result.audio_qc.analyzed) {
        JsonValue loudness = JsonValue::MakeObject();
        loudness.Set("integrated_lufs", JNumber(result.audio_qc.integrated_lufs));
        loudness.Set("true_peak_dbtp", JNumber(result.audio_qc.true_peak_dbtp));
        loudness.Set("loudness_range_lu", JNumber(result.audio_qc.loudness_range_lu));
        loudness.Set("short_term_max_lufs", JNumber(result.audio_qc.short_term_max_lufs));
        loudness.Set("momentary_max_lufs", JNumber(result.audio_qc.momentary_max_lufs));
        loudness.Set("sample_peak_dbfs", JNumber(result.audio_qc.sample_peak_dbfs));
        loudness.Set("rms_dbfs", JNumber(result.audio_qc.rms_dbfs));
        loudness.Set("silence_ratio", JNumber(result.audio_qc.silence_ratio));
        loudness.Set("longest_silence_seconds", JNumber(result.audio_qc.longest_silence_seconds));
        loudness.Set("clipping_sample_count",
                     JNumber(static_cast<double>(result.audio_qc.clipping_sample_count)));
        loudness.Set("channel_layout", JText(result.audio_qc.metadata.channel_layout));
        metrics.Set("loudness", loudness);
    }

    if (result.color_hdr.analyzed) {
        JsonValue color = JsonValue::MakeObject();
        const auto& info = result.color_hdr.color;
        color.Set("primaries", JText(info.primaries_name));
        color.Set("transfer", JText(info.transfer_name));
        color.Set("matrix", JText(info.matrix_name));
        color.Set("range", JText(info.range_name));
        color.Set("pixel_format", JText(info.pixel_format.name));
        color.Set("chroma_subsampling", JText(info.pixel_format.chroma_subsampling));
        color.Set("bit_depth", JNumber(static_cast<double>(info.pixel_format.bit_depth)));
        color.Set("level", JNumber(static_cast<double>(info.level)));
        color.Set("hdr_format", JText(result.color_hdr.hdr.format_name));
        color.Set("max_cll", JNumber(static_cast<double>(result.color_hdr.hdr.content_light.max_cll)));
        color.Set("max_fall",
                  JNumber(static_cast<double>(result.color_hdr.hdr.content_light.max_fall)));
        color.Set("dolby_vision_profile",
                  JNumber(static_cast<double>(result.color_hdr.hdr.dolby_vision.profile)));
        metrics.Set("color_hdr", color);
    }

    if (result.streaming_analyzed) {
        JsonValue streaming = JsonValue::MakeObject();
        streaming.Set("manifest_kind", JText(result.streaming_package.manifest_path.empty() ? "" : "local"));
        streaming.Set("variant_count",
                      JNumber(static_cast<double>(result.streaming_package.variants.size())));
        metrics.Set("streaming", streaming);
    }

    return metrics;
}

// ===========================================================================
// 极简 PDF 写入器（A4 / Helvetica / WinAnsi）
//
// 为什么不用 Qt 的打印模块：Qt6 PrintSupport 不在当前依赖里（只有 Widgets），
// 为一个"偶尔导出 PDF"的功能加一个 Qt 模块不划算。这里只做最朴素的多页文本 PDF：
// 每个字符先解码成 codepoint，落在 WinAnsi 之外的（中文、日文……）改由第二套字体
// （Identity-H + ToUnicode）按 UTF-16BE 十六进制画出来，不再一律替换成 '?'。
// ===========================================================================
class MiniPdfWriter {
public:
    // F1/F2: WinAnsi 的单字节字体（ASCII + Latin-1）。
    // F3: 中文用的 Type0/Identity-H 字体，喝 ToUnicode CMap 一起吃十六进制串。
    static constexpr int kFontWinAnsi1 = 3;
    static constexpr int kFontWinAnsi2 = 4;

    static constexpr double kPageWidth = 595.0;    // A4 纵向（pt）
    static constexpr double kPageHeight = 842.0;
    static constexpr double kMarginX = 48.0;
    static constexpr double kMarginTop = 760.0;
    static constexpr double kLeading = 14.0;
    static constexpr int kMaxLinesPerPage = 50;

    void AddLine(const std::string& text) { lines_.push_back(text); }

    bool Build(std::string& out) {
        std::vector<std::vector<std::string>> pages;
        std::vector<std::string> current;
        for (const auto& line : lines_) {
            current.push_back(line);
            if (current.size() >= static_cast<std::size_t>(kMaxLinesPerPage)) {
                pages.push_back(current);
                current.clear();
            }
        }
        if (!current.empty() || pages.empty()) pages.push_back(current);

        // ⚠️ 对象编号必须在 kids 落位**之前**算完：以前是等全部对象都填好才
        // `objects.insert(begin()+1, kids)`，插入把后面所有的下标整体顶掉一位，
        // 而 /Contents 与 /Kids 里已经按老编号写死了 —— 两边差一号，PDF 全废。
        // 这里让 kids 一开始就占住下标 1（对象 2），此后"下标 i 恒等于对象 i+1"，
        // 编号随 push 顺序自然算出，不再有插入错位。
        std::vector<std::string> objects;  // objects[0] 对应 PDF 对象 1
        objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");  // 1: Catalog
        objects.push_back(std::string());                        // 2: Pages（Kids，最后填）
        objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                          "/Encoding /WinAnsiEncoding >>");            // 3: F1
        objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
                          "/Encoding /WinAnsiEncoding >>");            // 4: F2

        // 5: F3（中文, Identity-H）与 6: ToUnicode CMap。两个都要等页面内容扫完才填得出来
        // （CMap 得知道这整份报告里都出现过哪些非 Latin 字符），所以先占位、编号先定。
        const int font_object = static_cast<int>(objects.size()) + 1;
        const int cmap_object = font_object + 1;
        objects.push_back(std::string());  // 5: F3
        objects.push_back(std::string());  // 6: ToUnicode

        const int first_content = static_cast<int>(objects.size()) + 1;
        for (const auto& page : pages) {
            const double top = kMarginTop;
            std::ostringstream stream;
            stream << "BT /F1 10 Tf " << kLeading << " TL " << kMarginX << " " << top
                   << " Td\n";
            for (const auto& line : page) {
                // 一行切成若干 run：Latin 段用 F1 画，中文段用 F3 画。
                // 不能整行二选一 —— 报告里"中文问题标题 + 英文 rule_id"这种行很常见。
                for (const Run& run : MakeRuns(line, codepoints_)) {
                    if (run.IsHex())
                        stream << "/F3 10 Tf <" << run.hex << "> Tj\n";
                    else
                        stream << "/F1 10 Tf (" << run.text << ") Tj\n";
                }
            }
            stream << "ET";
            const std::string content = stream.str();
            objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
                              content + "\nendstream");
        }

        const int first_page = first_content + static_cast<int>(pages.size());
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const int resource_object = first_content + static_cast<int>(i);
            objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                              Fixed(kPageWidth, 0) + " " + Fixed(kPageHeight, 0) + "] " +
                              "/Resources << /Font << /F1 " + std::to_string(kFontWinAnsi1) +
                              " 0 R /F2 " + std::to_string(kFontWinAnsi2) + " 0 R /F3 " +
                              std::to_string(font_object) + " 0 R >> >> " +
                              "/Contents " + std::to_string(resource_object) + " 0 R >>");
        }

        objects[font_object - 1] =
            "<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light /Encoding /Identity-H "
            "/DescendantFonts [ << /Type /Font /Subtype /CIDFontType2 /BaseFont /STSong-Light "
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> /DW 1000 "
            "/CIDToGIDMap /Identity >> ] /ToUnicode " + std::to_string(cmap_object) + " 0 R >>";
        objects[cmap_object - 1] = BuildToUnicodeCMap();

        // 2: Pages（Kids 填回占位槽，下标 1 -> 对象 2，编号不受影响）
        std::string kids = "<< /Type /Pages /Count " + std::to_string(pages.size()) + " /Kids [";
        for (std::size_t i = 0; i < pages.size(); ++i) {
            kids += " " + std::to_string(first_page + static_cast<int>(i)) + " 0 R";
        }
        kids += " ] >>";
        objects[1] = kids;

        std::string body = "%PDF-1.4\n";
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            offsets.push_back(body.size());
            body += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const std::size_t xref_offset = body.size();

        body += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
        body += "0000000000 65535 f \n";
        for (std::size_t offset : offsets) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%010zu", offset);
            body += std::string(buffer) + " 00000 n \n";
        }
        char trailer[64];
        std::snprintf(trailer, sizeof(trailer), "%zu", xref_offset);
        body += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
                " /Root 1 0 R >>\nstartxref\n" + std::string(trailer) + "\n%%EOF\n";

        out = body;
        return true;
    }

    // 有没有字会被丢掉（供调用方提示用户）
    static bool HasLossyText(const std::string& text);

private:
    // 一行文本切成的一段连续字节：
    //   text —— WinAnsi 单字节段，交给 F1 直接画（括号/反斜杠已转义）
    //   hex  —— UTF-16BE 十六进制段（不含尖括号），交给 F3 画
    struct Run {
        std::string text;
        std::string hex;
        bool IsHex() const { return !hex.empty(); }
    };

    // 逐段（run）生成器：ASCII / Latin-1 走 WinAnsi，其它（中文、日文……）走 UTF-16BE。
    //
    // 以前这里是"整行统一处理"，落到 WinAnsi 之外的字符一律写成 '?'，中文报告全是问号。
    // 现在按 run 切：一行里中英混排很常见，英文部分照旧走 F1，中文部分单独成段走 F3，
    // 两边都不丢。
    //
    // 非 Latin 的 codepoint 顺手记进 codepoints —— ToUnicode CMap 要用。
    static std::vector<Run> MakeRuns(const std::string& text, std::set<unsigned int>& codepoints) {
        std::vector<Run> runs;
        Run run;
        for (std::size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            unsigned int codepoint = c;
            std::size_t advance = 1;
            if (c >= 0xF0 && i + 3 < text.size()) {
                codepoint = ((c & 0x07u) << 18) |
                            ((static_cast<unsigned int>(text[i + 1]) & 0x3Fu) << 12) |
                            ((static_cast<unsigned int>(text[i + 2]) & 0x3Fu) << 6) |
                            (static_cast<unsigned int>(text[i + 3]) & 0x3Fu);
                advance = 4;
            } else if (c >= 0xE0 && i + 2 < text.size()) {
                codepoint = ((c & 0x0Fu) << 12) |
                            ((static_cast<unsigned int>(text[i + 1]) & 0x3Fu) << 6) |
                            ((static_cast<unsigned int>(text[i + 2]) & 0x3Fu));
                advance = 3;
            } else if (c >= 0xC0 && i + 1 < text.size()) {
                codepoint = ((c & 0x1Fu) << 6) | (static_cast<unsigned int>(text[i + 1]) & 0x3Fu);
                advance = 2;
            }

            const bool latin = (codepoint < 0x100);  // WinAnsi 覆盖得住
            if (latin) {
                if (run.IsHex()) { runs.push_back(run); run = Run(); }
                switch (codepoint) {
                    case '(': run.text += "\\("; break;
                    case ')': run.text += "\\)"; break;
                    case '\\': run.text += "\\\\"; break;
                    default:  run.text += static_cast<char>(codepoint); break;
                }
            } else {
                if (!run.IsHex()) { runs.push_back(run); run = Run(); }
                AppendHexPair(run.hex, 0xFEFF);  // BOM：让阅读器认出后面是 UCS-2 码点
                AppendHexPair(run.hex, codepoint);
                codepoints.insert(codepoint);
            }
            i += advance;
        }
        if (!run.text.empty() || !run.hex.empty()) runs.push_back(run);
        return runs;
    }

    // 追加一个 UTF-16BE 码元（4 位十六进制）
    static void AppendHexPair(std::string& out, unsigned int codepoint) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04X", codepoint & 0xFFFFu);
        out += buf;
    }

    // ToUnicode CMap：把 F3 用到的每个码点映射回它自己（Identity-H 下码点就等于码元值）。
    // 少了它，阅读器只能按字形索引认字，复制/搜索中文全文全是乱码。
    std::string BuildToUnicodeCMap() const {
        std::ostringstream cmap;
        cmap << "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
             << "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
             << "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
             << "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        cmap << codepoints_.size() << " beginbfchar\n";
        char buf[24];
        for (const unsigned int codepoint : codepoints_) {
            std::snprintf(buf, sizeof(buf), "<%04X> <%04X>\n", codepoint & 0xFFFFu,
                          codepoint & 0xFFFFu);
            cmap << buf;
        }
        cmap << "endbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend";
        return "<< /Length " + std::to_string(cmap.str().size()) + " >>\nstream\n" +
               cmap.str() + "\nendstream";
    }

    std::vector<std::string> lines_;
    std::set<unsigned int> codepoints_;  // 整份报告里出现过的非 Latin 码点
};

bool MiniPdfWriter::HasLossyText(const std::string& text) {
    for (unsigned char c : text) {
        if (c >= 0x80) return true;  // 粗略判断：UTF-8 多字节序列一定有 >=0x80 的字节
    }
    return false;
}

}  // namespace

const char* SeverityCode(model::IssueSeverity severity) {
    switch (severity) {
        case model::IssueSeverity::Critical: return "critical";
        case model::IssueSeverity::Error:    return "error";
        case model::IssueSeverity::Warning:  return "warning";
        case model::IssueSeverity::Info:     return "info";
    }
    return "info";
}

const char* CategoryCode(model::IssueCategory category) {
    switch (category) {
        case model::IssueCategory::Container: return "container";
        case model::IssueCategory::Video:     return "video";
        case model::IssueCategory::Audio:     return "audio";
        case model::IssueCategory::Timing:    return "timing";
        case model::IssueCategory::Bitrate:   return "bitrate";
        case model::IssueCategory::Gop:       return "gop";
        case model::IssueCategory::Metadata:  return "metadata";
        case model::IssueCategory::ColorHdr:  return "color_hdr";
        case model::IssueCategory::Other:     return "other";
    }
    return "other";
}

std::string QcReportOutputPath(const std::string& directory, const std::string& base_name,
                               qc::QcReportFormat format) {
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += base_name;
    path += qc::QcReportExtension(format);
    return path;
}

// ===========================================================================
// JSON
// ===========================================================================

std::string QcReportExporter::BuildJson(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    JsonValue root = JsonValue::MakeObject();
    root.Set("schema", JText("videoeye.qc-report"));
    root.Set("schema_version", JNumber(static_cast<double>(kReportSchemaVersion)));
    root.Set("generated_at", JText(report.generated_at));

    JsonValue tool = JsonValue::MakeObject();
    tool.Set("name", JText("VideoEye"));
    tool.Set("version", JText("2.0.0"));
    root.Set("tool", tool);

    JsonValue profile = JsonValue::MakeObject();
    profile.Set("id", JText(bundle.profile_id));
    profile.Set("name", JText(bundle.profile_name));
    root.Set("profile", profile);

    JsonValue file = JsonValue::MakeObject();
    file.Set("path", JText(report.file_path));
    file.Set("name", JText(report.file_name));
    file.Set("container_format", JText(report.container_format));
    file.Set("size_bytes", JNumber(static_cast<double>(report.file_size_bytes)));
    file.Set("duration_seconds", JNumber(report.duration_seconds));
    file.Set("overall_bitrate_bps", JNumber(static_cast<double>(report.overall_bitrate_bps)));
    file.Set("video_stream_count", JNumber(static_cast<double>(report.video_stream_count)));
    file.Set("audio_stream_count", JNumber(static_cast<double>(report.audio_stream_count)));
    root.Set("file", file);

    JsonValue summary = JsonValue::MakeObject();
    summary.Set("score", JNumber(report.score));
    summary.Set("verdict", JText(report.verdict));
    summary.Set("completed", JsonValue(report.completed));
    summary.Set("analysis_elapsed_ms", JNumber(run.elapsed_ms));
    if (!run.error.empty()) summary.Set("error", JText(run.error));
    JsonValue counts = JsonValue::MakeObject();
    counts.Set("critical", JNumber(static_cast<double>(
                               report.CountBySeverity(model::IssueSeverity::Critical))));
    counts.Set("error", JNumber(static_cast<double>(
                            report.CountBySeverity(model::IssueSeverity::Error))));
    counts.Set("warning", JNumber(static_cast<double>(
                              report.CountBySeverity(model::IssueSeverity::Warning))));
    counts.Set("info", JNumber(static_cast<double>(
                           report.CountBySeverity(model::IssueSeverity::Info))));
    summary.Set("issue_counts", counts);
    root.Set("summary", summary);

    JsonValue issues = JsonValue::MakeArray();
    for (const auto& issue : report.issues) issues.PushBack(BuildIssueObject(issue));
    root.Set("issues", issues);

    root.Set("metrics", BuildMetricsObject(run.analysis));

    JsonValue streams = JsonValue::MakeArray();
    for (const auto& stream : run.analysis.streams) streams.PushBack(BuildStreamObject(stream));
    root.Set("streams", streams);

    JsonValue rules = JsonValue::MakeArray();
    for (const auto& rule : report.rules) rules.PushBack(BuildRuleObject(rule));
    root.Set("rules", rules);

    if (bundle.has_comparison) {
        const qc::QcComparison& comparison = bundle.comparison;
        JsonValue node = JsonValue::MakeObject();
        node.Set("left_path", JText(comparison.left_path));
        node.Set("right_path", JText(comparison.right_path));
        node.Set("different_count", JNumber(static_cast<double>(comparison.DifferentCount())));
        JsonValue rows = JsonValue::MakeArray();
        for (const auto& row : comparison.rows) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("group", JText(row.group));
            item.Set("field", JText(row.field));
            item.Set("left", JText(row.left));
            item.Set("right", JText(row.right));
            item.Set("diff", JText(std::string([&row]() {
                switch (row.diff) {
                    case qc::QcFieldDiff::Same:        return "same";
                    case qc::QcFieldDiff::Different:   return "different";
                    case qc::QcFieldDiff::OnlyLeft:    return "only_left";
                    case qc::QcFieldDiff::OnlyRight:   return "only_right";
                    case qc::QcFieldDiff::Unavailable: return "unavailable";
                }
                return "unavailable";
            }())));
            if (row.numeric) {
                item.Set("left_value", JNumber(row.left_value));
                item.Set("right_value", JNumber(row.right_value));
                item.Set("delta", JNumber(row.delta));
            }
            item.Set("unit", JText(row.unit));
            rows.PushBack(item);
        }
        node.Set("rows", rows);
        root.Set("comparison", node);
    }

    return root.ToPrettyString(2) + "\n";
}

bool QcReportExporter::ExportJson(const std::string& path, const QcExportBundle& bundle) {
    return WriteUtf8File(path, BuildJson(bundle));
}

// ===========================================================================
// CSV —— 每个 issue 一行
// ===========================================================================

bool QcReportExporter::ExportCsv(const std::string& path, const QcExportBundle& bundle) {
    const model::QcReport& report = bundle.run.report;

    std::ostringstream body;
    body << "\xEF\xBB\xBF";  // UTF-8 BOM，Excel 直接双击打开才不会乱码
    body << "文件,规则ID,严重度,类别,问题,位置,流序号,实测值,阈值,出现次数,说明,建议\n";
    for (const auto& issue : report.issues) {
        body << CsvField(report.file_name) << ","
             << CsvField(issue.rule_id) << ","
             << CsvField(SeverityCode(issue.severity)) << ","
             << CsvField(CategoryCode(issue.category)) << ","
             << CsvField(issue.title) << ","
             << CsvField(issue.range.ToString()) << ","
             << issue.stream_index << ","
             << Fixed(issue.metric_value, 4) << ","
             << Fixed(issue.threshold, 4) << ","
             << issue.occurrence_count << ","
             << CsvField(SingleLine(issue.detail)) << ","
             << CsvField(SingleLine(issue.suggestion)) << "\n";
    }
    return WriteUtf8File(path, body.str());
}

// ===========================================================================
// HTML
// ===========================================================================

std::string QcReportExporter::BuildHtml(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    auto severity_class = [](model::IssueSeverity severity) {
        switch (severity) {
            case model::IssueSeverity::Critical: return "critical";
            case model::IssueSeverity::Error:    return "error";
            case model::IssueSeverity::Warning:  return "warning";
            case model::IssueSeverity::Info:     return "info";
        }
        return "info";
    };

    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
        << "  <meta charset=\"UTF-8\">\n  <title>VideoEye QC 报告 - "
        << Html(report.file_name) << "</title>\n"
        << "  <style>\n"
        << "    body { font-family: 'Microsoft YaHei', Arial, sans-serif; margin: 24px; color: #23272e; }\n"
        << "    h1 { font-size: 22px; margin-bottom: 4px; }\n"
        << "    h2 { font-size: 17px; margin-top: 26px; border-left: 4px solid #2f6fed; padding-left: 8px; }\n"
        << "    table { border-collapse: collapse; width: 100%; margin-top: 10px; font-size: 13px; }\n"
        << "    th, td { border: 1px solid #d8dee6; padding: 6px 8px; text-align: left; vertical-align: top; }\n"
        << "    th { background: #f2f4f7; }\n"
        << "    .score { font-size: 30px; font-weight: bold; }\n"
        << "    .pass { color: #1b7f3b; } .warn { color: #b26a00; } .fail { color: #b3261e; }\n"
        << "    .critical td:first-child { color: #b3261e; font-weight: bold; }\n"
        << "    .error td:first-child { color: #b3261e; }\n"
        << "    .warning td:first-child { color: #b26a00; }\n"
        << "    .info td:first-child { color: #1565c0; }\n"
        << "    .meta { color: #6b7280; font-size: 12px; }\n"
        << "  </style>\n</head>\n<body>\n";

    const std::string verdict_class =
        (report.score >= 90.0) ? "pass" : ((report.score >= 70.0) ? "warn" : "fail");

    out << "  <h1>VideoEye QC 报告</h1>\n";
    out << "  <p class=\"meta\">模板: " << Html(bundle.profile_name) << " ("
        << Html(bundle.profile_id) << ") ｜ 生成时间: " << Html(report.generated_at)
        << " ｜ 分析耗时: " << Fixed(run.elapsed_ms, 0) << " ms"
        << (report.completed ? (report.partial ? " ｜ <b>抽样完成·非全量</b>" : "")
                             : " ｜ <b>分析未跑完</b>") << "</p>\n";

    out << "  <h2>总体结论</h2>\n";
    out << "  <p class=\"score " << verdict_class << "\">" << Fixed(report.score, 1)
        << " / 100 — " << Html(report.verdict) << "</p>\n";
    out << "  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
    out << "    <tr><td>文件</td><td>" << Html(report.file_path) << "</td></tr>\n";
    out << "    <tr><td>容器格式</td><td>" << Html(report.container_format) << "</td></tr>\n";
    out << "    <tr><td>时长</td><td>" << Fixed(report.duration_seconds, 3) << " s</td></tr>\n";
    out << "    <tr><td>文件大小</td><td>" << Int64Text(report.file_size_bytes) << " 字节</td></tr>\n";
    out << "    <tr><td>整体码率</td><td>" << Fixed(report.overall_bitrate_bps / 1000.0, 1)
        << " kbps</td></tr>\n";
    out << "    <tr><td>视频流 / 音频流</td><td>" << report.video_stream_count << " / "
        << report.audio_stream_count << "</td></tr>\n";
    out << "    <tr><td>致命 / 错误 / 警告 / 提示</td><td>"
        << report.CountBySeverity(model::IssueSeverity::Critical) << " / "
        << report.CountBySeverity(model::IssueSeverity::Error) << " / "
        << report.CountBySeverity(model::IssueSeverity::Warning) << " / "
        << report.CountBySeverity(model::IssueSeverity::Info) << "</td></tr>\n";
    if (!run.error.empty()) out << "    <tr><td>错误信息</td><td>" << Html(run.error) << "</td></tr>\n";
    out << "  </table>\n";

    out << "  <h2>问题清单</h2>\n";
    if (report.issues.empty()) {
        out << "  <p>未发现问题。</p>\n";
    } else {
        out << "  <table>\n    <tr><th>严重度</th><th>类别</th><th>规则</th><th>问题</th>"
               "<th>位置</th><th>说明</th><th>建议</th></tr>\n";
        for (const auto& issue : report.issues) {
            out << "    <tr class=\"" << severity_class(issue.severity) << "\">"
                << "<td>" << Html(issue.SeverityText()) << "</td>"
                << "<td>" << Html(issue.CategoryText()) << "</td>"
                << "<td>" << Html(issue.rule_id) << "</td>"
                << "<td>" << Html(issue.title) << "</td>"
                << "<td>" << Html(issue.range.ToString()) << "</td>"
                << "<td>" << Html(issue.detail) << "</td>"
                << "<td>" << Html(issue.suggestion) << "</td></tr>\n";
        }
        out << "  </table>\n";
    }

    out << "  <h2>流列表</h2>\n";
    out << "  <table>\n    <tr><th>#</th><th>类型</th><th>编码</th><th>Profile</th>"
           "<th>分辨率 / 采样率</th><th>码率</th><th>帧数 / 包数</th></tr>\n";
    for (const auto& stream : run.analysis.streams) {
        const char* type = stream.IsVideo() ? "视频" : (stream.IsAudio() ? "音频" : "其他");
        const std::string resolution = stream.IsVideo()
            ? (std::to_string(stream.width) + "x" + std::to_string(stream.height))
            : (std::to_string(stream.sample_rate) + " Hz / " +
               std::to_string(stream.channels) + " 声道");
        out << "    <tr><td>" << stream.index << "</td><td>" << type << "</td><td>"
            << Html(stream.codec_name) << "</td><td>" << Html(stream.profile_name) << "</td><td>"
            << Html(resolution) << "</td><td>"
            << Fixed(stream.bitrate_bps / 1000.0, 1) << " kbps</td><td>"
            << stream.frame_count << " / " << stream.packet_count << "</td></tr>\n";
    }
    out << "  </table>\n";

    if (run.analysis.audio_qc.analyzed) {
        out << "  <h2>响度与音频 QC</h2>\n  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
        out << "    <tr><td>积分响度</td><td>" << Fixed(run.analysis.audio_qc.integrated_lufs, 2)
            << " LUFS</td></tr>\n";
        out << "    <tr><td>真峰值</td><td>" << Fixed(run.analysis.audio_qc.true_peak_dbtp, 2)
            << " dBTP</td></tr>\n";
        out << "    <tr><td>响度范围 LRA</td><td>"
            << Fixed(run.analysis.audio_qc.loudness_range_lu, 2) << " LU</td></tr>\n";
        out << "    <tr><td>采样峰值</td><td>" << Fixed(run.analysis.audio_qc.sample_peak_dbfs, 2)
            << " dBFS</td></tr>\n";
        out << "    <tr><td>静音占比</td><td>"
            << Fixed(run.analysis.audio_qc.silence_ratio * 100.0, 2) << " %</td></tr>\n";
        out << "    <tr><td>削波样本</td><td>" << run.analysis.audio_qc.clipping_sample_count
            << "</td></tr>\n";
        out << "  </table>\n";
    }

    if (bundle.has_comparison) {
        out << "  <h2>文件对比</h2>\n";
        out << "  <p class=\"meta\">原文件: " << Html(bundle.comparison.left_path)
            << " ｜ 对比文件: " << Html(bundle.comparison.right_path) << " ｜ 差异项: "
            << bundle.comparison.DifferentCount() << "</p>\n";
        out << "  <table>\n    <tr><th>分组</th><th>字段</th><th>原文件</th><th>对比文件</th>"
               "<th>差异</th></tr>\n";
        for (const auto& row : bundle.comparison.rows) {
            if (row.diff == qc::QcFieldDiff::Unavailable) continue;
            out << "    <tr><td>" << Html(row.group) << "</td><td>" << Html(row.field)
                << "</td><td>" << Html(row.left) << " " << Html(row.unit) << "</td><td>"
                << Html(row.right) << " " << Html(row.unit) << "</td><td>"
                << Html(qc::ToString(row.diff)) << "</td></tr>\n";
        }
        out << "  </table>\n";
    }

    out << "  <h2>规则快照</h2>\n";
    out << "  <table>\n    <tr><th>规则</th><th>类别</th><th>阈值</th><th>启用</th></tr>\n";
    for (const auto& rule : report.rules) {
        out << "    <tr><td>" << Html(rule.name) << " (" << Html(rule.id) << ")</td><td>"
            << Html(model::ToString(rule.category)) << "</td><td>" << Fixed(rule.threshold, 2)
            << " " << Html(rule.unit) << "</td><td>" << (rule.enabled ? "是" : "否")
            << "</td></tr>\n";
    }
    out << "  </table>\n";

    out << "  <p class=\"meta\">Generated by VideoEye 2.0 ｜ schema version "
        << kReportSchemaVersion << "</p>\n</body>\n</html>\n";
    return out.str();
}

bool QcReportExporter::ExportHtml(const std::string& path, const QcExportBundle& bundle) {
    return WriteUtf8File(path, BuildHtml(bundle));
}

// ===========================================================================
// 纯文本
// ===========================================================================

std::string QcReportExporter::BuildText(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    std::ostringstream out;
    out << "========================================\n  VideoEye QC 报告\n"
        << "========================================\n";
    out << "模板: " << bundle.profile_name << " (" << bundle.profile_id << ")\n";
    out << "文件: " << report.file_path << "\n";
    out << "生成时间: " << report.generated_at << "\n";
    out << "分析耗时: " << Fixed(run.elapsed_ms, 0) << " ms\n";
    out << "评分: " << Fixed(report.score, 1) << " / 100 (" << report.verdict << ")\n";
    if (report.partial) out << "注意: 本次为抽样扫描（命中包数上限），结果仅覆盖部分数据，结论仅供参考\n";
    else if (!report.completed) out << "注意: 分析未跑完，结果不完整\n";
    if (!run.error.empty()) out << "错误: " << run.error << "\n";

    out << "\n--- 汇总 ---\n";
    out << "致命 " << report.CountBySeverity(model::IssueSeverity::Critical) << " / 错误 "
        << report.CountBySeverity(model::IssueSeverity::Error) << " / 警告 "
        << report.CountBySeverity(model::IssueSeverity::Warning) << " / 提示 "
        << report.CountBySeverity(model::IssueSeverity::Info) << "\n";
    out << "时长 " << Fixed(report.duration_seconds, 3) << " s ｜ 大小 " << report.file_size_bytes
        << " 字节 ｜ 码率 " << Fixed(report.overall_bitrate_bps / 1000.0, 1) << " kbps\n";

    out << "\n--- 问题清单 ---\n";
    if (report.issues.empty()) {
        out << "未发现问题。\n";
    } else {
        for (std::size_t i = 0; i < report.issues.size(); ++i) {
            const auto& issue = report.issues[i];
            out << "[" << (i + 1) << "] " << issue.SeverityText() << " / " << issue.CategoryText()
                << " / " << issue.title << " (" << issue.rule_id << ")\n";
            out << "    位置: " << issue.range.ToString() << "\n";
            out << "    " << issue.detail << "\n";
            if (!issue.suggestion.empty()) out << "    建议: " << issue.suggestion << "\n";
        }
    }

    if (run.analysis.audio_qc.analyzed) {
        out << "\n--- 响度 ---\n";
        out << "  积分响度: " << Fixed(run.analysis.audio_qc.integrated_lufs, 2) << " LUFS\n";
        out << "  真峰值: " << Fixed(run.analysis.audio_qc.true_peak_dbtp, 2) << " dBTP\n";
        out << "  LRA: " << Fixed(run.analysis.audio_qc.loudness_range_lu, 2) << " LU\n";
    }

    if (bundle.has_comparison) {
        out << "\n--- 对比 ---\n" << BuildComparisonText(bundle.comparison);
    }

    return out.str();
}

bool QcReportExporter::ExportText(const std::string& path, const QcExportBundle& bundle) {
    return WriteUtf8File(path, BuildText(bundle));
}

// ===========================================================================
// PDF
// ===========================================================================

QcPdfExportResult QcReportExporter::ExportPdf(const std::string& path,
                                             const QcExportBundle& bundle) {
    QcPdfExportResult result;
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    MiniPdfWriter writer;
    writer.AddLine("VideoEye QC Report");
    writer.AddLine("");
    writer.AddLine("Profile : " + bundle.profile_name + " (" + bundle.profile_id + ")");
    writer.AddLine("File    : " + report.file_name);
    writer.AddLine("Path    : " + report.file_path);
    writer.AddLine("Time    : " + report.generated_at);
    writer.AddLine("Verdict : " + Fixed(report.score, 1) + " / 100  " + report.verdict);
    writer.AddLine("Elapsed : " + Fixed(run.elapsed_ms, 0) + " ms");
    writer.AddLine(kRowGroupSeparator);

    writer.AddLine("Issues: critical " +
                   std::to_string(report.CountBySeverity(model::IssueSeverity::Critical)) +
                   " / error " + std::to_string(report.CountBySeverity(model::IssueSeverity::Error)) +
                   " / warning " +
                   std::to_string(report.CountBySeverity(model::IssueSeverity::Warning)) +
                   " / info " + std::to_string(report.CountBySeverity(model::IssueSeverity::Info)));
    writer.AddLine("Container  : " + report.container_format);
    writer.AddLine("Duration   : " + Fixed(report.duration_seconds, 3) + " s");
    writer.AddLine("Size       : " + Int64Text(report.file_size_bytes) + " bytes");
    writer.AddLine("Bitrate    : " + Fixed(report.overall_bitrate_bps / 1000.0, 1) + " kbps");
    writer.AddLine("Streams    : video " + std::to_string(report.video_stream_count) +
                   " / audio " + std::to_string(report.audio_stream_count));
    writer.AddLine(kRowGroupSeparator);

    if (report.issues.empty()) {
        writer.AddLine("No issue found.");
    } else {
        writer.AddLine("Issue list:");
        for (std::size_t i = 0; i < report.issues.size(); ++i) {
            const auto& issue = report.issues[i];
            writer.AddLine("[" + std::to_string(i + 1) + "] " + std::string(SeverityCode(issue.severity)) +
                           " / " + issue.rule_id + " / " + issue.title);
            writer.AddLine("    " + issue.range.ToString());
            writer.AddLine("    " + SingleLine(issue.detail));
            if (!issue.suggestion.empty()) writer.AddLine("    fix: " + SingleLine(issue.suggestion));
        }
    }

    if (run.analysis.audio_qc.analyzed) {
        writer.AddLine(kRowGroupSeparator);
        writer.AddLine("Loudness   : " + Fixed(run.analysis.audio_qc.integrated_lufs, 2) + " LUFS");
        writer.AddLine("True peak  : " + Fixed(run.analysis.audio_qc.true_peak_dbtp, 2) + " dBTP");
        writer.AddLine("LRA        : " + Fixed(run.analysis.audio_qc.loudness_range_lu, 2) + " LU");
    }

    std::string bytes;
    if (!writer.Build(bytes)) return result;
    if (!WriteUtf8File(path, bytes)) return result;

    result.ok = true;
    // 报告里所有文本面一个个查代价太高（detail 字段最长），这里抽查标题/详情/建议三类。
    for (const auto& issue : report.issues) {
        if (MiniPdfWriter::HasLossyText(issue.title) ||
            MiniPdfWriter::HasLossyText(issue.detail) ||
            MiniPdfWriter::HasLossyText(issue.suggestion)) {
            result.text_loss = true;
            break;
        }
    }
    if (MiniPdfWriter::HasLossyText(report.verdict) ||
        MiniPdfWriter::HasLossyText(bundle.profile_name) ||
        MiniPdfWriter::HasLossyText(report.file_path)) {
        result.text_loss = true;
    }
    return result;
}

// ===========================================================================
// 对比导出
// ===========================================================================

std::string QcReportExporter::BuildComparisonText(const qc::QcComparison& comparison) {
    std::ostringstream out;
    out << "原文件  : " << comparison.left_path << "\n";
    out << "对比文件: " << comparison.right_path << "\n";
    out << "评分    : " << Fixed(comparison.left_score, 1) << " -> "
        << Fixed(comparison.right_score, 1) << "\n";
    out << "差异项  : " << comparison.DifferentCount() << " / " << comparison.AvailableRowCount()
        << "\n";

    for (const auto& group : comparison.GroupNames()) {
        out << "\n[" << group << "]\n";
        for (const auto& row : comparison.rows) {
            if (row.group != group) continue;
            out << "  " << row.field << ": " << row.left;
            if (!row.unit.empty()) out << " " << row.unit;
            out << "  ->  " << row.right;
            if (!row.unit.empty()) out << " " << row.unit;
            if (row.diff == qc::QcFieldDiff::Different) {
                out << "   (差 " << Fixed(row.delta, 3) << (row.unit.empty() ? "" : " " + row.unit) << ")";
            } else if (row.diff != qc::QcFieldDiff::Same) {
                out << "   (" << qc::ToString(row.diff) << ")";
            }
            out << "\n";
        }
    }
    return out.str();
}

bool QcReportExporter::ExportComparisonText(const std::string& path,
                                           const qc::QcComparison& comparison) {
    return WriteUtf8File(path, BuildComparisonText(comparison));
}

bool QcReportExporter::ExportComparisonCsv(const std::string& path,
                                          const qc::QcComparison& comparison) {
    std::ostringstream body;
    body << "\xEF\xBB\xBF";
    body << "分组,字段,原文件,对比文件,单位,是否一致,差值\n";
    for (const auto& row : comparison.rows) {
        const char* diff_text = "same";
        switch (row.diff) {
            case qc::QcFieldDiff::Same:        diff_text = "same";        break;
            case qc::QcFieldDiff::Different:   diff_text = "different";   break;
            case qc::QcFieldDiff::OnlyLeft:    diff_text = "only_left";   break;
            case qc::QcFieldDiff::OnlyRight:   diff_text = "only_right";  break;
            case qc::QcFieldDiff::Unavailable: diff_text = "unavailable"; break;
        }
        body << CsvField(row.group) << "," << CsvField(row.field) << "," << CsvField(row.left)
             << "," << CsvField(row.right) << "," << CsvField(row.unit) << "," << diff_text << ",";
        if (row.numeric) body << Fixed(row.delta, 4);
        body << "\n";
    }
    return WriteUtf8File(path, body.str());
}

bool QcReportExporter::ExportComparisonJson(const std::string& path,
                                           const qc::QcComparison& comparison) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("schema", JText("videoeye.qc-comparison"));
    root.Set("schema_version", JNumber(static_cast<double>(kReportSchemaVersion)));
    root.Set("generated_at", JText(model::CurrentTimestampString()));
    root.Set("left_path", JText(comparison.left_path));
    root.Set("right_path", JText(comparison.right_path));
    root.Set("left_score", JNumber(comparison.left_score));
    root.Set("right_score", JNumber(comparison.right_score));
    root.Set("left_verdict", JText(comparison.left_verdict));
    root.Set("right_verdict", JText(comparison.right_verdict));
    root.Set("different_count", JNumber(static_cast<double>(comparison.DifferentCount())));
    root.Set("compared_row_count", JNumber(static_cast<double>(comparison.AvailableRowCount())));

    JsonValue rows = JsonValue::MakeArray();
    for (const auto& row : comparison.rows) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("group", JText(row.group));
        item.Set("field", JText(row.field));
        item.Set("left", JText(row.left));
        item.Set("right", JText(row.right));
        item.Set("unit", JText(row.unit));
        item.Set("diff", JText(std::string([&row]() {
            switch (row.diff) {
                case qc::QcFieldDiff::Same:        return "same";
                case qc::QcFieldDiff::Different:   return "different";
                case qc::QcFieldDiff::OnlyLeft:    return "only_left";
                case qc::QcFieldDiff::OnlyRight:   return "only_right";
                case qc::QcFieldDiff::Unavailable: return "unavailable";
            }
            return "unavailable";
        }())));
        if (row.numeric) {
            item.Set("left_value", JNumber(row.left_value));
            item.Set("right_value", JNumber(row.right_value));
            item.Set("delta", JNumber(row.delta));
        }
        rows.PushBack(item);
    }
    root.Set("rows", rows);

    return WriteUtf8File(path, root.ToPrettyString(2) + "\n");
}

std::string QcReportExporter::BuildComparisonHtml(const qc::QcComparison& comparison) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n  <meta charset=\"UTF-8\">\n"
        << "  <title>VideoEye 文件对比</title>\n  <style>\n"
        << "    body { font-family: 'Microsoft YaHei', Arial, sans-serif; margin: 24px; }\n"
        << "    table { border-collapse: collapse; width: 100%; margin-top: 10px; font-size: 13px; }\n"
        << "    th, td { border: 1px solid #d8dee6; padding: 6px 8px; text-align: left; }\n"
        << "    th { background: #f2f4f7; }\n"
        << "    .different td { background: #fff5f5; }\n"
        << "    .meta { color: #6b7280; font-size: 12px; }\n"
        << "  </style>\n</head>\n<body>\n";
    out << "  <h1>VideoEye 文件对比</h1>\n";
    out << "  <p class=\"meta\">原文件: " << Html(comparison.left_path) << "<br>对比文件: "
        << Html(comparison.right_path) << "<br>评分 " << Fixed(comparison.left_score, 1) << " → "
        << Fixed(comparison.right_score, 1) << " ｜ 差异项 " << comparison.DifferentCount()
        << " / " << comparison.AvailableRowCount() << "</p>\n";
    out << "  <table>\n    <tr><th>分组</th><th>字段</th><th>原文件</th><th>对比文件</th>"
           "<th>单位</th><th>判定</th><th>差值</th></tr>\n";
    for (const auto& row : comparison.rows) {
        const bool different = row.diff == qc::QcFieldDiff::Different;
        out << "    <tr" << (different ? " class=\"different\"" : "") << "><td>" << Html(row.group)
            << "</td><td>" << Html(row.field) << "</td><td>" << Html(row.left) << "</td><td>"
            << Html(row.right) << "</td><td>" << Html(row.unit) << "</td><td>"
            << Html(qc::ToString(row.diff)) << "</td><td>"
            << (row.numeric ? Fixed(row.delta, 3) : std::string()) << "</td></tr>\n";
    }
    out << "  </table>\n</body>\n</html>\n";
    return out.str();
}

bool QcReportExporter::ExportComparisonHtml(const std::string& path,
                                           const qc::QcComparison& comparison) {
    return WriteUtf8File(path, BuildComparisonHtml(comparison));
}

bool QcReportExporter::ExportComparisonAuto(const std::string& path,
                                           const qc::QcComparison& comparison) {
    qc::QcReportFormat format = qc::QcReportFormat::Json;
    if (!qc::QcReportFormatFromPath(path, format)) format = qc::QcReportFormat::Json;
    switch (format) {
        case qc::QcReportFormat::Json: return ExportComparisonJson(path, comparison);
        case qc::QcReportFormat::Csv:  return ExportComparisonCsv(path, comparison);
        case qc::QcReportFormat::Html: return ExportComparisonHtml(path, comparison);
        case qc::QcReportFormat::Text: return ExportComparisonText(path, comparison);
        case qc::QcReportFormat::Pdf:  return ExportComparisonText(path, comparison);
    }
    return false;
}

// ===========================================================================
// 批量汇总
// ===========================================================================

QcBatchSummaryInput QcReportExporter::MakeBatchSummary(const std::string& root,
                                                       const qc::BatchQcRun& run,
                                                       const qc::QcProfile& profile) {
    QcBatchSummaryInput input;
    input.root = root;
    input.profile_id = profile.id;
    input.profile_name = profile.name;

    for (const auto& item : run.items) {
        QcBatchSummaryRow row;
        row.path = item.path;
        row.status = qc::ToString(item.status);
        row.error = item.error;
        row.verdict = item.verdict;
        row.output_path = item.output_path;
        row.score = item.score;
        row.elapsed_ms = item.elapsed_ms;
        row.critical_count = item.critical_count;
        row.error_count = item.error_count;
        row.warning_count = item.warning_count;
        row.info_count = item.info_count;
        input.rows.push_back(std::move(row));
    }

    input.succeeded = run.summary.succeeded;
    input.failed = run.summary.failed;
    input.cancelled = run.summary.cancelled;
    input.skipped = run.summary.skipped;
    input.critical_count = run.summary.critical_count;
    input.error_count = run.summary.error_count;
    input.warning_count = run.summary.warning_count;
    input.info_count = run.summary.info_count;
    input.elapsed_ms = run.summary.elapsed_ms;
    input.completed = run.summary.completed;
    return input;
}

bool QcReportExporter::ExportBatchSummaryJson(const std::string& path,
                                             const QcBatchSummaryInput& input) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("schema", JText("videoeye.qc-batch-summary"));
    root.Set("schema_version", JNumber(static_cast<double>(kReportSchemaVersion)));
    root.Set("generated_at", JText(model::CurrentTimestampString()));
    root.Set("root", JText(input.root));

    JsonValue profile = JsonValue::MakeObject();
    profile.Set("id", JText(input.profile_id));
    profile.Set("name", JText(input.profile_name));
    root.Set("profile", profile);

    JsonValue counts = JsonValue::MakeObject();
    counts.Set("total", JNumber(static_cast<double>(input.rows.size())));
    counts.Set("succeeded", JNumber(static_cast<double>(input.succeeded)));
    counts.Set("failed", JNumber(static_cast<double>(input.failed)));
    counts.Set("cancelled", JNumber(static_cast<double>(input.cancelled)));
    counts.Set("skipped", JNumber(static_cast<double>(input.skipped)));
    counts.Set("critical", JNumber(static_cast<double>(input.critical_count)));
    counts.Set("error", JNumber(static_cast<double>(input.error_count)));
    counts.Set("warning", JNumber(static_cast<double>(input.warning_count)));
    counts.Set("info", JNumber(static_cast<double>(input.info_count)));
    root.Set("counts", counts);
    root.Set("elapsed_ms", JNumber(input.elapsed_ms));
    root.Set("completed", JsonValue(input.completed));

    JsonValue rows = JsonValue::MakeArray();
    for (const auto& row : input.rows) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("path", JText(row.path));
        item.Set("status", JText(row.status));
        item.Set("score", JNumber(row.score));
        item.Set("verdict", JText(row.verdict));
        item.Set("critical_count", JNumber(static_cast<double>(row.critical_count)));
        item.Set("error_count", JNumber(static_cast<double>(row.error_count)));
        item.Set("warning_count", JNumber(static_cast<double>(row.warning_count)));
        item.Set("info_count", JNumber(static_cast<double>(row.info_count)));
        item.Set("elapsed_ms", JNumber(row.elapsed_ms));
        if (!row.output_path.empty()) item.Set("output_path", JText(row.output_path));
        if (!row.error.empty()) item.Set("error", JText(row.error));
        rows.PushBack(item);
    }
    root.Set("files", rows);

    return WriteUtf8File(path, root.ToPrettyString(2) + "\n");
}

bool QcReportExporter::ExportBatchSummaryCsv(const std::string& path,
                                            const QcBatchSummaryInput& input) {
    std::ostringstream body;
    body << "\xEF\xBB\xBF";
    body << "文件,状态,评分,结论,致命,错误,警告,提示,耗时(ms),输出路径,错误\n";
    for (const auto& row : input.rows) {
        body << CsvField(row.path) << "," << CsvField(row.status) << ","
             << Fixed(row.score, 1) << "," << CsvField(row.verdict) << ","
             << row.critical_count << "," << row.error_count << "," << row.warning_count << ","
             << row.info_count << "," << Fixed(row.elapsed_ms, 0) << ","
             << CsvField(row.output_path) << "," << CsvField(SingleLine(row.error)) << "\n";
    }
    return WriteUtf8File(path, body.str());
}

bool QcReportExporter::ExportBatchSummaryHtml(const std::string& path,
                                             const QcBatchSummaryInput& input) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n  <meta charset=\"UTF-8\">\n"
        << "  <title>VideoEye 批量 QC 汇总</title>\n  <style>\n"
        << "    body { font-family: 'Microsoft YaHei', Arial, sans-serif; margin: 24px; }\n"
        << "    table { border-collapse: collapse; width: 100%; margin-top: 10px; font-size: 13px; }\n"
        << "    th, td { border: 1px solid #d8dee6; padding: 6px 8px; text-align: left; }\n"
        << "    th { background: #f2f4f7; }\n"
        << "    .bad td:nth-child(5) { color: #b3261e; font-weight: bold; }\n"
        << "    .meta { color: #6b7280; font-size: 12px; }\n"
        << "  </style>\n</head>\n<body>\n";
    out << "  <h1>VideoEye 批量 QC 汇总</h1>\n";
    out << "  <p class=\"meta\">目录: " << Html(input.root) << " ｜ 模板: "
        << Html(input.profile_name) << " (" << Html(input.profile_id) << ") ｜ 完成 "
        << input.succeeded << " / 失败 " << input.failed << " / 取消 " << input.cancelled
        << " / 跳过 " << input.skipped << " ｜ 耗时 " << Fixed(input.elapsed_ms, 0)
        << " ms" << (input.completed ? "" : " ｜ <b>未完成（被取消）</b>") << "</p>\n";
    out << "  <table>\n    <tr><th>文件</th><th>状态</th><th>评分</th><th>结论</th>"
           "<th>致命</th><th>错误</th><th>警告</th><th>提示</th><th>耗时(ms)</th><th>输出</th></tr>\n";
    for (const auto& row : input.rows) {
        const bool bad = row.critical_count > 0 || row.error_count > 0;
        out << "    <tr" << (bad ? " class=\"bad\"" : "") << "><td>" << Html(row.path)
            << "</td><td>" << Html(row.status) << "</td><td>" << Fixed(row.score, 1)
            << "</td><td>" << Html(row.verdict) << "</td><td>" << row.critical_count
            << "</td><td>" << row.error_count << "</td><td>" << row.warning_count
            << "</td><td>" << row.info_count << "</td><td>" << Fixed(row.elapsed_ms, 0)
            << "</td><td>" << Html(row.output_path) << "</td></tr>\n";
    }
    out << "  </table>\n</body>\n</html>\n";
    return WriteUtf8File(path, out.str());
}

bool QcReportExporter::ExportBatchSummaryAuto(const std::string& path,
                                             const QcBatchSummaryInput& input) {
    qc::QcReportFormat format = qc::QcReportFormat::Json;
    if (!qc::QcReportFormatFromPath(path, format)) format = qc::QcReportFormat::Json;
    switch (format) {
        case qc::QcReportFormat::Json: return ExportBatchSummaryJson(path, input);
        case qc::QcReportFormat::Csv:  return ExportBatchSummaryCsv(path, input);
        case qc::QcReportFormat::Html: return ExportBatchSummaryHtml(path, input);
        case qc::QcReportFormat::Text: return ExportBatchSummaryCsv(path, input);
        case qc::QcReportFormat::Pdf:  return ExportBatchSummaryHtml(path, input);
    }
    return false;
}

// ===========================================================================
// 分派
// ===========================================================================

bool QcReportExporter::Export(const std::string& path, const QcExportBundle& bundle,
                              qc::QcReportFormat format) {
    switch (format) {
        case qc::QcReportFormat::Json: return ExportJson(path, bundle);
        case qc::QcReportFormat::Csv:  return ExportCsv(path, bundle);
        case qc::QcReportFormat::Html: return ExportHtml(path, bundle);
        case qc::QcReportFormat::Text: return ExportText(path, bundle);
        case qc::QcReportFormat::Pdf:  return ExportPdf(path, bundle).ok;
    }
    return false;
}

bool QcReportExporter::ExportReport(const std::string& path, const model::QcReport& report) {
    // 没有 profile 上下文可写，用占位值顶上 —— 报告主体（issues / metrics / streams）
    // 全部来自 report 本身，profile 只在页眉出现一次。
    QcExportBundle bundle;
    bundle.profile_id = "default";
    bundle.profile_name = "默认规则";
    bundle.run.ok = report.completed;
    bundle.run.report = report;
    if (!bundle.run.ok) bundle.run.error = "分析未完成";
    bundle.has_comparison = false;
    return ExportAuto(path, bundle);
}

bool QcReportExporter::ExportAuto(const std::string& path, const QcExportBundle& bundle) {
    qc::QcReportFormat format = qc::QcReportFormat::Json;
    if (!qc::QcReportFormatFromPath(path, format)) format = qc::QcReportFormat::Json;
    return Export(path, bundle, format);
}

}  // namespace reporting
}  // namespace videoeye
