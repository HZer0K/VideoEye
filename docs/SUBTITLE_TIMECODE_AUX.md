# 字幕、时码与辅助数据轨（功能 9）

> 目标：把交付审核里"看得见但要靠工具才查得全"的三类元数据做成可核查的清单 ——
> **字幕 cue**、**SMPTE 时码 / 章节**、**data 流与 SCTE-35**。
> 这三类东西的共同点是：错了不会导致播不出来，但会直接被客户退回
> （空字幕闪一下、时码对不上母版、广告插错位置）。

## 1. 模块划分

```
AnalysisCoordinator::Run()       全文件 demux（与码率/GOP、音频 QC 共用同一次扫描）
        │
        ├─ SubtitleAnalyzer      字幕流登记 + 包载荷解析 + cue 校验
        ├─ TimecodeAnalyzer      tmcd 时码轨 / metadata timecode tag / 章节
        └─ AuxDataAnalyzer       data 流枚举 + metadata 采集
                 └─ Scte35Analyzer   splice_info_section 位流解析（纯 C++）
        ▼
AnalysisResult::subtitle / timecode / aux_data
        ▼
QcRuleEngine                     subtitle.* / timecode.* / chapter.* / scte35.* 规则
        ▼
AnalysisPanel「字幕 / 辅助数据」页 + PlayerPanel 时间轴旁的 SMPTE 时码
```

| 文件 | 职责 |
| --- | --- |
| `core/model/SubtitleCueInfo.h/.cpp` | 字幕流 / cue / 问题的数据模型 |
| `core/model/TimecodeInfo.h/.cpp` | SMPTE 时码换算（含 drop-frame）、时码轨、章节 |
| `core/model/AuxiliaryDataInfo.h/.cpp` | data 流、SCTE-35 cue、metadata tag |
| `core/analyzer/SubtitleAnalyzer.h/.cpp` | SRT/ASS/WebVTT/tx3g/CEA-608 解析与校验 |
| `core/analyzer/TimecodeAnalyzer.h/.cpp` | tmcd 样本解码、metadata 时码、章节检查 |
| `core/analyzer/Scte35Analyzer.h/.cpp` | SCTE-35 二进制解析（无 FFmpeg 依赖） |
| `core/analyzer/AuxDataAnalyzer.h/.cpp` | FFmpeg 侧：data 流枚举、喂包、metadata 采集 |

依赖边界与 `VisualDefectAnalyzer` 一致：**解析算法不依赖 FFmpeg**，
只有"从 AVFormatContext 里把流找出来"那一层碰 avformat，
所以单测全部用合成输入（见 `tests/unit/test_subtitle_timecode_aux.cpp`）。

## 2. 字幕

### 2.1 支持矩阵

| 格式 | 第一阶段 | 说明 |
| --- | --- | --- |
| SRT / SubRip | ✅ 解析 cue | 序号 + `-->` 时间行，支持 `,` 毫秒写法 |
| ASS / SSA | ✅ 解析 cue | `Dialogue:` 行，剥离 `{覆盖指令}` 与 `\N` |
| WebVTT | ✅ 解析 cue | 跳过 `WEBVTT` 头与 `NOTE/STYLE/REGION` 块，剥离 `<v>` `<i>` 标签 |
| MOV/MP4 tx3g | ✅ 解析 cue | 样本前 2 字节是文本长度，末尾样式 box 忽略 |
| CEA-608 | ✅ 基础可见文本 | 控制码对跳过，北美基础字符集映射；完整服务层状态机待第二阶段 |
| CEA-708 | ⚠️ metadata + 包时间线 | DTVCC 数据包化需要服务层状态机 |
| DVB / DVD / PGS / XSUB | ⚠️ metadata + 包时间线 | 图形字幕，bitmap preview 待第二阶段 |

### 2.2 检查项与严重度

| 问题 | 规则 id | 级别 | 触发条件 |
| --- | --- | --- | --- |
| 空字幕 | `subtitle.cue_empty` | Warning | 剥离标签后没有可见字符（含全空白） |
| cue 重叠 | `subtitle.cue_overlap` | Warning | 起点早于上一条终点（容差 5 ms） |
| 时间倒序 | `subtitle.cue_order` | Error | 起点早于上一条起点 |
| 时长非法 | `subtitle.cue_invalid_duration` | Error | 结束时间不晚于开始时间 |
| 停留过短 | `subtitle.cue_too_short` | Warning | < 0.5 s（阈值可在规则页改） |
| 停留过长 | `subtitle.cue_too_long` | Warning | > 8 s |
| 阅读速度过快 | `subtitle.cue_too_fast` | Info | > 21 字符/秒 |
| 超出媒体时长 | `subtitle.cue_out_of_range` | Warning | 起点晚于媒体时长 |
| 缺语言 tag | `subtitle.missing_language` | Info | 流级 |
| 缺 handler name | `subtitle.missing_handler` | Info | 流级（MP4/MOV） |

阈值来源：`SubtitleOptions` 是分析侧默认值，**扫描前会从「规则与阈值」页同步一次**
（`AnalysisPanel::SyncSubtitleThresholdsFromRules`），避免两处阈值各说各话。

## 3. 时码与章节

### 3.1 三条取数路径

1. **tmcd 时码轨**：MOV/MP4 的 `tmcd` 轨，样本是 4 字节大端帧序号。
   FFmpeg 里它没有独立 codec id（codec 一直是 `none`），只能靠四字符 tag `tmcd` 认出来。
