# 码流 / 音频解析审计复核（2026-10-24）

> 对象：上一轮"10 项问题（7 P1 + 3 P2）"的审计结论。
> 本文做三件事：**逐项裁定**（哪些成立、哪些要改口）、**补漏**（原报告没覆盖到的）、
> **重排修复顺序**（按"能让测试先红"的依赖关系排，而不是按严重度排）。
>
> 复核方法：
> - 静态：读实现 + 对规范（VP9 6.2.2 / H.264 7.3.3 / ADTS / Opus RFC 6716 §3 / MP4 dOps）。
> - 动态：用 MSYS2 g++ 直接编仓库源码（不链 Qt/FFmpeg 运行时），构造**按规范写的**输入喂给
>   现有实现，打印实际输出。附录 A/B 是两段可直接转成 ctest 用例的复现程序。

---

## 一、总结论

原报告的 10 项**全部成立**，没有误报；严重度分级基本合理。但有 4 处需要修正，另有 7 项遗漏：

| 类别 | 结论 |
|---|---|
| 事实判断 | 10/10 成立。VP9 的"383×215"与 H.264 的"参考 27 / 输出 15、qp_valid=true"这两条我复现出了同样的机制与量级（我的样本参考 27 → 输出 31）。 |
| 根因描述 | 2 处需要改写：VP9 不是"某些 profile 错位"，而是 **RGB 与非 RGB 两个分支各错 1 位、方向相反**；H.264 不止"漏参考标记"，是**漏了 4 个语法元素**，且 `slice_type 5..9` 未归一化是同一处的第二个 bug。 |
| 严重度 | `es_inspector` 未启动应从 P1 上调为「功能整体未生效」（对外不可见，但 C4 是零产出，不是"部分不准"）。 |
| 遗漏 | 7 项，其中 2 项是 P1 级（H.264 slice_type 归一化、PPS 缺 entropy_coding_mode_flag 导致 P/B QP 修不动）。 |
| 测试口径 | 原报告的判断正确且重要，但还可以再进一步：**根因是"生成器抄了实现"**，所以要立一条硬规则（见第五节）。 |

---

## 二、动态复核证据

### 2.1 VP9 color_config（对应原报告 P1-1）

按规范 6.2.2 逐位写出关键帧头（192×108），喂 `Vp9BitstreamParser::ParseFromFrame`：

| 样本 | 规范期望 | 实测输出 |
|---|---|---|
| profile 0 / BT.709 / 4:2:0 limited | present=1，192×108 | **present=0**（判失败） |
| profile 0 / BT.709 / 4:2:0 full | present=1，192×108，range=1 | **present=0** |
| profile 1 / BT.709 / 4:2:2 full | present=1，192×108 | **present=0** |
| profile 2 / BT.2020 10bit | present=1，192×108 | **present=0** |
| profile 1 / **CS_RGB** | present=1，192×108 | **present=1，383×215** |

与原报告现象逐字吻合。根因比"read 错位"更精确一点：

```text
规范 6.2.2（不是"profile 1/3 才有 ColorRange"）：
  if (color_space != CS_RGB) {
      if (profile == 1 || profile == 3) reserved_zero  f(1)   ← 前置保留位
      ColorRange                                     L(1)    ← 所有 profile 都有
      if (profile == 1 || profile == 3) { subsampling_x; subsampling_y; reserved_zero }
      else { subsampling_x = subsampling_y = 1 }
  } else {
      ColorRange; subsampling_x = subsampling_y = 0           ← RGB 分支没有 sub 位
  }
```

实现把 `ColorRange` 是否存在绑到了 `profile==1||3`（`has_range_flag`），于是：

| 分支 | 位差 | 后果 |
|---|---|---|
| 非 RGB + profile 0/2 | **少读 1 位**（漏 ColorRange） | `frame_size` 提前 1 位 → 高度读到 `0x8035`+1 = 32822 > `kMaxPictureSize`(32768) → 命中越界保护 → 整份作废 `present=false` |
| 非 RGB + profile 1/3 | **少读 1 位**（漏前置 reserved_zero） | 同上，`present=false` |
| RGB + profile 1/3 | **多读 1 位**（读了一个不存在的 reserved_zero） | 192×108 → 383×215（`191<<1|1`、`107<<1|1`，数字完全对得上） |

两个方向相反的错位正好解释了"为什么有的错尺寸、有的直接失败"。修的时候必须**两个分支一起改**，只补 ColorRange 会把 RGB 分支继续留在错的状态。

