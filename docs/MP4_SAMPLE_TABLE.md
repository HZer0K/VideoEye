# MP4/fMP4 容器一致性校验（Sample Table）

## 1. 解决什么问题

MP4 的「索引」和「数据」是分开写的：mdat 里是一坨连续的码流，moov/stbl 里才是
每个样本的位置、大小、时间。播放器完全靠 stbl 这几张表去 mdat 里寻址，因此：

- 表之间不自洽 → 尾部样本读不到、花屏、爆音；
- chunk offset 写错/文件被截断 → seek 直接失败；
- ctts 负偏移 / elst 偏移 → 起播黑帧、首帧被藏、开头音画不同步；
- moov 在 mdat 之后 → 点播要下完整个文件才能起播；
- fMP4 分片序号或 tfdt 不连续 → 分段处停顿、时长统计错、seek 落点偏。

这些问题在 ffprobe 层面往往看不出来（它只按流读，不校验表），必须逐样本算一遍。

## 2. 模块落点

| 文件 | 职责 |
|---|---|
| `core/model/Mp4SampleInfo.h` | 样本 / 轨道表 / 分片 / 结果的数据契约（纯 C++，不依赖 Qt 与 Bento4） |
| `core/model/Mp4ConsistencyIssue.h` | 一致性问题（含问题码常量 + 转 `DiagnosticIssue`） |
| `core/analyzer/Mp4SampleTableAnalyzer.h/.cpp` | Bento4 解析 stbl / moof + 纯逻辑校验 `Validate()` |
| `core/model/QcModels.cpp` | `container.mp4.*` 规则定义（阈值与级别） |
| `core/analyzer/QcRuleEngine.cpp` | 把 finding 转成 QC 报告条目（按规则级别/阈值再过滤） |
| `core/analyzer/AnalysisTask.h` | `AnalysisOptions::analyze_mp4_sample_table` / `AnalysisResult::mp4_samples` |
| `core/analyzer/AnalysisCoordinator.cpp` | 全文件扫描时对 MP4 家族顺带跑一遍 |
| `core/analyzer/ContainerStructureAnalyzer.cpp` | 打开文件做结构分析时顺带跑一遍（供容器页展示） |
| `ui/analysis_panel/AnalysisPanel.cpp` | 容器页「Sample Table」子页 + 结构树联动 |

## 3. 数据流

```
文件
 ├─(Bento4) Mp4SampleTableAnalyzer::AnalyzeFile()   ← 解析
 │     ├─ stbl: stts/ctts/stss/stsz/stz2/stsc/stco/co64/elst
 │     ├─ moof: mfhd/traf/tfhd/tfdt/trun
 │     └─ 顶层 box 顺序（ftyp/moov/mdat/moof/sidx/styp）
 │
 ├─ Mp4SampleTableAnalyzer::Validate()              ← 纯函数，可单测
 │     └─ 产出 Mp4ConsistencyIssue 列表 + 逐样本 flags
 │
 ├─ 容器页 Sample Table 子页（全量展示，含被规则关掉的 Info）
 └─ QcRuleEngine（按 container.mp4.* 规则决定上报与级别）→ 诊断与报告 / 导出
```

## 4. 解析实现要点（Bento4 老版本的坑）

本仓库集成的 Bento4 版本**没有**暴露 stts/ctts/stsc 的 entries，只能逐个查：

- `AP4_SttsAtom::GetDts(sample, dts, &duration)` — 拿 DTS 与时长；
- `AP4_CttsAtom::GetCtsOffset(sample, offset)` — 拿 ctts 偏移；
- `AP4_StszAtom::GetSampleCount()/GetSampleSize(i)` — 权威样本数与大小；
- `AP4_StssAtom::GetEntries()`、`AP4_StcoAtom::GetChunkOffsets()`、`AP4_Co64Atom::GetChunkOffsets()` — 直接可遍历；
- `AP4_AtomSampleTable::GetSampleChunkPosition()` — 样本 → chunk / chunk 内序号。

**两个必须记住的陷阱**：

1. `GetDts` / `GetCtsOffset` 的 sample 参数是 **1-based**（源码注释 "sample indexes
   start at 1"，传 0 直接 `AP4_ERROR_OUT_OF_RANGE`）。探测"这张表覆盖多少样本"时必须
   从 1 开始，否则会得到「stts 覆盖 0 个样本」从而误报 `sample_count_mismatch`。
2. 各表样本数靠"探测"而非读表：先指数扩张找上界、再二分，内部有顺序访问缓存，开销可忽略。

## 5. 校验项与问题码

