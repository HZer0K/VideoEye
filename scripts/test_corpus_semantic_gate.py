#!/usr/bin/env python3
"""corpus_semantic_gate.py 的判别力自测。

盯的是什么 bug
--------------
这类「比对型门禁」最容易坏成"永远是绿"：结果表空了、字段全空、白名单
悄悄扩大、语料树换了还拿旧答案改卷、真值列整个消失 —— 都不会有任何编译
错误或异常，只是门禁失去判别力。所以这里不只测「一致 -> 0」，重点测
**故意破坏能精确报错**。

覆盖的破坏方式：
  1. 老字段 / 切片 2 新字段被改坏，必须报出字段名和路径；
  2. 白名单放行后变绿，但**没被命中的白名单**必须判红；
  3. 结果表缺行 / 字段全空 / golden 样本找不到 -> 覆盖守护判红；
  4. INDEX.tsv 变了 / golden 字段集合旧了 -> 冻结真值失效判红；
  5. 独立真值列缺失、truth_ok=0、真值漂移 -> 判红（切片 2 新增）；
  6. --update-golden 从 truth_* 列生成的新快照能被 --check 通过，且保留白名单；
  7. 宽高互换按已知口径成对放行，超过上限仍判红；
  8. golden 文件缺失 -> 参数错误退出码 2。

全部走子进程，断言真实进程退出码，不看函数返回值。

用法:
    python scripts/test_corpus_semantic_gate.py
退出码: 全部通过 0，任一断言失败 1。
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import corpus_semantic_gate as gate  # noqa: E402  复用字段清单与类型归一化

GATE = HERE / "corpus_semantic_gate.py"
PY = sys.executable

# 历史表头前 7 个 mi_* 列的顺序（与 gate.FIELD_NAMES 前 7 项顺序不同）。
MI_LEGACY = ["mi_duration_s", "mi_video_codec", "mi_width", "mi_height",
             "mi_audio_codec", "mi_sample_rate", "mi_channels"]
MI_EXTENDED = ["mi_" + f for f in gate.FIELD_NAMES[7:]]
TRUTH = ["truth_" + f for f in gate.FIELD_NAMES]
RESULT_HEADER = (["path", "idx_parse", "mi_ok", "detected_format"] + MI_LEGACY
                 + MI_EXTENDED + TRUTH + ["truth_ok", "truth_avformat_version"])
INDEX_HEADER = [
    "path", "category", "source", "container", "codec", "duration_s",
    "resolution", "size_bytes", "parse", "defect_heuristic",
]

N = 120
TRUTH_VERSION = "62.12.103"

failures = []


def expect(tag, cond, detail=""):
    print(("  [ok]   " if cond else "  [FAIL] ") + tag + ("" if cond else "  <- " + detail))
    if not cond:
        failures.append(tag)


def write_tsv(path, header, rows):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)


def read_tsv(path):
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        return reader.fieldnames, list(reader)


def expected_tsv(i):
    """合成 23 个字段的 TSV 字符串值（与 C++ ToColumns 的格式一致）。"""
    values = {
        "duration_s": f"{10.0 + i * 0.1:.3f}",
        "width": "1280",
        "height": "720",
        "video_codec": "h264",
        "audio_codec": "aac",
        "sample_rate": "48000",
        "channels": "2",
        "format": "mov,mp4,m4a,3gp,3g2,mj2",
        "stream_count": "2",
        "overall_bitrate_bps": "1500000",
        "video_profile": "High",
        "video_level": "41",
        "pix_fmt": "yuv420p",
        "frame_rate": "29.970",
        "frame_count": "300",
        "video_bitrate_bps": "1200000",
        "color_space": "bt709",
        "color_primaries": "bt709",
        "color_transfer": "bt709",
        "rotation": "90.0",
        "sample_fmt": "fltp",
        "bit_depth": "32",
        "audio_bitrate_bps": "128000",
    }
    assert set(values) == set(gate.FIELD_NAMES), "自测字段清单与门禁不一致"
    return values


def expected_json(i):
    return {f: gate.coerce(f, v) for f, v in expected_tsv(i).items()}


def base_result_row(root, i):
    e = expected_tsv(i)
    row = {
        "path": str(root / "normal" / f"s{i:03d}.mp4"),
        "idx_parse": "ok",
        "mi_ok": "1",
        "detected_format": "MP4",
        "truth_ok": "1",
        "truth_avformat_version": TRUTH_VERSION,
    }
    for field, value in e.items():
        row["mi_" + field] = value
        row["truth_" + field] = value
    return [row[name] for name in RESULT_HEADER]


def make_fixture():
    root = Path(tempfile.mkdtemp(prefix="semantic_gate_"))
    index = root / "INDEX.tsv"
    results = root / "results.tsv"
    golden = root / "golden.json"
    index_rows = [
        [f"normal/s{i:03d}.mp4", "normal", "synthetic", "mp4", "h264",
         f"{10.0 + i * 0.1:.3f}", "1280x720", "1000", "ok", "-"]
        for i in range(N)
    ]
    write_tsv(index, INDEX_HEADER, index_rows)
    write_tsv(results, RESULT_HEADER, [base_result_row(root, i) for i in range(N)])
    data = {
        "source_index_sha256": hashlib.sha256(index.read_bytes()).hexdigest(),
        "truth_avformat_version": TRUTH_VERSION,
        "sample_seed": 20261108,
        "fields": sorted(gate.FIELD_NAMES),
        "known_differences": [],
        "samples": [
            {"path": f"normal/s{i:03d}.mp4", "expected": expected_json(i)}
            for i in range(N)
        ],
    }
    golden.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return root, index, results, golden


def run(root, index, results, golden, extra=None, mode="--check"):
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    cmd = [PY, str(GATE), mode, "--index", str(index), "--results", str(results),
           "--root", str(root), "--golden", str(golden)] + (extra or [])
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", env=env)


class Mutate:
    """改完自动恢复：避免临时改动污染后续用例，也不用 git checkout。"""

    def __init__(self, path):
        self.path = path
        self.original = path.read_text(encoding="utf-8")

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        # ❗ Windows 上 Path.write_text() 走 open(newline=None)，会把 \n 翻成 \r\n。
        # 恢复出来的 INDEX.tsv 字节就变了，sha256 对不上，后面每个用例都会误报
        # "INDEX.tsv 已变化"。必须显式 newline="" 原样写回。
        with self.path.open("w", encoding="utf-8", newline="") as handle:
            handle.write(self.original)


def mutate_result(results, index, column, value):
    header, rows = read_tsv(results)
    rows[index][column] = str(value)
    write_tsv(results, header, [list(r.values()) for r in rows])


def mutate_golden(golden, fn):
    data = json.loads(golden.read_text(encoding="utf-8"))
    fn(data)
    with golden.open("w", encoding="utf-8", newline="") as handle:
        handle.write(json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def main():
    expect("字段清单恰好 23 项", len(gate.FIELD_NAMES) == 23,
           f"实际 {len(gate.FIELD_NAMES)}")
    root, index, results, golden = make_fixture()
    try:
        proc = run(root, index, results, golden)
        expect("基线一致 -> 0 且打印通过", proc.returncode == 0 and "语义门禁通过" in proc.stdout,
               f"rc={proc.returncode} out={proc.stdout[-400:]}")

        # ---- 单字段破坏（含切片 2 新字段）----
        cases = [
            ("mi_duration_s", "999.000", "duration_s"),
            ("mi_width", "640", "width"),
            ("mi_video_codec", "hevc", "video_codec"),
            ("mi_channels", "6", "channels"),
            ("mi_sample_rate", "44100", "sample_rate"),
            ("mi_audio_codec", "mp3", "audio_codec"),
            ("mi_pix_fmt", "yuv444p", "pix_fmt"),
            ("mi_frame_rate", "25.000", "frame_rate"),
            ("mi_overall_bitrate_bps", "99999999", "overall_bitrate_bps"),
            ("mi_video_profile", "Baseline", "video_profile"),
            ("mi_color_space", "bt470bg", "color_space"),
            ("mi_stream_count", "9", "stream_count"),
        ]
        for column, bad, field in cases:
            with Mutate(results):
                mutate_result(results, 3, column, bad)
                proc = run(root, index, results, golden)
                ok = (proc.returncode == 1 and "白名单外语义差异" in proc.stdout
                      and f"s003.mp4 {field}" in proc.stdout)
                expect(f"改坏 {column} -> 精确报 {field}", ok,
                       f"rc={proc.returncode} out={proc.stdout[-500:]}")

        # ---- 白名单 ----
        with Mutate(results), Mutate(golden):
            mutate_result(results, 3, "mi_duration_s", "999.000")
            proc = run(root, index, results, golden)
            expect("破坏后未加白名单 -> 1", proc.returncode == 1)
            mutate_golden(golden, lambda d: d["known_differences"].append({
                "path_contains": "s003.mp4", "field": "duration_s",
                "reason": "自测用的已知口径差异"}))
            proc = run(root, index, results, golden)
            expect("白名单命中 -> 0", proc.returncode == 0,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        with Mutate(golden):
            mutate_golden(golden, lambda d: d["known_differences"].append({
                "path_contains": "never-exists.mp4", "field": "duration_s",
                "reason": "不该被保留的陈旧白名单"}))
            proc = run(root, index, results, golden)
            expect("未被命中的白名单 -> 1", proc.returncode == 1 and "未被任何差异命中" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        with Mutate(golden):
            mutate_golden(golden, lambda d: d["known_differences"].append({
                "path_contains": "", "field": "duration_s", "reason": ""}))
            proc = run(root, index, results, golden)
            expect("空 path_contains / 空 reason 白名单 -> 1", proc.returncode == 1,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        # ---- 覆盖守护 ----
        with Mutate(results):
            header, rows = read_tsv(results)
            write_tsv(results, header, [list(r.values()) for r in rows[:30]])
            proc = run(root, index, results, golden)
            expect("结果表缺 90 行 -> 覆盖守护判红",
                   proc.returncode == 1 and "参与比对的行数" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        with Mutate(results):
            header, rows = read_tsv(results)
            for r in rows:
                for col in MI_LEGACY + MI_EXTENDED:
                    r[col] = ""
            write_tsv(results, header, [list(r.values()) for r in rows])
            proc = run(root, index, results, golden)
            expect("所有结论字段置空 -> 每字段守护判红",
                   proc.returncode == 1 and "字段 duration_s 只比对了 0 次" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-500:]}")

        with Mutate(results):
            header, rows = read_tsv(results)
            rows[0]["path"] = str(root / "normal" / "renamed.mp4")
            write_tsv(results, header, [list(r.values()) for r in rows])
            proc = run(root, index, results, golden)
            expect("golden 样本在结果表找不到 -> 1",
                   proc.returncode == 1 and "找不到" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        with Mutate(index):
            with index.open("a", encoding="utf-8") as handle:
                handle.write("normal/extra.mp4\tnormal\tsynthetic\tmp4\th264\t1.0\t1x1\t1\tok\t-\n")
            proc = run(root, index, results, golden)
            expect("INDEX.tsv 变化 -> 冻结真值失效判红",
                   proc.returncode == 1 and "INDEX.tsv 已变化" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        with Mutate(golden):
            mutate_golden(golden, lambda d: d["fields"].remove("pix_fmt"))
            proc = run(root, index, results, golden)
            expect("golden 字段集合落后 -> 1",
                   proc.returncode == 1 and "字段集合与当前门禁不一致" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        # ---- 切片 2：独立真值来源守护 ----
        with Mutate(results):
            mutate_result(results, 7, "truth_width", "640")
            proc = run(root, index, results, golden)
            expect("独立真值漂移 -> 1",
                   proc.returncode == 1 and "发生漂移" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        with Mutate(results):
            mutate_result(results, 7, "truth_ok", "0")
            proc = run(root, index, results, golden)
            expect("样本本轮没采到独立真值 -> 1",
                   proc.returncode == 1 and "没采到独立真值" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        with Mutate(results):
            header, rows = read_tsv(results)
            legacy = ["path", "idx_parse", "mi_ok", "detected_format"] + MI_LEGACY + MI_EXTENDED
            write_tsv(results, legacy, [[r[c] for c in legacy] for r in rows])
            proc = run(root, index, results, golden)
            expect("结果表整个没有 truth_* 列 -> 1",
                   proc.returncode == 1 and "没采到独立真值" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")
            proc = run(root, index, results, golden, ["--samples", "120"], mode="--update-golden")
            expect("无 truth_* 列时 --update-golden -> 2", proc.returncode == 2,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        # ---- --update-golden 端到端：新真值来源能自洽，且不清空白名单 ----
        with Mutate(golden):
            mutate_golden(golden, lambda d: d["known_differences"].append({
                "path_contains": "s003.mp4", "field": "duration_s",
                "reason": "保留性测试用（基线一致时这条应判红，单独验证）"}))
            proc = run(root, index, results, golden, ["--samples", "120"], mode="--update-golden")
            expect("--update-golden 从 truth_* 列生成成功 -> 0 且记录版本",
                   proc.returncode == 0 and TRUTH_VERSION in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")
            data = json.loads(golden.read_text(encoding="utf-8"))
            expect("重新生成仍保留已 review 的白名单",
                   len(data.get("known_differences", [])) == 1,
                   f"实际 {data.get('known_differences')}")
            expect("新 golden 记录 23 个字段",
                   sorted(data.get("fields", [])) == sorted(gate.FIELD_NAMES),
                   f"实际 {data.get('fields')}")

        # 用干净 golden 再跑一遍 update -> check，验证闭环。
        fresh_golden = root / "fresh_golden.json"
        env = dict(os.environ)
        env["PYTHONIOENCODING"] = "utf-8"
        env["PYTHONUTF8"] = "1"
        cmd = [PY, str(GATE), "--update-golden", "--index", str(index),
               "--results", str(results), "--root", str(root),
               "--golden", str(fresh_golden), "--samples", "120"]
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", env=env)
        expect("干净 golden 生成 -> 0", proc.returncode == 0,
               f"rc={proc.returncode} out={proc.stdout[-400:]}")
        proc = run(root, index, results, fresh_golden)
        expect("新 golden 复检 -> 0", proc.returncode == 0 and "语义门禁通过" in proc.stdout,
               f"rc={proc.returncode} out={proc.stdout[-500:]}")

        # ---- 宽高互换 ----
        with Mutate(results):
            mutate_result(results, 5, "mi_width", "720")
            mutate_result(results, 5, "mi_height", "1280")
            proc = run(root, index, results, golden)
            expect("宽高成对互换 -> 按已知口径放行",
                   proc.returncode == 0 and "宽高互换 1 对" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")
            proc = run(root, index, results, golden, ["--max-rotation-swaps", "0"])
            expect("互换超过上限 -> 判红", proc.returncode == 1 and "宽高互换" in proc.stdout,
                   f"rc={proc.returncode} out={proc.stdout[-400:]}")

        # ---- 参数错误 ----
        proc = run(root, index, results, root / "no-such-golden.json")
        expect("golden 缺失 -> 2", proc.returncode == 2, f"rc={proc.returncode}")
    finally:
        shutil.rmtree(root, ignore_errors=True)

    if failures:
        print(f"\n{len(failures)} 项断言失败:")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("\n判别力自测全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
