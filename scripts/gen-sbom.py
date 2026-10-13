#!/usr/bin/env python3
"""生成 VideoEye 分发物的 SBOM（CycloneDX 1.5 JSON）。

为什么要有这个脚本（而不是手写一份清单）:
    手写的第三方清单一定会和真实构建漂移 —— FFmpeg 换个包、Qt 升个版本、vcpkg 加个
    依赖，清单里那条就变成错的。而错的清单比没有更糟：合规审查会照着它去做判断。
    所以这里所有条目都从**构建实际使用的输入**里读：

      · FFmpeg   ← cmake/ffmpeg-version.json（版本 / URL / SHA256 / 许可证 / release tag）
      · Qt6      ← 命令行传入（CI 从 CMake 摘要里取）或 vcpkg_installed 里探测
      · vcpkg 包 ← vcpkg_installed/<triplet>/share/*/vcpkg.spdx.json（vcpkg 自己生成的）
      · VideoEye ← git describe（拿不到就用 CMake 给的版本）

只依赖标准库，任何机器上都能跑。

用法:
    python scripts/gen-sbom.py --version 2.0.0 --output dist/sbom/videoeye.cdx.json
    python scripts/gen-sbom.py --version 2.0.0 --qt-version 6.8.3 --vcpkg-dir vcpkg_installed
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# CycloneDX 的许可证字段优先用 SPDX ID；拿不到就退到名字，绝不猜。
FFMPEG_PURL_FALLBACK = "pkg:generic/ffmpeg@{version}"
QT_PURL = "pkg:qt/qt@{version}"
QT_LICENSE = "LGPL-3.0-only"
VIDEOEYE_LICENSE = "MIT"


def _read_json(path: str):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def git_commit() -> str:
    """当前提交的 sha。CI 里拿不到（浅克隆/无 tag）就返回空串，不硬造。"""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=15,
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return ""


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    try:
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return ""


def ffmpeg_component(cfg_path: str) -> dict:
    """FFmpeg 条目：版本/许可证/来源全部来自锁定文件。"""
    cfg = _read_json(cfg_path) or {}
    windows = cfg.get("windows", {}) or {}
    license_block = cfg.get("license", {}) or {}

    version = str(cfg.get("version", "")).strip()
    spdx = str(license_block.get("spdx", "")).strip()
    name = str(license_block.get("name", "")).strip()
    source_url = str(license_block.get("sourceUrl", "")).strip()
    build_id = str(windows.get("buildId", "")).strip()
    release_tag = str(windows.get("releaseTag", "")).strip()

    licenses = []
    if spdx:
        licenses.append({"license": {"id": spdx}})
    elif name:
        licenses.append({"license": {"name": name}})
    else:
        licenses.append({"license": {"name": "unknown（cmake/ffmpeg-version.json 未声明）"}})

    comp = {
        "type": "library",
        "name": "FFmpeg",
        "version": version or "unknown",
        "description": "音视频解码 / 封装 / 像素与重采样（libavcodec, libavformat, libavutil, libswscale, libswresample）",
        "licenses": licenses,
        "purl": FFMPEG_PURL_FALLBACK.format(version=version or "unknown"),
        "externalReferences": [],
    }
    if source_url:
        comp["externalReferences"].append(
            {"type": "distribution", "url": source_url, "comment": "FFmpeg 源码获取方式（LGPL 对应源码义务）"}
        )
    if windows.get("url"):
        comp["externalReferences"].append(
            {"type": "distribution", "url": windows["url"], "comment": "本次构建使用的预编译包"}
        )
    # 这两项让"这份二进制到底由哪次构建产出"可被追溯
    hashes = []
    if windows.get("sha256"):
        hashes.append({"alg": "SHA-256", "content": str(windows["sha256"]).lower()})
    if hashes:
        comp["hashes"] = hashes
    props = []
    if build_id:
        props.append({"name": "videoeye:ffmpeg.buildId", "value": build_id})
    if release_tag:
        props.append({"name": "videoeye:ffmpeg.releaseTag", "value": release_tag})
    if windows.get("provider"):
        props.append({"name": "videoeye:ffmpeg.provider", "value": str(windows["provider"])})
    if props:
        comp["properties"] = props
    return comp


def detect_qt_version(vcpkg_dir: str) -> str:
    """从 vcpkg 的 Qt6 包目录里读版本号（vcpkg.spdx.json 或目录名）。"""
    if not vcpkg_dir or not os.path.isdir(vcpkg_dir):
        return ""
    best = ""
    for root, _dirs, files in os.walk(vcpkg_dir):
        for fname in files:
            if fname != "vcpkg.spdx.json":
                continue
            data = _read_json(os.path.join(root, fname))
            if not data:
                continue
            for pkg in (data.get("packages") or []):
                nm = str(pkg.get("name", ""))
                ver = str(pkg.get("versionInfo", "") or pkg.get("version", ""))
                if "qtbase" in nm.lower() and ver:
                    if ver > best:
                        best = ver
    return best


def vcpkg_components(vcpkg_dir: str, limit: int = 200) -> list:
    """把 vcpkg 自己生成的 SPDX 包信息转成 CycloneDX 条目。

    只收录有版本号的包，且去重（同一个包会在多个 port 的 spdx 里重复出现）。
    """
    if not vcpkg_dir or not os.path.isdir(vcpkg_dir):
        return []
    seen = {}
    for root, _dirs, files in os.walk(vcpkg_dir):
        for fname in files:
            if fname != "vcpkg.spdx.json":
                continue
            data = _read_json(os.path.join(root, fname))
            if not data:
                continue
            for pkg in (data.get("packages") or []):
                nm = str(pkg.get("name", "")).strip()
                ver = str(pkg.get("versionInfo", "") or pkg.get("version", "")).strip()
                if not nm or not ver:
                    continue
                key = (nm.lower(), ver)
                if key in seen:
                    continue
                lic = str(pkg.get("licenseConcluded") or pkg.get("licenseDeclared") or "").strip()
                comp = {
                    "type": "library",
                    "name": nm,
                    "version": ver,
                    "purl": "pkg:vcpkg/{}@{}".format(nm.lower(), ver),
                }
                if lic and lic.upper() not in ("NOASSERTION", "NONE"):
                    comp["licenses"] = [{"license": {"name": lic}}]
                seen[key] = comp
                if len(seen) >= limit:
                    return list(seen.values())
    return list(seen.values())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="生成 VideoEye 分发物的 CycloneDX SBOM")
    ap.add_argument("--version", required=True, help="VideoEye 版本号（如 2.0.0）")
    ap.add_argument("--output", required=True, help="输出 JSON 路径")
    ap.add_argument(
        "--ffmpeg-config",
        default=os.path.join(REPO_ROOT, "cmake", "ffmpeg-version.json"),
        help="FFmpeg 锁定文件路径",
    )
    ap.add_argument("--qt-version", default="", help="Qt6 版本；留空则从 vcpkg_installed 探测")
    ap.add_argument("--vcpkg-dir", default="", help="vcpkg_installed 目录（用于探测 Qt 版本与依赖清单）")
    ap.add_argument("--platform", default="", help="目标平台标识，如 windows-x86_64")
    args = ap.parse_args(argv)

    qt_version = args.qt_version.strip() or detect_qt_version(args.vcpkg_dir)

    components = []

    # 1) 本项目自身
    ve = {
        "type": "application",
        "name": "VideoEye",
        "version": args.version,
        "description": "视频码流分析工具",
        "licenses": [{"license": {"id": VIDEOEYE_LICENSE}}],
        "purl": "pkg:github/HZer0K/VideoEye@{}".format(args.version),
    }
    commit = git_commit()
    if commit:
        ve["properties"] = [{"name": "videoeye:gitCommit", "value": commit}]
    components.append(ve)

    # 2) FFmpeg（随包分发的二进制，义务最重的一条）
    components.append(ffmpeg_component(args.ffmpeg_config))

    # 3) Qt6
    if qt_version:
        qt = {
            "type": "library",
            "name": "Qt6",
            "version": qt_version,
            "description": "GUI 框架（本项目只用 Qt Widgets）",
            "licenses": [{"license": {"id": QT_LICENSE}}],
            "purl": QT_PURL.format(version=qt_version),
            "externalReferences": [
                {"type": "distribution", "url": "https://www.qt.io/download-open-source"}
            ],
        }
        components.append(qt)
    else:
        print("[warn] 没拿到 Qt6 版本，SBOM 里会缺这一条", file=sys.stderr)

    # 4) vcpkg 依赖（Windows 静态/动态链进来的第三方库）
    deps = vcpkg_components(args.vcpkg_dir)
    components.extend(deps)

    serial = "urn:uuid:videoeye-{}-{}".format(args.version, datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S"))
    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": serial,
        "version": 1,
        "metadata": {
            "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "component": {
                "type": "application",
                "name": "VideoEye",
                "version": args.version,
            },
            "properties": [],
        },
        "components": components,
    }
    if args.platform:
        sbom["metadata"]["properties"].append({"name": "videoeye:platform", "value": args.platform})
    if commit:
        sbom["metadata"]["properties"].append({"name": "videoeye:gitCommit", "value": commit})

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as fh:
        json.dump(sbom, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    print("SBOM 已生成: {}".format(args.output))
    print("  组件数: {}（VideoEye + FFmpeg + Qt6 + {} 个 vcpkg 依赖）".format(len(components), len(deps)))
    print("  sha256: {}".format(sha256_of(args.output)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