### 2.2 H.264 slice QP（对应原报告 P1-2）

同一份 slice 数据（IDR I-slice，`pic_init_qp_minus26=0`，`slice_qp_delta=+1`），参考 QP = 27：

| 输入构造 | 参考 QP | 实现输出 | qp_valid |
|---|---|---|---|
| 规范写法（含 `dec_ref_pic_marking` 2 位），`slice_type=2` | 27 | **31** | **1（误报有效）** |
| 规范写法，`slice_type=7`（x264 全 I 帧实际会写这个值） | 27 | — | **0（解析失败，slice 计数 0）** |
| 现有单测的写法（跳过参考标记），`slice_type=2` | 27 | 27 | 1 |

第三行就是"测试与实现同源"的直接证据：**删掉的那两位，恰好就是实现漏读的那两位**，所以测试永远绿。

漏读的语法元素（H.264 7.3.3 顺序，在 `slice_qp_delta` 之前）：

1. `ref_pic_list_modification()` —— P/B/SP 必读（I/SI 不读）
2. `pred_weight_table()` —— `weighted_pred_flag && (P|SP)` 或 `weighted_bipred_idc==1 && B`
3. `dec_ref_pic_marking()` —— `nal_ref_idc != 0`（IDR 分支 2 位；非 IDR 分支 1 位 + 循环）
4. `cabac_init_idc` —— `entropy_coding_mode_flag && slice_type != I && != SI`

### 2.3 ADTS / Opus（对应原报告 P1-5 / P2-8 / P2-10）

| 输入 | 规范期望 | 实测 |
|---|---|---|
| ADTS 头声明 `frame_length=11`，实际只有 7 字节 | 拒绝（截断） | `ParseAdts=1 valid=1` → 调用方 `parsed_packets++` |
| ADTS `sampling_frequency_index=13`（reserved） | 拒绝 | `valid=1 sample_rate=0` |
| Opus TOC：60 ms × 3 帧 = 180 ms | 拒绝（>120 ms） | `valid=1 frame_count=3` |
| Opus TOC：code=1（CBR 两帧）但只有 1 字节 | 拒绝（缺帧长字节） | `valid=1 frame_count=2` |
| **合法 11 字节 dOps** | 接受 | `ParseOpusHead=0`（被 `size<19` 挡掉） |
| dOps 补到 19 字节 | `pre_skip=312`、`input_rate=48000` | **`pre_skip=14337`、`input_rate=-2135228416`**（大端被当小端读） |

dOps 这一条比原报告说的更值得修：`OpusHead`（Ogg）是小端、`dOps`（MP4）是**大端**，
现在两条路径共用同一段小端解码代码，只要有人显式走原 dOps 分支就一定读出垃圾数。

### 2.4 其余各项

- **ES 巡检未启动**：`AnalysisPipeline.cpp:292` 的 `if (result.bitstream_analyzed) return;`
  在 `:322` 的 `es_inspector_.Reset(...)` 之前。凡是有 extradata 的文件（MP4/MOV/大部分封装）
  巡检器**从未启用**，`slice_sei` 永远为空。C4 在主力路径上是零产出。
- **长度前缀未适配**：`InspectH264/InspectHevc` 只调 `ExtractAnnBNalUnits`；MP4 的包是
  `[4 字节长度][NAL]`，一条都抽不出。所以只修 2.4 上一条没用，MP4 仍然是空。
- **播放端 `data[]`**：`Decoders.cpp:582` `swr_convert(..., (const uint8_t**)frame_->data, ...)`，
  而 `data[]` 只有 8 个指针；`swr` 按 `ch_layout.nb_channels` 读 `in[8..N-1]`，
  那些位置其实是 `linesize[]` 的内容，会被当指针解引用。离线分析路径已经修好
  （`AnalysisPipeline::FeedAudioFrame` 用 `extended_data`，且有 12 声道回归用例），只有播放端漏了。
- **MP4 样本表 cancel**：`AnalysisInputSession.cpp:198` 只传 3 个参数，`cancel` 默认 nullptr；
  容器页那条路径（`ContainerStructureAnalyzer.cpp:168`）是传了的。**且即使传了也不够**——
  `ExpandSamples()` 的逐样本循环里没有 checkpoint，只在"每条轨之间"检查一次。
