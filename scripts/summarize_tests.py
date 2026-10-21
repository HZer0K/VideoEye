r"""统计仓库里测试的真实规模，给 README / 文档当单一事实源。

为什么要有这个: README 曾经长期写着「18 个可执行文件 + 17 组 ctest」, 实际是 45 / 45。
靠人肉更新文档迟早还会飘 —— 把计数交给脚本, 文档里引用命令, 升级一次跑一次即可。

用法:
    python scripts/summarize_tests.py            # 打印可读摘要
    python scripts/summarize_tests.py --json     # 输出 JSON (CI 里做断言用)
    python scripts/summarize_tests.py --update-readme
    python scripts/summarize_tests.py --check-readme

无论用哪个模式, 「编译了却没注册 ctest」或「注册了却没有可执行文件」都会让退出码为 1 ——
这两类不一致以前只打警告, 结果是表格漂了也没人发现。
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

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
RE_SCRIPT_CTEST = re.compile(
    r"add_test\(\s*NAME\s+(\w+)\s+COMMAND\s+\$\{VIDEOEYE_PYTHON_EXE\}[^)]*\)", re.S
)
SUMMARY_BEGIN = "<!-- BEGIN GENERATED TEST SUMMARY -->"
SUMMARY_END = "<!-- END GENERATED TEST SUMMARY -->"


def count_source_cases():
    """gtest 用例数（含 TEST / TEST_F / TEST_P）。"""
    cases = 0
    per_file = {}
    if not os.path.isdir(TESTS_DIR):
        return cases, per_file
    for path in sorted(Path(TESTS_DIR).rglob("*.cpp")):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        n = len(re.findall(r"^\s*TEST(?:_F|_P)?\(", text, re.M))
        per_file[path.relative_to(TESTS_DIR).as_posix()] = n
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
    script_groups = set(RE_SCRIPT_CTEST.findall(cmake))

    # add_test 指向的 exe 必须真实存在, 否则这条用例永远跑不起来（上次就差一个）
    missing = sorted({exe for _, exe in pairs} - set(executables))
    orphan = sorted(set(executables) - {exe for _, exe in pairs})

    cases, per_file = count_source_cases()

    return {
        "test_executables": len(executables),
        "ctest_groups": len(ctest_groups),
        "gtest_cases": cases,
        "script_ctest_groups": len(script_groups),
        "total_ctest_groups": len(ctest_groups) + len(script_groups),
        "executables_without_ctest": orphan,
        "ctest_without_executable": missing,
        "per_file_cases": per_file,
    }


def render_readme_summary(data):
    return "\n".join([
        SUMMARY_BEGIN,
        "| 指标 | 数量 |",
        "|---|---|",
        "| 测试可执行文件 | %d |" % data["test_executables"],
        "| ctest 用例组（gtest 可执行文件） | %d |" % data["ctest_groups"],
        "| python 脚本用例组（非 gtest） | %d |" % data["script_ctest_groups"],
        "| gtest 用例声明（含 `TEST` / `TEST_F` / `TEST_P`） | %d |" % data["gtest_cases"],
        "",
        "启用测试且找到 Python 时，静态注册的 ctest 用例组共 **%d = %d + %d**。"
        % (data["total_ctest_groups"], data["ctest_groups"], data["script_ctest_groups"]),
        "这里统计源码中的测试声明，不代表本次已运行通过的数量；参数化测试运行时可能展开成多个用例。",
        SUMMARY_END,
    ])


def replace_readme_summary(text, data):
    if text.count(SUMMARY_BEGIN) != 1 or text.count(SUMMARY_END) != 1:
        raise ValueError("README 必须包含唯一的测试统计生成标记")
    start = text.index(SUMMARY_BEGIN)
    end = text.index(SUMMARY_END, start) + len(SUMMARY_END)
    return text[:start] + render_readme_summary(data) + text[end:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--expect", type=int, default=None,
                    help="期望的测试可执行文件数，不符返回非 0")
    readme_mode = ap.add_mutually_exclusive_group()
    readme_mode.add_argument("--update-readme", action="store_true", help="更新 README 测试统计块")
    readme_mode.add_argument("--check-readme", action="store_true", help="验证 README 测试统计块")
    args = ap.parse_args()

    data = summarize()

    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    else:
        print("VideoEye 测试规模")
        print("  测试可执行文件 : %d" % data["test_executables"])
        print("  ctest 用例组   : %d" % data["ctest_groups"])
        print("  gtest 用例     : %d" % data["gtest_cases"])
        print("  python 用例组  : %d" % data["script_ctest_groups"])
        if data["ctest_without_executable"]:
            print("  [警告] 注册了 ctest 却没有对应可执行文件: %s"
                  % ", ".join(data["ctest_without_executable"]))
        if data["executables_without_ctest"]:
            print("  [警告] 编译了可执行文件却没注册 ctest: %s"
                  % ", ".join(data["executables_without_ctest"]))

    if data["executables_without_ctest"] or data["ctest_without_executable"]:
        return 1
    if args.expect is not None and data["test_executables"] != args.expect:
        print("实际 %d 个测试可执行文件，与期望的 %d 不一致"
              % (data["test_executables"], args.expect), file=sys.stderr)
        return 1
    if args.update_readme or args.check_readme:
        readme = os.path.join(ROOT, "README.md")
        try:
            with open(readme, "r", encoding="utf-8", newline="") as source:
                text = source.read()
            updated = replace_readme_summary(text, data)
            if args.update_readme:
                with open(readme, "w", encoding="utf-8", newline="") as output:
                    output.write(updated)
            elif updated != text:
                print("README 测试统计已过时，请运行 python scripts/summarize_tests.py --update-readme",
                      file=sys.stderr)
                return 1
        except (OSError, ValueError) as error:
            print(str(error), file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
