# 修复能力增强路线图（2026-11-10）

> **活文档**，随切片推进更新第 8 节进度表。代码基线是 `2e3b2a3`（2026-11-09）。
> 本文只定目标、顺序、判据与边界，不复述 API —— 接口看头文件，实现细节看 `core/repair/`。
> 本轮对上一份「下一阶段建议」做了逐条核对，凡与现状不符处已在第 1 节改写。

**目标**：恢复更多有效内容，同时能逐轨说明**恢复了哪些轨道、哪些时间区间、发生了什么损失**。
优先场景：普通 MP4 缺失 moov 的音视频混合文件、中间损坏后仍有完整 GOP 的文件。

**红线**：错误结论率优先于成功率。任何扩大覆盖的改动，先证明不会把「打开失败 / 被取消 /
超时 / 输出被上限截断 / 没扫全」报成「无问题」或「已修复」。缺失或被覆盖的内容若没有
其他副本，如实记录；补帧、静音、重复帧属于**合成结果**，与恢复出的原始内容分开呈现。

状态标记：`已实现` / `部分` / `空白` / `前置`（架构或验证条件未满足）。

---

## 1. 基线核对：为什么第 1 步要改写

上一份建议的第 1 步是「先修复上一轮发现的 8 项问题」。**F01–F08 已在 `2e3b2a3`
全部修完并推送**：163/163 全绿、9 组判别力变异全部精确判红、离屏启动 rc=124。
建议里列的验收条件（正常媒体逐轨守恒、30000/1001、取消 / 超时 / 输出上限 / 写入失败、
保码流路径按规范化访问单元比较）正是那一轮的内容。因此第一轮不再排 8 项修复，
改为「补齐基线缺口」（切片 A）。

| 建议中的主张 | 核对状态 | 证据 |
|---|---|---|
| 修复 8 项 + 建立守恒基线 | 已实现 | `2e3b2a3`；`tests/unit/test_repair_engine.cpp:1186/1561/1593` |
| 取消 / 超时 / 输出上限 / 写入失败正确终止 | 已实现 | `core/repair/RepairBudget.h`、`RepairTolerantTranscode` 的写入失败注入口 |
| 保码流按规范化 AU 比较 | 已实现 | `core/repair/RepairPayloadHash.h`（AAC ADTS 剥头），Executor 与 Verifier 共用 |
| 30000/1001 / B 帧 / VFR / 多音轨不加速丢帧 | 部分 | 分数帧率已测；B 帧 / VFR / 多音轨无**序列级**门禁，S08 `max_b_frames=0`、VFR 明确改写 CFR |
| 正常媒体守恒测试 | 部分 | 帧数 / 包数 / 时长 / 音轨数已有；`tests/support/MediaSequenceFingerprint.h` **只被导出测试引用**，repair 测试零引用 |
| 统一「已恢复样本」记录 | 空白 | 全仓无 `RecoveredSample` 类结构；现有的是区间级 / 轨级 / 全局级 |
| 分块扫描 + 有限缓存 + 共享预算 | 部分 | 各模块自设上限：整段 1 GiB / 64 MiB / 1 MiB；无统一框架 |
| 共享预算贯穿 | 部分 | `RepairBudget.h:11` 声称贯穿规划 / 发布，实际 Planner、FragmentForensics、两个 InitSegment、OutputWriter 都无 budget 参数 |
| 普通 MP4 重建只覆盖纯 H.264 / 纯 ADTS AAC 单轨 | 建议判断准确 | `RepairPlainMp4Rebuilder.h:3-16`；`:964` 多 mdat 拒绝、`:1015` 音视频交织拒绝 |
| 参考样片辅助重建 | 空白（普通 MP4） / 已实现（fMP4） | `RepairPlainMp4Rebuilder::Probe/Rebuild` 无 reference 参数；`RepairInitSegment.h` 已有 R03 核验链 |
| `TryResync` 依赖 demuxer seek + 关键帧标记 | 建议判断准确 | `RepairExecutor.cpp:246/284`；探测窗口只有 2 s / 128 包**读包**，不解码 |
| 恢复结果记成多区间、用户选缺口策略 | **已实现**，属重复建设 | `RepairRecoveredSegment`、`RepairGapPolicy`、`MergeContiguous` |
| POC 追踪器需「提取公共能力」 | 前置条件被高估 | `core/analysis/codec/` 已独立成 `VideoEyeCodec`，见 §5.1 |
| 轨道级转码 / 其他容器 / 多候选规划器 | 空白 / 部分 / 空白 | S08 写死「视频重编码 + 音频复制」；非 ISOBMFF 只通用 remux；`RepairPlanner::MakePlan` 返回单个 `RepairPlan` |
| 验收语料从完整文件受控制造损坏 | 已实现 | `tests/corpus/generate_real_corpus.py` 的 `drop_top_level_box` / `truncate_tail`，基准 `real_h264_aac_faststart.mp4` |

### 1.1 三处名实不符（会进报告，直接污染错误结论率）

| 位置 | 文案 | 实际 |
|---|---|---|
| `RepairPlanner.h:6`、`RepairCapabilityContracts.cpp:105` | S08 仍 unavailable | 已可运行：`RepairPlanner.cpp:146`、`RepairExecutor.cpp:319` |
| `RepairCapabilityContracts.cpp:45` | 「不反推普通 MP4 的 moov」 | 已有受限版 `RepairPlainMp4Rebuilder` |
| `RepairBudget.h:11` | 预算贯穿规划 / 发布 | 仅诊断 / 执行 / 验证 + 引擎层阶段间 `Checkpoint()` |

补充：`RepairFidelity.synthetic_frames_inserted` 在 `RepairEngine.cpp:409` 恒 `false` 与
「S08 不插入合成帧」一致，**不是漏记账**；是 `RepairCapabilityContracts.cpp:96` 的文案超前。
所以「合成内容单独呈现」目前是**空能力**，字段已备好，接通时不要用「字段早就有了」当证据。

> **后续（2026-11-14）**：这条已由切片 U（视频黑帧，§4.21）与切片 W（音频静音，§4.23）
> 接通 —— `synthetic_frames_inserted` 不再恒 `false`，音频侧落到
> `fidelity.synthetic_audio[]`。上面这段是当时的审计记录，原文保留；**重复帧填补仍是
> 空能力**，那条字段至今没有事实来源。

---

## 2. 目标与验收口径

上一份建议只给了四个指标名词，无法验收。这里定成可计算口径。

### 2.1 四个指标

| 指标 | 计算口径 | 排除 / 约束 |
|---|---|---|
| 恢复内容率 | 产物中能与原片**规范化访问单元哈希**逐一对齐的样本数 ÷ 原片实测样本数；分视频 AU / 音频 AU、逐轨出数 | 合成帧、重复帧、静音填补**不入分子**，单列 `synthetic_*`；跨容器 AAC 必须先剥 ADTS 头 |
| 逐轨完整性 | 每轨：包数、解码帧数、**显示顺序**（display_pts 单调 + 逐帧亮度滚动哈希对齐）、`[start, end]` 时间范围 | 帧数相等但顺序被换必须判失败；「音频样本数」口径先定死为解码后 PCM 采样点或 AAC AU×1024，`tests/support/TestMediaFactory.h` 的 `MediaProbe` 目前只有包级字段 |
| 时间轴偏差 | 对齐起点后 `max / avg |PTS_产物 − PTS_原片|`、时长相对误差、B 帧显示顺序倒置计数 | 连续拼接（`MergeContiguous`）的缺口平移要单独折算，不能混进「改了播放速度」 |
| 错误结论率 | 受控损坏语料上两类：**假成功**（丢了内容却报 `Repaired` / `Verified`）、**假完整**（没扫全却报无问题） | 每轮用判别力变异确认门禁判红；真实性见 §2.3 |

### 2.2 完整性标志分开落

分析成功 / 结论完整 / 是否抽样 / 是否超时 / 错误原因，分开字段，不合成派生值。
JSON / TXT / HTML 三种报告格式必须给出同一结论（项目已有格式一致性门禁）。
`RepairCoverage.bytes_scanned`、`reached_eof` 只能是**实测值**，不许用顶层盒大小之和或
`parse_ok && found_moov` 之类的派生式覆盖取证结果。

### 2.3 语料分层与各自能证明什么

- **受控语料（能算恢复率）**：完整原片 → `drop_top_level_box(moov)` / 中间覆盖 / 尾部截断，
  保留 `real_h264_aac_faststart.mp4` 作逐轨基准。现有 `generate_real_corpus.py` 已覆盖，
  扩充时优先加参数组合（B 帧、VFR、多音轨、HEVC），不要盲目堆样本数。
- **真实损坏文件（不算恢复率）**：无原片基准，只能验「结论自洽 + 不谎报 + 可复现」，
  例如缺 moov / 同步丢失 / 孤立分片。把「产物能打开」当成功率是错的。
- **正常媒体（零回归基线）**：任何扩大覆盖的改动，先证明健康文件的序列级守恒不变。
  ❗ `TestMediaSpec` 默认的**无 B 帧**程序化样本不能当逐帧基准：实测源解出 39 帧、
  保码流 remux 产物解出 40 帧，而两边逐包载荷序列完全一致 —— 是生成器尾部冲刷的
  「解码可见性」边界，不是修复链丢内容。逐帧基准用 ffmpeg 生成的
  `real_h264_aac_faststart.mp4`（含 B 帧），或 `spec.max_b_frames >= 2` 的样本。

### 2.4 判别力硬规则

每个切片必须：故意破坏判据 → 确认用例**精确报错** → 恢复 → 复跑绿。
恢复用字节写回 + `os.utime(path, None)` 刷新 mtime；**不要用 `git checkout -- <file>`**
（会抹掉该文件全部未提交改动）。有顺序语义的输出要单独钉死顺序。

---

## 3. 统一的样本恢复层：插在哪、字段怎么定

### 3.1 四层记账，只缺最底层

```
全局   RepairResult / RepairCoverage / RepairFidelity          已实现
轨道   RepairTrackConservation / verification.per_track       已实现
区间   RepairRecoveredSegment / RepairTimelineRepair / lost_ranges   已实现（A 补齐损失区间）
样本   RecoveredSample / RepairSampleLedger                     已实现（B）
轨/区间/执行总量的 packets_written 由账本求和（B）
```

证据**自下而上**：上层结论由样本层聚合而来。因此设计重点是「对齐现有结构、
不产生第二份真值」——`RepairTrackConservation` 的帧数 / 包数必须由样本层直接求和，
不允许两处各自统计后靠断言对齐。

### 3.2 `RecoveredSample` 字段

B 阶段实际落在 `core/domain/model/RecoveredSample.h`：三个枚举 + `RepairCodecConfig`
+ `RecoveredSample` + `RepairSampleLedgerSummary`。拆独立头是避免 `RepairTypes.h`
继续膨胀（原文预留的「字段膨胀时再拆」）。仍在 `core/domain` 叶子 / `NO_FFMPEG`
名单内，只许值类型 + 枚举 + string，**不许带 `AVPacket` / `AVCodecParameters`**。
`RepairResult` 只持 `RepairSampleLedgerSummary`，不持逐样本明细。

| 信息 | 字段 | 说明 |
|---|---|---|
| 源字节偏移、长度、轨道归属 | `source_offset` / `size` / `track_id` / `stream_index` | `track_id` 与 `stream_index` 分开：前者是容器轨号，后者是 demuxer 下标 |
| 编码配置及其有效区间 | `config_id` + `RepairCodecConfig{ codec, extradata_digest, valid_from, valid_to }` | 样本只持 id，不复制 avcC/hvcC/esds；参数集变化产生新 config |
| 解码顺序、显示顺序、随机访问点 | `decode_ordinal` / `display_ordinal` / `is_rap` / `rap_evidence` | `display_ordinal` 无证据时为 `unknown`，不填 0 |
| DTS / PTS / 时长及证据来源 | `dts` / `pts` / `duration` + `timestamp_evidence` | 区分原始时间戳与推导值；`timestamp_evidence` 见 §3.3 |
| 可信度、解码结果、损坏标记 | `confidence` / `decode_status` / `damage_flags` | 决定能否进入最终产物 |
| 归属区间 | `segment_index` | 指向 `RepairRecoveredSegment.index`，供「哪些区间」反查 |

### 3.3 三个枚举（避免用字符串反推类别）

- `timestamp_evidence`：`source_table`（moov/trun 原值）/ `container_box` / `bitstream`（VUI、tfdt、sidx）
  / `reference`（同源样片）/ `inferred`（推定，必须同时有 `inference_basis`）/ `unknown`。
- `confidence`：`verified`（有独立证据交叉核对）/ `probable` / `inferred` / `unknown`。
- `decode_status`：`not_attempted` / `ok` / `failed` / `not_decodable`。
  守恒契约：报 `ok` 必须有实际解码结果；失败必须有原因字段。

**启发式标注不能当硬真值**（项目已有教训）：可信度只用于决定是否采信，
不能写成「必须失败 / 必须成功」的判据。

### 3.4 分块扫描框架与共享预算

统一 `ScanWindow`（文件偏移 + 长度 + 轨上下文）+ 有界缓存（双上限：条数与字节数），
逐块 `budget->Checkpoint()`；禁止整段 `mdat` 读入内存（现状 1 GiB 是明确债务）。
producer 先接两个：容器解析（stsd/trun）、码流扫描（NAL/AU）；参考样片分析只产出
`RepairCodecConfig`，不产出样本。

`RepairRunBudget*` 必须补到：**Planner、FragmentForensics、InitSegmentReader、
InitSegmentBuilder、OutputWriter / 发布**。阻塞式 FFmpeg IO 要在打开前挂
`ffmpeg_io::AttachInterrupt()` 并按 `RemainingUs()` 设 deadline。
`RepairBudget.h` 的头注释要与实现一致，别再留「贯穿规划 / 发布」的空话。

### 3.5 不许做的事

- 不在 repair 公共头里泄漏 FFmpeg 或 `VideoEyeCodec` 类型（F03 已因公共头 include
  `libavcodec` 打红过 `LinkVisibilityTests`）。
- 不把推定值写成实测值；`inferred` 样本不得计入「已验证」的恢复率分子。
- 不让样本层与轨层各自统计同一事实。

---

## 4. 第一轮四项（切片 A–D）

依赖关系：A → B → {C, D}；C 与 D 可并行，但都依赖 B 的 `RecoveredSample`。
每个切片独立构建、独立验证、独立提交后再进下一片。

| 切片 | 目标 | 前置 | 状态（权威记录见 §8） |
|---|---|---|---|
| A | 基线收口：序列级守恒 + 名实相符 + 预算 / 损失区间结构化 | — | 完成（`2cec64c`） |
| B | 统一 `RecoveredSample` + 分块扫描框架 | A | 完成（`2cec64c`） |
| C | 一种录制格式的参考样片辅助 moov 重建 | B | 完成（`af73b69`） |
| D | 索引损坏后的直接扫描与 GOP 恢复 | B | 完成（层 2/3；层 1 未做，`5200d51`） |
| E / F / G | 显示顺序 / 时基证据 / 逐轨覆盖 | A–D | 完成（`5200d51` / `a73e42a` / `5932508`） |
| H | 参考辅助重建写 ctts（放开「参考无 ctts」） | C + E | 完成（`a8029e9`） |
| I | VFR 逐帧时长（参考 stts 非常量） | C + H | 完成（`1a7ebaf`） |
| J | S08 源侧保真属性探测与显式记账（阶段 6 起步） | I | 完成（`c4d1672`） |
| K | 真实损坏语料上的结论层守护（修 2 个真 bug） | A–J | 完成（`4856937`） |
| L | 本次明确未恢复项的显式标注（能力目录 8 条） | K | 完成（`6cd0a8a`） |
| M | 参考辅助重建路径（C/H/I）的结论层收敛 | K + L | 完成（`a7aac33`，见 §4.13） |
| N | fMP4 借参考初始化段路径的结论层收敛（修 3 个真 bug） | M | 完成（见 §4.14） |
| O | S08 重编码的编码器口径收敛（libx264 → 实测编码器） | N | 完成（见 §4.15） |
| P | S08 音画起点对齐（统一原点，保留源侧 Δ） | O | 完成（见 §4.16） |
| Q | 容器级 side data 真正继承（旋转矩阵 / HDR 静态元数据） | P | 完成（见 §4.17） |
| R | 能力边界的「支持程度」口径收敛（状态词表 + 三段范围） | Q | 完成（见 §4.18） |
| S | 合成初始化段的时基证据分级（sidx 声明 ≠ 已核验） | R | 完成（见 §4.19） |
| 阶段 5 | 时间轴与音画同步恢复 | B + 架构前置（§5.1） | 部分（只剩 `elst`） |
| 阶段 6 | 容忍式转码、其他容器、多候选规划器 | B + D | 进行中（J 已落地，见 §4.10 / §5.2） |

### 4.1 切片 A：基线收口

- **复用**：`MediaSequenceFingerprint.h`（逐包载荷哈希序列、DTS 单调、显示顺序 PTS 单调、
  逐帧亮度滚动哈希、空事实守护）、格式一致性门禁、`TestMediaFactory`。
- **新增**：
  1. repair 测试接入序列级守恒：正常媒体逐轨帧数 / 音频样本数 / 显示顺序 / 时间范围，
     覆盖 30000/1001、B 帧、VFR、多音轨；VFR 若当前引擎会改写，判据要写成「显式标注改写」
     而不是假装守恒。
  2. 修 §1.1 三处名实不符；`RepairCapabilityContracts.cpp:96` 的合成帧文案改成与实现一致。
  3. budget 补齐 §3.4 列出的五个环节，头注释同步。
  4. **保留** `RepairCoverage.skipped_ranges` —— 它记的是「扫描盲区」（如
     "mdat 载荷未逐字节扫描"），不是时间损失，改名会丢掉完整性标志。改为在
     `RepairResult` 新增结构化 `lost_ranges`（`RepairLostRange`：输入起止秒 / 可选字节
     范围 / `reason` / `evidence` / `recoverable`），由 `recovered_segments` 派生，
     不产生第二份真值表；JSON 加 `lost_ranges` 数组，TXT / HTML 的「恢复区间与时间轴」
     section 同步输出。无跳段时一行不出（反向守护）。
- **验收**：正常媒体零回归、三格式同结论、变异「去掉序列比对」/「预算不进 Planner」/
  「skipped_ranges 空表报完整」精确判红。
- **不做**：不扩大任何恢复策略，不碰 `RepairPlanner` 的动作选择逻辑。

### 4.2 切片 B：样本层 + 分块扫描

- **新增**：§3.2–3.4 的结构、枚举、扫描窗口；先接 H.264 / AAC 两个 producer；
  输出层统一按 `segment_index` 聚合，把 `RepairRecoveredSegment.packets_written` 与
  `RepairTrackConservation` 改成从样本层求和。
- **验收**：样本数守恒到上三层（反向守护：无样本时不得谎报有区间）；分块与整块扫描结果
  逐字节一致；预算 / 取消在扫描循环里逐迭代生效；缓存双上限溢出记残留问题而非静默丢。
- **B 落地（2026-11-10）**：
  - `core/repair/RepairSampleScan.{h,cpp}` 提供 `ScanWindow` / `SampleByteSource`
    （`MemoryByteSource` 整块对照 + `ChunkedByteSource` 有界分块）/ `RepairSampleLedger`
    / `CheckUpperLayerConservation` / `SampleSequenceDigest`。
  - `RepairPlainMp4Rebuilder` 的 ADTS / AVCC 两个 producer 改走分块字节源：
    **1 GiB 整段读入已删除**，每个 NAL 只读 64 KiB 语法前缀；`samples_rebuilt`
    与 `total_bytes` 改由账本求和。
  - 保码流重封装与 S08 转码的写包循环都写入账本；`outcome.packets_written`、
    逐区间 `packets_written`、`RepairTrackConservation.packets_written` 全部由账本派生，
    原来各自 `++` 的计数器已删除；守恒核对的 `problems` 进 JSON / TXT / HTML。
  - 「分块与整块逐字节一致」分两级验收：`SampleByteSource` 层用内存整块对照
    （只在测试里允许整段）；产品链用**不同块大小重建同一文件、产物二进制一致**
    （ADTS + H.264 两个 producer）。生产链不提供整块模式，避免把 1 GiB 债务放回来。
  - 时间证据分级：保码流 `ContainerBox` / `Probable`；无 moov 重建与 S08 为
    `Inferred` + `inference_basis`；`display_ordinal` 无 ctts / POC 证据时留空。
- **不做**：不接 HEVC / 音频其它编码，不改产物封装逻辑。

### 4.3 切片 C：参考样片辅助 moov 重建

- **复用**：`RepairInitSegmentReader` 的 R03 核验链（借来的 mdhd 时基必须与源侧
  sidx 时基、逐轨 trun 时长、逐 NAL 实测码流类型交叉核对）、`borrowed_parameters`、
  `compatibility` 四态、`RepairInitSegmentBuilder` 的 in-band 参数集解析。
- **三不借**：不借样本表（stts / ctts / stsz / stco）任何取值、不借 mdhd timescale
  当真值、不借 duration。**只借** stsd 解码配置（avcC / hvcC / esds）、NAL 长度前缀、
  track→codec 映射、ftyp 兼容 brands。样本大小、偏移、时间表一律从损坏文件重扫。
- **第一版格式指纹**：H.264 + AAC 单 mdat 交织、4 字节 NAL 长度前缀、AAC 恒定
  1024 采样/帧、采样率恒定。指纹不匹配一律 `unverified` + 拒绝，不猜。
- **拒绝条件**：多 mdat / 含 moof；参数集与 slice 语法不符；借来时基与源侧证据冲突；
  交织布局无法被「长度前缀 + ADTS 同步」唯一确定边界 → 该区间标 `不确定` 并记原因。
- **验收**：完整片删 moov 后与原片比逐轨样本数、显示顺序、时间范围；HEVC + AAC
  交织留作第二版；参考文件与源不同源时必须被拒（反向守护）。
- **不做**：不覆盖多 mdat / 分片 MP4 / 参数集完全不可恢复的场景。

### 4.4 切片 D：直接扫描与 GOP 恢复

- **三层**：
  1. 容器级重同步：损坏盒链之后寻找可信 `moof/trun/mdat`，校验盒大小不越界、
     `tfhd.track_id` 在配置里存在、trun 样本覆盖落在载荷区间、`tfdt` 单调递增。
  2. 码流级重同步：在可信载荷窗口内扫连续合法的 NAL / AU（H.264 length-prefix、
     HEVC 2 字节头 6-bit type），收集参数集与候选 RAP。
  3. 短窗口试解码：从 RAP 候选送有限帧给解码器，确认能建立有效解码状态
     （收到帧、无解码错误持续），才接入输出。
- **复用**：`RepairRecoveredSegment`、`RepairGapPolicy`（保留缺口 / 连续拼接已实现，不重做）、
  `DamageFinding("container.damaged_segment_skipped")`。
- **升级**：`recovery_point` 从自由文本升级为「证据来源枚举 + 描述」，
  例如 `av_seek_frame` / `moof_resync` / `bitstream_rap` / `trial_decode_ok`。
- **禁止**：把 `AV_PKT_FLAG_KEY` 当成可解码；把「探测到参数集」当成「配置有效」；
  把 2 s / 128 包读包探测当成试解码。
- **验收**：中间损坏样本产出「保留 0–12 s，跳过损坏段，恢复 15–60 s」两段区间；
  判别力变异「去掉试解码」应让半截 GOP 样本精确判红；`gap_policy` 两种模式时间轴
  分别符合预期。

**实现状态（2026-11-11）**：

- **层 3 已落地**：`core/repair/RepairTrialDecode.{h,cpp}`。从候选时间点 seek 过去，
  收同流最多 8 个包送真实解码器；首包必须同时满足「容器标记 KEY」+「NAL 里真有 IDR」
  +「解出 ≥1 帧」+「无 `AV_FRAME_FLAG_CORRUPT` / `decode_error_flags`」，才算通过。
  第一版只覆盖 H.264 且只认 4 字节长度前缀，其它编码返回 `attempted=false`
  （**没试过 ≠ 通过**，报告里必须写「未做试解码」）。
- **层 2 已落地（诊断向）**：`core/repair/RepairBitstreamResync.{h,cpp}`。逐块扫
  `mdat` 载荷，按 4 字节长度前缀走链，找到含 IDR/CRA 的候选 AU（H.264 / HEVC，
  HEVC 走 2 字节 6-bit type）。**它不写产物**：当前链路没有「不依赖索引的写包路径」，
  所以命中候选也只能写进 finding evidence 说「载荷里还有 N 个随机访问候选，但未接入
  输出」，绝不报成已恢复。这是本切片最明确的边界。
- **证据枚举已升级**：`RepairRecoveryEvidence`（`unknown` / `av_seek_frame` /
  `moof_resync` / `bitstream_rap` / `trial_decode_ok`），`RepairRecoveredSegment`
  新增 `recovery_evidence`，JSON 落机读码、TXT/HTML 落显示名，损失区间 evidence
  里也带上来源码。
- **层 1 未做**：容器级 `moof/trun/mdat` 重同步。它需要另一套「分片样本表重建 +
  自建 init/moof 后写包」路径，与 C 的 init 段核验链要先统一，且缺稳定的 fMP4
  中间损坏语料；本轮**明确不做**，不假装覆盖。
- **已知缺口**：端到端「拒绝不可解码恢复点」用例没能构造出稳定语料 —— 破坏中段
  关键帧载荷后，demuxer 直接丢掉了候选（`TryResync` 报「各步长均未找到可用关键帧
  候选」），测不到试解码那一层。因此该判据只在**模块级**守护
  （`RepairTrialDecode.RejectsCorruptedIdrPayload` 破坏 slice 中部，保留长度前缀与
  NAL 头合法）。等有真实「标记为 KEY 但解不出来」的样本再补端到端用例。


### 4.5 切片 E：显示顺序证据层（POC → 显示序 → ctts）

- **要解决的现实问题**：无参考重建产出的 moov 里没有 `ctts`，含 B 帧的源一旦走这条路，
  产物只能按解码序播放 —— 字节全对、画面顺序错，报告却看不出任何异常。
- **分三层**：
  1. **纯推导** `DeriveDisplayOrder`：给定解码序逐帧 POC 与帧时长证据，按随机接入点
     分段、段内按 POC 升序定显示序，给 `reordered_frames` / `max_reorder` /
     有符号合成时间偏移；跨段**不比较 POC**（POC 在 RAP 处归零是编码语义）。
  2. **实测** `MeasureDisplayOrder`：逐访问单元从分块源读载荷喂 POC 追踪器
     （`VideoEyeCodec` 私有链接，公共头不出现 codec 类型），逐单元检查取消与共享预算，
     单个 AU 超 8 MiB 直接判不可用（边界推断错了才会这么大）。
  3. **落盘** `BuildCttsBox`：写 `ctts` version **1**（有符号），连续相同偏移做
     run-length 压缩；条数与样本数不符、或结论不可用时**返回空**，让调用方"不写"。
