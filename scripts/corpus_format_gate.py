#!/usr/bin/env python3
"""格式探测真值门禁 —— 让「认成什么格式」这件事有独立真值。

背景
----
``validate_corpus.py::container_accuracy()`` 以前只是一行 print：把 11 种 INDEX
容器映射到展示名，算出识别率就完事，**从不判红**。更要命的是它只认 11 种容器，
4429 行里 1957 条带 ``container`` 真值的样本（image 689 / h264 240 / hevc 215 /
wav 63 / mp3 40 / 清单 47 ...）根本不在视野内 —— 换句话说，一半以上的格式探测
结论从来没有被任何门禁检查过。

这里把真值分成两组，避免把「产品压根没有专用解析器」误当成错误：

* **硬组**：FormatDetector 有明确枚举与魔数规则的容器族（MP4/MOV、Matroska、
  AVI、FLV、TS、PS、ASF、OGG、MXF、清单）。必须精确落进可接受集合；
  ``Unknown`` 单独统计并设上限（真值本身有 junk 前缀 / 私有封装时不强求）。
* **软组**：图片、裸流、纯音频、字幕、私有封装等。产品走 FFmpeg 通用回退是设计
  选择，不要求报出具体名字，但**绝不能跨大类误判**（HEIF/AVIF 落在 ISOBMFF 族
  是合法的，一张 PNG 被判成 MPEG-TS 就不是）。

这条分层很重要：把软组的 ``Unknown`` 也判红，等于要求 FormatDetector 认识全世界
所有格式，只会制造「为了绿而改阈值」的假门禁。

覆盖守护（缺一即判红）：结果表非空且够大、真值列齐全、硬/软两组样本都够、
真值容器种类够多、必需容器组都在、INDEX 的 container 与结果表 idx_container
一致、白名单每条都被**真实差异**命中。最后一条沿用负样本门禁的规矩：一行
白名单不许把整个门禁架空。
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_WHITELIST = REPO_ROOT / "tests" / "corpus" / "format_gate_whitelist.json"
DEFAULT_INDEX = "D:/Coding/Test/media/INDEX.tsv"
DEFAULT_ROOT = "D:/Coding/Test/media"

# 硬组：真值容器 -> 可接受的 FormatDetector::FormatName 集合。
# 同族别名（mp4/mov、matroska/webm）不算错，见 validate_corpus.py 的历史口径。
HARD_EXPECTED = {
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
    "mp4-segment": {"MP4", "MOV"},
    "hls-playlist": {"HLS"},
    "dash-manifest": {"DASH"},
}

# FormatDetector 展示名 -> 大类。跨大类判定用，不要求软组报出精确名字。
FORMAT_FAMILY = {
    "MP4": "isobmff", "MOV": "isobmff",
    "MKV": "matroska", "WebM": "matroska",
    "AVI": "avi", "FLV": "flv",
    "MPEG-TS": "ts", "MPEG-PS": "ps",
    "ASF": "asf", "OGG": "ogg", "MXF": "mxf",
    "Raw ES": "raw",
    "HLS": "manifest", "DASH": "manifest",
    "Generic": "generic", "Unknown": "unknown",
}

DEFAULT_SOFT_ALLOWED = {"unknown", "generic", "raw", "manifest"}

# 软组里需要放宽的族（都是「真值容器与产品大类本来就有交集」的合法情况）：
#   image  —— HEIF/AVIF 是 ISOBMFF 盒结构，判 MP4/MOV 合法；个别 WebP 落在 WebM。
#   裸视频 —— MPEG-4 / MPEG-2 裸流可能被 PS 或 ISOBMFF 解析器接住。
#   m4v    —— 有的带 ftyp，有的就是裸 MPEG-4 视频。
SOFT_ALLOWED = {
    "image": {"isobmff", "matroska", "unknown", "generic", "raw"},
    "h264": {"raw", "isobmff", "ps", "unknown", "generic"},
    "hevc": {"raw", "isobmff", "ps", "unknown", "generic"},
    "vvc": {"raw", "isobmff", "ps", "unknown", "generic"},
    "vc1": {"raw", "ps", "unknown", "generic"},
    "mpeg-video": {"raw", "ps", "unknown", "generic"},
    "m4v": {"isobmff", "raw", "ps", "unknown", "generic"},
    "rm": {"generic", "unknown", "avi"},
    "raw-audio": {"raw", "isobmff", "unknown", "generic"},
    # 下面三类是「真值容器与产品大类本来就有交集」的另一种形态：文件本身没有
    # 可识别魔数，Detect() 退回扩展名兜底（.mpg / .mp4 / .asf），于是落在 PS /
    # ISOBMFF / ASF 大类。它们不是探测错误，是兜底口径，单独列明免得以后被
    # 误当成「跨大类误判」而把真判据改松。
    "iv8": {"ps", "raw", "unknown", "generic"},
    "cavsvideo": {"isobmff", "raw", "ps", "unknown", "generic"},
    "argo_asf": {"asf", "generic", "unknown"},
}

MIN_ROWS = 3000
MIN_HARD_ROWS = 1200
MIN_SOFT_ROWS = 1200
MIN_TRUTH_CONTAINERS = 20
MAX_HARD_UNKNOWN = 20
REQUIRED_CONTAINERS = {"mp4", "matroska", "ts", "avi", "image", "h264", "wav", "asf", "ogg", "mxf"}


def rel_key(path: str, root: str) -> str:
    norm = path.replace("\\", "/")
    root_norm = root.replace("\\", "/").rstrip("/")
    if root_norm and norm.startswith(root_norm + "/"):
        norm = norm[len(root_norm) + 1:]
    return norm.lstrip("/").lower()


def load_tsv(path: Path) -> list[dict]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def load_whitelist(path: Path | None) -> dict:
    if not path or not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return data.get("exceptions", {}) if isinstance(data, dict) else {}


def check(results: Path, index: Path, root: str, whitelist_path: Path | None) -> int:
    failures: list[str] = []
    # 逐行判据可能一次报上千条；宏观覆盖守护必须单独收，否则会被截断线挤没，
    # 运维只看到「一堆文件判红」，看不到真正的根因（真值被换掉 / 门禁空转）。
    row_failures: list[str] = []
    whitelist = load_whitelist(whitelist_path)
    hits: set[str] = set()

    rows = load_tsv(results)
    if not rows:
        print("[format] 结果表为空 —— 门禁不能空转")
        return 1
    needed = {"path", "idx_container", "detected_format"}
    missing_cols = needed - set(rows[0])
    if missing_cols:
        print(f"[format] 结果表缺列 {sorted(missing_cols)}（需重跑 media_corpus_validate）")
        return 1
    if len(rows) < MIN_ROWS:
        failures.append(f"结果表只有 {len(rows)} 行 < {MIN_ROWS}，门禁空转")

    # INDEX 是唯一的真值来源；同时核对 validator 有没有把真值列抄错。
    truth: dict[str, dict] = {}
    for item in load_tsv(index):
        key = rel_key(item.get("path", ""), root)
        if key:
            truth[key] = item
    if not truth:
        print("[format] INDEX 为空 —— 没有真值可比")
        return 1

    hard_rows = 0
    soft_rows = 0
    hard_unknown = 0
    containers: Counter = Counter()
    mismatches: Counter = Counter()

    def exempt(row: dict) -> bool:
        key = rel_key(row.get("path", ""), root)
        if key in whitelist:
            hits.add(key)
            return True
        return False

    def judge(row: dict, message: str) -> None:
        if exempt(row):
            return
        row_failures.append(f"{message}: {row.get('path')}")

    for row in rows:
        key = rel_key(row.get("path", ""), root)
        source = truth.get(key)
        if source is None:
            continue
        container = (source.get("container") or "").strip().lower()
        if not container or container == "-":
            continue
        if (source.get("parse") or "").strip().lower() != "ok":
            continue
        if (row.get("idx_container") or "").strip().lower() != container:
            mismatches[container] += 1
        containers[container] += 1

        actual = (row.get("detected_format") or "").strip()
        expected = HARD_EXPECTED.get(container)
        if expected:
            hard_rows += 1
            if actual in expected:
                continue
            if actual == "Unknown":
                hard_unknown += 1
                continue
            judge(row, f"[硬组] {container} 被探测成 {actual!r}，应为 {sorted(expected)}")
            continue

        soft_rows += 1
        family = FORMAT_FAMILY.get(actual)
        if family is None:
            judge(row, f"[软组] {container} 落进未知探测结果 {actual!r}")
            continue
        allowed = SOFT_ALLOWED.get(container, DEFAULT_SOFT_ALLOWED)
        if family not in allowed:
            judge(row, f"[软组] {container} 跨大类误判成 {actual!r}({family})，允许 {sorted(allowed)}")

    if mismatches:
        failures.append(f"结果表 idx_container 与 INDEX.container 不一致 {sum(mismatches.values())} 行")
    if hard_rows < MIN_HARD_ROWS:
        failures.append(f"硬组样本只有 {hard_rows} < {MIN_HARD_ROWS}，精确判据空转")
    if soft_rows < MIN_SOFT_ROWS:
        failures.append(f"软组样本只有 {soft_rows} < {MIN_SOFT_ROWS}，跨类判据空转")
    if len(containers) < MIN_TRUTH_CONTAINERS:
        failures.append(f"真值容器只有 {len(containers)} 种 < {MIN_TRUTH_CONTAINERS}，语料被换掉了")
    missing_groups = REQUIRED_CONTAINERS - set(containers)
    if missing_groups:
        failures.append(f"缺失必需真值容器: {sorted(missing_groups)}")
    if hard_unknown > MAX_HARD_UNKNOWN:
        failures.append(f"硬组 Unknown 有 {hard_unknown} 条 > {MAX_HARD_UNKNOWN}，魔数识别退化")

    stale = set(whitelist) - hits
    if stale:
        failures.append(f"白名单失效（这些行本来就没违反判据）: {sorted(stale)[:8]}")

    print(f"[format] 结果 {len(rows)} 行，真值容器 {len(containers)} 种，"
          f"硬组 {hard_rows} / 软组 {soft_rows}，硬组 Unknown {hard_unknown}")
    print("[format] 样本最多的真值: " + ", ".join(f"{k}={v}" for k, v in containers.most_common(8)))
    print(f"[format] 白名单命中 {len(hits)}/{len(whitelist)}")

    all_failures = failures + row_failures
    if all_failures:
        print(f"[format] 判红 {len(all_failures)} 条（宏观 {len(failures)} / 逐行 {len(row_failures)}）:")
        for item in all_failures[:30]:
            print("  - " + item)
        if len(all_failures) > 30:
            print(f"  ... 另有 {len(all_failures) - 30} 条")
        return 1
    print("[format] 通过")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="格式探测真值门禁")
    parser.add_argument("--results", required=True, help="media_corpus_validate 产出的 results.tsv")
    parser.add_argument("--index", default=DEFAULT_INDEX, help="语料 INDEX.tsv（container 列是真值）")
    parser.add_argument("--root", default=DEFAULT_ROOT, help="语料根目录")
    parser.add_argument("--whitelist", default=str(DEFAULT_WHITELIST),
                        help="例外白名单 JSON，形如 {\"exceptions\": {\"<相对路径>\": \"原因\"}}")
    args = parser.parse_args()
    wl = Path(args.whitelist) if args.whitelist else None
    return check(Path(args.results), Path(args.index), args.root, wl)


if __name__ == "__main__":
    sys.exit(main())
