#include "core/reporting/QcReportExporter.h"

#include "core/reporting/QcReportExporterInternal.h"

#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace videoeye {
namespace reporting {

using detail::Fixed;
using detail::Int64Text;
using detail::SingleLine;
using detail::WriteUtf8File;

namespace {

const char* kRowGroupSeparator = "----------------------------------------";

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

}  // namespace reporting
}  // namespace videoeye