# 诊断与 QC 报告（QtAnalysisController + QcRuleEngine）

> 目标：把"播放时的实时统计"与"离线全文件体检"彻底分离，并用一套统一的数据契约
> （`DiagnosticIssue` / `QcReport`）串起「扫描 → 规则判定 → UI 呈现 → 报告导出」。

## 1. 分层与数据流

```mermaid
flowchart LR
    UI[AnalysisPanel 质量诊断页] -->|StartAnalysis(path)| CO[QtAnalysisController]
    CO -->|后台 std::thread| SCAN[avformat 全文件 demux 扫描]
    SCAN -->|ProgressReported| UI
    SCAN -->|AnalysisFinished| UI
    UI -->|AnalysisResult| ENG[QcRuleEngine]
    RULES[(QcRule 规则集<br/>阈值可在 UI 编辑)] --> ENG
    ENG -->|QcReport| UI
    UI -->|导出 html/json/csv/txt| EXP[ReportExporter::ExportQcReport]
```

设计要点：

- **与播放解耦**：`StreamAnalyzer` 只负责播放过程中的实时统计；`QtAnalysisController`
  独立打开同一个文件做一遍 demux，互不干扰（代价是一次额外 I/O，故默认手动触发）。
- **generation 防串扰**：`StartAnalysis()` 每次递增 `generation`，UI 侧
  `OnDiagnosticsProgress/Finished/Failed` 首行即校验 generation，快速切换文件时
  旧任务的回调会被直接丢弃（与 `MediaPlayer` 的 generation 思路一致）。
- **可取消**：`Cancel()` 置原子标志，扫描循环每包检查一次，退出后仍以
  `completed=false` 回传部分结果。

## 2. 核心模型（core/model/）

| 文件 | 作用 |
|------|------|
| `TimeRange.h` | 时间区间；`IsGlobal()` 表示文件级问题，`At()` 表示瞬时点 |
| `MetricSeries.h` | 时间序列指标（码率/帧率/响度），统一提供 Min/Max/Mean/StdDev/Percentile |
| `DiagnosticIssue.h` | 单条问题：严重度 + 类别 + 位置 + 实测值/阈值 + 建议 |
| `QcRule.h` | 规则定义：`MaxExceeded` / `MinBelow` / `NonZero` 三种判定 + 阈值/单位 |
| `QcReport.h` | 报告：文件概要 + 问题列表 + 规则快照 + 评分 + 结论 |

严重度 → 扣分：`Critical -30 / Error -12 / Warning -5 / Info -1`，评分 ≥90 通过、≥70 警告、否则不通过。

## 3. 扫描产出（core/domain/model/AnalysisResult.h）

`AnalysisResult` 是规则引擎的唯一输入，字段分四组：

- 文件级：容器格式、时长、大小、整体码率、是否可 seek、moov/mdat 顺序（仅 MP4 家族）
- 流级：`StreamDigest`（codec/profile/分辨率/fps/采样率/声道/码率/包数/关键帧数）
- 序列：`total_bitrate_kbps` / `video_bitrate_kbps` / `video_fps`（默认 1 s 粒度）
- 健康度：PTS 单调性、DTS 缺失率、最大时间戳跳变、GOP 间隔序列、包大小极值

## 4. 规则集

> **权威来源是 `core/qc/QcRuleDefaults.cpp` 的 `DefaultQcRules()`**，不是这张表。
> 下面只列早期手写的核心规则；后来按分析器拆出的规则族（`container.mp4.*`、
> `container.hls.*` / `container.dash.*`、`container.ts.*`、`subtitle.*`、
> `timecode.*`、`video.color.*`、`scte35.*` …）数量已经远超这张表，
> 完整清单在「规则与阈值」页与导出的 JSON 规则快照里。

