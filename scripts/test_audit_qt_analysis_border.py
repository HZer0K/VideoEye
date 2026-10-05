#!/usr/bin/env python3
"""audit_qt_analysis_border.py 的退出码自测。

盯的是同一类 bug：脚本打印了 [命中] 却 return 0，CI/pre-commit 一路绿着把
违规合并进主干。本项目已经踩过两次（check_layering.py、audit_qt_domain_border.py
各一次），所以在给新审计脚本配自测时，一律直接断言 main() 的返回码，
不看它的输出。

用例：
    1. 干净文件（纯 std）                      -> 0
    2. #include <QString> 尖括号 include      -> 1   （check_layering 看不见这种）
    3. 裸 QString 符号                        -> 1
    4. 只在注释里提到 QString                  -> 0   （注释不算依赖）
    5. "core/analysis" 之外放 Qt 文件          -> 不受影响（本脚本只扫 analysis）

外加一项：拿真实仓库跑一遍，断言当前确实干净（退出 0）。

用法:
    python scripts/test_audit_qt_analysis_border.py
退出码: 全部通过 0，任一断言失败 1。
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIT_PATH = os.path.join(HERE, "audit_qt_analysis_border.py")
REPO_ROOT = os.path.dirname(HERE)

CLEAN = """#include <string>
#include <vector>

bool F(const std::string& p, std::vector<int>& out) {
    out.push_back(static_cast<int>(p.size()));
    return true;
}
"""

HAS_INCLUDE = """#include <QString>

bool F(const std::string& p);
"""

HAS_SYMBOL = """bool F(const std::string& p) {
    return p == QString("x");
}
"""

COMMENT_ONLY = """// 以前这里写的是 QString，迁移后一律换成 std::string。
/*
 * QStringLiteral 这种也别再引进来。
 */
bool F(const std::string& p) {
    return !p.empty();
}
"""

# 放在 analysis 之外的文件：本脚本只守 core/analysis，不该被它扫到。
# 注意 core/analysis 目录必须真实存在（哪怕是空的）—— 脚本对"扫描根不存在"
# 是报错退出的，那不是本用例想测的分支。
OUTSIDE = """#include <QString>
bool G() { return true; }
"""


def load_audit_module():
    spec = importlib.util.spec_from_file_location("audit_qt_analysis_border", AUDIT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("无法加载 %s" % AUDIT_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_tree(root, files):
    for rel, text in files.items():
        path = os.path.join(root, rel)
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def run_audit(mod, root, files):
    """把扫描根指到临时目录里的 core/analysis，返回 (退出码, 输出文本)。"""
    write_tree(root, files)
    mod.SCAN_ROOT = os.path.join(root, "core", "analysis")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = mod.main()
    return code, buf.getvalue()


def case(name, files, expect_code, expect_marker=None):
    mod = load_audit_module()
    tmp = tempfile.mkdtemp(prefix="audit_analysis_selftest_")
    try:
        code, out = run_audit(mod, tmp, files)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    problems = []
    if code != expect_code:
        problems.append("退出码 %d, 期望 %d" % (code, expect_code))
    if expect_marker and expect_marker not in out:
        problems.append("输出里找不到标记 %r" % expect_marker)
    if problems:
        print("  [FAIL] %s" % name)
        for p in problems:
            print("         %s" % p)
        print("  ---- 审计输出 ----")
        for line in out.splitlines():
            print("  | %s" % line)
        return False
    print("  [ OK ] %s (exit=%d)" % (name, code))
    return True


def check_real_repo(mod):
    """真实仓库现状：core/analysis 应当零 Qt（退出 0）。

    这条同时防「自测假绿」—— 前面几条用例全绿也可能只是因为路径拼错、
    SCAN_ROOT 指歪、根本没扫到文件。这里要求真实目录真的一个命中都没有。
    """
    mod.ROOT = REPO_ROOT
    mod.SCAN_ROOT = os.path.join(REPO_ROOT, "core", "analysis")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = mod.main()
    out = buf.getvalue()
    if code != 0:
        print("  [FAIL] 真实仓库 core/analysis 仍有 Qt（退出码 %d）" % code)
        for line in out.splitlines()[:40]:
            print("  | %s" % line)
        return False
    print("  [ OK ] 真实仓库 core/analysis 零 Qt (exit=0)")
    return True


def main():
    print("audit_qt_analysis_border.py 退出码自测")
    print("-" * 60)

    ok = True
    ok &= case("纯 std 文件", {"core/analysis/FakeClean.cpp": CLEAN}, 0)
    ok &= case("尖括号 #include <QString>",
               {"core/analysis/FakeInc.cpp": HAS_INCLUDE}, 1, "[命中]")
    ok &= case("裸 QString 符号",
               {"core/analysis/FakeSym.cpp": HAS_SYMBOL}, 1, "[命中]")
    ok &= case("只在注释里提到 QString",
               {"core/analysis/FakeComment.cpp": COMMENT_ONLY}, 0)
    ok &= case("tree 里放 analysis 外的 Qt 文件",
               {"core/analysis/keep.mk": "", "core/player/FakeOutside.cpp": OUTSIDE}, 0)
    print("-" * 60)
    ok &= check_real_repo(load_audit_module())

    if ok:
        print("\n全部通过：audit_qt_analysis_border.py 的退出码语义（0=放行 / 1=拦截）符合预期")
        return 0
    print("\n有断言失败：要么本该拦下的没拦，要么真实仓库还没迁完")
    return 1


if __name__ == "__main__":
    sys.exit(main())
