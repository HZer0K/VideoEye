#include "PdfVerifier.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace videoeye_test {

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 从 pos 起跳过空白读一个整数
bool ReadInt(const std::string& s, std::size_t& pos, long long& out) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    bool negative = false;
    if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) {
        negative = (s[pos] == '-');
        ++pos;
    }
    if (pos >= s.size() || !IsDigit(s[pos])) return false;
    long long value = 0;
    while (pos < s.size() && IsDigit(s[pos])) {
        value = value * 10 + (s[pos] - '0');
        ++pos;
    }
    out = negative ? -value : value;
    return true;
}

// 在 [from, to) 里找 key，读出紧跟其后的第一个整数（"/Count 2" -> 2）
bool FindIntValue(const std::string& s, const std::string& key, std::size_t from, std::size_t to,
                  long long& out) {
    if (to > s.size()) to = s.size();
    std::size_t p = s.find(key, from);
    while (p != std::string::npos && p < to) {
        std::size_t q = p + key.size();
        long long value = 0;
        if (ReadInt(s, q, value)) {
            out = value;
            return true;
        }
        p = s.find(key, p + 1);
    }
    return false;
}

// 收集 [begin, end) 里所有 "N 0 R" 形式的对象号
std::vector<int> CollectRefs(const std::string& s, std::size_t begin, std::size_t end) {
    std::vector<int> refs;
    if (end > s.size()) end = s.size();
    std::size_t i = begin;
    while (i < end) {
        if (IsDigit(s[i])) {
            std::size_t j = i;
            long long value = 0;
            if (ReadInt(s, j, value)) {
                std::size_t k = j;
                while (k < end && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
                if (s.compare(k, 3, "0 R") == 0) {
                    refs.push_back(static_cast<int>(value));
                    i = k + 3;
                    continue;
                }
            }
        }
        ++i;
    }
    return refs;
}

void AppendUtf8(std::string& out, unsigned int codepoint) {
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

// 从内容流里还原文本：
//   (...)   —— 字面量串（WinAnsi 单字节）
//   <...>   —— 十六进制串（中文走的是 UTF-16BE）
void ExtractContentText(const std::string& stream, PdfVerifyResult& result) {
    // 操作符统计：Tj = 一次"画文本"，T*/Td/TD = 一次"换到下一行"
    for (std::size_t pos = 0; (pos = stream.find("Tj", pos)) != std::string::npos;) {
        ++result.text_runs;
        pos += 2;
    }
    for (const char* op : {"T*", "Td", "TD"}) {
        for (std::size_t pos = 0; (pos = stream.find(op, pos)) != std::string::npos;) {
            ++result.line_advances;
            pos += 2;
        }
    }

    for (std::size_t i = 0; i < stream.size();) {
        const char c = stream[i];
        if (c == '(') {
            ++i;
            while (i < stream.size()) {
                const char d = stream[i++];
                if (d == '\\' && i < stream.size()) {
                    result.text += stream[i++];
                    continue;
                }
                if (d == ')') break;
                result.text += d;
            }
        } else if (c == '<' && i + 1 < stream.size() && stream[i + 1] != '<') {
            ++i;
            std::string hex;
            while (i < stream.size() && stream[i] != '>') {
                if (HexValue(stream[i]) >= 0) hex += stream[i];
                ++i;
            }
            if (i < stream.size()) ++i;  // 吃掉 '>'

            std::vector<unsigned int> units;
            for (std::size_t k = 0; k + 3 < hex.size(); k += 4) {
                unsigned int unit = 0;
                for (int nibble = 0; nibble < 4; ++nibble) {
                    unit = (unit << 4) | static_cast<unsigned int>(HexValue(hex[k + nibble]));
                }
                units.push_back(unit);
            }
            for (std::size_t k = 0; k < units.size(); ++k) {
                const unsigned int unit = units[k];
                if (unit == 0xFEFF) continue;  // BOM：只是标记，不是字符
                if (unit >= 0xD800 && unit <= 0xDBFF && k + 1 < units.size() &&
                    units[k + 1] >= 0xDC00 && units[k + 1] <= 0xDFFF) {
                    const unsigned int codepoint =
                        0x10000u + ((unit - 0xD800u) << 10) + (units[k + 1] - 0xDC00u);
                    ++k;
                    AppendUtf8(result.text, codepoint);
                    continue;
                }
                AppendUtf8(result.text, unit);
            }
        } else {
            ++i;
        }
    }
}

}  // namespace

bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

PdfVerifyResult VerifyPdfFile(const std::string& path) {
    PdfVerifyResult result;
    std::string data;
    if (!ReadWholeFile(path, data)) {
        result.error = "读不到 PDF 文件: " + path;
        return result;
    }

    // ---- 文件头 ----
    if (data.compare(0, 5, "%PDF-") != 0) {
        result.error = "文件头不是 %PDF-（实际以 \"" + data.substr(0, 8) + "\" 开头）";
        return result;
    }
    const std::size_t header_end = data.find_first_of("\r\n", 5);
    result.version = data.substr(0, header_end == std::string::npos ? data.size() : header_end);
    if (data.find("%%EOF") == std::string::npos) {
        result.error = "文件末尾缺少 %%EOF";
        return result;
    }

    // ---- startxref / xref 表 ----
    const std::size_t startxref = data.rfind("startxref");
    if (startxref == std::string::npos) {
        result.error = "找不到 startxref";
        return result;
    }
    std::size_t cursor = startxref + 9;
    long long xref_offset = 0;
    if (!ReadInt(data, cursor, xref_offset)) {
        result.error = "startxref 后面不是数字";
        return result;
    }
    result.xref_offset = xref_offset;
    if (xref_offset < 0 || static_cast<std::size_t>(xref_offset) >= data.size() ||
        data.compare(static_cast<std::size_t>(xref_offset), 4, "xref") != 0) {
        result.error = "startxref 给出的偏移 " + std::to_string(xref_offset) + " 处不是 xref 表";
        return result;
    }

    cursor = static_cast<std::size_t>(xref_offset) + 4;
    long long first_object = 0;
    long long entry_count = 0;
    if (!ReadInt(data, cursor, first_object) || !ReadInt(data, cursor, entry_count) ||
        entry_count <= 0) {
        result.error = "xref 表头（起始对象号 / 条目数）解析失败";
        return result;
    }
    result.object_count = static_cast<int>(entry_count);

    std::vector<long long> offsets(static_cast<std::size_t>(entry_count), 0);
    std::vector<char> kinds(static_cast<std::size_t>(entry_count), 'f');
    for (long long i = 0; i < entry_count; ++i) {
        while (cursor < data.size() && std::isspace(static_cast<unsigned char>(data[cursor])))
            ++cursor;
        long long offset = 0;
        long long generation = 0;
        if (!ReadInt(data, cursor, offset) || !ReadInt(data, cursor, generation)) {
            result.error = "xref 第 " + std::to_string(i) + " 条解析失败";
            return result;
        }
        while (cursor < data.size() && std::isspace(static_cast<unsigned char>(data[cursor])))
            ++cursor;
        kinds[static_cast<std::size_t>(i)] = (cursor < data.size()) ? data[cursor] : 'f';
        offsets[static_cast<std::size_t>(i)] = offset;
        ++cursor;
    }
    if (kinds[0] != 'f') {
        result.error = "xref 第 0 条不是 free 项";
        return result;
    }

    // ---- 对象表 ----
    // 只认 xref 里登记的位置：偏移指不到 "<n> 0 obj" 就说明这份 PDF 的交叉引用表
    // 已经和正文对不上（这正是以前"kids 插入把编号整体顶掉一位"的病）。
    auto object_body = [&](int object_number, std::string& body) -> bool {
        if (object_number <= 0 || object_number >= static_cast<int>(entry_count)) return false;
        if (kinds[static_cast<std::size_t>(object_number)] != 'n') return false;
        const std::size_t offset = static_cast<std::size_t>(offsets[static_cast<std::size_t>(object_number)]);
        if (offset >= data.size()) return false;
        const std::string head = std::to_string(object_number) + " 0 obj";
        if (data.compare(offset, head.size(), head) != 0) return false;
        const std::size_t end = data.find("endobj", offset);
        if (end == std::string::npos) return false;
        body = data.substr(offset, end - offset);
        return true;
    };

    auto require_body = [&](int object_number, const std::string& what, std::string& body) -> bool {
        std::string head = std::to_string(object_number) + " 0 obj";
        if (object_number <= 0 || object_number >= static_cast<int>(entry_count) ||
            kinds[static_cast<std::size_t>(object_number)] != 'n') {
            result.error = what + " 引用的对象 " + std::to_string(object_number) + " 在 xref 里不存在";
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(offsets[static_cast<std::size_t>(object_number)]);
        if (offset >= data.size() || data.compare(offset, head.size(), head) != 0) {
            result.error = what + " 引用的对象 " + std::to_string(object_number) +
                           " 偏移无效（xref 指向的位置不是它）";
            return false;
        }
        if (!object_body(object_number, body)) {
            result.error = what + " 引用的对象 " + std::to_string(object_number) + " 读不出正文";
            return false;
        }
        return true;
    };

    // ---- trailer ----
    const std::size_t trailer_pos = data.find("trailer", static_cast<std::size_t>(xref_offset));
    if (trailer_pos == std::string::npos) {
        result.error = "找不到 trailer";
        return result;
    }
    std::size_t trailer_end = data.find("startxref", trailer_pos);
    if (trailer_end == std::string::npos) trailer_end = data.size();
    const std::string trailer = data.substr(trailer_pos, trailer_end - trailer_pos);

    long long size_value = 0;
    long long root_value = 0;
    if (!FindIntValue(trailer, "/Size", 0, trailer.size(), size_value)) {
        result.error = "trailer 里没有 /Size";
        return result;
    }
    if (!FindIntValue(trailer, "/Root", 0, trailer.size(), root_value)) {
        result.error = "trailer 里没有 /Root";
        return result;
    }
    if (size_value != entry_count) {
        result.error = "trailer /Size=" + std::to_string(size_value) +
                       " 与 xref 条目数 " + std::to_string(entry_count) + " 不一致";
        return result;
    }
    result.root_object = static_cast<int>(root_value);

    // ---- Catalog -> Pages ----
    std::string catalog;
    if (!require_body(result.root_object, "/Root", catalog)) return result;
    if (catalog.find("/Type /Catalog") == std::string::npos) {
        result.error = "/Root 指向的对象不是 /Type /Catalog";
        return result;
    }
    long long pages_value = 0;
    if (!FindIntValue(catalog, "/Pages", 0, catalog.size(), pages_value)) {
        result.error = "Catalog 里没有 /Pages";
        return result;
    }
    result.pages_object = static_cast<int>(pages_value);

    std::string pages;
    if (!require_body(result.pages_object, "/Pages", pages)) return result;
    if (pages.find("/Type /Pages") == std::string::npos) {
        result.error = "/Pages 指向的对象不是 /Type /Pages";
        return result;
    }
    long long count_value = 0;
    if (!FindIntValue(pages, "/Count", 0, pages.size(), count_value)) {
        result.error = "/Pages 里没有 /Count";
        return result;
    }
    result.page_count = static_cast<int>(count_value);

    const std::size_t kids_pos = pages.find("/Kids");
    if (kids_pos == std::string::npos) {
        result.error = "/Pages 里没有 /Kids";
        return result;
    }
    const std::size_t bracket_open = pages.find('[', kids_pos);
    const std::size_t bracket_close = pages.find(']', bracket_open);
    if (bracket_open == std::string::npos || bracket_close == std::string::npos) {
        result.error = "/Kids 不是数组";
        return result;
    }
    result.page_objects = CollectRefs(pages, bracket_open, bracket_close);
    if (result.page_objects.empty()) {
        result.error = "/Kids 里没有任何页面对象";
        return result;
    }
    if (static_cast<int>(result.page_objects.size()) != result.page_count) {
        result.error = "/Count=" + std::to_string(result.page_count) + " 与 /Kids 里的 " +
                       std::to_string(result.page_objects.size()) + " 个页面对不上";
        return result;
    }

    // ---- 每一页 ----
    for (const int page_object : result.page_objects) {
        std::string page;
        if (!require_body(page_object, "/Kids", page)) return result;
        if (page.find("/Type /Page") == std::string::npos) {
            result.error = "对象 " + std::to_string(page_object) + " 不是 /Type /Page";
            return result;
        }
        long long parent_value = 0;
        if (FindIntValue(page, "/Parent", 0, page.size(), parent_value) &&
            parent_value != pages_value) {
            result.error = "页面 " + std::to_string(page_object) + " 的 /Parent=" +
                           std::to_string(parent_value) + " 与 /Pages 对象号 " +
                           std::to_string(pages_value) + " 不一致";
            return result;
        }
        long long contents_value = 0;
        if (!FindIntValue(page, "/Contents", 0, page.size(), contents_value)) {
            result.error = "页面 " + std::to_string(page_object) + " 没有 /Contents";
            return result;
        }
        result.content_objects.push_back(static_cast<int>(contents_value));

        const std::size_t font_pos = page.find("/Font");
        if (font_pos != std::string::npos) {
            const std::size_t dict_open = page.find("<<", font_pos);
            const std::size_t dict_close = page.find(">>", dict_open);
            if (dict_open != std::string::npos && dict_close != std::string::npos) {
                for (const int font_object : CollectRefs(page, dict_open, dict_close)) {
                    std::string font;
                    if (!require_body(font_object, "/Font", font)) return result;
                    if (font.find("/Type /Font") == std::string::npos) {
                        result.error = "对象 " + std::to_string(font_object) + " 不是 /Type /Font";
                        return result;
                    }
                    bool seen = false;
                    for (const int existing : result.font_objects) {
                        if (existing == font_object) { seen = true; break; }
                    }
                    if (!seen) result.font_objects.push_back(font_object);
                }
            }
        }
    }
    if (result.font_objects.empty()) {
        result.error = "没有任何可用的 /Font 对象（页面资源里没有字体）";
        return result;
    }

    // ---- 内容流 ----
    for (const int content_object : result.content_objects) {
        std::string content;
        if (!require_body(content_object, "/Contents", content)) return result;
        long long length_value = 0;
        if (!FindIntValue(content, "/Length", 0, content.size(), length_value)) {
            result.error = "内容对象 " + std::to_string(content_object) + " 没有 /Length";
            return result;
        }
        const std::size_t stream_pos = content.find("stream");
        if (stream_pos == std::string::npos) {
            result.error = "内容对象 " + std::to_string(content_object) + " 没有 stream";
            return result;
        }
        std::size_t data_begin = stream_pos + 6;
        if (data_begin < content.size() && content[data_begin] == '\r') ++data_begin;
        if (data_begin < content.size() && content[data_begin] == '\n') ++data_begin;
        const std::size_t stream_end = content.find("endstream", data_begin);
        if (stream_end == std::string::npos) {
            result.error = "内容对象 " + std::to_string(content_object) + " 没有 endstream";
            return result;
        }
        std::size_t data_end = stream_end;
        while (data_end > data_begin &&
               (content[data_end - 1] == '\n' || content[data_end - 1] == '\r')) {
            --data_end;
        }
        const long long actual = static_cast<long long>(data_end - data_begin);
        if (actual != length_value) {
            ++result.stream_length_mismatch;
            result.error = "内容对象 " + std::to_string(content_object) + " 的 /Length=" +
                           std::to_string(length_value) + " 与实际 " + std::to_string(actual) +
                           " 字节不一致";
            return result;
        }
        ExtractContentText(content.substr(data_begin, stream_end - data_begin), result);
    }

    result.text_lines = (result.text_runs > 0) ? (result.line_advances + 1) : 0;
    result.ok = true;
    return result;
}

}  // namespace videoeye_test
