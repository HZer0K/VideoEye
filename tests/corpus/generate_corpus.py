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


def build_codec_hevc_pps_absurd_tile_count():
    """HEVC PPS 的 num_tile_columns_minus1 是荒谬大的 ue(v)（20 亿，规范上限 19）。

    PPS 里这个值和 num_tile_rows_minus1 直接决定两个循环的次数：裸 ReadUE()
    读到 1000000 就会 push_back 一百万次，读到 2^31 量级则直接"数十亿次空转"。
    实际抓到它的是 libFuzzer（FuzzBitstreamExtradata 在 17 字节输入上 10 秒超时），
    这里改成语义正确的 PPS：NAL type 34 + tiles_enabled_flag=1 + 荒谬 tile 数。
    """
    w = BitWriter()
    w.u(1, 0)          # forbidden_zero_bit
    w.u(6, 34)         # nal_unit_type = 34 (PPS)
    w.u(6, 0)          # nuh_layer_id
    w.u(3, 1)          # nuh_temporal_id_plus1
    w.ue(0)            # pps_pic_parameter_set_id
    w.ue(0)            # pps_seq_parameter_set_id
    w.u(1, 0)          # dependent_slice_segments_enabled_flag
    w.u(1, 0)          # output_flag_present_flag
    w.u(3, 0)          # num_extra_slice_header_bits
    w.u(1, 0)          # sign_data_hiding_enabled_flag
    w.u(1, 0)          # cabac_init_present_flag
    w.ue(0)            # num_ref_idx_l0_default_active_minus1
    w.ue(0)            # num_ref_idx_l1_default_active_minus1
    w.ue(0)            # init_qp_minus26 (se, 取 0)
    w.u(1, 0)          # constrained_intra_pred_flag
    w.u(1, 0)          # transform_skip_enabled_flag
    w.u(1, 0)          # cu_qp_delta_enabled_flag
    w.ue(0)            # pps_cb_qp_offset (se, 取 0)
    w.ue(0)            # pps_cr_qp_offset (se, 取 0)
    w.u(1, 0)          # pps_slice_chroma_qp_offsets_present_flag
    w.u(1, 0)          # weighted_pred_flag
    w.u(1, 0)          # weighted_bipred_flag
    w.u(1, 0)          # transquant_bypass_enabled_flag
    w.u(1, 1)          # tiles_enabled_flag
    w.u(1, 0)          # entropy_coding_sync_enabled_flag
    # num_tile_columns_minus1: 荒谬大（规范上限 19）。刻意取 20 亿而不是 1 亿：
    # 这个值仍能塞进 int，老代码会老老实实循环 20 亿次（超时/OOM）；再大就溢成负数，
    # 循环反而一次都不跑，测试就失去意义了。
    w.ue(2000000000)
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
# 样本构造: codec/VVC (H.266)
#
# 为什么要单独一批: VVC 的 NAL header 是 2 字节、POC 写在 picture header 里,
# 与 H.264/HEVC 的 slice_header 完全不同。"能跑通"在 VVC 上特别容易是假象 ——
# 位错开一位照样能读出看起来合理的数值。这批样本只负责一件事: 让"拒绝"这条
# 路径可复现, 而不是靠人工挑文件。
#
# 语法顺序以 FFmpeg libavcodec/cbs_h266_syntax_template.c 为准
# (与 H.266 7.3.2.3/7.3.2.4/7.3.2.8/7.3.2.9 逐条对应)。
# ---------------------------------------------------------------------------

