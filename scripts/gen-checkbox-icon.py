#!/usr/bin/env python
"""生成 QCheckBox::indicator 用的勾选图标 (32x32 RGBA PNG)。

纯标准库实现 (zlib + struct)，不依赖 PIL。
- 4x 超采样绘制后盒子降采样到 32x32，保证抗锯齿。
- 常规勾为深色 #0D1117：QSS 里指示器选中底色是 #58A6FF，深色勾对比度约 7:1
  (白勾只有 2.6:1)，可辨识性更好。
- 禁用勾为浅灰 #8B949E：QSS 里禁用选中底色是暗灰 #21262D，浅勾对比度约 4.6:1。
- 勾在 32x32 画布里留 ~25% 内边距：QSS 把 image 画满 16x16 的 indicator，
  实际可见勾约 12x12、粗约 2.5px，与全局 16px 指示框匹配。
"""
import struct
import zlib

S = 32          # 输出尺寸
SS = 4          # 超采样倍数
W = S * SS      # 工作分辨率

# 勾的折线（工作坐标系，端点圆头）
P0 = (int(W * 0.21), int(W * 0.52))
P1 = (int(W * 0.40), int(W * 0.71))
P2 = (int(W * 0.78), int(W * 0.24))
THICK = int(W * 0.13)   # 线宽半径

VARIANTS = {
    "checkbox_checked.png": (0x0D, 0x11, 0x17),          # 常规：深色勾
    "checkbox_checked_disabled.png": (0x8B, 0x94, 0x9E),  # 禁用：浅灰勾
}


def dist_point_seg(px, py, ax, ay, bx, by):
    abx, aby = bx - ax, by - ay
    apx, apy = px - ax, py - ay
    denom = abx * abx + aby * aby
    if denom == 0:
        return (apx * apx + apy * apy) ** 0.5
    t = max(0.0, min(1.0, (apx * abx + apy * aby) / denom))
    cx, cy = ax + t * abx, ay + t * aby
    return ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5


def build_alpha():
    alpha = [[0.0] * W for _ in range(W)]
    for y in range(W):
        for x in range(W):
            d = min(dist_point_seg(x, y, *P0, *P1), dist_point_seg(x, y, *P1, *P2))
            alpha[y][x] = max(0.0, min(1.0, THICK - d + 0.5))
    return alpha


def box_downsample(alpha):
    out = [[0.0] * S for _ in range(S)]
    for y in range(S):
        for x in range(S):
            acc = 0.0
            for dy in range(SS):
                for dx in range(SS):
                    acc += alpha[y * SS + dy][x * SS + dx]
            out[y][x] = acc / (SS * SS)
    return out


def write_png(path, rgb, coverage):
    raw = bytearray()
    for y in range(S):
        raw.append(0)  # filter type: none
        for x in range(S):
            a = int(round(coverage[y][x] * 255))
            raw += bytes((*rgb, a))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print(f"wrote {path} ({len(png)} bytes)")


if __name__ == "__main__":
    alpha = build_alpha()
    coverage = box_downsample(alpha)
    # 简单校验：中心附近应有覆盖，四角应为 0
    assert coverage[S // 2][S // 2] > 0.3, "check center should be covered"
    assert coverage[1][1] == 0.0 and coverage[S - 2][S - 2] == 0.0
    for name, rgb in VARIANTS.items():
        write_png(f"ui/theme/assets/{name}", rgb, coverage)
