# 报告与批量 QC（功能 12）

专业工具最终要输出**可交付、可追溯、可批量执行**的结果。本模块把零散的分析能力收敛成一条
流水线：**选模板 → 分析（单文件 / 目录批量）→ 看结论 → 导出报告 → 对比 / 接入 CI**。

## 1. 能力总览

| 能力 | 说明 |
| --- | --- |
| 单文件 QC 报告 | 一次分析产出评分、结论（通过/警告/不通过）与分级问题清单 |
| 批量目录扫描 | 递归目录、扩展名过滤、并发数限制、取消、进度回调 |
| 规则模板 | JSON 模板，覆盖音频/视频/容器/字幕/流媒体包的阈值与严重度；内置 5 套 |
| 多格式导出 | JSON（机器可读）/ CSV（`#` 元数据头 + 每问题一行）/ HTML / PDF / 纯文本 |
| 双文件对比 | 对比编码参数、时长、码率、GOP、响度、色彩 metadata，可用于转码前后校验 |

## 2. 模块架构

```
core/qc/QcProfile.h|.cpp        模板定义、5 套内置模板、序列化、覆盖项应用
core/qc/QcRunner.h|.cpp         统一分析入口（包装 AnalysisEngine + QcRuleEngine，不碰 Qt）
core/qc/BatchQcRunner.h|.cpp    目录发现 / 过滤 / 有限并发 / 取消 / 汇总
core/qc/BatchQcResults.h|.cpp   批量结果类型（BatchQcRun / 汇总计数），归 QcRules；报告导出只依赖它
core/qc/QcComparator.h|.cpp     双文件对比行构造
core/qc/QcAnalyzeRequest.h      单文件分析的请求/结果契约（批量层零 Qt 依赖）
core/qc/QcReportFormat.h|.cpp   导出格式枚举与扩展名
core/reporting/QcReportExporter.h|.cpp   单文件 / 对比 / 批量汇总 的 JSON·CSV·HTML·PDF·TXT 导出
ui/reporting_panel/             「报告与批量 QC」侧边栏页面
ui/reporting_panel/result_stamp.h        结果新鲜度与复用判定（不依赖 Qt，UI 与单测共用）
```

`core/qc` 拆成 `VideoEyeQcRules`（规则 / 模板 / 报告格式 / 批量结果类型）与 `VideoEyeQc`
（单文件 / 批量执行器）两个静态库，GUI 与单元测试共用同一套逻辑。
报告导出只链 `VideoEyeQcRules`，链接闭包里没有分析层与 FFmpeg。
QC 的入口是 GUI 的「报告与批量 QC」页 —— 项目不再提供独立的命令行程序。
批量扫描层（`BatchQcRunner`）
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

模板可由 UI「加载…」导入或「另存为…」导出，未识别的规则 id 会被跳过并提示。

## 4. UI 用法

「报告与批量 QC」页提供：

- **模板选择**：下拉选内置模板，或加载/另存自定义 JSON 模板，下方显示模板说明。
- **单文件报告**：基于当前已打开文件，按模板重新分析，显示总体评分与结论（红/橙/灰三档配色），
  并支持选择导出格式。
- **批量扫描**：选择目录、扩展名、并发数、是否递归、输出目录；运行后表格逐行显示
  文件 / 状态 / 评分 / 结论 / 各类问题计数 / 耗时 / 输出路径，可随时「停止」（已完成的保留）。
- **导出汇总**：把批量结果聚合成 CSV / JSON / HTML / TXT / PDF 五种汇总（保存对话框可选）。

**五种导出格式的正文只有一处来源**：JSON / CSV / HTML / PDF / TXT 里，PDF 与纯文本共用同一份
正文（`BuildText` 的逐行输出按显示宽度折行后画进 PDF）。单文件报告的章节共 11 节
（对照表见 `test_qc_report_exporter.cpp` 的 `kSectionPairs`）：

汇总 / 问题清单 / 证据与覆盖 / 参数变化时间线 / 帧级显示顺序 / 流列表 /
音频 QC（逐轨） / 响度（主音轨） / MPEG-TS 传输层 / 对比 / 规则快照

