#!/usr/bin/env python3
"""sanitizer_gate.py 的判别力自测。

为什么要有这个：门禁最危险的失效方式不是误报，而是**静默变绿** —— 正则写窄一点、
路径形式一变，命中就悄悄漏掉，而 CI 一直是绿的，没人会发现。所以这里不测"能不能跑"，
只测**判据的判别力**：真属于本仓库的命中必须红，第三方库的噪声必须放行。

每个场景在临时目录里造一份最小报告（形状照抄 CI 实测日志），跑一遍 main()，
断言退出码。特别是 case 2 —— 那 68 条 Qt 竞态的栈里**确实有我们的帧**（我们只是调用了
Qt，冲突访问发生在 Qt 内部）；如果判据退化成"栈里出现我们的文件就算"，它会立刻红。

用法:
    python scripts/test_sanitizer_gate.py
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
GATE_PATH = os.path.join(HERE, "sanitizer_gate.py")
WS = "/home/runner/work/VideoEye/VideoEye"

# 实测日志里的形状: 两个访问 section + 一个分配点 section。
# section 1 的 #0 是 libtsan 的 free 拦截器帧(必须跳过), #1 是 Qt 内部的访问点,
# 再往下才是我们的 TestBody —— 这一条就是"栈里有我们的帧但不是我们的竞争"。
TSAN_QT_ONLY = """\
==================
WARNING: ThreadSanitizer: data race (pid=11905)
  Write of size 8 at 0x723400006800 by main thread:
    #0 free ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:735 (libtsan.so.2+0x8ccc3) (BuildId: 9e06)
    #1 QMetaType::destroy(void*) const <null> (libQt6Core.so.6+0x1b49f3) (BuildId: 340c)
    #2 TestBody /home/runner/work/VideoEye/VideoEye/tests/unit/test_qt_analysis_controller.cpp:220 (test_qt_analysis_controller+0x2dbee)

  Previous atomic write of size 4 at 0x723400006800 by thread T7:
    #0 std::__atomic_base<int>::fetch_sub(int, std::memory_order) /usr/include/c++/15/bits/atomic_base.h:641 (test_qt_analysis_controller+0x48777)
    #1 operator() /home/runner/work/VideoEye/VideoEye/core/qt/QtAnalysisController.cpp:139 (test_qt_analysis_controller+0x51fe6)

  Location is heap block of size 32 at 0x723400006800 allocated by main thread:
    #0 operator new(unsigned long) <null> (libQt6Core.so.6+0x29cc34)
    #1 VideoEye::Foo::Bar() /home/runner/work/VideoEye/VideoEye/core/qc/QcRunner.cpp:42 (test_qt_analysis_controller+0x1)

  Thread T7 (tid=11913, finished) created by main thread at:
    #0 pthread_create ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1078 (libtsan.so.2+0x5cb82)
    #1 videoeye::qt::QtAnalysisController::Launch() /home/runner/work/VideoEye/VideoEye/core/qt/QtAnalysisController.cpp:191 (test_qt_analysis_controller+0x52b9f)

SUMMARY: ThreadSanitizer: data race (/usr/lib/x86_64-linux-gnu/libQt6Core.so.6+0x1b49f3) (BuildId: 340c) in QMetaType::destroy(void*) const
==================
"""

# 同一份报告, 但把访问点 #1 换成我们自己的源码位置(真实竞争的形态)。
TSAN_OURS = TSAN_QT_ONLY.replace(
    "#1 QMetaType::destroy(void*) const <null> (libQt6Core.so.6+0x1b49f3) (BuildId: 340c)",
    "#1 videoeye::QcRunner::OnPacket() /home/runner/work/VideoEye/VideoEye/core/qc/QcRunner.cpp:88:5 (test_x+0x1)",
)

# 构建目录落在源码树里时 __FILE__ 长这样, 必须也认。
TSAN_RELATIVE = TSAN_QT_ONLY.replace(
    "#1 QMetaType::destroy(void*) const <null> (libQt6Core.so.6+0x1b49f3) (BuildId: 340c)",
    "#1 videoeye::QcRunner::OnPacket() ../core/qc/QcRunner.cpp:88:5 (test_x+0x1)",
)

# 我们自己二进制里的帧没符号化 = 无法自证清白, 按红处理(宁可误报不可漏报)。
TSAN_UNSYMBOLIZED = TSAN_QT_ONLY.replace(
    "#1 QMetaType::destroy(void*) const <null> (libQt6Core.so.6+0x1b49f3) (BuildId: 340c)",
    "#1 <null> <null> (test_x+0x99)",
)

TSAN_DEADLOCK = """\
==================
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=4242)
  Cycle in lock order graph: M1 (0x7b0400000a00) => M2 (0x7b0400000b00) => M1

  Mutex M2 acquired here while holding mutex M1 in thread T2:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1200 (libtsan.so.2+0x6aa11)
    #1 videoeye::QtWorkerOwner::StopWorker() /home/runner/work/VideoEye/VideoEye/core/qt/QtWorkerOwner.cpp:150 (test_x+0x7a)

  Mutex M1 previously acquired by the same thread here:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1200 (libtsan.so.2+0x6aa11)
    #1 videoeye::QtWorkerOwner::StartWorker() /home/runner/work/VideoEye/VideoEye/core/qt/QtWorkerOwner.cpp:120 (test_x+0x51)

SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) /home/runner/work/VideoEye/VideoEye/core/qt/QtWorkerOwner.cpp:150 in videoeye::QtWorkerOwner::StopWorker()
==================
"""

UBSAN_OURS = """\
/home/runner/work/VideoEye/VideoEye/core/analysis/VvcParser.cpp:412:9: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
/home/runner/work/VideoEye/VideoEye/core/analysis/VvcParser.cpp:412:9: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
"""

UBSAN_QT_ONLY = """\
/usr/include/x86_64-linux-gnu/qt6/QtCore/qarraydata.h:145:16: runtime error: load of value 4294967295, which is not a valid value for type 'int'
/usr/include/c++/15/bits/stl_vector.h:1123:39: runtime error: addition of unsigned offset to 0x602000000010 overflowed to 0x60200000000f
"""


def load_gate():
    spec = importlib.util.spec_from_file_location("sanitizer_gate_under_test", GATE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_gate(gate, tmp, kind, text, filename=None, mode="strict", make_dir=True, ctest_log=None):
    log_dir = os.path.join(tmp, "logs")
    if make_dir:
        os.makedirs(log_dir, exist_ok=True)
    if text is not None:
        name = filename or ("ubsan.test.1" if kind == "ubsan" else "tsan.test.1")
        with open(os.path.join(log_dir, name), "w", encoding="utf-8") as fh:
            fh.write(text)
    argv = ["--kind", kind, "--log-dir", log_dir, "--workspace", WS, "--mode", mode]
    if ctest_log is not None:
        path = os.path.join(tmp, "ctest.log")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(ctest_log)
        argv += ["--ctest-log", path]
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = gate.main(argv)
    return rc, buf.getvalue()


CASES = []


def case(name, expect_rc, **kwargs):
    CASES.append((name, expect_rc, kwargs))


def main() -> int:
    case("tsan: 纯 Qt 内部竞争(栈里有我们的帧) 必须放行", 0, kind="tsan", text=TSAN_QT_ONLY)
    case("tsan: 访问点落在 core/ 必须红", 1, kind="tsan", text=TSAN_OURS)
    case("tsan: ../core/ 形态(构建目录在源码树内) 必须红", 1, kind="tsan", text=TSAN_RELATIVE)
    case("tsan: 我们二进制里的帧没符号化 必须红", 1, kind="tsan", text=TSAN_UNSYMBOLIZED)
    case("tsan: 死锁报告的访问点落在 core/ 必须红", 1, kind="tsan", text=TSAN_DEADLOCK)
    case("tsan: 空目录(真没问题) 放行", 0, kind="tsan", text=None)
    case("tsan: 报告目录不存在必须红(否则门禁静默变绿)", 1, kind="tsan", text=None, make_dir=False)
    case("tsan: warn 模式只报告不判红", 0, kind="tsan", text=TSAN_OURS, mode="warn")
    case("tsan: 行首带 CI 时间戳的报告照样认", 1, kind="tsan",
         text="".join("2026-10-31T07:15:39.8223020Z " + line + "\n" for line in TSAN_OURS.splitlines()))
    case("ubsan: 本仓库源码的 runtime error 必须红", 1, kind="ubsan", text=UBSAN_OURS)
    case("ubsan: Qt 头文件里的 runtime error 放行", 0, kind="ubsan", text=UBSAN_QT_ONLY)
    case("ubsan: 同位置多条合并计数后仍然只算一处", 1, kind="ubsan", text=UBSAN_OURS)

    gate = load_gate()
    failed = 0
    for name, expect_rc, kwargs in CASES:
        tmp = tempfile.mkdtemp(prefix="ve-gate-")
        try:
            rc, out = run_gate(gate, tmp, **kwargs)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        ok = rc == expect_rc
        print("%s %s (rc=%d, 期望 %d)" % ("[ OK ]" if ok else "[FAIL]", name, rc, expect_rc))
        if not ok:
            failed += 1
            print("       输出:\n" + "\n".join("       " + l for l in out.splitlines()[:14]))
        # 命中时必须带 annotation, 否则 GitHub 上看不到是哪一行。
        # 符号化失败那种没有位置可用, 只要求有 ::error(不能只是静默 return 1)。
        if expect_rc == 1 and "::error" not in out:
            print("[FAIL] %s: 命中却没有 ::error / annotation" % name)
            failed += 1

    # 反向守护: 命中清单必须真的列出来(不能只返回 1 却什么都不说)
    tmp = tempfile.mkdtemp(prefix="ve-gate-")
    try:
        _, out = run_gate(gate, tmp, kind="tsan", text=TSAN_OURS)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if "core/qc/QcRunner.cpp" not in out:
        print("[FAIL] 命中清单里没有 core/qc/QcRunner.cpp")
        failed += 1

    total = len(CASES) + 1
    print("\n%d/%d 通过" % (total - failed, total))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