- **音频 QC 完整性**：`AudioQcResult` 只有 `analyzed` 布尔 + `notes` 文本，没有解码失败数、
  丢弃样本数、覆盖时长；`decode` 失败、`swr` 缺失、中途改采样率都只是静默 return 或写一句 note。
  报告说的"前面产出过 PCM 后仍显示已分析"成立。

---

## 三、原报告需要修正的 4 处

| # | 原表述 | 修正 |
|---|---|---|
| 1 | "VP9 色彩字段读取错位……Profile 0/2 解析失败；RGB Profile 1 被解析成 383×215" | 现象对，但根因要写成"**ColorRange 在 VP9 里对所有 profile 都无条件存在**；实现的 `has_range_flag` 把'有没有 ColorRange'和'profile 是不是 1/3'混为一谈，导致非 RGB 分支少读 1 位、RGB 分支多读 1 位"。补充：profile 0/2 之所以是"失败"而非"错尺寸"，是因为越界保护在 32822 > 32768 处把整份作废了。 |
| 2 | "H.264 QP 输出错误：slice header 省略了参考标记等必要语法" | 是，但不完整：一共漏 **4 个**语法元素（列表修改 / 加权预测表 / 参考标记 / cabac_init_idc），且**同一函数里还有第二个 bug**：`slice_type` 5..9 未取模归一化，x264 的全 I 帧写 7 会直接解析失败（实测 qp_valid=0、I 计数 0、other=1）。这两条必须一起修，否则 P/B 帧照样错、类型分布照样错。 |
| 3 | "P1 ES 巡检初始化被提前返回阻断" | 定为 P1 可以，但性质要说清：不是"部分数据缺失"，而是**C4 这条功能在所有带 extradata 的文件上零产出**；且 `slice_sei` 目前没有任何下游消费者，所以外部看不出来——属于隐性欠账，一旦接到报告/QC 规则就会立刻暴露。 |
| 4 | "部分测试重复了实现中的错误假设" | 结论对，但要更狠一点：这两个生成器的注释都写着"不参考本项目 parser 的既有实现"，**事实相反**，所以它不只是"弱"，而是给了错误的信心。修法不是"补一个真实样本"就完事，而是立规则（第五节）。 |

---

## 四、原报告漏掉的 7 项

| 编号 | 严重度 | 问题 | 位置 |
|---|---|---|---|
| N1 | **P1** | `slice_type` 5..9 未归一化。x264 的全 I 帧写 7，实现既不认 `direct_spatial_mv_pred_flag`/`num_ref_idx_active_override_flag` 分支（P/B 的 6/7），又在 `Finish()` 里把 5..9 全归到 `type_other`。实测 `slice_type=7` + 规范写法 → `qp_valid=0`、slice 计数 0。 | `VideoEsInspector.cpp:391-443`、`:461-467` |
| N2 | **P1** | `H264PpsInfo` **没有 `entropy_coding_mode_flag`**，PPS 解析里直接 `SkipBits(1)` 丢掉。`cabac_init_idc` 存在与否取决于它 → **不先补这个字段，P/B 帧的 QP 根本修不了**。 | `core/domain/model/H264BitstreamInfo.h:200`、`H264BitstreamParser.cpp:455` |
| N3 | P2 | `out.qp_valid = has_h264_params_`：把"有参数集"当成"QP 可信"。应改成"成功解析的 slice 数 > 0 且全部通过范围校验"，并单独暴露 `qp_parsed_slices`。另 `out.frames = slice_types_.size()` 把 slice 数当帧数，一帧多 slice 会翻倍。 | `VideoEsInspector.cpp:459-472` |
| N4 | P2 | **AC-3 / E-AC-3 与 ADTS 是同一类问题**：`ParseAc3Packet` 用 `header.frame_size_bytes` 前进 offset，但从不校验它与"包剩余字节"的关系，截断包照样 `parsed_packets++`。修 ADTS 时顺手一起改。 | `AudioBitstreamAnalyzer.cpp:405-434` |
| N5 | P2 | `ConvertLengthPrefixToAnnexB()` 对 `prefix_bytes ∉ {1,2,4}` **死循环**（三个分支都不命中 → `nal_size=0` → `pos` 不前进 → 无限 push 起始码）。这是 public API，fuzz 可达；且截断时静默 `break`，调用方无法区分"截断"和"没有 NAL"。 | `ExtradataParser.cpp:127-170` |
| N6 | P2 | `es_inspector_` 的状态清理只做了一半：`inband_probes_` 每次 `Prepare` 都清，`es_inspector_` 只在带内分支 `Reset`。当前每次分析都新建 pipeline 所以还没炸，但 `PrepareBitstream` 的注释明确写了"同一个 pipeline 理论上可被复用"——一旦真复用（批量里省一次构造），上一份文件的结果会被 `FinishVideoEsInspection` 写进这一份，且旧 inspector 还会继续吃新文件的包。旁证：`es_inspector_configured_` 这个成员声明了但**从未使用**，说明本来就想做这件事。 | `AnalysisPipeline.cpp:238-330`、`AnalysisPipeline.h:243` |
| N7 | P2 | 巡检只覆盖**主视频轨**：`OnPacket` 里 `pkt->stream_index == video_stream_index_` 才 Feed，`FinishVideoEsInspection` 也只回填 `bitstream_analysis`（主轨）。C3 已经把码流解析扩展成多流（`bitstream_streams`），slice/SEI 这一层没跟上，附加轨永远是空的。 | `AnalysisPipeline.cpp:410`、`:346-348` |

