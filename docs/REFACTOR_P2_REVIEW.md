# P2 重构评审：AnalysisEngine / MediaPlayer / AnalysisFacade

结论先行：三条里 **P2-1（AnalysisEngine）必须拆，而且是大收益**；
**P2-2（MediaPlayer）方向对，但提案里有一半已经做过了**，重排后才有意义；
**P2-3（AnalysisFacade）基本已经完成**，只剩两个可以直接删掉的小口子。

本文所有行号都取自工作区（`E:/mycode/C/videoeye/VideoEye`），当前 HEAD `76e1346`。

---

## 0. 速览表

| 条目 | 现状（实测） | 提案判断是否成立 | 我的意见 |
|---|---|---|---|
| P2-1 AnalysisEngine | 1251 行，`Run()` 单函数 829 行（321–1149） | 成立，且比说的更严重 | 拆，但**顺序要调**：先终端态 helper + 测试，再 InputSession → Assembler → Pipeline → ScanLoop |
| P2-2 MediaPlayer | 1619 行，但 `PlaybackSession` / `AnalysisSession` / `TaskManager` / `QtWorkerOwner` 已抽出 | **一半过时**：播放生命周期与实时分析开关已经不在它身上 | 只拆剩下三块：`ExportController` → `ContainerInspectionController` → `RealtimeAnalysisController`；不要再造 `PlaybackController` |
| P2-3 AnalysisFacade | 只有 125 .cpp / 86 .h，pimpl，只持有 coordinator + 规则引擎 + 时间轴 + 结果缓存 | 不成立（描述的是旧版本） | 不用拆；做两处收口：`SetResult()` 删掉、`ApplySceneChanges()` 改成显式变更记录 |

---

## 1. P2-1：AnalysisEngine

### 1.1 现状核实

`core/analysis/orchestration/AnalysisEngine.cpp` 1251 行，逐段占用：

| 行区间 | 行数 | 内容 | 归属 |
|---|---|---|---|
| 45–252 | ~208 | 匿名命名空间：box 读取、moov 位置扫描、AudioQcProbe / FrameTypeProbe / ColorFrameProbe、ResultSink | 可整体搬 |
| 255–288 | 34 | `Notify*` / `MarkFailed` / `NotifyCancelled` | **终端态 helper，应先单独成文件** |
| 321–354 | 34 | `Run` 入口：挂取消源、提取扩展名、流媒体分流 | InputSession |
| 356–434 | 79 | `avformat_open_input` / `find_stream_info` / 中断回调 / 容器级元数据 | InputSession |
| 436–452 | 17 | MP4 sample table | InputSession |
| 454–485 | 32 | 流摘要（StreamDigest） | InputSession |
| 487–688 | 202 | 各分析器创建 + 解码器/swr/parser 准备（字幕、时码、aux、码率GOP、色彩HDR、音频QC） | Pipeline |
| 690–725 | 36 | `feed_audio_frame` | Pipeline |
| 727–999 | 273 | 逐包扫描循环 + 进度 + teardown / 释放 | ScanLoop |
| 1001–1148 | 148 | 收尾：音频 flush、汇总、码率序列、各分析器 `Finish()`、`NotifyFinished` | Assembler |
| 1150–1248 | 99 | `RunStreamingManifest` | 清单扫描（自足路径） |

`Run()` 一个函数 829 行、约 40 个局部状态（`fmt`、`buckets`、`frames_since_key`、`last_key_ts`、
`audio_probe`、`probe`、`color_probe`、`packet_index`、`last_pos`……）——这是提案要解决的真问题。

"新增一个分析维度要同时改 6 处"这句**属实**，具体是：`AnalysisOptions` 开关（261 行定义）、
487–502 创建、820–822 分发、1109–1140 收尾、结果字段、`AnalysisOptions` 里的 pass-down。
其中"创建 / 分发 / 收尾"三处就在这个 cpp 的同一段里，是最容易漏改的地方。

### 1.2 对提案的 5 点修正

