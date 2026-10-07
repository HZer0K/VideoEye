#!/usr/bin/env python3
"""audit_link_visibility.py 的退出码自测。

为什么要有这个：审计脚本靠退出码拦提交，而本项目已经栽过两次「打印了违规、进程却
return 0」—— 看输出人肉确认没问题，CI / pre-commit 拿到的却永远是绿灯。所以这里不测
正则写得全不全，只测**退出码语义**（干净仓库必须 0，违规必须 1）以及两类系统依赖、
递归闭包这几个核心判定。

造七种假场景（全部走临时目录，跑完自动清掉）：
    1. 干净仓库（头文件不带系统依赖）          -> 0
    2. 公共头要 Qt，Qt6::Core 挂 PRIVATE       -> 1
    3. 公共头要 Qt，Qt6::Core 挂 PUBLIC        -> 0
    4. 公共头要 FFmpeg，FFmpeg::* 挂 PRIVATE   -> 1
    5. P2-8 形态：公共头引用了另一模块的头，
       被引用模块的 Qt 依赖没透到本模块         -> 1（且只应命中引用方）
    6. 同 5 但边挂对（PUBLIC 链到底）          -> 0
    7. videoeye_add_module 的目录解析不出来    -> 1（不允许静默漏查）
外加一项：拿真实仓库跑一遍，断言当前全绿。

用法:
    python scripts/test_audit_link_visibility.py
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
AUDIT_PATH = os.path.join(HERE, "audit_link_visibility.py")

CLEAN_CMAKE = """\
file(GLOB_RECURSE ALPHA_SOURCES CONFIGURE_DEPENDS "alpha/*.cpp" "alpha/*.h")

videoeye_add_module(VideoEyeAlpha ${ALPHA_SOURCES})
target_link_libraries(VideoEyeAlpha
    PRIVATE VideoEyeBeta
)
"""

QT_PRIVATE_CMAKE = """\
file(GLOB_RECURSE ALPHA_SOURCES CONFIGURE_DEPENDS "alpha/*.cpp" "alpha/*.h")

videoeye_add_module(VideoEyeAlpha ${ALPHA_SOURCES})
target_link_libraries(VideoEyeAlpha
    PRIVATE Qt6::Core
)
"""

QT_PUBLIC_CMAKE = """\
file(GLOB_RECURSE ALPHA_SOURCES CONFIGURE_DEPENDS "alpha/*.cpp" "alpha/*.h")

videoeye_add_module(VideoEyeAlpha ${ALPHA_SOURCES})
target_link_libraries(VideoEyeAlpha
    PUBLIC Qt6::Core
)
"""

FFMPEG_PRIVATE_CMAKE = """\
file(GLOB_RECURSE ALPHA_SOURCES CONFIGURE_DEPENDS "alpha/*.cpp" "alpha/*.h")

videoeye_add_module(VideoEyeAlpha ${ALPHA_SOURCES})
target_link_libraries(VideoEyeAlpha
    PRIVATE FFmpeg::avformat
)
"""

# 5 / 6：Alpha 的公共头引用 Beta 的头；Beta 自己有 Qt 依赖。
# 区别只在两条边：Alpha->Beta 与 Beta->Qt6::Core 的 PUBLIC / PRIVATE。
TRANSITIVE_CMAKE = """\
file(GLOB_RECURSE ALPHA_SOURCES CONFIGURE_DEPENDS "alpha/*.cpp" "alpha/*.h")
file(GLOB_RECURSE BETA_SOURCES CONFIGURE_DEPENDS "beta/*.cpp" "beta/*.h")

videoeye_add_module(VideoEyeAlpha ${ALPHA_SOURCES})
target_link_libraries(VideoEyeAlpha
    PRIVATE VideoEyeBeta
)

videoeye_add_module(VideoEyeBeta ${BETA_SOURCES})
target_link_libraries(VideoEyeBeta
    PUBLIC Qt6::Core
)
"""

TRANSITIVE_OK_CMAKE = TRANSITIVE_CMAKE.replace(
    "PRIVATE VideoEyeBeta", "PUBLIC VideoEyeBeta"
)

NO_GLOB_CMAKE = """\
videoeye_add_module(VideoEyeAlpha ${ALPHA_SOURCES})
"""

PLAIN_H = """\
#pragma once

#include <string>

namespace videoeye {

class Widget {
public:
    int width() const { return w; }

private:
    int w = 0;
};

}  // namespace videoeye
"""

QT_H = """\
#pragma once

