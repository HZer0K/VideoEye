# -*- coding: utf-8 -*-
"""一次性脚本：把 EbmlAnalyzer 的 Qt 字符串/容器写法迁到 std，去掉 domain 边界上的 Qt 依赖。"""
import io
import sys

P = "core/analysis/container/EbmlAnalyzer.cpp"
s = io.open(P, encoding="utf-8", newline="").read()

Q = chr(34)   # 双引号
B = chr(92)   # 反斜杠

reps = []

# --- 文件头：标准库头 ---
reps.append((
    '#include "core/analysis/container/EbmlAnalyzer.h"\n'
    '#include "infrastructure/logging/Logger.h"\n'
    '#include <QFile>',
    '#include "core/analysis/container/EbmlAnalyzer.h"\n'
    '#include "infrastructure/logging/Logger.h"\n\n'
    '#include <QFile>',
))

# --- 元素名表：QMap -> std::map ---
reps.append((
    'QMap<uint64_t, QString>& EbmlAnalyzer::ElementNames() {\n'
    '    static QMap<uint64_t, QString> map;\n'
    '    if (map.isEmpty()) {',
    'std::map<uint64_t, std::string>& EbmlAnalyzer::ElementNames() {\n'
    '    static std::map<uint64_t, std::string> map;\n'
    '    if (map.empty()) {',
))

# --- CodecIdToName ---
reps.append((
    'QString EbmlAnalyzer::CodecIdToName(const QString& codec_id) {',
    'std::string EbmlAnalyzer::CodecIdToName(const std::string& codec_id) {',
))

# --- TrackTypeName ---
reps.append((
    'QString EbmlAnalyzer::TrackTypeName(int type) {\n    switch (type) {',
    'std::string EbmlAnalyzer::TrackTypeName(int type) {\n    switch (type) {',
))
reps.append((
    '        case 1:  return QString("' + '\u89c6\u9891 (Video)"' + ');\n'
    '        case 2:  return QString("' + '\u97f3\u9891 (Audio)"' + ');\n'
    '        case 3:  return QString("' + '\u590d\u5408 (Complex)"' + ');\n'
    '        case 0x10: return QString("Logo");\n'
    '        case 0x11: return QString("' + '\u5b57\u5e55 (Subtitle)"' + ');\n'
    '        case 0x12: return QString("' + '\u6309\u94ae (Buttons)"' + ');\n'
    '        case 0x20: return QString("' + '\u63a7\u5236 (Control)"' + ');\n'
    '        default:  return QString("' + '\u7c7b\u578b%1"' + ').arg(type);',
    '        case 1:  return "' + '\u89c6\u9891 (Video)"' + '";\n'
    '        case 2:  return "' + '\u97f3\u9891 (Audio)"' + '";\n'
    '        case 3:  return "' + '\u590d\u5408 (Complex)"' + '";\n'
    '        case 0x10: return "Logo";\n'
    '        case 0x11: return "' + '\u5b57\u5e55 (Subtitle)"' + '";\n'
    '        case 0x12: return "' + '\u6309\u94ae (Buttons)"' + '";\n'
    '        case 0x20: return "' + '\u63a7\u5236 (Control)"' + '";\n'
    '        default:  return "' + '\u7c7b\u578b' + '" + std::to_string(type);',
))

# --- ElementName ---
reps.append((
    'QString EbmlAnalyzer::ElementName(uint64_t id) {\n'
    '    auto& names = ElementNames();\n'
    '    auto it = names.find(id);\n'
    '    if (it != names.end()) return it.value();\n'
    "    if (id <= 0xFF) return QString(\"0x%1\").arg(id, 2, 16, QChar('0'));\n"
    "    if (id <= 0xFFFF) return QString(\"0x%1\").arg(id, 4, 16, QChar('0'));\n"
    '    return QString("0x%1").arg(id, 0, 16);\n'
    '}',
    'std::string EbmlAnalyzer::ElementName(uint64_t id) {\n'
    '    auto& names = ElementNames();\n'
    '    auto it = names.find(id);\n'
    '    if (it != names.end()) return it->second;\n'
    '    std::ostringstream oss;\n'
    '    oss << "0x" << std::hex << std::nouppercase << std::setfill(\'0\')\n'
    '        << std::setw(id <= 0xFF ? 2 : (id <= 0xFFFF ? 4 : 0)) << id;\n'
    '    return oss.str();\n'
    '}',
))

