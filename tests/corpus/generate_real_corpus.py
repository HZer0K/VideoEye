#!/usr/bin/env python3
r"""真实封装样本生成器 (tests/corpus/container/)。

与 generate_corpus.py 的分工
----------------------------
generate_corpus.py 里的样本是**逐字节手工构造的畸形样本**，--check 比对磁盘字节
与脚本产物，保证"改了脚本忘了重新生成"立刻红。

本脚本产的是**真实封装**：由 ffmpeg 编出来、能在播放器里正常播的文件。
它们补的是另一类缺口 —— 畸形样本只能证明"解析器不会崩"，证明不了"解析器在
真实文件上算出来的数是对的"。没有真实样本时，TS 的 PAT/PMT/PCR/连续计数、
AVI 的 idx1/movi、Ogg 的页链，都只有契约级用例在撑着。

用法
----
    python tests/corpus/generate_real_corpus.py            # 生成/覆盖全部真实样本
    python tests/corpus/generate_real_corpus.py --check    # 校验磁盘样本仍然合格

为什么 --check 不比字节
------------------------
ffmpeg 的输出**不是字节可复现的**（编码器版本、线程调度都会改字节），所以不能
像畸形样本那样逐字节比对。这里改成**语义校验**：文件存在、大小合理、同步字节
按预期步长严格对齐、容器魔数正确。

这仍然能抓住两类真正会咬人的漂移：
  * 换了个 ffmpeg 版本，产出变成了别的容器（魔数/步长校验红）；
  * 样本在入库/传输中被截断（步长整除性与"每一包都有同步字节"校验红）。
抓不住的是"编码器换了、字节不同但结构相同" —— 那本来就不该红。

缺 ffmpeg 时的行为
------------------
样本已在磁盘上 -> 照常语义校验（CI 上不需要 ffmpeg 也能验收）。
样本缺失且找不到 ffmpeg -> 打印 SKIP 并返回 0，不阻塞没有 ffmpeg 的机器。
"""

from __future__ import print_function

import argparse
import os
import shutil
import struct
import subprocess
import tempfile
import sys

CORPUS_DIR = os.path.dirname(os.path.abspath(__file__))

VIDEO = "testsrc=size=176x144:rate=10:duration=1"
AUDIO = "sine=frequency=440:sample_rate=44100:duration=1"

# 相对 CORPUS_DIR 的路径 -> 生成方式
#   kind: 校验时按哪种容器比对
#   build: 生成函数（返回字节）；derived 的样本从另一个样本派生，保证两者内容同源
#   derive: 派生函数（输入源字节，返回产物）；不填时沿用历史上的 M2TS 派生
SAMPLES = {}


def register(rel, kind, build=None, derived=None, derive=None):
    SAMPLES[rel] = {"kind": kind, "build": build, "derived": derived,
                    "derive": derive}


# ---------------------------------------------------------------------------
# 生成：调 ffmpeg
# ---------------------------------------------------------------------------

def find_ffmpeg():
    for name in ("ffmpeg", "ffmpeg.exe"):
        path = shutil.which(name)
        if path:
            return path
    return None


def run_ffmpeg(ffmpeg, args, suffix):
    """跑 ffmpeg 并返回产物字节。

    输出到**真实临时文件**再读回，不用 stdout 管道：管道不可 seek，ffmpeg 只能
    把容器里的总长度字段写成"未知"（实测 AVI 会变成 RIFF size = 0xFFFFFFFF、
    movi 也是 0xFFFFFFFF），那不是我们想拿来测的正常 AVI。
    """
    tmp = tempfile.NamedTemporaryFile(prefix="ve_real_corpus_", suffix=suffix,
                                      delete=False)
    tmp.close()
    try:
        cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + args + [tmp.name]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError("ffmpeg 失败: %s"
                               % proc.stderr.decode("utf-8", "replace"))
        with open(tmp.name, "rb") as fh:
            return fh.read()
    finally:
        os.unlink(tmp.name)


def build_ts(ffmpeg):
    """MPEG-TS：H.264 + AAC，ffmpeg 会写全 PAT / PMT / PCR / 连续计数。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", VIDEO,
        "-f", "lavfi", "-i", AUDIO,
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "5",
        "-c:a", "aac", "-b:a", "64k",
        "-f", "mpegts",
    ], ".ts")


def build_avi(ffmpeg):
    """AVI：MPEG-4 Part 2 + MP3，RIFF/hdrl/movi/idx1 都是 ffmpeg 正常写出来的。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", VIDEO,
        "-f", "lavfi", "-i", AUDIO,
        "-c:v", "mpeg4", "-c:a", "libmp3lame", "-b:a", "64k",
        "-f", "avi",
    ], ".avi")


