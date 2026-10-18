#!/usr/bin/env python3
r"""守卫：构建目录记录的 /showIncludes 前缀，必须与 cl.exe 实际输出一致。

为什么要有这条守卫
------------------
cl.exe 的 /showIncludes 输出语言不是固定的，它跟着 VS 的语言资源走：

    默认（无 VSLANG）   -> "注意: 包含文件:  "
    VSLANG=1033         -> "Note: including file: "

CMake 在 try_compile 阶段把这句前缀探测进
    build/<dir>/CMakeFiles/<ver>/CMake{C,CXX}Compiler.cmake
（变量 CMAKE_{C,CXX}_CL_SHOWINCLUDES_PREFIX），并写进
    build/<dir>/CMakeFiles/rules.ninja 的 msvc_deps_prefix。
ninja 靠它去剥 /showIncludes 的每一行，还原出"这个 .obj 依赖哪些头文件"。

一旦目录里记的前缀与 cl.exe 此刻实际吐的前缀对不上，剥壳就**静默失败**：
ninja 认为这个 TU 的依赖没变 -> 改了头文件也不重编 -> 链接的是陈旧对象。
在 Windows 上表现为 0xc0000409 / 0xc0000374 这类**看起来像代码写坏了**的崩溃，
实际是构建产物串味。本脚本把这件事在构建前就拦住，而不是等它变成崩溃。

钉住语言的办法见 CMakePresets.json 的 vendor.videoeye."vslang-note"：
windows-base 用 CMAKE_{C,CXX}_COMPILER_LAUNCHER 把 cl.exe 的语言钉成英文。
本脚本探测时会**按该目录配置的启动器还原环境变量**再跑 cl.exe，所以
"钉了 VSLANG 的目录"和"没钉、跟着系统走的目录"都能得到与真实构建一致的结果。

用法
----
    python scripts/check_msvc_showincludes_prefix.py              # 扫 build/ 下所有目录
    python scripts/check_msvc_showincludes_prefix.py build/release
    python scripts/check_msvc_showincludes_prefix.py --require-vslang
    python scripts/check_msvc_showincludes_prefix.py --strict-ambient  # 仅本地排查

    --require-vslang    要求每个目录都钉了 VSLANG（CI 用；本地刚加完启动器、
                        旧目录还没重配时会红，重配一次即绿）。
    --strict-ambient    连没钉语言的目录也拿环境语言比对（默认不做，理由见下）。

判定口径（重要）
----------------
**只拿钉了 VSLANG 的目录做成败判定。** 没钉语言的目录默认只提示、不判红 ——
因为那种目录构建时用的是"当下环境的语言"，它会随 VS 语言包 / 系统区域设置而变，
拿它当期望值会把守卫自己变成一条 flaky 用例（实测过：全量 ctest 里偶发报
"记的是中文、实际吐英文"，单独重跑三次全绿）。flaky 的守卫比没有守卫更糟：
它会训练人直接无视红灯。

想让它有牙齿：CI 走 --require-vslang，本地排查走 --strict-ambient。

退出码
------
    0  全部一致（或环境不具备条件 -> SKIP，不阻塞 Linux / MinGW）
    1  钉了语言的目录存在不一致；或 --require-vslang 下有目录没钉语言
"""

from __future__ import print_function

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.environ.get("VIDEOEYE_ROOT") or os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))

# cl.exe 在 MSYS 环境下会把 /showIncludes 当成路径做转换，必须先关掉。
MSYS_ENV = dict(os.environ)
MSYS_ENV["MSYS_NO_PATHCONV"] = "1"

PREFIX_RE = re.compile(
    r'^set\(CMAKE_(?:C|CXX)_CL_SHOWINCLUDES_PREFIX\s+"(.*)"\s*\)')
LAUNCHER_RE = re.compile(r'^CMAKE_CXX_COMPILER_LAUNCHER:STRING=(.*)$')
VSLANG_RE = re.compile(r"VSLANG=(\S+)")

# 两种语言下"前缀"的共同锚点。判定不依赖具体文案，只取锚点之前的部分。
ANCHOR_RE = re.compile(r"(?:including file|包含文件)")