# --- ParseBlockData ---
reps.append((
    'QString EbmlAnalyzer::ParseBlockData(const QByteArray& data, model::EbmlBlockSummary& summary) {',
    'std::string EbmlAnalyzer::ParseBlockData(const QByteArray& data,\n'
    '                                        model::EbmlBlockSummary& summary) {',
))
reps.append(('    QString lacetype;', '    std::string lacetype;'))
reps.append((
    '    return QString("' + 'Track=%1 Timecode=%2 Flags=0x%3%4%5%6 [%7 bytes]"' + ')\n'
    '        .arg(summary.track_number)\n'
    '        .arg(summary.timecode)\n'
    "        .arg(flags, 2, 16, QChar('0'))\n"
    '        .arg(summary.keyframe ? " KEY" : "")\n'
    '        .arg(summary.discardable ? " DISCARD" : "")\n'
    '        .arg(lacetype)\n'
    '        .arg(summary.data_size);',
    '    std::ostringstream oss;\n'
    '    oss << "Track=" << summary.track_number\n'
    '        << " Timecode=" << summary.timecode\n'
    '        << " Flags=0x" << std::hex << std::nouppercase << std::setfill(\'0\')\n'
    "        << std::setw(2) << static_cast<unsigned>(flags) << std::setfill(' ') << std::dec\n"
    '        << (summary.keyframe ? " KEY" : "")\n'
    '        << (summary.discardable ? " DISCARD" : "")\n'
    '        << lacetype\n'
    '        << " [" << summary.data_size << " bytes]";\n'
    '    return oss.str();',
))

# --- ParseSimpleBlockData ---
reps.append((
    'QString EbmlAnalyzer::ParseSimpleBlockData(const QByteArray& data, model::EbmlBlockSummary& summary) {',
    'std::string EbmlAnalyzer::ParseSimpleBlockData(const QByteArray& data,\n'
    '                                              model::EbmlBlockSummary& summary) {',
))

# --- tryString: 保留 QString 做 UTF-8 校验, 出口转 std::string ---
reps.append((
    '    auto tryString = [&]() -> QString {\n'
    '        QString s = QString::fromUtf8(data);\n'
    '        if (s.isEmpty()) return {};\n'
    '        for (int i = 0; i < s.size(); ++i) {\n'
    '            ushort ch = s[i].unicode();\n'
    '            if (ch == 0xFFFD) return {};\n'
    "            if (ch < 0x20 && ch != '\\n' && ch != '\\r' && ch != '\\t') return {};\n"
    '        }\n'
    '        return s;\n'
    '    };',
    '    auto tryString = [&]() -> std::string {\n'
    '        const QString s = QString::fromUtf8(data);\n'
    '        if (s.isEmpty()) return {};\n'
    '        for (int i = 0; i < s.size(); ++i) {\n'
    '            ushort ch = s[i].unicode();\n'
    '            if (ch == 0xFFFD) return {};\n'
    "            if (ch < 0x20 && ch != '\\n' && ch != '\\r' && ch != '\\t') return {};\n"
    '        }\n'
    '        return s.toStdString();\n'
    '    };',
))

# --- node.value 数值 ---
reps.append(('node.value = QString::number(', 'node.value = std::to_string('))
reps.append((
    "node.value = QString::number(readBeFloat(data, 4), 'f', 6); return;",
    'node.value = std::to_string(readBeFloat(data, 4)); return;',
))
reps.append(("node.value = QString::number(dur, 'f', 6);", 'node.value = std::to_string(dur);'))
reps.append((
    'node.value += QString("' + ' (%1:%2:%3)"' + ').arg(h, 2, 10, QChar(\'0\')).arg(m, 2, 10, QChar(\'0\')).arg(s, 2, 10, QChar(\'0\'));',
    'node.value += " (" + Pad2(h) + ":" + Pad2(m) + ":" + Pad2(s) + ")";',
))
reps.append((
    '{ node.value = data.toHex(); if (node.id == 0x73A4) result.segment_uid = node.value; return; }',
    '{ node.value = data.toHex().toStdString(); if (node.id == 0x73A4) result.segment_uid = node.value; return; }',
))
reps.append((
    'node.value = QString("' + '\u4e8c\u8fdb\u5236 %1 \u5b57\u8282' + '").arg(data.size()); return;',
    'node.value = "' + '\u4e8c\u8fdb\u5236 ' + '" + std::to_string(data.size()) + " ' + '\u5b57\u8282' + '"; return;',
))

