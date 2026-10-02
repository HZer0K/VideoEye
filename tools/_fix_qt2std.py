#!/usr/bin/env python3
"""把 compiler 报出的「QString 直接赋给 domain std::string」修掉。"""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def patch(rel, pairs):
    p = os.path.join(ROOT, rel)
    s = io.open(p, encoding="utf-8").read()
    for old, new in pairs:
        if old not in s:
            print("  MISS %s :: %s" % (rel, old[:70]))
            continue
        s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    print("  patched %s (%d)" % (rel, len(pairs)))


patch(r"core\analysis\container\AviStructureAnalyzer.cpp", [
    ('                si.codec = fcc_handler.trimmed();',
     '                si.codec = fcc_handler.trimmed().toStdString();'),
    ('                    if (!codecName.isEmpty()) s.codec = codecName;',
     '                    if (!codecName.isEmpty()) s.codec = codecName.toStdString();'),
    ('                    s.codec = codecName;',
     '                    s.codec = codecName.toStdString();'),
])

patch(r"core\analysis\container\OggStructureAnalyzer.cpp", [
    ('    result.file_path = file_path;',
     '    result.file_path = file_path.toStdString();'),
    ('        csi.codec = info.codec_name;',
     '        csi.codec = info.codec_name.toStdString();'),
    ('        csi.details = detail;',
     '        csi.details = detail.toStdString();'),
])

patch(r"core\analysis\container\TsStructureAnalyzer.cpp", [
    ('                        pes_elem.value = v;',
     '                        pes_elem.value = v.toStdString();'),
])

# PushField / PushFieldInt 的 name 参数被写成 const char*，
# 但调用点已经改成 std::string 拼接（"entry[" + std::to_string(i) + "]"）。
patch(r"core\analysis\container\Mp4BoxAnalyzer.cpp", [
    ('void PushField(model::Mp4BoxNode& node, const char* name, const std::string& value) {',
     'void PushField(model::Mp4BoxNode& node, const std::string& name, const std::string& value) {'),
    ('void PushFieldInt(model::Mp4BoxNode& node, const char* name, uint64_t value) {',
     'void PushFieldInt(model::Mp4BoxNode& node, const std::string& name, uint64_t value) {'),
])
