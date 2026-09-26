# 报告与批量 QC（功能 12）

专业工具最终要输出**可交付、可追溯、可批量执行**的结果。本模块把零散的分析能力收敛成一条
流水线：**选模板 → 分析（单文件 / 目录批量）→ 看结论 → 导出报告 → 对比 / 接入 CI**。

## 1. 能力总览

| 能力 | 说明 |
| --- | --- |
| 单文件 QC 报告 | 一次分析产出评分、结论（通过/警告/不通过）与分级问题清单 |
| 批量目录扫描 | 递归目录、扩展名过滤、并发数限制、取消、进度回调 |
| 规则模板 | JSON 模板，覆盖音频/视频/容器/字幕/流媒体包的阈值与严重度；内置 5 套 |
| 多格式导出 | JSON（机器可读）/ CSV（每问题一行）/ HTML / PDF / 纯文本 |
| 双文件对比 | 对比编码参数、时长、码率、GOP、响度、色彩 metadata，可用于转码前后校验 |
| 命令行 | `videoeye-cli` 支持 analyze / batch / compare / profiles |
| CI 集成 | 发现 error 及以上问题时返回非 0 退出码 |

## 2. 模块架构

```
core/qc/QcProfile.h|.cpp        模板定义、5 套内置模板、序列化、覆盖项应用
core/qc/QcRunner.h|.cpp         统一分析入口（包装 AnalysisCoordinator + QcRuleEngine）
core/qc/BatchQcRunner.h|.cpp    目录发现 / 过滤 / 有限并发 / 取消 / 汇总
core/qc/QcComparator.h|.cpp     双文件对比行构造
core/qc/QcAnalyzeRequest.h      单文件分析的请求/结果契约（批量层零 Qt 依赖）
core/qc/QcReportFormat.h|.cpp   导出格式枚举与扩展名
utils/QcReportExporter.h|.cpp   单文件 / 对比 / 批量汇总 的 JSON·CSV·HTML·PDF·TXT 导出
cli/videoeye-cli.cpp            命令行前端
ui/reporting_panel/             「报告与批量 QC」侧边栏页面
```

`core/qc` 与 `utils` 被抽进 `VideoEyeCore` 静态库，**GUI 与 CLI 共用同一套逻辑**，
避免「UI 一份实现、CLI 又抄一份」导致的判定分歧。批量扫描层（`BatchQcRunner`）
只依赖 `QcAnalyzeRequest` 这个轻量契约，因此单测可以喂一个「睡 20ms 就返回」的假分析函数，
无需任何媒体文件。

## 3. QC 模板

模板是**只存与默认规则集不同部分**的 JSON（overrides），格式：

```json
{
  "version": 1,
  "id": "hls-vod",
  "name": "HLS VOD",
  "description": "面向 HLS 点播交付，关注分片兼容与码率阶梯",
  "analysis_depth": "standard",
  "rules": {
    "video.gop.max_seconds": { "enabled": true, "threshold": 6.0, "severity": "error" }
  }
}
```

单条覆盖项每个字段带独立 `has_*` 标记 ——「没写」和「写了默认值」语义不同，老模板升级不丢字段。

内置 5 套模板（顺序即 UI 下拉框顺序）：

| id | 名称 | 适用场景 |
| --- | --- | --- |
| `general` | 通用 | 默认中庸配置，什么都查一遍（**不带任何覆盖项**，完全沿用默认规则集） |
| `broadcast` | 广播级 | 电视台 EBU R128 响度（-23 LUFS）、严格 GOP 与色彩合规 |
| `hls-vod` | HLS VOD | HLS 点播：分片兼容性、码率阶梯、关键帧对齐 |
| `short-video` | 短视频 | 短视频平台：响度 -14 LUFS、移动端兼容、快速转码 |
| `archive-master` | 归档母版 | 归档母版：保留时码、完整色彩/HDR 标注、无损优先 |

模板可由 UI「另存为…」导出，或 CLI 用 `--profile <路径>` 加载自定义模板（未识别的规则 id 会被跳过并提示）。

## 4. 命令行