另有 1 项清理项（P3）：`ExtradataTypes.h:215` 的 `length_size_minus_one = 31` 默认值错了
（应为 3），注释 "3 字节=31，4 字节=31" 也把"位数"和"minus_one"混在一起——
一旦按第 5 节的方案去读这个字段，就会被它坑一次。

---

## 五、测试口径：把"同源错误"变成结构性不可能

原报告的判断正确，但只说了"建议加入独立编码器样本和参考解析结果"。更硬的写法是三条规则：

1. **生成器的输入必须来自实现之外的描述，且注释里要能指到规范的节号。**
   `test_vp9_bitstream_parser.cpp:81` 的 `has_range = (profile == 1 || profile == 3)` 直接照抄了
   实现里的同名变量——这类"复制粘贴"是最难发现的失败模式。建议生成器改成"按规范 6.2.2 的顺序
   逐元素写"，并在每个 `Put` 后面注明该元素在规范中的名字（现在的注释有，但顺序本身是错的）。
2. **至少要有一条"语义黄金值"断言，而不是只断言"解析成功"。**
   383×215 这个错值今天之所以能存活，是因为测试只断言 `present == true` + 尺寸等于生成器写的值。
   对每个解析器加一条来自**其它工具**的期望值：`ffmpeg -i` 报告的尺寸/QP/帧数，硬编码在测试里，
   并注明生成命令。
3. **一个"删位试验"（bit-deletion test）**：对每个参数集/帧头解析器，写一条用例
   "把某个必读字段的位删掉后，解析必须失败或值必须改变"。它专门用来抓"少读一位还能凑出一个
   看似合法的数"这类畸形，成本很低（一个 helper 即可）。

现实条件：仓库已经有 `tests/corpus/generate_real_corpus.py`（ffmpeg 生成 + 语义校验 + 缺 ffmpeg
时 SKIP），`tests/corpus/container/real_h264_aac.ts` 也已在库。**扩展它比新写一套便宜**：

- 加 `codec/real_h264_qp.mp4`（X264，固定 `-qp 27`，便于断言）、`codec/real_vp9.webm`（libvpx，
  176×144）、`codec/real_opus_12ch.*`；
- 新加 `tests/unit/test_codec_conformance.cpp`，用 `AnalysisPipeline` 入口（不是直接调解析器）
  跑这些样本，断言"巡检器真的被启用过（`packets_scanned > 0`）+ 语义值对"。
  这一条同时覆盖原报告建议的"从 pipeline 入口运行的集成测试"和"真实样本"两个诉求。

---

## 六、修复顺序（按依赖排，不按严重度排）

排序原则：**先让测试能红，再修**。否则像 P1-1 那种"修完还是零产出"的情况会白干一轮。

### Step 0　先立红（不修代码）
- 新增 `test_codec_conformance.cpp` + 三个真实样本。
- 断言：`bitstream_analysis.slice_sei.packets_scanned > 0`（功能活着）、
  `slices.qp_avg` 等于样本的已知值（语义对）。
- 预期：TS 样本（带内路径）**QP 断言红**；MP4 样本（extradata 路径）**packets_scanned 断言红**。
  两个红点分别对应 2.4 与 2.2/2.3，顺序天然正确。

