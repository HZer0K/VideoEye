#!/usr/bin/env python3
"""真实语料回归门禁（nightly / 本地）

驱动 tools/media_corpus_validate 对一棵外部语料树跑三段解析（格式检测、容器结构、
媒体信息），再与语料自带的 INDEX.tsv 比对，按阈值给出通过/失败。

为什么单独成脚本而不是 ctest：
  * 语料是几十 GB 的外部目录，不在仓库里，PR CI 跑不了；
  * 一轮全量要几十秒到几分钟，且结果需要人看分布（主容器识别率、误收、超时）；
  * 阈值会随基线收紧，放在脚本参数里比埋在 CI yaml 里更好维护。

门禁项（任一命中即 exit 1）：
  1. 校验工具本身非 0 退出（崩溃 / 参数错误）
  2. 任一文件在结构或媒体阶段超时（struct_timed_out / mi_timed_out）
  3. INDEX parse=ok 的结构成功率低于 --min-struct-on-ok
  4. INDEX parse=ok 的媒体成功率低于 --min-media-on-ok（清单除外）
  5. INDEX parse=fail 的"仍然能打开"数量超过 --max-false-accept
  6. 清单文件被 MediaInfoAnalyzer 打开成功、或超时（应被入口拒绝）
  7. 传了 --semantic-golden 时，结论语义门禁未通过（23 个 mi_* 字段与
     media_corpus_validate 用同版本 libavformat 独立直读、冻结在 golden 里的
     truth_* 真值不一致），见 scripts/corpus_semantic_gate.py
  8. 负样本诊断门禁未通过（失败原因不可区分 / 守恒被破坏 / 无事实的成功结论），
     见 scripts/corpus_defect_gate.py；默认开启，--no-defect-gate 可跳过

报告项（只打印，不判红）：主容器识别率、与 ffprobe 的抽样交叉校验。
注意第 7 条不是抽样交叉校验的升级版而是另一回事：那条只打印 2 个字段的差异，
这条按 23 个字段逐样本判红、校验真值未漂移，且带覆盖守护防止"没有事实实时也一路绿"。
⚠️ 真值来自校验工具输出的 truth_* 列，所以不能用 --no-truth 生成的旧结果表跑语义门禁。

用法：
  python scripts/validate_corpus.py \
      --index D:/Coding/Test/media/INDEX.tsv \
      --root  D:/Coding/Test/media \
      --semantic-golden tests/corpus/semantic_golden.json \
      --ffprobe-sample 100

工具默认从 build/test-release/bin 找；没有时先构建：
  cmake --preset win-test-release -DVIDEOEYE_BUILD_CORPUS_VALIDATOR=ON
  cmake --build --preset win-test-release --target media_corpus_validate
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SEMANTIC_GATE = REPO_ROOT / "scripts" / "corpus_semantic_gate.py"
DEFECT_GATE = REPO_ROOT / "scripts" / "corpus_defect_gate.py"

MANIFEST_FORMATS = {"HLS", "DASH"}
MANIFEST_ERROR_TOKEN = "清单"

# INDEX 的 container 列（ffprobe format_name 的简化写法）→ 可接受的展示名集合。
# 有些是同一族的两种叫法，不能算识别错误：
#   mp4/mov —— QuickTime 与 ISO-BMFF 是同一族，FormatDetector 按 ftyp 品牌区分；
#   matroska —— .webm 扩展名会返回 WebM，但容器就是 Matroska。
# 对不上或不在表里的容器不参与识别率统计，避免把"INDEX 没标"算成识别错误。
CONTAINER_TO_FORMATS = {
    "mp4": {"MP4", "MOV"},
    "mov": {"MOV", "MP4"},
    "matroska": {"MKV", "WebM"},
    "webm": {"WebM", "MKV"},
    "avi": {"AVI"},
    "flv": {"FLV"},
    "ts": {"MPEG-TS"},
    "mpeg": {"MPEG-PS"},
    "mxf": {"MXF"},
    "asf": {"ASF"},
    "ogg": {"OGG"},
}


def default_tool_path() -> Path:
    exe = "media_corpus_validate.exe" if os.name == "nt" else "media_corpus_validate"
    for preset in ("test-release", "test-debug", "release", "debug"):
        candidate = REPO_ROOT / "build" / preset / "bin" / exe
        if candidate.exists():
            return candidate
    return REPO_ROOT / "build" / "test-release" / "bin" / exe


def as_int(value: str, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def as_float(value: str, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def run_validator(args) -> Path:
    tool = Path(args.tool)
    if not tool.exists():
        sys.exit(f"校验工具不存在: {tool}\n先构建: -DVIDEOEYE_BUILD_CORPUS_VALIDATOR=ON")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(tool),
        "--index", str(args.index),
        "--root", str(args.root),
        "--out", str(out_path),
        "--jobs", str(args.jobs),
        "--timeout-ms", str(args.timeout_ms),
    ]
    if args.limit:
        cmd += ["--limit", str(args.limit)]
    if args.skip:
        cmd += ["--skip", str(args.skip)]

    print(f"[tool] {' '.join(cmd)}", flush=True)
    completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    tail = (completed.stdout or "") + (completed.stderr or "")
    print("\n".join(tail.strip().splitlines()[-6:]), flush=True)
    if completed.returncode != 0:
        sys.exit(f"校验工具退出码 {completed.returncode}（崩溃或参数错误），结果不可信。")
    return out_path


def load_rows(out_path: Path) -> list[dict]:
    with out_path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def is_manifest(row: dict) -> bool:
    return row.get("detected_format", "") in MANIFEST_FORMATS


def run_semantic_gate(args, results: Path) -> list[str]:
    """结论语义门禁：可用性全绿不代表报出来的数字对。"""
    if not args.semantic_golden:
        return []
    cmd = [
        sys.executable, str(SEMANTIC_GATE), "--check",
        "--index", str(args.index),
        "--results", str(results),
        "--root", str(args.root),
        "--golden", str(args.semantic_golden),
    ]
    print(f"[semantic] {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print((proc.stdout or "").rstrip(), flush=True)
    if proc.stderr:
        print((proc.stderr or "").rstrip(), flush=True)
    if proc.returncode == 2:
        return [f"语义门禁输入不全（golden/结果表/INDEX 缺失）: {args.semantic_golden}"]
    if proc.returncode != 0:
        return ["结论语义门禁未通过（见上方 DIFF 与覆盖守护明细）"]
    return []


def run_defect_gate(args, results: Path) -> list[str]:
    """负样本诊断门禁：可用性全绿不代表「打不开」能说清是哪一种打不开。"""
    if args.no_defect_gate:
        return []
    cmd = [
        sys.executable, str(DEFECT_GATE),
        "--index", str(args.index),
        "--results", str(results),
        "--root", str(args.root),
        "--whitelist", str(args.defect_whitelist),
    ]
    print(f"[defect] {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print((proc.stdout or "").rstrip(), flush=True)
    if proc.stderr:
        print((proc.stderr or "").rstrip(), flush=True)
    if proc.returncode != 0:
        return ["负样本诊断门禁未通过（见上方判红明细）"]
    return []


def container_accuracy(rows: list[dict]) -> None:
    matched = 0
    considered = 0
    misses: Counter = Counter()
    for row in rows:
        if row.get("idx_parse") != "ok":
            continue
        expected = CONTAINER_TO_FORMATS.get(row.get("idx_container", "").strip().lower())
        if not expected:
            continue
        considered += 1
        actual = row.get("detected_format", "")
        if actual in expected:
            matched += 1
        else:
            misses[f"{row.get('idx_container', '?')}->{actual}"] += 1
    if considered:
        print(f"[format] 主容器识别率 {matched}/{considered} = {matched / considered:.2%}")
        for key, count in misses.most_common(8):
            print(f"           {key:<28} {count}")


def ffprobe_crosscheck(rows: list[dict], sample: int, ffprobe: str) -> None:
    """抽样与 ffprobe 做独立交叉校验。只报告，不判红：
    分辨率口径（DisplayWidth/旋转/封面图）与时长口径（垃圾前缀 MP3）存在
    已知差异，硬判红会变成"为了绿而改阈值"。"""
    candidates = [
        row for row in rows
        if row.get("idx_parse") == "ok" and row.get("mi_ok") == "1" and not is_manifest(row)
    ]
    if not candidates or sample <= 0:
        return
    random.seed(20261107)  # 固定种子，nightly 结果可比
    picks = random.sample(candidates, min(sample, len(candidates)))
    duration_bad = 0
    resolution_bad = 0
    for row in picks:
        proc = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format=duration:stream=width,height",
             "-of", "json", row["path"]],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if proc.returncode != 0:
            continue
        try:
            info = json.loads(proc.stdout)
        except json.JSONDecodeError:
            continue
        theirs = as_float(info.get("format", {}).get("duration"))
        ours = as_float(row.get("mi_duration_s"))
        if theirs > 0 and ours > 0:
            tolerance = max(0.5, theirs * 0.01)
            if abs(theirs - ours) > tolerance:
                duration_bad += 1
        widths = [as_int(s.get("width", 0)) for s in info.get("streams", [])]
        heights = [as_int(s.get("height", 0)) for s in info.get("streams", [])]
        width = as_int(row.get("mi_width"))
        height = as_int(row.get("mi_height"))
        if widths and width and height:
            pairs = {(w, h) for w, h in zip(widths, heights) if w and h}
            if pairs and (width, height) not in pairs and (height, width) not in pairs:
                resolution_bad += 1
    print(f"[ffprobe] 抽样 {len(picks)}: 时长差异 {duration_bad}, 分辨率口径差异 {resolution_bad}")


def main() -> int:
    parser = argparse.ArgumentParser(description="真实语料解析回归门禁")
    parser.add_argument("--index", required=True, help="语料自带的 INDEX.tsv")
    parser.add_argument("--root", required=True, help="语料根目录")
    parser.add_argument("--tool", default=str(default_tool_path()), help="media_corpus_validate 路径")
    parser.add_argument("--out", default=str(REPO_ROOT / "build" / "corpus_validation" / "results.tsv"))
    parser.add_argument("--jobs", type=int, default=8)
    # 单阶段预算。实测 fate-suite/jxl/orange.jxl 前三段就要 28~31 s（单线程），
    # 8 并发下 struct 阶段在 20~25 s 波动，20 s 会把它反复误判成超时；30 s 给
    # 并发留约 50% 余量，仍远低于「真卡死不返回」的量级，超时判据保持判红。
    parser.add_argument("--timeout-ms", type=int, default=30000, help="单文件单阶段超时")
    parser.add_argument("--limit", type=int, default=0, help="只跑前 N 条（调试用）")
    parser.add_argument("--skip", type=int, default=0)
    parser.add_argument("--min-struct-on-ok", type=float, default=0.995)
    parser.add_argument("--min-media-on-ok", type=float, default=0.995)
    parser.add_argument("--max-false-accept", type=int, default=60, help="parse=fail 却仍能打开的上限")
    parser.add_argument("--ffprobe-sample", type=int, default=0, help="与 ffprobe 交叉校验的抽样数（0=不做）")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--semantic-golden", default="",
                        help="结论语义真值快照；给了就顺带跑 corpus_semantic_gate.py --check")
    parser.add_argument("--no-defect-gate", action="store_true",
                        help="跳过负样本诊断门禁（默认开启）")
    parser.add_argument("--defect-whitelist",
                        default=str(REPO_ROOT / "tests" / "corpus" / "defect_gate_whitelist.json"),
                        help="负样本门禁的例外白名单 JSON")
    args = parser.parse_args()

    out_path = run_validator(args)
    rows = load_rows(out_path)
    if not rows:
        sys.exit("校验结果为空。")

    failures: list[str] = []
    timeouts = [r for r in rows if r.get("struct_timed_out") == "1" or r.get("mi_timed_out") == "1"]
    if timeouts:
        failures.append(f"{len(timeouts)} 个文件超时，首个: {timeouts[0]['path']}")

    manifests = [r for r in rows if is_manifest(r)]
    manifest_opened = [r for r in manifests if r.get("mi_ok") == "1"]
    if manifest_opened:
        failures.append(f"{len(manifest_opened)} 个清单被 MediaInfoAnalyzer 打开（应入口拒绝），"
                        f"首个: {manifest_opened[0]['path']}")
    manifest_not_refused = [
        r for r in manifests
        if r.get("mi_ok") != "1" and MANIFEST_ERROR_TOKEN not in (r.get("mi_error") or "")
    ]
    if manifest_not_refused:
        failures.append(f"{len(manifest_not_refused)} 个清单的拒绝原因不是清单守卫，"
                        f"首个: {manifest_not_refused[0]['path']} -> {manifest_not_refused[0].get('mi_error')}")

    ok_rows = [r for r in rows if r.get("idx_parse") == "ok"]
    struct_ok = sum(1 for r in ok_rows if r.get("struct_ok") == "1")
    if ok_rows:
        rate = struct_ok / len(ok_rows)
        print(f"[struct] INDEX parse=ok: {struct_ok}/{len(ok_rows)} = {rate:.2%}")
        if rate < args.min_struct_on_ok:
            failures.append(f"结构成功率 {rate:.2%} < 阈值 {args.min_struct_on_ok:.2%}")

    media_denominator = [r for r in ok_rows if not is_manifest(r)]
    media_ok = sum(1 for r in media_denominator if r.get("mi_ok") == "1")
    if media_denominator:
        rate = media_ok / len(media_denominator)
        print(f"[media]  INDEX parse=ok: {media_ok}/{len(media_denominator)} = {rate:.2%}"
              f"（清单 {len(manifests)} 条按预期拒绝，不计入）")
        if rate < args.min_media_on_ok:
            failures.append(f"媒体信息成功率 {rate:.2%} < 阈值 {args.min_media_on_ok:.2%}")

    missing_ok = sorted(
        (r for r in media_denominator if r.get("mi_ok") != "1"),
        key=lambda r: r.get("path", ""),
    )[:10]
    for row in missing_ok:
        print(f"          MISS {row['path']} :: {row.get('mi_error')}")

    fail_rows = [r for r in rows if r.get("idx_parse") == "fail"]
    # "误收"只认媒体信息阶段打开成功：INDEX 的 parse 是 ffprobe 的结论，
    # 结构分析能建出树只说明"字节结构完整"，不代表该文件能被正常解码（很多
    # 损坏 MP4 仍能读出 moov）。清单不计入——拒绝清单是预期行为。
    false_accept = [r for r in fail_rows if r.get("mi_ok") == "1" and not is_manifest(r)]
    struct_open = [r for r in fail_rows if r.get("struct_ok") == "1"]
    print(f"[reject] INDEX parse=fail: {len(fail_rows)} 条，媒体信息仍能打开 {len(false_accept)} 条"
          f"（阈值 {args.max_false_accept}）；结构可解析 {len(struct_open)} 条（仅参考）")
    if len(false_accept) > args.max_false_accept:
        failures.append(f"误收 {len(false_accept)} 条 > 阈值 {args.max_false_accept}")
        for row in false_accept[:10]:
            print(f"          ACCEPT {row['path']}")

    container_accuracy(rows)
    ffprobe_crosscheck(rows, args.ffprobe_sample, args.ffprobe)
    failures += run_semantic_gate(args, out_path)
    failures += run_defect_gate(args, out_path)

    print(f"\n结果明细: {out_path}")
    if failures:
        print("\n门禁未通过:")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("\n门禁通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