def rbsp_escape(rbsp):
    """按 H.264/H.266 的 emulation prevention 规则插入 0x03。

    ❗ 合成样本必须真的做这一步, 否则"手写位流"和"真实码流"不是同一种东西:
    一个荒谬大的 ue(v) 会写出长串 0, 很容易在字节边界凑出 `00 00 01` —— 分析器
    按 Annex B 找起始码时会在那里**截断 NAL**, 于是样本"被拒绝"是因为尾巴没了,
    而不是因为我们想测的那个上界。解析器侧会做反向去转义 (GetRbsp), 所以带上
    转义不会改变解出来的语法。
    """
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros == 2 and b <= 3:
            out.append(0x03)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def vvc_nal(nal_type, payload):
    """Annex B 封装一个 VVC NAL: 4 字节起始码 + 2 字节 header + (已转义的) RBSP。"""
    return b"\x00\x00\x00\x01" + bytes([0x00, (nal_type << 3) | 0x01]) + rbsp_escape(payload)


def vvc_sps_prefix(w, subpic_info=False):
    """按 7.3.2.3 写到 conformance_window 为止的 SPS 公共前缀。

    只写帧级解析真正会读到的字段; 之后的工具开关与 VUI 与本批样本的畸形点无关。
    """
    w.u(4, 0)    # sps_seq_parameter_set_id
    w.u(4, 0)    # sps_video_parameter_set_id
    w.u(3, 0)    # sps_max_sublayers_minus1
    w.u(2, 1)    # sps_chroma_format_idc = 4:2:0
    w.u(2, 1)    # sps_log2_ctu_size_minus5 -> CTU 64
    w.u(1, 0)    # sps_ptl_dpb_hrd_params_present_flag
    w.u(1, 0)    # sps_gdr_enabled_flag
    w.u(1, 0)    # sps_ref_pic_resampling_enabled_flag
    w.ue(1920)   # sps_pic_width_max_in_luma_samples
    w.ue(1080)   # sps_pic_height_max_in_luma_samples
    w.u(1, 0)    # sps_conformance_window_flag
    w.u(1, 1 if subpic_info else 0)  # sps_subpic_info_present_flag


def build_codec_vvc_sps_absurd_subpic_count():
    """VVC SPS 的 sps_num_subpics_minus1 是荒谬大的 ue(v) (10 亿, 规范上限 1024)。

    subpic 数直接决定后面三个 SkipBits 循环的次数; 无界读就是 10 亿次跳过,
    分析线程会被一个 6 字节的 SPS 拖死。期望: present=false, 且有界返回。

    ❗ 这条上界的守护点在**时间域**, 不是"样本被拒"这个结果本身: 去掉上界后循环
    次数由码流决定, 用例里的限时断言才会报警。别把"样本被拒"当成"上界被测到了" ——
    位流在荒谬值之后本来就不够读, 尾巴耗尽同样会置错并判非法(实测把循环强行夹到
    3 次, 样本照样被拒)。详见 test_corpus_robustness.cpp 里该用例的注释。
    """
    w = BitWriter()
    vvc_sps_prefix(w, subpic_info=True)
    w.ue(1000000000)  # sps_num_subpics_minus1
    return vvc_nal(15, w.flush())


def build_codec_vvc_sps_absurd_poc_msb_len():
    """VVC SPS 的 sps_poc_msb_cycle_len_minus1 是 2^31 量级的 ue(v)。

    这个值是 ph_poc_msb_cycle_val 的**位宽**; 无界读会让帧级 POC 解析去
    `ReadBits(2147483649)` —— 结果是位流被读穿后补 0, 造出一条 POC 全 0 的
    假时间线, 而且每一步都"成功"。期望: 整份 SPS 判非法。

    ❗ 这条样本钉的是端到端"不产结论", 而不是这条上界本身: ReadUEBounded(31) 属于
    纵深防御 —— 黑盒上观察不到差异(ReadUE 自己在 32 位前缀处就会报错, 换回去样本
    照样被拒, 实测变异为绿)。真正防"假时间线"的是追踪器对 msb_cycle 的显式拒绝,
    那条有专门用例(test_vvc_frame_order_tracker.cpp 的拒绝路径)。
    """
    w = BitWriter()
    vvc_sps_prefix(w, subpic_info=False)
    w.ue(2)      # sps_bitdepth_minus8 -> 10bit
    w.u(1, 0)    # sps_entropy_coding_sync_enabled_flag
    w.u(1, 0)    # sps_entry_point_offsets_present_flag
    w.u(4, 8)    # sps_log2_max_pic_order_cnt_lsb_minus4 -> 12 位 lsb
    w.u(1, 1)    # sps_poc_msb_cycle_flag
    w.ue(2147483648)  # sps_poc_msb_cycle_len_minus1: 远超规范上限 31
    return vvc_nal(15, w.flush())