def build_ogg(ffmpeg):
    """Ogg：Vorbis，多页 OggS 页链。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", AUDIO,
        "-c:a", "libvorbis", "-b:a", "64k",
        "-f", "ogg",
    ], ".ogg")


def build_mkv(ffmpeg):
    """Matroska：H.264 + AAC，ffmpeg 会写 EBML 头 / Segment / Info / Tracks / Cluster / Cues。

    为什么要额外加这一份：语料里此前**一个真实 MKV 都没有**，MKV 侧全靠手工拼的
    畸形字节（unknown-size / 深嵌套 / 截断）撑着 —— 那只能证明解析器不崩，证明不了
    "在真实文件上读出来的 Cues / Cluster 时间轴 / CodecPrivate 是对的"。而
    container.mkv.* 六条规则与元素树窗口行为都建立在"真实 muxer 的产物"上。

    -g 5 / 时长 1s / 10fps → 两个关键帧、两个 Cluster，Cues 落在片尾 ——
    正好能测"片尾索引在不在扫描窗口里"这类结论。
    """
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", VIDEO,
        "-f", "lavfi", "-i", AUDIO,
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "5",
        "-c:a", "aac", "-b:a", "64k",
        "-f", "matroska",
    ], ".mkv")


def build_m2ts(ts_bytes):
    r"""M2TS/AVCHD：每个 188 字节 TS 包前面插 4 字节 TP_extra_header。

    不从 ffmpeg 直接出（ffmpeg 的 mpegts 复用器写的是 188），而是从同一份 .ts
    派生 —— 这样 .ts 与 .m2ts 的**内容完全一致**，测到 192 步长时任何差异都只
    可能来自步长处理本身，而不是"两份不同的编码"。

    TP_extra_header（4 字节，大端 32 位）：
        高 2 位  = copy_permission_indicator（0 = 允许复制）
        低 30 位 = arrival_time_stamp（27MHz / 300 计数）
    """
    if len(ts_bytes) % 188 != 0:
        raise RuntimeError("源 TS 长度不是 188 的整数倍: %d" % len(ts_bytes))
    out = bytearray()
    index = 0
    for pos in range(0, len(ts_bytes), 188):
        ats = index * 0x10
        out += struct.pack(">I", ats & 0x3FFFFFFF)
        out += ts_bytes[pos:pos + 188]
        index += 1
    return bytes(out)


# ---- 从真实样本派生"真实损伤" --------------------------------------------
#
# 修复 / QC 的异常样本如果全靠手工拼字节，只能证明"解析器不崩"；派生损伤保留了
# 真实 muxer 写出的 moov / 样本表 / tag 链，坏掉的只有具体那一段。


def truncate_tail(data, ratio):
    """尾部截断：模拟录制中断 / 下载不全。"""
    if ratio <= 0.0 or ratio >= 1.0:
        raise ValueError("截断比例必须在 (0, 1) 内")
    return data[:int(len(data) * ratio)]


def mp4_box_ranges(data):
    """返回顶层 box [(type, start, end)]，支持 64 位 largesize 与 size==0。"""
    boxes = []
    pos = 0
    while pos + 8 <= len(data):
        size = struct.unpack(">I", data[pos:pos + 4])[0]
        name = data[pos + 4:pos + 8]
        header = 8
        if size == 1:
            if pos + 16 > len(data):
                break
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
            header = 16
        elif size == 0:
            size = len(data) - pos
        if size < header or pos + size > len(data):
            break
        boxes.append((name, pos, pos + size))
        pos += size
    return boxes


def drop_top_level_box(data, target):
    """删掉指定顶层 box（用于派生"缺 moov"与"孤立分片"）。"""
    ranges = mp4_box_ranges(data)
    if not any(name == target for name, _start, _end in ranges):
        raise RuntimeError("找不到顶层 box %r" % target)
    out = bytearray()
    for name, start, end in ranges:
        if name != target:
            out += data[start:end]
    if ranges:
        out += data[ranges[-1][2]:]
    return bytes(out)


def patch_chunk_offsets(data, ratio, only_first_track=False):
    r"""把 stco/co64 中同一比例的 chunk offset 改成远超文件大小的值。

    模拟"录制中途存储区域损坏、文件结构与后续数据仍在"—— 这是修复模块跳段恢复
    的真实触发条件。默认改写所有轨；only_first_track 只坏第一条轨（视频轨）。

    注意：若音频轨的 chunk offset 也损坏，FFmpeg 的音频索引会整体失效，
    即使 seek 到损坏点之后也读不到任何音频包，跳段恢复在音频侧永远无法收敛。
    因此验证"多段恢复"的样本只破坏视频轨，保留音频索引可读。
    """
    out = bytearray(data)
    patched = 0
    for tag, entry_size in ((b"stco", 4), (b"co64", 8)):
        idx = out.find(tag)
        while idx != -1:
            if idx >= 4:
                size = struct.unpack(">I", bytes(out[idx - 4:idx]))[0]
                count = struct.unpack(">I", bytes(out[idx + 8:idx + 12]))[0]
                table_end = idx - 4 + size
                if count > 0 and idx + 12 + count * entry_size <= table_end:
                    entry = int(count * ratio)
                    if entry >= count:
                        entry = count - 1
                    at = idx + 12 + entry * entry_size
                    value = 0x7F000000
                    if entry_size == 4:
                        out[at:at + 4] = struct.pack(">I", value)
                    else:
                        out[at:at + 8] = struct.pack(">Q", value)
                    patched += 1
                    if only_first_track:
                        return bytes(out)
            idx = out.find(tag, idx + 4)
    if patched == 0:
        raise RuntimeError("没有可改写的 stco/co64（源样本不是 faststart MP4？）")
    return bytes(out)


def _ts_pid(packet):
    return ((packet[1] & 0x1F) << 8) | packet[2]


def _ts_has_payload(packet):
    return ((packet[3] >> 4) & 0x03) in (1, 3)


def corrupt_ts_continuity(data):
    """把后半段某个非 PSI PID 的 continuity_counter 跳 +2，制造连续的计数错误。"""
    if len(data) % 188 != 0:
        raise RuntimeError("源 TS 长度不是 188 的整数倍")
    total = len(data) // 188
    for index in range(total - 2, total // 2, -1):
        packet = data[index * 188:(index + 1) * 188]
        if _ts_pid(packet) not in (0, 0x1FFF) and _ts_has_payload(packet):
            cc = packet[3] & 0x0F
            patched = bytes([(packet[3] & 0xF0) | ((cc + 2) & 0x0F)])
            offset = index * 188 + 3
            return data[:offset] + patched + data[offset + 1:]
    raise RuntimeError("没有可制造连续计数错误的包")


def break_ts_sync(data, breaks=2):
    """把若干个包首的 0x47 改成 0x00：下一个包能立刻重新对齐，sync_loss 会 +1/处。"""
    if len(data) % 188 != 0:
        raise RuntimeError("源 TS 长度不是 188 的整数倍")
    total = len(data) // 188
    out = bytearray(data)
    for i in range(breaks):
        index = total // 3 + i
        out[index * 188] = 0x00
    return bytes(out)


def flv_tag_spans(data):
    """返回 [(data_start, data_end, tag_type, timestamp)]，忽略 PreviousTagSize 校验。"""
    spans = []
    # FLV header: "FLV"(3) + version(1) + flags(1) + DataOffset(4，通常是 9)
    pos = struct.unpack(">I", data[5:9])[0] + 4  # DataOffset + PreviousTagSize0
    if pos > len(data):
        return spans
    while pos + 11 <= len(data):
        tag_type = data[pos] & 0x1F
        size = int.from_bytes(data[pos + 1:pos + 4], "big")
        timestamp = int.from_bytes(data[pos + 4:pos + 7], "big")
        start = pos + 11
        end = start + size
        spans.append((start, end, tag_type, timestamp))
        if end > len(data):
            break
        pos = end + 4
    return spans


def truncate_flv_mid_tag(data):
    """在最后一个完整 tag 的载荷中间截断，保证 truncated_tags > 0。"""
    spans = flv_tag_spans(data)
    for start, end, _tag_type, _ts in reversed(spans):
        if end <= len(data) and end - start > 1:
            return data[:start + (end - start) // 2]
    raise RuntimeError("找不到可截断的 FLV tag")


def build_mxf(ffmpeg):
    r"""MXF（SMPTE 377M / KLV）：MPEG-2 422P@ML 50Mbps + 48kHz PCM。

    为什么参数这么死板：ffmpeg 的 mxf 复用器只接受它认的合法组合，否则直接
    "Could not write header (incorrect codec parameters ?)"：
      * 视频必须是 MPEG-2 4:2:2 且 -b:v 50M（422P@ML 的定码率档）；
      * 音频采样率必须 48000（44.1k 会被拒）；
      * 分辨率太小（176x144）配 50M 也会编码失败，所以用 PAL 720x576。
    时长压到 0.4s：定码率下时长直接决定文件大小，1s 就要几 MB，没必要。

    这份样本要证明的是 container.mxf.* 的正面结论（有 Primer / 有 Index Table /
    有片尾 RIP / 未被截断）在**真实 muxer 产物**上成立 —— 手工拼的 KLV 与解析实现
    同源，两边一起错时测不出来。
    """
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc=size=720x576:rate=25:duration=0.4",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=0.4",
        "-c:v", "mpeg2video", "-pix_fmt", "yuv422p", "-b:v", "50M",
        "-c:a", "pcm_s16le",
        "-f", "mxf",
    ], ".mxf")


def build_ps(ffmpeg):
    r"""MPEG-PS / VOB：MPEG-2 视频 + MP2 音频。

    注意 ffmpeg 的 vob 复用器**不写** Program End Code（00 00 01 B9），收尾靠 padding
    包补齐 —— 这正是 container.ps.* 里不设"缺少 End Code"规则的原因，语料把这个事实
    钉住：一旦哪天改了复用器行为，check() 里的断言会先红。
    音频同样必须 48kHz（VOB 规范）。
    """
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc=size=320x176:rate=25:duration=1",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
        "-c:v", "mpeg2video", "-b:v", "800k",
        "-c:a", "mp2", "-b:a", "96k", "-ar", "48000",
        "-f", "vob",
    ], ".mpg")


def build_flv(ffmpeg):
    r"""FLV：H.264 + AAC。语料里此前只有手工拼的坏 FLV（bad_header / wrong_size），
    没有一份能正常播放的样本 —— container.flv.* 的正面结论从没在真实 muxer 产物上验过。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", VIDEO,
        "-f", "lavfi", "-i", AUDIO,
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "5",
        "-c:a", "aac", "-b:a", "64k",
        "-f", "flv",
    ], ".flv")


