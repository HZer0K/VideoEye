#!/usr/bin/env python3
"""check_automoc_registry.py 的退出码与判据自测。

盯的是什么 bug
--------------
tests/CMakeLists.txt 里的 AUTOMOC 名单（VIDEOEYE_TESTS_NEEDING_AUTOMOC）是
"哪些测试目标需要 moc"的唯一事实源。它有两个漂移方向：

  * 漏登记 —— 链接期缺 vtable，能发现但要等编完；
  * 多登记 —— 悄悄退化成"每个目标都开 AUTOMOC"，优化被一点点吃回去，
    而且**没有任何报错**。这才是真危险的方向，所以 check_automoc_registry.py
    两个方向都要拦。

这里钉死的是三件事：
  1. 一致 -> 0，漏登记/多登记/名单变量消失 -> 1（退出码语义，不是返回值）；
  2. 判据是"目标列出的源文件本身"有元对象宏，**不递归 include** ——
     递归版会把名单从 9 个吹到 43 个（实测: test_chart_decimation.cpp
     经两层 include 摸到 MetricChartWidget.h 的 Q_OBJECT，但 CMake 生成的
     mocs_compilation.cpp 依旧是空壳）；
  3. 注释里提到 Q_OBJECT 不算数。

全部走子进程，断言真实进程退出码。

用法:
    python scripts/test_check_automoc_registry.py
退出码: 全部通过 0，任一断言失败 1。
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_automoc_registry.py")
PY = sys.executable

CMAKE_TEMPLATE = """# 测试用最小 tests/CMakeLists.txt
set(VIDEOEYE_TESTS_NEEDING_AUTOMOC
%(registry)s
)

function(videoeye_add_test name)
endfunction()

%(calls)s
"""

failures = []


def build_repo(registry, calls, files):
    """在临时目录搭一个最小仓库，返回它的根路径。"""
    root = tempfile.mkdtemp(prefix="automoc_registry_")
    tests = os.path.join(root, "tests")
    unit = os.path.join(tests, "unit")
    os.makedirs(unit)
    for rel, content in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w", encoding="utf-8").write(content)
    registry_block = ("\n".join("    " + n for n in registry)
                      if registry else "")
    open(os.path.join(tests, "CMakeLists.txt"), "w", encoding="utf-8").write(
        CMAKE_TEMPLATE % {"registry": registry_block, "calls": calls})
    return root


def run(root, extra=None):
    env = dict(os.environ)
    env["VIDEOEYE_ROOT"] = root
    return subprocess.run([PY, CHECK] + (extra or []),
                          capture_output=True, env=env)


def expect(tag, cond, detail=""):
    print(("  [ok]   " if cond else "  [FAIL] ") + tag + (
        "" if cond else "  <- " + detail))
    if not cond:
        failures.append(tag)


def case(tag, registry, calls, files, want_rc, extra=None):
    root = build_repo(registry, calls, files)
    try:
        proc = run(root, extra)
        ok = proc.returncode == want_rc
        expect(tag, ok, "rc=%d 期望 %d | %s" % (
            proc.returncode, want_rc,
            (proc.stdout + proc.stderr).decode("utf-8", "replace").strip()[:200]))
        return proc
    finally:
        shutil.rmtree(root, ignore_errors=True)


PLAIN = "int main() { return 0; }\n"
WITH_OBJECT = "#include <QObject>\nclass Foo : public QObject {\n    Q_OBJECT\n};\n"
MENTION_ONLY = "// 这个测试只是提到 Q_OBJECT，本身没有元对象\nint main() {}\n"

print("== 1. 一致 -> 0 ==")
case("干净仓库(登记与源码一致) -> 0",
     ["test_a"],
     "videoeye_add_test(test_a unit/test_a.cpp)",
     {"tests/unit/test_a.cpp": WITH_OBJECT}, 0)

print("== 2. 漏登记 -> 1 ==")
case("源码有 Q_OBJECT 但没登记 -> 1",
     [],
     "videoeye_add_test(test_a unit/test_a.cpp)",
     {"tests/unit/test_a.cpp": WITH_OBJECT}, 1)

print("== 3. 多登记 -> 1（静默退化，最该拦的方向）==")
case("登记了其实不需要 moc 的目标 -> 1",
     ["test_a"],
     "videoeye_add_test(test_a unit/test_a.cpp)",
     {"tests/unit/test_a.cpp": PLAIN}, 1)

print("== 4. 名单变量消失 -> 1 ==")
root = tempfile.mkdtemp(prefix="automoc_registry_")
os.makedirs(os.path.join(root, "tests", "unit"))
open(os.path.join(root, "tests", "CMakeLists.txt"), "w", encoding="utf-8").write(
    "# 名单变量被删了\nvideoeye_add_test(test_a unit/test_a.cpp)\n")
open(os.path.join(root, "tests", "unit", "test_a.cpp"), "w",
     encoding="utf-8").write(PLAIN)
proc = run(root)
expect("VIDEOEYE_TESTS_NEEDING_AUTOMOC 消失 -> 1", proc.returncode == 1,
       "rc=%d" % proc.returncode)
shutil.rmtree(root, ignore_errors=True)

print("== 5. 注释里的 Q_OBJECT 不算数 ==")
case("只在注释里提到 Q_OBJECT -> 0（不算需要 moc）",
     [],
     "videoeye_add_test(test_a unit/test_a.cpp)",
     {"tests/unit/test_a.cpp": MENTION_ONLY}, 0)

print("== 6. 不递归 include（钉死判据，防退回递归版）==")
# test_b.cpp -> include "ui/widgets/Plain.h" -> include "ui/widgets/Widget.h"
# 只有最里层的 Widget.h 有 Q_OBJECT; CMake AUTOMOC 不会为它生成 moc
case("两层 include 外的 Q_OBJECT 不算 -> 0",
     [],
     "videoeye_add_test(test_b unit/test_b.cpp)",
     {"tests/unit/test_b.cpp": '#include "ui/widgets/Plain.h"\nint main() {}\n',
      "ui/widgets/Plain.h": '#include "ui/widgets/Widget.h"\n',
      "ui/widgets/Widget.h": WITH_OBJECT}, 0)

print("== 7. 同目录同名头算数 ==")
case("源文件同名 .h 含 Q_OBJECT -> 需要登记",
     ["test_c"],
     "videoeye_add_test(test_c unit/test_c.cpp)",
     {"tests/unit/test_c.cpp": '#include "test_c.h"\nint main() {}\n',
      "tests/unit/test_c.h": WITH_OBJECT}, 0)

print("== 8. --fix 能吐出可用名单 ==")
root = build_repo([], "videoeye_add_test(test_d unit/test_d.cpp)",
                  {"tests/unit/test_d.cpp": WITH_OBJECT})
try:
    proc = run(root, ["--fix"])
    out = proc.stdout.decode("utf-8", "replace")
    expect("--fix 退出码 0 且含 test_d",
           proc.returncode == 0 and "test_d" in out, out[:200])
finally:
    shutil.rmtree(root, ignore_errors=True)

print("== 9. 真实仓库 -> 0 ==")
proc = subprocess.run([PY, CHECK], capture_output=True, cwd=os.path.dirname(HERE))
expect("本仓库现状 -> 0", proc.returncode == 0,
       (proc.stdout + proc.stderr).decode("utf-8", "replace")[:400])

print()
if failures:
    print("FAILED: %d 项" % len(failures))
    for f in failures:
        print("   - " + f)
    sys.exit(1)
print("全部通过")
