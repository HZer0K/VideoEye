#!/usr/bin/env python3
"""负样本诊断门禁 —— 让「打不开」这件事能说清是哪一种打不开。

背景
----
INDEX.tsv 的 ``defect_heuristic`` 列标了 175 条异常样本的具体缺陷类型，但此前
所有门禁只消费 ``parse`` 列，而且用的是一条很宽的「误收 ≤ 60 条」数量上限
（实测 49 条，永远绿）。也就是说：产品对零字节文件、不存在的路径、纯文本 XML
一律回一句「FFmpeg 无法打开文件」，语料门禁一个字都不会报。

本门禁不把启发式标注当成「必须失败」的硬真值 —— 那个标注本身是启发式的：
``corrupt-structure`` 30 条里 15 条 parse=ok，``decode-artifact`` 39 条全是容器
正常的画面伪影。硬套只会制造假红。这里钉的是**结构性不变量**：

1. 失败原因必须结构化、可区分（守恒 + 类别多样性）；
2. 零字节必须归 ``empty_file``，非媒体文件不得归成文件级错误（反向守护）；
3. 报「结构有效」时必须有取证事实，不能是「无事实的成功结论」；
4. 正常参考样本不得被判失败（假阳性守护）；
5. 覆盖守护：缺列 / 空表 / 分组没事实 / 白名单失效一律判红。

白名单只豁免「按缺陷分组的语义判据」，绝不豁免守恒、多样性与覆盖守护 ——
否则一行白名单就能把整个门禁架空。
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_WHITELIST = REPO_ROOT / "tests" / "corpus" / "defect_gate_whitelist.json"
DEFAULT_INDEX = "D:/Coding/Test/media/INDEX.tsv"
DEFAULT_ROOT = "D:/Coding/Test/media"

# 与 core/domain/model/ContainerStructureInfo.h 的 ContainerFailureKindName 一一对应。
KNOWN_KINDS = {
    "none", "empty_path", "not_found", "is_directory", "empty_file",
    "allocation_failed", "open_failed", "stream_info_failed",
    "cancelled", "parse_failed",
}

# 必须真实出现在 INDEX 里的分组：缺一组说明语料或标注被换掉了，门禁不能空转。
REQUIRED_GROUPS = {
    "zero-byte", "metadata", "corrupt-structure", "malformed",
    "decode-artifact", "valid-reference",
}

# 报「有效」时必须有取证事实的分组（启发式标注为损坏/畸形/截断）。
DEFECT_GROUPS = {"corrupt-structure", "malformed", "truncated", "fuzz", "unclassified"}

# 不该被结构层判失败的分组。
HEALTHY_GROUPS = {"valid-reference", "decode-artifact"}

MIN_ABNORMAL_ROWS = 100
MIN_FAILURE_KINDS = 3


def rel_key(path: str, root: str) -> str:
    """把绝对路径 / 反斜杠路径统一成 INDEX 里的相对键。"""
    norm = path.replace("\\", "/")
    root_norm = root.replace("\\", "/").rstrip("/")
    if root_norm and norm.startswith(root_norm + "/"):
        norm = norm[len(root_norm) + 1:]
    return norm.lstrip("/").lower()


def load_tsv(path: Path) -> list[dict]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def load_whitelist(path: Path | None) -> dict:
    if not path:
        return {}
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return data.get("exceptions", {}) if isinstance(data, dict) else {}


def check(results: Path, index: Path, root: str, whitelist_path: Path | None) -> int:
    failures: list[str] = []
    whitelist = load_whitelist(whitelist_path)
    whitelist_hits: set[str] = set()

    rows = load_tsv(results)
    if not rows:
        print("[defect] 结果表为空 —— 门禁不能空转")
        return 1
    if "struct_failure_kind" not in rows[0]:
        print("[defect] 结果表缺 struct_failure_kind 列（需重跑 media_corpus_validate）")
        return 1

    index_rows = load_tsv(index)
    defects = {}
    for item in index_rows:
        key = rel_key(item.get("path", ""), root)
        if key:
            defects[key] = item.get("defect_heuristic", "") or "-"

    by_group: dict[str, list[dict]] = defaultdict(list)
    abnormal = 0
    for row in rows:
        key = rel_key(row.get("path", ""), root)
        group = defects.get(key, "-")
        if group != "-":
            abnormal += 1
            by_group[group].append(row)

    # ---- 1. 覆盖守护 ----------------------------------------------------
    if abnormal < MIN_ABNORMAL_ROWS:
        failures.append(f"INDEX 里带缺陷标注的样本只有 {abnormal} 条 < {MIN_ABNORMAL_ROWS}，门禁空转")
    missing_groups = REQUIRED_GROUPS - set(by_group)
    if missing_groups:
        failures.append(f"缺失必需缺陷分组: {sorted(missing_groups)}")

    valid_rows = [r for r in rows if r.get("struct_valid") == "1"]
    invalid_rows = [r for r in rows if r.get("struct_valid") == "0"]
    if not valid_rows or not invalid_rows:
        failures.append(f"结果表没有同时包含成功({len(valid_rows)})与失败({len(invalid_rows)})样本，守恒判据空转")

    # ---- 2. 列完整性与守恒（白名单不可豁免）------------------------------
    bad_kind_count = 0
    for row in rows:
        kind = (row.get("struct_failure_kind") or "").strip()
        if not kind:
            bad_kind_count += 1
            continue
        if kind not in KNOWN_KINDS:
            failures.append(f"未知失败类别 {kind!r}: {row.get('path')}")
            continue
        valid = row.get("struct_valid") == "1"
        if valid and kind != "none":
            failures.append(f"结构有效却带失败类别 {kind}: {row.get('path')}")
        if not valid and kind == "none":
            failures.append(f"结构失败却没有失败类别: {row.get('path')}")
        if not valid and not (row.get("struct_error") or "").strip():
            failures.append(f"结构失败却没有错误说明: {row.get('path')}")
    if bad_kind_count:
        failures.append(f"{bad_kind_count} 行 struct_failure_kind 为空（失败原因没落盘）")

    # ---- 3. 类别多样性：直接杀死「所有失败压成一句」的回归 ----------------
    failure_kinds = Counter((r.get("struct_failure_kind") or "").strip() for r in invalid_rows)
    distinct = {k for k in failure_kinds if k}
    if len(distinct) < MIN_FAILURE_KINDS:
        failures.append(f"失败类别只有 {len(distinct)} 种 < {MIN_FAILURE_KINDS}: {sorted(distinct)}")
    if "empty_file" not in failure_kinds:
        failures.append("没有任何 empty_file：零字节样本没被识别出来")
    if "open_failed" not in failure_kinds and "parse_failed" not in failure_kinds:
        failures.append("没有 open_failed / parse_failed：非媒体文件没被识别出来")

    # ---- 4. 分组语义判据（白名单可豁免）--------------------------------
    def exempt(row: dict) -> bool:
        key = rel_key(row.get("path", ""), root)
        if key in whitelist:
            whitelist_hits.add(key)
            return True
        return False

    def group_failure(group: str, row: dict, message: str) -> None:
        if exempt(row):
            return
        failures.append(f"[{group}] {message}: {row.get('path')}")

    for row in by_group.get("zero-byte", []):
        if (row.get("struct_failure_kind") or "").strip() != "empty_file":
            group_failure("zero-byte", row,
                          f"零字节文件被归成 {(row.get('struct_failure_kind') or '').strip()!r}，应为 empty_file")

    for row in by_group.get("metadata", []):
        kind = (row.get("struct_failure_kind") or "").strip()
        if kind in {"empty_file", "not_found", "is_directory", "empty_path", "none"}:
            group_failure("metadata", row,
                          f"非媒体文件被归成文件级错误 {kind!r}（应为 open_failed/parse_failed）")

    for group in HEALTHY_GROUPS:
        for row in by_group.get(group, []):
            if row.get("struct_valid") != "1":
                group_failure(group, row, "健康参考样本被结构层判为失败（假阳性）")

    for group in DEFECT_GROUPS:
        for row in by_group.get(group, []):
            if row.get("struct_valid") == "1" and not (row.get("struct_summary") or "").strip():
                group_failure(group, row, "报结构有效却没有任何取证事实（无事实的成功结论）")

    # ---- 5. 白名单必须被真实差异命中 ------------------------------------
    stale = set(whitelist) - whitelist_hits
    if stale:
        failures.append(f"白名单失效（这些行本来就没违反判据）: {sorted(stale)[:8]}")

    # ---- 输出 -----------------------------------------------------------
    print(f"[defect] 结果 {len(rows)} 行，异常标注 {abnormal} 行，"
          f"成功 {len(valid_rows)} / 失败 {len(invalid_rows)}")
    print("[defect] 失败类别: " + ", ".join(f"{k}={v}" for k, v in sorted(failure_kinds.items())))
    print("[defect] 分组: " + ", ".join(f"{g}={len(v)}" for g, v in sorted(by_group.items())))
    print(f"[defect] 白名单命中 {len(whitelist_hits)}/{len(whitelist)}")

    if failures:
        print(f"[defect] 判红 {len(failures)} 条:")
        for item in failures[:30]:
            print("  - " + item)
        if len(failures) > 30:
            print(f"  ... 另有 {len(failures) - 30} 条")
        return 1
    print("[defect] 通过")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="负样本诊断门禁")
    parser.add_argument("--results", required=True, help="media_corpus_validate 产出的 results.tsv")
    parser.add_argument("--index", default=DEFAULT_INDEX, help="语料 INDEX.tsv")
    parser.add_argument("--root", default=DEFAULT_ROOT, help="语料根目录（用于把绝对路径剥成相对键）")
    parser.add_argument("--whitelist", default=str(DEFAULT_WHITELIST),
                        help="例外白名单 JSON，形如 {\"exceptions\": {\"<相对路径>\": \"原因\"}}")
    args = parser.parse_args()
    wl = Path(args.whitelist) if args.whitelist else None
    return check(Path(args.results), Path(args.index), args.root, wl)


if __name__ == "__main__":
    sys.exit(main())