### Step 1　接线：让巡检器在带的路径上也启用（修 P1-3 + N6）
- 把 `AnalysisPipeline.cpp:317-329` 那段**搬到 `:292` 的 return 之前**，并去掉
  `if (result.bitstream_analyzed)` 的条件；`Reset()` 无条件执行（每个 `Prepare` 必调一次，
  顺带干掉 N6 的残留状态；`es_inspector_configured_` 要么用起来要么删掉）。
- 参数集注入拆成两处：extradata 路径在 `:328` 注入，带内路径保持现在的"拿到就补"。
- 此时 MP4 样本的 `packets_scanned` 变绿？**不会**——Feed 进去了但抽不出 NAL。正好验证 Step 2。

### Step 2　长度前缀适配（修 P1-4 + N5）
- `VideoEsInspector` 增加 `enum class NalFraming { AnnexB, LengthPrefixed }` +
  `SetNalFraming(prefix_bytes)`；`InspectH264/InspectHevc` 按 framing 走不同的抽取器。
- 前缀宽度来源：`ExtradataParser::Parse()` 得到的 `config.length_size_minus_one + 1`
  （avcC 在 byte4 低 2 位、hvcC 在 byte21 低 2 位，解析器已有）；带内路径默认 AnnexB。
  顺手修 `ExtradataTypes.h:215` 的错误默认值。
- 抽长度前缀 NAL 时**不要**复用 `ConvertLengthPrefixToAnnexB`（多一次拷贝，且它有 N5 的死循环
  与静默截断）；写一个直接产出 `NalUnit` 的 `ExtractLengthPrefixedNalUnits`，
  并在越界时把 `truncated` 记进 `SliceSeiSummary`。
- 修 N5：`prefix_bytes` 只接受 1/2/4，其它直接返回空 + 让循环严格前进（对齐本仓库"Seek 必须严格前进"的铁律）。

### Step 3　H.264 slice header 补全（修 P1-2 + N1 + N2 + N3）
- 先补模型：`H264PpsInfo` 加 `entropy_coding_mode_flag`，`H264BitstreamParser.cpp:455`
  改成存下来而不是 `SkipBits`。
- slice header 按 7.3.3 顺序补齐 4 个语法元素；每个循环都要 `ReadUEBounded`（列表修改的
  `modification_of_pic_nums_idc != 3` 循环、`dec_ref_pic_marking` 的循环）。
- `slice_type` 进来就 `% 5` 归一化，`Finish()` 按 0..4 分类。
- `qp_valid` 改成"解析成功数 > 0 且全部通过 [0,51]"，并新增 `qp_parsed_slices`；
  `frames` 不再等于 slice 数（要么改名 `slice_count` 只留一个，要么按 `first_mb_in_slice==0` 数帧）。

### Step 4　VP9 color_config（修 P1-1）
- 按第二节的分支重写，两个分支一起改；`reserved_zero != 0` 要判失败（现在是 `SkipBits`）。
- 修 `tests/unit/test_vp9_bitstream_parser.cpp` 生成器的顺序（前置 reserved_zero 的位置），
  并补一条 `p1 RGB` 的最小用例（现在完全没有覆盖 RGB 分支的尺寸断言）。

### Step 5　音频三件（修 P1-5 + P2-8 + P2-10 + N4）
都是小改动，建议一个提交做完：
- `ParseAdts`：`frame_length > size` 视为截断（返回三态或单独 `truncated` 标志）；
  `sampling_frequency_index ∈ {13,14}` / `sample_rate == 0` 判无效。
  `ParseAacPacket` 里 `offset + frame_length > size` 记 `truncated_packets`，不进 `parsed_packets`。
- `ParseAc3Packet`：同样加"声明帧长 ≤ 剩余字节"的校验。
- `ParseOpusToc`：读 `vbr` 位，按 `frame_count_code` 校验"必需的帧长字节数"，
  并按规范拒绝 `frame_count * frame_duration > 120 ms`。
- `ParseOpusHead`：拆成 `OpusHead`（LE，≥19 字节）与 `dOps`（BE，≥11 字节，
  `mapping_family != 0` 时 ≥13+channels）两个入口；不要用"有没有 OpusHead magic"来决定字节序判据
  ——两件事要一起决定，写在同一个分支里。

### Step 6　播放端多平面（修 P1-7）
- `Decoders.cpp:583` 改 `frame_->extended_data`（一行）。
- 顺带：`swr` 的输入布局是 `Init` 时按 `codec_ctx_->ch_layout` 建的，中途变化不会重建 →
  在 `ReceiveFrame` 里比对 `frame_->ch_layout` 与已建布局，不一致就重建 swr（否则 5.1→7.1 那种
  流会静默重采样错）。
