#!/usr/bin/env python3
"""audit_namespace_layout.py 的退出码自测。

为什么要有这个：本项目已经栽过两次「审计脚本打印了违规、进程却 return 0」——
看输出人肉确认是没问题的，但 CI / pre-commit 拿到的永远是绿灯。所以这里不测正则
写得对不对，只测**退出码语义**：干净仓库必须 0，出现没登记的违规必须 1。

造四种假场景（全部走临时目录，跑完自动清掉）：
    1. 干净仓库                       -> 0
    2. 重新引入 namespace analyzer     -> 1（R1）
    3. 重新引入 utils:: 引用           -> 1（R2）
    4. 子命名空间与目录不一致          -> 1（R3）
外加一项：拿真实仓库跑一遍，断言当前确实在册偏差之内（退出 0）。

用法:
    python scripts/test_audit_namespace_layout.py
退出码: 全部通过 0，任一断言失败 1。
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIT_PATH = os.path.join(HERE, "audit_namespace_layout.py")

CLEAN = """#pragma once

namespace videoeye {

class Widget {
public:
    int width() const { return w; }

private:
    int w = 0;
};

}  // namespace videoeye
"""

BAD_REDECLARE_ANALYZER = """#pragma once

namespace videoeye {

namespace analyzer {

class Widget {
public:
    int width() const { return w; }

private:
    int w = 0;
};

} // namespace analyzer

} // namespace videoeye
"""

BAD_REDECLARE_UTILS = """#pragma once

namespace videoeye {

namespace utils {

class Reader {
public:
    int size() const { return n; }

private:
    int n = 0;
};

} // namespace utils

} // namespace videoeye
"""

BAD_REF = """#pragma once

namespace videoeye {

class Reader {
public:
    int size() const { return n; }

private:
    int n = 0;
};

inline int Peek() {
    Reader r;
    return r.size() + utils::Helper();   // 故意写回 utils::
}

} // namespace videoeye
"""

BAD_DIR_MATCH = """#pragma once

namespace videoeye {

namespace stream {

class Reader {
public:
    int size() const { return n; }

private:
    int n = 0;
};

} // namespace stream

} // namespace videoeye
"""


def load_audit_module():
    spec = importlib.util.spec_from_file_location("audit_namespace_layout", AUDIT_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_in(files):
    """把 files 写进临时仓库跑审计，返回 (退出码, 输出)。"""
    with tempfile.TemporaryDirectory() as tmp:
        for rel, text in files.items():
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
        mod = load_audit_module()
        mod.ROOT = tmp
        buf = io.StringIO()
        code = 0
        with contextlib.redirect_stdout(buf):
            # 审计脚本是「有违规就 sys.exit(1)」的写法（跟 check_layering.py 一致），
            # 干净路径则是自然返回 None —— 两种收尾都得归一化成 int，否则断言拿到 None。
            try:
                mod.main()
            except SystemExit as exc:
                code = exc.code if isinstance(exc.code, int) else 1
        return code, buf.getvalue()


def check_real_repo(mod):
    buf = io.StringIO()
    code = 0
    with contextlib.redirect_stdout(buf):
        try:
            mod.main()
        except SystemExit as exc:
            code = exc.code if isinstance(exc.code, int) else 1
    out = buf.getvalue()
    if code != 0:
        print("  [FAIL] 真实仓库扫描退出码 %d, 期望 0（当前应当都在册）" % code)
        for line in out.splitlines():
            print("  | %s" % line)
        return False
    print("  [ OK ] 真实仓库扫描干净 (exit=0)")
    return True


def main():
    print("audit_namespace_layout.py 退出码自测")
    print("-" * 60)

    cases = [
        ("干净仓库 -> 必须放行",
         {"core/codec/Widget.h": CLEAN}, 0),
        ("重新引入 namespace analyzer -> R1 必须红",
         {"core/codec/Widget.h": BAD_REDECLARE_ANALYZER}, 1),
        ("重新引入 namespace utils -> R1 必须红",
         {"core/codec/Widget.h": BAD_REDECLARE_UTILS}, 1),
        ("写回 utils:: 引用 -> R2 必须红",
         {"core/codec/Widget.h": BAD_REF}, 1),
        ("子命名空间与目录不一致 -> R3 必须红",
         {"core/codec/Widget.h": BAD_DIR_MATCH}, 1),
    ]

    ok = True
    for name, files, expect in cases:
        code, out = run_in(files)
        hit = "OK"
        if code != expect:
            hit = "FAIL"
            ok = False
        print("  [%s] %-38s exit=%d 期望=%d" % (hit, name, code, expect))

    print("-" * 60)
    ok &= check_real_repo(load_audit_module())

    if ok:
        print("\n全部通过：审计脚本的退出码语义（0=放行 / 1=拦截）符合预期")
        return 0
    print("\n有断言失败：退出码语义可能又被改坏了（回归：打印违规却 return 0）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
