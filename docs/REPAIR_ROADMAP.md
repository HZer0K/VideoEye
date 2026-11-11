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

| 切片 | 目标 | 前置 | 状态 |
|---|---|---|---|
| A | 基线收口：序列级守恒 + 名实相符 + 预算 / 损失区间结构化 | — | 未开始 |
| B | 统一 `RecoveredSample` + 分块扫描框架 | A | 完成（未提交） |
| C | 一种录制格式的参考样片辅助 moov 重建 | B | 未开始 |
| D | 索引损坏后的直接扫描与 GOP 恢复 | B | 未开始 |
| 阶段 5 | 时间轴与音画同步恢复 | B + 架构前置（§5.1） | 未开始 |
| 阶段 6 | 容忍式转码、其他容器、多候选规划器 | B + D | 未开始 |

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

- **S08 现状**：写死「视频 libx264 重编码 + 逐条音频轨复制」，无「按轨判断是否需转码」。
  先做轨道级选择，再研究 GOP 范围局部重编码；接入前**必须验证编码参数与拼接边界兼容性**
  （profile / level / 像素格式 / time_base / `max_b_frames`）。
- **待补**：动态分辨率（当前固定首帧宽高）、10-bit、`color_primaries/trc/space/range`、
  HDR 元数据与 SEI、旋转 displaymatrix。补之前产物不能声称视觉无损。
- **黑帧 / 静音 / 重复帧**：目前完全没有，属空能力。接入时由用户**显式选择**并单独记账，
  且不计入恢复率分子；`synthetic_frames_inserted` 恒 false 的现状要同步改掉。
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
5. **合成与原始分开**：补帧 / 静音 / 重复帧单独呈现；`synthetic_*` 目前是空能力，
   字段存在不等于已实现。
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
| 阶段 5 时间轴 / 音画同步 | 部分（显示顺序 / ctts 已落地，分数帧率与音画统一区间映射未做） | | 见 §4.5 |
| 阶段 6 转码 / 其他容器 / 多候选 | 未开始 | | |

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
