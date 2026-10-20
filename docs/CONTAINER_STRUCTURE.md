# 容器结构解析

这一层回答的是"这个文件/流里面长什么样"：box／EBML／RIFF／TS 包的层次、每层的关键字段、
以及"按容器自己的规范就该成立"的那些一致性结论。

代码在 `core/analysis/container/`（各格式解析器 + 统一调度），检测规则在
`core/analysis/orchestration/FormatDetector.*`，MP4 的底层 box 读取在
`core/media/container/IsobmffParser.*`（不依赖 FFmpeg，可被纯单测覆盖）。

## 1. 检测与分发

`FormatDetector::Detect()` **先魔数、后扩展名**。魔数认不出的格式会回退到
`ContainerStructureAnalyzer::AnalyzeWithFFmpeg()`，那条路只有流列表 + metadata，
没有结构树 —— 这是"能打开"和"能解析"的分界，UI 上要能看出来。

| 格式 | 魔数判据 | 解析器 |
|------|---------|--------|
| MP4 / MOV | `ftyp`（+ brand 区分 MOV） | `Mp4BoxAnalyzer` + `Mp4SampleTableAnalyzer` |
| fMP4 分片 | 首盒 `styp` / `moof` | 同上（没有 `moov` 时按分片展示） |
| MKV / WebM | EBML `1A 45 DF A3`（DocType 区分） | `EbmlAnalyzer` |
| AVI | `RIFF....AVI ` | `AviStructureAnalyzer` |
| FLV | `FLV` | `FlvStructureAnalyzer` |
| MPEG-TS | `0x47` 等步长重复（**188 / 192 / 204 三种步长都试**） | `TsStructureAnalyzer` |
| ASF / WMV | `30 26 B2 75 8E 66 CF 11` | `AsfStructureAnalyzer` |
| OGG | `OggS` | `OggStructureAnalyzer` |
| MPEG-PS | `00 00 01 BA`（Pack Header） | `MpegPsStructureAnalyzer` |
| MXF | `06 0E 2B 34`（Partition Pack UL） | `MxfStructureAnalyzer` |
| 裸流 | `00 00 00 01` / `00 00 01` + 合理的 NAL 头 + 第二个起始码 | 走 FFmpeg（无结构树） |
| HLS / DASH | **只能靠扩展名**（`.m3u8` / `.mpd` 是纯文本） | `HlsManifestAnalyzer` / `DashManifestAnalyzer` |

2026-10 补的三条检测细节：

- TS 的 192 字节包（M2TS/AVCHD，同步字节在偏移 4）与 204 字节包（RS 纠错）
  以前认不出来，`.m2ts` 只能靠扩展名兜底，然后被按 188 步长解析、报"未找到同步字节"。
- 扩展名映射里 `f4v` 曾同时出现在 MP4 与 FLV 两个分支，后者是死代码。
- 扩展名表以前没有 `mpg` / `vob` / `mxf`，PS 与 MXF 文件只能落到"未知格式"再被
  FFmpeg 兜底；裸流（`.h264` / `.h265` / `.av1` …）同理。现在三种都能在检测层
  给出格式名，PS / MXF 还会走自己的结构解析器。

## 2. 每种格式的深度

"深度"指**解析到什么级别**，不是"有没有解析器"：