**① 命名冲突：`StreamingManifestAnalyzer` 这个新名字不能用。**
`core/analysis/streaming/` 里已经有 `DashManifestAnalyzer` / `HlsManifestAnalyzer` /
`SegmentQcAnalyzer`，`RunStreamingManifest` 是三者之上的**分发器**。再叫 `StreamingManifestAnalyzer`
会和既有名字在同一命名空间里互相混淆，而且读者会以为它是"某一个清单解析器"。
建议叫 `StreamingManifestScan`（或 `ManifestScanSession`），明确它是"一次清单扫描流程"。

**② `AnalysisPipeline` 不要持有 `model::AnalysisResult`。**
提案里 Pipeline 和 Assembler 都会碰结果。当前唯一的正确形态是：**一个拥有者**。
`AnalysisResult` 由 `AnalysisResultAssembler` 独占，Pipeline / ScanLoop / InputSession 统统只拿
`AnalysisResult&`。否则 Assembler 就退化成"另一个写同一块内存的地方"，出问题时无从定位。

**③ 取消源不能跟着切片走。**
`CancelSource()`（315 行）返回 `std::atomic<bool>*`，可能是 `active_cancel_source_` 也可能是
`&cancel_requested_`；`CancelSourceScope`（99 行的 RAII）**必须还是 `Run()` 里的第一件事**
（321–327 行注释也强调了：分流分支要走同一颗标志）。建议 InputSession 持有这颗指针，
往下传裸指针，不要每个切片各存一份 `shared_ptr`。

**④ `AVFormatContext*` 的生命周期是隐形约束，不是实现细节。**
代码里有三处注释在守护它：`color_hdr.UpdateFromStream()`（994–996）必须在 `avformat_close_input`
之前；`subtitle_analyzer.Finish()`（1108）必须在关闭之后、发信号之前。所以 InputSession 不能
"打开完就顺手 close"，必须提供 `Close()` 让引擎在 997 行的时点显式调用。这条不守住，
拆完会变成"能编过、结果少一串字段"的静默回归，运行时看不出来。

**⑤ 失败/取消有 6 条出口，且出口语义各不相同。**
360（分配失败）、388（打开被取消）、409（探测被取消）、771（读包被取消）、776（读包 IO 错误）、
956（取消 break + EOF）、968（包数上限 → Sampled）。拆的时候只要把某个 `return` 改成
"抛异常 / 交给 Assembler 统一收尾"，就会把"取消"改写成"失败"，
**症状是界面弹"文件损坏"而不是"已取消"** —— 这正是以前踩过的坑。建议一个切片一个切片加
`out_result` 回归（这是唯一能观测失败/取消终态的通道，见 `AnalysisEngine.h:56-60`）。

### 1.3 建议的切片顺序

我同意"InputSession 风险最低"，但要**把 Assembler 提到 ScanLoop 之前**：
Assembler 是纯聚合、不碰 FFmpeg，先把它摘出来，后面拆循环时"把一个 `AnalysisResult&` 传下去"
就只是一次签名变更，而不是"从 40 个局部里挑出该进 Assembler 的那几个"。

| 切片 | 内容 | 净减行数 | 验收 |
|---|---|---|---|
| **A0** | 把 255–288 的 `Notify*`/`MarkFailed`/`NotifyCancelled`/`kNoFfmpegErrorCode`/`ResultSink` 抽成 `orchestration/TerminalState.h/.cpp`；补一个"每条失败分支都同时写 status+message+error_code"的回归 | 0（搬） | 现有 `test_streaming_scan_status` / `test_diagnostics_result_flow` 全绿 |
| **A1** | `AnalysisInputSession`：open/probe/容器元数据/moov/MP4 sample table/流摘要 + `Close()`；取消源由它持有 | Run 829 → ~560 | `Main10_1080p` / `ConformanceWindowCropsDisplaySize` 等容器回归 |
| **A2** | `AnalysisResultAssembler`：独占 `AnalysisResult`，吃 Pipeline/ScanLoop 的中间态，产出最终终态 | ~120 移出 | 失败/取消测试走 `out_result` 断言 |
| **A3** | `AnalysisPipeline`：6 组分析器的创建 + 分发 + `Finish()`（487–688、820–822、1064–1140） | ~300 | 逐维度开关组合测试 |
| **A4** | `PacketScanLoop`：752–990 的主循环（提成带成员的 struct，不要用 15 个捕获的 lambda） | ~260 | 取消/IO 错误/包数上限三条路径 |
| **A5** | `StreamingManifestScan`：1150–1248 独立路径 | 99 | 清单 + 分片校验 |

