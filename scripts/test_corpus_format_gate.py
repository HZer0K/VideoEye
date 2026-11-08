#!/usr/bin/env python3
"""corpus_format_gate.py 的判别力自测：每条判据都要能被精确打破。

用合成的 INDEX / results 表驱动门禁，不碰真实语料；每个用例只改一个事实，
断言报出的那一条正好是它该报的 —— 防止「门禁恒绿」或「一条判据把所有事都拦了」。
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
import corpus_format_gate as gate  # noqa: E402

INDEX_COLS = ["path", "category", "source", "container", "codec", "duration_s",
              "resolution", "size_bytes", "parse", "defect_heuristic"]
RESULT_COLS = ["path", "category", "idx_container", "idx_parse", "detected_format"]

ROOT = "D:/Coding/Test/media"

# 硬组：真值容器 -> (条数, 产品探测结果)
HARD = [
    ("mp4", 400, "MP4"), ("matroska", 200, "MKV"), ("ts", 200, "MPEG-TS"),
    ("avi", 150, "AVI"), ("asf", 100, "ASF"), ("ogg", 100, "OGG"),
    ("mxf", 100, "MXF"), ("mpeg", 100, "MPEG-PS"), ("flv", 80, "FLV"),
    ("webm", 60, "WebM"), ("mov", 50, "MOV"), ("mp4-segment", 30, "MP4"),
    ("hls-playlist", 20, "HLS"), ("dash-manifest", 20, "DASH"),
]
# 软组：产品没有专用解析器，Unknown / Raw ES / Generic 都合法
SOFT = [
    ("image", 700, "Unknown"), ("h264", 250, "Raw ES"), ("hevc", 200, "Raw ES"),
    ("raw-audio", 100, "Raw ES"), ("wav", 70, "Unknown"), ("mp3", 50, "Unknown"),
    ("vvc", 50, "Raw ES"), ("ivf", 40, "Unknown"), ("webvtt", 30, "Unknown"),
    ("flac", 30, "Unknown"), ("ass", 30, "Unknown"), ("srt", 20, "Unknown"),
    ("y4m", 20, "Unknown"), ("dtshd", 20, "Unknown"), ("rm", 20, "Generic"),
]


def write_tsv(path: Path, columns: list[str], rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def build(root: Path, *, mutate_results=None, mutate_index=None,
          drop_cols=(), whitelist: dict | None = None,
          drop_rows: int = 0) -> tuple[Path, Path, Path | None]:
    index_rows: list[dict] = []
    result_rows: list[dict] = []
    for container, count, detected in HARD + SOFT:
        for i in range(count):
            rel = f"normal/{container}/sample{i}.bin"
            path = ROOT + "/" + rel
            index_rows.append({"path": rel, "category": "normal", "source": "synthetic",
                               "container": container, "codec": "-", "duration_s": "-",
                               "resolution": "-", "size_bytes": "1024", "parse": "ok",
                               "defect_heuristic": "-"})
            result_rows.append({"path": path, "category": "normal",
                                "idx_container": container, "idx_parse": "ok",
                                "detected_format": detected})
    if mutate_index:
        mutate_index(index_rows)
    if mutate_results:
        mutate_results(result_rows)
    if drop_rows:
        result_rows = result_rows[:-drop_rows]

    index_path = root / "INDEX.tsv"
    results_path = root / "results.tsv"
    cols = [c for c in RESULT_COLS if c not in drop_cols]
    write_tsv(index_path, INDEX_COLS, index_rows)
    write_tsv(results_path, cols, result_rows)

    wl_path = None
    if whitelist is not None:
        wl_path = root / "whitelist.json"
        wl_path.write_text(json.dumps({"exceptions": whitelist}), encoding="utf-8")
    return index_path, results_path, wl_path


def run(root: Path, **kwargs) -> tuple[int, str]:
    index_path, results_path, wl_path = build(root, **kwargs)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = gate.check(results_path, index_path, ROOT, wl_path)
    return rc, buf.getvalue()


class FormatGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def assert_red(self, rc: int, out: str, needle: str) -> None:
        self.assertNotEqual(rc, 0, f"期望判红却通过: {needle}\n{out}")
        self.assertIn(needle, out)

    def test_baseline_green(self) -> None:
        rc, out = run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("[format] 通过", out)
        self.assertIn("硬组 1610 / 软组 1630", out)

    def test_empty_results_red(self) -> None:
        p = self.root / "empty.tsv"
        p.write_text("", encoding="utf-8")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = gate.check(p, self.root / "INDEX.tsv", ROOT, None)
        self.assert_red(rc, buf.getvalue(), "结果表为空")

    def test_missing_column_red(self) -> None:
        rc, out = run(self.root, drop_cols=("detected_format",))
        self.assert_red(rc, out, "结果表缺列 ['detected_format']")

    def test_too_few_rows_red(self) -> None:
        rc, out = run(self.root, drop_rows=500)
        self.assert_red(rc, out, "门禁空转")

    def test_hard_explicit_mismatch_red(self) -> None:
        def mut(rows):
            rows[0]["detected_format"] = "MPEG-TS"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(rc, out, "[硬组] mp4 被探测成 'MPEG-TS'")
        self.assertIn("normal/mp4/sample0.bin", out)

    def test_hard_unknown_budget_red(self) -> None:
        def mut(rows):
            for r in rows[:gate.MAX_HARD_UNKNOWN + 1]:
                r["detected_format"] = "Unknown"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(rc, out, "硬组 Unknown 有 21 条")

    def test_hard_unknown_within_budget_green(self) -> None:
        def mut(rows):
            for r in rows[:5]:
                r["detected_format"] = "Unknown"
        rc, out = run(self.root, mutate_results=mut)
        self.assertEqual(rc, 0, out)

    def test_soft_cross_family_red(self) -> None:
        def mut(rows):
            for r in rows:
                if r["idx_container"] == "image":
                    r["detected_format"] = "MPEG-TS"
                    break
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(rc, out, "[软组] image 跨大类误判成 'MPEG-TS'(ts)")

    def test_soft_heif_isobmff_green(self) -> None:
        def mut(rows):
            changed = 0
            for r in rows:
                if r["idx_container"] == "image" and changed < 50:
                    r["detected_format"] = "MP4"  # HEIF/AVIF 是 ISOBMFF 盒结构
                    changed += 1
        rc, out = run(self.root, mutate_results=mut)
        self.assertEqual(rc, 0, out)

    def test_soft_unknown_detected_name_red(self) -> None:
        def mut(rows):
            for r in rows:
                if r["idx_container"] == "image":
                    r["detected_format"] = "MadeUpFormat"
                    break
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(rc, out, "落进未知探测结果 'MadeUpFormat'")

    def test_default_soft_allowed_red(self) -> None:
        # wav 没写进 SOFT_ALLOWED，走默认集合；被判成 AVI 必须红
        def mut(rows):
            for r in rows:
                if r["idx_container"] == "wav":
                    r["detected_format"] = "AVI"
                    break
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(rc, out, "[软组] wav 跨大类误判成 'AVI'(avi)")

    def test_truth_column_mismatch_red(self) -> None:
        def mut(rows):
            rows[0]["idx_container"] = "avi"
        rc, out = run(self.root, mutate_results=mut)
        self.assert_red(rc, out, "idx_container 与 INDEX.container 不一致")

    def test_missing_required_container_red(self) -> None:
        def mut_index(rows):
            for r in rows:
                if r["container"] == "mxf":
                    r["parse"] = "fail"
        rc, out = run(self.root, mutate_index=mut_index)
        self.assert_red(rc, out, "缺失必需真值容器: ['mxf']")

    def test_too_few_containers_red(self) -> None:
        def mut_index(rows):
            for r in rows:
                r["container"] = "mp4"
        rc, out = run(self.root, mutate_index=mut_index)
        self.assert_red(rc, out, "真值容器只有 1 种")

    def test_parse_fail_rows_skipped(self) -> None:
        # INDEX 标 parse=fail 的样本不参与真值比对：把唯一一条错判标掉就该绿
        def mut(rows):
            rows[0]["detected_format"] = "MPEG-TS"
        def mut_index(rows):
            rows[0]["parse"] = "fail"
        rc, out = run(self.root, mutate_results=mut, mutate_index=mut_index)
        self.assertEqual(rc, 0, out)

    def test_hard_rows_guard_red(self) -> None:
        def mut_index(rows):
            for r in rows:
                if r["container"] in ("mp4", "matroska"):
                    r["parse"] = "fail"
        rc, out = run(self.root, mutate_index=mut_index)
        self.assert_red(rc, out, "硬组样本只有 1010")

    def test_soft_rows_guard_red(self) -> None:
        def mut_index(rows):
            for r in rows:
                if r["container"] == "image":
                    r["parse"] = "fail"
        rc, out = run(self.root, mutate_index=mut_index)
        self.assert_red(rc, out, "软组样本只有 930")

    def test_empty_index_red(self) -> None:
        index_path, results_path, _ = build(self.root)
        write_tsv(index_path, INDEX_COLS, [])
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = gate.check(results_path, index_path, ROOT, None)
        self.assert_red(rc, buf.getvalue(), "INDEX 为空")

    def test_whitelist_suppresses_real_mismatch(self) -> None:
        key = "normal/mp4/sample0.bin"
        def mut(rows):
            rows[0]["detected_format"] = "MPEG-TS"
        rc, out = run(self.root, mutate_results=mut,
                      whitelist={key: "私有封装，扩展名与真值不一致"})
        self.assertEqual(rc, 0, out)
        self.assertIn("白名单命中 1/1", out)

    def test_stale_whitelist_red(self) -> None:
        rc, out = run(self.root, whitelist={"normal/mp4/sample0.bin": "这条本来就没违反判据"})
        self.assert_red(rc, out, "白名单失效")

    def test_whitelist_cannot_exempt_coverage(self) -> None:
        rc, out = run(self.root, drop_cols=("detected_format",),
                      whitelist={"normal/mp4/sample0.bin": "想用白名单盖住缺列"})
        self.assert_red(rc, out, "结果表缺列")

    def test_whitelist_cannot_exempt_truth_mismatch(self) -> None:
        def mut(rows):
            rows[0]["idx_container"] = "avi"
        rc, out = run(self.root, mutate_results=mut,
                      whitelist={"normal/mp4/sample0.bin": "想用白名单盖住真值列不一致"})
        self.assert_red(rc, out, "idx_container 与 INDEX.container 不一致")


if __name__ == "__main__":
    unittest.main(verbosity=2)