def build_codec_vvc_pps_absurd_tile_count():
    """VVC PPS 的 pps_num_exp_tile_columns_minus1 是荒谬大的 ue(v)。

    与 HEVC 的 tile 样本同型: 这个值决定 tile 列宽数组的读取次数,
    裸 ReadUE() 读到 10 亿就会循环 10 亿次。
    """
    w = BitWriter()
    w.u(6, 0)    # pps_pic_parameter_set_id
    w.u(4, 0)    # pps_seq_parameter_set_id
    w.u(1, 0)    # pps_mixed_nalu_types_in_pic_flag
    w.ue(1920)   # pps_pic_width_in_luma_samples
    w.ue(1080)   # pps_pic_height_in_luma_samples
    w.u(1, 0)    # pps_conformance_window_flag
    w.u(1, 0)    # pps_scaling_window_explicit_signalling_flag
    w.u(1, 0)    # pps_output_flag_present_flag
    w.u(1, 0)    # pps_no_pic_partition_flag = 0 -> 进入 tile 段
    w.u(1, 0)    # pps_subpic_id_mapping_present_flag
    w.u(2, 1)    # pps_log2_ctu_size_minus5
    w.ue(1000000000)  # pps_num_exp_tile_columns_minus1
    return vvc_nal(16, w.flush())


def build_codec_vvc_ph_missing_pps():
    """VVC picture header 引用了不存在的 PPS (id=63)。

    POC 的位宽来自"PPS -> SPS"这条链; 链断了就只能拒绝。
    期望: 帧级追踪不产帧、给出"没有解析到 PPS"的原因, 而不是按 0 位宽读一个 POC。
    """
    w = BitWriter()
    w.u(1, 1)    # ph_gdr_or_irap_pic_flag
    w.u(1, 0)    # ph_non_ref_pic_flag
    w.u(1, 0)    # ph_gdr_pic_flag
    w.u(1, 1)    # ph_inter_slice_allowed_flag
    w.u(1, 0)    # ph_intra_slice_allowed_flag
    w.ue(63)     # ph_pic_parameter_set_id (没有对应 PPS)
    w.u(8, 0)    # 随便一些位, 解析器不应走到这里
    return vvc_nal(19, w.flush())


def build_codec_vvc_slice_truncated():
    """slice header 里内联的 picture header 只写了一半就结束。

    sh_picture_header_in_slice_header_flag=1 时 PH 直接在 slice 里; 位流在
    ph_pic_order_cnt_lsb 之前就耗尽, 读出来的是补 0。期望: 不落帧。
    """
    w = BitWriter()
    w.u(1, 1)    # sh_picture_header_in_slice_header_flag
    w.u(1, 0)    # ph_gdr_or_irap_pic_flag
    w.u(1, 0)    # ph_non_ref_pic_flag
    w.u(1, 1)    # ph_inter_slice_allowed_flag
    w.u(1, 0)    # ph_intra_slice_allowed_flag
    w.ue(0)      # ph_pic_parameter_set_id
    return vvc_nal(0, w.flush())  # TRAIL_NUT


