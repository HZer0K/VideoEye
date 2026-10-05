#!/usr/bin/env python3
"""audit_qt_domain_border.py 的退出码自测。

背景（这段历史值得记下来）：
审计脚本曾经「打印了 [命中]，却 return 0」——早退分支写成
`if not arg_hits and not arg_probable: return 0`，于是只要没有 arg 命中，
纯「域字段被 Qt 类型污染」的违规会让 CI 徽章一路绿着合并进主干。
这类 bug 的特征是**脚本本身看着是对的**（人肉读输出确认命中都在），
所以只有退出码能兜住，光靠 review 盯输出拦不住。

于是这里用临时目录造四种场景，直接断言 main() 的返回码：
    1. 干净仓库（QString 已 .toStdString()）           -> 0
    2. QString 直接赋给域字段  result.error_message    -> 1 且输出含 [命中]
    3. QString::arg(域字段成员)（没有 std::string 重载） -> 1 且输出含 [命中]
    4. QString::arg(同名局部变量)（静态判定不了类型）   -> 0，仍是 [疑似] warning

外加一项：拿真实仓库跑一遍，断言当前确实干净（退出 0）。

用法:
    python scripts/test_audit_qt_domain_border.py
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
AUDIT_PATH = os.path.join(HERE, "audit_qt_domain_border.py")
REPO_ROOT = os.path.dirname(HERE)

# 假 domain 模型：给审计提供「域字段名」样本。
# error_message 这个名字很关键——它是本项目里被污染最多的字段，且同时是
# 典型的局部变量名（用来触发 arg_probable 分支）。
FAKE_DOMAIN_H = """#pragma once
#include <string>

namespace videoeye::model {

struct FakeRecord {
    std::string error_message;
    std::string name;
};

}  // namespace videoeye::model
"""

CLEAN_CPP = """void F(const FakeRecord& out, std::string& sink) {
    sink = out.error_message;
    sink = QString("bad").toStdString();
}
"""

# 真违规: QString 直接落进 std::string 字段。
REAL_HIT_CPP = """void F(FakeRecord& out) {
    out.error_message = QString("bad");
}
"""

# 高置信 arg 命中: QString::arg() 没有 std::string 重载, 这行编译不过。
ARG_HIT_CPP = """void F(const FakeRecord& info, QString& label) {
    label = QString("v%1").arg(info.error_message);
}
"""

# 疑似: arg() 吃的是同名局部变量, 静态审计拿不到类型, 只能降级为 warning。
ARG_PROBABLE_CPP = """void F(QString& label) {
    QString error_message = QString("x");
    label = QString("v%1").arg(error_message);
}
"""


def load_audit_module():
    spec = importlib.util.spec_from_file_location("audit_qt_domain_border", AUDIT_PATH)
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
    """把审计指向临时目录, 返回 (退出码, 输出文本)。"""
    write_tree(root, files)
    mod.ROOT = root
    mod.DOMAIN_DIR = os.path.join(root, "core", "domain", "model")
    mod.SCAN_DIRS = ["core/analysis"]
    # 白名单置空: 用例里的路径本来就不该命中任何白名单,
    # 留着模块自带的白名单只会让「该不该红」取决于白名单内容, 测不到脚本本身。
    mod.ALLOWED_FILES = {}
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = mod.main()
    return code, buf.getvalue()


def case(name, files, expect_code, expect_marker=None):
    mod = load_audit_module()
    tmp = tempfile.mkdtemp(prefix="audit_selftest_")
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
    """真实仓库现状：应当干净（退出 0）。

    相当于把 CI 里那一步再跑一遍。放这里是防「自测假绿」——
    前几条用例全绿也可能是因为自测根本没扫到文件（路径拼错、ROOT 指歪），
    这一条要求真实目录里确实一个确定命中都没有，才算整条链路通。
    """
    mod.ROOT = REPO_ROOT
    mod.DOMAIN_DIR = os.path.join(REPO_ROOT, "core", "domain", "model")
    mod.SCAN_DIRS = ["core/analysis", "core/player", "core/exporter", "core/media",
                     "core/ffmpeg", "infrastructure"]
    mod.ALLOWED_FILES = set(mod.ALLOWED_FILES)  # 保留白名单，与 CI 口径一致
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = mod.main()
    out = buf.getvalue()
    if code != 0:
        print("  [FAIL] 真实仓库扫描退出码 %d, 期望 0（当前应当无确定命中）" % code)
        for line in out.splitlines():
            print("  | %s" % line)
        return False
    print("  [ OK ] 真实仓库扫描干净 (exit=0)")
    return True


def main():
    print("audit_qt_domain_border.py 退出码自测")
    print("-" * 60)
    domain = {"core/domain/model/FakeModel.h": FAKE_DOMAIN_H}

    ok = True
    ok &= case("干净仓库（QString 已 .toStdString()）",
               dict(domain, **{"core/analysis/FakeClean.cpp": CLEAN_CPP}), 0)
    ok &= case("QString 直接赋值给域字段 -> 必须红",
               dict(domain, **{"core/analysis/FakeViolation.cpp": REAL_HIT_CPP}),
               1, "[命中]")
    ok &= case("QString::arg(域字段成员) -> 必须红",
               dict(domain, **{"core/analysis/FakeArg.cpp": ARG_HIT_CPP}),
               1, "[命中]")
    ok &= case("QString::arg(同名局部变量) -> 只 warning",
               dict(domain, **{"core/analysis/FakeLikely.cpp": ARG_PROBABLE_CPP}),
               0, "[疑似]")
    print("-" * 60)
    ok &= check_real_repo(load_audit_module())

    if ok:
        print("\n全部通过：审计脚本的退出码语义（0=放行 / 1=拦截）符合预期")
        return 0
    print("\n有断言失败：退出码语义可能又被改坏了（回归：打印 [命中] 却 return 0）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
