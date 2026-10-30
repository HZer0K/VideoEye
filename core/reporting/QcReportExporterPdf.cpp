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
using detail::WriteUtf8File;

namespace {

// 一行最多画多少"显示单位"：A4 宽 595pt、左右各 48pt 边距 → 正文宽 499pt。
// 10pt Helvetica 的 ASCII 平均字宽约 0.55em（5.5pt），中文（STSong-Light）
// 是全宽 1em（10pt）。按"半角 1、全角 2"的显示单位折算，理论上一行约 90 个
// 半角字符；取 88 略保守，免得 W / M 这类宽字母把最后一个字顶出右边界。
constexpr std::size_t kMaxLineUnits = 88;

// 按显示宽度折行。
//
// 为什么必须有这一层：PDF 的正文直接复用纯文本报告（见 ExportPdf），而文本
// 报告里有整段很长的行（TS 传输层那串统计、参数集的 Summary、issue 的 detail
// 展开后都可能上百字）。MiniPdfWriter 只按 \n 切段、不做自动换行 —— 超宽的行
// 会一路画出 /MediaBox 右边界，阅读器里直接被裁掉，而 PDF 自身仍然"结构有效"、
// 导出仍然返回成功。这类缺陷只有真渲染或逐字符量宽才看得见。
//
// 断行时不丢任何字符（包括断点处的空格），所以"PDF 去掉空白"与"文本报告去掉
// 空白"逐字相等 —— 这一点由 test_qc_report_exporter 的跨格式一致性用例守着。
std::vector<std::string> WrapToUnits(const std::string& line, std::size_t max_units) {
    std::vector<std::string> out;
    std::string current;
    std::size_t units = 0;
    for (std::size_t i = 0; i < line.size();) {
        const unsigned char lead = static_cast<unsigned char>(line[i]);
        std::size_t advance = 1;
        if (lead >= 0xF0)
            advance = 4;
        else if (lead >= 0xE0)
            advance = 3;
        else if (lead >= 0xC0)
            advance = 2;
        if (i + advance > line.size())
            advance = 1; // 尾部截断的字节按单字节处理，别越界
        // 非 ASCII 一律按全角算：报告正文里的非 ASCII 只有 CJK 与全角标点。
        const std::size_t width = (lead < 0x80) ? 1 : 2;
        if (units + width > max_units && !current.empty()) {
            out.push_back(current);
            current.clear();
            units = 0;
        }
        current.append(line, i, advance);
        units += width;
        i += advance;
    }
    out.push_back(current);
    return out;
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

    static constexpr double kPageWidth = 595.0; // A4 纵向（pt）
    static constexpr double kPageHeight = 842.0;
    static constexpr double kMarginX = 48.0;
    static constexpr double kMarginTop = 760.0;
    static constexpr double kLeading = 14.0;
    static constexpr int kMaxLinesPerPage = 50;

    void AddLine(const std::string& text) {
        lines_.push_back(text);
    }

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
        if (!current.empty() || pages.empty())
            pages.push_back(current);

        // ⚠️ 对象编号必须在 kids 落位**之前**算完：以前是等全部对象都填好才
        // `objects.insert(begin()+1, kids)`，插入把后面所有的下标整体顶掉一位，
        // 而 /Contents 与 /Kids 里已经按老编号写死了 —— 两边差一号，PDF 全废。
        // 这里让 kids 一开始就占住下标 1（对象 2），此后"下标 i 恒等于对象 i+1"，
        // 编号随 push 顺序自然算出，不再有插入错位。
        std::vector<std::string> objects;                       // objects[0] 对应 PDF 对象 1
        objects.push_back("<< /Type /Catalog /Pages 2 0 R >>"); // 1: Catalog
        objects.push_back(std::string());                       // 2: Pages（Kids，最后填）
        objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                          "/Encoding /WinAnsiEncoding >>"); // 3: F1
        objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
                          "/Encoding /WinAnsiEncoding >>"); // 4: F2

        // 5: F3（中文, Identity-H）与 6: ToUnicode CMap。两个都要等页面内容扫完才填得出来
        // （CMap 得知道这整份报告里都出现过哪些非 Latin 字符），所以先占位、编号先定。
        const int font_object = static_cast<int>(objects.size()) + 1;
        const int cmap_object = font_object + 1;
        objects.push_back(std::string()); // 5: F3
        objects.push_back(std::string()); // 6: ToUnicode

        const int first_content = static_cast<int>(objects.size()) + 1;
        for (const auto& page : pages) {
            const double top = kMarginTop;
            std::ostringstream stream;
            stream << "BT /F1 10 Tf " << kLeading << " TL " << kMarginX << " " << top << " Td\n";
            for (const auto& line : page) {
                // 一行切成若干 run：Latin 段用 F1 画，中文段用 F3 画。
                // 不能整行二选一 —— 报告里"中文问题标题 + 英文 rule_id"这种行很常见。
                for (const Run& run : MakeRuns(line)) {
                    if (run.IsHex())
                        stream << "/F3 10 Tf <" << run.hex << "> Tj\n";
                    else
                        stream << "/F1 10 Tf (" << run.text << ") Tj\n";
                }
                // ⚠️ 每行画完必须换到下一行。Tj 只把光标往右推、不会换行，少了 T*
                // 整页几十行会首尾相接画在同一条基线上（冲出页面、只剩开头可见），
                // 而文件照样"写得出来" —— 这类缺陷只有真渲染或逐操作数校验才看得见。
                stream << "T*\n";
            }
            stream << "ET";
            const std::string content = stream.str();
            objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                              "\nendstream");
        }

        const int first_page = first_content + static_cast<int>(pages.size());
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const int resource_object = first_content + static_cast<int>(i);
            objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + Fixed(kPageWidth, 0) + " " +
                              Fixed(kPageHeight, 0) + "] " + "/Resources << /Font << /F1 " +
                              std::to_string(kFontWinAnsi1) + " 0 R /F2 " + std::to_string(kFontWinAnsi2) +
                              " 0 R /F3 " + std::to_string(font_object) + " 0 R >> >> " + "/Contents " +
                              std::to_string(resource_object) + " 0 R >>");
        }

        objects[font_object - 1] = "<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light /Encoding /Identity-H "
                                   "/DescendantFonts [ << /Type /Font /Subtype /CIDFontType2 /BaseFont /STSong-Light "
                                   "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> /DW 1000 "
                                   "/CIDToGIDMap /Identity >> ] /ToUnicode " +
                                   std::to_string(cmap_object) + " 0 R >>";
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
        body += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
                std::string(trailer) + "\n%%EOF\n";

        out = body;
        return true;
    }

    // 有没有字被降级成 '?'（供调用方提示用户）
    //
    // ⚠️ 语义：只有"这个字体真的表示不了"才算丢字（目前是 BMP 外的字符，如 emoji）。
    // 以前这里是"凡出现非 ASCII 字节就算丢"，于是中文报告一律被判成 text_loss=true，
    // 界面弹出"中文已被替换为 ?" —— 与实际情况正好相反。
    bool HasLoss() const {
        return lossy_;
    }

