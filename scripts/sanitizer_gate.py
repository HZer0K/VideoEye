#!/usr/bin/env python3
"""Sanitizer 报告门禁: 只对本仓库源码判失败, 第三方库(Qt/libc/libstdc++)的噪声放行。

为什么需要它
============
两个 sanitizer 分支都不能靠 ctest 的退出码当判据:

* **UBSan**(linux-test-asan, 与 ASan 同开): preset 里 `halt_on_error=0`, 因为 Qt 头文件被
  include 进我们的 TU, 内联代码也带插桩 —— 开 halt 会整片红在 Qt 噪声上。代价是**项目自己
  的 UB 也不会让进程退出**, ctest 照样绿。
* **TSan**(linux-test-tsan): Qt 不是 TSan-clean 的库。Qt 内部的 data race 会让测试进程以
  66 退出, ctest 判失败 —— 于是整条 job 常年红在"不是我们的问题"上, 真属于自己的竞争反而
  淹在 68 条报告里没人看。改成 `exitcode=0` 后噪声不再判红, 但**我们自己的竞争也不再判红**。

所以判据从"进程退出码"换成"报告的落点": 报告必须来自 sanitizer 自己写的文件
(`log_path=...`, 因为 `--output-on-failure` 只在测试失败时回显输出, 测试通过时那些行被
直接吞掉 —— 实测 `--test-output-size-passed` 也救不回来), 再由本脚本按**路径**收窄:

    UBSan: 只认落在本仓库源码里的 `runtime error:`。
    TSan : 只认**竞态访问点**落在本仓库源码里的报告。

TSan 的"访问点"怎么认
---------------------
不能简单地说"栈里出现了我们的文件就红"。实测那 68 条 Qt 竞态的栈里几乎都有我们的帧 ——
我们只是调用了 Qt, 冲突的访问发生在 Qt 内部。所以每个报告按 section 切分, 取**最内层的
一帧**(跳过 libtsan/libasan 的拦截器帧)当"访问点":

    访问点在 core/ui/... 里            -> 我们的问题, 红
    访问点在 /usr/include/c++/... 里   -> 头文件内联进来的别人的代码, 放行
    访问点在我们自己的二进制里但没符号 -> 无法自证清白, 按红处理(宁可误报不可漏报)

`Location is heap block ... allocated by` / `Thread T7 ... created by` 这类 section 不参与
判定: 那是分配点和建线程点, 只能说明"我们碰过这块内存", 说明不了访问发生在谁家。

用法:
    python3 scripts/sanitizer_gate.py --kind tsan \
        --log-dir build/tsan --ctest-log ctest-sanitizer.log \
        --workspace "$PWD" --mode strict
退出码: 0 通过(或有命中但 mode=warn), 1 命中/无法判定。
"""
from __future__ import annotations

import argparse
import collections
import glob
import os
import re
import sys

# 构建产物 / 第三方: 路径里即便出现 core/ 也不算我们的源码。
EXCLUDE_PREFIXES = ("build/", "third_party/", "vcpkg_installed/", "out/", ".git/")
# 相对路径(构建目录落在源码树里时 __FILE__ 长这样)只能靠顶层目录名认。
SOURCE_TOPS = ("core/", "ui/", "infrastructure/", "tests/", "scripts/", "cmake/", "tools/")
# 我们自己的二进制: 符号化失败时靠它兜底(见模块说明)。
PROJECT_MODULE_PREFIXES = ("test_", "VideoEye", "videoeye")
# sanitizer 自己的拦截器帧(eg `#0 free ... (libtsan.so.2+0x..)`) —— 不是访问点。
SANITIZER_MODULE_PREFIXES = ("libtsan", "libasan", "libubsan", "libclang_rt")


def project_relative(path: str, workspace: str) -> str | None:
    """本仓库内的相对路径; 不属于本仓库(Qt/系统头/构建目录)返回 None。"""
    p = (path or "").strip().replace("\\", "/").strip('"')
    if not p:
        return None
    absolute = p.startswith("/") or re.match(r"^[A-Za-z]:/", p)
    if absolute:
        if not workspace:
            return None
        try:
            rel = os.path.relpath(os.path.normpath(p), os.path.normpath(workspace))
        except ValueError:
            return None
        rel = rel.replace("\\", "/")
        if rel == ".." or rel.startswith("../"):
            return None
    else:
        rel = os.path.normpath(p).replace("\\", "/")
        if rel == ".." or rel.startswith("../"):
            # 构建目录落在源码树里时, __FILE__ 会是 ../core/xxx.cpp。剥掉前导 ..
            # 后若落在源码顶层目录就算本仓库的, 否则(../usr/...、../vcpkg_installed/...)
            # 照旧丢弃。不剥这一层会把它们全漏掉。
            while rel.startswith("../"):
                rel = rel[3:]
            if not rel.startswith(SOURCE_TOPS):
                return None
    if any(rel.startswith(t) for t in EXCLUDE_PREFIXES):
        return None
    return rel