- **三不借**（延续 R03 的思想）：不借绝对帧率（只认 VUI `timing_info` 这类源侧证据，
  没有就明确不生成 ctts）、不借参考样片的 POC、不把"追踪器能跑"当成"顺序已验证"。
- **明确不做**：开放 GOP 前导图像（RASL/RADL）直接判不可用；`pic_order_cnt_type == 1`
  由追踪器返回不可用 + 原因；变帧率下不生成 ctts（第 i 帧时长不是常数，乘首帧时长
  会把整条时间轴压错）；`elst` 不重建（用有符号 ctts 表达负偏移，不做"整体平移 + 裁开头"）。
- **验收**：真实 B 帧语料（`real_x264_bframes_gop.mp4`）上，按 POC 推出的显示序必须与
  按容器 PTS 排出来的顺序**逐项一致**；含重排的自建码流重建后产物必须有 `ctts`
  且偏移等于 `(显示序 − 解码序) × 样本时长`；无 B 帧的源必须**不写** `ctts`。

**实现状态（2026-11-11）**：

- 新增 `core/repair/RepairDisplayOrder.{h,cpp}`；`VideoEyeRepair` 增加
  `PRIVATE VideoEyeCodec`（公共头仍只有 domain 值类型）。
- `RepairPlainMp4Rebuilder`：重建前先实测显示顺序，`BuildMoov` 在有重排时追加 `ctts`；
  结论经 `RepairExecutionOutcome` → `RepairResult.display_order` → JSON/TXT/HTML。
- **语料限制**：仓库里没有"既有 B 帧、SPS 又带 VUI timing、参数集还在 in-band"的真实
  样本 —— x264 产 B 帧但不写 VUI，libopenh264 写 VUI 但不出 B 帧（实测 ffmpeg 6.1 +
  `h264_metadata=tick_rate` 也无法给 x264 的 SPS 补出 VUI）。所以 ctts 落盘那条路径由
  **按 H.264 语法手写的最小码流**覆盖（只保证过 SPS/PPS/slice_header 解析，不含可解码
  图像数据）；显示顺序的语义正确性由真实 B 帧语料对账容器 PTS 负责。这两条合起来
  才算覆盖，缺任一条都会留下"看起来绿"的空洞。


### 4.6 切片 F：时基证据（VFR 声明 + 显式推定帧率）

- **要解决的现实问题**：报告里每个"秒"都来自"一帧多长"。这个值只有三种来源 ——
  源侧证据（SPS VUI `timing_info`、容器 `stts`、ADTS 采样率）、用户显式给的、
  或者根本没有。前两种都被当成"源本来就这样"，第三种找不到时就拒绝。但中间有个
  真实缺口：`fixed_frame_rate_flag == 0`（码流自己声明不是恒定帧率）被直接读掉丢弃，
  VFR 源会被当成 CFR 重建，报告仍写"时间轴 unchanged"。
- **要解决的第二件事**：没有 VUI timing 的 H.264（很常见）目前一律拒绝重建，
  用户只能看着能力不可用。允许显式给帧率才叫"扩大恢复范围"，但必须写清楚这是推定。
- **三条边界**：
  1. **不猜**：没有 VUI 也没有显式帧率时继续拒绝，错误文案要指出补救方式。
  2. **精确**：帧率写成约分后的分数（`30000/1001`），timescale 取 `num*m`、
     sample_delta 取 `den*m`（m 最小使 timescale ≥ 1000），不做小数折算 ——
     29.97 的十进制近似三十秒就差半帧。
  3. **如实**：`mode` / `frame_rate` / `constant` / `vfr_declared` 分开落字段；
     推定时基下 `fidelity.timeline_status` 只能是 `modified`，绝不能是 `unchanged`。
- **明确不做**：`elst` 重建；VFR 逐帧时长恢复（没有样本表就没有证据，只标注）；
  `mdhd` version 1（32 位时长会溢出时直接拒绝，不写截断值）。
- **验收**：无 VUI 无帧率 → 拒绝且原因含补救提示；给 25 fps → 产物
  `timescale=1000 / delta=40` 且 `mode=user_input`；29.97 → `frame_rate=30000/1001`、
  `stts.delta=2002`；`fixed_frame_rate_flag=0` → 不写 `ctts` 且限制项明确；
  32 位时长溢出 → 拒绝。


### 4.7 切片 G：逐轨覆盖与音画共同区间

- **要解决的现实问题**：`lost_ranges` 只有"源时间轴上 4–6 s 没进产物"。多轨时这句话
  是错的粒度 —— 视频索引失效时音频可能全程连续，反过来音画两条轨的缺口起点也常常
  差几百毫秒。只报一个全局秒数，用户无法判断"丢的是画面还是声音"，报告也没法解释
  `duration` 为什么比播放器显示的短（最长轨 = 视频，音画共同覆盖其实更短）。
- **事实源**：样本账本的**流式统计**（`TrackCoverageStats`），每样本一次 O(1) 更新、
  内存固定。**不许在明细缓存上算** —— 明细超过 4096 条 / 64 MiB 就是抽样，
  拿它算区间会把"没缓存到的样本"误判成丢帧。缺口条数与总时长由两个定长直方图
  导出，明细截断只影响列出的前 N 条，不影响计数。
- **判据（缺口 = 真的缺内容）**：`间隔 ≥ max(0.5 s, 3 × 该轨典型间隔)`。
  - 相对项是必须的：10 s 一帧的延时摄影，"间隔 10 s"是正常的；
  - 下限也是必须的：30 fps 下丢十几帧不值得报成"损失区间"；
  - 典型间隔取间隔直方图（log2 毫秒分桶）众数桶，是**估算**，报告写明是近似值；
  - 基准用"已见最大时间戳"而不是 `abs(相邻差)`：B 帧重排与时间戳重置都不是丢失。
- **明确不做**：不按轨重建 `elst`；不推断缺口里"应该有多少帧"（没有样本表就没有证据）；
  不把逐轨缺口换算成恢复率分子（分母没有源侧基线）。
- **验收**：只有真缺内容的那条轨报缺口；延时摄影 0 缺口；0.4 s 空洞 0 缺口；
  重排 + 时间戳重置 0 缺口；明细截断时条数仍为全量精确值；共同覆盖是交集（不相交时
  明确写"未知"）；无时间戳样本单独计数。

### 4.8 切片 H：参考辅助重建写 ctts（放开「参考无 ctts」前提）

- **要解决的现实问题**：C 的第一版直接拒绝含 `ctts` 的参考样片。于是「有 B 帧 + 缺 moov
  + 参数集只在 moov 里」这三种条件同时成立时是**死路**：无参考路径需要 VUI 或用户帧率
  且拿不到 `avcC`，参考路径又因为参考自带 ctts 被拒。而这类文件恰恰是最常见的录制格式。
- **改法**：参考可以含 `ctts`，`ReadProfile` 只记录 `has_ctts`（进 limitations），
  合成偏移**一律不借** —— 它是「这份文件的帧」的属性，照抄等于把另一份文件的显示顺序
  按到本片上。产物 ctts 由损坏文件自身的码流 POC 实测得到（复用切片 E 的
  `MeasureDisplayOrder` / `BuildCttsBox` 语义，写 version 1 有符号 + run-length 压缩）。
- **三条边界**：
  1. **条数守恒**：实测偏移条数必须等于视频样本数，否则**不写** ctts 并如实标注
     （写半张表比不写更糟：播放顺序会错且看不出来）。
  2. **不拿解码序冒充显示序**：`display_ordinal` 只在「实测帧数 == 视频 AU 数」时才用；
     否则退回解码序且不写 ctts。
  3. **样本层同步**：`pts_seconds` 带上实测偏移，`inference_basis` 写明
     「显示顺序由源侧码流 POC 实测」，不把实测值混进「参考 stts 推定」。
- **明确不做**：不借参考的 POC；不重建 `elst`；开放 GOP 前导图像仍判不可用（沿用 E）。
- **验收**：含 B 帧的真实双轨语料（`real_h264_aac_faststart.mp4`）删 moov 后重建成功，
  产物视频轨**有** `ctts`，且逐包 PTS 间隔与原片**逐项一致**（ctts 缺一条或偏移算错都会判红）。

### 4.9 切片 I：VFR 逐帧时长（参考 stts 非常量）

- **要解决的现实问题**：参考样片的 `stts` 是「每帧多长」**唯一**的源侧证据，而 C 的
  第一版要求它必须是单条恒定 delta（只容忍一个短尾帧）。真 VFR 素材（手机/相机录制、
  屏幕录制、变速导出）的 stts 天然多段，于是这类文件在「缺 moov + 参数集只在 moov
  里」时是死路 —— 而这恰恰是参考路径存在的理由。切片 F 放开的是**码流声明** VFR，
  这一片放开的是**参考样本表**里的真实逐帧时长，两者不是一件事。
- **改法**：`ReadProfile` 接受视频轨非常量 stts，把完整 run 序列存进
  `RepairReferenceTrackProfile.stts`；`ScanSource` 展开成逐帧时长表，按序给每个样本
  赋值（DTS 用整数 tick 累加，不用「序号 × 平均帧长」）；`BuildMoov` 按原 run-length
  写回多条 `stts`，`mdhd`/`tkhd`/`mvhd` 时长一律取逐帧表合计。
- **三条边界**：
  1. **只在样本数守恒时借**：`ScanSource` 已强制「视频样本数 == 参考样本数」，
     展开后帧数不等直接拒绝 —— 借一张长度对不上的表，等于把后半段每一帧的时长都
     算错，而且看不出来。
  2. **音频不放开**：AAC 恒为 1024 采样/帧，非常量 stts 仍拒绝（保留短尾帧例外）。
  3. **VFR 不写 `ctts`**：`ctts` 的偏移换算是「帧数 × 帧长」，帧长不恒定就没有定义，
     硬写会把整条时间轴压错。有 B 帧时宁可退回解码序，并在限制项写明「播放顺序可能
     与源片不同」。
- **顺带补的两个洞**：
  - `mdhd` 时长是 32 位，逐帧表合计可能溢出 → **拒绝**（本版不写 mdhd version 1）；
    以前恒定路径同样会静默截断，只是样本数不够多没暴露。
  - 账本时间轴自洽校验：末帧「起点 + 时长」必须等于逐帧表合计。stts 盒写对、账本按
    「序号 × 主 delta」算时间时产物照样能播，但报告里的样本时间戳是错的，而两处各自
    看都「说得通」。
- **明确不做**：`elst` 重建（沿用 F）；无参考时的 VFR 逐帧恢复（没有样本表就没有证据，
  只标注）；`mdhd` version 1。
- **验收**：VFR 参考可读（`variable_stts=true`、run 序列原样、主 delta 取 count 最大段
  而不是首段）；重建后产物 `stts` 逐段等于参考、`mdhd` 时长等于合计、**逐包 DTS 间隔
  与逐帧表一致**、无 `ctts`；音频轨非常量仍拒绝；时长溢出拒绝。

### 4.10 切片 J：S08 源侧保真属性探测（阶段 6 起步）

- **要解决的现实问题**：S08 把解码帧统一重编码成 8-bit `yuv420p`。源侧的
  位深、色彩标记、像素宽高比、色度采样位置、旋转 / 镜像矩阵、HDR 静态元数据
  全部不会跟着画面过去 —— 旧实现既不继承也不报告，产物能播，报告里只有一句
  「有代际画质损失」。10-bit HDR 手机录像被压成 8-bit SDR、竖屏视频横躺，
  用户看不到任何解释。路线图 §5.2 明确写了「补之前产物不能声称视觉无损」，
  这一片就是那个前置条件。
- **改法**：新增 header-only `RepairTranscodeFidelity.h`，用**首帧 + 输入流 +
  编码器实际选定的像素格式**逐项对照，输出 `model::RepairTranscodeFidelity`：
  - 能零风险继承的直接写进产物，并进 `preserved_properties`：
    像素宽高比、色度采样位置（落在 `AVFrame` 上，FFmpeg 8 起 `AVCodecContext`
    已无此字段）、以及**位深不变时**的色彩 primaries / transfer / space / range。
  - 色彩标记**只在位深不变时**继承：给 8-bit SDR 画面贴 `bt2020`/`smpte2084`
    会让播放器按 HDR 渲染，比不写标记更糟。位深变了就老实记损失。
  - 位深下降、色度抽样下降、旋转 / 镜像矩阵、HDR 静态元数据逐项进
    `lost_properties`。本版**不尝试继承旋转 / HDR**（低比特深度产物沿用源 HDR
    标记是错的），如实记账即可。
  - `inspected=false` 是独立状态：保码流路径 / 转码中途失败都保持未探测，
    空表不许冒充「属性均已保留」，`FinalizeTranscodeFidelity` 也不会把没探测
    过的清单抬成 `all_properties_preserved=true`。
- **顺带修的安全洞**：`sws` 原先按首帧尺寸 + 像素格式建一次就再也不动。源片
  中途换尺寸 / 换格式时继续喂旧 context 是**越界读**。现在用
  `TranscodeScaleState` 逐帧实测：变了就重建 context（重建失败宁可显式丢帧），
  并累计「尺寸与产物不同」的帧数写进报告；只有重建成功才 `Commit`。
- **构建契约同步**：新头在 `core/repair` 公共头闭包里带 `libav*` include，
  `VideoEyeRepair` 的 `FFmpeg::avcodec/avformat/avutil` 必须从 PRIVATE 提到
  **PUBLIC**（静态库 PRIVATE 只传 link、不传 include 目录）。这是
  `audit_link_visibility.py` 首轮判红抓到的真实问题，不是门禁误报；
  `media` / `ffmpeg_io` / `codec` 仍保持 PRIVATE。
- **落账链**：`RepairExecutionOutcome.transcode_fidelity` → `RepairEngine` →
  `RepairFidelity.transcode` → JSON `fidelity.transcode` 子对象 +
  TXT/HTML「转码保真探测」章节（同源 `BuildSections`）；
  另有 `video.transcode_property_loss` 一条独立 `DamageFinding`。
  非转码路径在三种格式里都明确写「未探测」。
- **明确不做**：旋转 / HDR 元数据的继承（需单独验证 muxer 是否真的写 `tkhd`
  矩阵，未验证前不宣称）；10-bit 编码器选型；逐轨转码决策；GOP 局部重编码；
  `elst`（沿用 F）。
- **验收**：13 + 2 = 15 个新用例（纯内存构造 `AVStream`/`AVFrame`/`AVCodecContext`
  逐项验证判别力 + 两个端到端三格式一致性 + 两个逐帧缩放状态守护）；
  11 组判别力变异全部精确判红后按字节恢复复跑绿。

### 4.11 切片 K：真实损坏语料上的结论层守护

- **为什么先做这一片**：A–J 给每个新字段都写了单测，但「真实受控损坏语料跑完整条链
  之后，报告还能不能逐轨说明恢复了哪些轨道 / 哪些区间 / 发生了什么损失」一直没人验。
  原 `RealCorpus*` 5 条用例只断言 `output_published` / 状态 / 段数 / 可解码帧数；
  `timeline_basis` / `display_order` / `fidelity.transcode` 在 `test_repair_engine.cpp`
  里**零引用**。另有 `lost_ranges` 与 `sample_ledger` 的端到端用例，但用的是
  `CreateTestMedia` 合成媒体，不是从完整文件受控制造损坏、有原片基准的语料。
- **摸底实测**（5 个场景：healthy / mid-damaged / tail-truncated × 保码流；
  healthy / mid-damaged × RecoverContent）暴露的不只是「缺断言」：
  1. **S08 路径报告撒谎**：`sample_ledger.available=1`、88 个样本，
     但 `track_coverage.available=0` / `tracks=0` / `unavailable_reason` 为空；
     TXT 因此写「逐轨覆盖: 未接入（**本次执行没有样本账本**）」。
     把「没统计」报成「没数据」，正是红线第 1 条禁止的错误结论。
  2. **时基无证据时静默省略**：`timeline_basis.mode=unknown`（S08 按推定帧率重建）
     时，TXT/HTML 整个「时基依据」章节消失，读者只能默认「时间轴大概没问题」。
  3. **显示顺序未测量时静默省略**：`display_order.analyzed=false` 时同样整节消失。
  4. `RepairSampleScan::Coverage()` 在没有任何样本时写「没有任何样本带时间戳」，
     把「没样本」说成「没时间戳」，口径不准（顺手修正）。
- **改法**：
  - `RepairTolerantTranscode` 收尾上抛 `sample_ledger.Coverage()`，按输出流下标填
    `kind` / `codec`（在流创建时捕获，`out`/`in` 释放前），`complete` 用本次执行的
    真实终止原因计算，并追加一条时间轴口径限制。
  - **刻意不补输入侧时间戳**：S08 产物时间轴按推定帧率从零重建，与 `lost_ranges`
    的输入时间轴不同轴；解码器异步、源包与输出帧不是一一对应，硬关联只会造出
    假区间。因此如实给「每轨恢复了多少样本 + 全部无输入侧时间戳 + 覆盖区间未知
    + 不可与损失区间相减」，而不是编一段 span。`gap_count=0` 的语义是「未判定」，
    不是「无损」。
  - 报告层：`available=false` 时按 `sample_ledger.available` 区分「有账本但没统计」
    与「确实没有账本」；`timeline_basis.mode=unknown`、
    `display_order.analyzed=false` 两种未测量态在 TXT/HTML 显式出行。
  - 同步修正 `RepairTypes.h` / `RepairExecutor.h` 里「例如转码路径没有账本」的过时注释。
- **验收**：5 个端到端用例（保码流三态 / 截断不许标完整 / S08 不许伪造 span /
  兜底文案反向守护 / 未测量态三格式）；10 组判别力变异全部精确判红后按字节恢复
  复跑绿。语料缺失用 `ASSERT_TRUE` 而非 `GTEST_SKIP` —— 结论层守护静默跳过等于没守护。
- **明确不做**：S08 输入时间轴覆盖区间的重建（需另做源包→输出帧映射并验证 B 帧
  重排）；`elst` 等「明确不恢复项」的显式标注由切片 L 落地（见 §4.12）；
  参考辅助重建路径（C/H/I）的纳入由切片 M 落地（见 §4.13）。

### 4.12 切片 L：本次明确未恢复项的显式标注

**要解决的问题**：路线图里写过十几次「明确不做」，报告里却一个字都没有。
「修复成功 + 空白」会被读成「这些方面也没问题」，与 K 修掉的
「未测量态静默省略」是同一类错误结论。空表的三种含义（真的没命中 /
没评估 / 被省略）必须在结论层分开。

**口径（红线）**

- 这是**能力声明**，不是本次损失清单。源是否真的含有 `elst` / HDR 没有检测，
  所以 `consequence` 只能写「不保证保留」，不许写「已丢失」。
- 只有本次执行 / 诊断里存在**结构化触发事实**才挂载一条；触发事实写成
  `字段=值`（如 `fidelity.video_reencoded=true`），不从 `summary` / `error` /
  `limitations` 等自由文本反推类别。没事实就返回空表，并显式写「未触发」。
- `available` 与 `items` 分开：评估过但零命中 ≠ 根本没评估；`available=false`
  的未测量态照样在三种格式里出行。
- 诊断 / 执行之前就结束（请求预检失败）时挂**零条**：还没采用任何策略，
  把全局免责声明糊上去就是「无事实却宣称不支持」。
- `timeline_basis.mode` 默认就是 `unknown`，它本身不是事实；只有
  「本次确实 attempted 过执行」时才允许据此挂 VFR 那条。

**交付**

- `RepairTypes.h`：`RepairDeclaredExclusion` / `RepairDeclaredExclusions`，
  `RepairResult.declared_exclusions`。
- `RepairDeclaredExclusions.{h,cpp}`：8 条静态目录 + 纯函数命中评估。目录是
  `edit_list_not_rebuilt` / `vfr_frame_durations_not_recovered` /
  `original_display_offsets_not_restored` / `input_timeline_span_not_reconstructed` /
  `synthetic_content_not_inserted` / `transcode_properties_not_inherited` /
  `fragment_resync_not_attempted` / `gop_local_reencode_not_attempted`。
- `RepairEngine.cpp`：把原 `Run` 改名为匿名命名空间的 `RunCore`，新的 `Run`
  是唯一收尾点，覆盖预检失败 / 诊断流产 / 取消 / 超时 / 成功发布全部返回路径。
  ❗ 刻意不用 RAII 守卫的析构来收尾 —— `return result` 若发生 move，析构改的是
  已被搬走的局部对象，字段会静默丢失。
- `RepairReportExporter.cpp`：JSON 顶层 `declared_exclusions` + TXT/HTML
  「本次明确未恢复项」**恒定章节**（未评估 / 零命中 / 有命中三态都出行）。

**明确不做**：任何一条排除项的「实际恢复」都不在本片范围（L 只把口径说清，
不重建 `elst`、不继承旋转 / HDR、不插合成帧、不做 GOP 局部重编码）；
参考辅助重建路径（C/H/I）由切片 M 纳入真实语料矩阵（见 §4.13）；S08 输入侧覆盖区间仍不给。

### 4.13 切片 M：参考辅助重建路径（C/H/I）的结论层收敛

**目标**：K/L 的真实语料矩阵只钉住保码流与 S08 两条主路径。把 C/H/I
（普通 MP4 缺 moov + 同源参考 MP4 → `container.mdat_reference_assisted`）
纳入矩阵，先做**摸底实测**再决定改什么 —— 结果在一次执行里抓到 **4 处自相矛盾
的结论**，全部是「无事实却给结论」这一类错误：

1. `fidelity.timeline_status = "unchanged"`：这条路的 `mdhd timescale` /
   `stts sample_delta` 是从参考片借的，源侧 moov 已丢失，没有独立时基基线。
   能力契约自己写着「只能是 `modified` / `unknown`」，代码却写了 `unchanged`。
   根因是 R03 那条红线只覆盖 fMP4 初始化段借用（`reference_init`），没覆盖
   `reference_assist`。
2. `fidelity.reference_compatibility = "not_applicable"`：同一份报告的
   「恢复区间」章节写着 `reference_assist.compatibility = verified`，
   保真度章节却说参考不适用。`borrowed_parameters` 同样没上抛。
3. `fidelity.perceptual_fidelity = "unknown_without_reference"`：本次明明**有**
   同源参考样片，只是没做逐帧画质比对。把「有参考但没测」写成「没有参考」
   是事实错误。
4. 「恢复区间」把时基依据写成「容器时间戳」：重封装阶段只在 `mode == "unknown"`
   时补 `container_pts`，参考辅助路径没提前占住 mode，于是「借来的时基」
   被渲染成「源容器时间戳」。

另外 `RepairVerifier` 的画质限制文案硬写「需要参考样片」—— 它不知道调用方
有没有给参考，这条路明明有参考，同一份报告里两句直接打架。

**做法**

- `RepairExecutor`：参考辅助重建成功后写 `timeline_basis.mode = "reference_borrowed"`、
  `frame_rate = "unknown"`、`constant = false`，并附「逐帧时长来自参考样本表，
  不是损坏文件实测值」的限制行。占住 mode 的另一个作用：不让重封装阶段把它
  改写成 `container_pts`。
- `RepairEngine`：`timeline_unknown` 并入 `reference_assist_used`；
  unknown 分支的 note 按来源分岔（参考样片借用 / fMP4 初始化段借用）；
  参考辅助分支上抛 `reference_compatibility` 与 `borrowed_parameters`，
  兼容性非 `verified` 时把 `compatibility_reason` 落进 limitations，
  `perceptual_fidelity` 改 `not_measured_with_reference` 并明说
  「有同源参考但未做逐帧比对，不能声称画质无损」。
- `RepairReportExporter`：`reference_borrowed` 渲染成「参考样片时基（借用，
  源侧无独立证据）」，并额外出一行「时基证据边界」+ 一条「借用的时基不能作为
  源片时间轴的证据」—— 只写 mode 会让读者把「有依据」误读成「源片已核验」。
- `RepairVerifier`：文案改「本次未启用画质比对」，不再替调用方断言有没有参考。
- `RepairDeclaredExclusions`：`vfr_frame_durations_not_recovered` 增加
  `timeline_basis.mode=reference_borrowed` 触发事实（目录仍 8 条），
  让「逐帧时长不可信」在这条路上也有结构化依据。

**守护**

- `RealCorpusReferenceAssistConclusionLayerIsConsistentInAllFormats`：真实语料
  `real_h264_aac_missing_moov.mp4` + 同源 `real_h264_aac_faststart.mp4`，
  钉住 `timeline_status=unknown`、`reference_compatibility` 与恢复区间同源、
  `perceptual_fidelity=not_measured_with_reference`、三格式都出行。
- `RealCorpusReferenceAssistUnmeasuredStatesAreExplicitInAllFormats`：未测量态
  （时基 / 显示序 / 画质）必须显式出行，不许静默省略。
- `MainPathsDoNotLeakReferenceBorrowedConclusions`：**反向守护** —— 保码流与
  S08 两条主路径不得出现「参考样片时基」这类结论（防止修一处串味两处）。
- `RealCorpusForeignReferenceNeverClaimsVerifiedBasis`：不相干参考片（源侧
  重建失败）不得被说成 `verified` 依据。
- `RepairDeclaredExclusions.ReferenceBorrowedBasisIsAnExplicitStructuredFact`：
  纯函数正反表驱动 + 目录覆盖守护，目录条数不变。

**判别力变异查出的两个覆盖缺口**（第一轮 M5 / M7 判绿）

1. `fidelity.borrowed_parameters` 没有任何断言：用例只覆盖了「恢复区间」章节里
   `reference_assist.borrowed_parameters` 那一份，保真度章节即使不上抛也照样绿。
   已补「借用参数: ftyp 兼容 brands（原样）」与「时间轴说明里带参考借用句」两条断言。
2. `if (assist.compatibility != "verified")` 这条限制**永远不触发**：`used == true`
   时兼容性由重建器唯一写入点保证是 `verified`，留着就是假覆盖。已删除，并留下
   注释说明契约与「哪天放开必须加回 + 配用例」。真正该守的不变量
   （非 `verified` 的参考根本不会被 `used`）由 `ForeignReferenceNeverClaimsVerifiedBasis` 守着。