def decode(raw):
    """MSVC 在中文下吐 GBK、英文下吐 ASCII，两头都试一遍。"""
    for enc in ("utf-8", "gb18030", "latin-1"):
        try:
            return raw.decode(enc)
        except Exception:
            continue
    return raw.decode("gb18030", "replace")


def find_cl_exe():
    """定位 cl.exe：先看 PATH，再退回常见的 VS2022 安装路径。"""
    tool = "where" if os.name == "nt" else "which"
    try:
        out = subprocess.check_output([tool, "cl.exe"],
                                      env=MSYS_ENV,
                                      stderr=subprocess.DEVNULL)
    except Exception:
        out = b""
    for line in decode(out).splitlines():
        line = line.strip()
        if line and os.path.exists(line):
            return line

    pattern = (r"C:\Program File*\Microsoft Visual Studio\2022\*"
               r"\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe")
    hits = sorted(glob.glob(pattern))
    return hits[-1] if hits else None


def read_recorded_prefix(build_dir):
    """读目录里 CMake 探测到的前缀。返回 (prefix, 来源文件) 或 (None, None)。"""
    for lang in ("C", "CXX"):
        for path in sorted(glob.glob(os.path.join(
                build_dir, "CMakeFiles", "*", "CMake%sCompiler.cmake" % lang))):
            with open(path, "rb") as fh:
                text = decode(fh.read())
            for line in text.splitlines():
                m = PREFIX_RE.match(line.strip())
                if m:
                    return m.group(1), path
    return None, None


def read_vslang(build_dir):
    """从 CMakeCache 里读出该目录配置的启动器钉了什么 VSLANG。没钉则返回 None。"""
    cache = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.exists(cache):
        return None
    with open(cache, "rb") as fh:
        text = decode(fh.read())
    for line in text.splitlines():
        m = LAUNCHER_RE.match(line.strip())
        if m:
            v = VSLANG_RE.search(m.group(1))
            return v.group(1) if v else None
    return None