def module_of(body: str) -> str:
    """帧尾的 `(libQt6Core.so.6+0x1b49f3)` -> `libQt6Core.so.6`。"""
    m = None
    for m in re.finditer(r"\(([^()\s]+)\+0x[0-9a-fA-F]+\)", body):
        pass  # 取最后一个: 前面可能还有 BuildId 之类的括号
    return m.group(1) if m else ""


# ---------------------------------------------------------------------------
# UBSan
# ---------------------------------------------------------------------------
# 位置形如 dir/file.cpp:42:13 , 但 Qt 内部的行未必带行列号,
# 所以下面用"含 runtime error: 即计入"的兜底, 避免格式一变就静默漏报。
UBSAN_LOCATED = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+):\s*runtime error:\s*(?P<msg>.+)$")
UBSAN_MARK = "runtime error:"


def scan_ubsan(sources: list[str], workspace: str) -> dict:
    total = 0
    hits = []      # (相对路径, 行号, 信息原文)
    unknown = []   # 有 runtime error 但没能定位到位置
    for src in sources:
        for raw in _read_lines(src):
            if UBSAN_MARK not in raw:
                continue
            total += 1
            line = raw.strip()
            m = UBSAN_LOCATED.match(line)
            rel = project_relative(m.group("path"), workspace) if m else None
            if rel:
                hits.append((rel, int(m.group("line")), m.group("msg").strip()))
            elif not m:
                unknown.append(line)
    return {"total": total, "hits": hits, "unknown": unknown}


# ---------------------------------------------------------------------------
# TSan
# ---------------------------------------------------------------------------
TSAN_WARNING = "WARNING: ThreadSanitizer:"
TSAN_SUMMARY = "SUMMARY: ThreadSanitizer:"
TSAN_FRAME = re.compile(r"^\s*#(?P<n>\d+)\s+(?P<body>.*)$")
# 帧里的源码位置: 绝对路径, 或构建目录落在源码树里时的 ../.. 形态。
TSAN_PATH = re.compile(
    # 扩展名分支必须长的在前: `c` 放前面会把 `QcRunner.cpp:88` 截成 `QcRunner.c`
    # 并把行号丢掉(实测踩过, 于是命中位置永远是 None)。
    r"(?P<path>(?:\.{1,2}/|/)[^\s:()]*?\.(?:cpp|cxx|cc|hpp|hxx|hh|mm|c|h|m))(?::(?P<line>\d+))?")

# 辅助 section: 分配点 / 建线程点 / 建锁点。它们只能说明"这块内存我们碰过",
# 说明不了访问发生在谁家 —— 拿它们判红等于把 Qt 内部的竞争全算到我们头上。
TSAN_AUX_MARKERS = ("Location is", "allocated by", "allocated here", "created by", "created at")


def _is_aux_section(header: str) -> bool:
    return any(mark in header for mark in TSAN_AUX_MARKERS)


def _frame_verdict(body: str, workspace: str) -> tuple[str, str | None, int | None]:
    """(kind, 相对路径, 行号), kind ∈ {skip, project, project_unsymbolized, external}。"""
    mod = module_of(body)
    if mod.startswith(SANITIZER_MODULE_PREFIXES):
        return "skip", None, None
    m = TSAN_PATH.search(body)
    if m:
        rel = project_relative(m.group("path"), workspace)
        if rel:
            line = int(m.group("line")) if m.group("line") else None
            return "project", rel, line
        # 有源码位置但不在本仓库: Qt / libstdc++ / 系统头。
        return "external", None, None
    if mod.startswith(PROJECT_MODULE_PREFIXES):
        # 我们自己的二进制却没有源码位置 = 符号化失败, 没法自证不是我们的。
        return "project_unsymbolized", None, None
    return "external", None, None