| 规则 id | 默认阈值 | 严重度 | 判定 |
|---------|----------|--------|------|
| `container.duration_invalid` | < 0.01 s | Critical | 时长缺失 |
| `container.unseekable` | — | Error | IO 不支持 seek |
| `container.moov_after_mdat` | — | Info | 未 faststart |
| `container.missing_video` / `missing_audio` | — | Warning | 缺流 |
| `video.bitrate.peak_ratio` | > 3.0 倍 | Warning | 峰值/均值 |
| `video.bitrate.low_bpp` | < 0.05 bpp | Info | 编码码率偏低 |
| `video.gop.max_seconds` | > 10 s | Warning | 关键帧间隔 |
| `video.gop.irregular` | σ/μ > 0.6 | Info | 关键帧不均匀 |
| `video.fps.unstable` | σ > 2.5 fps | Warning | 帧率抖动 |
| `video.resolution.odd` | — | Warning | 分辨率奇数边 |
| `timing.pts_non_monotonic` | — | Error | PTS 回退 |
| `timing.dts_missing` | > 5 % | Warning | DTS 缺失占比 |
| `timing.gap` | > 2.0 s | Warning | 时间戳跳变 |
| `audio.sample_rate_low` | < 32000 Hz | Info | 采样率偏低 |
| `audio.channel_missing` | — | Info | 未标注声道数 |

### 4.1 MPEG-TS 传输层与载荷级（`container.ts.*`）

输入不是逐包扫描产物，而是 `AnalysisResult::ts_transport`（`TransportStreamInfo`）：
失步 / 连续计数 / PCR / 加扰都是 **TS 包头里的事实**，avformat 的 demux 看不到包头，
所以 `AnalysisEngine` 会在扫描前按字节再扫一遍文件头（`AnalysisOptions::
analyze_ts_transport` / `ts_transport_max_packets`，默认 20 万包）。
解析器与容器结构页共用 `TsStructureAnalyzer`，是同一组数字。

同一个结构体里还有两组**载荷级**数字：`max_no_idr_interval_ms` / `idr_count` /
`video_pids_without_idr` 与 `max_pid_gap_ms` / `max_pid_gap_pid` / `stalled_pids`。
它们由 `scan_pes_payload`（默认开）驱动，拆完 PES 头继续看 ES：H.264 认 NAL type 5、
HEVC 认 19/20/21 当 IDR；其余编码不认（认错 NAL 头会把随机字节读成 type 5，
报出来的间隔纯属虚构）。

| 规则 id | 默认阈值 | 严重度 | 判定 |
|---------|----------|--------|------|
| `container.ts.missing_pat` | — | Critical | 没有 PAT，节目表无从得知 |
| `container.ts.missing_pmt` | — | Error | 有 PAT 但没 PMT（PAT 缺失时不重复报） |
| `container.ts.sync_loss` | > 0 次 | Error | 同步字节丢失后重新对齐 |
| `container.ts.continuity_error` | > 0.01 % | Warning | 连续计数错误**比例**（不是绝对条数） |
| `container.ts.pcr_gap` | > 100 ms | Warning | 最大 PCR 间隔（没采到 PCR 时不报） |
| `container.ts.scrambled` | — | Info | 加扰流：能解复用，但载荷不可读 |
| `container.ts.scan_incomplete` | — | Info | 只扫了窗口，提醒结论范围 |
| `container.ts.no_idr` | > 10000 ms | Warning | 视频 PID 最长的一段没有随机接入点：IDR 之间、**首帧到第一个 IDR**、**最后一个 IDR 到窗口末尾**三段的较大者 |
| `container.ts.pid_stall` | > 5000 ms | Warning | 某个 PID 的 PTS 断档（含"尾部缺口"：最后一条离节目最新 PTS 的距离） |

三条口径约束：

- `ts_transport.analyzed=false` 时整族规则静默 —— "没跑过"不是"没问题"；
- 载荷级另有 `pes_payload_scanned`：传输层扫了但载荷没扫（非 H.264/HEVC、
  或 `scan_pes_payload=false`）时 `no_idr` / `pid_stall` 也静默 ——
  此时 `max_no_idr_interval_ms == 0` 的含义是"没测"，不是"很健康"；
- 故障类规则的 detail 必须带出「扫了多少 / 共多少」，否则窗口结论会被读成全文件结论。

### 4.2 AVI / FLV / ASF / OGG / MKV（`container.avi.*` / `flv.*` / `asf.*` / `ogg.*` / `mkv.*`）

输入是 `AnalysisResult::container_diagnostics`（`ContainerDiagnosticsInfo`），由
`AnalysisInputSession::FillLegacyContainerFacts()` 按容器格式调用对应的
`*StructureAnalyzer` 采集，与「容器结构」页共用同一套解析代码。口径：