**明确不做**：借来的时基**仍是借的** —— 本片只把口径写对，不做「用参考片
反推源片时间轴」的独立核验；S08 输入侧覆盖区间仍不给；逐轨转码决策 /
其他容器 / 多候选规划器仍属 §5.2。

### 4.14 切片 N：fMP4 借参考初始化段路径的结论层收敛

**为什么还要再查一次**：M 收敛的是「普通 MP4 缺 moov + 同源参考 MP4」（C/H/I，
`container.mdat_reference_assisted`）。fMP4 缺初始化段（`reference_init`，
路径 `container.init_segment_missing`）走的是另一条链 —— 把参考文件的
`ftyp+moov/mvex/trex` 前置到源分片前面再重封装，R03 那条红线一直覆盖着它，
但**从来没有真实语料端到端用例**（§4.13 的未覆盖项）。同源语料其实早就有了：
`real_h264_aac_frag_orphan.mp4` 就是 `real_h264_aac_frag.mp4` 删掉 `ftyp` + `moov`
派生出来的。

**摸底实测一次抓到 3 处「无事实却给结论」**（全部是报告层，不是恢复能力）：

1. **「恢复区间」把借来的时基写成「容器时间戳」**：`timeline_basis.mode` 在
   `reference_init` 这条路上没人占位，重封装阶段照 `mode == "unknown"`
   补成 `container_pts`，于是同一份报告一边写「时基依据: 容器时间戳」，
   一边在保真度章节写「本次借用了参考初始化段的时基 / 时间轴变化 unknown」。
   与 M 修掉的是同一类错误，只是路径不同（M 改的是 C/H/I）。
2. **「产物实测=0.000s」**：`observed_duration_seconds` 只在「源侧有基线且可比对」
   时才记录，没有基线时写 0，报告于是印出「产物实测=0.000s」—— 而同一份报告的
   逐轨实测明明写着 1.221 s。0 只能表示「真的量到 0」，不能兼任「没量」。
3. **恢复比例的基线来源说反了**：旧文案一律写「按参考初始化段声明的逐轨样本数」，
   而参考初始化段里根本没有样本数（只有 `trex` 默认值），基线实际来自源侧
   `moof/trun`（诊断里写着 `moof/trun (2 segments)`）。

**做法**

- `RepairExecutor`：借用（`!synthesized`）初始化段成功后占住
  `timeline_basis.mode = "reference_init_borrowed"`、`frame_rate = unknown`、
  `constant = false`，detail 与限制项写明「timescale 借自参考初始化段；
  逐样本时长 / 字节偏移来自源侧 moof/trun」。自建（`synthesized`）分支不占位：
  它的 timescale 来自源侧 sidx / 分片声明，没有借另一份文件的东西。
- 刻意**不复用** `reference_borrowed`：C/H/I 借的是整张样本表（`stts`），
  fMP4 借的是起点（`mdhd` timescale）+ 解码配置，逐样本时长仍是源侧的。
  两者合成一个 mode 会让「逐帧时长是否也是借的」这个判断失去依据
  （`vfr_frame_durations_not_recovered` 就是靠 `mode=reference_borrowed` 挂载的）。
- `RepairReportExporter`：新 mode 渲染成「参考初始化段时基（借用，源侧无独立时基证据）」，
  并额外出「时基证据边界」行，明说哪一部分借了、哪一部分是源侧的。
- `RepairVerifier`：`observed_duration_seconds` 改为**真的量到的产物时长**，
  `timeline_comparable` 单独计算（源侧有基线 && 产物量得到）。可比对时的差值
  仍取「第一条可比对轨」，不改变 R03 的判定口径。
- `RepairEngine`：恢复比例的基线来源按 `RepairExpectedTrack.provenance` 落文案，
  不再硬写「参考初始化段声明」。

**守护**

- `RealCorpusInitSegmentBorrowedTimebaseIsExplicitInAllFormats`：真实语料
  `real_h264_aac_frag_orphan.mp4` + 同源 `real_h264_aac_frag.mp4`，钉住
  `mode=reference_init_borrowed`、三格式同一结论、反向守护「不许出现
  `时基依据: 容器时间戳`」、产物实测时长非 0 且 `可比对=no`、
  基线来源写 `moof/trun`。
- `MainPathsDoNotLeakReferenceBorrowedConclusions` 加两条反向断言：
  保码流与 S08 两条主路径不得出现「参考初始化段时基（借用…）」或其证据边界行
  —— 修一处串味两处是这类改动的典型事故。

**明确不做**：借来的 timescale **仍然是借的**，没有做「用参考初始化段反推源片
时间轴」的独立核验（源侧 sidx 时间基为 0 时根本没有可比对的基线）；
S08 输入侧覆盖区间仍不给。
（切片 N 当时把「合成初始化段（`synthesized`）路径的时基证据分级」也划在「不做」里，
已在 §4.19 补上。）

---

### 4.15 切片 O：S08 重编码的编码器口径收敛

**起点**：走查 §5.2 剩余能力时撞到一件比「加能力」更要紧的事 —— 本项目锁定的
FFmpeg 是 **LGPL 变体**（`cmake/ffmpeg-version.json`：
`--disable-libx264 --disable-libx265`，这是为了能随包分发 av\*.dll 的刻意取舍），
`avcodec_find_encoder_by_name("libx264")` 恒返回 null，S08 走兜底后实际启用的是
**libopenh264**（运行日志每帧都在打 `layerId(0) doesn't support profile(578)`）。

但代码与文档一直在说 libx264：

| 位置 | 旧文案 | 实际 |
|---|---|---|
| `RepairCapabilityContracts.cpp` `decodable_gop_transcode.evidence` | 「以 libx264 重编码」 | libopenh264 |
| `RepairDeclaredExclusions.cpp` `gop_local_reencode_not_attempted.evidence` | 「视频 libx264 重编码」 | 同上 |
| 同一份报告的 `fidelity.limitations` | 「编码器=libopenh264」 | 同上 |

于是「未实现能力边界」段说 libx264、「保真度与已知限制」段说 libopenh264——
与 M / N 修掉的是同一类错误：**静态文案把选型期望写成了既成事实**。
另外 `profile(578) → UNSPECIFIC` 这次静默降级从未被记录。

**做法**

- 新增结构化字段（唯一事实源）：`RepairTranscodeFidelity.encoder_preferred /
  encoder_used / encoder_fallback / source_profile / output_profile`，以及
  由修复层一次渲染好的 `encoder_note` / `profile_note`。
  ⚠️ 渲染**只做一次**：报告行、限制项、JSON 全读同一份文案。两处各拼一份必然漂移，
  这次要修的正是这种漂移。
- 编码器事实在 `avcodec_open2` 之后就地固化（`encoder->profile` 只有 open 后才有值，
  encoder 上下文随后会被 free），并**与 `inspected` 解耦**：没探测到首帧的路径上
  编码器仍然打开过，事实不能跟着 `inspected=false` 一起丢。
- 产物侧 profile 拿不到编码器回报时留空，由 `TranscodeProfileNote` 明说
  「编码器未回报，未能比对」—— 不推断「大概还是 High」。
- 三处文案改为中性：契约改成「重编码首选 libx264，但 LGPL 构建必然回退，实际编码器
  由运行时探测写入 `fidelity.transcode.encoder_used`」；排除项改成「视频轨统一按
  H.264 重编码，实际编码器以报告『重编码编码器』一行为准」。
- 报告新增「重编码编码器」「重编码 profile」两行（TXT/HTML 同源），JSON 落
  `encoder_used` / `encoder_fallback` / `encoder_note` 等机读字段 —— 让下游判断
  「这份产物是谁编的」不必正则匹配自由文本。

**守护**

- `RepairTranscodeFidelity.EncoderLabelNeverInventsAnEncoder`：表驱动覆盖
  「未走到编码器 / 首选可用 / 回退 / preferred 缺失」四态，反向守护「回退时不得
  把首选名当成实际编码器」。
- `RepairTranscodeFidelity.ProfileNoteRequiresMeasuredBothEnds`：源侧未声明不写、
  产物未回报写「未能比对」、相同写「一致」、不同写「已改变」。
- `RepairTranscodeFidelity.S08ReportsMeasuredEncoderInsteadOfPreferredName`：
  端到端钉住「字段 == 报告行 == JSON」，并断言报告里任何位置都不出现
  「libx264 重编码」。
- `RepairTranscodeFidelity.NonTranscodePathHasNoEncoderFact`：保码流路径不得凭空
  出现编码器行（没转码就没有编码器事实）。
- `RepairDeclaredExclusions.StaticTextNeverStatesEncoderAsFact`：能力目录 +
  排除项的全部文案不得出现「<编码器名> 重编码」这种把实测事实写死进静态模板的句子。

**明确不做**：不做「按构建自动选择最优编码器」的策略层（换 GPL 变体涉及许可证口径，
不是代码问题）；10-bit 编码在 libopenh264 上物理不可行，要等价于换 FFmpeg 变体；
`profile` 降级的**实际取值**仍未取到（libopenh264 不回填 `avctx->profile`），
报告如实写「未能比对」而不是猜一个。

---

### 4.16 切片 P：S08 音画起点对齐（统一原点）

**起点**：做切片 O 的端到端用例时顺手量了一下产物的两条轨起点，结果难看得不像
「修复成功」：

```
源片   video_start=0.480  audio_start=0.000   Δ=0.480
产物   video_start=0.000  audio_start=0.000   Δ=0.000   ← 差值被静默抹平
```

根因是两条轨**各归各的零**：

| 轨 | 旧实现 | 后果 |
|---|---|---|
| 视频 | `converted->pts = encoded_frames`（帧序号即 PTS） | 永远从 0 开始，源侧起点信息直接丢弃 |
| 音频 | 逐轨减去自己首包 PTS | 每条轨从 0 开始，轨间相对关系消失 |

两条轨互不知情，于是「视频比音频晚 0.48 s」这种源片（首段视频丢失、录音被打断、
`-itsoffset` 做出来的片子）修完就变成口型对不上，而且**报告一个字都不提** ——
比报错更糟：用户以为修好了。这与 M / N / O 修的是同一类毛病：**产物属性被改了，
结论层却声称没改**。

**做法**

- 先取每条参与输出的轨的**源侧呈现起点**，取其中最小者作**统一原点**，
  所有轨减同一个原点。起点差于是被保留。
- 起点取值有优先级：**`stream->start_time`（容器声明，已含 `elst` 补偿）＞
  首帧 PTS ＞ 首包 PTS**。❗ 不能直接用首包 PTS —— AAC 首包常是负数（编码器
  priming，实测 `-0.0232 s`），容器用 `elst` 把它补到 0；把它当起点会让**每条**
  产物凭空多一帧。
- 负的呈现起点一律夹到 0：负值只有一个来源（`elst` 已补偿掉的 priming），
  播放器看到的就是 0。不夹的话，「本来音画对齐」的源会被算出一个 AAC 帧的假差值。
- 视频侧平移量落在编码器时基（1/fps）上，按帧**取整**后作为常量偏移加到每帧 PTS；
  音频侧按 `av_rescale_q` 换算成各轨自己的时基，同样减统一原点。音频平移后仍为负的
  包只可能来自 priming，原样交给封装器由 `elst` 补偿。
- 结论层新增结构化字段 `av_start_offset_seconds` / `av_start_offset_preserved` /
  `av_sync_note`，报告落「音画起点差」「音画同步处理」两行（TXT/HTML 同源），
  JSON 落机读字段。
- **三种诚实态**分别有确定文案：两条轨都量到 → 写差值 + 「按统一原点平移，相对关系
  保留」；有音频但拿不到可比起点 → 明说「本次未做音画起点比对（不声称音画同步已
  保持）」；单轨视频 → 一声不响（没有音画同步可言）。

**守护**

- `RepairTranscodeFidelity.S08PreservesAudioVideoStartOffset`：造一个视频整体推后
  若干帧的重封装语料（音频不动，等价 `-itsoffset`），断言**产物**的音画起点差
  与源片近似相等，并三格式（TXT/HTML/JSON）同源。
- `RepairTranscodeFidelity.S08DoesNotInventAvOffsetWhenSourceHasNone`：反向守护 ——
  源片本来对齐（shift=0）时，产物不得**造出**一个假差值。这条正是实测中先红的
  那条（首版拿负 priming 当音频观测起点，算出 0.023 s 假差值）。
- `RepairTranscodeFidelity.VideoOnlySourceClaimsNoAvSync`：单轨视频不得出现
  音画同步结论。

**明确不做**

- 不重建 `elst`，也不改「`elst` 明确不重建、由切片 L 显式标注」的既有边界。
  本轮只保证**轨间相对关系**在转码后被保留，不保证编辑列表语义被还原。
- 不做区间裁剪后的重新对轴（`-ss` 场景下「用户选的那一段」的起点语义另有口径，
  归 §5.2 的轨道级转码决策一起考虑）。
- 不声称「音画同步已保持」：本轮保留的是**起点差**，帧级漂移要靠阶段 5 的时间轴
  工作（§5.1），两者不能混为一谈。

---

### 4.17 切片 Q：容器级 side data 的真正继承（旋转矩阵 / HDR 静态元数据）

**起点**：§5.2 一直把「旋转 displaymatrix 与 HDR 静态元数据的实际继承」列为待补。
摸底时发现旧实现的问题比「缺能力」更微妙 —— 它**根本不去看产物**：

```cpp
if (fidelity.has_display_matrix) {
    fidelity.lost_properties.push_back("画面旋转 / 镜像矩阵（…°）未继承：…");
}
```

这句话是从「我们没拷 side data」推出来的。产物侧一次都没打开过。哪天 muxer 自己
继承了、或者拷贝逻辑改了却忘了改文案，同一份报告就会自相矛盾 —— 与 M / N / O / P
是同一类错误：**拿意图当事实**。

而且这两项根本不需要重编码代价：旋转矩阵和 HDR 静态元数据都是**流级 side data**，
mp4 muxer 在写 header 时从输出流的 `codecpar->coded_side_data` 取。挂上去就完事。

**实测（探针，落地前删除）**

```
源片：has_display_matrix=1 deg=-90   mastering=1 cll=1
产物：has_display_matrix=0 deg=0     ← 旧实现确实丢了，像素横着存 → 播放器不转正
```

拷 side data 之后：

```
产物：has_display_matrix=1 deg=-90   mastering=1 cll=1
      attach_m=1 attach_hdr=1 out_has_m=1 out_has_hdr=1 measured=1
```

即 `movenc` 确实会把 `AV_PKT_DATA_DISPLAYMATRIX` 写进 `tkhd`、把
`MASTERING_DISPLAY_METADATA` / `CONTENT_LIGHT_LEVEL` 写进 `mdcv` / `clli` —— 这是本轮
实测出来的事实，不是查文档推断的。

**做法**

- **拷贝**：在 `avformat_write_header` **之前**把源侧（`codecpar` 优先，首帧 side data
  兜底，见 `SourceDisplayMatrix`）的矩阵与两份 HDR 元数据挂到输出视频流的
  `codecpar->coded_side_data`。拷贝动作单独记 `display_matrix_attached` /
  `hdr_metadata_attached`。
- **实测**：`av_write_trailer` + 关输入之后，重新打开产物文件，逐项读回它**自己**声明
  的方向矩阵与 HDR 元数据，落 `output_has_display_matrix` / `output_rotation_degrees` /
  `output_has_hdr_metadata` / `side_data_output_measured`。
  ❗ 只在写成功时测：写失败 / 截断的产物可能连 `moov` 都不完整，打开失败本身不该被读成
  「产物缺少方向信息」。
- **结论**（`FinalizeSideDataInheritance`，唯一入口）：
  | 源侧 | 产物侧实测 | 结论 |
  |---|---|---|
  | 有 | 有且角度一致（≤0.5°） | preserved「已在产物中实测到相同角度」 |
  | 有 | 有但角度不同 | lost「角度已改变」 |
  | 有 | 无 | lost「产物实测无方向信息」 |
  | 有 | **未实测** | unverified（第三类） |
  | 无 | — | 不提（印「源未声明」），不进任何一张表 |
- **HDR 有位深前提**：只有 `源位深 == 产物位深` 才拷（`TranscodeHdrInheritable`）。
  10 bit 源压成 8 bit 产物后照搬 HDR 标记，会让播放器把一个 SDR 画面按 HDR 渲染 ——
  与「色彩标记只在位深不变时继承」是同一条理由，只是这份元数据挂在容器侧，更容易漏。
  位深降级时 `hdr_inherit_blocked_by_bit_depth=true`，结论里写明「主动不沿用」，
  与「muxer 丢了」分开表述。
  ❗ 还加了**矛盾检测**：若 `blocked=true` 而产物实测仍有 HDR，报
  「位深降级却仍在产物中（异常）」—— 这比「拷贝逻辑写对了」更强：它连未来改坏
  拷贝条件也能自曝。
- **第三类清单** `unverified_properties`：这是本轮引入的新机制。「没查」和「查了没有」
  必须分开 —— 旧模型只有 lost / preserved 两张表，任何「不知道」都会被挤进其中一张。
  `all_properties_preserved` 的计算也随之分两档：`InspectTranscodeFidelity` 与
  `FinalizeTranscodeFidelity` 用**保守式**（源侧还挂着待验证 side data 就恒 false），
  只有 `FinalizeSideDataInheritance` 拿到实测后才能抬回 true。
- 报告：「旋转 / 镜像矩阵」「HDR 静态元数据」两行的文案由修复层一次渲染
  （`display_matrix_note` / `hdr_metadata_note`，源侧事实与产物实测同源），报告层只决定
  印不印；「源侧属性结论」行在有未实测项时追加「，N 项未实测」；items 追加
  「转码属性未实测: …」；JSON 落全部机读字段。

**守护**

- `S08InheritsRotationMatrixAndVerifiesInOutput`：端到端。源 90° → 断言**产物文件本身**
  带相同角度、`side_data_output_measured=true`、报告三格式同源，且不许出现「未实测」。
- `S08InheritsHdrStaticMetadata`：同上，针对 mastering + cll。
- `SourceWithoutSideDataClaimsNothing`：反向守护 —— 源片没有方向 / HDR 时不许印任何
  结论，也不许因为「没查到」把「属性均已保留」拉成 false。
- `RotationMatrixWithoutOutputEvidenceStaysUnverified`：只有源侧事实时不许下结论；
  未实测进 unverified，不进 lost。
- `MeasuredOutputWithoutMatrixReportsLoss` / `RotationAngleChangeIsNotSilentlyTreatedAsPreserved`：
  实测无矩阵、以及「有矩阵但角度变了」都必须报损失，后者尤其容易漏（只要有矩阵就算通过）。
- `HdrMetadataFollowsSameThreeStateRule`：HDR 同一套三态规则。
- `HdrInheritanceRequiresSameBitDepth`：位深前提四态（8→8 / 10→10 可继承，10→8 / 0→8
  不可），以及「主动不沿用」与「muxer 丢了」文案不许混用、矛盾检测必须报异常。

**明确不做**

- 不碰 **HDR 动态元数据**（逐帧 SEI 里的 ST.2086 / 杜比视界 RPU）：那是码流内载荷，
  重编码后必须由编码器重新生成，不是拷 side data 能解决的。
- 不碰 10-bit：`libopenh264` 不支持（`pix_fmts` 只有 yuv420p / yuvj420p），
  等价于换 FFmpeg 变体（许可证问题，见 §4.15）。
- 不判断「这份 HDR 元数据与重编码后的画面是否仍然相符」：本轮只保证**声明被搬运**且
  **位深前提成立**，不保证色彩体系自洽。
- **10 → 8 这条路径没有端到端语料**：本构建里唯一的 10-bit 编码器是 `ffv1`
  （yuv420p10le 可用），但要造「10-bit + HDR + 能被 S08 接受」的 mp4 样本，
  得先给 `TestMediaFactory` 加 10-bit 源支持。本轮用「纯函数四态 + 矛盾检测」兜住，
  如实记为**已知未覆盖**。

---

### 4.18 切片 R：能力边界的「支持程度」口径收敛

**问题**：三条能力契约一律 `status = "unsupported"`，而它们自己的 `evidence` 里分别写着
「当前版本已实现受限版无参考重建」「当前版本已能借同源初始化段…」「S08 已实现并可运行」。
同一份报告自相矛盾。更糟的是章节标题就叫「未实现能力边界（本次命中）」——
读者通常只看标题与状态词，于是把「这次已经靠借参数集修好了」读成「这次没做到」。

**根因**：`status` 想表达「本版本实现程度」，读者读成「本次是否生效」；而「做到哪 /
没做哪 / 做不了哪」全压在 `evidence` 一段散文里，状态词只能靠散文自辩——原文就有一句
「本契约的 unsupported 指的是尚未覆盖的范围」。用一句话给一个词打补丁，正是
范围没结构化的信号（与切片 Q 的 `unverified_properties` 同一个道理：缺失的维度
会被挤进已有的桶里）。

**做法**

- 状态词表（唯一事实源 `CapabilityStatusVocabulary()`）：`partially_supported` /
  `unsupported` / `deferred`。词表外的取值一律渲染成「未知状态（x）：不能据此判断支持程度」，
  绝不静默落成支持或不支持。
- 三段范围拆开，互不顶替：
  - `covered_scope`：本版本已实现的部分；
  - `not_covered_scope`：本版本没做、但物理上做得到；
  - `unrecoverable_scope`：任何实现都拿不回来（原有字段，语义收窄为纯物理边界）。
- `status` 与两张表互为约束：文案声称已实现 ⇒ 不许 `unsupported`；`partially_supported`
  ⇒ `covered_scope` 非空；同一句文案不许同时出现在两张表。
- 渲染一次（`CapabilityStatusLabel` / `CapabilityStatusHeadline`），结果落进
  `status_label` / `status_headline` 两个字段，报告层只打印——与 `encoder_note` /
  `av_sync_note` 同一套口径（`core/reporting` 不链 `core/repair`，也不许自己拼第二份文案）。
- 章节标题 `未实现能力边界（本次命中）` → `恢复能力边界（本次命中）`，并加一行「状态口径」：
  状态词描述本版本对这项能力的实现程度，不代表本次运行是否生效。
- `RepairInspector` 的诊断 summary 同步改口（原为「命中 N 条未实现能力边界」）。

三条契约的实际取值：全部 `partially_supported`（各有受限实现），`covered_scope` 2–3 条、
`not_covered_scope` 2–3 条。报告行形如：

```
moov_rebuild [部分支持] 触发证据: container.moov_missing
  支持程度: 部分支持：本版本已实现 2 项（见「已覆盖范围」），另有 3 项未覆盖（见「未覆盖范围」）；
            本条描述能力整体，不代表本次运行已生效
  已覆盖范围: 无参考受限重建：ftyp + 恰好一个 mdat + 单轨，且载荷为纯 AVCC H.264 或纯 ADTS AAC…
  未覆盖范围: 音视频交织的 mdat、多个 mdat、多轨并行
```

**守护**

- `CapabilityBoundariesAreFullyDefined`：五维度 + 状态词合法 + 两处渲染文本非空。
- `CapabilityStatusWordMatchesImplementedScope`（新）：状态词 ↔ `evidence` 一致性
  （文案声称已实现就不许 `unsupported`）、`partially_supported` 必有已覆盖条目、
  三段范围互不重叠、总述必须带「不代表本次运行已生效」、`evidence` 不许再出现
  「本契约的 unsupported」这类自辩。
- `UnknownCapabilityStatusIsNeverReadAsSupported`（新）：词表外取值、已覆盖为空的
  「部分支持」、内容矛盾的「未支持」，三种都要渲染成「不能据此判断」。
- `CapabilityBoundariesAreVisibleInAllReportFormats`：新增 4 个 JSON 字段与 3 个报告行；
  标题不许再出现「未实现能力边界」，状态词必须渲染成 `[部分支持]` 而不是英文码。

**明确不做**

- 不做「本次是否生效」的逐次标注：那需要把执行层的实际动作回写到契约，而
  `RepairResult` 里没有「这条路本次被用过」的事实。本轮只在文案上把口径写清，
  不让状态词冒充运行结果。**已知未覆盖**：报告能说「这条能力部分支持」，
  但不能说「本次用的就是它」。
- 不动 `deferred`（当前无实例）：留在词表里表示「已知能力、本版本不排期」，
  以免将来临时加词绕过守护。

---

### 4.19 切片 S：合成初始化段的时基证据分级

**起点**：§4.14（切片 N）留下的「明确不做」——「合成初始化段（`synthesized`）路径的
时基证据分级没改：它的 timescale 来自源侧 sidx / 分片声明，仍走 `container_pts`」。
摸下去发现这不只是「没分级」，而是**同一份报告里自相矛盾**：

| 报告位置 | 覆盖这条路径时写的是 | 事实 |
|---|---|---|
| 时基依据 | 「容器时间戳」 | 源片**没有 moov**，那个容器是自建出来的 |
| 时间轴变化 | `unchanged` + 「时间戳仅按输出容器时间基缩放，未补齐 / 未合成」 | 秒数依赖一条**我们自己选定**的 timescale |
| 时基证据（备注） | 「sidx timescale=12800」 | 这才是真实依据 |

**根因**：`RepairExecutor` 只在**借用**（`!synthesized`）分支里占住 `timeline_basis.mode`，
自建分支不占位，于是重封装阶段按 `mode == "unknown"` 补成 `container_pts`。
与 M / N 是同一类错误，只是路径又不同：M 改 C/H/I，N 改 fMP4 借参考段，本轮补最后一条。

**三处口径问题**

1. **「容器时间戳」不是源侧事实**：自建初始化段时，mdhd timescale 是我们按源侧 sidx
   声明写进去的。源片本来就没有 moov，报告把「我们选定的时基」说成源容器给的。
2. **`unchanged` 过度声称**：逐样本 tick 确实原样来自源侧 moof/trun，但 tick→秒的换算
   依赖上面那条声明；源片原始 mdhd 已丢失，没有可比的基线。
3. **时长比对是自证**：报告里「时间轴实测: 源侧估算=1.600s / 产物实测=1.600s / 可比对=yes」
   在**这条路径上**两侧都用同一个 sidx timescale 换算，相等只能说明换算没算错。

**时基证据分级**（本轮的新东西）

自建路径唯一能做的核对是：拿源侧 moof/trun 的逐样本时长累加反算总时长，与 sidx
声明的时长比。这是**两组源侧声明互相印证**，不是与原始 mdhd 核对。

- 记「核对到底跑过没有」：`timescale_crosschecked` 只在真的比对过且落在容差内才为 true。
  没跑过 ≠ 跑过并通过 —— 源侧没给出可累加的样本时长时，报告必须写「未做事后核对」。
