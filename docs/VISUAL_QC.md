# 画面质量与视觉缺陷检测（VisualDefectAnalyzer）

> 目标：让 QC 能发现**人眼可见的画面问题**，而不只是元数据（容器/码率/色彩字段）层面的不一致。
> 黑场、冻结、花屏马赛克、模糊、闪烁、过曝欠曝、色偏、隔行梳齿、letterbox/pillarbox
> 都属于"不看画面就发现不了"的那一类。

## 1. 定位与分层

```
解码线程 MediaPlayer::DecodeThread()
        │  按采样档位抽帧
        ▼
QualityAnalyzer::BuildSample()   AVFrame -> FrameSample（GRAY8 + RGB24 小图）
        │
        ▼
VisualDefectAnalyzer            纯 C++17，不依赖 FFmpeg（单测直接造图）
        │  Submit() 投递（队列有上限，满了丢帧）/ Feed() 同步（离线全帧）
        ▼
信号: VisualDefectFrameReady / VisualDefectReady / VisualDefectStatsReady
        ▼
AnalysisPanel「画面质量」页    曲线 + 缺陷列表 + 证据缩略图 + 导出
```

关键取舍：

- **算法层不碰 FFmpeg**：只吃 `model::FrameSample` 里的降采样像素。这样单测可以完全用
  合成图（见 `tests/unit/test_visual_defect.cpp`），不需要准备真实媒体文件，跑起来是毫秒级。
- **实时/离线两条路**：实时走工作线程 + 有上限队列（播放流畅优先，丢分析帧），
  离线全帧走同步 `Feed()`（一帧不丢，代价是播放变慢）。

## 2. 核心模型（core/model/）

| 文件 | 作用 |
|------|------|
| `QualityMetric.h` | `FrameSample`（降采样快照）、`ActivePictureArea`（有效画面区域）、`FrameQualityMetric`（单帧指标） |
| `VisualDefect.h` | `VisualDefectType` / `VisualDefectSeverity` / `VisualDefect`（时间段 + 触发值 + 阈值 + 证据图）、`VisualDefectReport` |

取值约定（与 `QualityAnalyzer::QualityMetrics` 一致）：

- 指标算不出来时写 **NaN**，绝不用 0 顶替 —— 0 在多数指标里都有意义（黑场比例 0 = 没有黑像素、
  帧差 0 = 完全静止）。
- 比例类指标值域 `[0,1]`，亮度类值域 `[0,255]`。
- 缺陷按**时间段**输出，不是逐帧刷屏；连续命中合并成一段，条件断开或 `Flush()` 时才落库。

## 3. 检测算法与默认阈值

| 缺陷 | 判据 | 默认阈值 |
|------|------|----------|
| 黑场 | `black_ratio`（Y≤24 的像素占比）且亮度均值过低 | 占比 ≥ 0.95 且均值 ≤ 24，持续 ≥ 0.3s |
| 冻结帧 | 与上一采样帧的平均绝对差（MAD/255），**且音频不静音** | 差异 ≤ 0.01（约 2.5 灰阶），持续 ≥ 1.0s |
| 花屏/马赛克 | 8×8 块边界落差 vs 块内部落差：`(b-i)/(b+i)` | ≥ 0.35，持续 ≥ 0.3s |
| 模糊 | 灰度图拉普拉斯方差 / 1000（锐度指数） | ≤ 1.0 且画面有内容（亮度标准差 ≥ 0.5），持续 ≥ 0.5s |
| 闪烁 | 亮度均值序列亮→暗→亮交替次数 + 平均跳变幅度 | 交替 ≥ 3 次且平均跳变 ≥ 8，窗口 ≥ 0.5s |
| 过曝 | 高光占比 / 硬削波占比 | 高光 ≥ 20% 或削波 ≥ 8%，持续 ≥ 0.3s |
| 欠曝 | 暗部占比（Y≤40）或全帧均值 | 暗部 ≥ 60% 或均值 ≤ 35（且未被判黑场），持续 ≥ 0.3s |
| 色偏 | `sqrt((R-Y)²+(B-Y)²)/255`（需采集 RGB） | ≥ 0.08（约 20 灰阶），持续 ≥ 1.0s |
| 隔行梳齿 | 跨场行差 vs 同场行差：`(full-field)/(full+field)`，且画面有运动 | ≥ 0.30，持续 ≥ 0.5s |
| 上下/左右黑边 | 逐行/逐列均值扫描连续黑边 | 两侧合计 ≥ 4%，持续 ≥ 0.5s |

几个刻意的决定：

- **冻结必须排除静音段**：静止画面 + 静音是片尾/黑屏留白，不是故障；静止画面 + 有声音才是卡死。
  素材没有音频流时按"不静音"处理（无从判断，宁可报出来）。