| 格式 | 深度 | 主要限制 |
|------|------|---------|
| MP4 / MOV / fMP4 | 深：box 树到字段级 + 样本表交叉校验（stbl / elst / moof-traf-trun / faststart） | 递归深度上限 8；单表条目上限 `max_entries_per_table` |
| MKV / WebM | 深：SeekHead / Info / Tracks(Video/Audio/Colour/Mastering) / Cluster / Cues / Chapters / Tags / Attachments | `BlockAdditions` 只列节点不展开 |
| MPEG-TS | 中深：包长自适应、失步重同步、PAT/PMT 跨包组装、PSI 描述符、SDT、PCR、CC 统计 | 默认只扫前 50 万包（可配）；不做 PES 载荷解析 |
| FLV | 中深：AMF0 `onMetaData`、Enhanced-FLV fourcc（HEVC/AV1/VP9）、tag 级时间戳/截断扫描 | Script Tag 只取标量键值 |
| AVI | 中深：RIFF chunk 树 + `strh`/`strf` + `idx1`/`indx`/`dmlh`/`AVIX` 索引校验 | `movi` 载荷不展开；QC 扫描跳过媒体数据 |
| ASF / WMV | 中深：7 类对象 GUID 有名字与字段、对象大小/Data Object/时长校验 | Header Extension / Metadata / DRM 归 `Unknown Object` |
| OGG | 中深：Vorbis / Opus / FLAC / Theora / Speex 标识头 + 注释块 + 页序号/BOS/EOS/截断扫描 | 只读每页前 4 KiB 数据（QC 只扫页头） |
| MPEG-PS | 中：Pack / System Header / PSM 映射 / PES 边界 + PTS 首末值 | 默认只扫前 32 MB；不解析 PES 载荷 |
| MXF | 中：KLV 对象流（Partition / Primer / Index / Essence）+ BER 长度校验 | 默认只扫前 64 MB；不解码 essence |

### MP4 的字段级覆盖

`IsobmffBox::fields` 是"规范字段名 → 可读值"。展示层只负责显示，不再自己判 `type`。
已填字段的类型：

```
ftyp/styp   major_brand, minor_version, compatible_brands
mvhd        version, timescale, duration, duration_seconds, next_track_id
tkhd        version, flags(enabled/in_movie/…), track_id, duration, width, height
mdhd        version, timescale, duration, language
hdlr        handler_type, name
sidx        reference_id, timescale, earliest_presentation_time, first_offset, reference_count
mfhd        sequence_number
tfhd/tfdt/trun  flags, track_id, base_media_decode_time, sample_count, data_offset
avcC        configuration_version, profile_idc, profile_compatibility, level_idc,
            length_size_minus_one, num_of_sps, num_of_pps
hvcC        general_profile_space/tier_flag/profile_idc/level_idc,
            min_spatial_segmentation_idc, parallelism_type, chroma_format_idc,
            bit_depth_luma/chroma_minus8, num_temporal_layers, length_size_minus_one
av1C        seq_profile, seq_level_idx_0, seq_tier_0, bit_depth, monochrome,
            chroma_subsampling_x/y, chroma_sample_position
esds        object_type_indication, stream_type, buffer_size_db, max_bitrate, avg_bitrate
colr        colour_type, colour_primaries, transfer_characteristics, matrix_coefficients, full_range_flag
pasp        hSpacing, vSpacing
btrt        bufferSizeDB, maxBitrate, avgBitrate
```

关键一点：**`stsd` 的 sample entry（`avc1` / `hvc1` / `mp4a` …）也是容器**。
它们以前不算容器，所以 `avcC`/`colr`/`pasp`/`btrt` 整棵子树从来没进过 box 树。
子 box 的起点按 entry 种类不同（视频 78 字节、音频按版本 28/44/64 字节），
见 `SampleEntryChildOffset()`。

`stsd` 允许声明多个 sample entry（Dolby Vision 双层、多码流打包等），样本通过
`stsc` 的 `sample_description_index` 选择用哪一条。`IsobmffTrack::sample_descriptions`
按声明顺序保留全部 fourcc，`codec` 仍取第一条（兼容既有展示与规则）；结构树本来就
能看到全部入口。轨道含多条描述时，容器规则会以 `container.mp4.multiple_sample_entries`
（Info）提示"codec 只代表第一条"。

## 3. MPEG-TS 的深度与代价

TS 是直播与 HLS 分片的主格式，也是这一层里唯一"参数一改结论就变"的解析器。
`TsStructureOptions` 暴露三个旋钮：