A0、A1 是"低风险且能立刻拿到收益"的两步；A3 是收益最大但改动最密的一步，建议单独一个 commit。

### 1.4 顺手发现的 3 个真问题（拆分时一并修）

1. **取消后仍报"分析完成"**：755 行 break 后一路走到 1146 行 `NotifyProgress(callbacks, 100.0, "分析完成")`，
   才在 1148 行发 `NotifyFinished(completed=false)`。取消路径上进度条会先跳到"分析完成"再变"已取消"。
   其它取消分支（389 / 410 / 773 / 1181 / 1211）都是当场返回，只有这一条走穿了整段收尾。
2. **`teardown_scan`（744）定义了却只在错误/取消分支用**；正常路径 991–999 手写了一遍
   （少 `audio_probe.Release()` 之外的顺序差异）。正常路径应直接调 `teardown_scan()`。
3. **`if (is_video)` 块（842–953）缩进已经和 843 行对不齐**，是以前删一段代码留下的痕迹，
   顺手格式化掉（`clang-format` 会自己改，但改动会混进重构 commit，建议单独一个 format commit）。

---

## 2. P2-2：MediaPlayer

### 2.1 现状核实：提案有一半已经做完了

`core/player/MediaPlayer.h` 里已经能看到既成的拆分：

- `PlaybackSession playback_session_`（297 行）—— demux / 解码 / 音频输出 / 播放时钟 / 播放状态机，
  `MediaPlayer` 只读状态、只通过 `PlaybackSession::Hooks`（1309 `InstallPlaybackHooks`）接回调。
- `AnalysisSession analysis_session_`（325 行）—— 12 个分析开关 + `StreamAnalyzer` + 视觉缺陷采样选项。
  `SetFrameTypeAnalysisEnabled` 等 8 个方法（615–626）已经是纯转发。
- `task::TaskManager task_manager_`（361 行）+ `qt::QtWorkerOwner export_workers_`（335 行）——
  容器结构分析、抽帧、媒体导出都已在受管线程上。

所以提案里的 **PlaybackController 不要再建**（会和 `PlaybackSession` 一模一样），
RealtimeAnalysisController 对应的是**还没搬走的播放回调**（下面第 3 点）。

剩余的真实重量（1619 行的分布）：

| 行区间 | 行数 | 内容 |
|---|---|---|
| 225–555 | 330 | `OpenInternal()`：打开/探测/解码器准备/中断状态/触发容器分析 —— **最大的一块** |
| 833–1200 | 370 | 抽帧 + 媒体导出的发起、排队、代际、取消（`frame_export_gen_` / `media_export_gen_` / `pending_*` / `export_shutdown_` / `RequestStop*` / `StartXxxNow*`） |
| 1200–1299 | 99 | `StartContainerStructureAnalysis()` |
| 1309–1605 | 296 | `OnPlaybackPacket/VideoFrame/AudioFrame/SeekDone/EndOfStream` + 视觉缺陷投递 |
| 其余 | ~520 | 转发函数、开关、信号 |

### 2.2 建议的切片顺序（与提案不同：先从导出开始）

