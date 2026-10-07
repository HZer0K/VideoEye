#!/usr/bin/env python3
"""审计：core 生产侧是否存在「Qt 类型直接写进 domain 的 std::string 字段」。

这是 scripts/check_layering.py 的补充：check_layering 只管 #include 方向，
管不到「Qt 类型从生产侧直接落进 domain 的 std::string 字段」这种字段级污染。

用法：
    python scripts/audit_qt_domain_border.py

做法：
1. 扫描 core/domain/model/*.h，抽取所有声明为 std::string 的成员名（含嵌套 struct）。
2. 扫描 core/analysis、core/player、core/exporter、core/media、infrastructure 下的 .cpp/.h，
   找出对域字段的赋值语句，其右值含 QString / QByteArray / QLatin1String / QStringLiteral 等 Qt 类型，
   且未显式 .toStdString()。
3. 另查 QString::arg() 直接吃 domain std::string 成员——arg() 没有 std::string 重载，必然编译不过。
   这一关先用「接收者类型」澄清一次：arg(opt.format) 里若 opt 是 Qt 侧自有结构
   （例如 exporter 的 ExportOptions），即便字段名与 domain 字段撞车也按合法用法放行，
   归入 [跳过]；只有接收者类型解析不到、或解析到 core/domain 的 struct 才计入违规。

输出里的 [疑似] 是静态审计判定不了类型的（arg() 吃的是与域字段同名的纯局部变量），
需人工确认，不是确定违规。
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


def collect_domain_structs() -> set[str]:
    """扫描 core/domain 下所有头文件，抽取 struct/class 名。

    为什么需要它: 字段名会撞车——domain 的 FrameData::format 是 std::string，
    而 Qt 侧 ExportOptions::format 是 QString，两者同名。判断 .arg(X.format)
    是否违规，光看字段名不够，得看 X 的类型是不是 domain 模型。
    """
    names: set[str] = set()
    base = os.path.dirname(DOMAIN_DIR)  # core/domain
    pat = re.compile(r"\b(?:struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)")
    for dirpath, _dirs, files in os.walk(base):
        for n in files:
            if not n.endswith(".h"):
                continue
            try:
                with open(os.path.join(dirpath, n), "r", encoding="utf-8") as f:
                    for line in f:
                        m = pat.search(line)
                        if m:
                            names.add(m.group(1))
            except Exception:
                continue
    return names


# 纯成员访问链: opt.format / obj.inner.field。
# 只有这种形式才能靠「变量声明」静态解析出接收者类型；
# 带函数调用（.value().format）或下标的形式一律退回原启发式（保守）。
MEMBER_CHAIN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+$")
MEMBER_PAIR_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)")


def resolve_var_type(text: str, var: str) -> str | None:
    """在文件文本里找 `Type var` / `Type& var` / `const Type& var` / 形参等声明，返回类型名。

    只做单文件、单次出现级别的粗糙解析——够用即可：这是启发式的「澄清」，
    解析不到时调用方按原保守逻辑处理，不因解析失败而放宽。
    """
    if not text:
        return None
    pat = re.compile(
        r"(?:^|[;{}(,])\s*"
        r"(?:const\s+|volatile\s+|static\s+|typename\s+)*"
        r"([A-Za-z_][A-Za-z0-9_:]*(?:\s*<[^;{}()]*>)?)\s*"
        r"(?:[&*]\s*|\s+)" + re.escape(var) + r"\b\s*[;=,)\[\{]",
        re.MULTILINE)
    m = pat.search(text)
    return m.group(1).strip() if m else None


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
    domain_structs = collect_domain_structs()
    print("domain struct/class 样本: %d" % len(domain_structs))

    def norm(rel: str) -> str:
        return rel.replace("\\", "/")

    hits = []
    arg_hits = []     # 高置信：arg() 吃的是 obj.<域字段>
    arg_probable = [] # 疑似：arg() 吃的是纯局部变量（名字与域字段同名）
    receiver_skipped = []  # 放行：接收者类型解析得到，但不是 domain 结构（字段名撞车）
    file_texts: dict[str, str] = {}
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
        file_texts[norm(os.path.relpath(path, ROOT))] = text
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
            # 纯成员访问链（opt.format）：接收者类型可静态解析，先澄清一次。
            # 字段名撞车（domain FrameData::format vs Qt 侧 ExportOptions::format）
            # 就发生在这里——解析到 Qt 侧自有结构就放行，不再冤枉合法用法。
            if MEMBER_CHAIN_RE.match(expr_stripped):
                field_pairs = [(b, m) for (b, m) in MEMBER_PAIR_RE.findall(expr_stripped)
                               if m in fields]
                if field_pairs:
                    base = field_pairs[-1][0]
                    recv_type = resolve_var_type(file_texts.get(norm(rel), ""), base)
                    short = recv_type.split("::")[-1] if recv_type else ""
                    if recv_type and short not in domain_structs:
                        receiver_skipped.append((rel, expr_stripped, recv_type, stmt[:160]))
                        continue
                    # 解析到 domain 结构；或类型解析不到（保守起见仍算确定命中）
                    arg_hits.append((rel, stmt[:200]))
                    break
            # 其它形式（函数调用/下标等）静态拿不到类型，退回原启发式：
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

    if skipped:
        print("\n[跳过] %d 处落在 Qt 侧自有结构（不属于 domain），按规则允许保留 QString："
              % len(skipped))
        for rel in sorted(ALLOWED_FILES):
            if any(norm(h[0]) == rel for h in skipped):
                print("  %s   <- %s" % (rel, ALLOWED_HINTS.get(rel, "")))

    def print_receiver_skipped():
        if not receiver_skipped:
            return
        print("\n[跳过] %d 处 arg() 的接收者不是 domain 结构（字段名与 domain 撞车，"
              "但接收者类型在 core/domain 之外声明），按合法用法放行："
              % len(receiver_skipped))
        cur = None
        for rel, expr, recv_type, text in receiver_skipped:
            if cur != rel:
                cur = rel
                print("\n--- %s ---" % rel)
            print("  arg(%s)  <- 接收者类型 %s" % (expr, recv_type))
            print("      << %s" % text[:120])

    # 早退分支只能在「三类全空」时才走，否则会出现
    # 「打印了 [命中] 却 return 0」——CI 上方块徽章是绿的，违规照旧合并进主干。
    # （scripts/test_audit_qt_domain_border.py 对这条路径有专门断言。）
    if not real_hits and not arg_hits and not arg_probable:
        print("\n[OK] 未发现 QString::arg() 直接吃 domain std::string 的成员")
        print_receiver_skipped()
        print("\n[退出码] 0 —— 无确定命中，CI / pre-commit 放行")
        return 0

    if real_hits:
        print("\n[命中] 域字段被 Qt 类型污染: %d 处" % len(real_hits))
        cur = None
        for rel, idx, text in real_hits:
            if cur != rel:
                cur = rel
                print("\n--- %s ---" % rel)
            print("  %4d: %s" % (idx, text))

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

    # 接收者澄清属于「放行」而非「疑似」，上面两个分支都该看到这条提示
    print_receiver_skipped()

    # 只有「确定命中」（域字段被 Qt 类型直接赋值 / arg() 吃域字段成员）才让 CI / pre-commit 红。
    # arg_probable 是静态判定不了类型的（名字撞车），属于人工确认项，不能让脚本红，
    # 否则一次误报之后所有人都会习惯性加 --no-verify，真命中也就跟着一起被忽略了。
    if real_hits or arg_hits:
        print("\n[退出码] 1 —— 存在确定命中，需修复或显式加白名单")
        return 1
    print("\n[退出码] 0 —— 仅『疑似』项，按 warning 处理")
    return 0


if __name__ == "__main__":
    sys.exit(main())
