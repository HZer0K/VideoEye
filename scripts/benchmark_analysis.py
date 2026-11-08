#!/usr/bin/env python3
r"""解析性能与规模基准驱动 + 阈值门禁。

配套 tools/analysis_benchmark（编译开关 VIDEOEYE_BUILD_ANALYSIS_BENCHMARK=ON）：
工具负责单文件计时 / RSS / 句柄采集，本脚本负责构造规模样本、跑完与基线 JSON 比对。

覆盖的场景（每个都对应一类真实风险）：
  * long_1h / long_4h —— 长时长：容器结构树、章节元数据、时长/码率计算随小时数增长；
  * large_4gb        —— 超过 4 GB：32 位偏移 / 文件大小字段溢出、按 size 跳读是否还准；
  * corpus_batch     —— 批量吞吐：几千次打开关闭后句柄是否配平、RSS 是否线性上涨。
    工具自身的 handles: start/end/delta 也参与门禁（delta > 4 即判失败）。

样本生成依赖 ffmpeg（PATH 里能找到即可）。1 h 样本用 testsrc2 合成（低分辨率 +
ultrafast，只为了"有真实帧与时间戳"，不追求画质）；4 h 样本由 1 h 样本
-stream_loop 3 -c copy 拼出来，避免真的编码 4 小时；>4 GB 样本是 1 h 样本
复制后截断到 4 GiB + 64 MiB —— 目的就是让文件大小越过 32 位边界，
moov 仍在文件头部（faststart），解析路径不会因为尾部填充而改变。

用法：
    python scripts/benchmark_analysis.py --update-baseline   # 首次/换机器后重建基线
    python scripts/benchmark_analysis.py                     # 与基线比对，超阈值 exit 1
    python scripts/benchmark_analysis.py --cases long_1h,corpus_batch
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_WORK_DIR = ROOT / "build" / "benchmark"
DEFAULT_BASELINE = ROOT / "tests" / "benchmark" / "baseline_analysis.json"

DEFAULT_TIME_RATIO = 1.5
DEFAULT_RSS_RATIO = 1.3
DEFAULT_MAX_HANDLE_DELTA = 4

HOUR_SECONDS = 3600
LARGE_FILE_BYTES = 4 * 1024 ** 3 + 64 * 1024 ** 2

HANDLE_RE = re.compile(r"handles: files=(\d+) max_file_delta=(-?\d+)")


def log(message: str) -> None:
    print(message, flush=True)


def find_tool(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.exists():
            raise SystemExit(f"找不到基准工具: {path}")
        return path
    names = ["analysis_benchmark.exe", "analysis_benchmark"]
    for preset in ("test-release", "release", "test-debug", "debug"):
        for name in names:
            candidate = ROOT / "build" / preset / "bin" / name
            if candidate.exists():
                return candidate
    raise SystemExit(
        "找不到 analysis_benchmark。请先构建：\n"
        "  cmake --preset win-test-release -DVIDEOEYE_BUILD_ANALYSIS_BENCHMARK=ON\n"
        "  cmake --build --preset win-test-release\n"
        "或用 --tool 指定可执行文件路径。"
    )


def run_command(command: list[str], description: str, timeout: int | None = None) -> subprocess.CompletedProcess:
    log(f"  $ {description}")
    started = time.perf_counter()
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    elapsed = time.perf_counter() - started
    if result.returncode != 0:
        raise SystemExit(f"{description} 失败（exit {result.returncode}）\n{result.stderr.strip()}")
    log(f"    完成，用时 {elapsed:.1f}s")
    return result


def pick_video_encoder(ffmpeg: str) -> str:
    result = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True)
    if "libx264" in result.stdout:
        return "libx264"
    if "mpeg4" in result.stdout:
        return "mpeg4"
    raise SystemExit("ffmpeg 里既没有 libx264 也没有 mpeg4，无法合成基准视频")


def ensure_long_1h(ffmpeg: str, path: Path, force: bool) -> None:
    if path.exists() and not force:
        log(f"复用已有样本: {path}")
        return
    encoder = pick_video_encoder(ffmpeg)
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi",
        "-i", f"testsrc2=size=160x120:rate=10:duration={HOUR_SECONDS}",
        "-c:v", encoder, "-pix_fmt", "yuv420p", "-g", "60",
        "-movflags", "+faststart",
        str(path),
    ]
    if encoder == "libx264":
        command[command.index("-g") : command.index("-g")] = ["-preset", "ultrafast"]
    run_command(command, f"合成 1 h 样本 -> {path.name}")


def ensure_long_4h(ffmpeg: str, source: Path, path: Path, force: bool) -> None:
    if path.exists() and not force:
        log(f"复用已有样本: {path}")
        return
    run_command(
        [
            ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-stream_loop", "3", "-i", str(source),
            "-c", "copy", "-movflags", "+faststart",
            str(path),
        ],
        f"拼接 4 h 样本 -> {path.name}",
    )


def ensure_large_4gb(source: Path, path: Path, force: bool) -> None:
    if path.exists() and not force and path.stat().st_size >= LARGE_FILE_BYTES:
        log(f"复用已有样本: {path} ({path.stat().st_size} 字节)")
        return
    if path.exists():
        path.unlink()
    shutil.copyfile(source, path)
    # 复制后扩展文件尾：NTFS/ext4 都按需分配，不会真的占 4 GB 物理空间。
    # 不是"构造合法 4 GB 媒体"，而是"构造一个超过 32 位边界的输入"——基准要测
    # 的是偏移/大小计算，不是解码吞吐。
    with path.open("r+b") as handle:
        handle.truncate(LARGE_FILE_BYTES)
    log(f"生成 >4 GB 样本: {path} ({LARGE_FILE_BYTES} 字节)")


def ensure_samples(work_dir: Path, force: bool) -> dict[str, Path]:
    samples = work_dir / "samples"
    samples.mkdir(parents=True, exist_ok=True)
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit(
            "PATH 里找不到 ffmpeg，无法生成基准样本。请安装 ffmpeg，或预先把 "
            f"long_1h.mp4 / long_4h.mp4 / large_4gb.mp4 放到 {samples} 后加 --skip-generate。"
        )
    long_1h = samples / "long_1h.mp4"
    long_4h = samples / "long_4h.mp4"
    large_4gb = samples / "large_4gb.mp4"

    log("准备基准样本：")
    ensure_long_1h(ffmpeg, long_1h, force)
    ensure_long_4h(ffmpeg, long_1h, long_4h, force)
    ensure_large_4gb(long_1h, large_4gb, force)
    return {"long_1h": long_1h, "long_4h": long_4h, "large_4gb": large_4gb}


def build_cases(samples: dict[str, Path], cases_filter: list[str] | None) -> dict[str, dict]:
    corpus_dirs = [ROOT / "tests" / "corpus" / "container", ROOT / "tests" / "corpus" / "audio"]
    cases: dict[str, dict] = {
        "long_1h": {
            "paths": [samples["long_1h"]],
            "repeat": 3,
            "timeout_ms": 120000,
            "expect_all_ok": True,
            "description": "1 h 长视频解析耗时与内存",
        },
        "long_4h": {
            "paths": [samples["long_4h"]],
            "repeat": 3,
            "timeout_ms": 180000,
            "expect_all_ok": True,
            "description": "4 h 长视频解析耗时与内存",
        },
        "large_4gb": {
            "paths": [samples["large_4gb"]],
            "repeat": 2,
            "timeout_ms": 300000,
            "expect_all_ok": True,
            "description": ">4 GB 文件偏移/大小处理",
        },
        "corpus_batch": {
            "paths": [p for p in corpus_dirs if p.exists()],
            "repeat": 1,
            "timeout_ms": 60000,
            # 语料里本来就有 empty.bin / truncated.* / 坏头文件这些"故意损坏"的样本，
            # 解析失败是它们存在的意义。批量用例只门禁吞吐 / 句柄 / RSS / 超时，
            # 不把预期内的失败算成回归 —— 那样每次基准都会红，门禁也就没人看了。
            "expect_all_ok": False,
            "description": "已提交语料批量吞吐、句柄与 RSS 稳定性",
        },
    }
    if cases_filter:
        unknown = sorted(set(cases_filter) - set(cases))
        if unknown:
            raise SystemExit(f"未知用例: {', '.join(unknown)}；可选: {', '.join(cases)}")
        cases = {name: cases[name] for name in cases_filter if name in cases}
    return cases


def run_case(
    tool: Path, name: str, case: dict, *, work_dir: Path, warmup: int, timeout_ms_override: int | None
) -> dict:
    csv_path = work_dir / f"benchmark_{name}.csv"
    timeout_ms = timeout_ms_override or case["timeout_ms"]
    command = [
        str(tool),
        "--out", str(csv_path),
        "--repeat", str(case["repeat"]),
        "--warmup", str(warmup),
        "--timeout-ms", str(timeout_ms),
        *[str(path) for path in case["paths"]],
    ]
    log(f"[{name}] {case['description']}")
    started = time.perf_counter()
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout_ms / 1000 + 600)
    wall_ms = (time.perf_counter() - started) * 1000.0
    if result.returncode != 0:
        raise SystemExit(f"[{name}] 基准工具失败（exit {result.returncode}）\n{result.stderr.strip()}")

    handle_match = HANDLE_RE.search(result.stderr)
    handle_delta = int(handle_match.group(2)) if handle_match else -1

    rows: list[dict] = []
    if not csv_path.exists():
        raise SystemExit(f"[{name}] 没有产出 CSV: {csv_path}")
    with csv_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise SystemExit(f"[{name}] CSV 为空: {csv_path}")

    total_ms = [float(row["total_ms"]) for row in rows]
    rss_peak_mb = max(float(row["rss_peak_mb"]) for row in rows)
    timed_out = sum(1 for row in rows if row["struct_timed_out"] == "1" or row["media_timed_out"] == "1")
    media_failed = sum(1 for row in rows if row["media_ok"] != "1")
    structure_failed = sum(1 for row in rows if row["struct_ok"] != "1")

    metrics = {
        "description": case["description"],
        "files": len(case["paths"]),
        "rows": len(rows),
        "repeat": case["repeat"],
        "median_total_ms": round(statistics.median(total_ms), 1),
        "max_total_ms": round(max(total_ms), 1),
        "rss_peak_mb": round(rss_peak_mb, 1),
        "handle_delta": handle_delta,
        "timed_out": timed_out,
        "media_failed": media_failed,
        "structure_failed": structure_failed,
        "wall_ms": round(wall_ms, 1),
        "csv": str(csv_path),
    }
    log(
        f"    中位 {metrics['median_total_ms']} ms，峰值 RSS {metrics['rss_peak_mb']} MB，"
        f"句柄增量 {metrics['handle_delta']}，超时 {timed_out}，整体 {metrics['wall_ms']} ms"
    )
    return metrics


def load_baseline(path: Path) -> dict:
    if not path.exists():
        log(f"基线不存在: {path}（首次运行请加 --update-baseline）")
        return {}
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def compare_case(name: str, metrics: dict, baseline: dict, thresholds: dict, case: dict) -> list[str]:
    problems: list[str] = []
    if metrics["timed_out"] > 0:
        problems.append(f"{metrics['timed_out']} 次解析超时（阈值 {metrics['timed_out']} > 0）")
    if case.get("expect_all_ok", True):
        if metrics["media_failed"] > 0:
            problems.append(f"{metrics['media_failed']} 次媒体解析失败")
        if metrics["structure_failed"] > 0:
            problems.append(f"{metrics['structure_failed']} 次结构解析失败")
    if metrics["handle_delta"] >= 0 and metrics["handle_delta"] > thresholds["max_handle_delta"]:
        problems.append(
            f"句柄增量 {metrics['handle_delta']} > {thresholds['max_handle_delta']}（批量打开/关闭泄漏）"
        )

    base = baseline.get("cases", {}).get(name)
    if not base:
        return problems + ["基线里没有这个用例（跳过耗时/RSS 比对）"]
    if "median_total_ms" in base:
        limit = base["median_total_ms"] * thresholds["time_ratio"]
        if metrics["median_total_ms"] > limit:
            problems.append(
                f"中位耗时 {metrics['median_total_ms']} ms > 基线 {base['median_total_ms']} ms × "
                f"{thresholds['time_ratio']:.2f} = {limit:.1f} ms"
            )
    if "rss_peak_mb" in base:
        limit = base["rss_peak_mb"] * thresholds["rss_ratio"]
        if metrics["rss_peak_mb"] > limit:
            problems.append(
                f"峰值 RSS {metrics['rss_peak_mb']} MB > 基线 {base['rss_peak_mb']} MB × "
                f"{thresholds['rss_ratio']:.2f} = {limit:.1f} MB"
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", help="analysis_benchmark 可执行文件路径")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK_DIR), help="样本与 CSV 的工作目录")
    parser.add_argument("--baseline", default=str(DEFAULT_BASELINE), help="基线 JSON 路径")
    parser.add_argument("--report", help="本次结果 JSON 路径（默认 <work-dir>/report.json）")
    parser.add_argument("--cases", help="只跑指定用例，逗号分隔: long_1h,long_4h,large_4gb,corpus_batch")
    parser.add_argument("--repeat", type=int, help="覆盖每个用例的重复次数")
    parser.add_argument("--warmup", type=int, default=1, help="每个用例的预热轮数（不计入基线）")
    parser.add_argument("--timeout-ms", type=int, help="覆盖单阶段解析超时")
    parser.add_argument("--time-ratio", type=float, help="耗时门禁倍率（默认取基线，否则 1.5）")
    parser.add_argument("--rss-ratio", type=float, help="RSS 门禁倍率（默认取基线，否则 1.3）")
    parser.add_argument("--max-handle-delta", type=int, help="句柄增量上限（默认取基线，否则 4）")
    parser.add_argument("--update-baseline", action="store_true", help="用本次结果覆盖基线")
    parser.add_argument("--skip-generate", action="store_true", help="复用已有样本，不调用 ffmpeg")
    parser.add_argument("--force-generate", action="store_true", help="即使样本已存在也重新生成")
    args = parser.parse_args()

    tool = find_tool(args.tool)
    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    baseline_path = Path(args.baseline)
    report_path = Path(args.report) if args.report else work_dir / "report.json"

    samples = {
        "long_1h": work_dir / "samples" / "long_1h.mp4",
        "long_4h": work_dir / "samples" / "long_4h.mp4",
        "large_4gb": work_dir / "samples" / "large_4gb.mp4",
    }
    if args.skip_generate:
        missing = [str(path) for path in samples.values() if not path.exists()]
        if missing:
            raise SystemExit(f"--skip-generate 但样本不存在: {', '.join(missing)}")
    else:
        samples = ensure_samples(work_dir, args.force_generate)

    cases_filter = [item.strip() for item in args.cases.split(",")] if args.cases else None
    cases = build_cases(samples, cases_filter)

    baseline = load_baseline(baseline_path)
    thresholds = {
        "time_ratio": args.time_ratio or baseline.get("thresholds", {}).get("time_ratio", DEFAULT_TIME_RATIO),
        "rss_ratio": args.rss_ratio or baseline.get("thresholds", {}).get("rss_ratio", DEFAULT_RSS_RATIO),
        "max_handle_delta": args.max_handle_delta
        if args.max_handle_delta is not None
        else baseline.get("thresholds", {}).get("max_handle_delta", DEFAULT_MAX_HANDLE_DELTA),
    }

    log(f"工具: {tool}")
    log(f"基线: {baseline_path if baseline else '（无）'}")
    log(f"阈值: 耗时 ×{thresholds['time_ratio']:.2f}，RSS ×{thresholds['rss_ratio']:.2f}，"
        f"句柄增量 ≤ {thresholds['max_handle_delta']}")

    results: dict[str, dict] = {}
    failures: list[str] = []
    for name, case in cases.items():
        if args.repeat:
            case["repeat"] = max(1, args.repeat)
        metrics = run_case(
            tool, name, case, work_dir=work_dir, warmup=args.warmup, timeout_ms_override=args.timeout_ms
        )
        results[name] = metrics
        problems = compare_case(name, metrics, baseline, thresholds, case)
        if args.update_baseline:
            # 首次生成基线时"基线里没有这个用例"是预期状态，不算失败。
            problems = [problem for problem in problems if "基线里没有这个用例" not in problem]
        if problems:
            failures.extend(f"[{name}] {problem}" for problem in problems)
            for problem in problems:
                log(f"    FAIL: {problem}")

    report = {
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "tool": str(tool),
        "work_dir": str(work_dir),
        "thresholds": thresholds,
        "baseline": str(baseline_path),
        "cases": results,
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    with report_path.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
    log(f"报告: {report_path}")

    if args.update_baseline:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema": 1,
            "generated": report["generated"],
            "note": "解析性能基线；同一台机器/同一构建类型下比较。换机器或换编译器后请重新 --update-baseline。",
            "thresholds": thresholds,
            "cases": {
                name: {
                    "median_total_ms": metrics["median_total_ms"],
                    "rss_peak_mb": metrics["rss_peak_mb"],
                    "handle_delta": metrics["handle_delta"],
                    "files": metrics["files"],
                    "repeat": metrics["repeat"],
                }
                for name, metrics in results.items()
            },
        }
        with baseline_path.open("w", encoding="utf-8") as stream:
            json.dump(payload, stream, ensure_ascii=False, indent=2)
        log(f"基线已更新: {baseline_path}")

    if failures:
        log("")
        log("基准门禁失败：")
        for failure in failures:
            log(f"  - {failure}")
        return 1

    log("")
    log("基准门禁通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