| 切片 | 内容 | 为什么是这个顺序 |
|---|---|---|
| **M1 `ExportController`** | 10 个成员（333–367 行）+ 833–1200 全部导出编排 + `CancelAllExports()` | **最该先拆的不是 AnalysisInputSession，而是它**。零 FFmpeg 生命周期、零取消源、对外只留 2 个信号和 2 个入口；是目前唯一能写单元测试的部分（已有 `test_media_switch_export.cpp` 可以直接扩）。而且它把"代际过滤 + 排队 + worker 生命周期"这三条最容易错的知识收进一个文件 |
| **M2 `ContainerInspectionController`** | 555 + 1200–1299，含 `kSlotContainerStructure` | 已经跑在 `TaskManager` 上，纯搬运；顺手把 109 行注释里的"OpenInternal: 容器结构分析完成"这类过期日志正文去掉 |
| **M3 `RealtimeAnalysisController`** | 1309–1605 + 370–389 的 15 个计数器 | 风险最高：`packet_index_ / video_frame_index_ / last_packet_ts_by_stream_ / missing_*_reported_` 等计数器和信号发射**交织**在同一个函数里，搬的时候要把"计数"和"发信号"一起搬，不能只搬计数。放到最后，且建议先把计数器合并成 `AnalysisCounters` 结构值对象再搬 |
| **M4 `OpenController`** | 225–555 | 放在最后：`open_interrupt_` / `open_cancel_` 是**成员**是有意为之（309–316 行注释：回调会被 AVIO/URLContext 各复制一份，栈上状态会悬垂）。搬的时候必须把它一起搬进新对象，不能降级成局部变量 |

**不建议照提案建的**：`PlaybackController`（= 已有的 `PlaybackSession`）。

---

## 3. P2-3：AnalysisFacade

### 3.1 现状：这三条职责已经收口了

`ui/AnalysisFacade.{h,cpp}` 是 **86 / 125 行** 的 pimpl，Impl 里只有 4 个成员
（`coordinator` / `qc_rule_engine` / `timeline_analyzer` / `result`），头文件已经不 include 任何
`core/analysis/*Analyzer.h` 与 `core/qt/*`（`.h:16` 明确写了这条约束）。

提案要的四个类，实际上已经以另一种形态存在：

| 提案的类 | 现状 |
|---|---|
| `QcReportSession` | `core/analysis/diagnostics/QcRuleEngine`（facade 已持有，`rules()` / `Evaluate()`） |
| `RealtimeTimelineSession` | `core/analysis/diagnostics/TimelineAnalyzer`（facade 已持有，`OnPacket/OnFrame/OnSyncSample/Snapshot`） |
| `AnalysisController` | `core/qt/QtAnalysisController`（286/157 行，线程 + generation + Qt 信号） |
| `AnalysisResultStore` | facade 自己的 `impl_->result` + 39–45 行的代际过滤 |

所以这一条**不需要拆**。再拆一层只会把"UI 只看到一个 `AnalysisFacade`"这个已经成立的约束重新打破。

### 3.2 但有两个口子该堵

1. **`AnalysisFacade::SetResult()` 没有任何调用者**（全仓库只有 .h:56 声明和 .cpp:79 定义）。
   它和"结果存储应成为只读快照来源"正好相反——留着它，未来谁都能往里塞一份"看起来是分析出来的"
   结果。建议直接删，让 `impl_->result` 的写入点收敛到 43 行那一条（AnalysisFinished 落地）。
2. **`ApplySceneChanges()`（119–122）是 facade 里唯一的对外写入口**，就地改
   `impl_->result.bitrate_gop`。要么保留但在注释里写死"这是全项目唯一允许改 facade 结果的地方"，
   要么让页面走"把它记下来、下次刷新时合并"。前者代价小，我倾向前者 + 注释。

---

## 4. 交叉约束（做之前先看）

- 分层：`core/analysis` 不能 include `core/player / qc / reporting / ffmpeg / qt`；
  新文件都放 `core/analysis/orchestration/` 和 `core/player/`，不会撞线（`core/player` 允许 include
  `core/analysis`，`core/analysis` 允许 include FFmpeg）。
- 各模块 CMake 是目录 `GLOB_RECURSE`，**加文件不用改 CMakeLists**，但 `tests/CMakeLists.txt`
  的源文件是 configure 期 GLOB，加测试文件要重新 configure。