2. **metadata timecode tag**：FFmpeg 会把 tmcd 的起始时码复制到视频流的 metadata 上
   （key 匹配 `timecode` / `time_code` / `tc` / `smpte_timecode` / `start_timecode`，大小写不敏感）。
3. **两条都没有** → 报 `timecode.missing`（Info）。

主时码优先取 tmcd 轨（逐样本读数最权威），没有再退到 metadata。

### 3.2 drop-frame

29.97 / 59.94 素材用 non-drop 时码时，**时码读数每小时比真实时间慢约 3.6 秒**，
广告插入点会整体漂移 —— 所以 `timecode.drop_frame_mismatch` 单独列一条规则：
帧率是 NTSC（29.97/59.94）但时码标记 non-drop（或反过来）就报 Warning。

换算按 SMPTE 12M：

```
drop:    每分钟（整十分钟除外）跳过前 dropFrames 帧（29.97 -> 2，59.94 -> 4）
帧号 -> 时码: 先按 10 分钟段补齐被跳过的帧数，再按整数帧率取模
时码 -> 帧号: fps*3600*h + fps*60*m + fps*s + f - drop*(totalMinutes - totalMinutes/10)
```

显示遵循 SMPTE 书写习惯：drop frame 用 `;` 分隔（`01:02:03;04`），non-drop 用 `:`。

### 3.3 章节

`AVFormatContext::chapters` 直接读，检查：

- **重叠**（起点早于上一章节终点）→ `chapter.overlap` Warning
- **越界**（终点超出媒体时长 0.5 s 以上）→ `chapter.out_of_range` Warning
- **倒序** → `chapter.non_monotonic` Error
- **零时长** / **无标题** → `chapter.zero_duration` / `chapter.missing_title` Info

## 4. 辅助数据轨与 SCTE-35

### 4.1 data 流枚举

`AVMEDIA_TYPE_DATA` 的流全部登记：类型（SCTE-35 / tmcd / KLV / Teletext / 私有）、
codec tag、handler name、language tag、包数与字节数。
目的是发现"封装环节把辅助轨弄丢了"或"多了一条没人认得的私有 data 流"。

### 4.2 SCTE-35 解析

按 ANSI/SCTE 35 的 `splice_info_section` 逐比特解析：

| 字段 | 说明 |
| --- | --- |
| `splice_command_type` | splice_null / schedule / insert / time_signal / bandwidth_reservation / private |
| `splice_event_id` | 32 bit event id |
| `out_of_network` | OUT=进广告，IN=回节目（UI 显示 OUT/IN） |
| `splice_time` | 33 bit PTS，(pts_adjustment + pts_time) mod 2^33 / 90000 |
| `break_duration` | 33 bit / 90000，配 `auto_return` |
| `unique_program_id` / `avail_num` / `avails_expected` | splice_insert 尾部字段 |
| segmentation_descriptor | event id、type_id（含中文名）、duration、UPID |

- CRC_32 用 MPEG-2 多项式（0x04C11DB7），校验失败记 `scte35.crc_invalid`。
- 载荷不从 `0xFC` 开始时会在前 16 字节内找 table_id（应对 pointer_field / 对齐字节）。
- 加密的 section（`encrypted_packet=1`）不解析，直接记原因。

规则：`scte35.parse_error`（Warning）、`scte35.crc_invalid`（Warning）、
`scte35.duration_missing`（Info，splice_insert 没给 duration 时下游无法判断插多长）。

## 5. UI

「字幕 / 辅助数据」页四个子页：

1. **字幕**：流表（编码/格式/承载/语言/handler/默认/强制/cue 数）+ cue 表
   （开始/结束/时长/字符/语言/问题/文本），支持按流过滤、"只看有问题的"、
   点击行跳转播放器、导出 cue CSV。有问题的行标红。
2. **时码与章节**：时码信息表（首帧时码/帧率/drop-frame/各时码源）+ 章节时间线。
3. **辅助数据与 SCTE-35**：data 流表 + SCTE-35 标记图（OUT 画在上方、IN 画在下方，
   一眼看出插入点是否成对）+ cue 明细表 + 导出 CSV。
4. **metadata**：容器级 / 流级 / 章节级 tag 的 key-value 表，可导出 CSV。

播放器控制条的时间标签旁新增 **SMPTE 时码**：
按当前帧率把播放位置换算成 `HH:MM:SS:FF`；
扫描拿到素材自带起始时码后，以它为基准累加（`AnalysisPanel::StartTimecodeReady` →
`PlayerPanel::SetStartTimecode`）。

## 6. 测试

| 测试 | 覆盖 |
| --- | --- |
| `tests/unit/test_subtitle_timecode_aux.cpp` | SRT/WebVTT/ASS 解析、tx3g 包载荷、CEA-608、重叠/空/倒序/时长窗口/越界校验、时码换算与 tmcd 首帧时码、drop-frame 合法性、SCTE-35 splice_insert + segmentation + CRC |
| `tests/unit/test_aux_qc_rules.cpp` | 规则 id 是否齐全、重叠/空字幕级别是否为 Warning、倒序是否为 Error、未分析时不误报、时码缺失与 drop-frame 不符、章节重叠、SCTE-35 缺 duration |

验收对应关系：

- SRT cue 重叠 → `subtitle.cue_overlap` Warning ✅
- 空字幕 cue → `subtitle.cue_empty` Warning ✅
- timecode track 输出首帧时码 → `TimecodeAnalyzer::DecodeTmcdSample` + `AnalysisResult::timecode.primary` ✅
- SCTE-35 样本列出 cue event → `Scte35Analyzer::ParseSection`（含 event id / splice time / duration / OUT-IN）✅