private:
    // 一行文本切成的一段连续字节：
    //   text —— WinAnsi 单字节段，交给 F1 直接画（括号/反斜杠已转义）
    //   hex  —— UTF-16BE 十六进制段（不含尖括号），交给 F3 画
    struct Run {
        std::string text;
        std::string hex;
        bool IsHex() const {
            return !hex.empty();
        }
    };

    // 逐段（run）生成器：ASCII / Latin-1 走 WinAnsi，其它（中文、日文……）走 UTF-16BE。
    //
    // 以前这里是"整行统一处理"，落到 WinAnsi 之外的字符一律写成 '?'，中文报告全是问号。
    // 现在按 run 切：一行里中英混排很常见，英文部分照旧走 F1，中文部分单独成段走 F3，
    // 两边都不丢。
    //
    // 非 Latin 的 codepoint 顺手记进 codepoints —— ToUnicode CMap 要用。
    std::vector<Run> MakeRuns(const std::string& text) {
        std::vector<Run> runs;
        Run run;
        auto flush = [&]() {
            if (!run.text.empty() || !run.hex.empty())
                runs.push_back(run);
            run = Run();
        };
        for (std::size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            unsigned int codepoint = c;
            std::size_t advance = 1;
            if (c >= 0xF0 && i + 3 < text.size()) {
                codepoint = ((c & 0x07u) << 18) | ((static_cast<unsigned int>(text[i + 1]) & 0x3Fu) << 12) |
                            ((static_cast<unsigned int>(text[i + 2]) & 0x3Fu) << 6) |
                            (static_cast<unsigned int>(text[i + 3]) & 0x3Fu);
                advance = 4;
            } else if (c >= 0xE0 && i + 2 < text.size()) {
                codepoint = ((c & 0x0Fu) << 12) | ((static_cast<unsigned int>(text[i + 1]) & 0x3Fu) << 6) |
                            ((static_cast<unsigned int>(text[i + 2]) & 0x3Fu));
                advance = 3;
            } else if (c >= 0xC0 && i + 1 < text.size()) {
                codepoint = ((c & 0x1Fu) << 6) | (static_cast<unsigned int>(text[i + 1]) & 0x3Fu);
                advance = 2;
            }

            const bool latin = (codepoint < 0x100); // WinAnsi 覆盖得住
            if (latin) {
                if (run.IsHex())
                    flush();
                switch (codepoint) {
                case '(':
                    run.text += "\\(";
                    break;
                case ')':
                    run.text += "\\)";
                    break;
                case '\\':
                    run.text += "\\\\";
                    break;
                default:
                    run.text += static_cast<char>(codepoint);
                    break;
                }
            } else {
                if (!run.IsHex()) {
                    flush();
                    // BOM 只在这个 run 的开头写一次 —— 以前是每个字符前都写，
                    // 于是中文之间被塞进一堆 CID 0xFEFF（字体里没有这个字形），
                    // 阅读器在每个字之间画一个空白/缺字框。
                    AppendHexPair(run.hex, 0xFEFF);
                }
                if (codepoint > 0xFFFF) {
                    // Identity-H 的码元只有 16 位，装不下 BMP 以外的字符（emoji 等）。
                    // 退化成 '?' 并置位 lossy_，让调用方如实提示。
                    AppendHexPair(run.hex, '?');
                    lossy_ = true;
                } else {
                    AppendHexPair(run.hex, codepoint);
                    codepoints_.insert(codepoint);
                }
            }
            i += advance;
        }
        flush();
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
            std::snprintf(buf, sizeof(buf), "<%04X> <%04X>\n", codepoint & 0xFFFFu, codepoint & 0xFFFFu);
            cmap << buf;
        }
        cmap << "endbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend";
        return "<< /Length " + std::to_string(cmap.str().size()) + " >>\nstream\n" + cmap.str() + "\nendstream";
    }

    std::vector<std::string> lines_;
    std::set<unsigned int> codepoints_; // 整份报告里出现过的非 Latin 码点
    bool lossy_ = false;                // 有字符被降级成 '?'（BMP 外字符）
};

} // namespace