| 选项 | 默认 | 为什么要有 |
|------|------|-----------|
| `max_scan_packets` | 500000（≈94 MB） | 旧实现写死 5000 包：10 Mbps 的 2 小时节目只看到前 0.8 秒，结论却标成功 |
| `max_resync_bytes` | 1 MiB | 旧实现遇到第一个非 `0x47` 字节就 `break`，一次传输错误丢掉后面整个文件 |
| `max_pes_per_pid` | 8 | 树里每条流展示的 PES 采样条数 |

结论里必须能看出来的东西：`metadata["已扫描包数"]`、`metadata["估算总包数"]`、
`metadata["扫描是否完整"]`、`metadata["失步重同步次数"]`、`metadata["连续计数错误"]`。
**不要把"只扫了前 N 包"当成"整个文件没问题"** —— 这是这一层最容易骗人的地方。

解析内容：

- **PAT / PMT 跨包组装**：`SectionAssembler` 按 `section_length` 拼接，一个包里有多段也能全拆出来。
  以前只解析"落在单个包里的那一段"，流一多（数据量大过 184 字节）后面的 ES 直接消失。
- **PSI 描述符**：`registration`(0x05，AC-3/E-AC-3/HEVC/AV1/Opus…)、`ISO_639_language`(0x0A)、
  `CA`(0x09)、AC-3(0x6A)/E-AC-3(0x7A)、teletext(0x56)、subtitling(0x59)、
  `stream_identifier`(0x52)。`stream_type == 0x06` 的流只能靠 registration 定性。
- **SDT**（`table_id 0x42`）：`service_descriptor` 给出节目名与提供商，挂到 `Program` 节点上。
- **PCR**：首末 PCR 与最大间隔（27 MHz → ms），用来判断"这个流的时钟稳不稳"。
- **连续计数**：按 PID 记 CC 错误；`discontinuity_indicator` 置位时不记错（那是拼接点）。
- **加扰**：`transport_scrambling_control != 0` 的包按 PID 计数。

包长与"包本体长度"是两件事，别混：`192` 是步长（前面 4 字节 TP_extra_header），
`204` 是步长（后面 16 字节 RS 校验），而 **TS 包本体恒为 188 字节**。
读取用步长、字段边界用 188，混用会把 TP 头/校验尾当载荷。

## 4. 结论怎么进 QC：`ts_transport`

解析出结构不等于能评分。TS 的传输层数字（失步 / 连续计数 / PCR / 加扰）以前只出现在
容器结构页的 metadata 与树里，批量 QC 拿不到 —— 规则只读 `AnalysisResult`。

现在 `TsStructureAnalyzer` 在解析完顺手产出 `model::TransportStreamInfo`
（扁平值对象，见 `core/domain/model/TransportStreamInfo.h`），
`AnalysisEngine` 在扫描前对 TS 输入按字节再扫一遍（预算独立：
`AnalysisOptions::ts_transport_max_packets`，默认 20 万包），填进
`AnalysisResult::ts_transport`，由 `container.ts.*` 七条规则消费。

四条必须守住的口径：

- **只对本地 TS 生效**：网络源不重扫（URL 读两遍会拿到不同的直播窗口）；
- **`analyzed=false` 时整族规则静默**：没跑过既不是"没问题"也不是"有问题"；
- **窗口结论要说清范围**：故障类规则的 detail 带出"扫了多少 / 共多少"，
  另有一条 `container.ts.scan_incomplete` 专门提示；
- **`analyzed` 之外还要看 `applicable`**：`analyzed=false` 分不出"这是 MP4，压根不该扫"
  和"这是 TS，但扫描失败了"。后者是最需要说清楚的一种 —— 上面整族规则的"未命中"
  都退化成"没有证据"。所以 `TransportStreamInfo` 另设 `applicable`（本地文件 +
  iformat 是 mpegts + 选项没关），报告层据此在 `applicable && !analyzed` 时显式告警，
  而不是静默略过这一节。

