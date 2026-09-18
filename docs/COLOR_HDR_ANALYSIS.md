# 色彩与 HDR 元数据分析

> 关联需求：功能 4（色彩与 HDR 元数据分析）
> 状态：已实现（2026-09-16）

色彩/HDR metadata 错误会导致播放端颜色错误、灰阶错误或 HDR/SDR 显示异常：
原色（primaries）、传递函数（transfer）、矩阵（matrix）、量化范围（range）、位深、
色度采样中任意一项标错，画面就会整体偏色、发灰或暗部糊成一片；PQ/HLG 内容若缺少
母版显示信息与 MaxCLL/MaxFALL，播放端只能按默认值猜测色调映射。

本模块把这些散落在 FFmpeg 各处的色彩/HDR 信息汇总成一份快照，并按规则给出异常组合告警。

## 1. 模块落点

| 文件 | 职责 |
| --- | --- |
| `core/model/ColorInfo.h/.cpp` | 色彩模型：语义枚举 + FFmpeg 原始枚举值映射 + 像素格式描述 |
| `core/model/HdrMetadataInfo.h/.cpp` | HDR 模型：母版显示、MaxCLL/MaxFALL、Dolby Vision 配置记录、HDR 格式判别 |
| `core/analyzer/ColorHdrAnalyzer.h/.cpp` | 分析器：从 codecpar / coded_side_data / AVPacket / AVFrame 抽取并汇总 |
| `core/analyzer/AnalysisTask.h` | `AnalysisOptions::analyze_color_hdr` + `AnalysisResult::color_hdr` |
| `core/analyzer/AnalysisCoordinator.cpp` | 在既有全文件 demux 循环中更新（几乎零额外 I/O） |
| `core/model/QcModels.cpp` + `core/analyzer/QcRuleEngine.cpp` | 8 条 `video.color.*` 规则，异常进入诊断报告 |
| `utils/ReportExporter.cpp` | 报告的「色彩与 HDR」章节（HTML / JSON / TXT） |
| `ui/analysis_panel/AnalysisPanel.*` | 「色彩与 HDR」页：色彩信息 / HDR 元数据 / 异常组合 + CSV 导出 |
| `tests/unit/test_color_hdr_rules.cpp` | 14 个 gtest 用例，覆盖 6.6 四项验收 |

### 设计约束

`ColorInfo` / `HdrMetadataInfo` **不依赖 FFmpeg**：模型层只认识语义枚举
（`ColorPrimariesKind::Bt2020`、`TransferKind::Pq` ...）与 `AVColorXXX` 的原始数值，
由 `ColorHdrAnalyzer.cpp` 负责中间映射。好处：

1. QC 规则与单测不需要链接 FFmpeg，直接构造样本即可跑验收；
2. `ColorHdrAnalyzer.h` 只前向声明 `AVStream/AVFrame/AVPacket/AVPacketSideData`，
   因此 `AnalysisTask.h`、`QcReport.h` 都能安全包含它；
3. FFmpeg 枚举一旦变值会**编译失败**而不是静默误判——
   `ColorHdrAnalyzer.cpp` 里有一组 `static_assert` 把 `AVCOL_TRC_SMPTE2084` 等
   与 `core/model/ColorInfo.h` 的 `ffmpeg_expect::*` 常量对齐。

## 2. 数据来源（四级兜底）

| 级别 | 来源 | 内容 |
| --- | --- | --- |
| 1 | `AVCodecParameters` | primaries / transfer / matrix / range / format / profile / level / bits_per_raw_sample |
| 2 | `AVCodecParameters::coded_side_data` | HDR10 静态元数据（SMPTE ST 2086、CTA-861.3）、DOVI configuration record |
| 3 | `AVPacket::side_data` | 逐包补充上面两类（部分封装只在包上带） |
| 4 | 解码首帧的 `AVFrame` side data | HDR10+ / DV RPU / HDR Vivid / 环境光等动态元数据 |

第 4 级是**惰性**的：只有当容器/码流层信息不全（或可能是 HDR）时才解码前
`max_probe_frames`(默认 2) 帧，`NeedsFrameProbe()` 控制开关；解码失败会记住并放弃，
不重试。反复调用 `UpdateFromXxx()` 是安全的——已拿到的字段不会被覆盖。

> FFmpeg 8.x 已移除 `AVStream::side_data`，容器级元数据统一落在 `AVCodecParameters::coded_side_data`；
> `AVPacket` 的计数成员也从 `nb_side_data` 改名为 `side_data_elems`（本项目已按 8.1 适配）。

## 3. 语义映射

| FFmpeg 原始值 | 语义枚举 | 展示名 |
| --- | --- | --- |
| `AVCOL_PRI_BT709`(1) | `ColorPrimariesKind::Bt709` | `BT.709` |
| `AVCOL_PRI_BT2020`(9) | `Bt2020` | `BT.2020` |
| `AVCOL_PRI_SMPTE432`(12) | `DisplayP3` | `Display P3` |
| `AVCOL_TRC_SMPTE2084`(16) | `TransferKind::Pq` | `PQ` |
| `AVCOL_TRC_ARIB_STD_B67`(18) | `Hlg` | `HLG` |
| `AVCOL_SPC_BT2020_NCL`(9) | `MatrixKind::Bt2020Ncl` | `BT.2020 NCL` |
| `AVCOL_RANGE_MPEG`(1) | `ColorRangeKind::Limited` | `Limited` |
| `AVCOL_RANGE_JPEG`(2) | `Full` | `Full` |

