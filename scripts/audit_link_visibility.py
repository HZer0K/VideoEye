#!/usr/bin/env python3
"""检查「公共头闭包需要的系统依赖, 是否由 PUBLIC 链接闭包提供」。

为什么要有这个: CMake 的 PUBLIC / PRIVATE 选择此前"只能靠人读代码守"（CMakeLists.txt
里自己这么写的）。挂错了不会立刻出事 —— 本仓库每个模块的 include 目录都是仓库根
（见 videoeye_add_module），模块内部照样编译；只有**外部目标** include 该模块的公共头
时才会撞墙: 静态库的 PRIVATE 依赖只传 link、不传 include 目录与编译定义，外部目标拿不到
Qt / FFmpeg 的头文件。P2-8 的 VideoEyePlayback -> VideoEyeQtAdapters 就是这么暴露的
（公共头用了 QtWorkerOwner.h，边却挂在 PRIVATE）。这条规则现在由本脚本守。

规则: 若某模块的「公共头闭包」需要某类系统依赖，则该模块 target 的 **PUBLIC 链接闭包**
必须提供至少一个同类依赖，否则判违规。

  | 依赖类 | 公共头里的形态                     | PUBLIC 闭包的要求  |
  |--------|------------------------------------|--------------------|
  | Qt     | #include <QString> / <QtCore/...>  | 至少一个 Qt6::*    |
  | FFmpeg | #include <libavformat/...> 等      | 至少一个 FFmpeg::* |

「公共头闭包」= 模块目录下全部头文件 + 它们用引号形式递归 include 到的仓库内头文件。
不区分"私有头"，是因为这里没有真私有头: include 目录是仓库根，任何模块头都能被外部
目标按 "core/xxx.h" 引到。

刻意不查（别当问题，也别顺手把边翻成 PUBLIC）:
  * 模块边（Qc -> Analysis、Playback -> Exporter/Infrastructure 等）挂在 PUBLIC 还是
    PRIVATE —— 有些公共头确实 include 了 PRIVATE 链接的模块，那些被 include 的头不带
    系统依赖，是刻意保留的；本脚本只看系统依赖。
  * 主程序 / 测试目标的 PRIVATE 依赖 —— 它们没有下游消费者。

退出码: 有违规 = 1, 全过 = 0（pre-commit / CI / ctest 都靠这个退出码把关）。
"""
import os
import re
import sys

ROOT = os.environ.get("VIDEOEYE_ROOT") or os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
)

# 系统依赖分类: (类名, 头文件 include 的匹配式, PUBLIC 闭包里的依赖前缀)
CLASSES = [
    ("Qt", re.compile(r"^(?:Q[A-Z]\w*|Qt[A-Za-z]+/)"), "Qt6::"),
    ("FFmpeg", re.compile(r"^lib(?:av|sw)[a-z]+/"), "FFmpeg::"),
]

RE_ADD_MODULE = re.compile(r"videoeye_add_module\(\s*(\w+)\s*([^)]*)\)")
RE_FILE_GLOB = re.compile(r"file\(\s*GLOB(?:_RECURSE)?\s+(\w+)([^)]*)\)", re.S)
RE_TLL_HEAD = re.compile(r"target_link_libraries\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)")
RE_INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
RE_VAR_REF = re.compile(r"\$\{(\w+)\}")


def strip_comment(line):
    """去掉行内 `#` 注释；引号内的 `#` 不算注释起点。"""
    out = []
    in_quote = False
    for ch in line:
        if ch == '"':
            in_quote = not in_quote
        elif ch == "#" and not in_quote:
            break
        out.append(ch)
    return "".join(out)


def read_cmake(root):
    path = os.path.join(root, "CMakeLists.txt")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    return "\n".join(strip_comment(line) for line in text.splitlines())


def balanced_args(text, start):
    """text[start] == '('，返回配平括号内的参数文本。"""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return text[start + 1:]


def parse_glob_dirs(cmake):
    """变量名 -> 该 glob 涵盖的目录集合（只取 *.h / *.hpp 模式）。"""
    out = {}
    for m in RE_FILE_GLOB.finditer(cmake):
        var, args = m.group(1), m.group(2)
        dirs = set()
        for pat in re.findall(r'"([^"]+)"', args):
            if pat.endswith((".h", ".hpp")):
                dirs.add(pat.rsplit("/", 1)[0] if "/" in pat else ".")
        out.setdefault(var, set()).update(dirs)
    return out


def parse_modules(cmake, glob_dirs):
    """模块名 -> 目录集合（顺带记录声明顺序）。"""
    modules = {}
    for m in RE_ADD_MODULE.finditer(cmake):
        name, args = m.group(1), m.group(2)
        dirs = set()
        for var in RE_VAR_REF.findall(args):
            dirs |= glob_dirs.get(var, set())
        modules[name] = dirs
    return modules


def parse_links(cmake):
    """模块名 -> {"public": [...], "private": [...]}，合并该 target 的多条调用。"""
    links = {}
    for m in RE_TLL_HEAD.finditer(cmake):
        target = m.group(1)
        # 开括号在 target 名之前，必须在整个匹配段内找 —— 用 m.end() 往后找会搜到
        # 下一条调用的括号，把参数错位解析成别的 target 的（实测踩过：全部模块凭空
        # 冒出"没有 PUBLIC 依赖"的假违规）。
        pos = cmake.index("(", m.start())
        args = balanced_args(cmake, pos)
        bucket = links.setdefault(target, {"public": [], "private": []})
        section = None
        for tok in args.split():
            if tok in ("PUBLIC", "INTERFACE"):
                section = "public"
            elif tok == "PRIVATE":
                section = "private"
            elif tok.startswith("${"):
                continue  # 变量（如 ${CMAKE_DL_LIBS}）不属于两类系统依赖
            elif section:
                bucket[section].append(tok)
    return links