- 构建：`cmake --build --preset win-test-release-local`，然后在
  `build/test-release` 里跑 `ctest.exe`。MSVC 输出是 GBK，落盘后按 gbk 解码读日志。
- pre-commit 有四道门，`check_layering.py` / `audit_qt_domain_border.py` 必须用 `python` 跑。
- 每个切片一个 commit，先 `git push` 再开下一个（本机出口间歇性抽风，失败隔一两分钟重试 2~3 次）。

## 5. 建议的 commit 序列

```
1) chore(docs): P2 重构评审                                 ✔ 073cf45
2) refactor(analysis): 终端态收口 + 拆 AnalysisInputSession   ✔ aad97a9
3) refactor(player): 拆 ExportController                    ✔ ce2abb2
4) refactor(analysis): 拆 AnalysisResultAssembler + 清单路径  ✔ f05509b
5) refactor(analysis): 拆 AnalysisPipeline                  ← 下一步（收益最大、改动最密）
6) refactor(analysis): 拆 PacketScanLoop
7) refactor(player): 拆 ContainerInspectionController → RealtimeAnalysisController → OpenController
8) chore(ui): 删 AnalysisFacade::SetResult，给 ApplySceneChanges 补约束注释
```

## 6. 已落地切片的实测

| 切片 | 变化 | 验证 |
|---|---|---|
| A0 终端态收口 | `AnalysisTerminalState.{h,cpp}`：`AnalysisCallbacks` + `MarkFailed` / `Notify*` / `ResultSink`；`AnalysisEngine.h` 仍 include 它，调用方无感 | ctest 45/45 |
| A1 `AnalysisInputSession` | `AnalysisEngine.cpp` 1251 → 1037，`Run()` 829 → ~615；`Open()` 返回 `Outcome{Ok,Failed,Cancelled}`，失败终态在会话里写进 result，引擎只发回调 | 构建 EXIT=0、ctest 45/45、check_layering OK |
| M1 `ExportController` | `MediaPlayer.cpp` 1619 → 1291，`.h` 393 → 349；10 个成员 + 7 个私有方法 + 4 个入口实现搬走；对外 API 与 10 条信号契约一字未改 | 构建 EXIT=0、ctest 45/45、check_layering OK |
| A2 `AnalysisResultAssembler` | 8 个 `FinalizeXxx()` + `Finish()`；`ScanBucket` 提到汇编器头（生产者和消费者都要用） | ctest 45/45 |
| A5 `StreamingManifestScan` | 清单路径整体搬走；三处手写取消收尾换成 `NotifyCancelled()` | ctest 45/45 |

当前 `AnalysisEngine.cpp` **830 行**（起点 1251），`MediaPlayer.cpp` **1291 行**（起点 1619）。
`Run()` 剩下的两块就是 A3（六个分析器的创建 + 逐包分发）与 A4（读包循环本体）。

### 搬运时踩到的两个坑（后面几片照着避）

1. **双关指针**：`teardown_scan` 里原来是 `avformat_close_input(&fmt)`（`fmt` 是 Run 的局部变量），
   而会话自己也持这枚指针 —— 会话析构会对已释放的上下文再关一次。必须改走 `input.Close()`
   并把局部的 `fmt` 置空。凡是"对象 + 局部别名"同时持有同一个裸资源的，搬的时候都要检查。
2. **搬段漏符号**：`const double file_duration` 原本定义在"流摘要"那段里，整段搬走后
   `timecode_analyzer.RegisterStreams(fmt, file_duration)` 才报未定义。搬完要 grep
   段内每个局部量的**下游**引用，不能只看被搬的那段自己能不能编。

### 已知抖动用例

`ContainerCancelPathTest.EbmlCancelYieldsNoResult` 时序敏感：它用"实测耗时的 90% 处落取消"
来命中建树之后的尾段，落点随机器负载漂移（单独重跑 13 次红 1 次，全量重跑 45/45）。
与本次重构无关（改动不碰 `core/analysis/container/*`），但值得单独整治成确定性用例。