CSV 是"每个问题一行"的数据表，不参与这条章节约定（它的行语义是"一个 issue"，不是"一节"），
但**结论层不能缺席**：数据表之前固定有一段以 `#` 开头的元数据头，见下节。

### CSV 的元数据头

只有问题行的表有个致命歧义：**空表**既可能是"真没问题"，也可能是"打开失败 / 被取消 /
超时 / 抽样只跑了一半"。这四种情况的数据区完全一样（只剩表头），换成 TXT / HTML / JSON
都能说清楚 —— 同一份 QC 结论换个格式就丢掉全部覆盖说明，正是「证据与覆盖」要消灭的假象。

所以单文件报告 CSV 在表头前输出一段元数据，`#` 行不属于数据表：

```
#VideoEye QC 报告（CSV）｜ 以 # 开头的行是元数据…
#报告,"文件",…,"路径",…,"模板",…,"生成时间",…,"分析耗时(ms)",…
#结论,"评分",…,"结论",…,"分析成功",是/否,"结论完整",是/否,"抽样扫描",是/否,"超时放弃",是/否,"错误原因",…
#统计,"问题数",…,"致命",…,"错误",…,"警告",…,"提示",…,"流数",…,"时长(s)",…,"大小(字节)",…,"规则",…,"启用规则",…
#覆盖,"维度","执行","能力","结论可用","处理对象","已扫","总量","覆盖率(%)","失败","限制"
#覆盖,"帧级结构","已执行","partial","否","包",5000,10000,50.0,0,"已到扫描窗口上限"
文件,规则ID,严重度,类别,问题,位置,流序号,实测值,阈值,出现次数,说明,建议
```

- 行语法两种：`#<节>,<键>,<值>,<键>,<值>…` 与 `#覆盖` 的列名行 + 逐维度行（列对齐）。
- 下游跳过 `#` 行即可（`pandas.read_csv(..., comment='#')`）；元数据行的值都压过换行，
  物理行数恒等于"元数据行数 + 数据记录数"，按行读取不会错位。
- 「分析成功」= 打开 + 分析是否正常返回（`QcRunResult::ok`）；「结论完整」= 是否覆盖完整扫描
  （`QcReport::completed`，取消/失败为否）；两者分开落，不合成一个派生值 —— 合成的词会把
  "打开失败"和"用户取消"挤到同一个标签下，而它们的处置方式完全不同。
- 「总量」未知时落 `-` 而不是 `0`（0 会被读成"覆盖 0%"）。
- 规则快照正文不进 CSV，但「规则 / 启用规则」条数要在：0 条启用 与 "N 条全跑过且都没命中"
  在空表上长得一样。

这条约束由 `CsvMetaHeaderSeparatesNoIssuesFromNotRun`（先断言两种情形的数据区**确实完全
一样**，再断言元数据头能区分）与 `CsvCoverageRowsMirrorDimensionStatus` 守着。

### 五格式的完整性块（结论层不能只活在 CSV 里）

CSV 把五个标志分开落，但历史上 JSON 只落 `completed / partial / error`，HTML / TXT / PDF
更把它们压成一个派生标签（`if(partial) else if(!completed)`）。后果是同一个「后台超时」
换个格式就成了「分析未跑完」，而「打开失败」与「用户取消」在四种格式里根本分不开；
`partial && !completed` 同时成立时派生标签还会吞掉「没跑完」。

现在五个标志由 `core/reporting/ReportIntegrityFacts.h` 的 `ReadIntegrityFacts()` 统一读成
一份事实，各格式只负责渲染：

| 格式 | 落盘位置 | 读法 |
|---|---|---|
| CSV | `#结论` 的五个中文键 | 跳过 `#` 行之外，按 `#结论,<键>,<值>` 成对读 |
| JSON | `summary.integrity` 对象 | `analysis_ok / conclusion_complete / sampled / timed_out / error`（`error` 恒落，空串也是事实） |
| HTML | `<p class="integrity">` | 一行固定键名文本 |
| TXT / PDF | 正文紧随「评分」的 `完整性:` 行 | 同一行文本；PDF 折行后去掉空白即可复原 |