def probe_actual_prefix(cl_exe, vslang):
    """真的跑一次 cl.exe /showIncludes，抓出它此刻吐的前缀。

    vslang 为该目录配置的启动器所钉的语言；None 表示不额外设置（跟着系统走）。
    返回 (prefix, 失败原因)；prefix 为 None 表示探不到 -> 调用方 SKIP。
    """
    env = dict(MSYS_ENV)
    if vslang:
        env["VSLANG"] = vslang
    else:
        env.pop("VSLANG", None)

    workdir = tempfile.mkdtemp(prefix="ve_showincludes_")
    try:
        with open(os.path.join(workdir, "probe.h"), "w") as fh:
            fh.write("#pragma once\n")
        with open(os.path.join(workdir, "probe.c"), "w") as fh:
            fh.write('#include "probe.h"\nint main(void) { return 0; }\n')

        cmd = [cl_exe, "/nologo", "/showIncludes", "/c",
               "/Fo" + os.path.join(workdir, "probe.obj"), "probe.c"]
        try:
            proc = subprocess.run(cmd, cwd=workdir, env=env,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        except Exception as exc:
            return None, "cl.exe 无法执行: %s" % exc

        text = decode(proc.stdout)
        for line in text.splitlines():
            if "probe.h" not in line:
                continue
            m = ANCHOR_RE.search(line)
            if not m:
                continue
            # 取锚点及其之前的全部内容，即前缀本体（含尾部冒号与空格）
            return line[:m.end()], None
        return None, "cl.exe 输出里没找到 /showIncludes 行"
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def normalize(prefix):
    """归一：去掉尾部冒号与全部空白，便于中英文两种形式对齐比较。"""
    if prefix is None:
        return None
    return re.sub(r"\s+", "", prefix.rstrip()).rstrip(":").rstrip()


def describe(prefix):
    try:
        prefix.encode("ascii")
        return "英文"
    except Exception:
        return "非 ASCII（中文/其它）"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dirs", nargs="*",
                    help="要检查的构建目录，默认扫 build/ 下所有一级子目录")
    ap.add_argument("--require-vslang", action="store_true",
                    help="严格模式：要求每个目录都钉了 VSLANG（CI 用）")
    ap.add_argument("--strict-ambient", action="store_true",
                    help="连没钉语言的目录也拿环境语言比对（默认不做，见下面'为什么'"
                         " ；只用于本地排查，别放进 CI）")
    args = ap.parse_args()

    targets = args.build_dirs
    if not targets:
        targets = sorted(glob.glob(os.path.join(ROOT, "build", "*")))

    dirs = []
    for d in targets:
        if not os.path.isabs(d):
            d = os.path.join(ROOT, d)
        if glob.glob(os.path.join(d, "CMakeFiles", "*", "CMakeCXXCompiler.cmake")):
            dirs.append(d)

    if not dirs:
        print("SKIP: 没有已 configure 的构建目录")
        return 0

    cl_exe = find_cl_exe()
    if not cl_exe:
        print("SKIP: 找不到 cl.exe（非 MSVC 环境）")
        return 0

    problems = []
    unpinned = []
    checked = 0
    for d in dirs:
        try:
            rel = os.path.relpath(d, ROOT)
        except ValueError:
            # 传入的目录与仓库根不在同一个盘符（Windows 上 relpath 会抛），
            # 退回首绝对路径，别让守卫自己崩掉。
            rel = d
        recorded, src = read_recorded_prefix(d)
        if recorded is None:
            continue
        vslang = read_vslang(d)

        # 没钉语言的目录**不拿环境语言判成败**。
        #
        # 为什么：这里判的是"构建时 cl.exe 会吐什么语言"，而没钉 VSLANG 的目录
        # 构建时用的就是**当下环境的语言** —— 那个值会随 VS 语言包、系统区域设置、
        # 甚至并发跑着的其它进程而变。拿它当期望值，守卫自己就变成了一条 flaky
        # 用例（实测过：全量 ctest 里偶发报"记的是中文、实际吐英文"，单独重跑
        # 三次全绿）。flaky 的守卫比没有守卫更糟 —— 它会训练人直接无视红灯。
        #
        # 所以没钉语言的目录只**提示**（重配一次即可带上 windows-base 的启动器），
        # 判定留给 --require-vslang（CI 用）与 --strict-ambient（本地排查用）。
        if vslang is None and not args.strict_ambient:
            unpinned.append(rel)
            if args.require_vslang:
                problems.append(
                    "%s: 没钉 VSLANG，cl.exe 输出语言跟着系统走，将来可能漂。"
                    "重配一次即可带上 windows-base 里的启动器。" % rel)
            continue

        actual, reason = probe_actual_prefix(cl_exe, vslang)
        if actual is None:
            print("SKIP %s: 探测失败（%s）" % (rel, reason or "未知原因"))
            continue
        checked += 1

        if normalize(recorded) != normalize(actual):
            problems.append(
                "%s: /showIncludes 前缀不匹配\n"
                "      目录里记的是 [%s]\n"
                "      cl.exe 实际吐的是 [%s]\n"
                "      后果: ninja 剥壳失败 -> 改了头文件不重编 -> 链接陈旧对象\n"
                "            （Windows 上表现为 0xc0000409 / 0xc0000374）\n"
                "      记录文件: %s\n"
                "      修法: 删掉上面的 CMake{C,CXX}Compiler.cmake 后重新 configure"
                % (rel, recorded.strip(), actual.strip(), src))

    if problems:
        print("MSVC /showIncludes 前缀检查失败:", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1

    print("MSVC /showIncludes 前缀检查通过: %d 个构建目录的前缀与 cl.exe 实际输出一致"
          % checked)
    if unpinned:
        print("提示: 以下目录没钉 VSLANG，输出语言跟着系统走（当前自洽，但会漂）: %s\n"
              "      重配一次即可带上 windows-base 里的编译器启动器；"
              "CI 可用 --require-vslang 把这条变成硬失败。"
              % ", ".join(unpinned))
    return 0


if __name__ == "__main__":
    sys.exit(main())
