r"""统计仓库里测试的真实规模，给 README / 文档当单一事实源。

为什么要有这个: README 曾经长期写着「18 个可执行文件 + 17 组 ctest」, 实际是 45 / 45。
靠人肉更新文档迟早还会飘 —— 把计数交给脚本, 文档里引用命令, 升级一次跑一次即可。

用法:
    python scripts/summarize_tests.py            # 打印可读摘要
    python scripts/summarize_tests.py --json     # 输出 JSON (CI 里做断言用)
    python scripts/summarize_tests.py --expect 45  # 数量对不上就返回非 0

CI 可以拿 --expect 卡住「加测试但忘了改文档」:
    python scripts/summarize_tests.py --expect "$(grep -o '[0-9]\+' README.md | head -1)"
"""
import argparse
import json
import os
import re
import sys

ROOT = os.environ.get("VIDEOEYE_ROOT") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), ".."
)

TESTS_DIR = os.path.join(ROOT, "tests")
UNIT_DIR = os.path.join(TESTS_DIR, "unit")
CMAKE = os.path.join(TESTS_DIR, "CMakeLists.txt")

# videoeye_add_test(xxx ...) 的 xxx 可能和第一个参数写在同一行，也可能换行写在下一行，
# 所以正则要允许 \s（含换行）。别用 grep -c "videoeye_add_test(" 数调用数 ——
# 那会把多行调用和函数定义行一起算进去，得出 46/44 这种对不上的数。
RE_TEST_CALL = re.compile(r"videoeye_add_test\(\s*(\w+)")
RE_RAW_EXE = re.compile(r"^\s*add_executable\(\s*(\w+)\s", re.M)
RE_CTEST = re.compile(r"add_test\(\s*NAME\s+(\w+)\s+COMMAND\s+(\w+)\s*\)", re.S)


def count_source_cases():
    """gtest 用例数（含 TEST / TEST_F / TEST_P）。"""
    cases = 0
    per_file = {}
    if not os.path.isdir(UNIT_DIR):
        return cases, per_file
    for name in sorted(os.listdir(UNIT_DIR)):
        if not name.endswith(".cpp"):
            continue
        path = os.path.join(UNIT_DIR, name)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        n = len(re.findall(r"^\s*TEST(?:_F|_P)?\(", text, re.M))
        per_file[name] = n
        cases += n
    return cases, per_file


def summarize():
    with open(CMAKE, "r", encoding="utf-8", errors="replace") as fh:
        cmake = fh.read()

    via_helper = RE_TEST_CALL.findall(cmake)
    raw = RE_RAW_EXE.findall(cmake)
    executables = sorted(set(via_helper) | set(raw))

    pairs = RE_CTEST.findall(cmake)
    ctest_groups = sorted({name for name, exe in pairs})

    # add_test 指向的 exe 必须真实存在, 否则这条用例永远跑不起来（上次就差一个）
    missing = sorted({exe for _, exe in pairs} - set(executables))
    orphan = sorted(set(executables) - {exe for _, exe in pairs})

    cases, per_file = count_source_cases()

    return {
        "test_executables": len(executables),
        "ctest_groups": len(ctest_groups),
        "gtest_cases": cases,
        "executables_without_ctest": orphan,
        "ctest_without_executable": missing,
        "per_file_cases": per_file,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--expect", type=int, default=None,
                    help="期望的测试可执行文件数，不符返回非 0")
    args = ap.parse_args()

    data = summarize()

    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    else:
        print("VideoEye 测试规模")
        print("  测试可执行文件 : %d" % data["test_executables"])
        print("  ctest 用例组   : %d" % data["ctest_groups"])
        print("  gtest 用例     : %d" % data["gtest_cases"])
        if data["ctest_without_executable"]:
            print("  [警告] 注册了 ctest 却没有对应可执行文件: %s"
                  % ", ".join(data["ctest_without_executable"]))
        if data["executables_without_ctest"]:
            print("  [警告] 编译了可执行文件却没注册 ctest: %s"
                  % ", ".join(data["executables_without_ctest"]))

    if args.expect is not None and data["test_executables"] != args.expect:
        print("实际 %d 个测试可执行文件，与期望的 %d 不一致"
              % (data["test_executables"], args.expect), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
