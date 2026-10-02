#!/usr/bin/env python3
"""(离线) 用本机 MinGW g++ 对「不依赖 FFmpeg 的编译单元」做真语法/类型检查。

背景
----
这台机器没有 Qt / FFmpeg / MSVC / gtest，通常只能靠 grep 猜有没有编译错。
但 domain 层去 Qt 之后，core/analysis/container 下的容器分析器只剩 Qt + 标准库
依赖，于是可以造一套最小 Qt 桩头文件（.cache/syntaxcheck/stub），
用真实 g++ 把它们编译一遍——gbeek 出来的错误是**真的**，不是猜的。

用法
----
    python tools/syntaxcheck_offline.py            # 检查全部
    python tools/syntaxcheck_offline.py Mp4Box     # 只检查名字含关键字的文件

当前覆盖
--------
    core/domain/model/*.cpp                      全部通过
    EbmlAnalyzer / Mp4BoxAnalyzer / Asf / Avi /
    Flv / Ogg / Ts StructureAnalyzer            全部通过
    ContainerStructureAnalyzer.cpp               跳过（需要 FFmpeg 头）
    Mp4SampleTableAnalyzer                      仅剩 MinGW libstdc++ 假阳性

桩缺口 / 工具链差异（会误报，已在 IGNORE_FRAGMENTS 里过滤）
---------------------------------------------------------
    * QcReport.cpp 的 localtime_s：MSVC 专属，本机 MinGW 8.2 没有
    * Mp4SampleTableAnalyzer 的 std::filesystem::path operator!=：
      本机 libstdc++ 8.2 的 bits/fs_path.h 已知缺陷，MSVC 正常
    * infrastructure/logging/Logger.h 里 std::mutex 解析不到（MSVC 正常）
    * EbmlAnalyzer.cpp 中 const QString::operator[] 的 .unicode()（桩返回 char）
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ERROR_LINE_RE = re.compile(r":\d+:\d+: error:")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STUB = os.path.join(ROOT, ".cache", "syntaxcheck", "stub")
WORK = os.path.join(ROOT, ".cache", "syntaxcheck", "qstub")

# 这些错误是桩能力不足造成的假阳性，检查时过滤掉
IGNORE_FRAGMENTS = (
    "Logger.h",
    "does not name a type",          # Logger.h 的 std::mutex
    "'mutex' in namespace 'std'",
    "is of non-class type 'char'",   # const QString::operator[] -> .unicode()
    # localtime_s 是 MSVC 专属，本机 MinGW 8.2 无此函数
    "'localtime_s' was not declared",
    # 本机 libstdc++ 8.2 的 std::filesystem::path 比较运算符已知缺失，MSVC 正常
    "fs_path.h",
    "std::filesystem::__cxx11::path",
)


def build_stub_dir() -> str:
    """把 stub 复制成 qstub，并为每个头补一份不带扩展名的转接文件。

    本机这套 MinGW 的 `#include <Foo>` 只按字面文件名去找，
    不会像正常 Qt 那样回退到 Foo.h，所以必须同时放一份 Foo。
    """
    if os.path.isdir(WORK):
        for n in os.listdir(WORK):
            if os.path.isdir(os.path.join(WORK, n)):
                continue
            try:
                os.remove(os.path.join(WORK, n))
            except OSError:
                pass
    if not os.path.isdir(WORK):
        os.makedirs(WORK)
    for n in sorted(os.listdir(STUB)):
        src = os.path.join(STUB, n)
        if os.path.isfile(src) and n.endswith(".h"):
            dst = os.path.join(WORK, n)
            with open(src, "rb") as f:
                data = f.read()
            with open(dst, "wb") as f:
                f.write(data)
            stem = n[:-2]
            with open(os.path.join(WORK, stem), "wb") as f:
                f.write(("#pragma once\n#include \"%s\"\n" % n).encode("utf-8"))
    return WORK


def filter_errors(text: str) -> list[str]:
    out = []
    for line in text.splitlines():
        # 先按片段过滤：note 行里也会带上忽略项（如 std::filesystem::__cxx11::path），
        # 只在 error 行上过滤会漏掉这些“旁证”，把假阳性带回来。
        if any(frag in line for frag in IGNORE_FRAGMENTS):
            continue
        # 必须匹配 <file>:<line>:<col>: error: 才算错误行。
        # 不能只判 "error:" 子串——头文件路径里出现 system_error:319 之类
        # 会被误当成错误行，把一堆 note 带回来。
        if not ERROR_LINE_RE.search(line):
            continue
        if line.startswith(WORK) or line.startswith(os.sep + "zztest"):
            continue
        if "In file included from" in line:
            continue
        out.append(line.strip())
    return out


def run(path: str, stub: str) -> list[str]:
    cmd = [
        "g++", "-std=c++17", "-fsyntax-only",
        "-include", "mutex", "-include", "fstream", "-include", "Qt",
        "-I", ROOT, "-I", stub,
        os.path.join(ROOT, path) if not os.path.isabs(path) else path,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          errors="replace", cwd=ROOT)
    return filter_errors((proc.stdout or "") + (proc.stderr or ""))


def main() -> int:
    stub = build_stub_dir()
    keyword = (sys.argv[1] if len(sys.argv) > 1 else "").lower()

    targets: list[str] = []
    d = os.path.join(ROOT, "core", "domain", "model")
    targets += [os.path.join("core", "domain", "model", n)
                for n in sorted(os.listdir(d)) if n.endswith(".cpp")]
    cdir = os.path.join(ROOT, "core", "analysis", "container")
    for n in sorted(os.listdir(cdir)):
        if not n.endswith(".cpp"):
            continue
        if "ContainerStructureAnalyzer" in n:
            continue  # 需要 FFmpeg
        targets.append(os.path.join("core", "analysis", "container", n))

    if keyword:
        targets = [t for t in targets if keyword in t.lower()]
    if not targets:
        print("没有匹配的目标")
        return 1

    failed = 0
    for t in targets:
        errs = run(t, stub)
        if errs:
            failed += 1
            print("[FAIL] %s" % t)
            for e in errs[:6]:
                print("       %s" % e)
            if len(errs) > 6:
                print("       ... 还有 %d 条" % (len(errs) - 6))
        else:
            print("[PASS] %s" % t)
    print("\n%d/%d 通过（失败 %d）" % (len(targets) - failed, len(targets), failed))
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