- 记实测值：`timescale_crosscheck_error_seconds`（逐轨偏差的最大值）、
  `timescale_crosscheck_tolerance_seconds`（逐轨容差的最小值，多轨取最严）。
- 渲染一次（`TimescaleCrosscheckText()`，唯一入口）：诊断、报告、JSON 读同一份文案。
  实际产物：`sidx timescale=12800（与源侧 moof/trun 逐样本时长累加核对：偏差 ≤ 0.000 s，容差 0.250 s）`
- 这条核对**发现不了整体被同比例改写的时基**：sidx 的 timescale 与它自己声明的时长
  同时被改写时，两侧一起缩放，比值不变。文案与文档都要说清这一点。

**做法**

- `RepairExecutor`：自建分支占住 `mode = "synthesized_init_timescale"`，
  与 `reference_init_borrowed`（借别人的）并列；detail 里带上分级后的核对文案；
  限制项按「核对过 / 没核对」分两支。
- `RepairEngine`：`timeline_unknown` 纳入 `synthesized_init`（与 `reference_assist_used`
  同档），`timeline_status` 从 `unchanged` 改为 `unknown`，note 写明「tick 原样来自
  moof/trun，但 tick→秒依赖自建时基」；`timeline_comparable` 时另加一条限制，
  说明这次一致是自证。
- `RepairReportExporter`：新 mode 渲染成「自建初始化段时基（源侧 sidx 声明，无原始
  mdhd 可核对）」，并额外出「时基证据边界」行 + 一条「时基结论」——
  与前两条借用路径同一套三段式（mode / 边界 / 结论）。

实际渲染（TXT）：

```
时基依据: 自建初始化段时基（源侧 sidx 声明，无原始 mdhd 可核对）
时基证据边界: 源侧 moov 已丢失；mdhd timescale 由自建初始化段按源侧 sidx 声明选定，不是源容器直接给出
- 时基结论: 逐样本 tick 原样来自源侧 moof/trun，但 tick → 秒的换算依赖自建时基，
  未与源片原始 mdhd 核对（已丢失），秒数不能作为源片时间轴的独立证据
- 时基细节: …时基核对: sidx timescale=12800（…偏差 ≤ 0.000 s，容差 0.250 s）
- 时基限制: 时基核对的是「trun 时长累加 ≈ sidx 声明时长」，两组声明同源于打包器；
  整体被同比例改写的时基无法由此发现，容差内的偏差也排除不了…
时间轴变化: unknown — 初始化段由 in-band 参数集自建，mdhd timescale 采用源侧 sidx 声明；…
```

**守护**

- `FragmentedSegmentsSynthesizeInitFromInBandParameterSets`（扩写）：钉 `mode`、
  `timeline_status=unknown`、三格式同一结论；反向守护「不许出现 `时基依据: 容器时间戳`」；
  这段语料确实给出了 trun 时长，所以还必须**不许**出现「未做事后核对」；
  证据边界行要整行匹配（只钉行首「源侧 moov 已丢失」时，把后半句改成「由源侧容器
  直接给出」也能蒙过去 —— 变异 S4 首轮就是这么判绿的）。
- `TimescaleCrosscheckGradeNeverClaimsAnUnrunCheck`（新，纯函数）：没核对时不许出现
  偏差数字；核对过时偏差与容差必须并排出现（只写「核对一致」，读者没法判断有多严）。

**明确不做**

- 不做「用源侧 NAL / 帧内容反推播放速度」：那需要逐帧解码计时，代价与语义都超出
  本轮（且源片被删的那部分本来就没有计时信息）。
- 不给 S08 输入侧覆盖区间（同 §4.14）。
- 不把这条路径的 `timeline_status` 抬回 `unchanged`，除非将来真的拿到与原始 mdhd
  等价的独立基线（例如切片自带 tfdt 且与 sidx 交叉一致）。

### 4.20 切片 T：S08 的轨级决策（copy / transcode / drop）

**起点**：§5.2 阶段 6 的第一条——「S08 写死『视频轨统一按 H.264 重编码 + 逐条音频轨
按原码流复制』，无『按轨判断是否需转码』」。摸下去发现这不是「还没做选择」，而是
**三个可观测的结论层缺口**（前两个由代码路径实测，第三个由容器行为实测）：

| 现象 | 旧行为 | 实测证据 |
|---|---|---|
| 第二视频轨 / 封面 / 字幕 / 数据轨 | 无声消失，`omitted_items` 里一条都没有 | `audio_map` 之外的流从未被登记；只有音频「复制参数失败」才记账 |
| 目标容器拒绝某条音频轨 | `avformat_write_header` 整体失败 → **整个转码失败**，退回避重封装 | 内存试写头实测：mp4 拒 truehd、mov 拒 flac / opus / truehd |
| 「这条轨为什么重编码」 | 只有一句「视频轨经过解码与重编码」 | 读起来像「该轨坏了」；实际多数情况是用户选了救内容模式 |

**判据取证（先量后写）**

- `avformat_query_codec()` **不可用作判据**：实测对 `mpegts` 这类没实现 `query_codec`
  的封装器返回垃圾值（-1163346256），按它判会误删轨；对 mp4 又会声称 vorbis /
  truehd 都支持。
- 改用**内存试写头**：建一个只含该音频轨的输出上下文，`avio_alloc_context` 挂内存
  写回调（带伪 seek，因为封装器写头时会回填 box 长度），调 `avformat_write_header`。
  结论与真实落盘写头逐条一致（mp4/truehd、mov/flac、mov/opus、mov/truehd 均拒绝；
  mp4/aac、mp4/flac 接受）。
- 实现时踩到一个堆损坏：`avio_context_free(&avio)` 会把 `avio` 置 **NULL**，
  用「`avio == nullptr`」判「缓冲没被接管」→ 对同一块 buffer double free
  （`0xc0000374`）。所有权必须在 `avio_alloc_context` 成功后立刻转移，不能事后
  从指针猜。

**改动**

1. 新模块 `core/repair/RepairTranscodeTracks.{h,cpp}`：纯函数
   `PlanTranscodeTracks()` 把每条输入轨判成 copy / transcode / drop + 稳定理由码；
   `ProbeTrackFitsContainer()` 做内存试写头；`TranscodeTrackDecisionText()` /
   `TranscodeTrackOmittedText()` 是唯一渲染入口。
2. `RepairTolerantTranscode`：先建探针表 → 决策 → **按决策**建输出流；被丢弃的轨
   进 `omitted_items`；首帧前的音频包只缓存「决策=复制」的轨（否则被丢的轨会把
   pending 预算吃光、误报「音频前缀可能缺失」）；主视频轨选择不再用
   `av_find_best_stream`，改成跳过封面图（`attached_pic`）的第一条视频轨。
3. 模型加 `RepairTranscodeTrackDecision` 与四个字段（决策表 / 输入流总数 /
   汇总 / 依据边界），报告层只打印 `text`，不自己拼。
4. 报告：「轨道决策」汇总行 + 「轨道决策覆盖」行（条数 ≠ 源侧流数时显式告警）
   + 逐轨决策条目；JSON 落 `track_decisions[]` / `track_decision_count` /
   `track_decision_input_streams` —— 只有数组时，「少了一条」在 JSON 里看不出来。
5. 声明排除新增 `extra_tracks_not_rebuilt`；`gop_local_reencode_not_attempted`
   的证词里「逐轨转码决策仍未实现」已过期，同步改掉（否则报告自相矛盾）。

实际渲染（TXT）：

```
轨道决策: 复制 1 轨 / 重编码 1 轨 / 未保留 1 轨
- 轨道决策: 流 #0（video, h264）→ 重编码：主视频轨：该轨未检出编码层损伤证据；
  本次重编码来自救内容模式的选择，不是该轨必需的修复（可感知画面会有代际损失）
- 轨道决策: 流 #2（audio, aac）→ 复制：音频轨：目标容器试写头接受该编码，按原码流复制（不重编码）
- 未保留: 流 #1（video, h264）：第二视频轨：S08 只有一条解码-重编码链，产物未保留该轨
  （这是本模块的能力边界，不是容器限制）
```

**判据边界（随结论一起报出）**

- 试写头只放该音频轨一条流：结论覆盖「容器 × 该编码」，不代表整份产物一定写得出来。
  真实写头失败时，错误文案改成「逐轨实测有没有被拒轨」，不再猜「音频编码参数
  可能不被接受」。
- 决策表里 `drop` 的轨**不**在决策行里重复打印（未保留清单已经列过），避免同一条
  理由出现两次。
- 源侧 `discard=all` 与容器拒绝是两件事，理由码分开（`stream_marked_discard` /
  `audio_drop_container_rejected`）。

**守护**

- 纯函数用例 11 个：覆盖自证（每条输入轨恰好一条、下标唯一且与探针集合一致）、
  封面图不当主轨、音频三态（接受 / 拒绝 / 未测）各自的动作与理由码、
  主视频轨「有损伤证据 / 只有模式选择」两种理由不许混用、非音视频轨逐类理由码、
  源侧已丢弃单独理由码、渲染形态。
- 容器实测用例 1 个：mp4+aac 接受、mp4+flac 接受、mov+flac 拒绝（且带回实测原因）、
  不存在的封装器 → `Unknown`（不许冒充接受或拒绝）。
- 端到端 2 个：双视频轨语料（工厂新增 `extra_video_tracks`）——决策表覆盖 3 条流、
  第二视频轨为 drop、产物只有 1 条视频轨、未保留清单里有它、三格式同源；
  单视频轨常规路径——决策表 2 条、音频由试写头实测判为接受、汇总串逐字匹配。
- 判别力 9 组（T1–T9）全部精确判红后按字节恢复复跑绿：
  容器拒绝改回「照旧复制」、封面图当主轨、覆盖自证不看下标、试写头恒判接受、
  实测原因不进结论、丢轨不进未保留清单、决策表被 `InspectTranscodeFidelity`
  整份替换后没补回（真实踩过的坑）、无损伤证据也写成「有损伤」、
  报告把 drop 重复打印。

**已知未覆盖**

- 「容器拒绝音频轨 → 产物仍然生成且该轨被剔除」这条**端到端**路径没有语料：
  本构建能造出来的真实源（mp4/mkv + aac / flac / opus）在目标容器里都被接受。
  目前只在纯函数与容器实测两层覆盖；真实拒绝场景需要外部样本（如 mov+flac）。
- 音频「转码而不是丢弃」还没做：本轮只做 copy / drop，没有音频重编码链。
- `track_action` 的 gate 只在「决策=copy」时建音频流；若将来加音频重编码，
  这里要一起改。

### 4.21 切片 U：视频轨时间轴空洞的黑帧填补

**起点**：§5.2 阶段 6 的「黑帧 / 静音 / 重复帧：目前完全没有，属空能力，接入时由用户
显式选择并单独记账，且不计入恢复率分子；`synthetic_frames_inserted` 恒 false 的现状要
同步改掉」。摸下去先确认这不是「少个开关」，而是**S08 会把源时间轴空洞静默压掉**：

| 现象 | 旧行为 | 实测证据 |
|---|---|---|
| 源视频轨中间缺一段 | 产物变短，音频按原时长走 → 越向后音画差越大，报告一字不提 | 40 包（其中空 12 帧）的语料：旧路径产物只写 39 帧 = 1.56 s，源视频轨声明 2.04 s |
| 有空洞的源用谁当 CFR 帧率 | `avg_frame_rate` = 帧数 ÷ **总时长**，空洞会把分母撑大 → 帧率偏小 | 25 fps、40 帧、空 12 帧的语料实测 `avg=1000/51(19.6)` / `r=25/1`；按 19.6 重编码等于每帧多算 12 ms，整条视频被拉慢 30% |
| 合成内容怎么记账 | `synthetic_frames_inserted` 是常量 `false`，字段在但能力不在 | `RepairEngine` 直接写死两个字面量 |

**判据取证（先量后写）**

- `avg_frame_rate` **不能**直接当 CFR 帧率：它是「帧数 ÷ 总时长」，源中间缺一段时
  显著偏小；`r_frame_rate`（时间戳的最小公倍帧率 = 名义帧率）在有空洞时仍是 25。
  规则改成「名义帧率高出平均值 10% 以上时用名义帧率」—— 那正是「有空档 / 丢帧」的
  签名，两者接近时仍沿用 `avg`（VFR 源的平均值更有代表性）。
- 「缺几帧」必须用**编码器时基**换算（`av_q2d(encoder->time_base)`），不能用秒数 /
  名义帧率另算一遍：两者一旦不一致，插进去的黑帧长度就和空洞对不上，产物反而更长。
- 语料工厂的最后一个包解不出帧是既有行为（clean 语料同样 20 包 / 19 帧），与空洞
  无关；端到端断言的基线因此一律取**实测解码帧数**，不写帧数常量。

**改动**

1. 新模块 `core/repair/RepairBlackFrameFill.{h,cpp}`：纯函数
   `PlanBlackFill()` 判定「有没有洞 / 该不该填 / 填几帧」，`BlackFillReasonCode()` /
   `BlackFillGapText()` / `BlackFillSummary()` / `BlackFillBasisNote()` 是唯一渲染入口。
   不碰 FFmpeg，可脱离解码链单测。
2. `RepairRequest::fill_video_gaps_with_black_frames`（默认 false）+ 界面勾选框
   「空档补黑帧」（只在「救内容」模式下可用，切模式时同步禁用，避免勾了不生效）。
3. `RepairTolerantTranscode`：解码循环里按相邻源 PTS 探测空洞 → 判定 → 真插黑帧
   （`av_image_fill_black` 一次性填好缓冲，逐帧 `av_frame_make_writable` 后送编码器，
   PTS 与真实帧共用同一个帧序号，因此空洞是真的被占住了、后续帧自然顺延）。
   合成帧按 PTS 精确归因（帧序号唯一且单调），**不进样本账本**。
4. 模型加 `RepairSyntheticGap` / `RepairSyntheticFill`；`fidelity.synthetic` 是事实源，
   `synthetic_frames_inserted` / `synthetic_frame_count` 只是它的投影。
5. 恢复比例扣合成时长：`covered -= seconds_inserted`，否则「填 0.48 s 黑场」会被算成
   「多恢复 0.48 s」。同时补上 S08 一直没写的 `source_duration_seconds` —— 空着不是
   「没问题」，是结论缺失（旧路径永远只报「缺少源侧时长基线」）。
6. 报告：「合成填补」汇总行 + 逐处空洞条目 + 依据边界；JSON 落
   `fidelity.synthetic.{inspected,requested,gaps[],frames_inserted,seconds_inserted,…}`。

实际渲染（TXT）：

```
合成填补: 探测到 1 处空洞；填补 12 帧（0.480 s 合成黑帧）
- 合成填补: 空洞 @0.800 s（第 21 个解码帧之后）：间隔 0.520 s ≈ 缺 12 帧 → 已插入 12 帧合成黑帧
- 合成填补依据: 判据：相邻源解码帧 PTS 间隔 vs 标称帧率 25/1 fps（源侧声明）；容差 1.5 帧，…
- 限制 / 未知项: 产物含 12 帧合成黑帧（0.480 s）：它们补回的是源时间轴上的空洞，
  不是从源解出来的画面；这段时长已从恢复比例分子中扣除
```

**判据边界（随结论一起报出）**

- **探测与插帧是两件事**：用户没启用也要报「源里有 N 处空洞」，否则「没插黑帧」会被
  读成「没有空洞」。
- 间隔不是帧时长**整数倍**时拒绝填补（更可能是时间戳本身坏了，按帧数外推等于用编出来
  的画面掩盖未诊断的缺陷）；单处超 5 s 拒绝（大段缺失该由未恢复区间如实呈现）；
  累计超 750 帧拒绝（`black_fill_budget_exhausted`）。拒绝就是一点都不填，不许「只填前 5 秒」。
- `gap_policy = MergeContiguous` 时一律不填（用户明确要求塌缩空洞，插帧会违背它）。
- 帧率不是源侧证据（`SourceFrameRate` 兜底 25 fps）时不换算、不填补。
- 合成帧已写入产物但**不计入**样本账本与写入包数（账本只记「与源对应的已恢复样本」），
  差值直接在 `note` 里写明，不让读者自己猜。
- 只有「相邻解码帧之间的整帧空洞」这一种形态被覆盖：轨道起点之前的空白、整段轨道缺失、
  音轨空洞都不在判据里。

**守护**（`tests/unit/test_repair_black_frame_fill.cpp`，18 个用例）

- 纯函数 12 个：容差内不算洞（1.0 / 1.2 / 1.4 / 1.49 帧）、整数倍洞填满、未启用只记账
  （缺失帧数照算）、非整数倍拒绝、帧率非源侧证据拒绝、`MergeContiguous` 优先于填补请求、
  大洞拒绝且「一帧都不填」、总预算耗尽、时间戳回退不算洞、**理由码唯一且不退化**、
  **未插帧的文案不许出现「已插入」**、**未探测不许渲染成「0 处空洞」**、
  汇总计数、缺源时间戳时标注探测不完整。
- 端到端 5 个：空洞在未启用时也被探到并进限制项；启用后产物多 12 帧、时长恢复、
  **恢复比例与未填补时基本相等且 < 1.0**、样本账本总数不变；三格式同源（JSON 可机读
  逐处空洞）；无空洞源不许凭空写出条目。

**已知未覆盖**

- 静音填补与重复帧填补仍未实现：音轨能复制 / 重编码 / 剔除，但没有「在时间轴空洞里
  补静音」这一步（切片 V 只补上了解码-重编码这条链，不生成不存在的声音）。
- 填补只覆盖视频轨相邻帧之间的空洞；「轨道起点之前 / 整段轨道缺失」的空白不补。
- 端到端的空洞是语料工厂用 PTS 位移造出来的（`video_gap_after_frame` /
  `video_gap_frames`），与真实丢包在容器层的表现一致，但没有拿真实损坏文件验证过。

### 4.22 切片 V：音频轨「转码成目标容器能收的编码」

**问题（切片 T 留下的缺口）**：容器拒绝原编码时只能 drop。drop 本身诚实，但它把
「这条音轨还在、只是编码不合适」变成「这条音轨没有了」——用户拿到一份没有声音的
产物。实测可复现的拒绝组合：`mov + flac`、`mov + opus`、`mov + truehd`（truehd 在
mp4 里报 Experimental feature）。这类组合在真实素材里不罕见：mov / mp4 容器里装着
从别处拷来的音轨，或用户选了 `.mov` 输出。

**做法**：补第三条路 —— 解码 → 重采样 → 重编码。

- `core/repair/RepairAudioTranscode.{h,cpp}`：
  - `AudioReencodeCandidates()`：候选偏好顺序 `aac → libopus → ac3 → libmp3lame → flac`，
    **按本构建真实存在的编码器过滤**（可用性通过可注入的函数判断，测试可以模拟缺失）。
  - `ChooseAudioEncodeSettings()`：采样率 / 声道 / 样本格式全部按编码器**自己声明的**
    能力选；源采样率在支持列表里就原样保留（不重采样 = 不动音高），不支持时取比例上
    最近的一个，并把「44100 → 48000」这类改动写进说明。
  - `SelectAudioReencodeTarget()`：逐候选做**两次实测** —— 编码器能按该参数开起来
    （拿到真实 extradata）+ 用这份编码参数试写目标容器的头。两条都过才选它。
    全不过时把每个候选的死因逐条记下（`audio_drop_no_fitting_encoder` 的依据）。
- 决策层（`RepairTranscodeTracks`）：容器拒绝的音频轨三态化 ——
  有目标 → `transcode`（理由码 `audio_reencode_container_rejected`，`target_codec` 机读字段）；
  用户关闭 → `drop`（`audio_drop_reencode_disabled`）；没目标 → `drop`
  （`audio_drop_no_fitting_encoder` + 逐候选尝试）。三种原因必须可区分：**「我们取舍了」
  和「我们做不到」对用户意味着完全不同的下一步**。
- 执行层：`AudioTranscodeChain`（解码器 → swr → `AVAudioFifo` → 编码器 → 写包），
  按源 PTS 逐帧定位（不按序号累加，源侧空洞照旧保留、只计数）、末帧不足一个编码块时
  用静音补齐并单独计数、四层缓冲（解码器 / swr / fifo / 编码器）全部排空。
- 请求选项 `reencode_audio_to_fit_container`（**默认开**，界面「音轨不符容器时重编码」）：
  不转码的替代结果是丢掉整条音轨，比有损重编码更糟；关掉时按「容器拒绝」剔除并写明
  是用户关的。
- 报告：JSON 的 `track_decisions[]` 增加 `target_codec`；`audio_reencoded` 不再恒 false；
  转码说明逐轨写清「原编码 → 目标编码 / 编解码帧数 / 时间戳跳变 / 末帧补齐采样数」。

**测试**：14 个用例（8 纯函数 + 6 端到端），另更新 2 个切片 T 的用例（决策语义变了）。

**已知未覆盖**

- 端到端只能用**测试注入**把某条音频轨的容器接受性强制判成拒绝：本构建造不出
  「源容器能装、目标容器不收」的真实语料 —— mov 写不进 flac / opus，这种组合因此
  永远不可能成为源。注入只影响接受性这一个判定，转码目标仍是真实实测的。
- 重编码是有损的，且可能改变采样率 / 声道；报告只写「变成了什么」，不评估听感损失。
- 静音填补仍未实现：这条链能重新编码已有声音，但不会生成源里不存在的声音
  （`timeline_gaps > 0` 时按源位置保留空洞，明确不补）。
  → **切片 W 已补上**（§4.23）：空洞仍在原地保留判定语义，但在用户显式启用时用静音
  占住它；本条保留为切片 V 当时的边界记录。

### 4.23 切片 W：音频轨时间轴空洞的合成静音填补

**起点**：§4.21 与 §4.22 两条「已知未覆盖」都写着同一句话 —— 音轨能复制 / 重编码 /
剔除，但没有「在时间轴空洞里补静音」这一步。摸下去先确认这不是「少个开关」：

| 现象 | 旧行为 | 实测证据 |
|---|---|---|
| 源音频轨中间缺一段 | 音频链按源 PTS **逐帧定位**，缺的位置直接跳过去（`next_pts_ = want`），产物里留一段没有任何采样覆盖的时间 | 语料 40 帧 / 25 fps / 44100 Hz、`audio_gap_after_frame=20` 空 12 帧：转码轨实测 `expected=33792 actual=46080` → `missing=12288` 采样（0.279 s） |
| 这段空洞出现在报告里吗 | 只落 `timeline_gaps` 计数，不说「缺多少、缺在哪、为什么不补」 | 报告一行不加；玩家跳过 / 补静音 / 卡住全凭实现，报告不提 |
| 码流复制轨上探测空洞 | 完全不看。**容器还把空洞并进了前一个样本的时长** —— 只按 PTS 跳变判据一个都探不到 | 实测 n=33 包 `start=0.743039 dur=0.301859`（≈13 个编码块，实际只携带 1 块），其余包 `dur=0.023220` |
| 用户为什么没看到填补 | 想补也没开关；`fill_*` 只有视频黑帧那一个 | `RepairRequest` 无音频项 |

**判据取证（先量后写）**

- 判据 = 「上一帧结束时的采样位置 vs 当前帧实际开始位置」，两边都换算到**编码器采样率**
  （重采样之后的时基）。用容器时基算会错，因为转码轨的输出时基与源不同。
- 容差 **半个编码块**（AAC 1024 → 512 采样）。原因：容器时基取整不足以构成空洞 ——
  TS 的 1/90000 落在 44100 上只抖 ±1 采样。按抖动补静音等于往产物里灌噪声。
  编码器不声明块长（flac）时，按 20 ms 兜底容差，并把这条写进 `basis`。
- 探测与填补是**两件事**：未启用时也要报「源里有 N 处空洞」。否则「没补静音」会被读成
  「没有空洞」—— 这正是切片 U 定下的口径，音频侧照抄。
- 码流复制轨的空洞分两种形态取证（都实测到了）：① 容器把空洞并进前一个样本的时长
  （`duration > nominal * 1.5` → 缺 `duration - nominal`）；② PTS 直接跳过去
  （`start - 上一包结束`）。只认 ① 会漏掉 ②，只认 ② 会漏掉这份真实语料。

**改动**

1. 新模块 `core/repair/RepairSilenceFill.{h,cpp}`：纯函数 `PlanSilenceFill()` 判定
   「有没有洞 / 该不该填 / 填多少采样」，`SilenceFillReasonCode()` /
   `SilenceFillGapText()` / `SilenceFillSummary()` / `SilenceFillBasisNote()` 是唯一
   渲染入口。不碰 FFmpeg，可脱离解码链单测。
   `SilenceFillVerdict` 九态：`NoGap / Fill / NotRequested / MergeContiguous /
   CopyTrackCannotSynthesize / GapTooLarge / BudgetExhausted / NoSampleRate /
   EncoderRejected`。
2. `RepairRequest::fill_audio_gaps_with_silence`（默认 false）+ 界面勾选框
   「空档补静音」（`AudioSilenceFillToggle`，只在「救内容」模式下可用，切模式与
   idle 状态同步禁用）。
3. `RepairTolerantTranscode` 的 `AudioTranscodeChain`：
   - `ConsumeFrame` 里 `want > next_pts_` 时调 `PlanSilenceFill(next_pts_, want, …)`；
     `Fill` → `PushSilence()` 且 **`next_pts_` 不跳**（空洞被静音占住了，后续帧自然顺延）；
     其余 → `++timeline_gaps; next_pts_ = want;` 并落一条 gap 记录。
   - `PushSilence()` 分块写静音（`av_samples_set_silence`），**编码包按块队列归因**：
     `current_packet_synthetic_` 为 0 = 纯真实、>0 = 含合成采样、**−1 = 队列错位时保守
     判含合成**。判定为含合成的包整块**不进样本账本**。
   - 复制轨探测在改 PTS **之前**做（改完就再也看不出原始间隔）。
   - 收尾按输入流下标逐轨落账，用关闭前固化的 `input_stream_count` —— **不能**再用
     `in->nb_streams`（输入上下文已关闭，读的是释放后的内存，实测紧接其后的字符串拷贝
     就 0xc0000005）。
4. 模型加 `RepairSyntheticAudioGap` / `RepairSyntheticAudioFill`；
   `fidelity.synthetic_audio` 是事实源，`samples_inserted` / `seconds_inserted` 只是投影。
5. 报告：TXT / HTML 新增「音频合成填补」汇总行 + 逐处空洞条目 + 「音频合成填补依据」；
   JSON 落 `fidelity.synthetic_audio[]`（逐处含 `reason_code` / `missing_samples` /
   `inserted_samples` / `source_position_seconds`）。限制项写明「产物含 N 采样合成静音
   （x.xxx s）…按区间时长估算的恢复比例无法反映这段缺失」，**未填补时也要写「源音频轨
   探测到 N 处空洞，本次未补静音」**。