// ===========================================================================
// PDF
// ===========================================================================

QcPdfExportResult QcReportExporter::ExportPdf(const std::string& path, const QcExportBundle& bundle) {
    QcPdfExportResult result;

    // 正文只有一处来源：纯文本版。
    //
    // 此前 PDF 自己另排一套（英文抬头 + 问题清单 + 主轨响度三项），于是
    // 「证据与覆盖」「参数变化时间线」「帧级显示顺序」「音频 QC（逐轨）」
    // 「MPEG-TS 传输层」这五节在 PDF 里一个字都没有。同一份 QC 结论换个格式
    // 导出就丢掉全部覆盖说明 —— 而 PDF 恰恰是最常用于交付与存档的格式，
    // 于是"问题清单里没问题"与"这个维度根本没跑"在存档件上又长得一样了，
    // 正是 DimensionStatus 这套结构要消灭的那种虚假结论。
    //
    // 复用之后两端在结构上不可能再漂移：文本报告加一节，PDF 自动就有。
    // 代价是 PDF 少了英文抬头与分隔线排版，换来的是结论一致 —— 值得。
    MiniPdfWriter writer;
    const std::string text = BuildText(bundle);
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = text.find('\n', begin);
        const std::string line = (end == std::string::npos) ? text.substr(begin) : text.substr(begin, end - begin);
        for (const std::string& wrapped : WrapToUnits(line, kMaxLineUnits))
            writer.AddLine(wrapped);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }

    std::string bytes;
    if (!writer.Build(bytes))
        return result;
    if (!WriteUtf8File(path, bytes))
        return result;

    result.ok = true;
    // 由写入器自己报告：它逐字符走过全部文本，知道哪些字真的没画出来。
    result.text_loss = writer.HasLoss();
    return result;
}

} // namespace reporting
} // namespace videoeye