- 回归：把 `test_analysis_pipeline_edges.cpp` 的 12 声道用例"下移"一层，直接测 `AudioDecoder`
  （现在只覆盖了 pipeline 的 `FeedAudioFrame`）。

### Step 7　音频 QC 完整性（修 P1-6）
模型加（跨层改动，放最后是因为它要动 model/analyzer/assembler/QC 规则/UI 文案五处）：

```cpp
enum class AudioQcCompleteness { NotRun, Complete, Partial, Unsupported, Failed };
int64_t decode_error_packets = 0;   // avcodec_send_packet < 0
int64_t decode_error_frames  = 0;   // avcodec_receive_frame 非 EAGAIN/EOF 的错误
int64_t dropped_frames       = 0;   // 中途改采样率/声道、swr 缺失
int64_t dropped_samples      = 0;
double  covered_seconds      = 0.0; // 实际参与统计的音频时长
double  coverage_ratio       = 0.0; // covered / metadata.stream_duration
AudioQcCompleteness completeness = AudioQcCompleteness::NotRun;
```

判定：`coverage_ratio < 0.99` → `Partial` + 一条 QC 问题（不要只写 note，文本会被忽略）；
`stream_duration>0 && total_samples==0` → `Failed` 而不是"无信号"；编码未实现 → `Unsupported`。
`AnalysisPipeline::FeedAudioFrame` 里所有 `return` 分支都要记数（现在有 4 个静默出口）。

### Step 8　取消令牌（修 P2-9）
- `AnalysisInputSession.cpp:198` 补第 4 个参数。
- `ExpandSamples()` 内部加 checkpoint（每 4096 个样本或每个 chunk 一次），否则传了也只在轨间生效。
- 容器页那条路径已有，注意别改坏 `MarkCancelled` 的语义。

### Step 9　收尾
- 全部改完后跑 `clang-format -i --style=file:.clang-format` 只作用于本次改动的文件
  （存量 454/514 不合规，全量会污染 diff）。
- `python scripts/summarize_tests.py --update-readme` 更新 README 测试统计。
- 新增畸形样本进 `tests/corpus/generate_corpus.py`（ADTS 截断、Opus 180 ms、dOps 11 字节、
  length-prefix 截断），伞形用例会自动覆盖。

---

## 附录 A　VP9 / 音频复核程序（可直接转测试）

```cpp
// MSYS2: g++ -std=c++17 -I. 本文件 AudioBitstreamParser.cpp Vp9BitstreamParser.cpp \
//        Vp9BitstreamInfo.cpp core/media/codec/BitReader.cpp
// 注意：BitReader.cpp 必须带上，否则链接报 ld returned 1 且诊断被沙箱吞掉。
Bits b;                                  // MSB-first 写入器
b.Put(2, 2); b.Put(profile & 1, 1); b.Put((profile >> 1) & 1, 1);
if (profile == 3) b.Put(0, 1);
b.Put(0,1); b.Put(0,1); b.Put(1,1); b.Put(0,1);      // show_existing / KEY / show / error_resilient
b.Put(0x498342, 24);
if (profile >= 2) b.Put(0, 1);
b.Put(color_space, 3);
if (color_space != 7) {
    if (profile == 1 || profile == 3) b.Put(0, 1);   // ← 这一位就是实现漏掉的
    b.Put(full_range, 1);                            // ← 所有 profile 都有
    if (profile == 1 || profile == 3) { b.Put(sx,1); b.Put(sy,1); b.Put(0,1); }
} else {
    b.Put(full_range, 1);                            // RGB 分支没有 sub 位
}
b.Put(width - 1, 16); b.Put(height - 1, 16); b.Put(0, 1);
```

## 附录 B　H.264 slice QP 复核程序

```cpp
// MSYS2: g++ -std=c++17 -I. -Ithird_party/prebuilt/windows-x64/ffmpeg/include \
//        本文件 VideoEsInspector.cpp ExtradataParser.cpp BitReader.cpp SliceSeiInfo.cpp
// avcodec.h 只用到 AVCodecID 枚举，不需要链 libavcodec。
b.PutUE(0); b.PutUE(slice_type);   // 用 7 试一次：现状直接解析失败
b.PutUE(0); b.Put(0, 4);           // first_mb / slice_type / pps_id / frame_num
b.PutUE(0); b.Put(0, 4);           // idr_pic_id / pic_order_cnt_lsb
b.Put(0, 1); b.Put(0, 1);          // ← dec_ref_pic_marking（现行实现漏读，单测也漏写）
b.PutSE(+1); b.Put(1, 1);          // slice_qp_delta / rbsp stop
// 参考 QP = 26 + pic_init_qp_minus26 + delta = 27；实测实现给 31 且 qp_valid=1。
```

