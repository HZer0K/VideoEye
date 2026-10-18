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
| HLS / DASH | **只能靠扩展名**（`.m3u8` / `.mpd` 是纯文本） | `HlsManifestAnalyzer` / `DashManifestAnalyzer` |

2026-10 补的两条检测细节：

- TS 的 192 字节包（M2TS/AVCHD，同步字节在偏移 4）与 204 字节包（RS 纠错）
  以前认不出来，`.m2ts` 只能靠扩展名兜底，然后被按 188 步长解析、报"未找到同步字节"。
- 扩展名映射里 `f4v` 曾同时出现在 MP4 与 FLV 两个分支，后者是死代码。

## 2. 每种格式的深度

"深度"指**解析到什么级别**，不是"有没有解析器"：

| 格式 | 深度 | 主要限制 |
|------|------|---------|
| MP4 / MOV / fMP4 | 深：box 树到字段级 + 样本表交叉校验（stbl / elst / moof-traf-trun / faststart） | 递归深度上限 8；单表条目上限 `max_entries_per_table` |
| MKV / WebM | 深：SeekHead / Info / Tracks(Video/Audio/Colour/Mastering) / Cluster / Cues / Chapters / Tags / Attachments | `BlockAdditions` 只列节点不展开 |
| MPEG-TS | 中深：包长自适应、失步重同步、PAT/PMT 跨包组装、PSI 描述符、SDT、PCR、CC 统计 | 默认只扫前 50 万包（可配）；不做 PES 载荷解析 |
| FLV | 中深：AMF0 `onMetaData`、Enhanced-FLV fourcc（HEVC/AV1/VP9） | Script Tag 只取标量键值 |
| AVI | 中：RIFF chunk 树 + `strh`/`strf` | 无 `idx1`/`indx` 索引校验，OpenDML 大文件不受控 |
| ASF / WMV | 中：7 类对象 GUID 有名字与字段 | Header Extension / Metadata / DRM 归 `Unknown Object` |
| OGG | 中：Vorbis / Opus / FLAC / Theora / Speex 标识头 + 注释块 | 只读每页前 4 KiB 数据 |

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

## 4. 加一个容器格式要动哪些文件

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

## 5. 测试

- `tests/unit/test_container_structure_analyzers.cpp`：五种格式 × 三类输入的参数化矩阵，
  外加 TS 深度用例（192 包长 / 失步重同步 / 描述符+SDT / CC 统计 / 扫描截断）。
- `tests/unit/test_isobmff_parser.cpp`：box 树与字段级解析（含 `stsd` 入口子树）。
- `tests/unit/test_corpus_robustness.cpp`：`tests/corpus/container/` 下的畸形样本
  （largesize 溢出、截断 moov、坏 FLV 头…）必须**有界终止**，不能死循环也不能崩。
- `tests/fuzz/fuzz_container_structure.cpp`：容器结构解析的 libFuzzer 入口（Linux + clang）。
