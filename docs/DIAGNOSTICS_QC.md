# 诊断与 QC 报告（AnalysisCoordinator + QcRuleEngine）

> 目标：把"播放时的实时统计"与"离线全文件体检"彻底分离，并用一套统一的数据契约
> （`DiagnosticIssue` / `QcReport`）串起「扫描 → 规则判定 → UI 呈现 → 报告导出」。

## 1. 分层与数据流

```mermaid
flowchart LR
    UI[AnalysisPanel 诊断与报告页] -->|StartAnalysis(path)| CO[AnalysisCoordinator]
    CO -->|后台 std::thread| SCAN[avformat 全文件 demux 扫描]
    SCAN -->|ProgressReported| UI
    SCAN -->|AnalysisFinished| UI
    UI -->|AnalysisResult| ENG[QcRuleEngine]
    RULES[(QcRule 规则集<br/>阈值可在 UI 编辑)] --> ENG
    ENG -->|QcReport| UI
    UI -->|导出 html/json/csv/txt| EXP[ReportExporter::ExportQcReport]
```

设计要点：

- **与播放解耦**：`StreamAnalyzer` 只负责播放过程中的实时统计；`AnalysisCoordinator`
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

## 3. 扫描产出（core/analyzer/AnalysisTask.h）

`AnalysisResult` 是规则引擎的唯一输入，字段分四组：

- 文件级：容器格式、时长、大小、整体码率、是否可 seek、moov/mdat 顺序（仅 MP4 家族）
- 流级：`StreamDigest`（codec/profile/分辨率/fps/采样率/声道/码率/包数/关键帧数）
- 序列：`total_bitrate_kbps` / `video_bitrate_kbps` / `video_fps`（默认 1 s 粒度）
- 健康度：PTS 单调性、DTS 缺失率、最大时间戳跳变、GOP 间隔序列、包大小极值

## 4. 规则集（默认 16 条）

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

### 新增一条规则

1. 在 `core/model/QcModels.cpp` 的 `DefaultQcRules()` 里追加一条（id / 名称 / 类别 / 严重度 / 判定 / 阈值 / 建议）；
2. 在 `QcRuleEngine::CheckRule()` 中按 id 增加分支，从 `AnalysisResult` 取数并调用 `Triggered(rule, value)`；
3. UI 的「规则与阈值」表与 JSON/HTML 导出都会自动带上新规则，无需改 UI 代码。

## 5. UI（AnalysisPanel「诊断与报告」页）

- 顶部：开始分析 / 取消 / 导出报告 + 进度条
- 概览：评分与结论、各严重度计数、容器/时长/码率/流数/包数/关键帧数
- 子页「问题清单」：逐秒码率 + 帧率双轴曲线，问题表格（严重度按颜色区分）
- 子页「规则与阈值」：启用勾选 + 阈值双击编辑，修改后立即用当前扫描结果重算报告；「恢复默认规则」

> ⚠️ 侧边栏顺序耦合：新增页面会改变 `AnalysisPanel::PopulateStackedWidget` 的页面索引，
> 必须同步 `MainWindow.cpp` 的 `nav_items`（二者按 0-based 位置一一对应）。

## 6. 报告导出

`utils::ReportExporter::ExportQcReport(path, report)` 按扩展名自动分派：
`.html`（总览 + 问题清单 + 规则快照）/ `.json`（含规则快照，便于 CI 回归对比）/ `.csv`（Excel）/ `.txt`。