def build_asf(ffmpeg):
    r"""ASF / WMV：WMV2 + WMA2。同理，只有手工造的坏 Header，缺正常样本。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", VIDEO,
        "-f", "lavfi", "-i", AUDIO,
        "-c:v", "wmv2", "-c:a", "wmav2", "-b:a", "64k",
        "-f", "asf",
    ], ".wmv")


def build_mov(ffmpeg):
    r"""MOV：H.264 + AAC 的 QuickTime 变体。与 MP4 共用 ISO BMFF 解析器，
    但 ftyp brand / 顶层 box 顺序由另一套 muxer 写出，用来验证 MOv 分派没有走错。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", VIDEO,
        "-f", "lavfi", "-i", AUDIO,
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "5",
        "-c:a", "aac", "-b:a", "64k",
        "-f", "mov",
    ], ".mov")


def build_audio_sample(ffmpeg, codec_args, suffix):
    """纯音频样本的统一入口：没有视频轨，验证"只开音频"不会被打回。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", AUDIO,
    ] + codec_args + ["-f", suffix.lstrip(".").replace("m4a", "ipod")], suffix)


def build_m4a(ffmpeg):
    return build_audio_sample(ffmpeg, ["-c:a", "aac", "-b:a", "96k"], ".m4a")


def build_mp3(ffmpeg):
    return build_audio_sample(ffmpeg, ["-c:a", "libmp3lame", "-b:a", "96k"], ".mp3")


def build_flac(ffmpeg):
    return build_audio_sample(ffmpeg, ["-c:a", "flac"], ".flac")


def build_wav(ffmpeg):
    return build_audio_sample(ffmpeg, ["-c:a", "pcm_s16le"], ".wav")


def build_opus(ffmpeg):
    return build_audio_sample(ffmpeg, ["-c:a", "libopus", "-b:a", "64k"], ".opus")


def build_mp4_faststart(ffmpeg):
    r"""H.264 + AAC 的 faststart MP4：**moov 前置**。

    它是本组异常样本的"健康原片"：只有 moov 在前，才能从文件头拿到完整样本表，
    再去派生"尾部截断 / 中间 chunk 越界 / 缺 moov"三种损伤 —— 这三种正是修复模块
    实际会遇到的真实故障形态。编码沿用 build_h264_bframes 的参数（两个 GOP）。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc2=size=320x176:rate=25:duration=1.2",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1.2",
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "15", "-bf", "3",
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "64k",
        "-movflags", "+faststart",
        "-f", "mp4",
    ], ".mp4")


