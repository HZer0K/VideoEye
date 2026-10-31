"""核对 tests/CMakeLists.txt 里的 AUTOMOC 名单与源码实况是否一致。

背景
----
tests/CMakeLists.txt 里 AUTOMOC 曾经无条件开：125 个测试目标里只有 9 个真需要
moc，其余 116 个生成的 mocs_compilation.cpp 是 "No files found that require
moc" 空壳 —— 空壳不做事，但每个都要跑一次 cmake_autogen 扫描 + 一次 cl 调用
（实测合计约占全量构建 CPU 的 12% + 9%）。改成「默认关、名单里的目标开」之后，
名单就成了唯一事实源，而它会往两个方向漂：

  * 漏登记：新增一个带 Q_OBJECT 的测试却忘了写进名单 —— 表现是链接期缺
    vtable / metaObject，能发现，但要等到编完才炸；
  * 多登记：名单里写了其实不需要 moc 的目标 —— 悄悄退化成「每个都开」，
    前功尽弃，而且没有任何人看得见。

第二个方向才是真危险：它不报错、不改行为、只是把优化一点点吃回去。所以这里
把「名单 == 源码实况」钉成一道门，两个方向不一致都 sys.exit(1)。

判据（= CMake AUTOMOC 真实的扫描范围，不递归）
------------------------------------------------
  * 目标**列出来的**源文件本身（含手列进来的生产 .cpp）里有
    Q_OBJECT / Q_GADGET / Q_NAMESPACE
  * 或与源文件同目录同名的 .h/.hpp 里有这些宏（CMake 会把这种头文件一起扫）
  * 或源文件里写了 #include "moc_xxx.cpp" / #include "xxx.moc"

⚠️ 别改成"递归扫 include" —— 那会把名单从 9 个吹到 43 个。实测过:
test_chart_decimation.cpp 只 include "ui/charts/ChartDecimation.h"，后者再
include "ui/charts/MetricChartWidget.h"（含 Q_OBJECT），而 CMake 生成的
mocs_compilation.cpp 依然是 "No files found that require moc" 空壳 —— 被
include 到的头文件不在目标的源文件列表里，AUTOMOC 就不为它生成 moc。

用法:
    python scripts/check_automoc_registry.py
    python scripts/check_automoc_registry.py --fix   # 打印该用的名单
退出码: 一致 0 / 不一致 1。
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.environ.get("VIDEOEYE_ROOT") or os.getcwd()
TESTS_DIR = os.path.join(ROOT, "tests")
TESTS_CMAKE = os.path.join(TESTS_DIR, "CMakeLists.txt")

SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")
MACRO = re.compile(r"\b(Q_OBJECT|Q_GADGET|Q_NAMESPACE)\b")
MOC_INCLUDE = re.compile(
    r'#\s*include\s+"(?:.*/)?(?:moc_[^"/]*\.cpp|[^"/]*\.moc)"')
CALL_HEAD = re.compile(r"^videoeye_add_test\s*\(", re.M)
REGISTRY = re.compile(r"set\(VIDEOEYE_TESTS_NEEDING_AUTOMOC\s+(.*?)\)", re.S)


def parse_registry(text):
    """返回 (名单集合, 是否找得到名单变量)。"""
    m = REGISTRY.search(text)
    if not m:
        return set(), False
    return set(m.group(1).split()), True


def parse_calls(text):
    """解析 videoeye_add_test(...) 调用 -> [(目标名, [源文件 token...])]。

    按括号配对取调用体，不做缩进假设 —— 结束括号缩进与否都能吃。
    """
    out = []
    for m in CALL_HEAD.finditer(text):
        i, depth, j = m.end(), 1, m.end()
        while j < len(text) and depth:
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
            j += 1
        body = text[i:j - 1]
        # 去掉行注释（测试目标上方的说明块里常出现 .cpp 字样）
        body = "\n".join(line.split("#")[0] for line in body.splitlines())
        toks = [t for t in body.split() if t != "AUTOMOC"]
        if not toks:
            continue
        out.append((toks[0], [t for t in toks[1:] if t.endswith(SOURCE_EXT)]))
    return out


def resolve(token):
    """把 CMake 里的源文件 token 落到磁盘路径。"""
    p = token.replace("${CMAKE_CURRENT_SOURCE_DIR}", TESTS_DIR)
    if not os.path.isabs(p):
        p = os.path.join(TESTS_DIR, p)
    return os.path.normpath(p)


def needs_moc(sources, search_dirs):
    """看目标列出的源文件（+ 同目录同名头）里有没有 Qt 元对象宏。

    不递归 include —— 见文件头那段实测说明。search_dirs 保留只是为了
    兼容调用点，当前判据用不到。
    """
    for path in sources:
        if not os.path.isfile(path):
            continue
        text = open(path, encoding="utf-8", errors="ignore").read()
        body = re.sub(r"//[^\n]*", "", text)
        if MACRO.search(body) or MOC_INCLUDE.search(body):
            return True
        # 同目录同名头文件: CMake 会把它和源文件一起交给 moc
        directory = os.path.dirname(path)
        stem = os.path.splitext(os.path.basename(path))[0]
        for ext in (".h", ".hpp"):
            twin = os.path.join(directory, stem + ext)
            if os.path.isfile(twin):
                twin_body = re.sub(
                    r"//[^\n]*", "",
                    open(twin, encoding="utf-8", errors="ignore").read())
                if MACRO.search(twin_body):
                    return True
    return False


def check():
    """返回 (缺登记, 多登记, 目标总数)。"""
    text = open(TESTS_CMAKE, encoding="utf-8").read()
    registry, found = parse_registry(text)
    if not found:
        return None, None, 0
    search_dirs = [ROOT, TESTS_DIR]
    calls = parse_calls(text)
    actual = {name for name, srcs in calls
              if needs_moc([resolve(s) for s in srcs], search_dirs)}
    return sorted(actual - registry), sorted(registry - actual), len(calls)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not os.path.isfile(TESTS_CMAKE):
        print("[!] 找不到 tests/CMakeLists.txt: %s" % TESTS_CMAKE)
        return 1
    missing, extra, total = check()
    if missing is None:
        print("[!] tests/CMakeLists.txt 里没有 VIDEOEYE_TESTS_NEEDING_AUTOMOC —— "
              "名单没了，AUTOMOC 会对全部测试目标关闭")
        return 1
    if "--fix" in argv:
        text = open(TESTS_CMAKE, encoding="utf-8").read()
        search_dirs = [ROOT, TESTS_DIR]
        actual = sorted(name for name, srcs in parse_calls(text)
                        if needs_moc([resolve(s) for s in srcs], search_dirs))
        print("set(VIDEOEYE_TESTS_NEEDING_AUTOMOC")
        for name in actual:
            print("    " + name)
        print(")")
        return 0
    if missing or extra:
        print("[!] AUTOMOC 名单与源码实况不一致:")
        for name in missing:
            print("   [缺登记] %s —— 源码里有 Qt 元对象宏却没登记，链接会缺 vtable"
                  % name)
        for name in extra:
            print("   [多登记] %s —— 源码里没有元对象宏，登记它只是白跑一次"
                  " autogen + cl" % name)
        print("\n[退出码] 1 —— 用 scripts/check_automoc_registry.py --fix 生成名单")
        return 1
    print("OK: AUTOMOC 名单与源码一致 (登记 %d 个 / 测试目标 %d 个)"
          % (len(parse_registry(open(TESTS_CMAKE, encoding="utf-8").read())[0]),
             total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