像素格式由 `av_pix_fmt_desc_get` 解析：位深取 `comp[0].depth`，
色度采样按 `log2_chroma_w/h` 推导，`yuvjXXX` 视为"隐含 full range"（冲突时会告警）。

## 4. HDR 格式判别

`model::ClassifyHdrFormat(color, hdr)` 的判定顺序：

```
DolbyVision > HDR Vivid > HDR10+ > HLG > HDR10(PQ + 完整静态元数据) > 裸 PQ > SDR
```

- **HLG 不要求静态元数据**（广播电视本来就没有 MaxCLL/MaxFALL），所以只有 PQ 才校验
  SMPTE ST 2086 与 MaxCLL/MaxFALL；
- 有 Dolby Vision 配置记录时不再要求 MaxCLL/MaxFALL（DV 由 RPU 提供动态元数据）；
- 元数据缺失的 PQ 记为 `Hdr10Basic`，展示为「PQ（静态元数据不全，非完整 HDR10）」。

## 5. QC 规则

| 规则 id | 严重度 | 触发条件 |
| --- | --- | --- |
| `video.color.hdr_missing_mastering` | Warning | PQ 但缺 SMPTE ST 2086 母版显示信息（非 DV） |
| `video.color.hdr_missing_light_level` | Warning | PQ 但缺/只写了一半 MaxCLL、MaxFALL |
| `video.color.hdr_low_bitdepth` | Error | PQ/HLG 但位深 < 10 bit（8 bit 承载 HDR 会出色带） |
| `video.color.wide_gamut_sdr_transfer` | Warning | BT.2020/P3 原色配 SDR 传递函数 |
| `video.color.matrix_mismatch` | Warning | 矩阵与原色不同代（2020 配 709、709 配 601 等） |
| `video.color.range_conflict` | Warning | yuvjXXX 标成 Limited、RGB 标成 Limited、RGB 标了色度下采样 |
| `video.color.unspecified` | Info | primaries/transfer/matrix/range 有未标注项 |
| `video.color.dv_no_compatibility` | Info | DV 的 `bl_signal_compatibility_id == 0`（Profile 5 无兼容层） |

规则类别为新增的 `IssueCategory::ColorHdr`（展示名「色彩/HDR」），
全部计入 `QcReport` 评分，也能在「诊断与报告 → 规则与阈值」页单独开关。

## 6. UI

侧边栏新增「色彩与 HDR」页（数据驱动生成，行号自动对齐），三个子页：

1. **色彩信息**：像素格式 / 位深 / 色度采样 / Primaries / Transfer / Matrix / Range / Profile / 分辨率；
2. **HDR 元数据**：HDR 格式 / 母版显示色域与亮度 / MaxCLL / MaxFALL / Dolby Vision / HDR10+ / 数据来源；
3. **异常组合**：由规则判定产生的问题，按严重度着色（缺失值在信息表里用橙色加粗标出）。

「开始分析」与「码率与 GOP」「音频 QC」「诊断与报告」**共用同一次全文件扫描**，
不重复读文件；「导出 CSV」输出两张信息表的全部行。

## 7. 验收与手工验证

`tests/unit/test_color_hdr_rules.cpp`（14 个用例）覆盖 6.6：

| 场景 | 期望 | 用例 |
| --- | --- | --- |
| SDR Rec.709 | `BT.709 / BT.709 / BT.709`，无色彩告警 | `SdrBt709ClassifiesTo709AcrossPrimariesTransferMatrix` |
| HDR10 | 识别 PQ + BT.2020 + 10bit + MaxCLL/MaxFALL，无告警 | `Hdr10SampleIsRecognizedWithoutWarnings` |
| 缺元数据的 HDR | 输出 warning（母版显示 + MaxCLL/MaxFALL） | `PqWithoutMetadataWarns` |
| full / limited range | 正确区分；yuvj 标 Limited 时告警 | `FullAndLimitedRangeAreDistinguished` / `YuvjMarkedAsLimitedIsAConflict` |

实机样片（ffmpeg 6.1 + libx265 生成，FFmpeg 8.1 读取）实测：

| 样片 | 结果 |
| --- | --- |
| `yuv420p` + bt709 | `SDR ｜ BT.709 / BT.709 / BT.709 / Limited / 8bit / 4:2:0`，无告警 |
| `yuv420p10le` + bt2020 + PQ + master-display + max-cll | `HDR10`，母版亮度 `0.005 - 1000 cd/m²`，MaxCLL 1000 / MaxFALL 400，无告警 |
| 同上但去掉 master-display/max-cll | 两条 warning（缺母版显示信息、缺 MaxCLL/MaxFALL） |
| `-color_range pc` | 像素格式 `yuvj420p`，Range = `Full`，无冲突告警 |
