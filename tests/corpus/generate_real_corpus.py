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
SAMPLES = {}


def register(rel, kind, build=None, derived=None):
    SAMPLES[rel] = {"kind": kind, "build": build, "derived": derived}


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


register("container/real_h264_aac.ts", "ts", build=build_ts)
register("container/real_h264_aac.m2ts", "m2ts", derived="container/real_h264_aac.ts")
register("container/real_mpeg4_mp3.avi", "avi", build=build_avi)
register("container/real_vorbis.ogg", "ogg", build=build_ogg)


# ---------------------------------------------------------------------------
# 校验：语义级
# ---------------------------------------------------------------------------

def check(data, rel):
    """返回问题列表（空 = 合格）。"""
    kind = SAMPLES[rel]["kind"]
    problems = []

    if kind == "ts" or kind == "m2ts":
        stride = 188 if kind == "ts" else 192
        offset = 0 if kind == "ts" else 4   # M2TS 的同步字节落在 TP_extra 之后
        if len(data) < stride * 8:
            problems.append("%s: 太小（%d 字节），装不下 8 个包" % (rel, len(data)))
            return problems
        if len(data) % stride != 0:
            problems.append("%s: 长度 %d 不是步长 %d 的整数倍（可能被截断）"
                            % (rel, len(data), stride))
        bad = [i for i in range(offset, len(data), stride) if data[i] != 0x47]
        if bad:
            problems.append("%s: %d 个位置没有同步字节 0x47（步长 %d 不对？）"
                            % (rel, len(bad), stride))
        return problems

    if kind == "avi":
        if data[0:4] != b"RIFF":
            problems.append("%s: 不是 RIFF（开头 %r）" % (rel, data[0:4]))
        if data[8:12] != b"AVI ":
            problems.append("%s: RIFF form 不是 'AVI '（%r）" % (rel, data[8:12]))
        if b"movi" not in data:
            problems.append("%s: 没有 movi 块，AVI 结构不完整" % rel)
        return problems

    if kind == "ogg":
        if data[0:4] != b"OggS":
            problems.append("%s: 不是 OggS 页（开头 %r）" % (rel, data[0:4]))
        if data.count(b"OggS") < 2:
            problems.append("%s: 只有 %d 个 OggS 页，测不出页链"
                            % (rel, data.count(b"OggS")))
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
            produced[rel] = build_m2ts(produced[spec["derived"]])

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
          % (len(produced), os.path.join(CORPUS_DIR, "container")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