| 问题码 | 级别 | 触发条件 |
|---|---|---|
| `container.mp4.not_faststart` | Warning | moov 在 mdat 之后且非分片文件 |
| `container.mp4.sample_count_mismatch` | Error | stsz / stts / ctts 覆盖的样本数不一致 |
| `container.mp4.missing_chunk_offsets` | Error | 有样本但无 stco/co64 |
| `container.mp4.stco_co64_conflict` | Error | stco 与 co64 同时存在 |
| `container.mp4.stco_overflow` | Error | 最大 chunk 偏移 > 4 GB 却还在用 stco |
| `container.mp4.stss_count_mismatch` | Error/Warning | stss 关键帧号越界 / 实际关键帧数与 stss 不符 |
| `container.mp4.chunk_offset_out_of_range` | Error | offset+size 越过文件末尾 |
| `container.mp4.dts_not_monotonic` | Error | DTS 回退 |
| `container.mp4.negative_cts` | Warning | PTS < 0（ctts v1 负偏移） |
| `container.mp4.sample_offset_gap` | Warning | 同 chunk 内偏移不连续（stsz 与 stco/stsc 不符） |
| `container.mp4.zero_size_sample` | Warning | 样本大小为 0 |
| `container.mp4.zero_duration_sample` | Info | 样本时长为 0 |
| `container.mp4.first_sample_not_sync` | Warning | 首个样本不是关键帧 |
| `container.mp4.elst_first_frame_shift` | Warning | elst 把首帧推后超过阈值（默认 33 ms） |
| `container.mp4.elst_empty_edit` | Info | 存在 empty edit |
| `container.mp4.av_start_mismatch` | Warning | 音视频首个样本呈现时间差超阈值（默认 40 ms） |
| `container.mp4.fragment_sequence_gap` | Error/Warning | moof 序号重复/回退(Error) 或跳号(Warning) |
| `container.mp4.fragment_time_gap` | Error/Warning | tfdt 回退(Error) 或与上一分片不连续(Warning) |
| `container.mp4.fragment_time_origin` | Info | 首个分片 tfdt 非 0（直播切片/截取） |
| `container.mp4.fragment_data_offset` | Error | base_data_offset / default-base-is-moof / trun.data_offset 全无 |

## 6. UI

容器页（文件结构）在 MP4/MOV 时右侧详情区多一个 **Sample Table** 子页：

- 顶部：轨道下拉（Track id / 类型 / 编码 / 样本数）+ 导出样本 CSV；
- 中部：样本表 `# / 偏移 / DTS(s) / PTS(s) / ΔCTS(ms) / 时长(ms) / 大小 / Chunk / 关键帧`，
  异常行按严重程度着色（红=读不到数据或解码错乱，黄=影响兼容/体验），鼠标悬停显示原因；
- 底部：一致性问题表（级别/位置/问题/说明/建议）+ 分片（moof）表。

**结构树联动**：点击左侧 box 树里的 `stts / ctts / stss / stsz / stsc / stco / co64 / elst /
moof / traf / tfhd / tfdt / trun / mfhd` 会自动切到 Sample Table 子页，并：

- 按所属 trak 选中对应轨道；
- 高亮关联列（stts→DTS/时长，ctts→PTS/ΔCTS，stsz→大小，stco/co64→偏移/Chunk，stss→关键帧）；
- 点 `stss` 时只列关键帧；
- 点 moof 系列时底部切到「分片 (moof)」表。

## 7. 测试

- 单测：`tests/unit/test_mp4_sample_table.cpp`（21 例，只测 `Validate()`，喂合成表，
  不需要 Bento4 / Qt / 真实文件）。覆盖 faststart 判定、各表样本数不一致、offset 越界、
  负 CTS、DTS 回退、chunk 内空洞、elst 阈值、音视频起点、分片序号跳号/回退、
  tfdt 空洞、幂等性。
- 实机验收（见下）用 ffmpeg 生成样本 + Bento4 真机解析。

## 8. 实机验收记录（ffmpeg 生成样本）

| 样本 | 预期 | 实测 |
|---|---|---|
| `ffmpeg … -movflags +faststart` | faststart，无布局告警 | `faststart=1`，issues=0 |
| 同上但 `-movflags -faststart` | 给出首屏加载提示 | `[WARN] not_faststart`（moov 0x15789 在 mdat 0x28 之后）|
| 截断到 45 KB | offset 越界 → error | `[ERROR] chunk_offset_out_of_range`（视频 43 个 / 音频 81 个样本越界）|
| `-movflags +frag_keyframe+empty_moov+default_base_moof` | 分片连续，无误报 | 4 个 moof / 7 个 traf，seq 1..4 连续、tfdt 连续，issues=0 |
| 挖掉中间一个 moof+mdat | 序号不连续 → warning | `[WARN] fragment_sequence_gap`（1→3）+ `[WARN] fragment_time_gap`（2 处时间空洞）|
