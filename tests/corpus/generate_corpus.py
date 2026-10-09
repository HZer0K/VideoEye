#!/usr/bin/env python3
r"""阶段 3: 异常输入确定性样本生成器 (tests/corpus/)。

用法:
    python tests/corpus/generate_corpus.py            # 生成/覆盖全部样本
    python tests/corpus/generate_corpus.py --check    # 校验磁盘样本与生成结果一致

为什么用脚本生成而不是直接提交二进制: 每个样本的"畸形点"必须能被审阅 ——
脚本里每段字节都注明构造意图(哪个字段被改坏、期望解析器如何反应), 而 .bin
文件在 code review 里只是一团乱码。--check 保证入库字节与脚本始终一致:
改了脚本忘了重新生成时, ctest 里的 CorpusGeneratorCheck 直接红。

样本组织(与 tests/unit/test_corpus_robustness.cpp 的类别分派一一对应):
    codec/       extradata / 码流参数集
    container/   容器结构 (MP4 / FLV / ASF / EBML / 通用空文件)
    streaming/   HLS / DASH 清单
    subtitle/    字幕文本 / SCTE-35 二进制段

覆盖清单(阶段 3.1 要求, 每一条都能在下面找到):
    截断文件 / 空文件 / 极短文件 / 错误长度字段 / 极大 ue(v) / 负 DASH r /
    未知长度 EBML 元素 / 深层嵌套 EBML / 错误 MP4 largesize / 错误 FLV header /
    ASF 对象长度小于头部 / 含 BOM 和异常大小写的 HLS / 不完整字幕 cue /
    错误 SCTE-35 CRC
"""
import argparse
import os
import struct
import sys

CORPUS_DIR = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------------------
# 小工具: 无符号整数 / bit 写入
# ---------------------------------------------------------------------------

def u32be(v):
    return struct.pack(">I", v)


def u64be(v):
    return struct.pack(">Q", v)


def u32le(v):
    return struct.pack("<I", v)


def u64le(v):
    return struct.pack("<Q", v)


class BitWriter:
    """MSB-first 比特写入器, 用来手工构造 H.264 NAL 位流。"""

    def __init__(self):
        self.bits = []

    def u(self, n, value):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def ue(self, value):
        """Exp-Golomb ue(v): leadingZeros + 1 + infoBits。"""
        code = value + 1
        lead = code.bit_length() - 1
        self.bits.extend([0] * lead)
        self.bits.append(1)
        for i in range(lead - 1, -1, -1):
            self.bits.append((code >> i) & 1)

    def flush(self):
        while len(self.bits) % 8:
            self.bits.append(0)
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for j in range(8):
                byte = (byte << 1) | self.bits[i + j]
            out.append(byte)
        return bytes(out)


# ---------------------------------------------------------------------------
# MP4 / ISOBMFF box
# ---------------------------------------------------------------------------

def mp4_box(box_type, payload=b"", declared_size=None, largesize=None):
    """构造一个 box。

    默认 size 字段写真实长度; declared_size 用来单独把长度字段改坏;
    largesize 非 None 时走 size==1 + 64 位 largesize 的路径。
    """
    if largesize is not None:
        return u32be(1) + box_type + u64be(largesize) + payload
    total = 8 + len(payload) if declared_size is None else declared_size
    return u32be(total) + box_type + payload


def build_ftyp():
    # isom 观察兼容性列表写一条, 24 字节
    return mp4_box(b"ftyp", b"isom" + u32be(0x200) + b"isom" + b"mp41")


# ---------------------------------------------------------------------------
# EBML / Matroska
# ---------------------------------------------------------------------------

def ebml_size(n, width):
    """已知长度按指定字节数编码 (VINT, 首字节含标记位)。"""
    if width == 1:
        assert n < 0x7F, "0x7F 是 1 字节的未知长度编码, 已知长度不要撞它"
        return bytes([0x80 | n])
    assert n < (1 << (7 * width)) - 1, "该宽度下 all-ones 表示未知长度, 不能用于已知长度"
    head = (1 << (8 - width)) | (n >> (8 * (width - 1)))
    tail = n & ((1 << (8 * (width - 1))) - 1)
    return bytes([head]) + tail.to_bytes(width - 1, "big")


