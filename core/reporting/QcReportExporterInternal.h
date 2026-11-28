#pragma once

// QcReportExporter 家族跨编译单元共享的小工具。
//
// 这些 helper 原本都挤在 QcReportExporter.cpp 的匿名 namespace 里，物理拆分后
// 有 2 个以上 TU 要用，所以提取到这里。全部 inline（头内定义），不会有 ODR 问题。
//
// 只被单个格式用到的 helper（Html / kRowGroupSeparator / JSON 的 JNumber / JText
// 及各种 builder）继续留在各自 TU 的匿名 namespace 里，不进这里。
//
// CsvField 例外：它原来是 QcReportExporterCsv.cpp 的局部 helper，但参考帧质量对比
// 的导出器（ReferenceQualityExporter）也要写同一套 CSV，而「按 RFC 4180 转义 + 中和
// 表格软件的公式注入」这件事抄第二份迟早会漏一处 —— 所以它是共享的。

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "core/domain/io/Utf8Path.h"
#include "core/reporting/QcReportExporter.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace reporting {
namespace detail {

// JSON schema 的版本号。只要字段语义有破坏性变化就加一，
// CI 侧据此决定要不要兼容旧结构。
inline constexpr int kReportSchemaVersion = 1;

inline std::string Fixed(double value, int precision) {
    if (!std::isfinite(value))
        return std::string();
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(precision) + "f").c_str(), value);
    return std::string(buf);
}

inline std::string Int64Text(long long value) {
    return std::to_string(value);
}

// CSV/PDF 里换行会把一行拆成两行，而 detail/suggestion 里有换行（如 FFmpeg 命令行示例）。
inline std::string SingleLine(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c == '\n' || c == '\r')
            c = ' ';
    }
    return out;
}

// CSV 转义（RFC 4180）：字段整体用双引号包住，内部的双引号翻倍。
//
// 另外对以 = + - @ 开头的字段前置一个单引号：Excel / LibreOffice / Google Sheets
// 会把这样的单元格当成公式执行（=cmd|'/c ...、@SUM(...) 等），导出的报告会被
// 打开它的表格软件执行的载荷，前置 ' 之后它只是普通文本。
//
// ❗ 单引号必须落在**引号之内**。写在引号外面（先上 ' 再上 "）会让字段的第一个
// 字符变成 '，RFC 4180 解析器据此判定"这个字段没有加引号"，于是把后面那对引号
// 当成普通字符原样保留 —— 实测占位符 "-" 会被读成 "'-"，值里带引号时更乱。
//
// 只用于**文本**列。数字列（含负数）直接写原值，不要过这里 —— 否则 "-18.5"
// 会变成 "'-18.5"，读回来就不是数了。
inline std::string CsvField(const std::string& text) {
    std::string out = "\"";
    if (!text.empty()) {
        const char first = text.front();
        if (first == '=' || first == '+' || first == '-' || first == '@')
            out += '\'';
    }
    for (char c : text) {
        if (c == '"')
            out += "\"\"";
        else
            out += c; // 逗号/换行/CR 本来就落在引号里面，属于 RFC 4180 的合法转义
    }
    out += '"';
    return out;
}

inline bool WriteUtf8File(const std::string& path, const std::string& content) {
    std::ofstream file(ToNativePath(path), std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("无法写入报告文件: " + path);
        return false;
    }
    file << content;
    return file.good();
}

// 把任意 UTF-8 纯文本按 PDF 版式写出。MiniPdfWriter 在 QcReportExporterPdf.cpp 的
// 匿名 namespace 里，所以实现也放那里；三个报告家族（单文件 / 对比 / 批量汇总）
// 的 PDF 都走这一个入口，避免各自另排一套正文之后与自家 TXT 漂移。
QcPdfExportResult WriteTextAsPdf(const std::string& path, const std::string& text);

} // namespace detail
} // namespace reporting
} // namespace videoeye