```bash
# 单文件：分析并导出 JSON（默认_fail-on=error，仅 error 及以上才非 0 退出）
videoeye-cli analyze input.mp4 --profile hls-vod --json report.json

# 批量：扫描目录，每个文件按选中格式落盘，再出一份汇总 CSV
videoeye-cli batch D:\media --profile broadcast --out reports --summary reports\summary.csv

# 对比：转码前后一致性校验
videoeye-cli compare source.mov transcoded.mp4 --csv diff.csv

# 列出内置模板（--json 直接导出 JSON 供二次加工）
videoeye-cli profiles
```

常用参数：

| 参数 | 含义 |
| --- | --- |
| `--profile <id\|路径>` | 内置模板 id 或 JSON 模板文件路径 |
| `--json/--csv/--html/--txt/--pdf <路径>` | 导出对应格式报告 |
| `--out <目录>` | 批量扫描的报告输出目录（目录同构，默认不落盘） |
| `--summary <路径>` | 批量汇总报告路径 |
| `--ext <mp4,mov,mxf>` | 扩展名过滤（默认全部文件） |
| `--jobs <N>` | 并发数（默认 4，上限 16） |
| `--fail-on <级别>` | `info\|warning\|error\|critical\|none`，达到该级别返回非 0 |

退出码：`0` 无阻断问题；`1` 存在 `--fail-on` 指定级别以上的问题（供 CI 判定构建失败）。

> PDF 限制：内置 Helvetica（base-14）字体只编码拉丁字符，中文字形无法嵌入，会被替换成 `?`。
> 需要完整中文请改用 `--html`。

## 5. UI 用法

「报告与批量 QC」页提供：

- **模板选择**：下拉选内置模板，或加载/另存自定义 JSON 模板，下方显示模板说明。
- **单文件报告**：基于当前已打开文件，按模板重新分析，显示总体评分与结论（红/橙/灰三档配色），
  并支持选择导出格式。
- **批量扫描**：选择目录、扩展名、并发数、是否递归、输出目录；运行后表格逐行显示
  文件 / 状态 / 评分 / 结论 / 各类问题计数 / 耗时 / 输出路径，可随时「停止」（已完成的保留）。
- **导出汇总**：把批量结果聚合成 CSV/JSON/HTML 汇总。

线程模型：后台 `std::thread` 跑分析，UI 更新统一经 `QMetaObject::invokeMethod(QueuedConnection)` 回主线程；
取消时置原子标记，worker 取下一个任务前检查，正在跑的分析也能通过请求级 `cancel` 感知到；
`Run()` 返回前必然 `join` 完所有 worker，**不会遗留后台线程**。

## 6. 对比模式

`QcComparator::CompareRuns(left, right)` 逐字段对比，输出 `QcCompareRow`：

- 文件级（排在最前）：文件名、判定结论、评分、时长 —— 即使两侧分析数据都为空也能体现差异
- 容器：格式、时长、文件大小、总体码率、流数量
- 视频：编码、分辨率、像素格式、帧率、关键帧数
- 码率与 GOP：平均/峰值码率、GOP 长度、关键帧间隔
- 音频：编码、声道、采样率、响度（LUFS）/ 真峰值
- 色彩与 HDR：primaries / transfer / matrix / range / 位深 / HDR 元数据

数值字段按容差判定（时长差 3ms、响度差 0.1 LU 不视为差异）；单侧缺失标记为 OnlyLeft/OnlyRight，
两侧都没有标为 Unavailable（不算差异）。

## 7. 测试验收

- JSON 报告 schema 稳定：顶层 `profile` / `file` / `metrics` / `streams` / `rules` / `issues` 并列，
  总览字段（评分、结论、问题计数）收在 `summary` 对象里 —— 注意**没有** `report` 包裹层。
- CSV 严格「一个问题一行」（表头 + N 行 issue），批量汇总同理。
- 批量任务取消后不遗留后台线程（`Run()` 内 `join`），且未跑到的项落成 `Cancelled`。
- CLI 发现 error 及以上问题时返回非 0 退出码。
- 单元测试覆盖：JSON 工具、QC 模板（内置 5 套 / 序列化往返 / 覆盖项 / 未知规则检测）、
  批量扫描（发现 / 过滤 / 取消无残留 / 并发上限）、对比、报告导出。
