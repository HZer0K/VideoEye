#!/usr/bin/env python3
"""只做语法检查，不生成 obj：改动几个文件后快速确认能编过。

背景：本机沙箱会拦 vcvars64.bat（走 cmd.exe 时直接 SIGTERM），所以没法靠
"先跑 vcvars 再 ninja" 这条路。这里直接把 MSVC / Windows SDK 的 include 目录
拼进 INCLUDE 环境变量，再用 build/release/compile_commands.json 里的原始命令
跑 cl.exe /Zs（只语法检查，不产出文件，因此不需要 linker）。

用法：
    python scripts/syntaxcheck.py QcReportExporter MediaExporter
    python scripts/syntaxcheck.py core/analysis    # 参数是文件名的子串
"""
import json
import os
import re
import subprocess
import sys

MSVC = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
SDK = r"C:\Program Files (x86)\Windows Kits\10"
SDKV = "10.0.26100.0"

INCLUDE = ";".join([
    MSVC + r"\include",
    SDK + r"\Include" + "\\" + SDKV + r"\ucrt",
    SDK + r"\Include" + "\\" + SDKV + r"\um",
    SDK + r"\Include" + "\\" + SDKV + r"\shared",
    SDK + r"\Include" + "\\" + SDKV + r"\winrt",
    SDK + r"\Include" + "\\" + SDKV + r"\cppwinrt",
])
LIB = ";".join([
    MSVC + r"\lib\x64",
    SDK + r"\Lib" + "\\" + SDKV + r"\ucrt\x64",
    SDK + r"\Lib" + "\\" + SDKV + r"\um\x64",
])


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    targets = sys.argv[1:]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    db_path = os.path.join(root, "build", "release", "compile_commands.json")
    with open(db_path, encoding="utf-8") as f:
        db = json.load(f)

    env = os.environ.copy()
    env["INCLUDE"] = INCLUDE
    env["LIB"] = LIB
    env["MSYS_NO_PATHCONV"] = "1"

    ok = True
    found = 0
    for entry in db:
        path = entry["file"].replace("\\", "/")
        if not any(t.replace("\\", "/") in path for t in targets):
            continue
        found += 1
        cmd = re.sub(r"/Fo\S+", "", entry["command"])
        cmd = re.sub(r"/Fd\S+", "", cmd)
        cmd = re.sub(r"\s/c\s", " ", cmd) + " /Zs"
        print("=== " + os.path.basename(path))
        r = subprocess.run(cmd, cwd=entry["directory"], shell=True, env=env,
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace")
        if r.returncode != 0:
            ok = False
            print((r.stdout + r.stderr)[-3000:])
        else:
            print("OK")
    if found == 0:
        print("没有匹配到任何文件，检查参数（用文件名的子串，如 QcReportExporter）")
        return 1
    print("checked=%d %s" % (found, "ALL_OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