def module_headers(root, dirs):
    headers = []
    for d in dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for cur, _sub, files in os.walk(base):
            for f in files:
                if f.endswith((".h", ".hpp")):
                    headers.append(os.path.normpath(os.path.join(cur, f)))
    return headers


def resolve_include(root, inc, cur_file):
    """把引号形式的 include 解析到仓库内文件；解析不到（生成头 / 外部头）返回 None。"""
    candidates = [os.path.join(root, inc), os.path.join(os.path.dirname(cur_file), inc)]
    for c in candidates:
        c = os.path.normpath(c)
        if c.startswith(root + os.sep) and os.path.isfile(c):
            return c
    return None


def scan_module(root, headers):
    """返回 (needs, scanned)。needs = [(类名, 相对路径, 行号, include 名)]。

    递归跟随引号 include 进仓库内其它头文件 —— 被 include 的头（哪怕来自 PRIVATE
    链接的模块）带进来的系统依赖，最终也会落在 include 该模块公共头的消费者头上。
    """
    needs = []
    seen = set()
    stack = list(headers)
    while stack:
        path = os.path.normpath(stack.pop())
        if path in seen:
            continue
        seen.add(path)
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        rel = os.path.relpath(path, root).replace("\\", "/")
        for m in RE_INCLUDE.finditer(text):
            delim, inc = m.group(1), m.group(2)
            if delim == "<":
                for cls, pat, _prefix in CLASSES:
                    if pat.match(inc):
                        line_no = text[: m.start()].count("\n") + 1
                        needs.append((cls, rel, line_no, inc))
                        break
            else:
                resolved = resolve_include(root, inc, path)
                if resolved and resolved not in seen:
                    stack.append(resolved)
    return needs, len(seen)


def main():
    cmake = read_cmake(ROOT)
    if cmake is None:
        print("[!] 找不到 %s/CMakeLists.txt —— 请设置 VIDEOEYE_ROOT 指向仓库根" % ROOT)
        sys.exit(1)

    glob_dirs = parse_glob_dirs(cmake)
    modules = parse_modules(cmake, glob_dirs)
    links = parse_links(cmake)

    violations = []
    checked_headers = 0
    hits = {cls: [] for cls, _, _ in CLASSES}

    for name in sorted(modules):
        dirs = modules[name]
        if not dirs:
            violations.append((name, None, None))
            continue

        headers = module_headers(ROOT, dirs)
        checked_headers += len(headers)
        needs, _scanned = scan_module(ROOT, headers)
        if not needs:
            continue

        # PUBLIC 链接闭包: 沿模块的 PUBLIC 边传递（CMake 对 PUBLIC 依赖就是这么传播的）
        closure, stack = set(), [name]
        while stack:
            n = stack.pop()
            if n in closure:
                continue
            closure.add(n)
            for dep in links.get(n, {}).get("public", []):
                if dep in modules:
                    stack.append(dep)
        available = set()
        for n in closure:
            available |= set(links.get(n, {}).get("public", []))

        by_class = {}
        for cls, rel, line, inc in needs:
            by_class.setdefault(cls, []).append((rel, line, inc))

        for cls, _pat, prefix in CLASSES:
            if cls not in by_class:
                continue
            hits[cls].append(name)
            if not any(tok.startswith(prefix) for tok in available):
                violations.append((name, cls, by_class[cls]))

    if violations:
        print("[!] 链接可见性违规（公共头需要的系统依赖没有从 PUBLIC 边透出去）:")
        for name, cls, evidence in violations:
            print("   %s" % name)
            if cls is None:
                print("       无法解析出模块目录: videoeye_add_module 的参数里没有能对上")
                print("       file(GLOB ... \"dir/*.h\") 的变量 —— 脚本会因此漏查, 按失败处理。")
                continue
            print("       公共头闭包需要 %s, 但 PUBLIC 闭包里没有对应的依赖。" % cls)
            print("       证据:")
            shown = set()
            for rel, line, inc in evidence:
                key = (rel, inc)
                if key in shown:
                    continue
                shown.add(key)
                print("           %s:%d   #include <%s>" % (rel, line, inc))
                if len(shown) >= 3:
                    break
            prefix = dict((c, p) for c, _pat, p in CLASSES)[cls]
            private = [t for t in links.get(name, {}).get("private", []) if t.startswith(prefix)]
            if private:
                print("       修法: 把 %s 从 PRIVATE 提到 PUBLIC —— 行为中性, 现有消费者本就自带它,"
                      % ", ".join(sorted(private)))
                print("             只是让 include 这个公共头的外部目标也能拿到对应头文件与编译定义。")
            else:
                print("       修法: 给这个 target 补一条 PUBLIC 的 %s 依赖。" % prefix)
        print()
        print("共 %d 个模块违规。修掉之后重跑: python scripts/audit_link_visibility.py"
              % len(violations))
        print("[退出码] 1 —— 公共头依赖没有从 PUBLIC 边透出, 提交/流水线会被拦下")
        sys.exit(1)

    print("OK: 链接可见性一致（公共头需要的系统依赖都在 PUBLIC 闭包里）")
    print("检查了 %d 个模块 / %d 个头文件（含递归跟随到的其它模块头）"
          % (len(modules), checked_headers))
    for cls, _pat, _prefix in CLASSES:
        if hits[cls]:
            print("   %s 命中 %d 个模块: %s" % (cls, len(hits[cls]), ", ".join(sorted(hits[cls]))))


if __name__ == "__main__":
    main()