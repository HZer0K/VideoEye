# -*- coding: utf-8 -*-
"""修 Asf / Avi / Flv / Ogg / Ts 五个结构分析器：Qt 字符串 -> domain 边界转换。

1) 先做显式多行替换（assert 命中）；
2) 再对「单行语句、RHS 以 QString 开头」补 .toStdString()；
3) 最后 domain 容器 append -> push_back。
"""
import io
import re
import sys

FILES = ["core/analysis/container/AsfStructureAnalyzer.cpp",
         "core/analysis/container/AviStructureAnalyzer.cpp",
         "core/analysis/container/FlvStructureAnalyzer.cpp",
         "core/analysis/container/OggStructureAnalyzer.cpp",
         "core/analysis/container/TsStructureAnalyzer.cpp"]

FIELD = (r'(?:name|type|value|extra|details|codec|summary|error_message|'
         r'file_path|track_type_name|codec_name|language|track_name|id_hex|doc_type|'
         r'message|title|resolution|uri)')

# 多行/特殊处理，逐条断言
SPECIFIC = {
    "core/analysis/container/AsfStructureAnalyzer.cpp": [
        ('    result.file_path = file_path;', '    result.file_path = file_path.toStdString();'),
    ],
    "core/analysis/container/AviStructureAnalyzer.cpp": [
        ('    result.file_path = file_path;', '    result.file_path = file_path.toStdString();'),
    ],
    "core/analysis/container/FlvStructureAnalyzer.cpp": [
        ('    result.file_path = file_path;', '    result.file_path = file_path.toStdString();'),
    ],
    "core/analysis/container/OggStructureAnalyzer.cpp": [
        ('    result.file_path = file_path;', '    result.file_path = file_path.toStdString();'),
    ],
    "core/analysis/container/TsStructureAnalyzer.cpp": [
        ('    result.file_path = file_path;', '    result.file_path = file_path.toStdString();'),
        ('    result.summary = QString("MPEG-TS | %1 包/已扫描 | %2 节目 | %3 流 | %4 PES 采样")\n'
         '                         .arg(total_packets).arg(pat_programs.size())\n'
         '                         .arg(result.streams.size()).arg(total_pes);',
         '    result.summary = (QString("MPEG-TS | %1 包/已扫描 | %2 节目 | %3 流 | %4 PES 采样")\n'
         '                          .arg(total_packets).arg(pat_programs.size())\n'
         '                          .arg(result.streams.size()).arg(total_pes))\n'
         '                         .toStdString();'),
    ],
    "core/analysis/container/FlvStructureAnalyzer.cpp": [
        ('    QString summary = QString("FLV | Video Tags: %1 | Audio Tags: %2 | Script Tags: %3")\n'
         '                          .arg(video_tags).arg(audio_tags).arg(script_tags);\n'
         '    if (result.metadata.contains("width") && result.metadata.contains("height"))\n'
         '        summary += QString(" | %1x%2").arg(result.metadata["width"], result.metadata["height"]);\n'
         '    if (result.metadata.contains("duration"))\n'
         '        summary += QString(" | %1s").arg(result.metadata["duration"]);\n'
         '    result.summary = summary;',
         '    std::string summary = "FLV | Video Tags: " + std::to_string(video_tags) +\n'
         '                          " | Audio Tags: " + std::to_string(audio_tags) +\n'
         '                          " | Script Tags: " + std::to_string(script_tags);\n'
         '    if (result.metadata.count("width") && result.metadata.count("height"))\n'
         '        summary += " | " + result.metadata["width"] + "x" + result.metadata["height"];\n'
         '    if (result.metadata.count("duration"))\n'
         '        summary += " | " + result.metadata["duration"] + "s";\n'
         '    result.summary = summary;'),
    ],
    "core/analysis/container/OggStructureAnalyzer.cpp": [
        ('// 解析 Vorbis/Opus 注释块 (vendor + KEY=VALUE 列表)，从 start 起\n'
         'void parseVorbisComments(const QByteArray& d, int start, QMap<QString, QString>& out) {\n'
         '    int pos = start;\n'
         '    if (pos + 4 > d.size()) return;\n'
         '    uint32_t vlen = oggLE32(d, pos); pos += 4;\n'
         '    if (pos + static_cast<int>(vlen) > d.size()) return;\n'
         '    QString vendor = QString::fromUtf8(d.mid(pos, vlen));\n'
         '    pos += vlen;\n'
         '    if (!vendor.isEmpty()) out["vendor"] = vendor;\n'
         '    if (pos + 4 > d.size()) return;\n'
         '    uint32_t count = oggLE32(d, pos); pos += 4;\n'
         '    for (uint32_t i = 0; i < count && pos + 4 <= d.size(); ++i) {\n'
         '        uint32_t clen = oggLE32(d, pos); pos += 4;\n'
         '        if (pos + static_cast<int>(clen) > d.size()) break;\n'
         '        QString comment = QString::fromUtf8(d.mid(pos, clen));\n'
         '        pos += clen;\n'
         '        int eq = comment.indexOf(\'=\');\n'
         '        if (eq > 0) {\n'
         '            QString k = comment.left(eq).toUpper();\n'
         '            QString v = comment.mid(eq + 1);\n'
         '            if (!out.contains(k)) out[k] = v;\n'
         '        }\n'
         '    }\n'
         '}',
         '// 解析 Vorbis/Opus 注释块 (vendor + KEY=VALUE 列表)，从 start 起\n'
         '// 结果直接写进 domain 的 metadata（std::string），所以内部一律用 std 类型。\n'
         'void parseVorbisComments(const QByteArray& d, int start,\n'
         '                        std::map<std::string, std::string>& out) {\n'
         '    int pos = start;\n'
         '    if (pos + 4 > d.size()) return;\n'
         '    uint32_t vlen = oggLE32(d, pos); pos += 4;\n'
         '    if (pos + static_cast<int>(vlen) > d.size()) return;\n'
         '    const std::string vendor = QString::fromUtf8(d.mid(pos, vlen)).toStdString();\n'
         '    pos += vlen;\n'
         '    if (!vendor.empty()) out["vendor"] = vendor;\n'
         '    if (pos + 4 > d.size()) return;\n'
         '    uint32_t count = oggLE32(d, pos); pos += 4;\n'
         '    for (uint32_t i = 0; i < count && pos + 4 <= d.size(); ++i) {\n'
         '        uint32_t clen = oggLE32(d, pos); pos += 4;\n'
         '        if (pos + static_cast<int>(clen) > d.size()) break;\n'
         '        const std::string comment = QString::fromUtf8(d.mid(pos, clen)).toStdString();\n'
         '        pos += clen;\n'
         '        size_t eq = comment.find(\'=\');\n'
         '        if (eq != std::string::npos && eq > 0) {\n'
         '            std::string k = comment.substr(0, eq);\n'
         '            std::transform(k.begin(), k.end(), k.begin(),\n'
         '                           [](unsigned char c) { return std::toupper(c); });\n'
         '            std::string v = comment.substr(eq + 1);\n'
         '            if (out.count(k) == 0) out[k] = v;\n'
         '        }\n'
         '    }\n'
         '}'),
    ],
}