def build_codec_vvc_truncated_vvcc():
    """截断的 vvcC: num_of_arrays=1, NAL 声明 256 字节, 实际只剩 6 字节。

    ❗ 这条样本是"valid 语义"的回归: 与 avcC/av1C 一致, 截断必须
    valid=false + 非空 error_message。旧实现只写 error_message、valid 仍为 true,
    上层会把"全 0 的 config"当成一次成功的配置记录解析。
    """
    ptl = bytes([
        0xFF,               # reserved(5) | lengthSizeMinusOne(2)=3 | ptl_present(1)
        0x00, 0x01,         # ols_idx(9)=0 | num_sublayers(3)=0 | cfr(2)=0 | chroma(2)=1
        0x40,               # bit_depth_minus8(3)=2 | reserved(5)
        0x01,               # reserved(2) | num_bytes_constraint_info(6)=1
        0x02,               # general_profile_idc(7)=1 | general_tier_flag(1)=0
        0x33,               # general_level_idc = 51 (3.1)
        0x80,               # frame_only(1) | multilayer(1) | constraint info(6)
        0x00,               # num_sub_profiles = 0
        0x07, 0x80,         # max_picture_width  = 1920
        0x04, 0x38,         # max_picture_height = 1080
        0x00, 0x00,         # avg_frame_rate
    ])
    arrays = bytes([0x01])          # num_of_arrays = 1
    arrays += bytes([0x0F])         # completeness | reserved | NAL_unit_type = 15 (SPS)
    arrays += bytes([0x00, 0x01])   # numNalus = 1
    arrays += bytes([0x01, 0x00])   # nalUnitLength = 256 (实际只有 6 字节)
    arrays += bytes([0x00, 0x00, 0x51, 0x80, 0x00, 0x40])
    return ptl + arrays


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


def build_container_asf_header_size_overflow():
    """ASF Header Object 声明的长度溢出 int64（0xD100560000002E46）。

    obj_start + obj_size 会变成负数，Seek(负数) 被夹到 0，下一轮又读到文件头 ——
    同一个 obj_size、同一个偏移，循环原地打转。FuzzContainerStructure 在 31 字节的
    输入上就是这么超时的（FuzzContainerStructure-timeout-6e4a9c38…）。
    另外 num_objects 声明 65535 个对象，实际一个都放不下。
    """
    header_guid = bytes.fromhex("3026B2758E66CF11A6D900AA0062CE6C")
    out = bytearray()
    out += header_guid
    out += u64le(0xD100560000002E46)  # Header Object 总长: 天文数字, 加偏移后溢出成负数
    out += u32le(0x0000FFFF)          # Number of Header Objects = 65535
    out += b"\x00\x07\xA0"            # reserved1 / reserved2 + 1 字节尾巴
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
    "codec/hevc_pps_absurd_tile_count.bin": build_codec_hevc_pps_absurd_tile_count,
    # VVC: 参数集与帧头解析的拒绝路径
    "codec/vvc_sps_absurd_subpic_count.bin": build_codec_vvc_sps_absurd_subpic_count,
    "codec/vvc_sps_absurd_poc_msb_len.bin": build_codec_vvc_sps_absurd_poc_msb_len,
    "codec/vvc_pps_absurd_tile_count.bin": build_codec_vvc_pps_absurd_tile_count,
    "codec/vvc_ph_missing_pps.bin": build_codec_vvc_ph_missing_pps,
    "codec/vvc_slice_truncated.bin": build_codec_vvc_slice_truncated,
    "codec/vvc_truncated_vvcc.bin": build_codec_vvc_truncated_vvcc,

    # container/
    "container/empty.bin": lambda: b"",
    "container/tiny.bin": lambda: b"\x00\x00\x01",
    "container/truncated.mp4": build_container_truncated_mp4,
    "container/mp4_bad_largesize.mp4": build_container_mp4_bad_largesize,
    "container/mp4_size_overflow.mp4": build_container_mp4_size_overflow,
    "container/flv_bad_header.flv": build_container_flv_bad_header,
    "container/flv_wrong_size.flv": build_container_flv_wrong_size,
    "container/asf_object_short.wmv": build_container_asf_object_short,
    "container/asf_header_size_overflow.wmv": build_container_asf_header_size_overflow,
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