def scan_tsan(sources: list[str], workspace: str) -> dict:
    reports = []        # dict(kind, summary, where=[(rel, line, section)], unresolved=[section], sections=n)
    crashes = []        # sanitizer 运行时自己崩了
    cur = None
    section = None
    section_verdict = None

    def close_section() -> None:
        nonlocal section, section_verdict
        if cur is not None and section is not None and section_verdict is not None:
            kind, rel, line = section_verdict
            if kind == "project":
                cur["where"].append((rel, line, section))
            elif kind == "project_unsymbolized":
                cur["unresolved"].append(section)
        section, section_verdict = None, None

    def close_report() -> None:
        nonlocal cur
        close_section()
        if cur is not None:
            reports.append(cur)
        cur = None

    for src in sources:
        for raw in _read_lines(src):
            line = raw.rstrip("\n")
            if line.startswith(TSAN_WARNING):
                close_report()
                cur = {"kind": line[len(TSAN_WARNING):].strip(), "summary": "", "where": [], "unresolved": [],
                       "sections": 0}
                continue
            if "ThreadSanitizer: CHECK failed" in line or "ThreadSanitizer: FATAL" in line:
                crashes.append(line.strip())
                continue
            if cur is None:
                continue
            if line.startswith(TSAN_SUMMARY):
                cur["summary"] = line.strip()
                close_report()
                continue
            m = TSAN_FRAME.match(line)
            if m:
                if section is None:
                    continue
                if section_verdict is None:  # 每个 section 只看最内层那一帧
                    verdict = _frame_verdict(m.group("body"), workspace)
                    # skip = sanitizer 自己的拦截器帧(free/malloc/pthread_*), 不是访问点,
                    # 继续往上看。写成 `if verdict:` 会把它当成结论锁死, 于是每个 section
                    # 的第一个真实帧再也不会被看到 —— 门禁永远绿。
                    if verdict[0] != "skip":
                        section_verdict = verdict
                continue
            stripped = line.strip()
            if stripped and stripped.endswith(":"):
                # section 头(eg `Write of size 8 at 0x.. by main thread:`)。
                close_section()
                if not _is_aux_section(stripped):
                    cur["sections"] += 1
                    section = stripped
                    section_verdict = None
        close_report()
        # 多份报告文件之间不要串味
        cur, section, section_verdict = None, None, None
    return {"reports": reports, "crashes": crashes}


def _short_summary(summary: str) -> str:
    """`SUMMARY: ThreadSanitizer: data race (/usr/lib/..+0x..) (BuildId: ..) in QMetaType::destroy`
    -> `data race in QMetaType::destroy`。annotation 的正文要短, 不然 GitHub 直接截掉。"""
    text = re.sub(r"^SUMMARY:\s*ThreadSanitizer:\s*", "", summary or "").strip()
    text = re.sub(r"\([^()]*\+0x[0-9a-fA-F]+\)\s*", "", text)   # 模块+偏移
    text = re.sub(r"\(BuildId:\s*[0-9a-f]+\)\s*", "", text)      # BuildId
    text = re.sub(r"\s+", " ", text).strip()
    return text[:120]


def _read_lines(path: str):
    """逐行读, 顺手剥掉 GitHub Actions 日志前缀(`2026-10-31T07:29:36.9723020Z `)。

    正规来源是 sanitizer 自己写的报告文件(没有前缀), 但 CI 日志被 tee 下来之后行首会
    多这么一截; 剥掉它, 同一份解析既能吃文件也能吃日志, 省掉一次"为什么 CI 里 0 条"。
    """
    stamp = re.compile(r"^\ufeff?\d{4}-\d\d-\d\dT[\d:.]+Z ")
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            yield stamp.sub("", raw)


