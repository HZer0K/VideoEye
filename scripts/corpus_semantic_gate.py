#!/usr/bin/env python3
"""真实语料的「结论语义」门禁：从"能打开"进到"报得对"。

为什么单独立这道门
------------------
scripts/validate_corpus.py 判的是三段解析链路的**可用性**：打开成功率、超时、
误收、清单拒绝。这些都是二值的，全绿也拦不住「文件打开了，但时长/分辨率/
编码格式全报错」——用户看到的是数字，不是 ok/not ok。
它自己的 ffprobe_crosscheck() 只抽样比 2 个字段，且明确写了"只报告，不判红"。

真值来源（切片 2）
------------------
切片 1 拿 PATH 里的 ffprobe 6.1 当真值，而产品链接 FFmpeg 8.1.3，11 条白名单
全部是工具版本噪音；预编译依赖包又只给 DLL、不给 ffprobe.exe。
现在 tools/media_corpus_validate 会在同一进程内用**同版本 libavformat** 独立再
打开一次，直读 AVFormatContext / AVCodecParameters 原始字段，输出 truth_* 列。
取值路径与产品不同（产品是「FormatFromContext 写文本 → ParseMediaInfoText 读回」），
所以仍抓得住单位、时基、字段错配、格式化精度这类结论层错误；但它不再是另一个
FFmpeg 版本，噪音消失。

  * --update-golden 从结果表的 truth_* 列冻结真值（不跑外部工具，PR/离线可跑）；
  * --check 拿 mi_* 与冻结真值逐字段比对，并额外校验当前 truth_* 没有漂移
    （FFmpeg 升级 / 语料变化会让冻结真值失效，主动判红要求复核）；
  * 差异按 (样本, 字段) 归类，白名单外一律判红；
  * 三道覆盖守护防止「没有事实实时也一路绿」。

三道覆盖守护（对应项目里反复踩过的"做了才有用、坏了看不出来"）
  1. 参与比对的行数、字段数、每个字段的比对数都有下限；全空/全跳过会红。
  2. golden 里的样本在结果表里找不到、或本轮没采到真值，超过上限即红。
  3. 白名单条目必须被真实差异命中；没命中的白名单判红，防止白名单无声膨胀。

已知口径差异（内置，不进白名单文件）
  * 宽高互换：旋转视频 coded/display 口径不同，成对出现才算，且设占比上限。
  * 时长容差 max(0.5s, 1%)；帧率容差 0.02 fps；旋转容差 0.2 度。
  * 码率相对容差 12%：FormatBitRate 只保留 1 位小数（"3.6 Mb/s"），量化误差
    在 Mb/s 量级可达数万 bps，精确相等会整批假判红。

用法
----
  # 生成/更新真值快照（需要先跑一轮带 truth_* 列的全量 validator）
  python scripts/corpus_semantic_gate.py --update-golden \
      --index D:/Coding/Test/media/INDEX.tsv \
      --root  D:/Coding/Test/media \
      --results build/corpus_validation/results.tsv --samples 240

  # 日常校验（秒级，不打开任何媒体文件）
  python scripts/corpus_semantic_gate.py --check \
      --index D:/Coding/Test/media/INDEX.tsv \
      --root  D:/Coding/Test/media \
      --results build/corpus_validation/results.tsv

  # 也可以由 validate_corpus.py --semantic-golden <path> 在一轮里顺带跑。

退出码: 0 通过，1 判红，2 参数/文件错误。
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import random
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GOLDEN = REPO_ROOT / "tests" / "corpus" / "semantic_golden.json"

MANIFEST_FORMATS = {"HLS", "DASH"}
SAMPLE_SEED = 20261108

# 纯分层随机抽样会漏掉稀有事实：240 个样本里 rotation 命中 0 次，覆盖下限只能
# 设成 0，那道守护就等于没有。这里对每个稀有字段确定性地补若干样本（按路径
# 排序取前 N，保证可复现），让稀疏字段也真的被比对到。
RARE_FIELDS = ["rotation", "video_level", "color_primaries", "color_transfer",
               "video_bitrate_bps", "frame_count"]
RARE_SAMPLES_PER_FIELD = 4

# 顺序与 tools/media_corpus_validate/MediaInfoFields.h 的 FieldNames() 一致。
# 新增字段必须两边同步加；check 会拿 golden["fields"] 与这里比对，防止旧快照混入。
FIELD_NAMES = [
    "duration_s", "width", "height", "video_codec", "audio_codec",
    "sample_rate", "channels", "format", "stream_count",
    "overall_bitrate_bps", "video_profile", "video_level", "pix_fmt",
    "frame_rate", "frame_count", "video_bitrate_bps", "color_space",
    "color_primaries", "color_transfer", "rotation", "sample_fmt",
    "bit_depth", "audio_bitrate_bps",
]
FIELD_COLUMNS = {f: "mi_" + f for f in FIELD_NAMES}
TRUTH_COLUMNS = {f: "truth_" + f for f in FIELD_NAMES}

INT_FIELDS = {"width", "height", "sample_rate", "channels", "stream_count",
              "video_level", "frame_count", "bit_depth"}
FLOAT_FIELDS = {"duration_s", "frame_rate", "rotation"}
BITRATE_FIELDS = {"overall_bitrate_bps", "video_bitrate_bps", "audio_bitrate_bps"}
STRING_FIELDS = set(FIELD_NAMES) - INT_FIELDS - FLOAT_FIELDS - BITRATE_FIELDS

# 稀疏字段的逐字段覆盖下限。依据是 240 样本真值快照的实测覆盖率（括号内）：
# 每条都 > 0 且约为实测的一半，所以该字段整列被清空/没采到时仍会精确判红；
# 但不要求每个字段都像宽高那样有上百次比对。默认下限由 --min-per-field 给。
FIELD_MIN_COMPARISONS = {
    "video_profile": 20,      # 实测 39
    "video_level": 12,        # 实测 25
    "frame_count": 18,        # 实测 36
    "video_bitrate_bps": 10,  # 实测 20
    "color_space": 12,        # 实测 27
    "color_primaries": 8,     # 实测 19
    "color_transfer": 8,      # 实测 20
    "rotation": 2,            # 实测 4（RARE_FIELDS 补样后才有事实可比）
}

ZERO_TOKENS = {"", "0", "0.0", "0.000", "-"}


def as_int(value, default=0):
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return default


def as_float(value, default=0.0):
    try:
        return float(str(value).strip())
    except (TypeError, ValueError):
        return default


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rel_key(path: str, root: str) -> str:
    """把绝对/相对、正反斜杠、大小写不一致的路径统一成 golden 的键。"""
    p = str(path).replace("\\", "/")
    r = str(root or "").replace("\\", "/").rstrip("/")
    if r:
        prefix = r.lower() + "/"
        if p.lower().startswith(prefix):
            p = p[len(r) + 1:]
    return p.lower().lstrip("/")


def load_results(path: Path, root: str) -> dict:
    """返回 {rel_key: row}。结果表的 path 列通常是绝对路径，用 root 剥成相对键。"""
    rows = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            key = rel_key(row.get("path", ""), root)
            if key:
                rows[key] = row
    return rows


def load_index(path: Path, root: str) -> dict:
    rows = {}
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            key = rel_key(row.get("path", ""), root)
            if key:
                rows[key] = row
    return rows


def coerce(field: str, value):
    """把 TSV 里的字符串统一成 golden 存的类型。"""
    if field in INT_FIELDS:
        return int(as_float(value))
    if field in FLOAT_FIELDS or field in BITRATE_FIELDS:
        return round(as_float(value), 3)
    return str(value).strip()


def truth_from_row(row: dict) -> dict | None:
    """从结果表的 truth_* 列取真值；没采到真值返回 None。

    零值/空串代表「这个文件没有该事实」，不进 expected —— 否则会拿 0 去比
    产品侧的 0，制造一堆「双方都没有却算一致」的假覆盖。
    ❗ 只按字符串判零不够：avg_frame_rate 为 0 时 ToColumns() 会写出 "-0.000"，
    字符串不在 ZERO_TOKENS 里，归一化后是 -0.0，于是 31 个无帧率样本被当成
    有事实，check 只能一次次跳过。所以还要按数值再判一次。
    """
    if row.get("truth_ok") != "1":
        return None
    out = {}
    for field in FIELD_NAMES:
        raw = (row.get(TRUTH_COLUMNS[field]) or "").strip()
        if raw in ZERO_TOKENS:
            continue
        value = coerce(field, raw)
        if field in STRING_FIELDS:
            if not str(value).strip():
                continue
        elif value <= 0:
            continue
        out[field] = value
    return out or None


def truth_version(results: dict) -> str:
    """从结果表里取 libavformat 版本（全表同一份），冻结进 golden 便于追溯。"""
    versions = Counter((r.get("truth_avformat_version") or "").strip()
                       for r in results.values() if r.get("truth_ok") == "1")
    return versions.most_common(1)[0][0] if versions else "unknown"


def select_keys(index_rows: dict, results: dict, count: int) -> list:
    """按主容器分层抽样，固定种子保证 golden 可复现；只取有独立真值的行。"""
    buckets: dict[str, list] = {}
    for key, idx in index_rows.items():
        row = results.get(key)
        if not row or idx.get("parse") != "ok" or row.get("mi_ok") != "1":
            continue
        if row.get("detected_format", "") in MANIFEST_FORMATS:
            continue
        if truth_from_row(row) is None:
            continue
        bucket = (idx.get("container") or "other").strip().lower()
        buckets.setdefault(bucket, []).append(key)
    for keys in buckets.values():
        keys.sort()

    # 先固定稀有字段样本，再从各容器桶里轮转补齐到 count。
    rare: set[str] = set()
    for field in RARE_FIELDS:
        column = TRUTH_COLUMNS[field]
        candidates = []
        for key, row in results.items():
            idx = index_rows.get(key)
            if not idx or idx.get("parse") != "ok" or row.get("mi_ok") != "1":
                continue
            if row.get("detected_format", "") in MANIFEST_FORMATS:
                continue
            if row.get("truth_ok") != "1":
                continue
            if (row.get(column) or "").strip() in ZERO_TOKENS:
                continue
            candidates.append(key)
        candidates.sort()
        rare.update(candidates[:RARE_SAMPLES_PER_FIELD])
    for name in buckets:
        buckets[name] = [k for k in buckets[name] if k not in rare]

    rng = random.Random(SAMPLE_SEED)
    for keys in buckets.values():
        rng.shuffle(keys)
    names = sorted(buckets)
    picked: list[str] = sorted(rare)
    # 轮转取，容器种类多时也不会被大桶吃光配额。
    while len(picked) < count:
        progressed = False
        for name in names:
            if buckets[name]:
                picked.append(buckets[name].pop())
                progressed = True
                if len(picked) >= count:
                    break
        if not progressed:
            break
    return sorted(picked)


def update_golden(args) -> int:
    index_path = Path(args.index)
    results_path = Path(args.results)
    golden_path = Path(args.golden)
    for path in (index_path, results_path):
        if not path.exists():
            print(f"缺少输入文件: {path}")
            return 2

    index_rows = load_index(index_path, args.root)
    results = load_results(results_path, args.root)
    if results and TRUTH_COLUMNS["duration_s"] not in next(iter(results.values())):
        print(f"结果表没有 {TRUTH_COLUMNS['duration_s']} 列：请用带真值采集的 "
              f"media_corpus_validate 重跑一轮（不要加 --no-truth）。")
        return 2
    keys = select_keys(index_rows, results, args.samples)
    if len(keys) < args.min_rows:
        print(f"可选样本只有 {len(keys)} 条，少于覆盖下限 {args.min_rows}。")
        return 1

    samples = []
    for key in keys:
        expected = truth_from_row(results[key])
        if expected is None:
            continue
        samples.append({"path": key, "expected": expected})
    if not samples:
        print("一条真值都没取到，检查 validator 是否开启了 truth 采集。")
        return 1

    # 白名单是人工 review 的结论，不能因为重新生成真值就被清空 ——
    # 否则「重新生成一次 golden」就能让所有已知差异静默消失，门禁形同虚设。
    # 保留后仍受 check 的「每条必须被命中」约束，语料换了会主动提醒清理。
    previous_whitelist = []
    if golden_path.exists():
        try:
            previous_whitelist = json.loads(golden_path.read_text(encoding="utf-8")).get(
                "known_differences", [])
        except (json.JSONDecodeError, OSError):
            previous_whitelist = []

    golden = {
        "_comment": "由 scripts/corpus_semantic_gate.py --update-golden 生成。真值来自 "
                    "tools/media_corpus_validate 用同版本 libavformat 独立直读的 truth_* 列，"
                    "不是 VideoEye 媒体信息文本自证；更新需人工 review 差异后再提交。"
                    "切片 2 起真值与产品同为 8.1.3，原 ffprobe 6.1 的 7 条版本噪音白名单已清空。",
        "source_index_sha256": sha256_file(index_path),
        "truth_avformat_version": truth_version(results),
        "sample_seed": SAMPLE_SEED,
        "fields": sorted(FIELD_NAMES),
        "known_differences": previous_whitelist,
        "samples": samples,
    }
    golden_path.parent.mkdir(parents=True, exist_ok=True)
    golden_path.write_text(json.dumps(golden, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    coverage = Counter(f for s in samples for f in s["expected"])
    print(f"[golden] 候选 {len(keys)}，写入 {len(samples)} 条 -> {golden_path}")
    print(f"[golden] 真值来源 libavformat {golden['truth_avformat_version']}（与产品同版本）")
    print(f"[golden] 保留已 review 的白名单 {len(previous_whitelist)} 条（check 会验证每条仍被命中）")
    print("[golden] 字段覆盖: " + ", ".join(f"{k}={v}" for k, v in sorted(coverage.items())))
    return 0


def compare_field(field: str, expected, actual) -> bool | None:
    """True 一致 / False 不一致 / None 不可比（产品侧没有这个事实）。"""
    raw = (actual or "").strip()
    if raw in ZERO_TOKENS:
        return None
    if field == "duration_s":
        got = as_float(raw)
        if got <= 0:
            return None
        return abs(got - float(expected)) <= max(0.5, abs(float(expected)) * 0.01)
    if field == "frame_rate":
        got = as_float(raw)
        if got <= 0:
            return None
        return abs(got - float(expected)) <= 0.02
    if field == "rotation":
        got = as_float(raw)
        if got == 0.0:
            return None
        return abs(got - float(expected)) <= 0.2
    if field in BITRATE_FIELDS:
        got = as_float(raw)
        want = float(expected)
        if got <= 0 or want <= 0:
            return None
        # FormatBitRate 的 1 位小数在 Mb/s 量级有 ~0.05 Mb/s 量化误差。
        return abs(got - want) <= max(200.0, want * 0.12)
    if field in INT_FIELDS:
        got = as_int(raw)
        if got <= 0:
            return None
        return got == int(expected)
    if field in STRING_FIELDS:
        got = raw.lower()
        if not got:
            return None
        return got == str(expected).strip().lower()
    return None


def strict_equal(field: str, expected, current) -> bool:
    """真值漂移判定：两边都来自 ToColumns()，应当逐字一致（数值允许浮点尾差）。"""
    if field in INT_FIELDS:
        return int(expected) == int(current)
    if field in FLOAT_FIELDS or field in BITRATE_FIELDS:
        return abs(float(expected) - float(current)) < 1e-6
    return str(expected).strip().lower() == str(current).strip().lower()


def check(args) -> int:
    golden_path = Path(args.golden)
    results_path = Path(args.results)
    index_path = Path(args.index)
    for path in (golden_path, results_path, index_path):
        if not path.exists():
            print(f"缺少输入文件: {path}")
            return 2
    golden = json.loads(golden_path.read_text(encoding="utf-8"))
    results = load_results(results_path, args.root)
    samples = golden.get("samples", [])
    whitelist = golden.get("known_differences", [])
    failures: list[str] = []
    for i, rule in enumerate(whitelist):
        if not rule.get("path_contains") or not rule.get("reason"):
            failures.append(f"白名单第 {i + 1} 条缺 path_contains / reason: {rule}")

    # 语料树换了却还拿旧 golden 比，等于拿旧答案对新卷子。
    current_sha = sha256_file(index_path)
    if golden.get("source_index_sha256") != current_sha:
        failures.append(f"INDEX.tsv 已变化（golden 记的是 {golden.get('source_index_sha256', '')[:12]}，"
                        f"当前 {current_sha[:12]}），必须显式 --update-golden 后复核")
    # 字段集合变了却还用旧快照，新字段会被静默跳过 —— 覆盖守护要主动拦下。
    if sorted(golden.get("fields", [])) != sorted(FIELD_NAMES):
        failures.append(f"golden 字段集合与当前门禁不一致（{len(golden.get('fields', []))} vs "
                        f"{len(FIELD_NAMES)}），必须 --update-golden 重新冻结真值")

    per_field = Counter()
    compared_fields = 0
    compared_rows = 0
    skipped = 0
    missing = []
    truth_missing = []
    drift = []
    differences = []
    rotation_swaps = 0
    for sample in samples:
        key = rel_key(sample.get("path", ""), args.root)
        row = results.get(key)
        if row is None:
            missing.append(key)
            continue
        current_truth = truth_from_row(row)
        if current_truth is None:
            truth_missing.append(key)
        else:
            expected = sample.get("expected", {})
            if set(current_truth) != set(expected) or any(
                not strict_equal(f, expected[f], current_truth[f]) for f in expected
            ):
                drift.append(key)
        row_compared = False
        expected = sample.get("expected", {})
        for field, value in expected.items():
            if field not in FIELD_COLUMNS:
                continue
            verdict = compare_field(field, value, row.get(FIELD_COLUMNS[field]))
            if verdict is None:
                skipped += 1
                continue
            row_compared = True
            compared_fields += 1
            per_field[field] += 1
            if not verdict:
                differences.append({"path": key, "field": field,
                                    "expected": value, "actual": row.get(FIELD_COLUMNS[field])})
        if row_compared:
            compared_rows += 1

    # 宽高互换：旋转口径差异，成对出现才算，落单的仍然按差异判红。
    by_path_field = {(d["path"], d["field"]): d for d in differences}
    swap_keys = []
    for (path, field), diff in by_path_field.items():
        if field not in ("width", "height"):
            continue
        other = "height" if field == "width" else "width"
        mate = by_path_field.get((path, other))
        if not mate:
            continue
        if str(diff["expected"]) == str(mate["actual"]) and str(mate["expected"]) == str(diff["actual"]):
            swap_keys.append((path, field))
            swap_keys.append((path, other))
    swap_set = set(swap_keys)
    if swap_set:
        rotation_swaps = len(swap_set) // 2
        differences = [d for d in differences if (d["path"], d["field"]) not in swap_set]

    # 白名单：按 (路径子串, 字段) 放行，且每条都必须被真实差异命中。
    hits: set[int] = set()
    allowed = []
    red = []
    for diff in differences:
        matched = None
        for i, rule in enumerate(whitelist):
            if rule.get("field") == diff["field"] and rule.get("path_contains", "") in diff["path"]:
                matched = i
                break
        if matched is None:
            red.append(diff)
        else:
            hits.add(matched)
            allowed.append(diff)
    unused = [i for i in range(len(whitelist)) if i not in hits]
    if unused:
        for i in unused:
            failures.append(f"白名单第 {i + 1} 条未被任何差异命中: {whitelist[i]}")

    # ---- 覆盖守护 ----
    if compared_rows < args.min_rows:
        failures.append(f"参与比对的行数 {compared_rows} < {args.min_rows}")
    if compared_fields < args.min_fields:
        failures.append(f"参与比对的字段数 {compared_fields} < {args.min_fields}")
    for field in FIELD_NAMES:
        limit = FIELD_MIN_COMPARISONS.get(field, args.min_per_field)
        if per_field[field] < limit:
            failures.append(f"字段 {field} 只比对了 {per_field[field]} 次 < {limit}")
    if len(missing) > args.max_missing:
        failures.append(f"{len(missing)} 个 golden 样本在结果表里找不到（> {args.max_missing}），"
                        f"首个: {missing[0] if missing else '-'}")
    if len(truth_missing) > args.max_truth_missing:
        failures.append(f"{len(truth_missing)} 个 golden 样本本轮没采到独立真值"
                        f"（> {args.max_truth_missing}），首个: {truth_missing[0] if truth_missing else '-'}")
    if drift:
        failures.append(f"{len(drift)} 个样本独立真值相对 golden 发生漂移，"
                        f"需 --update-golden 重新冻结并复核，首个: {drift[0]}")
    if rotation_swaps > args.max_rotation_swaps:
        failures.append(f"宽高互换 {rotation_swaps} 对 > 上限 {args.max_rotation_swaps}，"
                        f"别拿旋转口径当万能白名单")
    if red:
        failures.append(f"白名单外语义差异 {len(red)} 条")

    print(f"[semantic] golden 样本 {len(samples)}，比对行 {compared_rows}，字段比对 {compared_fields}，"
          f"不可比跳过 {skipped}，宽高互换 {rotation_swaps} 对")
    print("[semantic] 逐字段: " + ", ".join(f"{k}={per_field[k]}" for k in FIELD_NAMES))
    print(f"[semantic] 真值缺失 {len(truth_missing)}，真值漂移 {len(drift)}")
    print(f"[semantic] 差异 {len(differences)} 条（白名单放行 {len(allowed)}，判红 {len(red)}）")
    for diff in red[:20]:
        print(f"    DIFF {diff['path']} {diff['field']}: golden={diff['expected']} actual={diff['actual']}")
    if len(red) > 20:
        print(f"    ... 另有 {len(red) - 20} 条")

    if failures:
        print("\n语义门禁未通过:")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("语义门禁通过。")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="真实语料结论语义门禁")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--update-golden", action="store_true",
                      help="从结果表的 truth_* 列冻结真值快照")
    mode.add_argument("--check", action="store_true", help="按冻结真值校验结果表")
    parser.add_argument("--index", help="语料 INDEX.tsv")
    parser.add_argument("--root", default="", help="语料根目录")
    parser.add_argument("--results", help="media_corpus_validate 输出的 TSV（需含 truth_* 列）")
    parser.add_argument("--golden", default=str(DEFAULT_GOLDEN), help="真值快照 JSON")
    parser.add_argument("--samples", type=int, default=240, help="golden 样本数")
    parser.add_argument("--min-rows", type=int, default=100)
    parser.add_argument("--min-fields", type=int, default=1000)
    parser.add_argument("--min-per-field", type=int, default=20)
    parser.add_argument("--max-missing", type=int, default=0)
    parser.add_argument("--max-truth-missing", type=int, default=0)
    parser.add_argument("--max-rotation-swaps", type=int, default=12)
    args = parser.parse_args()

    if not args.index or not args.results or not args.root:
        parser.error("--index / --results / --root 都是必填")
    return update_golden(args) if args.update_golden else check(args)


if __name__ == "__main__":
    sys.exit(main())
