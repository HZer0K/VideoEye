# 字幕、时码与辅助数据轨（功能 9）

> 目标：把交付审核里"看得见但要靠工具才查得全"的三类元数据做成可核查的清单 ——
> **字幕 cue**、**SMPTE 时码 / 章节**、**data 流与 SCTE-35**。
> 这三类东西的共同点是：错了不会导致播不出来，但会直接被客户退回
> （空字幕闪一下、时码对不上母版、广告插错位置）。

## 1. 模块划分

```
AnalysisEngine::Run()       全文件 demux（与码率/GOP、音频 QC 共用同一次扫描）
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
| `core/domain/model/SubtitleCueInfo.h/.cpp` | 字幕流 / cue / 问题的数据模型 |
| `core/domain/model/TimecodeInfo.h/.cpp` | SMPTE 时码换算（含 drop-frame）、时码轨、章节 |
| `core/domain/model/AuxiliaryDataInfo.h/.cpp` | data 流、SCTE-35 cue、metadata tag |
| `core/analysis/diagnostics/SubtitleAnalyzer.h/.cpp` | SRT/ASS/WebVTT/tx3g/CEA-608 解析与校验 |
| `core/analysis/diagnostics/TimecodeAnalyzer.h/.cpp` | tmcd 样本解码、metadata 时码、章节检查 |
| `core/analysis/diagnostics/Scte35Analyzer.h/.cpp` | SCTE-35 二进制解析（无 FFmpeg 依赖） |
| `core/analysis/diagnostics/AuxDataAnalyzer.h/.cpp` | FFmpeg 侧：data 流枚举、喂包、metadata 采集 |

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
| CEA-608 | ✅ 服务层状态机 | CC1-CC4 四通道；pop-on（RCL/EOC）、paint-on（RDC）、roll-up（RU2/3/4 + CR）；PAC 行定位、mid-row、特殊/扩展字符、EDM/ENM/DER/BS；XDS 单独计数不进文本 |
| CEA-708 | ✅ DTVCC 解码 | cc_data 三层解包（三元组 → DTVCC 包 → service block）；C0/C1 命令（DF0-7/DLC/DLM/SPL/SWA/SPA/SPC/DLY/RST/HIDE/FF/CR/BS）、G0/G1/EXT1 字符；按服务号产出 cue，统计丢包 |
| DVB / DVD / PGS / XSUB | ⚠️ metadata + 包时间线 | 图形字幕，只做流级 metadata 与时间线；**不做 OCR / 像素合成**（边界见 §2.5） |

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

### 2.3 隐藏字幕状态机（D2）

实现在 `core/analysis/diagnostics/CeaCaptionDecoder.h`（纯 C++17，三个类）：

| 类 | 职责 |
| --- | --- |
| `Cea608Decoder` | 一个字节对进、cue 出。按场（field 1/2）分 CC1-CC4 **四个独立通道**，每通道各有"显示 / 预装载"两块显存与三种模式 |
| `Cea708Decoder` | 一个 cc_data 字节对流进。拼 DTVCC 包（sequence 2 bit + size 6 bit）→ 拆 service block（含扩展服务号）→ 跑 C0/C1 命令与 G0/G1 字符 |
| `CeaCaptionDecoder` | 门面。吃 MP4 的 2 字节对或 MPEG-2 的 cc_data 三元组，按 cc_type 分流，合并两个解码器的 cue 并按时间排序 |

为什么必须是有状态的解码器，而不是"抽可见字符"：

| 现象 | 旧实现（拼可见字符） | 状态机 |
| --- | --- | --- |
| pop-on 字幕 | 预装载时就当成已显示，**时间提前 0.5–1 s** | 只有 EOC 才上屏 |
| roll-up 滚动 | 旧行不擦除，文本越积越长 | CR 时按 2/3/4 行窗口滚动 |
| CC1 + CC2 双语 | 串成一句乱码 | 按通道位（控制码 bit3）分开，各自成 cue |
| EDM / ENM | 忽略 → 上一句刚走、下一句已在显示 | 各自只清对应显存 |
| 708 | 完全不解 | 按服务号（S1 主字幕 / S2 二语）分别产出 |

**cue 的切分口径**（两块显存 + 待提交状态）：

- 写入只更新缓冲，记下"画面开始变化"的时刻（`pending_ts`）；
- 只在**结构事件**（CR / EDM / EOC / ENM / 模式切换 / 收尾）提交，提交时先按
  `pending_ts` 结束上一条、再按同一时刻开新一条；
- 不在每次按键提交 —— 否则 paint-on 逐字符输入会产出 `O` / `ON` / `ONE` 三条 cue。

### 2.4 隐藏字幕的服务层结论

`SubtitleStreamInfo` 多了两块统计（只对 ClosedCaption 流填充），并汇总进流表的"说明"列：

- `cc608`：`channel_mask`（CC1..CC4 位图）、pop-on / paint-on / roll-up 次数、PAC / mid-row /
  特殊字符 / 扩展字符计数、XDS 条数、最大行号；
- `cc708`：出现过的服务号列表、DTVCC 包数 / 服务块数、**包序号跳变次数**（丢包）、窗口数。

配套两条规则：

| 问题 | 规则 id | 级别 | 触发条件 |
| --- | --- | --- | --- |
| 隐藏字幕流没有解出文本 | `subtitle.cc.no_text` | Warning | 有 CC 流但 `cue_count == 0` |
| DTVCC 包序号跳变 | `subtitle.cc.dtvcc_loss` | Warning | 708 的 `sequence_gap_count > 0` |

> 双语节目里 CC1 与 CC2、708 的 S1 与 S2 是**各自独立的时间线**，所以 cue 校验按
> `(流, caption_channel)` 分组 —— 按流整体校验会把合法的双语重叠误判成 `subtitle.cue_overlap`。

### 2.5 图形字幕的边界（D3 结论）

**结论：不做 OCR，也不做像素级合成分析。** 现状与理由是：

1. 依赖边界：`vcpkg.json` 只有 `qtbase`（+ 测试期 `gtest`），没有任何 OCR 引擎。
   引入 Tesseract 意味着几十 MB 模型、几十毫秒/帧的推理、以及一套新的准确率口径 ——
   对"交付审核"这个场景，收益远小于把误报解释清楚的成本。
2. 判定不会更可靠：图形字幕的实质缺陷（缺行、错位、超安全区）用 OCR 也判不准；
   而能做准的那几件事（是否为空 presentation、时长是否为零、窗口是否越界）
   **根本不需要解码像素**，只要读 PGS/DVB 的段头即可。
3. 现有产出已经够定位：流级 metadata（编码 / 语言 / forced / 包数 / 字节数）+ 包时间线，
   足够回答"这条轨在不在、覆盖多长、是不是 forced"。

如果将来要在这条边界内加值，**唯一推荐的增量**是按段解析 PGS/DVB 头（`PCS` / `WDS` /
`PDS`），产出"窗口数 / 对象数 / 画面尺寸 / 空 presentation / 零时长"这几项 ——
纯字节解析、零解码、零新依赖。当前**未实现**，不要把它当成已有能力。

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
| `tests/unit/test_cea_caption_decoder.cpp` | 608：pop-on 上屏时机（不提前）/ roll-up 每画面一条 cue / CC1-CC2 分离 / XDS 不进文本 / 特殊与扩展字符（含 UTF-8 列换算）/ ENM 与收尾；708：service 1 文本与清窗口、多服务分离、扩展服务号、包序号跳变、`cc_valid=0` 丢弃、608+708 混流按时间排序 |
| `tests/unit/test_aux_qc_rules.cpp` | 规则 id 是否齐全、重叠/空字幕级别是否为 Warning、倒序是否为 Error、未分析时不误报、时码缺失与 drop-frame 不符、章节重叠、SCTE-35 缺 duration |

验收对应关系：

- SRT cue 重叠 → `subtitle.cue_overlap` Warning ✅
- 空字幕 cue → `subtitle.cue_empty` Warning ✅
- timecode track 输出首帧时码 → `TimecodeAnalyzer::DecodeTmcdSample` + `AnalysisResult::timecode.primary` ✅
- SCTE-35 样本列出 cue event → `Scte35Analyzer::ParseSection`（含 event id / splice time / duration / OUT-IN）✅