---

## 附：一页版结论

- 10 项全部成立，无假阳性；VP9 与 H.264 两项我用独立程序复现出了同样的机制与量级。
- 必改口：VP9 根因是"两个分支各错 1 位、方向相反"；H.264 漏的是 4 个语法元素且有第二个
  bug（slice_type 未归一化）；巡检未启动是"零产出"而非"部分缺失"。
- 必补：P/B QP 的前置条件是给 `H264PpsInfo` 补 `entropy_coding_mode_flag`（现在被 `SkipBits` 丢掉）。
- 顺序：**先建 pipeline 级红测 → 接线 → 长度前缀 → H.264 → VP9 → 音频 → 播放端 → 音频 QC 模型
  → cancel**。原来的顺序把"修 VP9"放第一，但 VP9 与其余问题没有依赖关系，先修它并不会让任何
  已有测试变绿；而"接线 + 长度前缀"不先做，H.264/VP9 在 MP4 上永远测不到。

---

## 七、修复记录（同日完成）

按第六节的顺序落地，每一项都配了会先红的用例。改动清单：

| 步骤 | 改动 | 关键文件 |
|---|---|---|
| 1 接线 | ES 巡检器的 `Reset` 提到 `PrepareBitstream` 开头无条件执行，参数集注入移到 `if (result.bitstream_analyzed) return;` **之前**；`es_inspector_configured_` 真正用起来（缺参数集时留日志）；带内路径只给**主轨**注入参数集 | `AnalysisPipeline.cpp` |
| 2 长度前缀 | 新增 `ExtradataParser::ExtractLengthPrefixedNalUnits` / `LooksLikeLengthPrefixed`；`VideoEsInspector::SetNalFraming(Auto/AnnexB/LengthPrefixed, prefix_bytes)`，Auto 先看起始码再验证长度前缀链；宽度取自 avcC byte4 / hvcC byte21 的低 2 位 | `ExtradataParser.*`、`VideoEsInspector.*`、`AnalysisPipeline.cpp` |
| 2 死循环 | `ConvertLengthPrefixToAnnexB` 对 `prefix_bytes ∉ {1,2,4}` 直接返回空；`nal_size == 0` 也收手（两者都会让 `pos` 不前进） | `ExtradataParser.cpp` |
| 2 默认值 | `CodecConfig::length_size_minus_one` 31 → 3（合法值 0..3），注释改成正确的表述 | `ExtradataTypes.h` |
| 3 H.264 | `H264PpsInfo::entropy_coding_mode_flag` 落地（不再 `SkipBits`）；slice header 补齐 `ref_pic_list_modification` / `pred_weight_table` / `dec_ref_pic_marking` / `cabac_init_idc`；`slice_type % 5` 归一化；`qp_valid` 改成"至少一个 slice 的 QP 通过范围校验"，新增 `qp_parsed_slices`，`frames` 改为按 `first_mb_in_slice == 0` 计数；`NalUnit` 增加 `nal_ref_idc` | `H264BitstreamInfo.h`、`H264BitstreamParser.cpp`、`VideoEsInspector.cpp`、`ExtradataTypes.h`、`SliceSeiInfo.h` |
| 4 VP9 | `color_config()` 按 6.2.2 重写：非 RGB 分支 profile 1/3 先读前置 `reserved_zero` 再读 `ColorRange`；RGB 分支只读 `ColorRange`；两处 `reserved_zero != 0` 判失败 | `Vp9BitstreamParser.cpp` |
| 5 音频 | `ParseAdts`：`sampling_frequency_index ≥ 13` 判无效、声明帧长超过包剩余字节 → `truncated`（不计 parsed）；`ParseAc3Packet` 同样校验帧长；`ParseOpusToc` 读 vbr/padding 位、code 1/3 要求第 2 字节、总时长 ≤ 120 ms；`ParseOpusHead` 按 magic 在同一分支里同时决定偏移与字节序，新增 `ParseDOps`（大端、≥11 字节） | `AudioBitstreamParser.*`、`AudioBitstreamAnalyzer.cpp` |
| 6 播放端 | `swr_convert` 输入改 `frame_->extended_data`；新增 `RebuildSwr`/`SwrMatches`，帧的 `ch_layout`/`format`/`sample_rate` 与建 swr 时不一致就重建 | `Decoders.cpp/.h` |
| 7 音频 QC | `AudioQcResult` 增加 `completeness`（NotRun/Complete/Partial/Unsupported/Failed）、`decode_error_packets/frames`、`dropped_frames/samples`、`covered_seconds`、`coverage_ratio`；`FeedAudioFrame` 的 4 个静默出口全部记账；新增 QC 规则 `audio.qc.incomplete`（Partial/Failed 出问题，不只写 note）；JSON 与 `ToString()` 一并输出 | `AudioQcResult.*`、`AudioQcAnalyzer.*`、`AnalysisPipeline.cpp`、`QcModels.cpp`、`QcRuleEngineCheckAudio.cpp`、`QcReportExporterJson.cpp`、`AudioQcPage.cpp` |
| 8 cancel | 样本表分析补第 4 个参数（`cancel_source_`）；`ExpandSamples` 每 4096 个样本 checkpoint 一次，取消时 `CollectTrack` 返回 false → 整份结果作废 | `AnalysisInputSession.cpp`、`Mp4SampleTableAnalyzer.cpp` |