6. `RepairDeclaredExclusions`：`synthetic_content_not_inserted` 的触发条件加
   `!any_audio_silence`（补过静音还报「未插入合成内容」就是假话），并补事实
   `fidelity.synthetic_audio[].samples_inserted=0`；能力契约的 `not_covered_scope` /
   `timeline_impact` 同步改写。

实际渲染（TXT）：

```
音频合成填补: 音频流 #1：探测到 1 处空洞；补入静音 12288 采样（0.279 s 合成静音）
- 音频合成填补: 空洞 @0.766 s（第 34 个音频帧之后）：间隔 0.279 s ≈ 缺 12288 采样 → 已补入 12288 采样静音
- 音频合成填补依据: 判据：相邻音频帧 PTS 间隔 vs 编码块 1024 采样（采样率 44100 Hz）；容差半个编码块
- 限制 / 未知项: 产物含 12288 采样合成静音（0.279 s）：它们补回的是源音频轨时间轴上的空洞，
  不是从源解出来的声音；这段时长已从恢复比例分子中扣除，按区间时长估算的恢复比例无法反映这段缺失
```

**判据边界（随结论一起报出）**

- **码流复制轨补不了**，且必须与「用户没启用」区分开：`audio_silence_track_copied` vs
  `audio_silence_not_requested`。不重编码就没有编码器，造不出这段采样 ——
  **「我们取舍了」和「我们做不到」对用户意味着完全不同的下一步**，写成前者会让用户去
  改一个改了也没用的开关。
- `gap_policy = MergeContiguous` 时一律不补（用户明确要求塌缩空洞，补静音会违背它）。
- 单处超 5 s 不补（大段缺失该由未恢复区间如实呈现），累计超 30 s 不补
  （`audio_silence_budget_exhausted`）。拒绝就是一点都不补，不许「只补前 5 秒」。
- 采样率未知（`audio_silence_no_sample_rate`）时不补：没有采样率就无法把秒数换算成
  采样数，硬补只会补错长度。
- 合成采样**不进**样本账本与写入包数（账本只记「与源对应的已恢复样本」）。混合块里的
  真实采样会一并排除，所以断言只能写「只减不增」（`EXPECT_LE`），最多少记一个编码块
  —— 这一点也写进 `note`，不让读者自己猜。
- 判据只覆盖「同一轨相邻帧之间的采样空洞」：轨道起点之前的空白、整段轨道缺失、视频轨
  空洞都不在这里。

**守护**（`tests/unit/test_repair_silence_fill.cpp`，19 个用例）

- 纯函数 13 个：容差内不算洞、缺一个块可填、未启用只记账（缺失采样数照算）、
  码流复制轨即使启用也拒绝、`MergeContiguous` 优先于填补请求、大洞拒绝且「一个采样
  都不补」、总预算耗尽、时间戳回退不算洞、采样率未知拒绝、**理由码唯一且不退化**、
  **未填补的文案不许出现「已补入」**、**未探测不许渲染成「0 处空洞」**、汇总计数、
  包缺时长时标注探测不完整。
- 端到端 6 个：空洞在未启用时也被探到并进限制项；启用后补入静音且**样本账本只减不增**；
  复制轨探到空洞但报「未填补（该轨为码流复制）」；三格式同源（JSON 可机读逐处空洞）；
  干净源不许凭空写出条目；转码轨上补静音后时长恢复。

**已知未覆盖**

- **重复帧填补仍未实现**：这是路线图第 1 项里剩下的最后一半。静音与黑帧都是「补空」，
  重复帧要判「源里本该有更多帧、只是重复了」—— 需要「同一 PTS 上多个解码帧」或
  「帧内容与前一帧逐字节相同」这类证据，本切片没有取证。
- 端到端要跑通重编码轨，必须**注入容器拒绝**：本构建造不出「源容器能装、目标容器不收」
  的音频语料（mov 写不进 flac / opus，这种组合永远不可能成为源）。注入只改接受性这
  一个判定，沿用切片 V 的 `SetTranscodeAudioContainerRejectedForTesting`。
- 语料的空洞是语料工厂**跳过** `[audio_skip_begin, audio_skip_end)` 区间的编码帧造出来的
  （`audio_gap_after_frame` / `audio_gap_frames`，不后推 PTS），与真实丢包在容器层的
  表现一致，但没拿真实损坏文件验证过。
- 编码器只接受一部分静音时（`audio_silence_encoder_rejected`）只记账不重试；本轮没有
  可复现的拒绝语料，该分支靠纯函数用例守。

---

## 5. 后续阶段与架构前置

### 5.1 阶段 5：时间轴与音画同步（先解决接入条件）

**POC 可以低成本接入，不是架构禁忌**：

- `core/analysis/codec/` 已独立成 `VideoEyeCodec`（`CMakeLists.txt:414` 的 GLOB），
  只吃 domain + media、不碰 FFmpeg 函数；`H264PocTracker` / `HevcPocTracker` 在其中。
- `scripts/check_layering.py:49` 的 repair 禁令是 `player / qc / reporting / ffmpeg / qt`，
  **不含 `core/analysis`**。只需给 `VideoEyeRepair` 加一条
  `PRIVATE VideoEyeCodec` 链接，并过 `LinkVisibilityTests`；因为 `VideoEyeCodec` 的公共头
  带 `core/media` 的 `NalUnit`，repair **公共头不得 include** POC 头，只在 `.cpp` 里用。
- 命名空间 R3：具名子命名空间必须匹配某祖先目录名。

**边界（接入前必须补测试）**：`H264PocTracker` 明确不支持 `pic_order_cnt_type == 1`、
field coding 不据 POC 步长推缺帧、不重建 DPB / RPL。遇到时 `available=false` + 原因，
不静默给 0。

**重点处理**：B 帧解码序 → 显示序并生成有证据的 `ctts`；分数帧率、VFR、时间戳回退 /
重置 / 分段缺口；音频按采样数推进，视频与音频使用统一的区间映射。
**POC 只用于显示顺序推导**，绝对帧率与时间间隔另找证据（VUI `timing_info`、sidx、
tfdt 或用户显式输入）。信息不足时提供**显式推定模式**，结构化记录 `mode`、
`user_input`、`inference_basis`、`confidence`，并复用 `RepairCapabilityBoundary`
承载「不可恢复范围」说明；`fidelity.timeline_status` 只能是 `modified / unknown`。

### 5.2 阶段 6：容忍式转码与其他容器

- **S08 现状**：**切片 T 已落地轨级决策**（见 §4.20）——每条输入轨判成
  copy / transcode / drop 并给出稳定理由码；容器接受性由内存试写头逐轨实测
  （`avformat_query_codec` 对 mpegts 返回垃圾值，不可作判据）；被丢弃的轨进
  「未保留」清单，不再无声消失；容器拒绝的音频轨宁可剔除并报出，也不拖着整份
  产物写头失败。实际编码器由运行时探测（LGPL 构建不含 libx264，会回退到
  libopenh264，见 §4.15），报告按实测名写出。
  **仍待做**：重编码范围仍是整条视频轨——GOP 级局部重编码要先验证编码参数与
  拼接边界兼容性（profile / level / 像素格式 / time_base / `max_b_frames`）。
  ~~音频只有 copy / drop，没有「转码成容器能收的编码」这条链。~~ **切片 V 已补上**
  （见 §4.22）：容器拒绝原编码时解码 → 重采样 → 重编码，目标由两次实测选出。
- ~~**待补**：动态分辨率（当前固定首帧宽高）、10-bit、`color_primaries/trc/space/range`、
  HDR 元数据与 SEI、旋转 displaymatrix。补之前产物不能声称视觉无损。~~
  **切片 J 已部分解决**（见 §4.10）：这些属性现在逐项实测并写进报告，SAR /
  色度采样位置 / 同色深色彩标记已继承；动态分辨率按帧重建 `sws` 并计数。
  **切片 Q 已解决**（见 §4.17）：旋转 displaymatrix 与 HDR 静态元数据现在真的拷进
  产物（挂输出流 `codecpar` 的 side data，`movenc` 写 `tkhd` / `mdcv` + `clli` 已实测
  证实），并由收尾处重新打开产物逐项实测后才下结论；HDR 带位深前提与矛盾检测。
  **仍待补**：HDR **动态**元数据（逐帧 SEI / 杜比视界 RPU，必须由编码器重新生成）、
  10-bit 编码器选型与全链路 10-bit 保真。
  在这些能力落地前，产物只能说「属性损失已列明」，不能说「视觉无损」。
- **黑帧 / 静音 / 重复帧**：**切片 U 已落地视频黑帧填补**（见 §4.21）——按「相邻源解码帧
  PTS 间隔 vs 标称帧率」探测空洞，用户显式启用（`fill_video_gaps_with_black_frames`，
  界面「空档补黑帧」，只在救内容模式生效）时插合成黑帧把时长补回去；逐处空洞记账
  （位置 / 缺失帧数 / 插了几帧 / 为什么没插），填补时长不进恢复率分子，
  `synthetic_frames_inserted` 不再恒为 false。
  **切片 W 已落地音频静音填补**（见 §4.23）——音频链同样按「相邻帧 PTS 间隔 vs 编码块
  采样数」探测空洞（容差半个编码块），用户显式启用（`fill_audio_gaps_with_silence`，
  界面「空档补静音」）时用静音占住空洞；码流复制轨**补不了**（不重编码就没有编码器），
  这种情况用**不同理由码**报出，不与「没启用」混写。合成采样不进样本账本，
  `fidelity.synthetic_audio` 逐处落账。
  **仍待做**：重复帧填补（要判「源里本该有更多帧、只是重复了」，证据形态与「补空」
  完全不同，本切片没有取证）；填补只覆盖「同一轨相邻帧之间」的空洞，轨道起点之前的
  空白 / 整段轨道缺失不补。
  顺带修掉一个真缺陷：`avg_frame_rate` 是「帧数 ÷ 总时长」，有空洞时偏小
  （实测 19.6 vs 名义 25），拿它当 CFR 帧率会把整条视频拉慢 30%；
  现在名义帧率高出平均值 10% 以上时改用 `r_frame_rate`。
- **容器优先级**：TS/M2TS（同步恢复、PES 重组、时间戳断裂）、AVI（索引重建与损坏
  chunk 后扫描）、FLV（tag 边界恢复与时间戳修正）、MKV（Cluster 扫描与 Cues 重建）。
  现状：非 ISOBMFF 只走通用 remux，结构级诊断只对 ISOBMFF 生效。
- **多候选规划器**：`RepairPlan` → `vector<CandidatePlan>`，每候选独立临时产物 + 验证，
  再按用户目标比较保留轨道 / 有效内容 / 时间轴变化 / 转码范围。
  ⚠️ **共享预算要按候选重新分摊**，否则第一个候选吃光预算；`RepairOutputWriter`
  的临时产物命名与清理要配套；`RepairRunBudget` 的语义从「一次修复」变为「一次修复的
  全部分支」。

---

## 6. 风险与不做清单

1. **错误结论率红线**：空表可能是真没问题 / 打开失败 / 被取消 / 超时。新格式先问
   「结论层从哪来」，完整性标志分开落。
2. **内存**：整段 mdat 入内存（1 GiB）必须在 B 阶段改掉；扫描逐块、缓存有界。
3. **参考样片污染**：R03 已有「帧都能解、播放时长却变了」的事故；借来时基必须
   有源侧独立证据，否则 `unverified` 拒绝。
4. **保码流比较口径**：必须走规范化 AU（F03 的 ADTS 剥头），别把容器包头变化当内容损坏。
5. **合成与原始分开**：补帧 / 静音 / 重复帧单独呈现。合成视频黑帧（§4.21）与合成
   音频静音（§4.23）已落地并逐处记账，**重复帧仍是空能力** —— 字段存在不等于已实现。
6. **无证据不声称**：B 帧显示顺序、绝对帧率、音画同步起点，没有 ctts / elst / VUI
   证据时保持「不确定」。
7. **真实损坏文件不算恢复率**：无原片基准，只验结论自洽。
8. **公共头闭包**：repair 公共头不得带 FFmpeg / `VideoEyeCodec` 类型。

---

## 7. 每个切片的验证链

1. `source /d/Coding/C/videoeye/msvc_env.sh`（不 source 会触发 cl C1083 连锁失败）。
2. `cmake --build build/_recheck_test` → `ctest --test-dir build/_recheck_test
   --output-on-failure --timeout 300` → 离屏
   `QT_QPA_PLATFORM=offscreen timeout 10 ./VideoEye.exe`（**rc=124 = 存活**）。
3. `build/_recheck_test` 的 `CMAKE_SUPPRESS_REGENERATION=ON` → **增删源文件必须手工
   `cmake -S . -B build/_recheck_test`**；`core/repair` 与 `core/domain` 是 GLOB_RECURSE，
   新增 `.h` 无需登记但仍需重新 configure；测试新增源要登记 `tests/CMakeLists.txt`。
4. 门禁：`scripts/check_automoc_registry.py`（AUTOMOC 是名单制）、分层 / 命名空间 /
   链接可见性、`scripts/summarize_tests.py --update-readme`（README 快照对不上就红）、
   clang-format **18.1.x**（只格式化改动文件）、`git diff --check`。
5. 判别力：每组变异精确判红 → 字节写回恢复 + `os.utime(path, None)` → 复跑绿；
   恢复后凡是要跑门禁的产物都要重建，避免复用变异态 obj。
6. 提交信息含中文引号 / 括号时用 `git commit -F <file>`；推送走
   `scripts/git_api_push.py --token-file`（本机 `github.com` POST 有 ~2 KB 上限）。

---

## 8. 进度记录

| 切片 | 状态 | 提交 | 验证摘要 |
|---|---|---|---|
| 核对（本轮） | 完成 | — | 只读核对；结论同步到 `.workbuddy/memory/2026-11-09.md` |
| A 基线收口 | 完成 | `2cec64c` | 163/163 全绿；新增 8 个用例；判别力 m1/m2/m3 全部精确判红后恢复复跑绿。A-4 按「保留扫描盲区 + 新增损失区间」实现，见 §4.1 |
| B 样本层 + 分块扫描 | 完成 | `2cec64c` | 164/164 全绿；新增 13 个用例（11 单测 + 2 产品）；判别力 m1–m4 全部精确判红后字节写回 + `os.utime` 恢复复跑绿；1 GiB 整段读入已删除；门禁全绿、离屏 rc=124。首次并行全量出现 1 个与修复链无关的既有 flaky（`AudioOutputStabilityTest.StopClearsBacklogAndAllowsReopen` 用假后端指针相等判重开，堆地址复用），单跑与二次全量均绿 |
| C 参考样片辅助 moov 重建 | 完成 | `af73b69` | 165/165 全绿；新增 9 个用例；8 组判别力变异精确判红后恢复复跑 9/9 全绿；门禁全绿、离屏 rc=124。见下方「切片 C 验收记录」 |
| D 直接扫描 + GOP 恢复 | 完成（层 2/3 落地，层 1 未做） | `5200d51` | 167 个 ctest 组首轮 166 过（唯一红是 README 快照，更新后复跑全绿）；新增 13 个用例（9 码流扫描 + 4 试解码）；4 组判别力变异精确判红；门禁全绿、离屏 rc=124。见下方「切片 D 验收记录」 |
| E 显示顺序证据层（POC → ctts） | 完成（本轮） | `5200d51` | 新增 `RepairDisplayOrder.{h,cpp}` 与 11 个用例；真实 B 帧语料显示序与容器 PTS 逐项一致；自建重排码流重建后 `ctts` 偏移逐条对上；6 组判别力变异精确判红。见下方「切片 E 验收记录」 |
| F 时基证据（VFR 声明 + 显式帧率） | 完成（本轮） | `a73e42a` | 新增 `RepairTimelineBasis` 与 11 个用例；无 VUI 时显式帧率可重建且标 `user_input`；`fixed_frame_rate_flag=0` 不再被静默当成 CFR；8 组判别力变异精确判红。见下方「切片 F 验收记录」 |
| G 逐轨覆盖与音画共同区间 | 完成（本轮） | `5932508` | 新增 `RepairTrackCoverageSummary` 与 9 个用例；逐轨缺口由账本流式统计导出（明细截断不影响计数）；共同覆盖取交集；6 组判别力变异精确判红。见下方「切片 G 验收记录」 |
| H 参考辅助重建写 ctts | 完成（本轮） | `a8029e9` | 含 ctts 的参考不再被拒（只记录不借用）；产物 ctts 由源侧 POC 实测；真实 B 帧语料删 moov 后重建成功且逐包 PTS 与原片一致；4 组判别力变异精确判红。见 §4.8 |
| I VFR 逐帧时长 | 完成（本轮） | `1a7ebaf` | 参考 stts 非常量不再整份拒绝；产物 `stts` 逐段等于参考、`mdhd` 时长取合计、逐包 DTS 间隔与逐帧表一致、VFR 下不写 ctts；新增 5 个用例与 5 组判别力变异（全部精确判红）。见 §4.9 |
| 阶段 5 时间轴 / 音画同步 | 部分（显示顺序 / ctts / 时基证据 / 逐轨覆盖 / VFR 逐帧时长已落地；`elst` 明确不重建并由 L 显式标注） | | 见 §4.5–§4.9、§4.12 |
| J S08 源侧保真属性探测 | 完成（本轮） | `c4d1672` | 位深 / 色彩 / 旋转 / HDR / 动态分辨率逐项实测并同源写进三种报告；可零风险继承的已继承；`sws` 按帧尺寸重建修掉越界读；15 个用例 + 11 组判别力变异全部精确判红。见 §4.10 |
| K 真实语料结论层守护 | 完成（本轮） | `4856937` | 实测查出 S08「有账本却报没有账本」与时基 / 显示顺序未测量态静默省略两个真 bug；S08 上抛逐轨覆盖且不许伪造 span；5 个用例 + 10 组判别力变异全部精确判红。见 §4.11 |
| L 本次明确未恢复项显式标注 | 完成（本轮） | `6cd0a8a` | 8 条能力目录 + 结构化触发矩阵；9 个纯函数用例 + 3 个真实语料端到端用例；172/172 ctest（1938 gtest）；13 组判别力变异全部精确判红后按字节恢复复跑绿。见 §4.12 与下方「切片 L 验收记录」 |
| M 参考辅助重建路径（C/H/I）结论层收敛 | 完成（本轮） | `a7aac33` | 摸底实测一次抓到 4 处自相矛盾结论（借来的时基写成 `unchanged`、兼容性 `not_applicable`、有参考却说没有参考、借用时基渲染成容器时间戳）；5 个用例（含主路径反向守护）；172/172 ctest（1943 gtest）；11 组判别力变异全部精确判红（首轮 M5/M7 判绿，补断言 + 删不可达分支后转红）。见 §4.13 与下方「切片 M 验收记录」 |
| N fMP4 借参考初始化段路径结论层收敛 | 完成（本轮） | | 摸底实测抓到 3 处「无事实却给结论」（借用时基渲染成容器时间戳、产物实测时长写成 0.000s、恢复比例基线来源说反）；1 个真实语料端到端用例 + 2 条主路径反向断言；172/172 ctest（1944 gtest）；4 组判别力变异全部精确判红。见 §4.14 与下方「切片 N 验收记录」 |
| O S08 重编码的编码器口径收敛 | 完成（本轮） | | 实测证实本构建无 libx264（LGPL 变体），S08 实际用 libopenh264；三处静态文案把首选名写成了既成事实；新增结构化编码器 / profile 字段与 5 个用例；172/172 ctest（1949 gtest）；5 组判别力变异全部精确判红。见 §4.15 与下方「切片 O 验收记录」 |
| P S08 音画起点对齐（统一原点） | 完成（本轮） | | 实测抓到旧实现把源侧 0.48 s 音画起点差静默抹成 0 且报告不提；改为两轨减同一原点，差值保留并进报告；新增 3 个用例；172/172 ctest（1952 gtest）；P1/P3 精确判红、P2 判绿（如实记账：现有语料不可判别）。见 §4.16 与下方「切片 P 验收记录」 |
| Q 容器级 side data 真正继承 | 完成（本轮） | | 实测：旋转矩阵 / HDR 静态元数据旧实现全丢，`movenc` 其实支持（tkhd / mdcv+clli）；改为写头前拷 side data + 收尾打开产物实测；新增第三类 `unverified_properties` 与 HDR 位深前提 + 矛盾检测；新增 6 个用例；172/172 ctest（1958 gtest）；7 组判别力变异全部精确判红。见 §4.17 与下方「切片 Q 验收记录」 |
| R 能力边界支持程度口径 | 完成（本轮） | | 三条契约一律 `unsupported` 而 evidence 写着「已实现…」，章节标题还叫「未实现能力边界」；改状态词表（`partially_supported` / `unsupported` / `deferred`）+ 拆出 `covered_scope` / `not_covered_scope` 与物理不可恢复三段，状态词与两张表互为约束；新增 3 个用例；172/172 ctest（1960 gtest）；6 组判别力变异全部精确判红。见 §4.18 与下方「切片 R 验收记录」 |
| S 合成初始化段时基分级 | 完成（本轮） | | 自建路径不占 `timeline_basis.mode`，回落成「容器时间戳」（源片根本没有 moov），且 `timeline_status=unchanged`；改为 `synthesized_init_timescale` + `unknown`；时基核对可测、可断言「没核对 ≠ 核对通过」；点明时长比对在这条路上是自证；新增 1 个纯函数用例 + 扩写端到端用例；172/172 ctest（1961 gtest）；7 组判别力变异全部精确判红（S4 首轮判绿 → 断言不够严）。见 §4.19 与下方「切片 S 验收记录」 |
| T S08 的轨级决策 | 完成（本轮） | | 实测三个缺口：第二视频轨/封面/字幕/数据轨无声消失、容器拒绝某条音频轨时整份产物写头失败、「为什么重编码」从未落账；改为逐轨 copy/transcode/drop + 稳定理由码，容器接受性用内存试写头实测（`avformat_query_codec` 对 mpegts 返回垃圾值，不可用）；新增 14 个用例（11 纯函数 + 1 容器实测 + 2 端到端）；173/173 ctest（1976 gtest）；9 组判别力变异全部精确判红。见 §4.20 与下方「切片 T 验收记录」 |
| V 音频轨重编码成容器能收的编码 | 完成（本轮） | | 容器拒绝原编码时不再只能丢轨：解码 → 重采样 → 重编码，目标由「编码器存在 + 参数能开 + 试写头接受」两次实测选出，一条都过不了才 drop 并逐条报出原因。落链时抓到两个真缺陷：首帧探测期间的音频包对重编码轨是直接丢掉的（产物音频从 0.58 s 才开始）、四层缓冲不排空会让结尾少 47 ms 而产物仍报成功。新增 14 个用例；188/188 ctest；10 组判别力变异全部精确判红。见 §4.22 与下方「切片 V 验收记录」 |
| U 视频轨空洞的黑帧填补 | 完成（本轮） | | 实测两件事：S08 把源时间轴空洞静默压掉（40 包空 12 帧 → 产物只写 39 帧，音画越拖越偏且报告不提）；有空洞的源 `avg_frame_rate` 偏小（19.6 vs 名义 25），拿它当 CFR 帧率会把整条视频拉慢 30%。改为显式启用的黑帧填补 + 逐处空洞记账 + 恢复比例扣合成时长；新增 18 个用例；174/174 ctest（1994 gtest）；10 组判别力变异全部精确判红。见 §4.21 与下方「切片 U 验收记录」 |
| W 音频轨空洞的合成静音填补 | 完成（本轮） | | 实测：音频链按源 PTS 逐帧定位，源中间缺一段就把位置直接跳过去，产物里留一段没有采样覆盖的时间且报告不提；码流复制轨上黑洞**探不到** —— 容器把空洞并进了前一个样本的时长（实测 33 个包 `dur=0.301859` 顶 13 个编码块，其余 `dur=0.023220`）。改为显式启用的静音填补 + 两种形态的空洞探测 + 逐处记账 + 「复制轨补不了」与「没启用」用不同理由码区分；新增 19 个用例；176/176 ctest（2028 gtest）；13 组判别力变异全部精确判红。见 §4.23 与下方「切片 W 验收记录」 |
| 阶段 6 转码 / 其他容器 / 多候选 | 进行中（J / T / U / V / W 已落地） | | 见 §5.2 |

### 切片 C 验收记录（2026-11-11）

**范围与约束**（§4.3）：第一版只认「H.264 + AAC、单 `mdat`、无 `moov` / `moof`、
4 字节 NAL 长度前缀、AAC 恒定 1024 采样/帧、采样率恒定、无 `ctts` / B 帧」这一种
录制格式。三不借仍然成立：不把 `stts` / `ctts` / `stsz` / `stco` 任何取值当真值、
不把 `mdhd timescale` 当真值、不借 `duration`；只借 `stsd` 解码配置（`avc1` / `avcC`、
`mp4a` / `esds`）、NAL 长度前缀、track→codec 映射、`ftyp` brands。样本大小、偏移、
时间表一律从损坏文件重扫。

- **参考自洽**：ASC 采样率 / 声道与 `mp4a` 声明一致、音频 `timescale == sample_rate`、
  `sample_delta == 1024`、`stts` / `stsz` / `stsc` / `stco` 逐轨守恒、chunk 偏移落在
  参考 `mdat` 载荷内且无重叠、按 `relative_start` 排序后能完整解释载荷。
- **源侧自证**：按 chunk 相对起点逐块推进；视频 AU 由 4 字节 NAL 长度链 +
  `first_mb_in_slice` 实测且必须覆盖 chunk 硬边界；音频候选长度必须等于该 chunk 的
  源侧实际字节数；收尾要求消费字节数 == `mdat` 载荷大小、逐轨样本数守恒。
- **报告**：`RepairReferenceAssist` 同时进 JSON / TXT / HTML，列出借用参数、兼容性、
  源侧布局核验、时基核对与限制；样本层把推断时间戳标为 `Inferred`，不与实测混写。

**验证**：

- 新增 9 个 gtest 用例（`RepairReferenceMoovTests`）：参考片自洽 / B 帧与时基篡改拒绝 /
  Probe 正反 / 逐轨序列与时间轴守恒 / 不同源与截断反向守护 / 载荷裁剪反向守护 /
  端到端引擎 + 三格式报告 / 引擎拒绝异源参考 / 分块大小产物二进制一致。