# --- 尾部字符串判空/赋值 ---
reps.append((
    '    QString s = tryString();\n'
    '    if (!s.isEmpty() && s.size() <= 256) { node.value = s; return; }\n'
    '    if (data.size() <= 64) { node.value = data.toHex(); return; }\n'
    '    node.value = data.left(32).toHex() + "...(" + QString::number(data.size()) + " bytes)";',
    '    std::string s = tryString();\n'
    '    if (!s.empty() && s.size() <= 256) { node.value = s; return; }\n'
    '    if (data.size() <= 64) { node.value = data.toHex().toStdString(); return; }\n'
    '    node.value = data.left(32).toHex().toStdString() + "...(" + std::to_string(data.size()) + " bytes)";',
))

# --- 遍历容器类型 ---
reps.append(('QVector<model::EbmlElementNode>', 'std::vector<model::EbmlElementNode>'))

# --- 叶子节点数值转移 ---
reps.append(('n.value.toInt()', 'ParseInt(n.value)'))
reps.append(('n.value.toULongLong()', 'ParseUInt(n.value)'))
reps.append(('n.value.toDouble()', 'ParseDouble(n.value)'))

# --- id_hex / last() ---
reps.append((
    'node.id_hex = QString("0x%1").arg(id, 0, 16);',
    '{\n'
    '            std::ostringstream idoss;\n'
    '            idoss << "0x" << std::hex << std::nouppercase << id;\n'
    '            node.id_hex = idoss.str();\n'
    '        }',
))
reps.append((
    'model::EbmlElementNode& child = parent->children.last();',
    'model::EbmlElementNode& child = parent->children.back();',
))

# --- Analyze 收尾 ---
reps.append(('    result.file_path = filePath;', '    result.file_path = filePath.toStdString();'))
reps.append((
    '        result.error_message = QString("' + '\u65e0\u6cd5\u6253\u5f00\u6587\u4ef6: %1' + '").arg(filePath);\n'
    '        LOG_ERROR(result.error_message.toStdString());',
    '        result.error_message = "' + '\u65e0\u6cd5\u6253\u5f00\u6587\u4ef6: ' + '" + filePath.toStdString();\n'
    '        LOG_ERROR(result.error_message);',
))
reps.append((
    'result.error_message = QString("' + '\u4e0d\u662f\u6709\u6548\u7684 EBML/Matroska/WebM \u6587\u4ef6' + '");',
    'result.error_message = "' + '\u4e0d\u662f\u6709\u6548\u7684 EBML/Matroska/WebM \u6587\u4ef6' + '";',
))
reps.append((
    'LOG_INFO("EBML ' + '\u5206\u6790\u5b8c\u6210: " + result.doc_type.toStdString()',
    'LOG_INFO("EBML ' + '\u5206\u6790\u5b8c\u6210: " + result.doc_type',
))

missing = []
for a, b in reps:
    if a not in s:
        missing.append(a.strip().split("\n")[0][:70])
    s = s.replace(a, b)

# --- 注入 Parse* / Pad2 辅助 ---
ANCHOR = 'namespace videoeye {\nnamespace analyzer {\n'
HELPERS = ANCHOR + '''
namespace {

// 替代 QString::toInt()/toULongLong()/toDouble() 的宽松语义
// （解析失败返回默认值，而不是抛异常），保持 EbmlAnalyzer 原有行为。
int64_t ParseInt(const std::string& s, int64_t def = 0) {
    try { return std::stoll(s); } catch (...) { return def; }
}
uint64_t ParseUInt(const std::string& s, uint64_t def = 0) {
    try { return std::stoull(s); } catch (...) { return def; }
}
double ParseDouble(const std::string& s, double def = 0.0) {
    try { return std::stod(s); } catch (...) { return def; }
}

// 补零到两位，替代 QString::arg(v, 2, 10, QChar('0'))
std::string Pad2(long long v) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << v;
    return oss.str();
}

}  // namespace
'''
if ANCHOR not in s:
    missing.append('<ANCHOR>')
s = s.replace(ANCHOR, HELPERS, 1)

# --- 补标准库头 ---
if '#include <sstream>' not in s:
    s = s.replace('#include <QBuffer>\n',
                  '#include <QBuffer>\n\n#include <iomanip>\n#include <sstream>\n#include <string>\n', 1)

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("MISSING:", missing)
print("QString:", s.count("QString"), "QChar:", s.count("QChar"),
      "QVector:", s.count("QVector"), "QMap:", s.count("QMap"), "QByteArray:", s.count("QByteArray"))