def ebml_unknown_size(width=8):
    """未知长度编码: 该宽度下载荷全 1。真实 muxer 多用 8 字节形式。"""
    if width == 1:
        return b"\xff"
    return bytes([1 << (8 - width)]) + b"\xff" * (width - 1)


def ebml_elem(elem_id, payload=b"", size_width=None, unknown=False):
    if unknown:
        size_bytes = ebml_unknown_size(8)
    elif size_width is not None:
        size_bytes = ebml_size(len(payload), size_width)
    else:
        size_bytes = None
        for w in range(1, 9):
            if w == 1 and len(payload) >= 0x7F:
                continue
            if w > 1 and len(payload) >= (1 << (7 * w)) - 1:
                continue
            size_bytes = ebml_size(len(payload), w)
            break
        assert size_bytes is not None
    return elem_id + size_bytes + payload


def ebml_header():
    return ebml_elem(bytes.fromhex("1A45DFA3"),
                     ebml_elem(bytes.fromhex("4282"), b"matroska"))


# ---------------------------------------------------------------------------
# SCTE-35
# ---------------------------------------------------------------------------

def crc32_mpeg2(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def build_scte35_section():
    """一个结构完全合法的最小 splice_info_section (splice_null)。

    布局(section_length=17 从 protocol_version 起算, 含末尾 CRC):
        FC | ssi=0 private=0 sap=11 + section_length(12) | protocol_version(8)
        | encrypted(1)+alg(6)+pts_adjustment(33)+cw_index(8)
        | tier(12)+command_length(12) | command_type=0x00 | descriptor_loop_length(16)
        | CRC32
    """
    body = bytearray()
    body.append(0xFC)
    body.append(0x30)          # ssi=0 private=0 sap=11 + section_length[11:8]=0
    body.append(0x11)          # section_length[7:0] = 17
    body.append(0x00)          # protocol_version
    body.extend(b"\x00" * 5)   # encrypted + encryption_algorithm + pts_adjustment
    body.append(0x00)          # cw_index
    body.extend(bytes([0xFF, 0xF0, 0x00]))  # tier=0xFFF + command_length=0
    body.append(0x00)          # splice_command_type = splice_null
    body.extend(b"\x00\x00")   # descriptor_loop_length
    crc = crc32_mpeg2(bytes(body))
    body.extend(u32be(crc))
    return bytes(body)


# ---------------------------------------------------------------------------
# 样本构造: codec/
# ---------------------------------------------------------------------------

def build_codec_huge_uev_h264():
    """极大 ue(v): SPS 的 seq_parameter_set_id 用 33 个前导零的 ue(v)。

    seq_parameter_set_id 的合法范围只有 0..31, 33 个前导零意味着解析器要
    在"读了多少位/能左移多少位"上有上界; 无上界的实现会在这里移位溢出
    或把 NAL 剩余位读穿。
    """
    w = BitWriter()
    w.u(1, 0)          # forbidden_zero_bit
    w.u(2, 3)          # nal_ref_idc
    w.u(5, 7)          # nal_unit_type = 7 (SPS)
    w.u(8, 0x64)       # profile_idc = High
    w.u(8, 0x00)       # constraint flags
    w.u(8, 0x1F)       # level_idc = 3.1
    w.ue(0xFFFFFFFF)   # seq_parameter_set_id: 33 个前导零的极端 ue(v)
    w.u(1, 0)          # 截断 SPS 到这里为止(畸形)
    return b"\x00\x00\x00\x01" + w.flush()


def build_codec_truncated_avcc():
    """错误长度字段 (avcC): SPS 声明 16 字节, 实际只剩 4 字节。

    解析器要么判错, 要么只读实际可用的字节; 绝不能按声明长度越界读。
    """
    return bytes([
        0x01,              # configurationVersion
        0x64, 0x00, 0x1F,  # profile / compatibility / level
        0xFF,              # lengthSizeMinusOne = 3
        0xE1,              # numOfSequenceParameterSets = 1
        0x00, 0x10,        # SPS length = 16 (实际只有 4 字节)
        0x67, 0x64, 0x00, 0x1F,
    ])


# ---------------------------------------------------------------------------
# 样本构造: container/
# ---------------------------------------------------------------------------

def build_container_truncated_mp4():
    """截断文件: moov 声明 4096 字节, 文件在 moov 头部就结束了。"""
    ftyp = build_ftyp()
    moov_payload = b"\x00" * 24
    moov = mp4_box(b"moov", moov_payload, declared_size=4096)
    return ftyp + moov


def build_container_mp4_bad_largesize():
    """错误 MP4 largesize: size==1 时 64 位 largesize=8 < 16 (连头都不够)。

    解析器必须识别为损坏 box 并停止, 不能按 8 字节继续推进。
    """
    return build_ftyp() + mp4_box(b"free", b"", largesize=8)


def build_container_mp4_size_overflow():
    """错误长度字段 + 溢出: largesize = 0xFFFFFFFFFFFFFFF8。

    offset(24) + size 在 uint64 上回绕成 16, "pos+size > end" 的朴素比较
    会把它当合法 box 接受, 然后 pos 回绕、重复扫描。必须有溢出检查。
    """
    return build_ftyp() + mp4_box(b"free", b"", largesize=0xFFFFFFFFFFFFFFF8)


def build_container_flv_bad_header():
    """错误 FLV header: 魔数 'FLA' (第三个字节被改坏)。"""
    return b"FLA\x01\x01\x00\x00\x00\x09" + b"\x00\x00\x00\x00"


def build_container_flv_wrong_size():
    """错误长度字段 (FLV): DataSize 声明 0xFFFFFF, 实际只有 6 字节。

    解析器按声明长度 seek 之后不得崩溃 / 死循环, 应当发现越界并停止。
    """
    header = b"FLV\x01\x01\x00\x00\x00\x09"
    prev = b"\x00\x00\x00\x00"          # PreviousTagSize0
    tag = bytes([0x09, 0xFF, 0xFF, 0xFF]) + b"\x00\x00\x00" + b"\x00"  # 11 字节 tag 头
    payload = b"\x17\x00\x00\x00\x00\x00"  # 远少于声明的 0xFFFFFF
    return header + prev + tag + payload


def build_container_asf_object_short():
    """ASF 对象长度小于头部: 对象 size=10 (< 公共头 24 字节)。

    解析器里 obj_size - 24 会回绕成天文数字, 直接 file.Read() 去要这块内存
    —— 必须夹住。样本里第一个对象用的是 File Properties GUID, 走的正是
    会做减法的那条路径。
    """
    header_guid = bytes.fromhex("3026B2758E66CF11A6D900AA0062CE6C")
    file_props_guid = bytes.fromhex("A1DCAB8C47A9CF118EE400C00C205365")
    header_size = 80
    out = bytearray()
    out += header_guid
    out += u64le(header_size)   # Header Object 总长
    out += u32le(1)             # Number of Header Objects
    out += b"\x01\x02"          # reserved1 / reserved2
    out += file_props_guid
    out += u64le(10)            # 声称 10 字节, 比 24 字节公共头还短
    out += b"\x00" * (header_size - len(out))
    return bytes(out)


def build_container_ebml_unknown_size():
    """未知长度 EBML 元素: Segment 与内部 Cluster 都用 8 字节 all-ones 编码。

    未知长度的语义是"延伸到父元素末尾"; 当成普通 size 用会把元素边界算错,
    当成 0 会一个元素都读不出来。
    """
    inner = ebml_elem(bytes.fromhex("E7"), b"\x00\x00\x00\x00")           # Timestamp
    cluster = ebml_elem(bytes.fromhex("1F43B675"), inner, unknown=True)    # Cluster, 未知长度
    segment = ebml_elem(bytes.fromhex("18538067"), cluster, unknown=True)  # Segment, 未知长度
    return ebml_header() + segment


def build_container_ebml_deep_nesting():
    """深层嵌套 EBML: 100 层 Cluster 自嵌套。

    递归解析器必须在 64 层(kMaxDepth)附近停下, 且整体时间有界。
    """
    inner = ebml_elem(bytes.fromhex("E7"), b"\x00\x00\x00\x00")
    for _ in range(100):
        inner = ebml_elem(bytes.fromhex("1F43B675"), inner)
    segment = ebml_elem(bytes.fromhex("18538067"), inner)
    return ebml_header() + segment


def build_container_truncated_mkv():
    """截断的 MKV: Segment 声明 1000 字节, 实际只有 20 字节。"""
    header = ebml_header()
    cluster_payload = b"\x00" * 12
    segment = bytes.fromhex("18538067") + ebml_size(1000, 8) + cluster_payload
    return header + segment


# ---------------------------------------------------------------------------
# 样本构造: streaming/
# ---------------------------------------------------------------------------

def build_streaming_hls_bom_lowercase():
    """含 BOM 和异常大小写的 HLS: UTF-8 BOM + 全小写标签。

    BOM 是真实世界里最常见的"清单第一行对不上"来源; 标签大小写按 RFC
    应当大写, 但只做质检的工具不该因此把整份清单判成"不是 HLS"。
    """
    text = (
        "\ufeff#extm3u\n"
        "#ext-x-version:3\n"
        "#ext-x-targetduration:6\n"
        "#ext-x-media-sequence:0\n"
        "#extinf:6.000,\n"
        "seg-1.ts\n"
        "#extinf:6.000,\n"
        "seg-2.ts\n"
        "#ext-x-endlist\n"
    )
    return text.encode("utf-8")


def build_streaming_dash_negative_r():
    """负 DASH r: <S r="-5"/>。

    r 是"额外重复次数", 只允许 >= 0 (或 -1 表示到 Period 末尾)。负数直接
    static_cast<uint32_t> 会回绕成 43 亿, 展开循环跑满 5000 段上限才停。
    """
    text = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<MPD mediaPresentationDuration="PT30S">\n'
        '  <Period>\n'
        '    <AdaptationSet mimeType="video/mp4">\n'
        '      <Representation id="v0" bandwidth="800000" width="1280" height="720">\n'
        '        <SegmentTemplate timescale="1000" initialization="init.mp4"\n'
        '                         media="seg-$Number$.m4s" startNumber="1">\n'
        '          <SegmentTimeline>\n'
        '            <S t="0" d="2000" r="-5"/>\n'
        '            <S d="2000" r="2"/>\n'
        '          </SegmentTimeline>\n'
        '        </SegmentTemplate>\n'
        '      </Representation>\n'
        '    </AdaptationSet>\n'
        '  </Period>\n'
        '</MPD>\n'
    )
    return text.encode("utf-8")