历史字段 `summary.completed / partial / error` 保留，避免破坏既有下游；新对象与之同源，
不会互相矛盾。HTML / TXT / PDF 共用 `IntegrityLineText()`，所以三端只需一套解析口径。

`test_report_format_consistency.cpp` 把 7 种结论态（正常 / 真没问题 / 打开失败 / 用户取消 /
后台超时 / 抽样 / 抽样且未完成）各导出成 5 种格式，逐份解析回结构化事实，比对条目序列、
级别、计数与五个完整性标志；另有专测断言 6 种「没命中」的结论态在每种格式里两两可分。

### 扩展名 ↔ 正文的分派契约

Auto 系列（`ExportAuto` / `ExportComparisonAuto` / `ExportBatchSummaryAuto`）按路径扩展名
选实现，**落盘内容必须与扩展名一致**。历史缺陷：`.pdf` 对比写成 TXT、`.txt` / `.pdf`
汇总写成 CSV / HTML；更隐蔽的是认不出的扩展名静默退回 JSON，留下「叫 `.xyz` 的 JSON
文件」，下游按扩展名读必然出错。

现在三个家族都有五种实现，PDF 统一走 `detail::WriteTextAsPdf()`（各家族先生成自己的
纯文本正文，再按显示宽度折行画进 PDF），因此 TXT 与 PDF 不可能出现两个版本：

| 家族 | JSON | CSV | HTML | TXT | PDF |
|---|---|---|---|---|---|
| 单文件报告 | `ExportJson` | `ExportCsv` | `ExportHtml` | `ExportText` / `BuildText` | `ExportPdf` |
| 双文件对比 | `ExportComparisonJson` | `ExportComparisonCsv` | `ExportComparisonHtml` | `ExportComparisonText` | `ExportComparisonPdf` |
| 批量汇总 | `ExportBatchSummaryJson` | `ExportBatchSummaryCsv` | `ExportBatchSummaryHtml` | `ExportBatchSummaryText` | `ExportBatchSummaryPdf` |

- 未知扩展名：三个 Auto 一律返回 `false` 且**不落文件**；显式传格式的 `Export(path, bundle, format)`
  不受此限（格式由调用方决定，文件名也由调用方负责）。
- `ExportReport(裸 QcReport)` 的能力边界：它没有运行上下文，只在「分析确实跑完了」时由
  UI 调用，因此 `run.ok` 恒 `true`；`report.completed / partial` 保留自己的结论层语义，
  **不与 `run.ok` 绑定**（绑起来会让「分析成功 / 结论完整」永远同值），也不伪造
  `"分析未完成"` 之类的错误原因。需要表达超时 / 打开失败 / 被取消的调用方请走
  `ExportReport(path, QcRunResult)` 重载，或直接构造 `QcExportBundle` 走 `ExportAuto`。

`test_report_dispatch_consistency.cpp` 让三个家族各导出 5 种扩展名（15 份产物），用**不依赖
产品代码**的识别器按 BOM / 魔数 / 结构标记判实际格式，再校验结构有效性与本家族独有特征串
—— 最后一条专门防「格式对了但正文串到别的家族」（例如汇总 PDF 误调单文件 `ExportPdf`，
产物是合法 PDF，只看魔数发现不了）。另有未知扩展名拒绝、识别器自证判别力、便捷入口
五维度解耦与 TXT/PDF 同源四组守护。

这条约束由 `test_qc_report_exporter` 的三个用例守着：

- `PdfBodyIsTheSameTextAsTheTxtReport` —— 判据是"去掉空白后 PDF 还原出的文本与纯文本报告
  逐字相同"。以前 PDF 自己另排了一套（英文抬头 + 问题清单 + 主轨响度），于是同一份结论
  导出成 PDF 就丢掉全部覆盖说明，而 PDF 恰恰是最常用于交付与存档的格式。
- `TextAndHtmlEmitTheSameSetOfSections` —— 抓出两格式实际输出的所有章节名，与 `kSectionPairs`
  对照：**表外章节**（某一端加了新章节却没同步）与**一端有一端没有**都直接红。
  「流列表」「规则快照」在 HTML 与 JSON 里存在了若干版本，纯文本与 PDF 里一个字都没有 ——
  少了流列表，拿 PDF 交付时看不到被检查的对象里有哪些轨；少了规则快照，"没报问题"
  与"阈值放得很宽"在纸面上长得一模一样。