- 全量：`cmake --build build/_recheck_test` rc=0，`ctest` **165/165 全绿**，
  离屏启动 `rc=124`；clang-format 18.1.8、AUTOMOC 9/145、分层、命名空间、
  README 快照（146 / 146 / 1856 / 19）、`git diff --check` 全绿。
- 判别力 8 组变异：M1 时基交叉核验、M2 音频 chunk 字节守恒、M4 TXT/HTML 报告、
  M5 引擎结果透传、M6 JSON 字段、M7 诊断 finding 接线、M8 整类布局守卫，
  预期用例均精确判红；M3 单独拆「视频 NAL 链覆盖」不判红 —— 该守卫与下一 chunk
  起点对齐 / 收尾字节守恒互为冗余，属防御性断言，已由复合变异 M8 验证整体有效。
  全部按字节写回 + `os.utime` 恢复，复跑 9/9 全绿。
- 首轮并行全量出现 1 例既有时序 flaky：`ContainerCancelPathTest.DashCancelYieldsNoResult`
  按 `elapsed_ms` 折算取消点，单跑 16.9 s 全绿、二次全量 165/165 全绿，与修复链无关。
- 全量构建曾挂在本机 vcpkg `z-applocal` 依赖部署子进程（CPU≈0 持续 8 分钟）；
  终止挂起链后 `-j 2` 重跑 rc=0，属文件占用类环境 flake。

**未覆盖（留后续）**：HEVC、多 `mdat`、多轨、B 帧 / `ctts` / VFR、分片 MP4、
源侧独立时基证据，以及阶段 5 的时间轴 / 音画同步恢复。

### 切片 D 验收记录（2026-11-11）

**做了什么**（§4.4 三层里的第 2、3 层）：

- `RepairRecoveryEvidence`（5 值枚举）+ `RepairRecoveredSegment.recovery_evidence`：
  恢复点从自由文本升级为「机读码 + 描述」。JSON 落 `recovery_evidence`，
  TXT/HTML 落显示名，`lost_ranges.evidence` 里也带来源码 —— 三格式同一事实源。
- `RepairTrialDecode`：候选恢复点必须过真实解码器裁决。判据是四道与门
  （容器 KEY / NAL 有 IDR / 解出 ≥1 帧 / 无损坏标记）；第一版只认 H.264 +
  4 字节长度前缀，其它编码返回 `attempted=false`，报告写「未做试解码」，
  不把「没试」说成「通过」。
- `RepairBitstreamResync`：逐块扫 `mdat` 载荷找 RAP 候选（H.264 / HEVC）。
  **只做诊断**：找到候选也只写「载荷内还有 N 个候选，但未接入输出」，
  因为当前没有不依赖索引的写包路径。
- 执行器接线：`TryResync` 找到关键帧后逐候选试解码，拒绝即继续找；
  全部失败时把「是没找到候选，还是找到了但解不出来」分别写进 residual finding。

**验证**：

- 新增 13 个用例：`RepairBitstreamResyncTests` 9 个（H.264/HEVC 正例、
  纯 P 帧反例、分块大小一致、预算取消、窗口截断、mdat 定位正反、未支持编码）、
  `RepairTrialDecodeTests` 4 个（健康关键帧通过、slice 中部破坏必须拒绝、
  取消不谎报、越界 seek 不谎报）。
- 产品级：`RealCorpusMidFileDamageResumesIntoSegments` 增加
  「第二段证据 == `trial_decode_ok`」断言；`LostRanges...AllReportFormats`
  增加 JSON 机读码与 TXT/HTML「恢复点来源」断言。
- 全量：`cmake --build build/_recheck_test` rc=0，`ctest` 167 组首轮 166 过
  （唯一红是 README 快照），`summarize_tests.py --update-readme` 后复跑全绿；
  离屏启动 rc=124；clang-format 18.1.8、AUTOMOC 9/147、分层、命名空间、
  `git diff --check` 全绿；README 快照 148 / 148 / 1869 / 19。
- 判别力 4 组：M1 试解码恒通过、M2 证据不升级、M3 码流扫描不要求 RAP
  （冗余守卫，需复合变异）、M4 JSON 丢证据，均让预期用例精确判红；
  按字节写回 + `os.utime` 恢复后全绿、无残留。

**未覆盖（如实记账）**：

- **层 1 容器级 moof/trun 重同步未做**：需要「分片样本表重建 + 自建 init/moof
  后写包」的另一条路径，且要先与 C 的 init 段核验链统一，当前也没有稳定的
  fMP4 中间损坏语料。
- **端到端「拒绝不可解码恢复点」缺语料**：破坏中段关键帧载荷后，demuxer 直接
  丢掉了候选（`TryResync` 报「各步长均未找到可用关键帧候选」），根本走不到
  试解码那一层。该判据目前只在模块级守护；因此第一次写的端到端用例属于
  **假覆盖**，已删除，不留在仓库里充数。
- **码流候选无法定位到时间轴**：没有容器索引就没有时基证据，`byte_offset`
  是唯一硬事实。要把它换成秒并写进产物，需要样本表反查或参考样片（C 的能力），
  属下一轮。

### 切片 E 验收记录（2026-11-11）

**交付**

- 新增 `core/repair/RepairDisplayOrder.{h,cpp}`：
  `DeriveDisplayOrder`（纯推导）/ `MeasureDisplayOrder`（逐 AU 实测 POC）/
  `BuildCttsBox`（version 1 有符号 + RLE）/ `SummarizeDisplayOrder`（结果模型摘要）。
  公共头只有 domain 值类型，`FrameOrderTracker` 与 `libavcodec` 全在 `.cpp`；
  `VideoEyeRepair` 新增 `PRIVATE VideoEyeCodec`，`check_layering.py` 仍绿。
- `RepairPlainMp4Rebuilder`：重建前实测显示顺序，`BuildMoov` 在有重排时追加 `ctts`；
  结论（`analyzed` / `available` / `ctts_required` / `ctts_written` 分开落）经
  `RepairExecutionOutcome` → `RepairResult.display_order` → JSON/TXT/HTML。
- `RepairTypes.h` 新增 `RepairDisplayOrderSummary`；报告新增「显示顺序」行与
  「显示顺序依据 / 帧时长证据 / 显示顺序限制」条目，三格式同源。

**测试（11 个用例）**

- 推导表驱动：段内按 POC 定序、跨段不比较 POC、`max_reorder`、有符号偏移、
  前导图像/空输入判不可用、无恒定帧时长不生成 ctts、条数不符时一个字节都不写、
  `ctts_written` 不能靠标志位凑出来。
- 真实语料：`real_x264_bframes_gop.mp4`（30 帧 / 2 GOP / 4 个 B 帧层）上按 POC 排出的
  显示序与按容器 PTS 排出的顺序**逐项一致**，`reordered_frames > 0`（防止空跑）。
- 真实样本接线：`real_h264_inband_missing_moov.mp4` 重建后 `analyzed && available`、
  `duration_source == vui_timing`、无 B 帧故 `ctts_written == false` 且产物无 `ctts`。
- 自建重排码流（手写 SPS/PPS/slice_header，两个 GOP 的 I P B B）：重建产物
  `ctts` 偏移逐条等于 `{0, +4, −2, −2} × 2`，且 `ctts` 为 version 1。
- 引擎 + 三格式：`RepairEngine::Run` 后 `result.display_order` 有结论，
  JSON 含 `display_order` / `ctts_written` / `reordered_frames`，TXT/HTML 含
  「显示顺序」与推导依据。

**判别力（6 组，全部精确判红后恢复复跑 11/11 全绿）**

| 变异 | 拆掉的东西 | 判红的用例 |
|---|---|---|
| M1 | 排序键换成解码序 | 段内定序 + 自建码流 ctts |
| M2 | `ctts` 写成 version 0 | 有符号偏移 + 自建码流 ctts |
| M3 | 不再按随机接入点分段 | 段内定序 |
| M4 | 永不要求写 `ctts` | 自建码流 ctts |
| M5 | JSON 不落 `display_order` | 引擎 + 三格式 |
| M6 | 引擎不把结论写进 `RepairResult` | 引擎 + 三格式 |

**验证链**

- 全量构建 rc=0；`ctest` **168/168 全绿**（163.82 s）；离屏启动 rc=124。
- clang-format 18.1.8、AUTOMOC 9/148、分层、命名空间、`git diff --check` 全绿；
  README 快照 149 / 149 / 1880 / 19。

**未覆盖（如实记账）**

- **真实语料缺口**：仓库里没有「既有 B 帧、SPS 带 VUI timing、参数集在 in-band」的
  样本。x264 产 B 帧但不写 VUI，libopenh264 写 VUI 但不出 B 帧；实测 ffmpeg 6.1 的
  `h264_metadata=tick_rate` 也无法给 x264 的 SPS 补出 VUI。于是 ctts 落盘路径用自建
  最小码流覆盖（可过 SPS/PPS/slice_header 解析，不含可解码图像数据），
  语义正确性由真实 B 帧语料对账 PTS 负责 —— 两条合起来才算覆盖。
- **参考辅助重建（C）的产物仍不写 ctts**：C 的第一版明确要求参考样片无 `ctts`
  （也就是无重排），两条路的前提互斥，暂不重复接入。
- **变帧率 / 分数帧率 / 音画统一区间映射 / `elst`** 未做，仍属阶段 5 剩余部分。

### 切片 F 验收记录（2026-11-11）

**交付**

- `model::RepairTimelineBasis`（`mode` / `frame_rate` / `constant` / `vfr_declared` /
  `detail` / `limitations`）+ `RepairResult.timeline_basis`：时基证据单独成一块，
  不从其它字段反推。`RepairRequest` 新增 `assumed_frame_rate_num/den`
  （精确分数，"0/0" = 不推定；只给一半直接拒绝）。
- `RepairPlainMp4Rebuilder`：
  - `ParsePlainSps` 支持两条证据路径。没有 VUI / 没有 `timing_info` / `timing_info`
    无效，这三种情况统一走 `fall_back_or_reject`：有显式帧率就用（`mode=user_input`
    + 限制项写明"不是源侧证据"），没有就拒绝，错误文案指出可用显式帧率补救。
  - `fixed_frame_rate_flag` 不再被读掉丢弃：`=0` 时按名义帧率重建，但
    `duration.constant=false` → 不生成合成偏移 → **不写 ctts**，并落限制项。
  - `ComputeTimelineTicks`：帧率分数 → `(timescale, sample_delta)`，取最小放大倍数
    使 timescale ≥ 1000；32 位 mdhd 时长上限做硬拒绝（本版本不写 version 1）。
  - `ScanResult` 记录 `timeline_mode` / `frame_rate_text`（约分后的精确分数）/
    `timeline_detail`；ADTS 路径落 `mode=adts_sample_rate`（采样率 ≠ 帧率，不冒充）。
- 引擎：显式帧率参数不完整时在开跑前失败；`fidelity.timeline_status` 在
  `mode=user_input` 或 `vfr_declared` 时写 `modified`（此前会落到 `unchanged`）。
- 报告：JSON 落 `timeline_basis` 对象（含 `vfr_declared` 布尔与精确 `frame_rate`）；
  TXT/HTML 落「时基依据 / 帧时长是否恒定 / 码流声明非恒定帧率」三行 + 时基限制项。
- UI：修复面板新增「推定帧率」下拉（23.976 / 24 / 25 / 29.97 / 30 / 50 / 59.94 / 60
  与「不推定」），值用 `"num/den"` 精确分数传给请求。

**顺带修掉的一个"嘴硬"**：切片 E 的 `DeriveDisplayOrder` 在**没算出**合成偏移时
（变帧率 / 时长未知 / 偏移超范围）也会追加一句「显示序与解码序一致，产物不需要 ctts」。
那是把"不知道"说成"没问题"。现在只有真的算过偏移、且每帧偏移都是 0 才说这句话。

**测试（11 个用例，`tests/unit/test_repair_timeline_basis.cpp`）**

- 换算表：25 → 1000/40、50 → 1000/20、30000/1001 → 30000/1001、24000/1001 原样。
- 反例：`0/1`、`25/0` 拒绝；两亿样本按 25 fps 换算超出 32 位必须拒绝（报错含
  `mdhd version 1`）；`samples == 0` 表示"还不知道"时跳过高限检查。
- 无 VUI：无帧率 → `attemptable=false` 且原因含「帧率证据」「显式帧率」；
  给 25 fps → 重建成功、产物 `mdhd timescale=1000`、`stts.delta=40`、
  `mode=user_input`、限制项含「显式输入」。
- `timing_info` 无效（`num_units_in_tick=0`）：无帧率拒绝，给 30 fps 则按 30/1 重建。
- VFR 声明：`vfr_declared=true`、`ctts_written=false`、产物无 `ctts`、
  显示顺序限制项里**不得**出现「显示序与解码序一致」。
- 分数帧率：`frame_rate=30000/1001`、`timescale=60000`、`stts.delta=2002`。
- 引擎：参数只给分子 → `Failed` 且不发布；给完整 25/1 跑通 → 结果 `mode=user_input`、
  `timeline_status=modified`、三格式报告都写出来源。
- 报告：直接构造 `vfr_declared` 的结果，JSON/TXT/HTML 三处都要出现 VFR 声明与精确帧率。

**语料**：手写最小码流（`tests/unit/H264TestBitstream.h`）。原 `test_repair_display_order.cpp`
里的逐位构造器抽到这里共用 —— 切片 E 与 F 都要改 SPS 的 VUI / `fixed_frame_rate_flag`，
两份各抄一遍迟早不一致。抽出后 E 的 11 个用例复跑全绿。

**判别力（8 组，全部精确判红后恢复复跑全绿）**

| 变异 | 拆掉的东西 | 判红的用例 |
|---|---|---|
| M1 | 忽略 `fixed_frame_rate_flag` | VFR 声明不下发 ctts |
| M2 | VFR 仍按恒定时长算偏移 | 同上 |
| M3 | 无 VUI 时无视显式帧率 | 显式帧率重建 |
| M4 | `mode` 报成 `vui_timing` | 显式帧率重建 |
| M5 | 去掉 32 位时长守卫 | 时长溢出拒绝 |
| M6 | 引擎不把推定时基标成 modified | 引擎接线 |
| M7 | 帧率不约分（`60000/2002`） | 分数帧率精确值 |
| M8 | 没算偏移也说"显示序一致" | VFR 声明不下发 ctts |

**验证链**

- 全量构建 rc=0；`ctest` **169/169 全绿**（157.07 s / 二轮 199.55 s）；离屏启动 rc=124。
  第三轮出现一次 `AudioOutputStabilityTests` 红 —— 就是 B 阶段记过的既有 flaky
  （假后端用指针相等判重开，堆地址复用），单跑与二轮全量均绿，未改业务代码。
- clang-format 18.1.8、AUTOMOC 9/149、分层、命名空间、`git diff --check` 全绿；
  README 快照 150 / 150 / 1891 / 19。

**未覆盖（如实记账）**

- **VFR 的逐帧时长不恢复**：没有样本表就没有逐帧证据，当前只按名义帧率重建并
  标注"逐帧时长不可验证"。要真恢复需要从容器 `stts` / 参考样片取逐帧时长，属
  下一轮（C 的参考路径也没有接）。
- **音画统一区间映射**：由切片 G 补上（逐轨覆盖 + 共同覆盖交集），见下方「切片 G 验收记录」。
- **`elst` 不重建**：负偏移用 ctts version 1 表达，不做"整体平移 + 裁开头"。

### 切片 G 验收记录（2026-11-11）

**交付**

- `RepairTrackGap` / `RepairTrackCoverage` / `RepairTrackCoverageSummary` +
  `RepairResult.track_coverage`：逐轨样本数、覆盖区间、典型间隔、缺口条数与总时长、
  共同覆盖区间，全部走 JSON / TXT / HTML 同一事实源。
- `TrackCoverageStats`（`RepairSampleScan.{h,cpp}`）：账本内逐样本 O(1) 更新。
  两个定长直方图（间隔 + 候选缺口时长，log2 毫秒分桶）+ 有界缺口明细（32 条）。
  **条数与总时长由直方图导出，明细截断只影响列出的条数**。
- 执行器：`complete` 由「未取消 / 未超时 / 未读错 / 未截尾 / 未达输出上限」共同决定；
  codec 名在建输出轨时捕获（demuxer 收尾前就关闭了）；`kind` 取验证目标。

**判据**（`间隔 ≥ max(0.5 s, 3 × 该轨典型间隔)`）

- 相对项必要：10 s 一帧的延时摄影，`间隔 10 s` 是正常的；
- 下限必要：30 fps 下丢十几帧不值得报成损失区间；
- 基准是「已见最大时间戳」，不是 `abs(相邻差)` —— B 帧重排与时间戳重置都不是丢失；
- 典型间隔取众数桶（几何中值），是**估算值**，报告写明近似；
- 方向是少报：缺口起点可能晚一个重排深度。

**验证**

- 新增 9 个 gtest 用例（`RepairTrackCoverageTests`）：只有真缺内容的轨报缺口 /
  延时摄影不算丢失 / 0.4 s 空洞不报 / 重排与时间戳重置不报 / 明细截断时条数仍精确 /
  共同覆盖是交集 / 不相交时明确未知 / 无时间戳样本单独计数 / 空账本给出原因。
- 端到端：`LostRangesAreStructuredAndVisibleInAllReportFormats` 增加
  「`available` / `complete` / JSON 字段 / TXT·HTML 共同覆盖与逐轨缺口」断言。
- 全量：构建 rc=0（一次链接卡死 20 分钟无 CPU 变化，终止后 `-j 2` 重跑 rc=0，
  属本机环境 flake）；`ctest` **170/170 全绿**（161.23 s）；离屏启动 rc=124；
  clang-format 18.1.8、AUTOMOC 9/151、分层、命名空间、`git diff --check` 全绿；
  README 快照 151 / 151 / 1900 / 19。
- 判别力 6 组全部精确判红后按字节恢复复跑绿：M1 去掉下限、M2 去掉相对项、
  M3 全部样本记到同一条轨、M4 条数取自被截断的明细、M5 共同覆盖取并集、
  M6 用 `abs(差)` 当间隔。

**未覆盖（如实记账）**

- **不推断缺口里该有多少帧**：没有样本表就没有逐帧证据，只报「这一段没有内容」。
- **不换算恢复率**：逐轨缺口不进恢复率分子（分母仍缺源侧基线）。
- **参考辅助重建 / 转码路径没有逐轨覆盖**：这两条路径的样本账本未接入
  `TrackCoverageStats`，报告会明确写「未接入」，不会当成零缺口。

### 切片 H 验收记录（2026-11-11）

**交付**

- `RepairReferenceTrackProfile.has_ctts`：参考含 ctts 时只记录、不拒绝；限制项写明
  「不借用其合成偏移」。`RepairReferenceRebuildResult.display_order` 把实测结论带进结果。
- `ScanSource` 顺带产出视频 AU 范围（就是逐字节守恒用的那一份，不重扫），
  再用参考的 `avcC`（从 sample entry 原样取出）驱动 `MeasureDisplayOrder`；
  `pts_seconds` 带上实测偏移、`display_ordinal` 用 POC 测出的显示序号。
- `BuildMoov` 在 `stts` 之后写 `ctts`（version 1 有符号 + run-length 压缩），
  条数不等于样本数时**不写**。
- 执行器：参考路径的 `display_order` 原样上抛，报告三格式同步（已有 `display_order` 段落）。

**验证**

- 新增/改造 2 个用例：`ReadProfileAcceptsBFrameReferenceButDoesNotBorrowCtts`
  （记录 has_ctts、借用清单不含 ctts、限制项有说明、篡改时基仍拒绝）、
  `RebuildWithBFrameReferenceWritesMeasuredCtts`（真实语料删 moov → 重建 →
  产物视频轨有 ctts → **逐包 PTS 间隔与原片逐项一致**）。
- 全量：构建 rc=0；`ctest` **170/170 全绿**（161.11 s）；离屏启动 rc=124；
  clang-format 18.1.8、AUTOMOC 9/151、分层、命名空间、`git diff --check` 全绿；
  README 快照 151 / 151 / 1901 / 19。
- 判别力 4 组全部精确判红后按字节恢复复跑绿：M1 不写 ctts、M2 写全零 ctts、
  M3 不喂参数集（POC 不可用）、M4 重新拒绝含 ctts 的参考。

**未覆盖（如实记账）**

- **`elst` 仍不重建**：负偏移用 ctts version 1 表达，不做「整体平移 + 裁开头」。
- **开放 GOP 前导图像（RASL/RADL）仍判不可用**（沿用切片 E 的边界）。
- ~~**逐帧时长仍取参考 stts 的恒定值**：VFR 参考（stts 多条不同 delta）依旧拒绝。~~
  已由切片 I 解决，见 §4.9。
- **只覆盖 H.264(AVCC) + AAC 双轨**：HEVC / 多轨 / 分片 MP4 不在本版。

### 切片 I 验收记录（2026-11-11）

**交付**

- `RepairReferenceTrackProfile`：新增 `stts`（完整 run 序列）与 `variable_stts`；
  `sample_delta` 语义明确为「主 delta = count 最大的那一段」，恒定轨不变。
- `ReadProfile`：视频轨非常量 stts 不再拒绝，记录全序列 + 限制项（「逐帧时长按参考
  样本表借用…产物不写 ctts」）+ 借用清单一条；音频轨仍拒绝（文案改为「音频必须恒为
  1024 采样/帧」）；`timescale_check` 与布局指纹带 VFR 段数。
- `ScanSource`：展开逐帧时长表（帧数不等直接拒绝）；`RepairFrameDurationEvidence`
  走 `source="stts_variable" / constant=false`；DTS 用 **uint64 tick 累加**，
  `duration_seconds` 取该帧时长；末尾加 **账本时间轴自洽校验**。
- `BuildMoov`：`TrackOutput.stts_runs` 按原 run-length 写多条 `stts`；
  `TrackDurationTicks` 统一给 `mdhd`/`tkhd`/`mvhd` 时长；`Rebuild` 增加 32 位溢出
  拒绝与「逐帧表覆盖样本数」校验。

**验证**

- 新增 5 个用例：`ReadProfileAcceptsVariableSttsReference`（含「主 delta 不是首段」的
  判别点）、`ReadProfileRejectsVariableSttsOnAudioTrack`、
  `RebuildBorrowsVariableSttsAndKeepsFrameDurations`（产物 stts 逐段 + mdhd 时长 +
  **逐包 DTS 间隔**）、`RebuildRejectsOverflowingMediaDuration`、
  `VariableSttsReferenceWithBFramesWritesNoCtts`。
- 测试辅助 `PatchTrackStts`：把视频/音频 stts 换成任意 run 序列并同步修祖先盒大小、
  `stco/co64` 偏移（**按值判断是否需要平移**：moov 在 mdat 之后时偏移根本不该动）、
  `mdhd` 时长；`real_h264_aac_faststart.mp4` 也用它造「B 帧 + VFR」语料。
- 判别力 5 组全部精确判红后按字节恢复复跑绿：M1 VFR 整份拒绝、M2 PTS 用
  「序号 × 主 delta」、M3 时长退回「样本数 × 主 delta」、M4 VFR 也当恒定帧长写 ctts、
  M5 去掉 32 位守卫。
  ⚠️ M2 第一轮**判绿**：当时的断言（产物逐包 DTS 间隔）测的是 `stts` 盒，而账本 PTS
  不进产物 —— 这正是「断言没覆盖到被改动的东西」。补上账本末帧终点自洽校验后复测判红。

**未覆盖（如实记账）**

- **`elst` 仍不重建**（阶段 5 唯一剩下的一项）。
- **VFR 且含 B 帧时产物无 `ctts`**：播放顺序退回解码序。这是有意的取舍（换算需要
  恒定帧长），报告限制项已写明，但「能播对画面顺序」这件事在 VFR 下确实做不到。
- **无参考路径的 VFR 逐帧恢复**仍不做：没有样本表就没有证据，只在 `timeline_basis`
  里标 `vfr_declared` + `modified`（切片 F）。
- **`mdhd` version 1 未实现**：超出 32 位的时长直接拒绝。

### 切片 J 验收记录（2026-11-12）

**交付**

- 新 `core/repair/RepairTranscodeFidelity.h`（header-only）：
  `TranscodeBitDepth` / `ApplyTranscodeInheritance` / `ApplyTranscodeFrameProperties` /
  `TranscodeScaleState` / `InspectTranscodeFidelity` / `FinalizeTranscodeFidelity`。
- `model::RepairTranscodeFidelity`（`RepairTypes.h`）挂到 `RepairFidelity.transcode`；
  `RepairExecutionOutcome.transcode_fidelity` 作为执行层→引擎的唯一入口。
- `RepairTolerantTranscode.cpp`：重编码前实测；`sws` 按帧尺寸 / 格式重建；
  新增 `video.transcode_property_loss` finding；`transcode_note` 带探测摘要。
- `RepairEngine.cpp` 原样上抛；`RepairReportExporter.cpp` 新增
  `BuildTranscodeFidelity`，`BuildFidelity` 落 `transcode` 子对象，
  `BuildSections` 给 TXT/HTML 加「转码保真探测」整节（含未探测态）。
- `RepairCapabilityContracts.cpp`：S08 的 `quality_risks` / `evidence` 补上
  位深 / 色彩 / 旋转 / HDR 的风险与「未实测不得声称已保留」的口径。
- 顶层 `CMakeLists.txt`：`VideoEyeRepair` 的 FFmpeg 三件套 PRIVATE → PUBLIC；
  `LinkVisibilityTests` / `LinkVisibilitySelfTestTests` 首轮判红即由此而来。

**验证**

- 新增 `test_repair_transcode_fidelity`（15 用例）：无证据反向守护、普通 8-bit
  不谎报、10-bit 位深损失、位深变化只拦色彩标记不误伤 SAR / 色度位置、
  同色深真写进编码器与转换帧才允许进 preserved、清单说继承但帧没写必须判红、
  旋转矩阵 / 单位矩阵、HDR side data、色度抽样、动态分辨率收尾、
  缩放状态（失败重试 / 格式变化不算分辨率变化）、S08 端到端三格式、
  保码流路径三格式都写「未探测」。
- 判别力 11 组全部精确判红后按字节恢复复跑绿：M1 位深损失不报、M2 位深变了
  也照搬色彩标记、M3 声称继承色度位置但没写帧、M4 未探测被抬成已保留、
  M5 旋转不判损失、M6 HDR 不判损失、M7 引擎不上抛、M8 JSON 不写子对象、
  M9 TXT/HTML 把未探测写成已实测、M10 缩放状态不计数、M11 格式变化不重建 sws。
  ⚠️ M9 与 M10 第一轮**判绿**：前者断言只找泛化的「未探测」，被另一行
  「源侧属性结论」蒙混过去；后者计数逻辑当时埋在 `.cpp` 循环里，纯函数用例
  根本碰不到。修法分别是把断言钉在**具体那一行的值**上、以及把计数抽成
  `TranscodeScaleState` 让它可以被直接驱动。M6 第一轮是变异脚本自身语法错
  （BUILD-FAILED），改锚点后复测判红。
