#!/usr/bin/env python3
"""审计：core 生产侧是否存在「Qt 类型直接写进 domain 的 std::string 字段」。

做法：
1. 扫描 core/domain/model/*.h，抽取所有声明为 std::string 的成员名（含嵌套 struct）。
2. 扫描 core/analysis、core/player、core/exporter、core/media、infrastructure 下的 .cpp/.h，
   找出对域字段的赋值语句，其右值含 QString / QByteArray / QLatin1String / QStringLiteral 等 Qt 类型，
   且未显式 .toStdString()。
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOMAIN_DIR = os.path.join(ROOT, "core", "domain", "model")
SCAN_DIRS = [
    "core/analysis", "core/player", "core/exporter", "core/media",
    "core/ffmpeg", "infrastructure",
]

QT_TYPES_RE = re.compile(r"\b(QString|QByteArray|QLatin1String|QStringLiteral|"
                         r"QStackedString|QStringView)\b")

# 纯局部标识符（不是 obj.field 的成员访问）。
# QString::arg(纯局部变量) 常常只是「局部变量名恰好和某个域字段同名」
# （例如 StreamInfo::codec 的字段名 codec，而本行 codec 是 QString），
# 静态审计拿不到类型，所以这类只降级为「疑似」由人工确认，不当作确定命中。
LOCAL_ONLY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# 这些文件里的 QString 字段属于 Qt 侧自有结构（不是 domain 模型），
# 例如 FfmpegCommandExplainer.h 的 ExplainItem::description、
# FfmpegProcessRunner.h 的 FfmpegRunResult::error_message。
# 它们留在 Qt 层是允许的——只有落进 domain 才违规。
ALLOWED_FILES = {
    "core/ffmpeg/FfmpegProcessRunner.cpp",
    "core/ffmpeg/FfmpegCommandExplainer.cpp",
}
ALLOWED_HINTS = {
    "core/ffmpeg/FfmpegProcessRunner.cpp": "FfmpegRunResult 是 Qt 侧自有结构（QString 字段）",
    "core/ffmpeg/FfmpegCommandExplainer.cpp": "ExplainItem 是 Qt 侧自有结构（QString 字段）",
}

# 已被显式转换的行（行内出现 toStdString / fromStdString 转出）
TO_STD_RE = re.compile(r"\.toStdString\(\)")

# 赋值左值里的域字段名
ASSIGN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*(\.[A-Za-z_][A-Za-z0-9_]*)*\s*=\s*(.+?)(\s*//.*)?$")


def collect_string_fields() -> set[str]:
    """扫描 core/domain 下所有模型头，抽取所有 std::string 成员名（含嵌套 struct）。"""
    fields: set[str] = set()
    pat = re.compile(r"\bstd::string\s+([A-Za-z_][A-Za-z0-9_]*)\s*(;|=)")
    for dirpath, _dirs, names in os.walk(DOMAIN_DIR):
        for n in sorted(names):
            if not n.endswith(".h"):
                continue
            try:
                with open(os.path.join(dirpath, n), "r", encoding="utf-8") as f:
                    for line in f:
                        m = pat.search(line)
                        if m:
                            fields.add(m.group(1))
            except Exception:
                continue
    return fields


def iter_files():
    for d in SCAN_DIRS:
        base = os.path.join(ROOT, d)
        for dirpath, _dirs, names in os.walk(base):
            for n in names:
                if n.endswith((".cpp", ".h")):
                    yield os.path.join(dirpath, n)


def main() -> int:
    fields = collect_string_fields()
    if not fields:
        print("未能从 domain 模型抽取到 std::string 字段")
        return 1
    print("domain std::string 字段样本: %d" % len(fields))

    def norm(rel: str) -> str:
        return rel.replace("\\", "/")

    hits = []
    arg_hits = []     # 高置信：arg() 吃的是 obj.<域字段>
    arg_probable = [] # 疑似：arg() 吃的是纯局部变量（名字与域字段同名）
    stmts: list[tuple[str, int, str]] = []
    field_pat = re.compile(r"\.(" + "|".join(sorted(fields, key=len, reverse=True)) + r")\s*=")
    # 把一条逻辑语句合并成单行后再判定（ MERGE 版本）：
    #   - 跳过注释/预处理器/函数定义行
    #   - 累计到括号配平且以 ';' 结尾为止
    opening = "([{"
    closing = ")]}"

    def scan_path(path: str):
        try:
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
        except Exception:
            return
        buf = ""
        start_line = 0
        for idx, raw in enumerate(text.splitlines(), 1):
            line = raw.strip()
            if not line:
                continue
            if buf == "" and (line.startswith("//") or line.startswith("/*")
                              or line.startswith("*") or line.startswith("#")
                              or line.startswith("}") or line.endswith("{")):
                continue
            if buf == "":
                start_line = idx
            buf += " " + line if buf else line
            bal = sum(buf.count(c) - buf.count(o) for c, o in zip(closing, opening))
            if bal > 0 and not buf.rstrip().endswith(";"):
                continue
            stmt = buf.strip()
            buf = ""
            if not stmt.endswith(";"):
                continue
            stmts.append((os.path.relpath(path, ROOT), start_line, stmt))
            if not field_pat.search(stmt):
                continue
            if TO_STD_RE.search(stmt):
                continue
            if not QT_TYPES_RE.search(stmt):
                continue
            hits.append((os.path.relpath(path, ROOT), start_line, stmt[:160]))

    for p in iter_files():
        scan_path(p)

    # 第二关：QString::arg() 没有 std::string 重载，
    # 形如 .arg(<域字段>) 却没包 QString::fromStdString(...) 的，一律编译不过。
    arg_pat = re.compile(r"\.arg\(([^()]*)\)")
    token_pat = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\b")
    for rel, _idx, stmt in stmts:
        if "arg(" not in stmt:
            continue
        if norm(rel) in ALLOWED_FILES:
            continue  # Qt 侧自有结构，arg 吃 QString 成员是合法的
        exprs = arg_pat.findall(stmt)
        if not exprs:
            continue
        # 注意：这里不能因为整句含 .toStdString() 就跳过——
        # 恰恰是 "...  .arg(域字段).toStdString();" 这类才是真问题。
        for expr in exprs:
            if "fromStdString(" in expr:
                continue
            expr_stripped = expr.strip()
            # arg(obj.<域字段>) —— 元访问，静态可判定
            member_hit = "." in expr_stripped and any(
                tok in fields for tok in token_pat.findall(expr_stripped))
            # arg(局部变量) 且该名字也是域字段名 —— 类型未知，降级为疑似
            local_hit = LOCAL_ONLY_RE.match(expr_stripped) is not None and any(
                tok in fields for tok in token_pat.findall(expr_stripped))
            if member_hit:
                arg_hits.append((rel, stmt[:200]))
            elif local_hit:
                arg_probable.append((rel, expr_stripped, stmt[:200]))
            else:
                continue
            break

    real_hits = [h for h in hits if norm(h[0]) not in ALLOWED_FILES]
    skipped = [h for h in hits if norm(h[0]) in ALLOWED_FILES]

    if not real_hits:
        print("\n[OK] 未发现 Qt 类型直接写进 domain std::string 的赋值")
    else:
        print("\n[命中] 域字段被 Qt 类型污染: %d 处" % len(real_hits))
        cur = None
        for rel, idx, text in real_hits:
            if cur != rel:
                cur = rel
                print("\n--- %s ---" % rel)
            print("  %4d: %s" % (idx, text))

    if skipped:
        print("\n[跳过] %d 处落在 Qt 侧自有结构（不属于 domain），按规则允许保留 QString："
              % len(skipped))
        for rel in sorted(ALLOWED_FILES):
            if any(norm(h[0]) == rel for h in skipped):
                print("  %s   <- %s" % (rel, ALLOWED_HINTS.get(rel, "")))

    if not arg_hits and not arg_probable:
        print("\n[OK] 未发现 QString::arg() 直接吃 domain std::string 的成员")
        if arg_probable:
            pass
        return 0

    if arg_hits:
        print("\n[命中] QString::arg() 直接吃 domain std::string 成员: %d 处" % len(arg_hits))
        cur = None
        for rel, text in arg_hits:
            if cur != rel:
                cur = rel
                print("\n--- %s ---" % rel)
            print("  %s" % text)
    if arg_probable:
        print("\n[疑似] %d 处 arg() 吃的是纯局部变量（名字与某 domain 字段同名，"
              "\n       静态审计无法判定类型，需人工确认这些变量是否真是 std::string）："
              % len(arg_probable))
        cur = None
        for rel, expr, text in arg_probable:
            if cur != rel:
                cur = rel
                print("\n--- %s ---" % rel)
            print("  arg(%s)   <<  %s" % (expr, text[:120]))
    return 0

    print("\n[命中] %d 处疑似:" % len(hits))
    cur = None
    for rel, idx, text in hits:
        if cur != rel:
            cur = rel
            print("\n--- %s ---" % rel)
        print("  %4d: %s" % (idx, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
