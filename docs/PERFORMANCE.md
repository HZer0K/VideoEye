# 性能：长视频与 UI（阶段 5）

功能正确之后才轮到这一层。目标不是"把界面调快一点"，而是回答三个问题：

1. **一个 1 小时以上、10 万级帧记录、多轨的素材会不会把内存吃穿？**
2. **曲线和表格在长素材下还会不会每来一个样本就全量重绘一次？**
3. **点了取消之后，界面多久承认"取消了"、后台多久真的停下来？**

这三件事以前只能靠"感觉卡不卡"。现在它们各有**上限、预算和指标**，并且被单测钉住。

---

## 1. 指标口径

指标实现在 `infrastructure/logging/PerfMetrics.h`（与开发期临时埋点 `VE_PERF` 的区别见
该文件头）。**硬约束：这些数字只写日志，不进普通用户界面。** 用户看到的只能是
"分析完成 / 已取消"这类状态；毫秒数与 MB 数属于工程信息，出现在弹窗里只会让人困惑。

| 指标名 | 常量 | 单位 | 预算 | 埋点位置 | 什么时候记 |
|--------|------|------|------|----------|------------|
| `open_ms` | `kMetricOpenMs` | ms | 5000 | `core/player/OpenController.cpp` `Prepare()` | **只在成功打开时**（失败时测的是"多快发现打不开"） |
| `analysis_ms` | `kMetricAnalysisMs` | ms | — | `ui/analysis_panel/DiagnosticsPage.cpp` `OnFacadeFinished()` | **只在跑完时**（被取消的那次不算） |
| `cancel_response_ms` | `kMetricCancelResponseMs` | ms | 100 | `DiagnosticsPage::CancelScan()` | 每次取消请求 |
| `cancel_settle_ms` | `kMetricCancelSettleMs` | ms | 3000 | `CancelLatencyProbe::Settle()` | 取消请求 → 后台终态回调到达 |
| `report_export_ms` | `kMetricReportExportMs` | ms | 10000 | `DiagnosticsPage::OnExportClicked()` | 只量"写文件"，不含文件选择对话框 |
| `frame_export_ms` | `kMetricFrameExportMs` | ms | — | `core/exporter/MediaExporter.cpp` `Export()` | **只在导出成功时** |
| `peak_memory_mb` | `kMetricPeakMemoryMb` | MB | — | `RecordMemoryMetrics()`（分析结束 / 导出结束） | 取 OS 维护的高水位 |
| `rss_memory_mb` | `kMetricRssMemoryMb` | MB | — | 同上 | 当前驻留内存 |

设计上的两个刻意选择：

- **失败的样本不记进耗时指标**。取消/失败那一次测的是"多久退出来"，把它混进平均值
  会把正常样本拉低，反而看不出长素材的真实成本。三处埋点分别用 `PerfScope::Discard()`
  与两个小的守卫类（`OpenTimingScope` / `ExportTimingGuard`）在析构时按成功标志统一判断 ——
  失败 `return` 在这些函数里散落十几处，每处手写一次 `Discard()` 迟早漏一处。
- **内存不轮询**。峰值取 OS 维护的高水位（`PeakWorkingSetSize` / `VmHWM`），本来就不
  需要自己开线程采样；每次批量刷新都写一条 gauge 的话，长视频分析能刷出上万行日志，
  把真正要看的打开 / 分析 / 导出耗时淹掉。

### 怎么读

日志文件 `videoeye_debug.log`，与 exe 同目录（路径由 `ui/app/main.cpp` 设置）。

```
[INFO] [metric] open_ms=812.443 ms
[INFO] [metric] analysis_ms=93421.772 ms
[INFO] [metric] cancel_response_ms=0.085 ms
[INFO] [metric] cancel_settle_ms=47.648 ms
[INFO] [metric] peak_memory_mb=1421.500 MB
[WARN] [metric][超预算] open_ms=6300.000 ms (budget 5000.000 ms)
```

超预算**只升 Warning、不改变行为**。预算存在的意义不是"报警"，而是让"慢"有个可判定
的边界 —— 否则每次讨论"取消是不是太慢了"都只能靠感觉。