def build_streaming_dash_extreme_count():
    """极端 duration/timescale: 分片数算出来是天文数字。

    duration=1 / timescale=1e9 => 每片 1ns, 配上 PT1000000S 的时长,
    分片数 ~1e15。展开循环必须在 max_segments_per_representation 处截断,
    否则"解析一份清单"就变成永不返回。
    """
    text = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<MPD mediaPresentationDuration="PT1000000S">\n'
        '  <Period>\n'
        '    <AdaptationSet mimeType="video/mp4">\n'
        '      <Representation id="v0" bandwidth="800000">\n'
        '        <SegmentTemplate timescale="1000000000" duration="1"\n'
        '                         initialization="init.mp4" media="seg-$Number$.m4s"\n'
        '                         startNumber="1"/>\n'
        '      </Representation>\n'
        '    </AdaptationSet>\n'
        '  </Period>\n'
        '</MPD>\n'
    )
    return text.encode("utf-8")


# ---------------------------------------------------------------------------
# 样本构造: subtitle/
# ---------------------------------------------------------------------------

def build_subtitle_srt_incomplete_cue():
    """不完整字幕 cue: 第二条的 "-->" 断成 "--", 第三条没有结尾空行。"""
    text = (
        "1\n"
        "00:00:01,000 --> 00:00:02,500\n"
        "第一条完整字幕\n"
        "\n"
        "2\n"
        "00:00:05,000 -- 00:00:07,000\n"
        "箭头断掉, 这不是一条合法 cue\n"
        "\n"
        "3\n"
        "00:00:08,000 --> 00:00:09,000\n"
        "结尾没有空行的最后一条"
    )
    return text.encode("utf-8")


