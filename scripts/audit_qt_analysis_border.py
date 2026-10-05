#!/usr/bin/env python3
"""审计：core/analysis 全层不许出现任何 Qt。

背景
----
docs/ARCHITECTURE.md 里的分层方向是 domain -> media/ffmpeg_io -> analysis -> ...，
analysis 被允许看懂 FFmpeg 与 domain，但**没有任何理由认识 Qt** —— Qt 是 UI 侧的
东西，字符串与二进制缓冲在 analysis 里一律用 std::string / std::vector。

以前这层靠两道门守：
  * scripts/check_layering.py  —— 只匹配 #include "..."，Qt 是尖括号
    （#include <QString>），它根本看不见；
  * scripts/audit_qt_domain_border.py —— 只盯"Qt 类型有没有落进 domain 的
    std::string 字段"，是字段级的，不管 analysis 自己的内部用法。
于是 analysis 里积了 21 个文件、468 处 Qt 用法（QString 324、QByteArray 81 占大头），
其中 8 个公开头文件直接把 QString 写进了签名，谁 include 谁就被迫拉 Qt6::Core。

本脚本补的是第三道：**文件级 + 符号级**，只要 analysis 里出现 Qt 的 include 或
Qt 类型名，就命中。迁移完成后这层应当恒为 0 命中。

用法：
    python scripts/audit_qt_analysis_border.py

退出码：0 = 干净；1 = 有命中（CI / pre-commit 拦下）。
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.environ.get("VIDEOEYE_ROOT") or os.getcwd()
SCAN_ROOT = os.path.join(ROOT, "core", "analysis")

# Qt 类型 / 枚举 / 名字空间的符号面。
# 只列 analysis 里真实出现过的（QString QByteArray QFile QFileInfo QDataStream
# QIODevice QMap QVector QSet QStack QChar QUuid QBuffer QStringList QtEndian），
# 加上"真要写 Qt 时最容易顺手抓来用"的那一批（QObject QMutex QThread QDir
# QElapsedTimer QTextStream QList QVariant），免得下次写进来时没人守。
QT_SYMBOLS = [
    "QStringList", "QStringView", "QString", "QByteArray", "QChar", "QFileInfo",
    "QFile", "QDataStream", "QIODevice", "QTextStream", "QDir", "QBuffer",
    "QElapsedTimer", "QThread", "QObject", "QMutex", "QVariant", "QSet",
    "QStack", "QMap", "QVector", "QList", "QUuid", "QtEndian", "QtGlobal",
    # QtGlobal 的定型别名与最小/最大宏：它们比 QString 更隐蔽，不写进来就漏了 ——
    # 分析层里 qint64 有 30 处、qMin 5 处，光换掉 QString 而留着它们，
    # 这一层照样离不开 Qt（qint64 就是 qlonglong，Qt 自带的定型别名）。
    "qint8", "qint16", "qint32", "qint64", "qintptr",
    "quint8", "quint16", "quint32", "quint64", "quintptr",
    "qreal", "qulonglong",
    "qMin", "qMax", "qAbs", "qBound", "qSwap",
]

RE_INCLUDE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
RE_SYMBOL = re.compile(r"\b(" + "|".join(QT_SYMBOLS) + r")\b")


def walk() -> list[str]:
    out = []
    for dirpath, _dirs, names in os.walk(SCAN_ROOT):
        for n in names:
            if n.endswith((".h", ".cpp")):
                out.append(os.path.join(dirpath, n))
    return sorted(out)


def scan(path: str) -> list[tuple[int, str, str]]:
    hits: list[tuple[int, str, str]] = []
    text = open(path, encoding="utf-8", errors="ignore").read()
    # 去掉注释：文档里提到 "QString" 不构成依赖（check_layering.py 同一做法）。
    body = re.sub(r"//[^\n]*", "", text)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)

    for idx, line in enumerate(body.splitlines(), 1):
        for inc in RE_INCLUDE.findall(line):
            if inc.split("/")[0].startswith("Qt") or inc.startswith("Qt"):
                hits.append((idx, "include <%s>" % inc, line.strip()[:100]))
        for sym in RE_SYMBOL.findall(line):
            hits.append((idx, "符号 %s" % sym, line.strip()[:100]))
    return hits


def main() -> int:
    if not os.path.isdir(SCAN_ROOT):
        print("[错误] 找不到 core/analysis，VIDEOEYE_ROOT = %r" % ROOT)
        return 1

    total = 0
    per_file: dict[str, list[tuple[int, str, str]]] = {}
    for p in walk():
        hits = scan(p)
        if hits:
            # 相对扫描根而不是仓库根来显示路径：自测会把 SCAN_ROOT 指到临时目录，
            # 那个目录常常在另一个盘符上，relpath(跨盘) 在 Windows 直接抛
            # "path is on mount, start on mount"。显示成相对 SCAN_ROOT 更贴合本脚本
            # 的语义（报告"analysis 层里哪个文件"），也顺带避开这个坑。
            per_file[os.path.relpath(p, SCAN_ROOT).replace("\\", "/")] = hits
            total += len(hits)

    count = {s: 0 for s in QT_SYMBOLS}
    for hits in per_file.values():
        for _, why, _ in hits:
            if why.startswith("符号"):
                count[why.split()[1]] += 1

    if not per_file:
        print("[OK] core/analysis 零 Qt")
        print("[退出码] 0 —— 无命中，CI / pre-commit 放行")
        return 0

    print("[命中] core/analysis 里有 Qt 的文件 %d 个、共 %d 处：" % (len(per_file), total))
    for rel in sorted(per_file):
        print("\n--- %s (%d) ---" % (rel, len(per_file[rel])))
        cur = None
        for idx, why, line in per_file[rel]:
            if cur != idx:
                print("  %4d: %s" % (idx, line))
                cur = idx
            print("         ^ %s" % why)
    print("\n按符号汇总：" + ", ".join(
        "%s=%d" % (s, n) for s, n in sorted(count.items(), key=lambda x: -x[1]) if n))
    print("\n[退出码] 1 —— core/analysis 不允许出现 Qt，需迁到 std "
          "(std::string / std::vector / std::ifstream / std::map) 或挪到 UI 侧")
    return 1


if __name__ == "__main__":
    sys.exit(main())