单测里可以直接取累计值（`PerfMetrics::GetInstance().Snapshot(name)` 返回
`count / last / sum / min / max`），阶段 5 的用例就是这么断言的。

---

## 2. 上限：增长必须被截断，而不是被观察

指标告诉你"发生了什么"，上限保证"不会发生什么"。这一层的原则是：**能靠代码保证的，
不要靠事后看日志发现长歪了。**

| 东西 | 上限 | 位置 |
|------|------|------|
| 明细表记录数 | 100 000（`kDefaultMaxRecordCount`） | `ui/analysis_panel/RecordTableModel.h` |
| 曲线点数 | 4 000（`kMaxChartPoints`） | `ui/charts/ChartDecimation.h` |
| 曲线标记数 | 1 500（`kMaxChartMarkers`） | 同上 |
| 滚动窗口项数 | 300（`kMaxRecentChartItems`） | 同上 |
| 画质采样数 | 20 000（`kMaxQualitySamples`） | `ui/analysis_panel/VisualDefectPage.cpp` |
| 缺陷记录数 | 5 000（`kMaxDefectRecords`） | 同上 |
| 证据缩略图 | 200（`kMaxEvidenceFrames`） | 同上 |

`kMaxRecentChartItems`（滚动窗口）与 `kMaxChartPoints`（抽稀）是两件事，别混用：
抽稀是**压缩**（全时段都在，只是点变少），滚动窗口是**丢弃**（旧的直接不要）。

---

## 3. 表格：从 `QTableWidget` 到 `QAbstractTableModel`

三张会随素材增长的明细表（视频帧 / 数据包 / 音频帧）都换成了
`QTableView` + `RecordTableModel<Record>`（`ui/analysis_panel/RecordTableModel.h`）。

为什么必须换：`QTableWidget` 的每一个格子都是一个堆上的 `QTableWidgetItem`，
内存是 `记录数 × 列数`；`QAbstractTableModel` 只在 `data()` 里**现算**单元格文本，
内存只跟记录数走。更要命的是"裁剪最旧的 N 条"这个动作：整表重建在 10 万行下是
"卡住主线程"，而 `beginRemoveRows()/endRemoveRows()` 只让视图重算受影响的行。

几个容易踩空的点，改这张表的人务必知道：

- **行号 ≠ 记录下标**。筛选生效时模型只暴露 `visible_`，跨表跳转必须用
  `RowOfRecord()` / `RecordIndexOfRow()` 换算。以前帧表↔包表的 PTS 互跳是"解析第 3 列
  文本"再找行，筛选一开就跳错。
- **`RecordTableModel` 刻意没有 `Q_OBJECT`**。它是模板，moc 处理不了模板类。
- **批量追加走 `AppendRecords()`**，一次 `rowsInserted`；逐条 `Append` 会把
  "每帧一次全量重绘"又请回来。
- **GOP 摘要是派生数据**，跟着帧记录走（帧被裁剪时要一起清空重建），所以留在
  `VideoFrameTableWidget` 里，在 `FlushPending()` 时算。

---

## 4. 曲线：一个实现、一个上限、一次提交

`ui/charts/ChartDecimation.h` 是唯一的抽稀实现。本轮之前有六处各写一份上限与策略
（4000 / 600 / 300 / 200 ...），最糟的一版是"每组取最大值" —— 它把**谷值也抹掉了**，
码率塌陷、响度跌落、亮度骤降这类"往下掉"的缺陷在曲线上直接消失。

默认 `DecimationMode::MinMax`：每个桶同时保留最小值和最大值，峰和谷都在。
`test_chart_decimation` 里那条方波用例就是钉这个的。

另外两件事：

- **不要每个样本触发一次完整重绘**。`SeriesBatch` / `SubmitDecimated` 攒一批后
  `Replace()` 一次 —— 逐点 `Append()` 会每次 `update()`，实测 7 条曲线 × 4000 点
  ≈ 3.5 s，全花在重绘上。