- **纯色/黑场帧不算模糊**：它们的拉普拉斯方差天然接近 0，那是"没细节"，由黑场/曝光去报。
- **黑场不重复计欠曝**：15/255 已经是黑场，不再叠一条欠曝。
- **梳齿要求有运动**：静止的隔行素材看不出梳齿，只有场间时间差才会形成梳状边缘。

## 4. 有参考指标：PSNR / SSIM / VMAF

- **PSNR / SSIM**：`QualityAnalyzer::CompareFrames()`（AVFrame 输入）与
  `CompareSamples()`（已降采样快照输入，批量比对时省掉重复缩放）。
  SSIM 按 Wang 2004 简化式返回真实值 `[-1,1]`（负值表示负相关，不做截断）。
- **VMAF**：需要外部 libvmaf（库 + 模型文件），属于可选依赖，第一阶段不接入。
  接口与字段已预留 —— `QualityMetrics::vmaf` 恒为 NaN，`QualityAnalyzer::ComputeVmaf()`
  是替换点，将来接进来时调用方与报告字段都不用改。

## 5. 性能策略

| 档位 | 采样率 | 分析宽度 | 说明 |
|------|--------|----------|------|
| 快速 | 1 fps | 160 | 播放几乎无感 |
| 标准（默认） | 2 fps | 256 | 默认档 |
| 精细 | 5 fps | 384 | 细节更全，开销约 3 倍 |
| 离线全帧 | 每帧 | 256 | 同步分析，一帧不丢，播放会明显变慢 |

- 每帧先降采样再做统计（256×144 的拉普拉斯/边缘统计是微秒级）。
- 实时档位走**有上限队列**（默认 8 格）+ 工作线程：`Submit()` 永不阻塞解码线程，
  队列满直接丢帧，丢弃数显示在汇总里（播放压力大时的直观证据）。
- 离线档位直接 `Feed()` 同步分析，不经过队列。
- 缺陷条数上限 2000，采样指标上限 20 万条，超长素材不会把内存吃光。

## 6. UI 呈现（分析面板「画面质量」页）

- **采样档位 / 模糊阈值 / 冻结阈值 / 是否采集缩略图**：改完立即下发播放器。
- **质量曲线**：
  - 亮度均值（左轴 0–255）+ 黑像素比例（右轴 0–1）；
  - 锐度指数（对数压缩，避免清晰画面的大数值把帧差曲线压平）+ 帧间差异。
- **缺陷列表**：类型 / 严重度 / 起止时间 / 时长 / 触发值 / 说明；点击行跳转播放器对应时间。
- **证据缩略图**：选中缺陷显示该段起始帧的小图，可批量导出 PNG（`缺陷序号_类型_时间.png`）。
- **导出**：缺陷 CSV（含类型代码与触发阈值，便于复核）。
- 汇总行会显示已分析帧数、缺陷数、有效画面区域，以及被丢弃的分析帧数。

## 7. 测试验收（tests/unit/test_visual_defect.cpp）

| 用例 | 验收点 |
|------|--------|
| `AllBlackClipMustBeDetected` | 全黑片段必须识别黑场，起止与时长合理 |
| `FiveSecondFreezeMustBeDetected` | 冻结 5 秒样本必须识别冻结段（且 ≥3s 升级为 Error） |
| `StillFrameWithSilentAudioIsNotFreeze` | 静止 + 静音不报冻结 |
| `BlurredFrameHasLowerSharpnessThanSharpFrame` | 模糊样本锐度必须低于清晰样本，且低于阈值 |
| `BlurSegmentIsReportedForBlurredClip` | 连续模糊片段必须产出缺陷段 |
| `FlatFrameIsNotReportedAsBlur` | 纯色帧不算模糊 |
| `LetterboxReportsActivePictureArea` | letterbox 样本必须输出有效画面区域（y=8，高度 36-16） |
| `FlickerIsDetectedOnAlternatingLuma` | 亮暗交替必须识别闪烁 |
| `CombingScoreSeparatesInterlacedAndProgressive` | 隔行/逐行梳齿分数可区分 |
| `BlockinessScoreSeparatesMosaicAndNatural` | 马赛克/正常画面块效应分数可区分 |
| `AsyncQueueDropsInsteadOfBlocking` | 队列满时丢帧而不是阻塞 |
| `FeedIsSynchronousAndAnalyzesEveryFrame` | 离线路径一帧不丢 |

## 8. 已知局限

- 结果依赖采样率：闪烁这种高频现象建议在「精细」或「离线全帧」档位下看。
- 色偏是统计意义的判定，本身是暖色调风格的素材（日落、钨丝灯）可能误报，可调阈值。
- 模糊阈值基于降采样后的锐度指数，与分辨率/内容相关，实际使用建议按素材微调。
- 当前是**播放时实时分析**；离线全文件批量体检（不播放、一次扫完出报告）尚未接入
  `AnalysisCoordinator`，后续可复用同一套 `VisualDefectAnalyzer` 补齐。