#include <QString>

namespace videoeye {

class Widget {
public:
    QString label() const;

private:
    int w = 0;
};

}  // namespace videoeye
"""

FFMPEG_H = """\
#pragma once

#include <libavformat/avformat.h>

namespace videoeye {

int Probe(AVFormatContext* fmt);

}  // namespace videoeye
"""

# Alpha 的公共头用引号形式引用 Beta 的头（从仓库根可解析）
ALPHA_REFERS_BETA_H = """\
#pragma once

#include "beta/Worker.h"

namespace videoeye {

int Run(Worker* w);

}  // namespace videoeye
"""

BETA_QT_H = """\
#pragma once

#include <QObject>

namespace videoeye {

class Worker : public QObject {
    Q_OBJECT
};

}  // namespace videoeye
"""


def load_audit_module():
    spec = importlib.util.spec_from_file_location("audit_link_visibility", AUDIT_PATH)
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
        print("  [FAIL] 真实仓库扫描退出码 %d, 期望 0" % code)
        for line in out.splitlines():
            print("  | %s" % line)
        return False
    print("  [ OK ] 真实仓库扫描干净 (exit=0)")
    return True


def main():
    print("audit_link_visibility.py 退出码自测")
    print("-" * 60)

    cases = [
        ("干净仓库 -> 必须放行",
         {"CMakeLists.txt": CLEAN_CMAKE, "alpha/Widget.h": PLAIN_H}, 0),
        ("公共头要 Qt / Qt6::Core 挂 PRIVATE -> 必须红",
         {"CMakeLists.txt": QT_PRIVATE_CMAKE, "alpha/Widget.h": QT_H}, 1),
        ("公共头要 Qt / Qt6::Core 挂 PUBLIC -> 必须放行",
         {"CMakeLists.txt": QT_PUBLIC_CMAKE, "alpha/Widget.h": QT_H}, 0),
        ("公共头要 FFmpeg / FFmpeg::* 挂 PRIVATE -> 必须红",
         {"CMakeLists.txt": FFMPEG_PRIVATE_CMAKE, "alpha/Widget.h": FFMPEG_H}, 1),
        ("P2-8 形态：引用了 PRIVATE 模块的头, Qt 没透出 -> 必须红",
         {"CMakeLists.txt": TRANSITIVE_CMAKE,
          "alpha/Widget.h": ALPHA_REFERS_BETA_H,
          "beta/Worker.h": BETA_QT_H}, 1),
        ("同形态但 PUBLIC 链到底 -> 必须放行",
         {"CMakeLists.txt": TRANSITIVE_OK_CMAKE,
          "alpha/Widget.h": ALPHA_REFERS_BETA_H,
          "beta/Worker.h": BETA_QT_H}, 0),
        ("模块目录解析不出来 -> 按失败处理(不允许静默漏查)",
         {"CMakeLists.txt": NO_GLOB_CMAKE, "alpha/Widget.h": PLAIN_H}, 1),
    ]

    ok = True
    for name, files, expect in cases:
        code, out = run_in(files)
        hit = "OK"
        if code != expect:
            hit = "FAIL"
            ok = False
            for line in out.splitlines():
                print("  | %s" % line)
        print("  [%s] %-44s exit=%d 期望=%d" % (hit, name, code, expect))

    # 第 5 条还要求"只命中引用方"：Beta 自己依赖透得出去，不该被算违规。
    code, out = run_in({"CMakeLists.txt": TRANSITIVE_CMAKE,
                        "alpha/Widget.h": ALPHA_REFERS_BETA_H,
                        "beta/Worker.h": BETA_QT_H})
    if code == 1 and "VideoEyeBeta" in out:
        print("  [FAIL] 第 5 条应只命中引用方 VideoEyeAlpha, 输出里却出现了 VideoEyeBeta")
        ok = False
    else:
        print("  [ OK ] 第 5 条只命中引用方（被引用模块自身的 PUBLIC 边是对的）")

    print("-" * 60)
    ok &= check_real_repo(load_audit_module())

    if ok:
        print("\n全部通过：审计脚本的退出码语义（0=放行 / 1=拦截）符合预期")
        return 0
    print("\n有断言失败：退出码语义可能又被改坏了（回归：打印违规却 return 0）")
    return 1


if __name__ == "__main__":
    sys.exit(main())