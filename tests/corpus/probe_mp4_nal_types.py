#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""探查 MP4 媒体样本里到底有没有带内参数集（SPS/PPS）。

用途：回答一个决定实现形态的问题 —— MP4/MOV 的参数集只写在 avcC 里，
编码器（libx264 在 AV_CODEC_FLAG_GLOBAL_HEADER 下关掉 b_repeat_headers）
**不会**在码流里重复它们。那么任何"只扫媒体包"的解析器（POC 追踪、
参数集变化追踪）在这类文件上都会看到"零个 SPS"。

这个脚本按 ISO/IEC 14496-12 直接走 stsd(avcC) + stsz/stco/co64 + mdat，
把每个 sample 的长度前缀 NAL 类型逐个列出来 —— 结论是字节级的，不靠猜。

用法：python probe_mp4_nal_types.py <file.mp4>
"""

import struct
import sys


def iter_boxes(data, start, end):
    pos = start
    while pos + 8 <= end:
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        typ = data[pos + 4 : pos + 8]
        header = 8
        if size == 1:
            if pos + 16 > end:
                return
            size = struct.unpack(">Q", data[pos + 8 : pos + 16])[0]
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or pos + size > end:
            return
        yield typ, pos + header, pos + size
        pos += size


def find_box(data, start, end, path):
    """按点分路径找 box，返回 (payload_start, payload_end) 或 None"""
    node_start, node_end = start, end
    for want in path.split("."):
        found = None
        for typ, ps, pe in iter_boxes(data, node_start, node_end):
            if typ == want.encode("latin-1"):
                found = (ps, pe)
                break
        if found is None:
            return None
        node_start, node_end = found
        # full box 的 payload 开头是 version(1)+flags(3)，全都要跳过；
        # stsd 在这之后还有 4 字节 entry_count，然后才是第一个子 box（avcC / avc1 ...）。
        if want in ("stsd", "stsz", "stco", "co64"):
            node_start += 4
        if want == "stsd":
            node_start += 4
    return node_start, node_end


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()

    moov = find_box(data, 0, len(data), "moov")
    mdat = find_box(data, 0, len(data), "mdat")
    if moov is None or mdat is None:
        print("不是可解析的 MP4（缺 moov 或 mdat）")
        return 1

    # avcC 里的 lengthSizeMinusOne
    avcc = find_box(data, moov[0], moov[1], "trak.mdia.minf.stbl.stsd.avcC")
    length_size = 4
    if avcc is not None:
        payload = data[avcc[0] : avcc[1]]
        if len(payload) >= 5:
            length_size = (payload[4] & 0x03) + 1
    print("avcC lengthSizeMinusOne -> prefix=%d 字节" % length_size)

    stsz = find_box(data, moov[0], moov[1], "trak.mdia.minf.stbl.stsz")
    stco = find_box(data, moov[0], moov[1], "trak.mdia.minf.stbl.stco")
    if stsz is None:
        print("没有 stsz")
        return 1
    p = stsz[0]
    sample_size = struct.unpack(">I", data[p : p + 4])[0]
    sample_count = struct.unpack(">I", data[p + 4 : p + 8])[0]
    sizes = []
    if sample_size == 0:
        for i in range(sample_count):
            sizes.append(struct.unpack(">I", data[p + 8 + 4 * i : p + 12 + 4 * i])[0])
    else:
        sizes = [sample_size] * sample_count

    offsets = []
    if stco is not None:
        q = stco[0]  # find_box 已跳过 version/flags，这里正对 entry_count
        n = struct.unpack(">I", data[q : q + 4])[0]
        for i in range(n):
            offsets.append(struct.unpack(">I", data[q + 4 + 4 * i : q + 8 + 4 * i])[0])
    else:
        co64 = find_box(data, moov[0], moov[1], "trak.mdia.minf.stbl.co64")
        if co64 is None:
            print("没有 stco/co64")
            return 1
        q = co64[0]  # 同上，正对 entry_count
        n = struct.unpack(">I", data[q : q + 4])[0]
        for i in range(n):
            offsets.append(struct.unpack(">Q", data[q + 4 + 8 * i : q + 12 + 8 * i])[0])

    # 样本在 chunk 内是连续存放的，而 ffmpeg 产出的渐进式 MP4 只有一个 chunk，
    # 所以直接按 stsz 的顺序从第一个 chunk 偏移开始顺序走完即可 ——
    # 不需要 stsc（samples_per_chunk），换来的是脚本短一半且结论不弱化。
    mdat_start, mdat_end = mdat
    counts = {}
    sample_nal_types = []
    cursor = offsets[0] if offsets else mdat_start
    for size in sizes:
        off = cursor
        cursor += size
        pos = off
        limit = min(off + size, mdat_end)
        types = []
        while pos + length_size <= limit:
            if length_size == 4:
                nalu_size = struct.unpack(">I", data[pos : pos + 4])[0]
            elif length_size == 2:
                nalu_size = struct.unpack(">H", data[pos : pos + 2])[0]
            else:
                nalu_size = data[pos]
            pos += length_size
            if nalu_size == 0 or pos + nalu_size > limit:
                break
            nal_type = data[pos] & 0x1F
            types.append(nal_type)
            counts[nal_type] = counts.get(nal_type, 0) + 1
            pos += nalu_size
        sample_nal_types.append(types)

    names = {1: "slice(非IDR)", 5: "slice(IDR)", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD"}
    print("样本数 %d，NAL 类型统计（样本数据里）:" % len(sample_nal_types))
    for t in sorted(counts):
        print("  type %2d %-14s x%d" % (t, names.get(t, "?"), counts[t]))
    print()
    print("前 5 个样本的 NAL 组成:", sample_nal_types[:5])
    inband_sps = counts.get(7, 0)
    print()
    print("结论: 带内 SPS 出现 %d 次 —— %s"
          % (inband_sps, "码流里带参数集" if inband_sps else "参数集只在 avcC 里，码流里没有"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