- 全量构建 / ctest / 离屏启动 / clang-format 18.1.8 / AUTOMOC / 分层 /
  命名空间 / README 快照 / `git diff --check` 见提交记录。

**未覆盖（如实记账）**

- **旋转 / HDR 元数据本版只记账不继承**：产物画面方向可能与源片不同，
  HDR 标记不沿用。要有真实旋转 / HDR 语料验证 muxer 的 `tkhd` 行为才能放开。
- **10-bit 编码器未选型**：仍是统一 8-bit `yuv420p`，位深损失只是「说清楚」，
  不是「补回来」。
- **逐轨转码决策 / GOP 局部重编码 / 黑帧静音重复帧 / 其他容器 / 多候选规划器**
  仍属阶段 6 未完成部分，见 §5.2。
- **动态分辨率端到端语料缺失**：`TranscodeScaleState` 已单测，但没有
  「真实中途变尺寸的 MP4 → 跑完整 S08」的用例；产物里那些帧仍按首帧尺寸
  缩放，不是原样保留。

### 切片 K 验收记录（2026-11-12）

**交付**

- `core/repair/RepairTolerantTranscode.cpp`：输出流创建时捕获 `stream_meta`
  （输出流下标 → kind / codec 名）；收尾上抛 `sample_ledger.Coverage()`，
  `complete` 由 `stop` / 读错 / 写失败 / 资源失败 / 达上限共同判定；
  追加「产物时间轴从零重建，不可与损失区间相减」限制项。
- `core/reporting/RepairReportExporter.cpp`：`available=false` 的兜底文案按
  `sample_ledger.available` 分叉；时基 `unknown`、显示顺序 `analyzed=false`
  各补显式行；逐轨行的「覆盖区间未知」不再与下一行重复。
- `core/repair/RepairSampleScan.cpp`：空账本的 `unavailable_reason` 改为
  「本次执行没有登记任何样本」，不再误称「没有带时间戳的样本」。
- `RepairTypes.h` / `RepairExecutor.h`：同步过时注释。
- `tests/unit/test_repair_engine.cpp`：新增 5 个用例与
  `ConclusionLayerProbe` / `RunConclusionLayer` / `WriteConclusionReports`
  / `ConclusionTextHas` / `ConclusionTextLacks` / `RequireRealCorpus` 辅助。

**验证**

- 5 个用例：`RealCorpusPreserveModeReportsFullConclusionLayer`（healthy + mid 表驱动，
  逐轨覆盖 / 共同区间 / 时基 / 三格式同源）、
  `RealCorpusTailTruncationNeverMarksCoverageComplete`、
  `RealCorpusTranscodePathReportsTrackCoverageWithoutFakeSpans`、
  `CoverageUnavailableReasonDistinguishesMissingLedgerFromMissingStats`（正反双向）、
  `TimelineUnknownAndDisplayOrderUnmeasuredAreExplicitInAllFormats`（正反双向）。
- 判别力 10 组全部精确判红后按字节恢复复跑绿：M1 S08 不上抛覆盖、
  M2 不写时间轴口径、M3 保码流 `complete` 恒 true、M4 S08 `complete` 恒 false、
  M5 兜底恢复硬编码「没有账本」、M6 时基 unknown 整节不出行、
  M7 显示顺序未测量不出行、M8 引擎不上抛、M9 账本不统计无时间戳样本、
  M10 给 S08 样本伪造 PTS 制造假 span。
- `ConclusionTextHas` 的口径：TXT 是 `key: value`、HTML 是 `<th>key</th><td>value</td>`，
  带冒号的整串在 HTML 里永远找不到。要求 TXT 出现完整行、HTML 同时出现 key 与 value，
  既不放松「三格式同源」也不因排版假红。

**未覆盖（如实记账）**

- **S08 仍给不出输入时间轴上的逐轨覆盖区间**：这是本次刻意保留的物理限制，
  不是遗漏。要做需先建立「源包 → 解码帧 → 编码包」映射并验证 B 帧重排与
  解码延迟，否则就是编一段假 span。
- **`elst` / 开放 GOP 前导图像 / VFR+B 帧退回解码序等「明确不恢复项」**
  K 时仍未在报告里逐项显式标注；**切片 L 已落地**（见 §4.12），但只标注口径，
  不实际恢复。
- **参考辅助重建路径（C/H/I）**：K 与 L 当时只钉住保码流与 S08 两条主路径；
  **切片 M 已把它纳入真实语料矩阵**（见 §4.13），并修掉了该链上把借用时基
  写成 `unchanged` 等四处错误结论。

### 切片 L 验收记录（2026-11-12）

**交付**

- `core/domain/model/RepairTypes.h`：`RepairDeclaredExclusion` / `RepairDeclaredExclusions`
  与 `RepairResult.declared_exclusions`。
- `core/repair/RepairDeclaredExclusions.{h,cpp}`：8 条静态能力目录（六维度：
  code / title / applies_to / consequence / evidence / status）+ 纯函数
  `BuildDeclaredExclusions()`；触发只看结构化字段，事实写成 `字段=值`。
- `core/repair/RepairEngine.cpp`：原 `Run` → 匿名命名空间 `RunCore`，新 `Run`
  作为唯一收尾点，覆盖 5 个早退点与主返回点。
- `core/reporting/RepairReportExporter.cpp`：JSON 顶层 `declared_exclusions`；
  TXT/HTML 从 `BuildSections` 恒定输出「本次明确未恢复项」章节。
- `tests/unit/test_repair_declared_exclusions.cpp`（9 用例）、
  `tests/unit/test_repair_engine.cpp`（追加 3 用例）、`tests/CMakeLists.txt`
  （新目标 `RepairDeclaredExclusionsTests`）、`README.md`（快照）。

**验证**

- 全量 `ctest` **172/172**（1938 个 gtest 用例，本片 +12）；离屏
  `QT_QPA_PLATFORM=offscreen timeout 10 ./VideoEye.exe` **rc=124**。
- 门禁全绿：clang-format **18.1.8**（只格式化 7 个改动源文件）、
  `check_automoc_registry.py`（登记 9 / 测试目标 152）、`check_layering.py`、
  `audit_namespace_layout.py`、`audit_link_visibility.py`、
  `summarize_tests.py --update-readme` + `--check-readme`、`git diff --check`。
- 判别力 **13 组全部精确判红**后按字节写回 + `os.utime` 恢复、重建复跑
  9/9 + 3/3 全绿：M1 引擎不上抛、M2 无事实也挂全部模板、M3 零命中静默省略、
  M4 未评估态不出行、M5 JSON 不写 items、M6 未探测当成实测损失、
  M7 `mode=unknown` 无执行也挂免责声明、M8 预检早退闸门失效、
  M9 TXT/HTML 不输出章节、M10 目录少一条、M11 状态被抬成 `recovered`、
  M12 触发事实退化成自由文本、M13 S08 专属项串到保码流路径。
  M10 同时被「目录覆盖守护 / 触发矩阵 / 事实结构化 / 三格式同源」四组用例抓到。

**两个实测踩到的坑**

1. `RepairTimelineBasis::mode` 默认就是 `"unknown"`。第一版把它单独当触发事实，
   结果**连请求预检失败**都会挂上「VFR 逐帧时长不恢复」—— 正是「无事实却宣称
   不支持」。修法：`vfr_declared=true`（源侧证据）单独成立；`mode=unknown`
   只在本次有 `attempted=true` 的执行时才成立，另加「诊断 / 执行之前结束 →
   零条」的总闸门，并为闸门配 `PreRepairEarlyExitDoesNotCarryGlobalDisclaimers`
   守护（否则又是一个「坏了看不出来」的检查）。
2. 收尾**不能用 RAII 守卫的析构**：`return result` 一旦发生 move，析构里改的是
   已被搬走的局部对象，字段静默丢失。改成「唯一返回点的包装函数」。
   另外变异脚本在 Windows Python 里 `shell=True` 走 `cmd.exe`，不认
   `source msvc_env.sh`；显式指定 Git Bash 才跑得动。

**未覆盖（如实记账）**

- 8 条排除项**只标注口径，不实际恢复**：仍不重建 `elst`、不继承旋转 / HDR、
  不补 10-bit、不插黑帧 / 静音 / 重复帧、不做分片重同步与 GOP 局部重编码。
- **参考辅助重建路径（C/H/I）已由切片 M 纳入真实语料矩阵**（见 §4.13）；
  S08 输入侧时间轴覆盖区间仍不给（需先做源包 → 解码帧 → 编码包映射并验证 B 帧重排）。
- 逐轨转码决策 / 其他容器 / 多候选规划器仍属 §5.2 未完成部分。

### 切片 M 验收记录（2026-11-13）

**范围**：§4.13。真实语料 `real_h264_aac_missing_moov.mp4`（由 `real_h264_aac_faststart.mp4`
去 moov 派生，天然同源）+ 同源参考，走 `container.mdat_reference_assisted`。

- 摸底实测（临时探针，落地前删除）一次打出 4 处自相矛盾：`timeline_status=unchanged`、
  `reference_compatibility=not_applicable`、`perceptual_fidelity=unknown_without_reference`、
  时基依据渲染成「容器时间戳」。四处全部修掉。
- 能力契约自证：R03 的注释写着 `timeline_status` 只能是 `modified` / `unknown`，
  代码却写 `unchanged` —— 这条红线原先只钉 fMP4 初始化段借用。
- 新增 5 个用例：真实语料三格式同源（36 条断言）、未测量态显式出行、主路径反向守护、
  不相干参考不得声称 `verified`、目录纯函数正反表驱动。
- 验证：172/172 ctest（1943 gtest）、离屏 `rc=124`、clang-format 18.1.8、
  AUTOMOC / 分层 / 命名空间 / 链接可见性 / README 快照（153·153·1943·19）/
  `git diff --check` 全绿。
- 判别力 **11 组全部精确判红**后按字节恢复 + `os.utime` 复跑 64/10/15/36 全绿：
  M1 时基口径退回 `container_pts`、M2 参考辅助不算作时基无证据、M3 借来的时基仍写
  `unchanged`、M4 参考兼容性不上抛、M5 借用参数不上抛、M6 有参考却说没有参考、
  M7 时间轴说明不透传参考借用、M8 报告不出时基证据边界、M9 时基依据显示成容器时间戳、
  M10 声明目录丢 `reference_borrowed` 事实、M11 画质限制文案退回「需要参考样片」。

**首轮判绿的两组（覆盖缺口，已修）**

1. M5：只覆盖了「恢复区间」里的 `reference_assist.borrowed_parameters`，保真度章节
   不上抛也照样绿 → 补「借用参数: ftyp 兼容 brands（原样）」断言。
2. M7：原变异打的是 `if (assist.compatibility != "verified")`，实测该分支**不可达**
   （`used == true` ⇒ 兼容性由重建器唯一写入点保证 `verified`），属假覆盖 →
   删除分支并留注释；改为变异「时间轴说明不透传参考借用」，同时补该句断言。

**未覆盖（如实记账）**

- 借来的时基**仍是借的**：没有做「用参考片反推源片时间轴」的独立核验，
  报告只声明 `unknown` + 证据边界。
- 真实语料矩阵仍只有保码流 / S08 / C-H-I 三条路径；`reference_init`（fMP4 初始化段）
  路径的端到端用例**已由切片 N 补上**（见 §4.14），M 当时确实没有。
- S08 输入侧时间轴覆盖区间仍不给；逐轨转码决策 / 其他容器 / 多候选规划器仍属 §5.2。

### 切片 N 验收记录（2026-11-13）

**范围**：§4.14。真实语料 `real_h264_aac_frag_orphan.mp4`（由 `real_h264_aac_frag.mp4`
删 `ftyp` + `moov` 派生，天然同源）+ 同源参考，走 `container.init_segment_missing`
→ 借参考初始化段 → 重封装。

- 摸底实测（临时探针，落地前删除）打出 3 处「无事实却给结论」：时基依据渲染成
  「容器时间戳」（保真度章节却写着借用 / unknown）、`时间轴实测: 产物实测=0.000s`
  （同一份报告逐轨实测 1.221 s）、恢复比例基线写成「参考初始化段声明的逐轨样本数」
  （参考初始化段里没有样本数，基线来自源侧 `moof/trun (2 segments)`）。三处全部修掉。
- 新增 `timeline_basis.mode = "reference_init_borrowed"`，刻意与 C/H/I 的
  `reference_borrowed` 分开：前者只借 timescale + 解码配置（逐样本时长仍来自 trun），
  后者借整张 stts。合成成一个 mode 会让 `vfr_frame_durations_not_recovered`
  这条排除项的挂载依据失真。
- 新增 1 个用例：`RealCorpusInitSegmentBorrowedTimebaseIsExplicitInAllFormats`；
  `MainPathsDoNotLeakReferenceBorrowedConclusions` 加 4 条反向断言。
- 验证：172/172 ctest（1944 gtest）、离屏 `rc=124`、clang-format **18.1.8**、
  AUTOMOC（9 / 152）/ 分层 / 命名空间 / 链接可见性 / README 快照（153·153·1944·19）/
  `git diff --check` 全绿。
- 判别力 4 组全部精确判红后按字节恢复 + `os.utime` 复跑 54/54 全绿：
  M1 不占住时基（回落 `container_pts`）、M2 报告层渲染成「容器时间戳」、
  M3 产物实测时长退回「只在可比对时记」、M4 基线来源文案退回「参考初始化段声明」。

**未覆盖（如实记账）**

- 借来的 timescale **仍然是借的**：没有做「用参考初始化段反推源片时间轴」的
  独立核验（源侧 sidx 时间基为 0 时根本没有可比对的基线）。
- 合成初始化段（`synthesized`）路径的时基证据分级没改：它的 timescale 来自源侧
  sidx / 分片声明，仍走 `container_pts`，未单列证据边界。
- 能力目录里 `parameter_set_recovery` 仍标 `unsupported`，而这条路正是靠借参数集
  恢复成功的。文案已说明「unsupported 指尚未覆盖的范围」，但状态词与「本次命中」
  并排仍容易被读成「这次没做到」——要改需要一整套「部分支持」的状态词表，
  牵动 `RepairCapabilityBoundary.status` 的既有守护，本轮不动。
  **（已在切片 R 处理：状态词表 + `covered_scope` / `not_covered_scope` 三段范围，见 §4.18）**

### 切片 O 验收记录（2026-11-13）

**范围**：§4.15。走查 §5.2 剩余能力时撞到的**口径错误**（不是能力缺口）：
`cmake/ffmpeg-version.json` 锁定的是 BtbN `win64-lgpl-shared` 变体
（`--disable-libx264 --disable-libx265`），`avcodec_find_encoder_by_name("libx264")`
恒为 null，S08 兜底后实际用 **libopenh264**（运行日志逐帧打印
`layerId(0) doesn't support profile(578), change to UNSPECIFIC profile`）。
而契约 / 排除项 / 路线图三处写的是 libx264，同一份报告的 `fidelity.limitations`
却写「编码器=libopenh264」。

- 摸底探针（临时 TEST，落地前删除）实测输出：
  `used=[libopenh264] pref=[libx264] fb=1 src_prof=[Constrained Baseline] out_prof=[]`
  —— 产物侧 `avctx->profile` 未回填，因此 profile 比对只能如实写「未能比对」。
- 新增字段 `encoder_preferred / encoder_used / encoder_fallback /
  source_profile / output_profile` + 一次渲染好的 `encoder_note / profile_note`；
  报告行、限制项、JSON 全读同一份文案。
- 编码器事实与 `inspected` 解耦：没探测到首帧的路径上编码器也打开过，事实不能丢。
- 新增 5 个用例（4 个保真度 + 1 个静态文案守护）；172/172 ctest（1949 gtest）、
  离屏 `rc=124`、门禁全绿（clang-format 18.1.8 / AUTOMOC 9·152 / 分层 / 命名空间 /
  链接可见性 / README 快照 153·153·1949·19）/ `git diff --check`。
- 判别力 5 组全部精确判红后按字节恢复复跑全绿：
  M1 回退时用首选名渲染、M2 产物 profile 缺实测却写「一致」、M3 契约文案退回
  「libx264 重编码」、M4 报告层另抄首选名、M5 回退事实不进限制项。
  ❗ M4 首轮判绿：TXT 的限制项里也有一条以「重编码编码器: 」开头的文本，
  裸 `find` 被它蒙过去；改成钉 `"\n重编码编码器: <note>\n"`（行级）后转红。
  ❗ M5 首轮变异写成了「删掉整个 if 块」，编译期就报 C2888/C1075 —— 变异必须是
  可编译的语义破坏，否则测的是构建失败不是判别力。

**未覆盖（如实记账）**

- 产物侧 profile 仍取不到（libopenh264 不回填 `avctx->profile`），报告写
  「未能比对」；要拿到真实产物 profile 得解析产物 SPS，本轮不做。
- 不涉及「换 GPL 变体 / 引入 libx265」：那是许可证口径决策，10-bit 也卡在这里。
- 编码器回退**不影响**已有的 PSNR 结论口径，但 PSNR 值现在来自 openh264 而非 x264，
  同一码率下的数值不能与历史 x264 产物直接横比。

### 切片 P 验收记录（2026-11-13）

**范围**：§4.16。做切片 O 的端到端用例时顺手量了产物两条轨的起点，撞到的**结论层
说谎**：源片 `video_start=0.480 / audio_start=0.000`，产物变成 `0 / 0`，报告不提。
旧实现在代码里就不成立 —— 视频 PTS 直接用帧序号（永远从 0 开始），音频逐轨减自己
首包 PTS，两条轨互不知情，轨间相对关系必然丢。

- 新增字段 `av_start_offset_seconds / av_start_offset_preserved / av_sync_note`；
  报告落「音画起点差」「音画同步处理」两行（TXT/HTML 同源），JSON 落机读字段。
- 改为**统一原点**：先取每条轨的源侧呈现起点，取最小者作原点，所有轨减同一个原点。
  视频按 1/fps 取整并夹到帧，音频按 `av_rescale_q` 换算成各轨时基。
- 起点取值优先级：`stream->start_time`（含 `elst` 补偿）＞ 首帧 PTS ＞ 首包 PTS；
  负起点一律夹到 0。
- 新增 3 个用例（22 个保真度用例全绿）；172/172 ctest（1952 gtest）、离屏 `rc=124`、
  门禁全绿（clang-format 18.1.8 / AUTOMOC 9·152 / 分层 / 命名空间 / 链接可见性 /
  README 快照 153·153·1952·19）/ `git diff --check`。
- 实测（探针，落地前删除）：
  `源 video_start=0.4800 audio_start=0.0000 → 产物 video_start=0.4800 audio_start=0.0000`，
  `av_sync_note` = 「源侧音画起点差 0.48（视频较晚）；产物按统一原点平移，相对关系保留」。

**判别力变异**

| 变异 | 破坏点 | 结果 |
|---|---|---|
| P1 | 视频退回 `pts = encoded_frames`（不加原点偏移） | 精确判红 `S08PreservesAudioVideoStartOffset` |
| P2 | 锚点忽略 `stream->start_time`（退回首帧 PTS） | **判绿（不可判别）** |
| P3 | 负起点不夹 0（priming 被当成真实起点） | 精确判红 `S08DoesNotInventAvOffsetWhenSourceHasNone` |

❗ **P2 判绿如实记账，不粉饰**：现有语料里 `stream->start_time` 与首帧 PTS 只差
0.0061 s（`6144 ticks`），折算到 25 fps 是 0.15 帧，被 `llround` 吞成 0 帧，因此
无可观察差异。要让这条分支可判别，得构造**带 `elst` 的语料**（容器声明起点与首包
PTS 相差 ≥ 半帧），本轮未做 —— `avformat` 没有公开的「写 `elst`」API，只能靠
`-ss` 重封装或手工插盒。该分支保留：真实容器（iTunes 风格带编辑列表的 mp4）会让它
生效，删掉是把已知的正确行为交给运气。**它的定位是「已知未覆盖」，不是「已验证」**。

**未覆盖（如实记账）**

- 不重建 `elst`：本轮只保留轨间相对关系，不还原编辑列表语义；切片 L 的
  「`elst` 本次未恢复」标注继续成立。
- 帧级漂移（不是起点差）不在本轮范围：要靠阶段 5 的时间轴工作（§5.1）。
  报告用的是「音画**起点**差」，文案刻意没写「音画同步已保持」。
- 视频侧偏移量按 1/fps 取整 → 源侧起点若落在半帧以内（如 0.0061 s），产物会归到
  整数帧边界。这不影响本轮的判据（起点差），但不能拿产物当「逐帧时间戳无损」的证据。

### 切片 Q 验收记录（2026-11-13）

**范围**：§4.17。§5.2 里挂了很久的「旋转 displaymatrix 与 HDR 静态元数据的实际继承」。
摸底发现旧实现的问题不是「缺能力」，而是**从不看产物**：

```cpp
if (fidelity.has_display_matrix)
    fidelity.lost_properties.push_back("画面旋转 / 镜像矩阵（…°）未继承：…");
```

这句是从「我们没拷 side data」推的。产物侧一次没打开过 —— 与 M / N / O / P 同一类错误。

**实测（探针，落地前删除）**

```
旧实现  源 has=1 deg=-90 / mastering=1 cll=1
        产物 has=0 deg=0   / （无）
拷贝后  产物 has=1 deg=-90 / mastering=1 cll=1
        attach_m=1 attach_hdr=1 out_has_m=1 out_has_hdr=1 measured=1
```

即 `movenc` 确实会从输出流 `codecpar->coded_side_data` 写 `tkhd` 矩阵与 `mdcv` / `clli`
—— 这是实测出来的，不是查文档推断的。两处 `avformat_write_header` 之前的拷贝是关键。

**做法**

- 拷贝（写头前）：矩阵 + 两份 HDR side data 挂到输出视频流 `codecpar`；
  记 `display_matrix_attached` / `hdr_metadata_attached`。
- 实测（trailer + 关输入后）：重新打开产物，逐项读回它自己声明的元数据；
  记 `output_*` / `side_data_output_measured`。只在写成功时测（残缺产物打不开，
  不该被读成「缺少方向信息」）。
- 结论（`FinalizeSideDataInheritance`，唯一入口）：一致 → preserved；角度不同 →
  lost「角度已改变」；产物无 → lost「产物实测无方向信息」；**未实测 → unverified**。
- 引入第三类清单 `unverified_properties`：「没查」不能挤进 lost 或 preserved。
  `all_properties_preserved` 因此分两档 —— `InspectTranscodeFidelity` /
  `FinalizeTranscodeFidelity` 用保守式（源侧还有待验证 side data 就恒 false），
  只有拿到产物实测才抬回 true。
- HDR 加**位深前提**（`TranscodeHdrInheritable`）+ **矛盾检测**（决定不拷、产物却带着
  → 报异常）；「主动不沿用」与「muxer 丢了」文案分开。
- 报告：「旋转 / 镜像矩阵」「HDR 静态元数据」两行读修复层渲染的 note；
  「源侧属性结论」追「，N 项未实测」；items 追「转码属性未实测: …」；JSON 落 10 个机读字段。

**验证**：172/172 ctest（1958 gtest）、离屏 `rc=124`、门禁全绿（clang-format 18.1.8 /
AUTOMOC 9·152 / 分层 / 命名空间 / 链接可见性 / README 快照 153·153·1958·19）/
`git diff --check`。新增 6 个用例（4 个纯函数 + 2 个端到端 + 位深四态）。

**判别力变异**

| 变异 | 破坏点 | 结果 |
|---|---|---|
| Q1 | 不挂矩阵 side data（退回旧行为） | 精确判红 `S08InheritsRotationMatrixAndVerifiesInOutput` |
| Q2 | 不做产物实测（measured 恒 false） | 精确判红旋转 / HDR / 无 side data 三条 |
| Q3 | 有矩阵就算保留（不看角度） | 精确判红 `RotationAngleChangeIsNotSilentlyTreatedAsPreserved` |
| Q4 | 未实测写成「丢失」 | 精确判红 `RotationMatrixWithoutOutputEvidenceStaysUnverified` |
| Q5 | 保守结论退回（去掉待验证条件） | 精确判红同一条 |
| Q6 | 位深前提恒 true | 精确判红 `HdrInheritanceRequiresSameBitDepth` |
| Q7 | 矛盾检测失效 | 精确判红同一条 |

❗ **变异脚本的 mtime 坑（本轮新踩）**：`os.utime(p, None)` 只把 mtime 设成「当前」，
同一秒内连续两次改动会让 ninja 认为文件没变而**跳过重编** —— Q7 首轮就是跑在 Q6 的
二进制上，报出的红是假的。修法是 `os.utime(p, (now + 2, now + 2))` 保证严格递增；
并且每组变异都要确认「构建真的编译过」（`grep -c "Building CXX"` > 0）。

**未覆盖（如实记账）**

- **10 → 8 的端到端语料没有**：本构建唯一支持 10-bit 的编码器是 `ffv1`
  （`yuv420p10le / yuv422p10le / yuv444p10le`），而 H.264 只有 `libopenh264`
  （`pix_fmts` 仅 `yuv420p / yuvj420p`）。要造「10-bit + HDR + S08 可接受」的 mp4 样本，
  得先给 `TestMediaFactory` 加 10-bit 源支持。本轮用纯函数四态 + 矛盾检测兜住，
  但**S08 侧那句 `hdr_inheritable` 判断本身没有端到端守护**（8-bit 语料上恒为 true）。
- HDR 元数据只在 `codecpar` 侧拷贝；若某容器的 HDR 元数据只挂在**解码帧** side data 上，
  本版不会拷（矩阵侧有帧兜底，HDR 侧没有）。会如实报「产物实测无」。
- 不碰动态 HDR（逐帧 SEI / RPU）与 10-bit 编码，理由同 §4.17「明确不做」。

### 切片 R 验收记录（2026-11-13）

**范围**：§4.18。走查「能力边界」章节时撞到的口径错误（不是能力缺口）。

**旧状态（自相矛盾）**

| 契约 | 状态词 | 自己的 evidence 写着 |
|---|---|---|
| `moov_rebuild` | unsupported | 「当前版本已实现受限版无参考重建（RepairPlainMp4Rebuilder）」 |
| `parameter_set_recovery` | unsupported | 「当前版本已能借同源初始化段…或由 in-band SPS/PPS(VPS) 自建」 |
| `decodable_gop_transcode` | unsupported | 「S08_TolerantTranscode 已实现并可运行」 |