- `HtmlSectionsAppearInTheDocumentedOrder` —— 章节**齐全**还不够，**顺序**也有语义：
  「证据与覆盖」必须在「流列表」之前（读者要先知道哪些维度真跑了，才知道后面的数字能推到
  什么结论），「规则快照」在最后。`BuildHtml` 现在就是一串 `Append*` 调用，挪一行就换顺序
  且不会有任何编译错误，所以顺序钉死在 `kSectionPairs` 上。

另一个用例 `PdfWrapsOverlongLinesInsteadOfDrawingPastTheMargin` 守折行：`MiniPdfWriter`
只按 `\n` 切段、不自动换行，超宽的行会一路画出页面右边界被裁掉，而文件仍然"结构有效"。

**结果新鲜度与复用**（`ui/reporting_panel/result_stamp.h`）：导出的单文件报告永远绑定
「当前文件 + 当前模板」。

- 切换文件会让上一次结果立即失效、导出禁用（文件内容可能已不同），重新分析成功后才恢复；
  「无结果时点导出」会先取好输出目录、待本次分析成功后在同目录继续导出，不会静默导出旧结果。
- 切换模板不再一律作废：若缓存的原始 `AnalysisResult` 满足三个条件 —— 同一文件版本
  （本地文件按大小与修改时间校验；网络源没有本地指纹，默认重新分析）、新模板要求的分析维度
  都已被这次扫描覆盖（`AnalysisOptionsCovers`）、上次扫描完整成功（含抽样完成，不含取消/失败/超时）
  —— 就直接用它按新模板重算规则（只跑 `QcRuleEngine::Evaluate`，不重跑 FFmpeg / 不解码），
  结果仍算「新鲜」、导出保持可用；否则标为过期。

线程模型：后台 `std::thread` 跑分析，UI 更新统一经 `QMetaObject::invokeMethod(QueuedConnection)` 回主线程；
取消时置原子标记，worker 取下一个任务前检查，正在跑的分析也能通过请求级 `cancel` 感知到；
`Run()` 返回前必然 `join` 完所有 worker，**不会遗留后台线程**。

## 5. 对比模式

`QcComparator::CompareRuns(left, right)` 逐字段对比，输出 `QcCompareRow`：

- 文件级（排在最前）：文件名、判定结论、评分、时长 —— 即使两侧分析数据都为空也能体现差异
- 容器：格式、时长、文件大小、总体码率、流数量
- 视频：编码、分辨率、像素格式、帧率、关键帧数
- 码率与 GOP：平均/峰值码率、GOP 长度、关键帧间隔
- 音频：编码、声道、采样率、响度（LUFS）/ 真峰值
- 色彩与 HDR：primaries / transfer / matrix / range / 位深 / HDR 元数据

数值字段按容差判定（时长差 3ms、响度差 0.1 LU 不视为差异）；单侧缺失标记为 OnlyLeft/OnlyRight，
两侧都没有标为 Unavailable（不算差异）。

## 6. 测试验收

- JSON 报告 schema 稳定：顶层 `profile` / `file` / `metrics` / `streams` / `rules` / `issues` 并列，
  总览字段（评分、结论、问题计数）收在 `summary` 对象里 —— 注意**没有** `report` 包裹层。
- CSV 严格「一个问题一行」（表头 + N 行 issue），批量汇总同理；数据表之前允许有 `#` 开头的
  元数据头（见上节），判据是"跳过 `#` 行后剩下表头 + N 行、每行 12 列"。
- 批量任务取消后不遗留后台线程（`Run()` 内 `join`），且未跑到的项落成 `Cancelled`。
- 单元测试覆盖：JSON 工具、QC 模板（内置 5 套 / 序列化往返 / 覆盖项 / 未知规则检测）、
  批量扫描（发现 / 过滤 / 取消无残留 / 并发上限）、对比、报告导出。