- 结构页需要建树，QC 扫描不需要（`build_tree=false`）；AVI 跳过 `movi`、
  OGG/FLV 只扫页头/tag 头，避免大文件把分析时间拖垮；
- 各分析器的 `analyzed` 为假时整族静默 —— 没扫过的容器不下结论；
- 这些数字是**容器自己的结构事实**，不是 FFmpeg demux 的结论。

| 规则 id | 默认阈值 | 严重度 | 判定 |
|---------|----------|--------|------|
| `container.avi.riff_size_exceeds_file` | — | Error | 顶级 RIFF 长度超过文件大小 |
| `container.avi.missing_index` | — | Warning | 有音视频流却既无 `idx1` 也无 `indx` |
| `container.avi.index_bad_offsets` | > 0 项 | Error | `idx1` 条目偏移 + 长度越过文件末尾 |
| `container.avi.indx_truncated` | — | Error | `indx` 声明条目数超过块实际长度 |
| `container.avi.no_opendml` | — | Warning | 有 `AVIX` 却没有 `dmlh` |
| `container.avi.truncated_chunks` | > 0 块 | Error | RIFF 块声明长度越过文件末尾 |
| `container.avi.zero_length_streams` | > 0 条 | Warning | `strh.dwLength = 0`，时长/帧数不可信 |
| `container.flv.missing_metadata` | — | Info | 没有 `onMetaData` |
| `container.flv.missing_avc_sequence_header` | — | Error | 声明 AVC/HEVC/AV1 却没见到序列头 |
| `container.flv.timestamp_backwards` | > 0 次 | Warning | tag 时间戳相对上一条回退 |
| `container.flv.truncated_tags` | > 0 个 | Error | tag 数据越过文件末尾 |
| `container.flv.tag_size_mismatch` | > 0 处 | Warning | `PreviousTagSize` 与实际 tag 长度不符 |
| `container.asf.missing_data_object` | — | Error | 没有 Data Object，文件里没有媒体负载 |
| `container.asf.header_object_size_invalid` | > 0 个 | Error | 子对象 size 非法（< 24 / 越界） |
| `container.asf.header_size_exceeds_file` | — | Critical | Header Object 长度超过文件大小 |
| `container.asf.duration_invalid` | — | Warning | 头里的播放时长缺失或为 0 |
| `container.ogg.page_sequence_gap` | > 0 次 | Warning | 同一逻辑流的页序号不连续 |
| `container.ogg.missing_bos` | > 0 条 | Error | 逻辑流没有 BOS 页 |
| `container.ogg.missing_eos` | > 0 条 | Info | 逻辑流没有 EOS 页（可能仍在写入） |
| `container.ogg.truncated_pages` | > 0 页 | Error | 页声明长度越过文件末尾 |
| `container.ogg.resyncs` | > 0 次 | Warning | 扫描中需要重新找 `OggS` 同步 |

#### MKV / WebM 为什么有两套解析

`container.mkv.*` 的事实来自 `MkvContainerScanner`，**不是**结构页那棵元素树（`EbmlAnalyzer`）：

| | `EbmlAnalyzer`（结构页） | `MkvContainerScanner`（QC） |
|---|---|---|
| 产物 | 元素树 + Track/Cue/Block 表 | 十来个计数（Cues、Cluster 时间轴、CodecPrivate、unknown-size） |
| 读取方式 | 只读文件开头 32 MiB（`kMaxScanBytes`）建树 | 只读元素头，载荷一律 `Seek` 跳过，内存 O(1) |
| 上限 | 20 万节点 + 树内块节点 2 万（`kMaxTreeBlocks`）；撞节点上限即判解析失败 | `MkvScanOptions::max_elements`（20 万元素头），撞上限置 `scan_complete=false` |
| 长片 | 窗口外的尾部不建树，`scan_truncated=true`，摘要里写明"仅扫描前 N MiB" | 一级扫描只碰 Cluster/Cues/Tracks，正常文件几百次元素头，能覆盖到片尾 |

也就是说：**结构页看树，QC 看事实**。凡是"必须扫完整个文件才敢下结论"的规则
（缺 Cues、时间戳回退、截断）都只能建立在扫描器上；反过来，扫描器**不建树**，
不承担结构展示。