再加上章节标题「**未实现**能力边界（本次命中）」，读者只看这两处就会把
「这次靠借参数集修好了」读成「这次没做到」。

**根因**：`status` 混装了两件事 —— 本版本的实现程度、以及读者以为的「本次是否生效」；
而「做到哪 / 没做哪 / 做不了哪」三段全压在 evidence 一段散文里，于是状态词只能靠散文
自辩（原文：「本契约的 unsupported 指的是尚未覆盖的范围」）。这与切片 Q 的
`unverified_properties` 是同一个结构性问题：缺失的维度会被挤进已有的桶里。

**做法**

- 状态词表唯一事实源 `CapabilityStatusVocabulary()`：`partially_supported` /
  `unsupported` / `deferred`；词表外取值渲染成「未知状态（x）：不能据此判断支持程度」。
- 三段范围拆开：`covered_scope`（已实现）/ `not_covered_scope`（没做但做得到，新增）/
  `unrecoverable_scope`（任何实现都拿不回来，语义收窄为纯物理边界）。
- 状态词与两张表**互为约束**（用例守护，不只是文档约定）：文案声称已实现 ⇒ 不许
  `unsupported`；`partially_supported` ⇒ `covered_scope` 非空；同一句文案不许同时
  出现在两张表；词义与内容矛盾时按更保守一侧渲染（「应按未支持解读」/「不能读成
  本版本无实现」）。
- 渲染一次（`CapabilityStatusLabel` / `CapabilityStatusHeadline`）→ 落进
  `status_label` / `status_headline`，报告层只打印。理由有二：`core/reporting` 不链
  `core/repair`（分层规定 repair 只产出 domain 结果），且第二处拼文案必然与第一处漂移
  —— 与切片 O/P 的 `encoder_note` / `av_sync_note` 同一套口径。
- 标题改为「恢复能力边界（本次命中）」，加一行「状态口径」：状态词描述本版本实现程度，
  不代表本次运行是否生效；`RepairInspector` 的 summary 同步从「命中 N 条未实现能力边界」
  改为「命中 N 条能力边界契约（…不等于本次未生效）」。

**实际渲染**（TXT，`moov_rebuild` 命中）

```
恢复能力边界（本次命中）
命中条数: 1
状态口径: 状态词描述本版本对这项能力的实现程度，不代表本次运行是否生效；…
- moov_rebuild [部分支持] 触发证据: container.moov_missing
-   支持程度: 部分支持：本版本已实现 2 项（见「已覆盖范围」），另有 3 项未覆盖（见「未覆盖范围」）；本条描述能力整体，不代表本次运行已生效
-   已覆盖范围: 无参考受限重建：ftyp + 恰好一个 mdat + 单轨，且载荷为纯 AVCC H.264 或纯 ADTS AAC（RepairPlainMp4Rebuilder）
-   未覆盖范围: 音视频交织的 mdat、多个 mdat、多轨并行
-   不可恢复范围: mdat 本身被截断、覆盖或加密的部分：没有样本边界信息就切不出访问单元
```

**验证**：172/172 ctest（1960 gtest）、离屏 `rc=124`、门禁全绿（clang-format 18.1.8 /
AUTOMOC 9·152 / 分层 / 命名空间 / 链接可见性 / README 快照 153·153·1960·19）/
`git diff --check`。新增 3 个用例（`CapabilityStatusWordMatchesImplementedScope`、
`UnknownCapabilityStatusIsNeverReadAsSupported`，并扩写 `CapabilityBoundariesAreFullyDefined`
与 `CapabilityBoundariesAreVisibleInAllReportFormats`）。

**判别力变异**（`build/mut_r.py`，每组都确认构建真的编译过）

| 变异 | 破坏点 | 结果 |
|---|---|---|
| R1 | `parameter_set_recovery` 状态词退回 `unsupported` | 精确判红 `CapabilityStatusWordMatchesImplementedScope` |
| R2 | 报告层甩英文状态码（不读渲染好的标签） | 精确判红 `CapabilityBoundariesAreVisibleInAllReportFormats` |
| R3 | 章节标题退回「未实现能力边界」 | 精确判红同一条 |
| R4 | 词表外取值渲染成「支持」 | 精确判红 `UnknownCapabilityStatusIsNeverReadAsSupported` |
| R5 | 总述去掉「不代表本次运行已生效」 | 精确判红 2 条（状态词守护 + 报告可见性） |
| R6 | 不渲染总述（报告落到「未提供支持程度说明」） | 精确判红 3 条 |

**已知未覆盖（如实记账）**

- **没有「本次是否生效」的逐次标注**：`RepairResult` 里没有「这条能力本次被用过」的事实
  （`plan.actions` 只记不可用策略的契约引用），所以报告只能说「这条能力部分支持」，
  不能说「本次用的就是它」。要补得把执行层的实际动作回写进契约，属另一轮工作。
- `deferred` 当前无实例，留在词表里避免将来临时加词绕过守护。

### 切片 S 验收记录（2026-11-13）

**范围**：§4.19。§4.14 留下的「合成初始化段（`synthesized`）路径的时基证据分级没改」。

**摸底**（真实语料 `fmp4_synth`，视频轨 + global_sidx + in-band SPS/PPS）

| 报告位置 | 修复前 | 修复后 |
|---|---|---|
| 时基依据 | 容器时间戳 | 自建初始化段时基（源侧 sidx 声明，无原始 mdhd 可核对） |
| 时基证据边界 | （没有这一行） | 源侧 moov 已丢失；mdhd timescale 由自建初始化段按源侧 sidx 声明选定，不是源容器直接给出 |
| 时间轴变化 | unchanged | unknown（tick 原样来自 moof/trun，但 tick→秒依赖自建时基） |
| 时基核对 | 「sidx timescale=12800」一句话 | 偏差 ≤ 0.000 s / 容差 0.250 s，并注明「同比例整体改写发现不了」 |

**三处「无事实却给结论」**

1. 源片没有 moov，「容器时间戳」里的那个容器是我们自己写的 —— 把选定的时基说成源侧事实。
2. `unchanged` 声称时间轴与源片一致，而秒数依赖一条声明；原始 mdhd 已丢失，没有可比基线。
3. 「源侧估算 = 产物实测 / 可比对=yes」在这条路是**自证** —— 两侧同一个 sidx timescale 换算。

**做法**：`timescale_crosschecked` + 最坏偏差 + 最严容差三段实测；`TimescaleCrosscheckText()`
唯一渲染入口；`mode = synthesized_init_timescale`；`timeline_unknown` 纳入 `synthesized_init`；
报告出「时基证据边界」行 + 「时基结论」；`timeline_comparable` 时另加一条「这是自证」的限制。

**验证**：172/172 ctest（1961 gtest）、离屏 `rc=124`、门禁全绿（clang-format 18.1.8 /
AUTOMOC 9·152 / 分层 / 命名空间 / 链接可见性 / README 快照 153·153·1961·19）/
`git diff --check`。新增 1 个纯函数用例 + 扩写 `FragmentedSegmentsSynthesizeInitFromInBandParameterSets`
（原断言 `timeline_status == "unchanged"` 编码的正是旧口径，已随行为一起改掉）。

**判别力变异**（`build/mut_s.py`，每组都确认 `Building CXX` > 0）

| 变异 | 破坏点 | 结果 |
|---|---|---|
| S1 | 自建路径回落到 `container_pts` | 精确判红（mode + 反向守护「不许出现容器时间戳」） |
| S2 | `timeline_unknown` 不纳入 `synthesized_init` | 精确判红（`unchanged`） |
| S3 | 报告渲染成「容器时间戳」 | 精确判红 |
| S4 | 证据边界行改成「源侧容器直接给出」 | **首轮判绿** → 断言只钉了行首，补成整行匹配后判红 |
| S5 | 核对没跑也记成「核对通过」 | 精确判红（报告出现「未做事后核对」） |
| S6 | 去掉「时长比对是自证」的限制项 | 精确判红 |
| S7 | 分级渲染把「没核对」当成「核对通过」 | 精确判红（纯函数用例） |

❗ **S4 的教训**：断言 `text.find("时基证据边界: 源侧 moov 已丢失")` 只钉住行首，
把后半句改成相反的结论照样通过。**钉文案就要钉整行**，尤其是「A 与 B 不同 / 不是 A」
这类靠后半句表达判定的句子。

**未覆盖（如实记账）**

- **整体被同比例改写的时基仍发现不了**：核对的是「trun 时长累加 ≈ sidx 声明时长」，
  两者都从打包器来；若 sidx 的 timescale 与它声明的时长被一起改写，比值不变、核对恒过。
  要发现这种得有一条与 sidx 无关的基线（例如分片内 tfdt 序列），本轮没做。
- **「核对失败 → 拒绝自建」这条分支没有专门语料**：只有一个「没有 sidx」的语料
  （`FragmentedSegmentsWithoutSidxStillNeedReference`）。造「trun 与 sidx 时基不一致」
  的样本得手工改一处 trun，本轮未造 —— 该分支的判据是 `deviation > tolerance`，
  已有代码但无端到端守护。
- 自建路径仍不重建音频轨，也不重建 edts/ctts，这两条限制沿用 §4.14。

### 切片 T 验收记录（2026-11-13）

**摸底（先量后写）**

| 现象 | 实测结论 |
|---|---|
| 第二视频轨 / 封面 / 字幕 / 数据轨 | `RepairTolerantTranscode` 只给 `video_index` 与音频建输出流，其余流从未被登记；`omitted_items` 里也查不到 |
| 容器拒绝某条音频轨 | `avformat_write_header` 整体失败 → S08 整体失败 → 退回保重封装；旧文案猜「音频编码参数可能不被目标容器接受」，不说是哪条 |
| 「为什么重编码」 | 只有一句「视频轨经过解码与重编码（N 帧）」，没有证据来源 |
| `avformat_query_codec` 能否当判据 | **不能**：mpegts 返回 -1163346256（未实现 `query_codec`），mp4 声称 vorbis / truehd 都支持 |
| 内存试写头能否当判据 | **能**：mp4/truehd 拒、mov/flac 拒、mov/opus 拒、mov/truehd 拒、mp4/aac 与 mp4/flac 接受，与落盘写头逐条一致 |

**做法**：新模块 `RepairTranscodeTracks`（纯决策 + 内存试写头 + 唯一渲染）；S08 先决策
再建输出；被丢轨进「未保留」；主视频轨跳过封面图；报告出「轨道决策」汇总行与覆盖行；
JSON 落决策数组 + 两个计数；声明排除新增 `extra_tracks_not_rebuilt` 并修掉
`gop_local_reencode_not_attempted` 里「逐轨决策未实现」的过期证词。

**验证**：173/173 ctest（1976 gtest）、离屏 `rc=124`、门禁全绿（clang-format 18.1.8 /
AUTOMOC 9·153 / 分层 / 命名空间 / 链接可见性 / README 快照 154·154·1976·19）、
`git diff --check`。新增 14 个用例（`test_repair_transcode_tracks`）+ 1 个声明排除用例；
测试媒体工厂新增 `extra_video_tracks`。

**判别力变异**（`build/mut_t.py`，每组都确认 `Building CXX` > 0）

| 变异 | 破坏点 | 结果 |
|---|---|---|
| T1 | 容器拒绝的音频轨改回「照旧复制」 | 精确判红（`AudioRejectedByContainerIsDroppedNotCopied`） |
| T2 | 封面图被选成主视频轨 | 精确判红（`CoverArtIsNeverThePrimaryVideoTrack`） |
| T3 | 覆盖自证不看下标唯一性 | 精确判红（`DuplicateIndexBreaksCoverageProof`） |
| T4 | 试写头无论成败都判「接受」 | 精确判红（`ContainerProbeMeasuresTheRealMuxer`） |
| T5 | 实测原因（错误码含义）不进结论 | 精确判红（同上） |
| T6 | 被丢弃的轨不进「未保留」清单 | 精确判红（端到端双视频轨） |
| T7 | 决策表被 `InspectTranscodeFidelity` 整份替换后没补回 | 精确判红（两个端到端用例） |
| T8 | 没有损伤证据也写成「该轨有损伤」 | 精确判红（`VideoReasonDistinguishesDamageFromModeChoice`） |
| T9 | 报告把 drop 决策也当决策行打印 | 精确判红（端到端「不许出现 `轨道决策: 流 #1（video`」） |

**本轮踩到的真 bug（都进了结论层）**

1. **决策表被整份替换抹掉**：`outcome.transcode_fidelity = InspectTranscodeFidelity(...)`
   是整份赋值，早写的决策表静默消失 —— 报告里一条都没有。修法是把落账收成一个
   `apply_track_plan()`，替换之后再调一次（不是两处各写一遍赋值）。
2. **`avio_context_free()` 会把指针置 NULL**：用「`avio == nullptr`」判「缓冲没被接管」
   导致对同一块 buffer double free（`0xc0000374`）。所有权必须在
   `avio_alloc_context` 成功后立刻转移。
3. **`transcode_note` 分隔符悬空**：决策汇总早写时 `transcode_note` 还是空的，
   渲染成「；轨道决策=…」。汇总挪到收尾统计处。

**未覆盖（如实记账）**

- 「容器拒绝音频轨 → 产物仍生成且该轨被剔除」没有端到端语料：本构建造得出的真实源
  （mp4 / mkv + aac / flac / opus）在目标容器里都被接受。当前只有纯函数 + 容器实测两层覆盖。
- 音频只有 copy / drop，没有「转码成容器能收的编码」这条链（需要音频重编码 + 重采样，
  且要处理 priming 与音画起点，属独立切片）。
- 第二视频轨的**重建**没做：多视频轨需要并行的解码-重编码链与各自参数集管理。

### 切片 U 验收记录（2026-11-14）

**范围与约束**（§4.21）：只覆盖「相邻源解码帧之间存在整帧空洞」这一种形态，且只在
S08 转码链（救内容模式）里、由用户**显式启用**时才插帧。合成内容单独记账、不进恢复率
分子、报告必须标明产物含合成内容。静音 / 重复帧填补不在本轮范围（音轨只有 copy / drop）。

- **验证**：174/174 ctest（1994 gtest）、离屏启动 rc=124、四门禁 + `git diff --check`
  全绿、README 快照 155 · 155 · 1994 · 19。新增 18 个用例（12 纯函数 + 5 端到端 +
  1 端到端反向守护）。
- **判别力变异 U1–U10 全部精确判红后按字节恢复复跑绿**：

| 变异 | 破坏点 | 结果 |
|---|---|---|
| U1 | 用户没启用也照插黑帧 | 精确判红（`DisabledFillStillRecordsTheGap`） |
| U2 | 容差阈值 1.5 → 1.0 帧 | 精确判红（`IntervalWithinToleranceIsNotAGap`） |
| U3 | 整数倍校验容差 0.12 → 10 | 精确判红（`NonIntegralIntervalIsRefusedEvenWhenEnabled`） |
| U4 | 单处上限失效（×100） | 精确判红（`LargeGapIsRefusedNotSilentlyTruncated`） |
| U5 | 合成帧记进样本账本 | 精确判红（账本 121 vs 109） |
| U6 | 恢复比例不扣合成时长 | 精确判红（比例被抬到 1.0） |
| U7 | 探测标志不置位 | 精确判红（`GapIsDetectedAndReportedEvenWhenFillIsOff`） |
| U8 | 报告不打印逐处空洞 | 精确判红（TXT/HTML 里找不到「已插入」） |
| U9 | 没有空洞也记一条 | 精确判红（`CleanSourceHasNoGapRecords`：clean 源凭空 18 处） |
| U10 | 帧率证据修复回退（`avg` 优先） | 精确判红（理由码变 `black_fill_not_integral`） |

**本轮踩到的真 bug（都进了结论层）**

1. **空洞被静默压掉**：S08 的 PTS 是帧序号重建的，源时间戳只用来算一次统一原点 ——
   中间缺一段就变成「产物更短、音频不变」，音画越拖越偏，报告一个字不提。
2. **`avg_frame_rate` 不能当 CFR 帧率**：它是「帧数 ÷ 总时长」，空洞把分母撑大。
   实测 25 fps、40 帧、空 12 帧的源：`avg = 1000/51`（19.6）而 `r = 25/1`。
   按 19.6 重编码等于每帧多算 12 ms，整条视频被拉慢 30%。
3. **S08 从不写 `source_duration_seconds`**：于是转码产物永远只报「缺少源侧时长基线，
   未给出恢复比例」—— 空着不是「没问题」，是结论缺失；而「填补不许抬高恢复比例」
   这条也只有在有基线时才可验证。
4. **`av_image_fill_black` 的行宽是 `ptrdiff_t[4]`**，`AVFrame::linesize` 是 `int[8]`，
   64 位下不能隐式转换（C2664），必须摊平一次。

**未覆盖（如实记账）**

- 静音填补 / 重复帧填补未实现：补静音必须先有「音频转码成容器能收的编码」这条链。
- 填补不覆盖轨道起点之前的空白与整段轨道缺失。
- 端到端的空洞是语料工厂用 PTS 位移造出来的（`video_gap_after_frame` /
  `video_gap_frames`），容器层表现与真实丢包一致，但没拿真实损坏文件验证过。
- 语料工厂的最后一个包解不出帧（clean 语料同样 20 包 / 19 帧）是既有行为，
  本轮不动它，只在断言里用实测帧数做基线并写清原因。

### 切片 V 验收记录（2026-11-14）

**范围与约束**（§4.22）：只解决「原编码被目标容器拒绝」这一种触发条件。目标编码由
两次实测筛出（编码器能按该参数开起来 + 该编码参数能过目标容器的试写头），不是查表。
一个候选都过不了才 drop，并逐条报出被拒原因。

- **验证**：175/175 ctest（2009 gtest）、离屏启动 rc=124、四门禁 + `git diff --check`
  全绿、README 快照 156 · 156 · 2009 · 19。新增 14 个用例（8 纯函数 + 6 端到端），
  另更新 3 个既有用例（切片 T 的音频决策语义变了、声明排除的触发矩阵一分为二）。
- **判别力变异 W1–W10 全部精确判红后按字节恢复复跑绿**：

| 变异 | 破坏点 | 结果 |
|---|---|---|
| W1 | 编码器不支持源采样率也照用 | 精确判红（`SettingsResampleAndSaySo`） |
| W2 | 无视编码器声道能力 | 精确判红（`SettingsDownmixWhenEncoderRejectsChannels`） |
| W3 | 跳过容器试写头（参数能开 = 容器能收） | 精确判红（`NoCandidateFitsReportsEveryAttempt` 变成「找到了」） |
| W4 | 决策层无视实测目标，拒绝就 drop | 精确判红（决策 + 端到端音轨消失） |
| W5 | 无视用户关闭，照样重编码 | 精确判红（`audio_drop_reencode_disabled` 不再出现） |
| W6 | 首帧探测期间不缓存重编码轨的包 | 精确判红（产物音频起点 0.58 s） |
| W7 | 不排空音频链（四层缓冲） | 精确判红（产物音频短 0.047 s） |
| W8 | 重编码了仍报「音频原样复制」 | 精确判红（`audio_reencoded=false`） |
| W9 | JSON 不落 `target_codec` | 精确判红（报告用例） |
| W10 | 决策文本不写「重编码成什么」 | 精确判红（文本断言 + 报告用例） |

**本轮踩到的真 bug（都进了结论层）**

1. **首帧探测期间的音频包对重编码轨是直接丢掉的**：缓存条件当时写的是
   `track_action == "copy"`，转码轨既没进缓冲、也没被写出。实测后果是产物音频
   从 0.58 s 才开始（正好等于探测消耗的时长），而整份产物仍报成功 —— 这类
   「开头少一段」的产物播放器照样能播，只有量时长才看得出来。
2. **四层缓冲不排空会让结尾少一截**：解码器 / swr / `AVAudioFifo` / 编码器各有一层
   缓冲，末帧还不足一个编码块（aac 1024 采样）。漏掉 flush 时产物短 0.047 s（两帧），
   同样不影响「能播」。用例的时长容差因此收到 5 ms —— 用 0.12 s 等于没测。
3. **`libopus` 在 44100 下直接开不起来**（只接受 48 kHz）：不做采样率协商就等于
   「候选看起来可用、实际全部 open 失败」。参数选择必须按编码器声明的能力走。
4. **`swresample` 没链进 `VideoEyeRepair`**：静态库的 PRIVATE 依赖不外传，调用方
   全部 LNK2019（`swr_init` / `swr_convert` / `swr_get_out_samples`），必须挂 PUBLIC。

**未覆盖（如实记账）**

- 端到端只能用测试注入把容器接受性判成拒绝 —— 本构建造不出「源容器能装、目标容器
  不收」的真实语料（mov 写不进 flac / opus）。注入只改这一个判定，且会在
  `container_fit_detail` 里留下痕迹。
- 重编码是有损的，采样率 / 声道可能被迫改变；报告只写「变成了什么」，不评估听感。
- 静音填补仍未实现：`timeline_gaps > 0` 时按源位置保留空洞并计数，明确不补。
- 音频轨的 `audio_reencoded=true` 目前只表示「至少有一条轨被重编码」，不区分是哪一条
  —— 逐轨事实在 `track_decisions[].target_codec` 与转码说明里。

### 切片 W 验收记录（2026-11-14）

**范围与约束**（§4.23）：只覆盖「同一轨相邻音频帧之间的采样空洞」这一种形态，且只在
S08 转码链（救内容模式）里、由用户**显式启用**时才补。判据容差是**半个编码块**
（AAC 1024 → 512 采样），因为容器时基取整（TS 的 1/90000 落在 44100 上）只抖 ±1 采样，
按抖动补静音等于往产物里灌噪声。合成内容单独记账、不进恢复率分子、不进样本账本。

- **验证**：176/176 ctest（2028 gtest）、离屏启动 rc=124、四门禁 + `git diff --check`
  全绿、README 快照 157 · 157 · 2028 · 19。新增 19 个用例（13 纯函数 + 6 端到端），
  另更新 3 个既有文件（声明排除的触发矩阵一分为二、能力契约文案、界面勾选框）。
- **判别力变异 X1–X13 全部精确判红后按字节恢复复跑绿**（标签用 X 不用 W：
`build/mut_v.py` 已占用 `W1–W10`，同号两义会让后来人对不上账）：

| 变异 | 破坏点 | 结果 |
|---|---|---|
| X1 | 容差归零（±1 采样也算洞） | 精确判红（`JitterWithinToleranceIsNotAGap`） |
| X2 | 无视未启用，照样补 | 精确判红（`DisabledFillStillRecordsTheGap` + 2 个端到端） |
| X3 | 码流复制轨也当可补 | 精确判红（`CopyTrackCannotSynthesizeEvenWhenEnabled` + `CopiedTrackReportsGapButCannotSynthesize`） |
| X4 | 无视 `gap_policy` | 精确判红（`MergePolicyWinsOverFillRequest`） |
| X5 | 无视单处上限 | 精确判红（`LargeGapIsRefusedNotPartiallyFilled`） |
| X6 | 无视累计预算 | 精确判红（`TotalBudgetStopsInsertion`） |
| X7 | 判为可补却不真写采样 | 精确判红（`FillInsertsSilenceWithoutInflatingTheLedger` + 报告用例） |
| X8 | 合成包照进样本账本 | 精确判红（`FillInsertsSilenceWithoutInflatingTheLedger`） |
| X9 | 探到空洞但不落账 | 精确判红（3 个端到端） |
| X10 | 认不出「撑大的包时长」形态 | 精确判红（`CopiedTrackReportsGapButCannotSynthesize`） |
| X11 | 未探测渲染成「0 处空洞」 | 精确判红（`UninspectedSummaryDoesNotClaimZeroGaps`） |
| X12 | 理由码重复（复制轨/未启用同码） | 精确判红（`LargeGapIsRefusedNotPartiallyFilled` + `EveryRefusalHasItsOwnReasonCode`） |
| X13 | 未填补的文案写「已补入」 | 精确判红（`RefusalTextNeverClaimsInsertion`） |

**本轮踩到的真 bug（都进了结论层）**

1. **SEH 0xc0000005：在 `avformat_close_input()` 之后仍访问 `in->nb_streams`**。
   逐轨落账循环紧接在关闭输入之后，读的是已释放的 `AVFormatContext`；崩溃点恰好落在
   第一轮循环里的字符串拷贝上，看起来像「std::string 的问题」，实际是悬垂读。
   改成关闭前固化的 `input_stream_count`。**教训**：收尾循环里的每一个 `in->` 都要问
   一句「这时流还开着吗」。
2. **码流复制轨上一个空洞都探不到**：只按「PTS 跳变」判据时，实测 n=33 包
   `start=0.743039 dur=0.301859`（≈13 个编码块，实际只携带 1 块），其余包
   `dur=0.023220` —— **容器把空洞并进了前一个样本的时长**。补上 `inflated` 形态
   （`duration > nominal * 1.5`）之后才看得见。这条直接决定「复制轨到底有没有洞」
   这个结论能不能给，不是实现细节。
3. **探测必须在改 PTS 之前做**：复制轨写包时会按输出时间轴改写 PTS，改完之后原始间隔
   就没了 —— 顺序错了就是「永远探到 0 处空洞」，而且不会报错。
4. **混合块归因要用保守值 −1**：一个编码块里既有真实采样又有合成静音时，块队列与包
   队列可能错位；这时必须判「含合成」把它整块排除在账本外，宁可少记一个块也不能把
   合成采样算进恢复率。

**未覆盖（如实记账）**

- 端到端要跑通重编码轨，必须用测试注入把容器接受性判成拒绝 —— 本构建造不出
  「源容器能装、目标容器不收」的音频语料（mov 写不进 flac / opus）。注入只改接受性
  这一个判定，沿用切片 V 的 `SetTranscodeAudioContainerRejectedForTesting`。
- 语料的空洞是语料工厂**跳过**一段编码帧造出来的（`audio_gap_after_frame` /
  `audio_gap_frames`，不后推 PTS），与真实丢包在容器层的表现一致，但没拿真实损坏文件
  验证过。
- `EncoderRejected` 分支（编码器只接受一部分静音）没有可复现语料，只靠纯函数用例守。
- 样本账本断言只能写「只减不增」：混合块里的真实采样会随合成采样一起被排除，
  最多少记一个编码块 —— 精确相等在当前归因粒度下是不成立的断言。
- **重复帧填补仍未实现**：判据要「同一 PTS 上多个解码帧」或「帧内容与前一帧逐字节
  相同」这类证据，与「补空」完全不同，本切片没有取证。