报告里也落了原始数字（JSON 的 `metrics.ts_transport`、TXT/HTML 的"MPEG-TS 传输层"
一节）。理由：规则只在**命中**时才有 detail，未命中什么都不留，于是"连续计数错误 = 0"
和"压根没扫"在报告里长得一模一样。落了扫描范围与各计数，读者自己就能判断
`container.ts.*` 的未命中是"没问题"还是"没证据"。

## 5. 加一个容器格式要动哪些文件

1. `core/domain/model/ContainerStructureInfo.h`：`ContainerFormat` 加枚举值
2. `core/analysis/orchestration/FormatDetector.*`：魔数 + 扩展名 + `FormatName` / `FormatTitle`
3. `core/analysis/container/XxxStructureAnalyzer.{h,cpp}`：解析器本体（接口与其它解析器一致：
   `Analyze(path, result, cancel, options)`，失败要写 `result.error_message`，取消不算失败）
4. `core/analysis/container/ContainerStructureAnalyzer.cpp`：`switch (fmt)` 加一条分发
5. `tests/CMakeLists.txt`：把新 .cpp 加进 `test_container_structure_analyzers`（该测试用
   `TEST_P` 把"文件不存在 / 魔数不符 / 最小合法文件"压成矩阵，新格式只加一行 case）
6. 本文件第 1、2 节的表

解析器必须遵守三条：

- **失败可解释**：`error_message` 非空且能让人看懂；文件打不开统一给"无法打开文件"。
- **取消可冒泡**：`infrastructure::Checkpoint(cancel)` 命中时要一路返回到返回值，
  不能"这一层 return 了、外层照样 valid=true"。
- **有上限**：包数/条目数/深度都要有上界，并让上界出现在结果里（metadata 或 summary）。

## 6. 测试

- `tests/unit/test_container_structure_analyzers.cpp`：五种格式 × 三类输入的参数化矩阵，
  外加 TS 深度用例（192 包长 / 失步重同步 / 描述符+SDT / CC 统计 / 扫描截断）。
- `tests/unit/test_isobmff_parser.cpp`：box 树与字段级解析（含 `stsd` 入口子树）。
- `tests/unit/test_container_ts_qc_rules.cpp`：`container.ts.*` 规则族
  （级别、比例 vs 绝对条数、无 PCR 时不造结论、`analyzed=false` 整族静默、阈值可调）。
- `tests/unit/test_corpus_robustness.cpp`：`tests/corpus/container/` 下的畸形样本
  （largesize 溢出、截断 moov、坏 FLV 头…）必须**有界终止**，不能死循环也不能崩。

### 6.1 真实封装样本

畸形样本只能证明"解析器不会崩"，证明不了"真实文件上算出来的数是对的"。
`tests/corpus/container/real_*` 补的是这一半，由 `generate_real_corpus.py`
用 ffmpeg 生成并入库：

| 样本 | 用途 |
|------|------|
| `real_h264_aac.ts` | 真实 PAT/PMT/PCR/连续计数（188 步长） |
| `real_h264_aac.m2ts` | 192 步长（由上面那份 .ts 插 TP_extra 派生，内容同源） |
| `real_mpeg4_mp3.avi` | 真实 hdrl/strl/movi/idx1 |
| `real_vorbis.ogg` | 真实 OggS 页链 |

`--check` 做的是**语义校验**（同步字节步长、容器魔数、长度整除性），不比字节 ——
ffmpeg 的输出不是字节可复现的，但"换版本产出变成别的容器"和"样本被截断"
这两类真会咬人的漂移照样抓得住。ctest 用例：`RealCorpusCheck`。

这条语料已经抓到过一个真 bug：M2TS 的**最后一个包被丢弃**。扫描循环以前一律要求
读满一个步长，而最后一包后面没有下一包的 TP_extra 头、只剩 188 字节，短读即失败 ——
181 个包只数出 180 个，`scan_complete` 却照报"已扫全文件"。手工样本撞不到这条，
因为它们都能被步长整除。
- `tests/fuzz/fuzz_container_structure.cpp`：容器结构解析的 libFuzzer 入口（Linux + clang）。