⚠️ 两件事的可失败性也是独立的：结构树建不出来（畸形文件 / 超长片）时
`ContainerStructureAnalyzer` 仍会先跑一遍扫描器，所以 `container.mkv.*` 不会因为
"树建失败"而集体静默。反过来说，**别把扫描器塞回树成功之后的分支**。

`unknown_size_elements` 的处理是这一层最容易写错的地方：Matroska 允许元素
`size` 全 1 表示"长度未知"（流式写出的 Segment/Cluster 很常见），此时边界只能靠
**下一个 level-1 元素**（Cluster / Cues / Tags / Chapters / Attachments / SeekHead /
Info / Tracks）推断。推断错了轻则后面的 Cues 被吞掉，重则原地打转 ——
`fuzz_container_structure` 每轮都会跑这个入口。

| 规则 id | 默认阈值 | 严重度 | 判定 |
|---------|----------|--------|------|
| `container.mkv.missing_cues` | — | Warning | 扫完全文件仍没有 Cues 索引（点播不可 seek） |
| `container.mkv.cluster_timestamp_backwards` | > 0 次 | Error | 相邻 Cluster 时间戳回退 |
| `container.mkv.missing_codec_private` | > 0 条 | Error | AVC/HEVC/AAC/Vorbis 轨道缺 CodecPrivate |
| `container.mkv.unknown_size_element` | > 0 个 | Info | 元素声明"长度未知"（流式写出的典型特征） |
| `container.mkv.truncated` | — | Warning | 元素头/载荷越过 Segment 或文件末尾 |
| `container.mkv.scan_incomplete` | — | Info | 元素预算用尽，结论只覆盖已扫描部分 |

`missing_cues` 有三道门控，缺一条就会变成刷屏的假警报：`scan_complete=false`（没扫完）、
`truncated=true`（截断文件天然没索引，那是 truncated 规则的账）、
`duration_seconds<=0`（直播录制边写边发，本来就不写 Cues）。

### 4.3 MP4 多 sample entry（`container.mp4.*`）

`Mp4SampleTableAnalyzer` 发现 `stsd` 声明了多于一条 sample entry 时产出
`container.mp4.multiple_sample_entries`（Info）。它不表示文件损坏，而是提醒：
`track.codec` 只代表第一条描述，其余描述对应的样本（可能通过 `stsc` 的
`sample_description_index` 选择）编码可能不同；Dolby Vision 双层属于刻意用法。

### 新增一条规则

1. 在 `core/qc/QcRuleDefaults.cpp` 的 `DefaultQcRules()` 里追加一条（id / 名称 / 类别 / 严重度 / 判定 / 阈值 / 建议）；
2. 在 `QcRuleEngine::CheckRule()` 中按 id 增加分支，从 `AnalysisResult` 取数并调用 `Triggered(rule, value)`；
3. UI 的「规则与阈值」表与 JSON/HTML 导出都会自动带上新规则，无需改 UI 代码。

## 5. UI（AnalysisPanel「诊断与报告」页）

- 顶部：开始分析 / 取消 / 导出报告 + 进度条
- 概览：评分与结论、各严重度计数、容器/时长/码率/流数/包数/关键帧数
- 子页「问题清单」：逐秒码率 + 帧率双轴曲线，问题表格（严重度按颜色区分）
- 子页「规则与阈值」：启用勾选 + 阈值双击编辑，修改后立即用当前扫描结果重算报告；「恢复默认规则」

> ℹ️ 侧边栏条目由 `content_stack_` 的 `pageTitle` 自动生成，页面项带 `UserRole` stack 下标，
> 行号与页面索引已解耦（`MainWindow::PopulateSidebarItems/OnSidebarChanged`）。新增页面时只需
> 关注分组范围：`MainWindow.cpp` 里的分组表按当前 16 页定义，页面数量不符会自动退化为平铺列表。

## 6. 报告导出

`utils::ReportExporter::ExportQcReport(path, report)` 按扩展名自动分派：
`.html`（总览 + 问题清单 + 规则快照）/ `.json`（含规则快照，便于 CI 回归对比）/ `.csv`（Excel）/ `.txt`。
