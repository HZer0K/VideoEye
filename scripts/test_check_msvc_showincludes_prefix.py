#!/usr/bin/env python3
r"""scripts/check_msvc_showincludes_prefix.py 的自测。

只覆盖"不需要真的跑 cl.exe"的那部分：解析 CMake 探测文件、解析 CMakeCache
里的 VSLANG 启动器、前缀归一比较。探测 cl.exe 的那条路径依赖本机 VS，
放到守卫本体里，环境不具备时会自己 SKIP。

用法: python scripts/test_check_msvc_showincludes_prefix.py
"""

from __future__ import print_function

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_msvc_showincludes_prefix as guard  # noqa: E402

FAILURES = []


def check(name, got, want):
    if got != want:
        FAILURES.append("%s: 期望 %r，实得 %r" % (name, want, got))


def make_build_dir(tmp, prefix=None, launcher=None, with_files=True):
    """搭一个最小可认的构建目录骨架。prefix/launcher 为 None 时不写对应文件。"""
    d = os.path.join(tmp, "bd")
    if with_files:
        os.makedirs(os.path.join(d, "CMakeFiles", "3.27.1"))
        with open(os.path.join(d, "CMakeFiles", "3.27.1",
                               "CMakeCXXCompiler.cmake"), "w") as fh:
            fh.write('set(CMAKE_CXX_COMPILER_ID "MSVC")\n')
            if prefix is not None:
                fh.write('set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "%s")\n' % prefix)
    if launcher is not None:
        with open(os.path.join(d, "CMakeCache.txt"), "w") as fh:
            fh.write("CMAKE_CXX_COMPILER_LAUNCHER:STRING=%s\n" % launcher)
    return d


def main():
    tmp = tempfile.mkdtemp(prefix="ve_prefix_selftest_")
    try:
        # --- 前缀解析：英文 / 中文 / 缺失 ---
        d_en = make_build_dir(tmp + "/en", "Note: including file: ")
        check("解析英文前缀",
              guard.read_recorded_prefix(d_en)[0], "Note: including file: ")

        d_zh = make_build_dir(tmp + "/zh",
                              "\u6ce8\u610f: \u5305\u542b\u6587\u4ef6:  ")
        got_zh = guard.read_recorded_prefix(d_zh)[0]
        check("解析中文前缀", guard.normalize(got_zh),
              guard.normalize("\u6ce8\u610f: \u5305\u542b\u6587\u4ef6:"))

        d_none = make_build_dir(tmp + "/none", None)
        check("没有前缀时返回 None", guard.read_recorded_prefix(d_none)[0], None)

        # --- VSLANG 启动器解析 ---
        check("读出 VSLANG=1033",
              guard.read_vslang(make_build_dir(tmp + "/v1", None,
                                               "cmake;-E;env;VSLANG=1033")),
              "1033")
        check("启动器不带 VSLANG 时返回 None",
              guard.read_vslang(make_build_dir(tmp + "/v2", None,
                                               "ccache")),
              None)
        check("没有 CMakeCache 时返回 None",
              guard.read_vslang(make_build_dir(tmp + "/v3", None, None)),
              None)

        # --- 归一比较：中英文不同、但各自与自己对得上 ---
        check("英文与英文一致",
              guard.normalize("Note: including file: ")
              == guard.normalize("Note: including file:"), True)
        check("中英互不相同",
              guard.normalize("Note: including file: ")
              == guard.normalize("\u6ce8\u610f: \u5305\u542b\u6587\u4ef6: "),
              False)
        check("None 归一后仍是 None", guard.normalize(None), None)

        # --- 没钉语言的目录不得判红（反 flaky 的关键一条）---
        # 那种目录构建时用的是"当下环境的语言"，会随 VS 语言包/系统区域而变。
        # 拿它当期望值会让守卫在全量 ctest 里偶发误报。默认模式必须放过它。
        plain = make_build_dir(tmp + "/plain", "Note: including file: ", None)
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "check_msvc_showincludes_prefix.py")
        sys_exe = sys.executable or "python3"
        proc = subprocess.run([sys_exe, script, plain],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check("没钉语言的目录默认不判红", proc.returncode, 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if FAILURES:
        print("自测失败:", file=sys.stderr)
        for f in FAILURES:
            print("  " + f, file=sys.stderr)
        return 1
    print("自测通过: check_msvc_showincludes_prefix.py 的解析与比较逻辑")
    return 0


if __name__ == "__main__":
    sys.exit(main())
