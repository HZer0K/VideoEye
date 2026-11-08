#!/usr/bin/env python3
"""corpus_defect_gate.py 的判别力自测。

每个用例只改一处事实，断言门禁精确报出那一条 —— 「做了才有用、坏了看不出来」
的门禁等于没有门禁。覆盖守护、守恒、类别多样性、分组语义、白名单全部单独验。
"""

from __future__ import annotations

import contextlib
import csv
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import corpus_defect_gate as gate  # noqa: E402

# 与真实 INDEX.tsv 的分组数量一致（175 条异常标注）。
GROUPS = {
    "valid-reference": 50,
    "decode-artifact": 39,
    "corrupt-structure": 30,
    "malformed": 21,
    "unclassified": 18,
    "metadata": 8,
    "fuzz": 4,
    "truncated": 2,
    "extension-mismatch": 2,
    "zero-byte": 1,
}

RESULT_COLUMNS = ["path", "struct_valid", "struct_failure_kind",
                  "struct_error", "struct_summary", "idx_parse"]
INDEX_COLUMNS = ["path", "category", "defect_heuristic", "parse"]


def write_tsv(path: Path, columns: list[str], rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def build(root: Path, *, mutate_results=None, mutate_index=None,
          whitelist: dict | None = None,
          result_columns: list[str] | None = None) -> tuple[Path, Path, Path | None]:
    index_rows: list[dict] = []
    result_rows: list[dict] = []

    def add(rel: str, defect: str, valid: bool, kind: str,
            error: str, summary: str, parse: str = "ok") -> None:
        index_rows.append({"path": rel.replace("/", "\\"), "category":
                           "abnormal" if defect != "-" else "normal",
                           "defect_heuristic": defect, "parse": parse})
        # 模拟 validator：结果表里的 path 是绝对路径 + 反斜杠混合。
        result_rows.append({
            "path": str(root / rel).replace("/", "\\"),
            "struct_valid": "1" if valid else "0",
            "struct_failure_kind": kind,
            "struct_error": error,
            "struct_summary": summary,
            "idx_parse": parse,
        })

    for i in range(120):
        add(f"normal/file{i}.mp4", "-", True, "none", "", "结构正常")

    for group, count in GROUPS.items():
        for i in range(count):
            rel = f"abnormal/{group}/sample{i}.bin"
            if group == "zero-byte":
                add(rel, group, False, "empty_file", "文件为空", "", "fail")
            elif group == "metadata":
                add(rel, group, False, "open_failed", "Invalid data found", "", "fail")
            elif group == "corrupt-structure" and i < 10:
                # 给多样性贡献第三种 parse_failed
                add(rel, group, False, "parse_failed", "box 越界", "", "fail")
            elif group in gate.DEFECT_GROUPS:
                add(rel, group, True, "none", "", "结构树已扫描", "ok")
            else:
                add(rel, group, True, "none", "", "结构正常", "ok")

    if mutate_index:
        mutate_index(index_rows)
    if mutate_results:
        mutate_results(result_rows)

    index_path = root / "INDEX.tsv"
    results_path = root / "results.tsv"
    write_tsv(index_path, INDEX_COLUMNS, index_rows)
    write_tsv(results_path, result_columns or RESULT_COLUMNS, result_rows)

    whitelist_path = None
    if whitelist is not None:
        whitelist_path = root / "whitelist.json"
        whitelist_path.write_text(json.dumps({"exceptions": whitelist}), encoding="utf-8")
    return index_path, results_path, whitelist_path


def run(root: Path, **kwargs) -> tuple[int, str]:
    index_path, results_path, whitelist_path = build(root, **kwargs)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = gate.check(results_path, index_path, str(root), whitelist_path)
    return rc, buf.getvalue()


class CorpusDefectGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def assert_red(self, output: str, rc: int, token: str) -> None:
        self.assertEqual(rc, 1, f"应当判红，实际通过。输出:\n{output}")
        self.assertIn(token, output, f"没有精确报出 {token!r}。输出:\n{output}")

    def test_baseline_green(self) -> None:
        rc, out = run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("通过", out)
        self.assertIn("异常标注 175 行", out)

    def test_missing_kind_column(self) -> None:
        def mut(rows):
            for r in rows:
                del r["struct_failure_kind"]
        rc, out = run(self.root, mutate_results=mut,
                      result_columns=[c for c in RESULT_COLUMNS if c != "struct_failure_kind"])
        self.assert_red(out, rc, "缺 struct_failure_kind 列")

    def test_empty_results(self) -> None:
        def mut(rows):
            rows.clear()
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "结果表为空")

    def test_blank_kind(self) -> None:
        def mut(rows):
            rows[0]["struct_failure_kind"] = ""
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "struct_failure_kind 为空")

    def test_unknown_kind(self) -> None:
        def mut(rows):
            rows[0]["struct_failure_kind"] = "made_up"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "未知失败类别")

    def test_valid_with_failure_kind(self) -> None:
        def mut(rows):
            rows[0]["struct_failure_kind"] = "empty_file"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "结构有效却带失败类别")

    def test_invalid_without_kind(self) -> None:
        def mut(rows):
            for r in rows:
                if r["struct_failure_kind"] == "empty_file":
                    r["struct_failure_kind"] = "none"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "结构失败却没有失败类别")

    def test_invalid_without_error(self) -> None:
        def mut(rows):
            for r in rows:
                if r["struct_failure_kind"] == "empty_file":
                    r["struct_error"] = ""
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "结构失败却没有错误说明")

    def test_all_failures_collapsed_to_one_kind(self) -> None:
        def mut(rows):
            for r in rows:
                if r["struct_valid"] == "0":
                    r["struct_failure_kind"] = "open_failed"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "失败类别只有 1 种")

    def test_zero_byte_not_recognized(self) -> None:
        def mut(rows):
            for r in rows:
                if r["struct_failure_kind"] == "empty_file":
                    r["struct_failure_kind"] = "open_failed"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "零字节文件被归成")

    def test_no_empty_file_kind_at_all(self) -> None:
        def mut(rows):
            for r in rows:
                if r["struct_failure_kind"] == "empty_file":
                    r["struct_failure_kind"] = "parse_failed"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "零字节样本没被识别出来")

    def test_metadata_misclassified_as_file_error(self) -> None:
        def mut(rows):
            for r in rows:
                if r["struct_error"] == "Invalid data found":
                    r["struct_failure_kind"] = "empty_file"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "非媒体文件被归成文件级错误")

    def test_healthy_sample_reported_failed(self) -> None:
        def mut(rows):
            for r in rows:
                if "valid-reference" in r["path"]:
                    r["struct_valid"] = "0"
                    r["struct_failure_kind"] = "parse_failed"
                    r["struct_error"] = "误判"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "健康参考样本被结构层判为失败")

    def test_success_without_any_evidence(self) -> None:
        def mut(rows):
            for r in rows:
                if "corrupt-structure" in r["path"] and r["struct_valid"] == "1":
                    r["struct_summary"] = ""
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(out, rc, "无事实的成功结论")

    def test_missing_required_group(self) -> None:
        def mut(rows):
            rows[:] = [r for r in rows if r["defect_heuristic"] != "metadata"]
        rc, out = run(self.root, mutate_index=mut)
        self.assert_red(out, rc, "缺失必需缺陷分组")

    def test_too_few_abnormal_rows(self) -> None:
        def mut(rows):
            rows[:] = [r for r in rows if r["defect_heuristic"] == "-"][:20]
        rc, out = run(self.root, mutate_index=mut)
        self.assert_red(out, rc, "门禁空转")

    def test_stale_whitelist(self) -> None:
        rc, out = run(self.root, whitelist={"normal/file0.mp4": "不该有例外"})
        self.assert_red(out, rc, "白名单失效")

    def test_whitelist_hit_suppresses_group_failure(self) -> None:
        # 让一条「成功但缺取证事实」的损坏样本违反分组判据，再用精确路径豁免；
        # 守恒与多样性不受影响，白名单命中后整体应绿。
        def mut2(rows):
            for r in rows:
                if "corrupt-structure" in r["path"] and "sample10.bin" in r["path"] \
                        and r["struct_valid"] == "1":
                    r["struct_summary"] = ""
        rc, out = run(self.root, mutate_results=mut2,
                      whitelist={"abnormal/corrupt-structure/sample10.bin": "已知无 summary"})
        self.assertEqual(rc, 0, out)
        self.assertIn("白名单命中 1/1", out)

    def test_whitelist_cannot_override_conservation(self) -> None:
        def mut(rows):
            rows[0]["struct_failure_kind"] = "empty_file"  # valid=1 却带失败类别
        rc, out = run(self.root, mutate_results=mut,
                      whitelist={"normal/file0.mp4": "想豁免守恒"})
        self.assert_red(out, rc, "结构有效却带失败类别")

    def test_rel_key_normalizes_absolute_windows_path(self) -> None:
        self.assertEqual(gate.rel_key("D:/Coding/Test/media\\abnormal\\a.bin",
                                     "D:/Coding/Test/media"), "abnormal/a.bin")
        self.assertEqual(gate.rel_key("abnormal\\a.bin", "D:/Coding/Test/media"),
                         "abnormal/a.bin")


if __name__ == "__main__":
    unittest.main(verbosity=2)