# ---------------------------------------------------------------------------
# 汇总 / 输出
# ---------------------------------------------------------------------------
def build_summary(kind: str, data: dict, sources: list[str], workspace: str) -> tuple[list[str], list[str], int, int]:
    """返回 (summary 行, annotation 行, 命中处数, 总数)。"""
    lines = []
    rows = []      # (rel, line, 文案, 次数)
    total = 0

    if kind == "ubsan":
        grouped = collections.OrderedDict()
        for rel, ln, msg in data["hits"]:
            key = (rel, ln, msg[:100])
            grouped[key] = grouped.get(key, 0) + 1
        rows = [(rel, ln, msg, n) for (rel, ln, msg), n in sorted(grouped.items(), key=lambda kv: (kv[0][0], kv[0][1]))]
        total = data["total"]
        lines.append("## UBSan（本仓库源码）")
        lines.append("")
        lines.append("扫描来源：%s" % (", ".join(sources) if sources else "（无）"))
        lines.append("")
        lines.append("runtime error 共 %d 条，落在本仓库源码 %d 条，去重后 %d 处。" % (total, len(data["hits"]), len(rows)))
    else:
        grouped = collections.OrderedDict()
        unresolved = collections.OrderedDict()
        for rep in data["reports"]:
            total += 1
            for rel, ln, header in rep["where"]:
                key = (rel, ln, _short_summary(rep["summary"]) or header)
                grouped[key] = grouped.get(key, 0) + 1
            for header in rep["unresolved"]:
                unresolved[header] = unresolved.get(header, 0) + 1
        rows = [(rel, ln, msg, n)
                for (rel, ln, msg), n in sorted(grouped.items(), key=lambda kv: (kv[0][0], kv[0][1] or 0))]
        for header, n in unresolved.items():
            rows.append(("(符号化失败)", None, header, n))
        flagged = len([r for r in data["reports"] if r["where"] or r["unresolved"]])
        noise = total - flagged
        lines.append("## TSan（本仓库源码）")
        lines.append("")
        lines.append("扫描来源：%s" % (", ".join(sources) if sources else "（无）"))
        lines.append("")
        lines.append("竞态/死锁报告共 %d 条，访问点落在本仓库源码的 %d 条（去重后 %d 处），"
                     "第三方库内部 %d 条已放行。"
                     % (total, flagged, len(rows), noise))

    if rows:
        lines.append("")
        lines.append("| 位置 | 次数 | 报告 |")
        lines.append("| --- | --- | --- |")
        for rel, ln, msg, n in rows[:60]:
            where = "`%s:%s`" % (rel, ln) if ln else "`%s`" % rel
            lines.append("| %s | %d | %s |" % (where, n, msg.replace("|", "\\|")[:120]))
        if len(rows) > 60:
            lines.append("")
            lines.append("（另有 %d 处未列出，见 job 日志）" % (len(rows) - 60))

    if kind == "ubsan" and data["unknown"]:
        lines.append("")
        lines.append("另有 %d 条 runtime error 没能解析出位置，需人工确认（可能是第三方代码）："
                     % len(data["unknown"]))
        for line in data["unknown"][:10]:
            lines.append("")
            lines.append("    " + line[:200])

    annotations = []
    for rel, ln, msg, _n in rows[:20]:
        if not ln or not rel.endswith(tuple([".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"])):
            continue
        note = msg.replace("%", "%25").replace("\r", "").replace("\n", " ")[:180]
        annotations.append("::error file=%s,line=%d,title=%s::%s" % (rel, ln, "UBSan" if kind == "ubsan" else "TSan", note))
    return lines, annotations, len(rows), total


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="sanitizer 报告门禁(只对本仓库源码判失败)")
    ap.add_argument("--kind", required=True, choices=("ubsan", "tsan"))
    ap.add_argument("--log-dir", default="", help="sanitizer 自己写报告的目录")
    ap.add_argument("--ctest-log", default="", help="ctest 输出落盘副本(次要来源)")
    ap.add_argument("--workspace", default=os.getcwd())
    ap.add_argument("--mode", default="strict", help="strict(默认) / warn(只报告不判红)")
    args = ap.parse_args(argv)

    workspace = args.workspace.rstrip("/\\")
    strict = (args.mode or "strict").strip().lower() != "warn"
    prefix = "ubsan" if args.kind == "ubsan" else "tsan"

    sources = []
    if args.ctest_log and os.path.isfile(args.ctest_log):
        sources.append(args.ctest_log)
    # 目录必须存在。不存在 = 变量没传进步骤, 报告只会回到 stderr, 而 `--output-on-failure`
    # 在测试通过时不回显 —— 门禁会静默变绿。宁可红。
    if not args.log_dir or not os.path.isdir(args.log_dir):
        print("::error::报告目录不存在（%r）：sanitizer 报告无处可落，这道门禁会静默变绿"
              % args.log_dir)
        return 1
    sources += sorted(p for p in glob.glob(os.path.join(args.log_dir, prefix + "*")) if os.path.isfile(p))

    data = scan_ubsan(sources, workspace) if args.kind == "ubsan" else scan_tsan(sources, workspace)
    lines, annotations, rows, total = build_summary(args.kind, data, sources, workspace)
    text = "\n".join(lines)
    print(text)

    out = os.environ.get("GITHUB_STEP_SUMMARY")
    if out:
        try:
            with open(out, "a") as fh:
                fh.write(text + "\n")
        except OSError:
            pass

    for line in annotations:
        print(line)

    if args.kind == "tsan":
        crashes = data["crashes"]
        if crashes:
            print("")
            print("sanitizer 运行时自身崩溃 %d 次（ctest 会因为进程非零退出而红，先看这里）：" % len(crashes))
            for line in crashes[:10]:
                print("    " + line[:200])
            print("::warning::ThreadSanitizer 运行时崩溃 %d 次，失败的测试未必是业务缺陷" % len(crashes))
        unparsed = [r for r in data["reports"] if not r["sections"]]
        if unparsed:
            print("::warning::有 %d 条报告没有解析出任何 section（格式可能变了），需人工确认" % len(unparsed))

    if rows and strict:
        print("::error::本仓库源码存在 sanitizer 命中（%s，共 %d 处）；修完再合，"
              "或把仓库变量 %s_GATE_MODE 设成 warn 先摸底" % (args.kind, rows, args.kind.upper()))
        return 1
    if rows:
        print("%s: %d 处命中，但 GATE_MODE=warn，本轮不判红。" % (args.kind, rows))
    else:
        print("%s: 本仓库源码无命中（共扫描 %d 条报告）" % (args.kind, total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
