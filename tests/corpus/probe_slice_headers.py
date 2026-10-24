#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐样本解 MP4 里 H.264 slice_header，产出"帧级 POC 追踪"的字节级参考输出。

用途：C++ 侧（H264PocTracker）说"扫到 7 帧"，容器侧说"有 40 个视频样本，
每个样本一个 slice NAL"。两边对不上时，需要一份**与被测代码零共享**的第三方
判据来确定谁对。这个脚本就是那份判据：它按 H.264 7.3.2.1.1（SPS）/ 7.3.3
（slice_header）/ 8.2.1（POC 推导）从 avcC + mdat 直接把每个样本的
first_mb_in_slice / slice_type / frame_num / poc_lsb 解出来。

用法：python probe_slice_headers.py <file.mp4>
"""

import struct
import sys


# ---------------------------------------------------------------------------
# ISO BMFF 最小 walker
# ---------------------------------------------------------------------------

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
        yield typ.decode("latin1"), pos + header, pos + size
        pos += size


def find_box(data, start, end, path):
    cur_s, cur_e = start, end
    for want in path.split("."):
        got = None
        for name, s, e in iter_boxes(data, cur_s, cur_e):
            if name == want:
                got = (s + 8 if want == "stsd" else s, e)
                break
        if got is None:
            return None
        cur_s, cur_e = got
    return cur_s, cur_e


# ---------------------------------------------------------------------------
# 位读取（RBSP，MSB-first）
# ---------------------------------------------------------------------------

class Bits:
    def __init__(self, data):
        self.d = data
        self.pos = 0  # 位位置

    def bits_left(self):
        return len(self.d) * 8 - self.pos

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.d[self.pos >> 3]
            bit = (byte >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def ue(self):
        zeros = 0
        while self.bits_left() > 0 and self.u(1) == 0:
            zeros += 1
        if zeros == 0:
            return 0
        return (1 << zeros) - 1 + self.u(zeros)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


def unescape(data):
    out = bytearray()
    zeros = 0
    for b in data:
        if zeros >= 2 and b == 0x03:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def scaling_list(br, size):
    last, next_scale = 8, 8
    for j in range(size):
        if next_scale != 0:
            delta = br.se()
            next_scale = (last + delta + 256) % 256
        last = next_scale if next_scale != 0 else last


def parse_sps(rbsp):
    br = Bits(rbsp)
    profile = br.u(8)
    br.u(8)  # constraint flags + reserved
    br.u(8)  # level
    br.ue()  # sps id
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma = br.ue()
        if chroma == 3:
            br.u(1)  # separate_colour_plane_flag
        br.ue()  # bit_depth_luma_minus8
        br.ue()  # bit_depth_chroma_minus8
        br.u(1)  # qpprime_y_zero_transform_bypass_flag
        if br.u(1):  # seq_scaling_matrix_present_flag
            count = 8 if chroma != 3 else 12
            for i in range(count):
                if br.u(1):
                    scaling_list(br, 16 if i < 6 else 64)
    log2_frame_num = br.ue() + 4
    poc_type = br.ue()
    log2_poc_lsb = 0
    if poc_type == 0:
        log2_poc_lsb = br.ue() + 4
    elif poc_type == 1:
        br.u(1)
        br.se()
        br.se()
        n = br.ue()
        for _ in range(n):
            br.se()
    br.ue()  # max_num_ref_frames
    br.u(1)  # gaps_in_frame_num_value_allowed_flag
    br.ue()  # pic_width_in_mbs_minus1
    br.ue()  # pic_height_in_map_units_minus1
    frame_mbs_only = br.u(1)
    return {
        "log2_max_frame_num": log2_frame_num,
        "poc_type": poc_type,
        "log2_max_poc_lsb": log2_poc_lsb,
        "frame_mbs_only": frame_mbs_only,
    }


def parse_pps_bottom_flag(rbsp):
    br = Bits(rbsp)
    br.ue()
    br.ue()
    br.u(1)  # entropy_coding_mode_flag
    return br.u(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()

    moov = find_box(data, 0, len(data), "moov")
    trak = find_box(data, *moov, "trak")
    mdia = find_box(data, *trak, "mdia")
    minf = find_box(data, *mdia, "minf")
    stbl = find_box(data, *minf, "stbl")
    stsd = find_box(data, *stbl, "stsd")
    stsz = find_box(data, *stbl, "stsz")
    stco = find_box(data, *stbl, "stco")
    co64 = find_box(data, *stbl, "co64")
    mdat = find_box(data, 0, len(data), "mdat")

    # stsd 里的第一个条目 → avcC
    avcc = None
    for name, s, e in iter_boxes(data, stsd[0], stsd[1]):
        if name == "avc1" or name == "avc3":
            for n2, s2, e2 in iter_boxes(data, s + 78, e):
                if n2 == "avcC":
                    avcc = (s2, e2)
            break
    if avcc is None:
        print("没找到 avcC，不是 AVC 样本")
        return 1
    s, e = avcc
    prefix = (data[s + 4] & 0x03) + 1
    num_sps = data[s + 5] & 0x1F
    pos = s + 6
    sps_list = []
    for _ in range(num_sps):
        n = struct.unpack(">H", data[pos : pos + 2])[0]
        sps_list.append(data[pos + 2 : pos + 2 + n])
        pos += 2 + n
    num_pps = data[pos]
    pos += 1
    pps_list = []
    for _ in range(num_pps):
        n = struct.unpack(">H", data[pos : pos + 2])[0]
        pps_list.append(data[pos + 2 : pos + 2 + n])
        pos += 2 + n
    print("prefix =", prefix, "字节; SPS", len(sps_list), "PPS", len(pps_list))

    sps = parse_sps(unescape(sps_list[0]))
    bottom_flag = parse_pps_bottom_flag(unescape(pps_list[0]))
    print("SPS:", sps, "PPS bottom_field_pic_order_in_frame_present_flag =", bottom_flag)

    q = stsz[0]
    uniform = struct.unpack(">I", data[q + 4 : q + 8])[0]
    cnt = struct.unpack(">I", data[q + 8 : q + 12])[0]
    sizes = ([uniform] * cnt if uniform else
             [struct.unpack(">I", data[q + 12 + 4 * i : q + 16 + 4 * i])[0] for i in range(cnt)])
    if stco:
        q = stco[0]
        n = struct.unpack(">I", data[q + 4 : q + 8])[0]
        offs = [struct.unpack(">I", data[q + 8 + 4 * i : q + 12 + 4 * i])[0] for i in range(n)]
    else:
        q = co64[0]
        n = struct.unpack(">I", data[q + 4 : q + 8])[0]
        offs = [struct.unpack(">Q", data[q + 8 + 8 * i : q + 16 + 8 * i])[0] for i in range(n)]

    # ❗ stco/co64 给的是 **chunk** 偏移，不是每个 sample 的偏移。
    # 一个 chunk 里连续放着 samples_per_chunk 个 sample，sample 的偏移要自己累加
    # —— 直接拿 chunk 偏移当 sample 偏移，只会解出每个 chunk 的第一个 sample。
    stsc = find_box(data, *stbl, "stsc")
    q = stsc[0]
    entries = struct.unpack(">I", data[q + 4 : q + 8])[0]
    runs = []
    for i in range(entries):
        first_chunk = struct.unpack(">I", data[q + 8 + 12 * i : q + 12 + 12 * i])[0]
        per_chunk = struct.unpack(">I", data[q + 12 + 12 * i : q + 16 + 12 * i])[0]
        runs.append((first_chunk, per_chunk))

    def samples_in_chunk(chunk_index):  # chunk 从 1 开始
        per = 1
        for first_chunk, per_chunk in runs:
            if chunk_index >= first_chunk:
                per = per_chunk
        return per

    sample_offsets = []
    cursor = 0
    for chunk_index, chunk_off in enumerate(offs, start=1):
        for _ in range(samples_in_chunk(chunk_index)):
            if cursor >= len(sizes):
                break
            sample_offsets.append(chunk_off)
            chunk_off += sizes[cursor]
            cursor += 1
    if len(sample_offsets) != len(sizes):
        print("sample 偏移只解出 %d / %d 个，stsc 与 stco 不一致" % (len(sample_offsets), len(sizes)))
        return 1

    print("样本数", len(sizes))
    # ❗ 本脚本**不**算 POC。POC 推导要维护 prevPicOrderCntMsb / prevFrameNumOffset
    # 的跨包状态，还要处理 IDR 重置、poc_lsb 回绕、delta_pic_order_cnt_bottom ——
    # 那正是被测代码的职责，在这里再写一遍只会得到第二份"同样可能算错"的实现，
    # 而且错的时候两边谁也说服不了谁。真正的显示顺序对账交给 B 组用例，
    # 它拿容器里的 PTS 当参考（编码器写的，与本仓库零共享）。
    #
    # 这里只解**单包内自洽**的字段：first_mb_in_slice / slice_type / frame_num /
    # poc_lsb。它们足以回答"这一包到底是不是一帧的开头"，
    # 也就是当初 40 帧只扫到 7 帧那个 bug 的判据。
    ok = 0
    non_zero_first_mb = 0
    for i, (off, size) in enumerate(zip(sample_offsets, sizes)):
        sample = data[off : off + size]
        p = 0
        while p + prefix <= len(sample):
            nal_size = int.from_bytes(sample[p : p + prefix], "big")
            p += prefix
            payload = sample[p : p + nal_size]
            p += nal_size
            nal_type = payload[0] & 0x1F
            if nal_type in (1, 2, 5):
                rbsp = unescape(payload[1:])
                br = Bits(rbsp)
                first_mb = br.ue()
                slice_type = br.ue() % 5
                br.ue()
                frame_num = br.u(sps["log2_max_frame_num"])
                field_pic = 0
                bottom = 0
                if not sps["frame_mbs_only"]:
                    field_pic = br.u(1)
                    if field_pic:
                        bottom = br.u(1)
                idr = nal_type == 5
                if idr:
                    br.ue()  # idr_pic_id
                poc_lsb = 0
                if sps["poc_type"] == 0:
                    poc_lsb = br.u(sps["log2_max_poc_lsb"])
                    if bottom_flag and not field_pic:
                        br.se()  # delta_pic_order_cnt_bottom
                if first_mb == 0:
                    ok += 1
                else:
                    non_zero_first_mb += 1
                if i < 8:
                    print("  sample %2d type=%d first_mb=%-6d slice_type=%d frame_num=%2d poc_lsb=%d"
                          % (i, nal_type, first_mb, slice_type, frame_num, poc_lsb))
    print("解析出的 slice 数:", ok + non_zero_first_mb,
          " 其中 first_mb_in_slice==0:", ok, " 非 0:", non_zero_first_mb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