新增/重写的用例：

- `test_vp9_bitstream_parser`：生成器改成按 6.2.2 顺序（去掉复制自实现的 `has_range`）；新增
  `Profile0FullRangeBitIsNotDropped`、`Profile1RgbKeepsExactFrameSize`、**删位试验**
  `DeletingColorRangeBitBreaksFrameSize`。
- `test_video_es_inspector`：`MakeSliceHeader` 补齐 7.3.3 的全部前置语法；新增
  `H264IdrSliceQpMatchesSpec`（27）、`H264SliceTypeVariantIsNormalized`（type=7）、
  `H264PSliceHasItsOwnSyntax`、`H264NonReferenceSliceSkipsDecRefPicMarking`、
  `H264FramesCountsPicturesNotSlices`、`H264LengthPrefixedPacketIsParsed`、`H264AutoFramingDetectsLengthPrefix`。
- `test_audio_bitstream_parser`：`AdtsTruncatedFrameIsNotValid`、`AdtsRejectsReservedSampleRateIndex`、
  `OpusDopsIsBigEndian`、`MinimalDops11BytesIsAccepted`、
  `OpusTocRejectsOverlongAndMissingLengthBytes`、`OpusTocReadsVbrAndPaddingFlags`。
- `test_h264_bitstream_parser`：`PpsKeepsEntropyCodingModeFlag`（CAVLC/CABAC 两态）、
  `ConvertLengthPrefixRejectsInvalidWidth`、`ExtractLengthPrefixedNalUnits`、
  `LooksLikeLengthPrefixedDistinguishesAnnexB`。

仍未做（后续）：

- 巡检器只覆盖主视频轨（N7）：多轨要改成 per-stream 实例，属结构性改动。
- `test_codec_conformance.cpp`（从 `AnalysisPipeline` 入口跑真实样本、断言 `packets_scanned > 0`）
  需要扩展 `tests/corpus/generate_real_corpus.py` 并新增 CMake 条目；`_recheck_test` 关闭了
  自动 regenerate，增删测试文件必须手工重新 configure。

### 收尾：测试口径与门禁

- `AudioQcPage::VerdictRuleCount()`（新增静态方法）：裁决表条数（11 → 12）改由实现导出，
  `test_audio_qc_page` 两处断言跟随它 —— 以后再加 `audio.*` 规则不必再去测试里改魔法数字。
  这一条本身就是「派生数据跟数据源走」在测试侧的落地。
- `python scripts/summarize_tests.py --update-readme`：README 测试统计随新增用例更新
  （否则 `TestSummaryReadmeCheck` 会红）。
- 全量回归：`build/_recheck_test` 下 `ctest` **117/117 通过**（117 s）。
- 格式门禁按**整文件**校验本次改动过的文件（`scripts/ci-changed-files.sh` + `clang-format
  --dry-run --Werror`），所以触碰到的 20 个文件里沉淀的存量格式债也一并 `clang-format -i`
  对齐了；逻辑改动与纯空白改动建议拆成两个提交，便于 review。

