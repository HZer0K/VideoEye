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
| A 基线收口 | 完成（未提交） | | 163/163 全绿；新增 8 个用例；判别力 m1/m2/m3 全部精确判红后恢复复跑绿。A-4 按「保留扫描盲区 + 新增损失区间」实现，见 §4.1 |
| B 样本层 + 分块扫描 | 完成（未提交） | | 164/164 全绿；新增 13 个用例（11 单测 + 2 产品）；判别力 m1–m4 全部精确判红后字节写回 + `os.utime` 恢复复跑绿；1 GiB 整段读入已删除；门禁全绿、离屏 rc=124。首次并行全量出现 1 个与修复链无关的既有 flaky（`AudioOutputStabilityTest.StopClearsBacklogAndAllowsReopen` 用假后端指针相等判重开，堆地址复用），单跑与二次全量均绿 |
| C 参考样片辅助 moov 重建 | 未开始 | | |
| D 直接扫描 + GOP 恢复 | 未开始 | | |
| 阶段 5 时间轴 / 音画同步 | 未开始 | | |
| 阶段 6 转码 / 其他容器 / 多候选 | 未开始 | | |