- **不要每帧复制大型 `std::vector`**。`DecimateFrom()` 直接从记录数组构造 (x, y) 对，
  不再先拷一份 `double` 数组。

---

## 5. 取消：两个预算和一个看门狗

"取消"是两个时刻，分开记才有意义：

| | 含义 | 预算 | 怎么保证 |
|---|------|------|----------|
| **响应** | 用户按下取消 → 界面状态更新（按钮置灰、进度条转"取消中"） | 100 ms | 这段全在主线程上，超预算说明中间夹了同步等待 |
| **收敛** | 用户按下取消 → 后台线程真正退出、终态回调到达 | 3000 ms | 依赖 FFmpeg 中断回调与循环检查点 |

三条硬规则：

1. **取消只发请求，不发终态。** 终态由 `OnFacadeFinished` / `OnFacadeFailed` 统一发，
   每次扫描恰好一次。以前"点取消时立刻发一个假终态"，而那一刻任务其实还在跑 ——
   那个假终态和真终态混在一起，正是状态收不干净的原因。
2. **收敛看门狗只告知，不抢救。** 超过 `kCancelSettleBudgetMs` 还没终态时，它把等待
   时长继续显示在进度条上（"取消中（后台仍在收尾 N 秒）"）。**绝不弹框、绝不强制恢复
   界面** —— 强制恢复等于承认"后台还在写结果对象"，而它随后到达的终态会覆盖掉用户
   新开的那次分析；状态分家比多等几秒糟得多。弹框还会把"打开即分析"这类自动流程
   卡在模态循环里等不到人点。
3. **空闲态不进"取消中"。** `CancelScan()` 先看一眼 `facade_->IsRunning()`：没有在跑的
   任务时直接返回，不置取消态、也不起看门狗 —— 否则看门狗会去等一个永远不会来的
   终态，然后每秒把进度条刷一次"仍在收尾"，而界面上其实什么都没在做。

---

## 6. 回归防线

改动这一层的任何一处，下面这些用例会立刻告诉你：

| 测试 | 钉住什么 |
|------|----------|
| `PerfMetricsTests` | 累计口径、`last` 是最近一次、`Discard()` 不记录、探针重复 `Settle()` 返回 -1（重复终态必须显形）、预算常量自洽 |
| `ChartDecimationTests` | MinMax 保住谷值、输出永不超过上限、NaN 跳过不占桶、输出时间单调 |
| `RecordTableModelTests` | 裁剪后索引重排、筛选下"可见行 vs 记录"分离、10 万条插入有 O(n²) 护栏 |
| `AudioFrameTableWidgetTests` / `VideoFrameTableWidgetTests` / `PacketTableWidgetTests` | 记录确实住在模型里（不再每个格子一个 item）、行数列数表头 |
| `VisualDefectPageTests` | 证据缩略图只留最近 200 张且被释放、缺陷记录封顶 5000 |
| `DiagnosticsPageTests` | 取消后立刻更新状态、响应延迟在预算内、只发请求不发终态、看门狗报等待不伪造终态、终态一到看门狗停、全程无模态框 |
| `ScanTerminalStateTests` | 取消后共用同一次扫描的各页一起回到 Idle |

`DiagnosticsPageTests` 的取消用例靠 `tests/unit/BlockingTcpEndpoint.h`（只监听、从不
回包的本地 TCP 端口）造出一个"真的在跑、且不会自己结束"的扫描 —— 拿真实大文件或外网
地址都做不到既确定又离线。

---

## 7. 还没做的

- **没有自动化的长视频基准**。目前只有指标与上限，1 小时素材的实测数字仍要靠人跑一次
  再看日志。要做的话：生成一个合成长素材（脚本可复用 `tests/corpus/generate_corpus.py`
  的思路），把 `open_ms / analysis_ms / peak_memory_mb` 落进一个基线文件。
- **`analysis_ms` 没有预算**。分析耗时与素材长度强相关，定一个数只会变成"总是超预算"
  的噪音，等有基线再定。
- **导出还没有峰值内存曲线**，只在导出结束时采一次。