# 单行：RHS 以 QString 开头 -> 补 .toStdString()
LINE_RE = re.compile(r'^(\s*(?:[\w\.\-\>\]\)]+)\.' + FIELD +
                     r'\s*(?:\+=|=)\s*)(QString\b)(.*)$')

SWEEP = [
    ('.children.append(', '.children.push_back('),
    ('.element_tree.append(', '.element_tree.push_back('),
    ('.streams.append(', '.streams.push_back('),
]

STATUS = 0
for path in FILES:
    s = io.open(path, encoding="utf-8", newline="").read()
    for a, b in SPECIFIC.get(path, []):
        if a not in s:
            print("MISSING in %s: %s" % (path, a.strip().split("\n")[0][:70]))
            STATUS = 1
            continue
        s = s.replace(a, b, 1)

    # 单行补转换
    lines = s.split("\n")
    for i, ln in enumerate(lines):
        m = LINE_RE.match(ln)
        if not m:
            continue
        if ".toStdString()" in ln:
            continue
        lines[i] = m.group(1) + "QString" + m.group(3).rstrip() + ".toStdString();"
    s = "\n".join(lines)

    for a, b in SWEEP:
        s = s.replace(a, b)

    if path.endswith("OggStructureAnalyzer.cpp"):
        s = s.replace('#include <QFile>\n',
                      '#include <QFile>\n\n#include <algorithm>\n#include <cctype>\n'
                      '#include <map>\n#include <string>\n', 1)

    io.open(path, "w", encoding="utf-8", newline="").write(s)
    print("done", path)

raise SystemExit(STATUS)