def build_h264_inband_faststart(ffmpeg):
    r"""MP4 / 纯 H.264，关键帧重复携带 SPS/PPS（repeat-headers=1）。

    "普通 MP4 无参考 moov 重建"的正面样本：mdat 是单轨 AVCC，参数集与 VUI
    timing 都在码流里，重建器才能在没有任何参考文件时推断样本表与时基。
    刻意不混音轨 —— 音视频交织的 mdat 无法在不猜边界的前提下拆开。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc2=size=320x176:rate=25:duration=1.2",
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "15",
        "-x264-params", "repeat-headers=1",
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        "-f", "mp4",
    ], ".mp4")


def build_fmp4(ffmpeg):
    r"""分片 MP4（fMP4）：empty_moov + 每个关键帧一个 moof/mdat。

    实际用于 DASH/HLS 的媒体分片就是这个结构。语料里此前没有真实分片样本，
    "缺初始化段需要参考片"这条修复分支只在合成字节上验过。"""
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc2=size=320x176:rate=25:duration=1.2",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1.2",
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "15",
        "-c:a", "aac", "-b:a", "64k",
        "-movflags", "+frag_keyframe+empty_moov+default_base_moof",
        "-f", "mp4",
    ], ".mp4")


def build_h264_bframes(ffmpeg):
    r"""MP4 / H.264: libx264 + **B 帧** + 两个 GOP。

    存在的理由：帧级 POC 追踪要证明"按 POC 排出来的显示顺序 == 真实显示顺序"，
    而这条结论**只有**在码流里真的有 B 帧时才测得到 —— 没有 B 帧时解码顺序
    与显示顺序天然相同，怎么算都对，测试会变成恒真的空跑。

    ❗ 不能用 TestMediaFactory（tests/support）现场生成：那份样本依赖
    FFmpeg 的预编译库，而仓库锁定的预编译版**没有 libx264**（GPL 组件），
    会降级到 libopenh264 —— 那个编码器不出 B 帧，于是"要 B 帧"的用例
    只能产出一个没有 B 帧的样本。所以这份语料必须由带 libx264 的 ffmpeg
    命令行生成并入库，测试只负责消费。

    封装选 MP4 而不是裸流：裸流的包没有时间戳，而这条对账的参考输出恰恰是
    容器里的 PTS（显示时间戳）—— 有容器才有可对账的参考。
    -g 15 / 30 帧 → 恰好两个 IDR，顺带能验 GOP 分段。
    """
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc2=size=320x176:rate=25:duration=1.2",
        "-c:v", "libx264", "-preset", "veryfast", "-g", "15", "-bf", "3",
        "-pix_fmt", "yuv420p",
        "-f", "mp4",
    ], ".mp4")


def build_hevc_bframes(ffmpeg):
    r"""MP4 / HEVC: libx265 + **B 帧** + 四个 IDR（keyint 12 / 40 帧）。

    与 build_h264_bframes 同样是为帧级 POC 对账准备的，但 HEVC 的 POC 公式、
    slice_segment_header 布局、IRAP 语义整套都不一样（见 HevcPocTracker.h），
    拿 H.264 的样本测不到任何一条 HEVC 专属判据。

    关键参数：
      bframes=3 + b-adapt=0  —— 固定 IBBB P BBB P 结构，POC 回退幅度可预测
      keyint=min-keyint=12   —— 四个 IDR，顺带验"IDR 不参与重排计数"
      scenecut=0             —— 否则 testsrc2 的色块变化会插进额外 I 帧
      repeat-headers=0       —— 参数集**只**写进 hvcC，强制走 extradata 路径
                                （真实 MP4 就是这样；这条路径最容易静默失效）
    封装选 MP4 的理由同 H.264：对账的参考是容器 PTS，裸流没有时间戳。
    """
    return run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc2=size=192x108:rate=25:duration=1.6",
        "-c:v", "libx265", "-preset", "medium",
        "-x265-params",
        "keyint=12:min-keyint=12:bframes=3:b-adapt=0:scenecut=0:repeat-headers=0:log-level=error",
        "-pix_fmt", "yuv420p", "-tag:v", "hvc1",
        "-f", "mp4",
    ], ".mp4")


def build_vp9_altref(ffmpeg):
    r"""WebM / VP9: libvpx-vp9 **两遍编码 + auto-alt-ref**，带 4 个 superframe。

    这份语料是为 VP9 帧级显示顺序准备的，而 VP9 的特殊之处在于**它没有 POC**：
    显示顺序只能靠 show_frame / show_existing_frame 的事件序还原（见
    Vp9FrameOrderTracker.h）。要测到这条路径，码流里必须有"隐藏帧"：

      * alt-ref 帧以 show_frame=0 编码，自己从不显示，只提供参考；
      * 稍后由 show_existing_frame 把它显示出来 —— 这才是 VP9 的重排。

    关键参数（都是试出来的，缺一条就没有隐藏帧，样本会退化成"无重排"）：
      -auto-alt-ref 1 -lag-in-frames 16 -arnr-maxframes 4
      -pass 1 / -pass 2  —— **单遍编码不出 ARF**，实测单遍 50 个包 0 个 superframe，
                            两遍才有 4 个（每个 = ARF + 可见帧）。
      -g 12              —— 4 个 GOP，顺带验 GOP 分段
      -auto-alt-ref 打开时 libvpx 会把 [ARF, 可见帧] 打成一个 **superframe**，
      所以"容器包数"（50）会**少于**"已编码帧数"（54）。测试用例正是拿这个
      差值来证明 superframe 真的被拆开了 —— 不拆的话 frames_observed 会等于 50。

    封装选 WebM 而不是 IVF：WebM 是一等容器（本项目有完整解析链路），
    IVF 只是个裸流外壳。
    """
    # 两遍编码：第一遍只做统计（输出丢弃），第二遍才是产物。
    # ❗ passlog 必须显式指定并清掉，否则会在仓库工作目录里留一堆
    # ffmpeg2pass-0.log —— 那东西会被 git status 认成未跟踪文件。
    passlog = os.path.join(tempfile.gettempdir(), "ve_vp9_altref_passlog")
    common = [
        "-f", "lavfi", "-i", "testsrc2=size=160x96:rate=25:duration=2",
        "-c:v", "libvpx-vp9", "-b:v", "300k", "-g", "12", "-keyint_min", "12",
        "-auto-alt-ref", "1", "-lag-in-frames", "16", "-arnr-maxframes", "4",
        "-passlogfile", passlog, "-an",
    ]
    try:
        run_ffmpeg(ffmpeg, common + ["-pass", "1", "-f", "null"], ".log")
        return run_ffmpeg(ffmpeg, common + ["-pass", "2", "-f", "webm"], ".webm")
    finally:
        for extra in ("-0.log", "-0.log.mbtree"):
            path = passlog + extra
            if os.path.exists(path):
                os.unlink(path)


def build_av1_altref(ffmpeg):
    r"""MP4 / AV1: libaom-av1 **两遍编码 + auto-alt-ref**，带隐藏帧（alt-ref）。

    这份语料是为 AV1 帧级显示顺序准备的。AV1 与 VP9 的关键差别是它**有**
    order_hint 这个真字段，所以测试能第一次拿"码流里读出来的顺序值"去对
    容器 PTS，而不只是靠显示事件序反推。

    关键参数：
      -auto-alt-ref 1 -lag-in-frames 16 —— libaom 的 alt-ref **只在两遍编码下
                                          才生效**（"2-pass only"），单遍跑
                                          不会产出隐藏帧，样本会退化成
                                          "解码顺序 == 显示顺序"的空跑。
      -g 12                             —— 两个关键帧，顺带验 GOP 分段
      -cpu-used 8                       —— 语料生成不该成为 CI / 本地最慢的一步

    ❗ passlog 必须显式指向临时目录并清理，否则会在仓库根目录留下
    ffmpeg2pass-0.log（VP9 那一次就踩过）。

    封装选 MP4：AV1 在 MP4 里是**一包一帧**（不像 VP9 有 superframe），
    于是"扫到的帧数 == 容器包数"本身就是一条有用的断言。
    """
    passlog = os.path.join(tempfile.gettempdir(), "ve_av1_altref_passlog")
    common = [
        "-f", "lavfi", "-i", "testsrc2=size=176x96:rate=25:duration=2.4",
        "-c:v", "libaom-av1", "-b:v", "300k", "-g", "12",
        "-cpu-used", "8", "-row-mt", "1",
        "-auto-alt-ref", "1", "-lag-in-frames", "16",
        "-passlogfile", passlog, "-an",
    ]
    try:
        run_ffmpeg(ffmpeg, common + ["-pass", "1", "-f", "null"], ".log")
        return run_ffmpeg(ffmpeg, common + ["-pass", "2", "-pix_fmt", "yuv420p", "-f", "mp4"], ".mp4")
    finally:
        for extra in ("-0.log", "-0.log.mbtree"):
            path = passlog + extra
            if os.path.exists(path):
                os.unlink(path)


register("container/real_h264_aac.ts", "ts", build=build_ts)
register("container/real_h264_aac.m2ts", "m2ts", derived="container/real_h264_aac.ts")
register("container/real_mpeg4_mp3.avi", "avi", build=build_avi)
register("container/real_vorbis.ogg", "ogg", build=build_ogg)
register("container/real_h264_aac.mkv", "mkv", build=build_mkv)
register("container/real_mpeg2_pcm.mxf", "mxf", build=build_mxf)
register("container/real_mpeg2_mp2.mpg", "ps", build=build_ps)
register("container/real_h264_aac.flv", "flv", build=build_flv)
register("container/real_wmv2_wma.wmv", "asf", build=build_asf)
register("container/real_h264_aac.mov", "mov", build=build_mov)
register("container/real_h264_aac_faststart.mp4", "mp4", build=build_mp4_faststart)
register("container/real_h264_aac_frag.mp4", "fmp4", build=build_fmp4)
register("audio/real_aac.m4a", "audio", build=build_m4a)
register("audio/real_mp3.mp3", "audio", build=build_mp3)
register("audio/real_flac.flac", "audio", build=build_flac)
register("audio/real_pcm.wav", "audio", build=build_wav)
register("audio/real_opus.opus", "audio", build=build_opus)

# ---- 异常样本：全部从上面的真实样本派生，保留真实 moov / 索引 / tag 链 ----
register("container/real_h264_aac_tail_truncated.mp4", "mp4_tail_truncated",
         derived="container/real_h264_aac_faststart.mp4",
         derive=lambda d: truncate_tail(d, 0.7))
register("container/real_h264_aac_missing_moov.mp4", "mp4_missing_moov",
         derived="container/real_h264_aac_faststart.mp4",
         derive=lambda d: drop_top_level_box(d, b"moov"))
# 无参考 moov 重建的正面样本：单轨视频 + 关键帧内嵌 SPS/PPS + SPS VUI timing。
register("container/real_h264_inband_faststart.mp4", "mp4", build=build_h264_inband_faststart)
register("container/real_h264_inband_missing_moov.mp4", "mp4_missing_moov",
         derived="container/real_h264_inband_faststart.mp4",
         derive=lambda d: drop_top_level_box(d, b"moov"))
register("container/real_h264_aac_mid_damaged.mp4", "mp4_mid_damaged",
         derived="container/real_h264_aac_faststart.mp4",
         # 0.45 命中第 13 个视频 chunk（约 0.52 s），避开 0.6 s 的关键帧；
         # 只破坏视频轨，保留音频索引，跳段后才能读到损坏点之后的关键帧与音频。
         derive=lambda d: patch_chunk_offsets(d, 0.45, only_first_track=True))
register("container/real_h264_aac_frag_orphan.mp4", "fmp4_orphan",
         derived="container/real_h264_aac_frag.mp4",
         derive=lambda d: drop_top_level_box(drop_top_level_box(d, b"ftyp"), b"moov"))
register("container/real_h264_aac_cc_error.ts", "ts_continuity",
         derived="container/real_h264_aac.ts", derive=corrupt_ts_continuity)
register("container/real_h264_aac_sync_loss.ts", "ts_sync_loss",
         derived="container/real_h264_aac.ts", derive=break_ts_sync)
register("container/real_h264_aac_truncated.flv", "flv_truncated",
         derived="container/real_h264_aac.flv", derive=truncate_flv_mid_tag)
register("container/real_mpeg4_mp3_truncated.avi", "avi_truncated",
         derived="container/real_mpeg4_mp3.avi",
         derive=lambda d: truncate_tail(d, 0.75))
# 扩展名与实际容器不符：内容是 TS，文件名是 .mp4（container.extension_mismatch 的端到端样本）
register("container/real_h264_aac_as.mp4", "ts_mismatch",
         derived="container/real_h264_aac.ts", derive=lambda d: d)

register("codec/real_x264_bframes_gop.mp4", "mp4", build=build_h264_bframes)
register("codec/real_x265_bframes_gop.mp4", "mp4", build=build_hevc_bframes)
register("codec/real_vp9_altref_gop.webm", "mkv", build=build_vp9_altref)
register("codec/real_av1_altref_gop.mp4", "mp4", build=build_av1_altref)


# ---------------------------------------------------------------------------
# 校验：语义级
# ---------------------------------------------------------------------------

def nal_type_counts(data):
    """统计裸 H.264（Annex B）里各 NAL 类型的个数。

    只按 3 字节起始码扫（00 00 01）：4 字节起始码是它的超集，跳过前面那个 0
    之后同样能被匹配到。够用于结构校验 —— 这里要的是"该有的类型在不在"，
    不是精确的 NAL 边界。
    """
    counts = {}
    pos = 0
    while True:
        found = data.find(b"\x00\x00\x01", pos)
        if found < 0:
            return counts
        header = found + 3
        if header >= len(data):
            return counts
        nal_type = data[header] & 0x1F
        counts[nal_type] = counts.get(nal_type, 0) + 1
        pos = header + 1


def mp4_top_level_boxes(data):
    """返回顶层 box 列表 [(type, payload_start, payload_end)]。

    只处理标准头（32 位 size）与 64 位 largesize（size == 1）；
    size == 0（box 延伸到文件尾）也支持。这是 ISO BMFF 的最小子集，
    够用来确认"文件结构完整"，不追求覆盖所有边角。
    """
    boxes = []
    pos = 0
    while pos + 8 <= len(data):
        size = struct.unpack(">I", data[pos:pos + 4])[0]
        name = data[pos + 4:pos + 8]
        header = 8
        if size == 1:
            if pos + 16 > len(data):
                break
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
            header = 16
        elif size == 0:
            size = len(data) - pos
        if size < header or pos + size > len(data):
            break
        boxes.append((name, pos + header, pos + size))
        pos += size
    return boxes


def mp4_box_ranges_lenient(data):
    """同 mp4_box_ranges，但允许最后一个 box 声明长度越过文件末尾（截断样本要用）。"""
    boxes = []
    pos = 0
    while pos + 8 <= len(data):
        size = struct.unpack(">I", data[pos:pos + 4])[0]
        name = data[pos + 4:pos + 8]
        if size == 1:
            if pos + 16 > len(data):
                break
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
        elif size == 0:
            size = len(data) - pos
        boxes.append((name, pos, pos + size))
        if size < 8 or pos + size > len(data):
            break
        pos += size
    return boxes


def mp4_chunk_offsets(data):
    """读出 stco/co64 的全部 chunk offset（用于确认"中间损坏"样本真的越界）。"""
    offsets = []
    for tag, entry_size in ((b"stco", 4), (b"co64", 8)):
        idx = data.find(tag)
        while idx != -1:
            if idx >= 4:
                size = struct.unpack(">I", data[idx - 4:idx])[0]
                count = struct.unpack(">I", data[idx + 8:idx + 12])[0]
                if idx + 12 + count * entry_size <= idx - 4 + size:
                    for i in range(count):
                        at = idx + 12 + i * entry_size
                        if entry_size == 4:
                            offsets.append(struct.unpack(">I", data[at:at + 4])[0])
                        else:
                            offsets.append(struct.unpack(">Q", data[at:at + 8])[0])
            idx = data.find(tag, idx + 4)
    return offsets


def flv_tag_stats(data):
    """FLV tag 链的语义统计：tag 数、越过文件尾的 tag、同类时间戳回退。"""
    stats = {"tags": 0, "truncated": 0, "timestamp_backwards": 0}
    if data[0:3] != b"FLV":
        return stats
    pos = struct.unpack(">I", data[5:9])[0] + 4
    last_ts = {}
    while pos + 11 <= len(data):
        tag_type = data[pos] & 0x1F
        size = int.from_bytes(data[pos + 1:pos + 4], "big")
        timestamp = int.from_bytes(data[pos + 4:pos + 7], "big")
        if pos + 11 + size > len(data):
            stats["truncated"] += 1
            return stats
        if tag_type in (8, 9):
            if tag_type in last_ts and timestamp < last_ts[tag_type]:
                stats["timestamp_backwards"] += 1
            last_ts[tag_type] = timestamp
        stats["tags"] += 1
        pos = pos + 11 + size + 4
    if pos < len(data) and len(data) - pos > 0:
        stats["truncated"] += 1
    return stats


def ts_continuity_error_count(data, stride=188, offset=0):
    """按 PID 重算 continuity_counter 错误（discontinuity_indicator 置位时跳过）。"""
    last_cc = {}
    errors = 0
    for pos in range(offset, len(data) - stride + 1, stride):
        packet = data[pos:pos + stride]
        if packet[0] != 0x47:
            continue
        pid = _ts_pid(packet)
        if pid == 0x1FFF:
            continue
        afc = (packet[3] >> 4) & 0x03
        if afc not in (1, 3):
            continue
        if afc == 3:
            af_len = packet[4]
            if af_len > 0 and (packet[5] & 0x80):
                last_cc.pop(pid, None)
                continue
        cc = packet[3] & 0x0F
        previous = last_cc.get(pid)
        if previous is not None and (previous + 1) % 16 != cc:
            errors += 1
        last_cc[pid] = cc
    return errors


def check_audio(data, rel):
    problems = []
    ext = os.path.splitext(rel)[1].lower()
    if ext == ".m4a":
        boxes = mp4_top_level_boxes(data)
        names = [name for name, _start, _end in boxes]
        if not names or names[0] != b"ftyp" or b"moov" not in names:
            problems.append("%s: 不是完整的 M4A（顶层 box: %s）" % (rel, names))
    elif ext == ".mp3":
        if data[0:3] != b"ID3" and not (len(data) > 1 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0):
            problems.append("%s: 不是 MP3（开头 %r）" % (rel, data[0:3]))
    elif ext == ".flac":
        if data[0:4] != b"fLaC":
            problems.append("%s: 不是 FLAC（开头 %r）" % (rel, data[0:4]))
    elif ext == ".wav":
        if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
            problems.append("%s: 不是 WAVE（开头 %r）" % (rel, data[0:12]))
    elif ext == ".opus":
        if data[0:4] != b"OggS" or data.find(b"OpusHead") < 0:
            problems.append("%s: 不是 Ogg Opus（缺 OggS / OpusHead）" % rel)
    else:
        problems.append("%s: 未知音频扩展名 %s" % (rel, ext))
    return problems


def check(data, rel):
    """返回问题列表（空 = 合格）。"""
    kind = SAMPLES[rel]["kind"]
    problems = []

    if kind in ("ts", "m2ts", "ts_mismatch", "ts_continuity", "ts_sync_loss"):
        stride = 192 if kind == "m2ts" else 188
        offset = 4 if kind == "m2ts" else 0   # M2TS 的同步字节落在 TP_extra 之后
        if len(data) < stride * 8:
            problems.append("%s: 太小（%d 字节），装不下 8 个包" % (rel, len(data)))
            return problems
        if len(data) % stride != 0:
            problems.append("%s: 长度 %d 不是步长 %d 的整数倍（可能被截断）"
                            % (rel, len(data), stride))
        bad = [i for i in range(offset, len(data), stride) if data[i] != 0x47]
        if kind == "ts_sync_loss":
            # 这份样本的全部意义就是"同步字节被破坏"，必须至少有 1 处。
            if not bad:
                problems.append("%s: 没有人为破坏的同步字节，测不到失步路径" % rel)
        elif bad:
            problems.append("%s: %d 个位置没有同步字节 0x47（步长 %d 不对？）"
                            % (rel, len(bad), stride))
        if kind == "ts_continuity":
            if ts_continuity_error_count(data, stride, offset) == 0:
                problems.append("%s: 没有连续计数错误，测不到 container.ts.continuity_error" % rel)
        return problems

    if kind in ("flv", "flv_truncated"):
        if data[0:3] != b"FLV":
            problems.append("%s: 不是 FLV（开头 %r）" % (rel, data[0:3]))
            return problems
        stats = flv_tag_stats(data)
        if stats["tags"] < 4:
            problems.append("%s: 只解析到 %d 个 tag，样本太小" % (rel, stats["tags"]))
        if kind == "flv":
            if stats["truncated"] != 0:
                problems.append("%s: 正常样本里出现了 %d 个越界 tag"
                                % (rel, stats["truncated"]))
            if stats["timestamp_backwards"] != 0:
                problems.append("%s: 正常样本里出现 %d 次时间戳回退"
                                % (rel, stats["timestamp_backwards"]))
        else:
            if stats["truncated"] == 0:
                problems.append("%s: 没有越界 tag，测不到 container.flv.truncated_tags" % rel)
        return problems

    if kind == "asf":
        header_guid = bytes.fromhex("3026B2758E66CF11A6D900AA0062CE6C")
        data_guid = bytes.fromhex("3626B2758E66CF11A6D900AA0062CE6C")
        if data[0:16] != header_guid:
            problems.append("%s: 不是 ASF Header Object（开头 %r）"
                            % (rel, data[0:16]))
            return problems
        if data.find(data_guid) < 0:
            problems.append("%s: 找不到 Data Object，ASF 结构不完整" % rel)
        return problems

    if kind == "audio":
        return check_audio(data, rel)

    if kind == "avi":
        if data[0:4] != b"RIFF":
            problems.append("%s: 不是 RIFF（开头 %r）" % (rel, data[0:4]))
        if data[8:12] != b"AVI ":
            problems.append("%s: RIFF form 不是 'AVI '（%r）" % (rel, data[8:12]))
        if b"movi" not in data:
            problems.append("%s: 没有 movi 块，AVI 结构不完整" % rel)
        return problems

    if kind == "avi_truncated":
        if data[0:4] != b"RIFF" or data[8:12] != b"AVI ":
            problems.append("%s: 不是 AVI（开头 %r）" % (rel, data[0:12]))
            return problems
        declared = struct.unpack("<I", data[4:8])[0] + 8
        if declared <= len(data):
            problems.append("%s: RIFF 声明长度 %d 未超过实际 %d 字节，没有真正截断"
                            % (rel, declared, len(data)))
        return problems

    if kind == "ogg":
        if data[0:4] != b"OggS":
            problems.append("%s: 不是 OggS 页（开头 %r）" % (rel, data[0:4]))
        if data.count(b"OggS") < 2:
            problems.append("%s: 只有 %d 个 OggS 页，测不出页链"
                            % (rel, data.count(b"OggS")))
        return problems

    if kind == "mkv":
        # 只校验"这是 Matroska、骨架元素都在"。深度解析的正确性由 C++ 用例断言
        # （解析器和这里各写一遍 EBML 走查，才是真的交叉验证）。
        if data[0:4] != b"\x1a\x45\xdf\xa3":
            problems.append("%s: 不是 EBML 头（开头 %r）" % (rel, data[0:4]))
            return problems
        # WebM 只是 Matroska 的一个受限子集：DocType 是 "webm" 而不是
        # "matroska"，元素骨架完全一样（VP9 的样本就是 webm）。
        if b"matroska" not in data[:256] and b"webm" not in data[:256]:
            problems.append("%s: EBML 头里 DocType 既不是 matroska 也不是 webm" % rel)
        # Cluster ID 0x1F43B675 / Cues ID 0x1C53BB6B。ffmpeg 默认把 Cues 写在片尾，
        # 少了它 container.mkv.missing_cues 在真实样本上就无从判定。
        for elem_id, name in ((b"\x1f\x43\xb6\x75", "Cluster"),
                              (b"\x1c\x53\xbb\x6b", "Cues")):
            if data.find(elem_id) < 0:
                problems.append("%s: 找不到 %s 元素（%r）"
                                % (rel, name, elem_id))
        return problems

    if kind == "mxf":
        # 只校验"这是一份结构完整的 MXF"：分区头/尾 + Primer + Index Table + 片尾 RIP。
        # 只比 UID 的 4..13 字节：末 3 字节是 <kind><status><reserved>，status 有
        # 开放/封闭 × 完整/不完整四种取值（ffmpeg 写的是 0x04 = 封闭且完整），
        # 把 status 写死进断言会在换 muxer 时误报。
        if data[0:4] != b"\x06\x0e\x2b\x34":
            problems.append("%s: 不是 SMPTE UL 键开头（%r）" % (rel, data[0:4]))
            return problems
        for tail, name in (
                (b"\x0d\x01\x02\x01\x01\x02", "Header Partition"),
                (b"\x0d\x01\x02\x01\x01\x04", "Footer Partition"),
                (b"\x0d\x01\x02\x01\x01\x05", "Primer Pack"),
                (b"\x02\x53\x01\x01\x0d\x01\x02\x01\x01", "Index Table Segment"),
        ):
            if data.find(tail) < 0:
                problems.append("%s: 找不到 %s" % (rel, name))
        # RIP 必须在文件尾（它是最后一个 KLV），否则 container.mxf.missing_rip
        # 的"反读文件尾"逻辑在这份样本上就无从验证。
        rip_tail = b"\x0d\x01\x02\x01\x01\x11"
        if data.rfind(rip_tail) < 0:
            problems.append("%s: 找不到片尾 Random Index Pack" % rel)
        elif len(data) - data.rfind(rip_tail) > 1024:
            problems.append("%s: RIP 离文件尾 %d 字节，不在尾部（反读窗口会漏）"
                            % (rel, len(data) - data.rfind(rip_tail)))
        return problems

    if kind == "ps":
        # PS 没有索引，只能校验骨架：Pack Header + System Header + 视频/音频 PES。
        # "没有 Program End Code"是 ffmpeg vob 复用器的正常行为，这里不判。
        if data[0:4] != b"\x00\x00\x01\xba":
            problems.append("%s: 不是 Pack Header 开头（%r）" % (rel, data[0:4]))
            return problems
        for code, name in ((b"\x00\x00\x01\xbb", "System Header"),
                           (b"\x00\x00\x01\xe0", "视频 PES"),
                           (b"\x00\x00\x01\xc0", "音频 PES")):
            if data.find(code) < 0:
                problems.append("%s: 找不到 %s（%r）" % (rel, name, code))
        return problems

    if kind == "h264_annexb":
        # 裸流没有容器层可校验，只能校验"码流里该有的东西都在"：
        # 起始码开头 + SPS/PPS 各至少一份 + 至少两个 IDR（两个 GOP）+ 足够多的 slice。
        # 少了任何一类，"帧顺序"的用例都会退化成恒真的空跑。
        if data[0:4] not in (b"\x00\x00\x00\x01", b"\x00\x00\x01"):
            problems.append("%s: 不是 Annex B（开头 %r）" % (rel, data[0:4]))
            return problems
        types = nal_type_counts(data)
        for nal_type, name, minimum in ((7, "SPS", 1), (8, "PPS", 1),
                                        (5, "IDR slice", 2), (1, "非 IDR slice", 10)):
            if types.get(nal_type, 0) < minimum:
                problems.append("%s: %s 只有 %d 个（至少需要 %d 个）"
                                % (rel, name, types.get(nal_type, 0), minimum))
        return problems

    if kind in ("mp4", "mov"):
        # 只走顶层 box（递归进 moov 没必要：这里要证明的是"这是个能解的视频文件"，
        # 帧层面的东西由 C++ 用例去断言）。
        boxes = mp4_top_level_boxes(data)
        names = [name for name, _start, _end in boxes]
        if not boxes:
            problems.append("%s: 一个顶层 box 都没解出来（不是 MP4？开头 %r）"
                            % (rel, data[0:8]))
            return problems
        if names[0] != b"ftyp":
            problems.append("%s: 第一个顶层 box 是 %r，不是 ftyp" % (rel, names[0]))
        for required in (b"moov", b"mdat"):
            if required not in names:
                problems.append("%s: 缺 %s box（顶层只有 %s）"
                                % (rel, required.decode("ascii"),
                                   ", ".join(n.decode("ascii") for n in names)))
        # 带 B 帧的样本必须能被解出 B 帧，否则帧顺序用例是恒真空跑。
        # 这里用 avcC 之外的信息判不了，交给 C++ 用例的 ASSERT_GT(b_count, 0) 兜底。
        return problems

    if kind == "mp4_tail_truncated":
        boxes = mp4_box_ranges_lenient(data)
        names = [name for name, _start, _end in boxes]
        if b"ftyp" not in names or b"moov" not in names:
            problems.append("%s: 截断后必须保留 ftyp + moov（顶层只有 %s）"
                            % (rel, "、".join(n.decode("ascii") for n in names)))
        elif boxes[-1][0] != b"mdat" or boxes[-1][2] <= len(data):
            problems.append("%s: 最后一个 mdat 声明 %d 字节，未越过实际 %d 字节 —— "
                            "没有真正截断" % (rel, boxes[-1][2] - boxes[-1][1], len(data)))
        return problems

    if kind == "mp4_missing_moov":
        boxes = mp4_top_level_boxes(data)
        names = [name for name, _start, _end in boxes]
        if b"moov" in names:
            problems.append("%s: moov 还在，不是「缺 moov」样本" % rel)
        if b"mdat" not in names:
            problems.append("%s: 只剩 %s，修复模块测不到「无元数据」路径"
                            % (rel, "、".join(n.decode("ascii") for n in names)))
        return problems

    if kind == "mp4_mid_damaged":
        boxes = mp4_top_level_boxes(data)
        names = [name for name, _start, _end in boxes]
        for required in (b"ftyp", b"moov", b"mdat"):
            if required not in names:
                problems.append("%s: 缺 %s box，结构损坏不止「中间一段」"
                                % (rel, required.decode("ascii")))
        offsets = mp4_chunk_offsets(data)
        if not any(offset >= len(data) for offset in offsets):
            problems.append("%s: 没有越界的 chunk offset，测不到跳段恢复" % rel)
        return problems

    if kind == "fmp4":
        boxes = mp4_top_level_boxes(data)
        names = [name for name, _start, _end in boxes]
        if b"ftyp" not in names or b"moov" not in names:
            problems.append("%s: 分片 MP4 缺 ftyp/moov 初始化段" % rel)
        if not any(name == b"moof" for name in names):
            problems.append("%s: 没有 moof 媒体分片" % rel)
        return problems

    if kind == "fmp4_orphan":
        boxes = mp4_box_ranges_lenient(data)
        names = [name for name, _start, _end in boxes]
        if b"moov" in names or b"ftyp" in names:
            problems.append("%s: 初始化段还在，不是孤立媒体分片" % rel)
        if not names or names[0] != b"moof":
            problems.append("%s: 第一个顶层 box 是 %r，不是 moof"
                            % (rel, names[0] if names else b""))
        return problems

    problems.append("%s: 未知样本类型 %s" % (rel, kind))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只校验磁盘样本是否仍然合格（不比字节，比结构）")
    args = ap.parse_args()

    missing = [rel for rel in SAMPLES
               if not os.path.exists(os.path.join(CORPUS_DIR, rel))]

    if args.check:
        if missing:
            print("SKIP: 真实样本缺失（%s），本机没有生成过"
                  % ", ".join(sorted(missing)))
            return 0
        problems = []
        for rel in sorted(SAMPLES):
            with open(os.path.join(CORPUS_DIR, rel), "rb") as fh:
                problems += check(fh.read(), rel)
        if problems:
            print("真实语料校验失败:", file=sys.stderr)
            for p in problems:
                print("  " + p, file=sys.stderr)
            print("重新生成: python tests/corpus/generate_real_corpus.py",
                  file=sys.stderr)
            return 1
        print("真实语料校验通过: %d 个样本结构合格" % len(SAMPLES))
        return 0

    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        if missing:
            print("SKIP: 找不到 ffmpeg，且真实样本缺失（%s）"
                  % ", ".join(sorted(missing)))
            return 0
        print("样本已存在，未找到 ffmpeg，跳过生成")
        return 0

    produced = {}
    # 两趟：derived 的样本要吃别人的产物，而字典序恰好把 real_*.m2ts 排在
    # real_*.ts 前面（"m" < "t"），一趟循环会 KeyError。先出所有原始样本。
    for rel in sorted(SAMPLES):
        if SAMPLES[rel]["derived"] is None:
            produced[rel] = SAMPLES[rel]["build"](ffmpeg)
    for rel in sorted(SAMPLES):
        spec = SAMPLES[rel]
        if spec["derived"] is not None:
            source = produced[spec["derived"]]
            produced[rel] = spec["derive"](source) if spec["derive"] else build_m2ts(source)

    for rel, data in sorted(produced.items()):
        problems = check(data, rel)
        if problems:
            print("生成结果自检失败:", file=sys.stderr)
            for p in problems:
                print("  " + p, file=sys.stderr)
            return 1
        path = os.path.join(CORPUS_DIR, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)

    print("已生成 %d 个真实样本 -> %s"
          % (len(produced), CORPUS_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
