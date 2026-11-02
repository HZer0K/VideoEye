#!/usr/bin/env python3
"""从矢量源生成应用图标（PNG 多尺寸 + Windows ICO）。

图标唯一真源是 ``ui/theme/assets/app_icon.svg``。本脚本用浏览器无头渲染把它栅格化，
理由是不想为了渲染 SVG 再往 vcpkg 里加依赖（cairo / QtSvg 都不便宜），而浏览器渲染
的渐变、贝塞尔与抗锯齿都是矢量级精确的。

小尺寸（<=24px）切到一份"加粗版"几何：12/256 的描边缩到 16px 只剩 0.75px，
直接缩小会糊成一层淡蓝，图标在任务栏里就散架了。

用法::

    python scripts/gen_icon.py            # 生成全部产物
    python scripts/gen_icon.py --check    # 只校验产物是否比矢量源旧（CI 用）

产物::

    ui/theme/assets/app_icon_{16,24,32,48,64,128,256,512}.png
    ui/theme/assets/app_icon.ico          # Windows 可执行文件图标
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent
ASSETS = REPO_ROOT / "ui" / "theme" / "assets"
SVG_SOURCE = ASSETS / "app_icon.svg"

PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)

# 栅格化的统一画布。必须明显大于 Chromium 的最小窗口尺寸（实测约 500px），
# 否则浏览器会把窗口抬大、而截图仍按 window-size 裁 —— 图形只剩左上角一块。
MASTER = 1536

# ICO 帧：小尺寸走未压缩 DIB（老工具也认），256 走 PNG（省 250 KiB）。
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
ICO_PNG_SIZES = {256}

# 这么小的尺寸需要更粗的描边才立得住（见模块注释）。
SMALL_SIZE_LIMIT = 24


def small_variant(svg: str) -> str:
    """小尺寸几何：描边加粗、三角放大，去掉只在放大后才看得见的装饰。"""
    replacements = (
        ('stroke-width="12"', 'stroke-width="21"'),
        ("M114 109 L114 147 L147 128 Z", "M112 104 L112 152 L153 128 Z"),
        ('r="36" fill="url(#iris)"', 'r="38" fill="url(#iris)"'),
        (
            'r="36" fill="none" stroke="#0D1117" stroke-opacity="0.5" stroke-width="3"',
            'r="38" fill="none" stroke="#0D1117" stroke-opacity="0.35" stroke-width="2"',
        ),
    )
    for old, new in replacements:
        if old not in svg:
            raise SystemExit(f"gen_icon: 小尺寸变体的锚点不存在，SVG 被改过？ {old!r}")
        svg = svg.replace(old, new)
    # 光晕与内描边在小尺寸上只会变成脏边
    svg = svg.replace('<circle cx="128" cy="128" r="66" fill="url(#glow)"/>', "")
    svg = svg.replace(
        '<rect x="9" y="9" width="238" height="238" rx="55" fill="none"\n'
        '        stroke="url(#rim)" stroke-width="2"/>',
        "",
    )
    return svg


def find_browser() -> Path:
    candidates = [
        Path(os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)"))
        / "Microsoft/Edge/Application/msedge.exe",
        Path(os.environ.get("PROGRAMFILES", r"C:\Program Files"))
        / "Microsoft/Edge/Application/msedge.exe",
        Path(os.environ.get("PROGRAMFILES", r"C:\Program Files"))
        / "Google/Chrome/Application/chrome.exe",
        Path(os.environ.get("LOCALAPPDATA", "")) / "Google/Chrome/Application/chrome.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    found = (
        shutil.which("chromium")
        or shutil.which("google-chrome")
        or shutil.which("chromium-browser")
    )
    if found:
        return Path(found)
    raise SystemExit(
        "gen_icon: 找不到 Edge / Chrome。SVG 栅格化需要 Chromium 内核浏览器；\n"
        "           已生成的 PNG/ICO 仍可直接使用，只是换机器后无法重新生成。"
    )


def render_master(browser: Path, svg_path: Path, out_png: Path) -> Image.Image:
    """把 SVG 栅格化到 MASTER×MASTER（矢量铺满视口，整张截图即完整图形）。"""
    with tempfile.TemporaryDirectory(prefix="videoeye-icon-") as tmp:
        cmd = [
            str(browser),
            "--headless=new",
            "--disable-gpu",
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-extensions",
            "--hide-scrollbars",
            "--force-device-scale-factor=1",
            f"--user-data-dir={Path(tmp) / 'profile'}",
            "--default-background-color=00000000",
            f"--window-size={MASTER},{MASTER}",
            f"--screenshot={out_png}",
            svg_path.as_uri(),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        if not out_png.is_file():
            raise SystemExit(f"gen_icon: 浏览器渲染失败\n{result.stderr[-2000:]}")

    image = Image.open(out_png).convert("RGBA")
    # 尺寸断言：曾经因为窗口被 clamp，截图比预期小、图形只剩左上角，
    # 而缩放置信了截图尺寸，于是 16/24/32 全成了"放大的局部"。
    if image.size != (MASTER, MASTER):
        raise SystemExit(
            f"gen_icon: 浏览器输出尺寸异常 {image.size} != {(MASTER, MASTER)}；"
            "换个大一点的 --window-size 或降 MASTER"
        )
    return image


def dib_frame(image: Image.Image) -> bytes:
    """32bpp 未压缩 DIB 帧（含 AND 掩码），ICO 里小尺寸用这个最兼容。"""
    size = image.width
    header = struct.pack(
        "<IiiHHIIiiII",
        40,  # biSize
        size,  # biWidth
        size * 2,  # biHeight = 颜色 + 掩码
        1,  # biPlanes
        32,  # biBitCount
        0,  # biCompression = BI_RGB
        size * size * 4,
        0,
        0,
        0,
        0,
    )
    pixels = image.load()
    rows = bytearray()
    for y in range(size - 1, -1, -1):  # DIB 自底向上
        for x in range(size):
            r, g, b, a = pixels[x, y]
            rows += bytes((b, g, r, a))

    # AND 掩码：每行按 4 字节对齐；全透明像素置位（其余交给 alpha 通道）
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray(stride)
        for x in range(size):
            if pixels[x, y][3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return header + bytes(rows) + bytes(mask)


def write_ico(frames: dict[int, Image.Image], out_path: Path) -> None:
    """手写 ICO：Pillow 的 ICO 保存器只能从单张图缩放，拿不到逐尺寸的 hinting。"""
    entries = []
    payload = bytearray()
    offset = 6 + 16 * len(frames)
    for size in sorted(frames):
        image = frames[size]
        if size in ICO_PNG_SIZES:
            with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
                tmp_name = tmp.name
            image.save(tmp_name, format="PNG", optimize=True)
            data = Path(tmp_name).read_bytes()
            os.unlink(tmp_name)
        else:
            data = dib_frame(image)
        entries.append(
            struct.pack(
                "<BBBBHHII",
                0 if size >= 256 else size,
                0 if size >= 256 else size,
                0,
                0,
                1,
                32,
                len(data),
                offset,
            )
        )
        payload += data
        offset += len(data)
    out_path.write_bytes(struct.pack("<HHH", 0, 1, len(frames)) + b"".join(entries) + bytes(payload))


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 VideoEye 应用图标")
    parser.add_argument("--check", action="store_true", help="只检查产物是否比矢量源旧")
    args = parser.parse_args()

    if not SVG_SOURCE.is_file():
        raise SystemExit(f"gen_icon: 矢量源不存在: {SVG_SOURCE}")

    products = [ASSETS / f"app_icon_{size}.png" for size in PNG_SIZES] + [ASSETS / "app_icon.ico"]
    if args.check:
        source_mtime = SVG_SOURCE.stat().st_mtime
        stale = [p.name for p in products if not p.is_file() or p.stat().st_mtime < source_mtime]
        if stale:
            print(f"gen_icon: 产物与矢量源不同步，请运行 python scripts/gen_icon.py: {stale}")
            return 1
        print("gen_icon: 产物与矢量源同步")
        return 0

    svg = SVG_SOURCE.read_text(encoding="utf-8")
    browser = find_browser()
    ASSETS.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="videoeye-icon-src-") as tmp:
        tmp_path = Path(tmp)
        big_svg = tmp_path / "big.svg"
        small_svg = tmp_path / "small.svg"
        big_svg.write_text(svg, encoding="utf-8")
        small_svg.write_text(small_variant(svg), encoding="utf-8")

        masters = {
            "big": render_master(browser, big_svg, tmp_path / "master_big.png"),
            "small": render_master(browser, small_svg, tmp_path / "master_small.png"),
        }

        frames: dict[int, Image.Image] = {}
        for size in PNG_SIZES:
            master = masters["small" if size <= SMALL_SIZE_LIMIT else "big"]
            image = master.resize((size, size), Image.LANCZOS)
            image.save(ASSETS / f"app_icon_{size}.png", format="PNG", optimize=True)
            frames[size] = image

    write_ico({size: frames[size] for size in ICO_SIZES}, ASSETS / "app_icon.ico")

    total = sum(p.stat().st_size for p in products if p.is_file())
    print(f"gen_icon: 已生成 {len(PNG_SIZES)} 个 PNG + 1 个 ICO，合计 {total / 1024:.1f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
