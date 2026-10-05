#!/usr/bin/env python3
"""check_layering.py 的退出码自测。

盯的是什么 bug
--------------
2026-10-05 之前，check_layering.py 发现违规时**只打印 [!] 然后照常 return 0**。
结果是：

  * `.git/hooks/pre-commit` 里的 `if ! "$PY" scripts/check_layering.py` 永远不成立
    —— 那条分层门形同虚设，违规代码能一路提交；
  * CI 的 lint 步骤（`run: python3 scripts/check_layering.py`）一路绿。

违规信息人肉看得到，机器永远拦不住 —— 和 audit_qt_domain_border.py 当年
「打印了 [命中] 却 return 0」是同一类 bug。脚本已修（有违规 sys.exit(1)），
这里用临时目录把退出码语义钉死，防止再退回去。

用例（全部走子进程，断言的是真实进程退出码，不是函数返回值）
------------------------------------------------------------
  1. 干净仓库                                  -> 0
  2. domain 反向 include core/analysis         -> 1（最该拦的那条）
  3. analysis 反向 include core/reporting      -> 1
  4. media   反向 include core/analysis        -> 1
  5. NO_FFMPEG 层（core/domain）include libav* -> 1（FFmpeg 也不许进 domain）
  6. 层内自引用（core/ffmpeg_io 包含自己的头文件）-> 0，不许误报
  7. 真实仓库现状                              -> 0

外加一项 matches_banned 的单元断言：禁 "core/ffmpeg" 时
"core/ffmpeg_io/xxx.h" 不能算违规（两个不相干的层，朴素前缀匹配会踩）。

用法:
    python scripts/test_check_layering.py
退出码: 全部通过 0，任一断言失败 1。
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "check_layering.py")
REPO_ROOT = os.path.dirname(HERE)


def write_tree(root, files):
    for rel, text in files.items():
        path = os.path.join(root, rel)
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        # newline="\n": 源码换行符在 Windows 上是 LF（少数文件例外），
        # 用默认换行写会掺进 \r\n，和真实仓库不一致，测的就不是被测对象了。
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def run_check(root):
    """在 root 这份临时仓库里跑一遍 check_layering.py，返回 (退出码, 输出)。"""
    env = dict(os.environ)
    env["VIDEOEYE_ROOT"] = root
    proc = subprocess.run(
        [sys.executable, SCRIPT],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def case(name, files, expect_code, expect_marker=None, expect_absent=None):
    root = tempfile.mkdtemp(prefix="layering_selftest_")
    try:
        write_tree(root, files)
        code, out = run_check(root)
    finally:
        shutil.rmtree(root, ignore_errors=True)

    problems = []
    if code != expect_code:
        problems.append("退出码 %d, 期望 %d" % (code, expect_code))
    if expect_marker and expect_marker not in out:
        problems.append("输出里找不到标记 %r" % expect_marker)
    if expect_absent and expect_absent in out:
        problems.append("输出里不该出现 %r" % expect_absent)

    if problems:
        print("  [FAIL] %s" % name)
        for p in problems:
            print("         %s" % p)
        for line in out.splitlines():
            print("  | %s" % line)
        return False
    print("  [ OK ] %s (exit=%d)" % (name, code))
    return True


def check_matches_banned_unit():
    """禁 "core/ffmpeg" 不能误伤 "core/ffmpeg_io/"（路径段比较，不是裸前缀）。"""
    root = tempfile.mkdtemp(prefix="layering_selftest_")
    original_root = os.environ.get("VIDEOEYE_ROOT")
    os.environ["VIDEOEYE_ROOT"] = root
    try:
        spec = importlib.util.spec_from_file_location("check_layering", SCRIPT)
        if spec is None or spec.loader is None:
            print("  [FAIL] 无法加载 %s" % SCRIPT)
            return False
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except SystemExit as exc:
        print("  [FAIL] import check_layering 时提前退出（退出码 %s）：违规检测没跑完"
              % exc.code)
        return False
    finally:
        os.environ.pop("VIDEOEYE_ROOT", None)
        if original_root is not None:
            os.environ["VIDEOEYE_ROOT"] = original_root
        shutil.rmtree(root, ignore_errors=True)

    problems = []
    if mod.matches_banned("core/ffmpeg_io/Baz.h", ["core/ffmpeg"]):
        problems.append("core/ffmpeg_io 被 core/ffmpeg 的禁区误吞了")
    if not mod.matches_banned("core/ffmpeg/av.h", ["core/ffmpeg"]):
        problems.append("core/ffmpeg 自己的头文件没落在禁区里")
    # 注意 matches_banned 是「include 有没有落在禁区目录里」的纯函数，跟被扫的是哪
    # 一层无关 —— 所以它照样会对"禁区里写了自己的层目录"返回 True。真正的层内自引用
    # 不算违规，是靠各层 RULES 里都不把自己写进禁区实现的（见 case "层内自引用"）。
    if mod.matches_banned("core/ffmpeg_io/Baz.h", ["core/analysis"]):
        problems.append("兄弟层的头文件被 core/analysis 的禁区吞了")
    if problems:
        print("  [FAIL] matches_banned 单元断言")
        for p in problems:
            print("         %s" % p)
        return False
    print("  [ OK ] matches_banned 路径段比较正确")
    return True


def check_real_repo():
    """真实仓库现状：应当干净（退出 0）。

    防「自测假绿」—— 前几条用例全绿也可能是因为 VIDEOEYE_ROOT 传歪、脚本压根没扫到
    文件。这条要求当前主干确实零违规，才算整条链路通。
    """
    env = dict(os.environ)
    env.pop("VIDEOEYE_ROOT", None)
    proc = subprocess.run(
        [sys.executable, SCRIPT],
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
    )
    if proc.returncode != 0:
        print("  [FAIL] 真实仓库分层检查退出码 %d, 期望 0" % proc.returncode)
        for line in (proc.stdout or "").splitlines():
            print("  | %s" % line)
        return False
    print("  [ OK ] 真实仓库分层干净 (exit=0): %s" % (proc.stdout or "").strip())
    return True


def main():
    print("check_layering.py 退出码自测")
    print("-" * 60)

    ok = True
    ok &= case("干净仓库", {}, 0, "OK")
    ok &= case("domain 反向 include core/analysis -> 必须拦",
               {"core/domain/model/Bad.h": '#include "core/analysis/StreamAnalyzer.h"\n'},
               1, "[退出码] 1")
    ok &= case("analysis 反向 include core/reporting -> 必须拦",
               {"core/analysis/Fakes.cpp": '#include "core/reporting/QcReport.h"\n'},
               1, "[退出码] 1")
    ok &= case("media 反向 include core/analysis -> 必须拦",
               {"core/media/Fake.cpp": '#include "core/analysis/StreamAnalyzer.h"\n'},
               1, "[退出码] 1")
    ok &= case("domain 直接 include FFmpeg -> 必须拦",
               {"core/domain/model/Bad2.h": "#include <libavformat/avformat.h>\n"},
               1, "[退出码] 1")
    ok &= case("层内自引用不算违规",
               {"core/ffmpeg_io/Fake.h": '#include "core/ffmpeg_io/Baz.h"\n'},
               0, "OK")
    print("-" * 60)
    ok &= check_matches_banned_unit()
    ok &= check_real_repo()

    if ok:
        print("\n全部通过：分层检查退出码语义（0=放行 / 1=拦截）符合预期")
        return 0
    print("\n有断言失败：分层检查可能退回到「打印了 [!] 却 return 0」")
    return 1


if __name__ == "__main__":
    sys.exit(main())