def build_subtitle_vtt_incomplete_cue():
    """不完整 WebVTT cue: 第二条时间行只有一个箭头, 结束时间被截掉。"""
    text = (
        "WEBVTT\n"
        "\n"
        "00:00:01.000 --> 00:00:02.500\n"
        "合法 cue\n"
        "\n"
        "00:00:05.000 -->\n"
    )
    return text.encode("utf-8")


def build_subtitle_scte35_valid_crc():
    """结构合法 + CRC 正确的 SCTE-35 段(用于给"错误 CRC"提供对照)。"""
    return build_scte35_section()


def build_subtitle_scte35_bad_crc():
    """错误 SCTE-35 CRC: 同一段合法 section, 末字节翻转。

    期望: 段本身仍可解析(valid=true), 但 crc_checked=true / crc_valid=false
    —— "CRC 坏了"必须能被质检规则看见。
    """
    section = bytearray(build_scte35_section())
    section[-1] ^= 0xFF
    return bytes(section)


# ---------------------------------------------------------------------------
# 样本清单
# ---------------------------------------------------------------------------

SAMPLES = {
    # codec/
    "codec/huge_uev.h264": build_codec_huge_uev_h264,
    "codec/truncated_avcc.bin": build_codec_truncated_avcc,

    # container/
    "container/empty.bin": lambda: b"",
    "container/tiny.bin": lambda: b"\x00\x00\x01",
    "container/truncated.mp4": build_container_truncated_mp4,
    "container/mp4_bad_largesize.mp4": build_container_mp4_bad_largesize,
    "container/mp4_size_overflow.mp4": build_container_mp4_size_overflow,
    "container/flv_bad_header.flv": build_container_flv_bad_header,
    "container/flv_wrong_size.flv": build_container_flv_wrong_size,
    "container/asf_object_short.wmv": build_container_asf_object_short,
    "container/ebml_unknown_size.mkv": build_container_ebml_unknown_size,
    "container/ebml_deep_nesting.mkv": build_container_ebml_deep_nesting,
    "container/truncated.mkv": build_container_truncated_mkv,

    # streaming/
    "streaming/hls_bom_lowercase.m3u8": build_streaming_hls_bom_lowercase,
    "streaming/dash_negative_r.mpd": build_streaming_dash_negative_r,
    "streaming/dash_extreme_count.mpd": build_streaming_dash_extreme_count,

    # subtitle/
    "subtitle/srt_incomplete_cue.srt": build_subtitle_srt_incomplete_cue,
    "subtitle/vtt_incomplete_cue.vtt": build_subtitle_vtt_incomplete_cue,
    "subtitle/scte35_valid_crc.bin": build_subtitle_scte35_valid_crc,
    "subtitle/scte35_bad_crc.bin": build_subtitle_scte35_bad_crc,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只校验磁盘上的样本与生成结果是否一致")
    args = ap.parse_args()

    mismatches = []
    wrote = 0
    for rel, builder in sorted(SAMPLES.items()):
        path = os.path.join(CORPUS_DIR, rel)
        expected = builder()
        if args.check:
            if not os.path.exists(path):
                mismatches.append("%s: 文件缺失" % rel)
                continue
            with open(path, "rb") as fh:
                actual = fh.read()
            if actual != expected:
                mismatches.append("%s: 磁盘 %d 字节 != 生成 %d 字节"
                                  % (rel, len(actual), len(expected)))
            continue

        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(expected)
        wrote += 1

    if args.check:
        if mismatches:
            print("语料校验失败:", file=sys.stderr)
            for m in mismatches:
                print("  " + m, file=sys.stderr)
            print("重新运行: python tests/corpus/generate_corpus.py", file=sys.stderr)
            return 1
        print("语料校验通过: %d 个样本与生成器一致" % len(SAMPLES))
        return 0

    print("已生成 %d 个样本 -> %s" % (wrote, CORPUS_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())