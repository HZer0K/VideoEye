# VideoEye 全模块代码审查报告

- **审查日期**: 2026-10-02
- **代码基线**: `b1a3c1b`（工作区干净，无未提交改动）
- **审查范围**: `core/` `ui/` `infrastructure/` `tests/` 共 297 个源文件、约 7.5 万行
- **审查方式**: 按 10 个模块并行做只读静态审查，输出后对关键 P0 逐条回读源码复核（已复核项标注 ✅）

## 一、问题总览

| 严重程度 | 数量 | 含义 |
|---|---|---|
| **P0** | 31 | 崩溃 / 死锁 / 越界 / OOM / 死循环 / 功能完全失效 |
| **P1** | 75 | 判定或解析逻辑错误，导致误报、漏报、结果不可用 |
| **P2** | 60 | 性能、健壮性、可维护性问题 |
| **合计** | 166 | — |

| 模块 | P0 | P1 | P2 | 风险画像 |
|---|---|---|---|---|
| core/player 播放与解码 | 3 | 6 | 6 | 线程同步与硬解帧传递 |
| 分析引擎 / 并发 / 任务调度 | 4 | 5 | 5 | 取消点与对象生命周期 |
| core/analysis/codec 码流解析 | 9 | 7 | 6 | 恶意码流的 OOM / 死循环，HEVC 全线失效 |
| core/analysis/container 容器解析 | 5 | 7 | 4 | 堆 UAF、无深度/节点上限 |
| core/analysis/quality 画质视觉缺陷 | 1 | 9 | 6 | 马赛克检测是死代码，阈值体系不归一 |
| diagnostics / qc 诊断与规则 | 3 | 11 | 6 | 字幕重叠漏判、结论评级脱钩 |
| streaming HLS / DASH | 2 | 9 | 9 | 时间轴整数溢出、live 语义缺失 |
| exporter / ffmpeg 工作台 | 2 | 4 | 5 | **导出 100% 失败**、进程树残留 |
| reporting / JSON | 1 | 6 | 5 | **PDF 一律不可读**、CSV 注入 |
| ui 界面层 | 1 | 9 | 8 | 可见性判断错误、主线程阻塞 |

---

## 二、P0 必修清单（按"影响面 × 触发概率"排序）

### 1. 导出功能 100% 失败 —— 临时文件扩展名让格式推断失效 ✅
`core/exporter/MediaExporter.cpp:177` + `:214`

临时路径为 `out.mp4.part-<pid>-<uuid>`，而 `avformat_alloc_output_context2(&out_fmt, nullptr, nullptr, path)` 第三/四参数为空，只能按**最后一个 `.` 之后的后缀**猜 muxer，实际拿到 `part-1234-uuid` 匹配不到任何封装 → 直接返回"无法确定输出格式"。`opt.format` 全程只用于挑编码器（`:332`），从未传给输出上下文。
**修复**: `const AVOutputFormat* f = av_guess_format(opt.format.c_str(), nullptr, nullptr);` 显式传入，或临时名保留扩展名。
**注意**: `tests/unit/test_media_exporter.cpp` 只有"损坏输入报错"和"取消打断 IO"两例，**没有任何成功路径用例**，所以这个让导出全废的 bug 一直测不出来。补端到端成功用例优先级同样高。

### 2. PDF 报告一律不可读 —— 对象编号整体错位 ✅
`core/reporting/QcReportExporter.cpp:286 / :301 / :316`

`objects.insert(objects.begin()+1, kids)` 在**所有对象号算完之后**才插入 Pages 节点，其后每个对象号 +1，但已写入的引用没同步：
- `first_content`(:286) 算得 4，插入后内容流实际是 5 → 页对象里 `/Contents 4 0 R` 指向的是 Helvetica-Bold **字体字典**
- `first_page`(:301) = `4+N`，插入后页对象实际是 `5+N` → `/Kids` 指向的是**内容流**
- `/Pages 2 0 R` 与 `/F1 3 /F2 4` 恰好侥幸正确，所以 xref 自洽但对象图错乱，合规阅读器报"文件已损坏"

**修复**: 先把 Pages 占位插好再编号，或插入后统一重算；加"对象号 ↔ 引用"一致性断言 + 单页/多页回归测试。

### 3. HEVC 分辨率 / profile / level 全部为空 —— NAL type 取错位 ✅
`core/media/codec/ExtradataParser.cpp:202-203`

```cpp
uint16_t header = (data[0] << 8) | data[1];
nal.type = (header >> 3) & 0x3F;   // ❌ 取到的是 bit8..3 = nuh_layer_id
```
HEVC NAL 头 16 bit 布局：`forbidden(1) | type(6) | layer_id(6) | temporal_id+1(3)`，`nal_unit_type` 位于 **bit14..bit9**，正确写法是 `(data[0] >> 1) & 0x3F`（`ExtradataTypes.h:121` 的注释里写的就是这个）。真实 SPS `d0=0x42` 时 type 算成 0，`IsSpsNalUnit` 永不命中 → `BitstreamAnalyzer.cpp:310-327` 三轮匹配全落空。
**修复**: 改 `(data[0] >> 1) & 0x3F`。
**连带**: `ExtradataParser.cpp:494` 的 `ParseHvcC` 是桩（读完 profile 三个字段就 `offset+=20; return`，从不解析 VPS/SPS/PPS 数组）；`:736` 长度前缀流错走 `ParseAnnexB()`，恒 0 个 NAL —— 三处叠加使 MP4 里的 HEVC 完全没有参数集信息。
**测试盲区**: `test_hevc_bitstream_parser.cpp:81` 用 `MakeNal(33, ...)` 手工塞 type，绕过了 `ParseHevcNalUnit`，所以单测全绿而真实数据全错。

### 4. ISO BMFF 嵌套 trak 造成堆 UAF ✅
`core/media/container/IsobmffParser.cpp:537-549`

`IsContainer("trak")` 为真，内层 `tracks.emplace_back()` 触发 vector 扩容后，外层保存的 `saved_trak` 与 `ctx.trak` 立即失效，第 546 行 `ctx.trak->media_timescale` 及后续 mdia/stbl 的 `ParseStts(*ctx.trak, …)` 全写已释放内存。MSVC vector 首容量=1，第二次 trak 必扩容。
**修复**: `tracks` 改 `std::deque` / 存索引，或禁止 trak 自嵌套。

### 5. EBML 解析三重失控：栈溢出 + 无限节点 + VINT 错位
`core/analysis/container/EbmlAnalyzer.cpp:787 / :814 / :386`

- `:787` master 元素递归 `depth` 一路传下去却**从不校验**，3 字节即可造一层嵌套 → 栈溢出（MP4 侧有 `max_depth=8`，EBML 侧裸奔）
- `:814` 未知长度叶子（`size == 0xFF…FF`）既不下钻也不 skip，把载荷当兄弟节点继续读 → 错位后每约 2 字节产出一个节点，几十 GB 文件 OOM
- `:386/406` VINT 首字节为 0 时 `width` 走到 9，`0xFF >> 9` 恒 0 且多读 1 字节 → 全文件连锁偏移
- `:879` 每个 Block 都建节点且无上限，200 万 Block ≈ 400MB+，`ConvertEbmlTree` 再复制一份（MP4 路径有 5 万条上限，EBML 路径一个都没有）

**修复**: `depth > 32` 直接返回；未知长度叶子 skip 到父尾；VINT 宽度强制 1..8；加 `max_elements` / `max_bytes_scanned` 双上限。
**测试盲区**: `tests/unit` 下**没有 test_ebml**，这些在 CI 里永远发现不了。

### 6. 码流解析器对恶意码流无上界保护 —— OOM / 死循环 / UB
`num_xxx_minus1` 这类 `ue(v)` 计数全部直接用作循环上界与 vector 大小：

| 位置 | 字段 | 后果 |
|---|---|---|
| `HevcBitstreamParser.cpp:519`→`:234` | `num_short_term_ref_pic_sets`（规范上限 64） | 填 1e9 → 4GB 分配 → `bad_alloc` |
| `H264BitstreamParser.cpp:302` | `num_ref_frames_in_pic_order_cnt_cycle`（≤255） | 4.29e9 次空转 |
| `H264BitstreamParser.cpp:399-407` | `pic_size_in_map_units_minus1` + `while((1u<<bits)<groups)` | `bits` 达 32 时 `1u<<32` 是 UB |
| `VvcBitstreamParser.cpp:463/476/492/605/690` | `sps_num_subpics_minus1`、`num_ref_pic_lists`、`num_ver/hor` | 2^31 次循环；`:492` `x+1` 在 0xFFFFFFFF 时回绕成 **0**，`SkipBits(0)` 不报错 → **静默错位** |
| `HevcBitstreamParser.cpp:545` | `1 << (log2_min_cb+3+log2_diff)` | 两个 log2 未校验，和 ≥31 时有符号左移 UB，`ctu` 变负 |

**修复**: 入口按规范上界校验后返回错误；位移统一 `1ULL` 并守卫 `<= 30`。
**测试盲区**: 全部单测都是正常码流，**没有任何一条喂入超大 `ue(v)`**，P0 路径全裸奔。

### 7. VVC SPS/PPS 无错误检查即标记 present
`core/analysis/quality/../codec/VvcBitstreamParser.cpp:719 / :769 / :392`

全文件唯一的 `present=true` 置位点前面数百个 `ReadUE/ReadBit` 的 `HasError()` **从未检查**（H.264 `:350`、HEVC `:540` 都做了，唯独 VVC 没有），VPS `:392` 更是先置位后查错。垃圾 SPS 直接进 `ApplyVvcSummary()` 写入 `result_.width/height`。
**测试盲区**: `test_vvc_bitstream_parser.cpp:292 GarbageInputDoesNotCrash` 只断言"不崩"，因 `present` 恒为真，其 `if(sps.present)` 分支恒执行，**实际放行了垃圾值**。

### 8. AnalysisPanel 播放期所有表格一行都不刷新 ✅
`ui/analysis_panel/AnalysisPanel.cpp:593`

```cpp
void AnalysisPanel::FlushPendingUiUpdates() {
    if (!isVisible()) return;   // ❌ 恒为 false
```
`MainWindow.cpp:274` 只 `new ui::AnalysisPanel(content_stack_)`（仅设父子关系），**从未 `content_stack_->addWidget(analysis_panel_)`**，子页面由 `PopulateStackedWidget` 单独加入 stack，面板自己是隐藏孤儿控件 → `isVisible()` 恒 false → 帧/包/音频帧/GOP 表、宏块、场景切换、画面质量页在播放时全部静默，脏标记与记录还一直堆积。
**修复**: 改用 `external_stack_->currentWidget()` 判断，或让各子页自行判断 `isVisible()`。

### 9. PlaybackSession 经典丢失唤醒 —— Stop/Play 随机挂死 ✅
`core/player/PlaybackSession.cpp:92 / :114 / :121`

`state_`/`should_stop_` 是 atomic 但在**锁外**改写，而等待侧在 `cv_.wait(lock, pred)` 内**持锁**求值谓词（`:263`）。窗口：解码线程求值谓词 false → UI 线程 store + `notify_one()`（此刻无等待者）→ 解码线程才进入阻塞 → 之后再无通知。`Pause()` 后 `Play()` 失效，`Stop()` 卡在 `:126` 的 `join()`。
**修复**: 状态迁移放进 `mutex_` 临界区后再 notify，或统一"锁内改 + 锁内通知"。

### 10. 解复用阶段中断回调被人为置空 —— 网络源 Stop 挂死
`core/player/MediaPlayer.cpp:293-294`（配 `core/ffmpeg_io/FfmpegInterrupt.h:41-46`）

`open_interrupt_.cancel = nullptr` 且 `:325` 把 `deadline_us` 归零后，回调恒返回 0。`av_read_frame` 在 RTSP/HTTP 断流时不返回 → `Stop()` → `join()` 无限等待，UI 冻结、进程退不掉。
**修复**: `cancel` 指向一个由 `Stop()` 置真的 `std::atomic<bool>`。
**后续**: 该状态已从 `MediaPlayer` 成员改为每次打开独立的 `OpenAttempt`（`core/player/OpenController.h`），上述语义不变。

### 11. 硬解时把 GPU 帧当 CPU 帧交给分析链路
`core/player/PlaybackSession.cpp:361` + `core/player/Decoders.cpp:250/262`

`GetLastRawFrame()` 返回硬解帧（`AV_PIX_FMT_CUDA/D3D11`，`data[0]` 是设备指针），被塞进 `ctx.raw_frame` 后一路到宏块分析（`MediaPlayer.cpp:1274`）与 `FeedVisualDefectFrame`。sws 不接受 hw 格式 → **逐帧失败刷日志**，画面质量检测 / 场景切换 / 宏块在硬解下全线静默失效。
**修复**: `VideoDecoder` 保留 hwdownload 后的 `sw_frame` 并暴露，硬解时像素类分析只吃 CPU 帧。

### 12. BlockingIo 线程 detach 后访问已析构成员（UAF）
`core/player/MediaPlayer.cpp:1063 / :1080`

`RunBlockingIo` 声明"可能不响应取消"，关闭时 `TaskManager.cpp:420-427` 会 detach 该线程；但任务体在 1080 行访问 `self->task_manager_.IsCurrent(...)`。`QPointer` 只在 `~QObject` 才清空，而 `task_manager_` 是普通成员、在派生析构后立即销毁 → 线程对已析构对象加锁。1058-1061 的注释声称"不持有裸引用，符合约定"，实际不成立。
**修复**: 任务体只按值捕获 `CancelToken` 与结果容器，不回指宿主。

### 13. MainWindow 析构后仍向已销毁对象投递事件
`ui/main_window/MainWindow.cpp:102 / :845-857`

`~MainWindow` 里 `WaitForAll(3000)` 放弃 media-info 线程后，线程仍执行 `self->mediainfo_generation_` 与 `QMetaObject::invokeMethod(self, …)`。若 `~QObject` 已跑完，postEvent 到已销毁对象即崩溃。
**修复**: 改用 `shared_ptr<std::atomic<bool>> alive_` 按值捕获，析构置 false，invoke 前二次判定。

### 14. MP4 全表解析无取消点 —— 第二次分析永久排队
`core/analysis/orchestration/AnalysisEngine.cpp:349-369` + `:661`

`ScanMoovAfterMdat`(350) 与 `Mp4SampleTableAnalyzer::AnalyzeFile`(360) 都不接收 cancel 参数（签名见 `Mp4SampleTableAnalyzer.h:32`），唯一取消检查在包循环顶部(661)。大 MP4 在此卡数分钟，`QtAnalysisController.cpp:38-47` 只能把新请求挂进 `pending_`，`running_` 恒 true → UI 永久"扫描中"。
**修复**: 传 cancel 并分段轮询；`pending_` 加超时兜底，超时发 Canceled 终态并复位 UI。

### 15. 控制器析构 join 无上限
`core/qt/QtAnalysisController.cpp:23-29`

`AnalysisEngine.cpp:339` 把 `deadline_us` 置 0，读取阶段只响应 cancel；IO 不响应中断回调时 `Cancel()` 也救不了 → 关窗时 UI 线程永久阻塞。
**修复**: 给 join 设预算，超时 detach（`engine_` 改 `shared_ptr` 保活）。

### 16. Raw 序列拖动进度条 —— 逐个像素整帧解码，界面冻死
`ui/player/PlayerPanel.cpp:218-225`（配 `:1151-1275`）

`sliderMoved` 在 `showing_raw_image_` 时直接 `ShowRawFrame(v)`，**无 100ms 节流**（同函数普通分支 221-223 有节流）。`ShowRawFrame` 在主线程同步 `file.read()`(1170) + 逐像素 YUV→RGB 双重循环(1202-1262)，4K 帧约 800 万次运算；拖动一次产生上百次调用。读取失败时 1159/1165/1173 还在拖动回调里弹模态框 → 弹窗风暴。
**修复**: raw 分支复用 `last_drag_seek_ms_` 节流；像素转换移后台或建查找表；失败提示改状态栏。

### 17. DASH `<S r=>` 展开可死循环 + 时间轴整数溢出
`core/analysis/streaming/DashManifestAnalyzer.cpp:228 / :232 / :644`

`r="-1"` → `static_cast<uint32_t>` 得 4294967295，循环 42.9 亿次；内层**没有取消检查**（取消只在外层 `:222`），`push_segment` 达 5000 上限后只是 return 不 break → 卡死且取消无响应。`:232` `cursor = start + e.d * (r+1)`，`d="-1"` → UINT64_MAX，乘法/加法回绕 → cursor 倒流，Validate 的 gap/overlap 判定整体失真。
**修复**: `t/d/r` 按 int64 校验 ≥0；按剩余额度 clamp；内层每 1024 次查取消。

### 18. Windows 下 kill 只杀单进程，子进程树残留
`core/ffmpeg/FfmpegProcessRunner.cpp:175`

`QProcess::kill()` 在 Windows 只 TerminateProcess 当前 PID。`FfmpegToolLocator.cpp:85` 把 `.bat/.cmd` 也判为可执行，wrapper 场景下 ffmpeg 是孙进程；ffmpeg 自身在采集/硬件/管道场景也会派生子进程 → kill 后子进程继续写输出文件、句柄泄漏。
**修复**: Windows 用 Job Object（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），Unix 用 `kill(-pgid)`。

### 19. VMAF 空桩的 NaN 静默落库
`core/analysis/quality/QualityAnalyzer.cpp:90 / :102-109`

`ComputeVmaf()` 是桩，恒返回 `quiet_NaN()`，但 `CompareSamples` 无条件写 `metrics.vmaf` 并置 `valid=true`。NaN 会流进曲线与 JSON 导出（JSON 不允许 `NaN` 字面量，序列化器产出非法文件）。
**修复**: 不支持时不赋值；导出层统一做 `isfinite` 过滤。

### 20. SCTE-35 descriptor tag 按 16 bit 读 —— segmentation 全错 ✅
`core/analysis/diagnostics/Scte35Analyzer.cpp:420`

规范 §9.3.1 规定 `splice_descriptor_tag` 为 **8 bit**，代码 `Read(16)` 多读 8 bit 后，`descriptor_length` 实际读到 identifier 首字节 `'C'`(0x43=67)，identifier 读到 `"UEI?"` 永远 ≠ `kCueiIdentifier` → 所有 segmentation_descriptor 解析不出来，且 `descriptor_body_end` 被 67 撑大触发"长度越界"误报。
**测试盲区**: `test_subtitle_timecode_aux.cpp:120` 用 `out.Write(0x02, 16)` **把错误写进了 fixture**，所以单测全绿而真实数据全错。改代码的同时必须同步改 fixture。

> 其余 P0（VINT 宽度、stsd FullBox 头、ASF `obj_size-24` 下溢、FLV `read(1).at(0)` 越界、`1u<<32` UB 等）见第三节对应模块表。

---

## 三、分模块问题清单

### 3.1 core/player —— 播放与解码

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 ✅ | `PlaybackSession.cpp:92/114/121` | 锁外 notify，丢失唤醒 → Stop/Play 挂死 |
| P0 ✅ | `MediaPlayer.cpp:293` + `FfmpegInterrupt.h:41` | 解复用期中断回调置空 → 网络源 Stop 挂死 |
| P0 ✅ | `PlaybackSession.cpp:361` + `Decoders.cpp:250/262` | 硬解 GPU 帧交给分析链路 → 画质检测全线静默失效 |
| P1 | `Decoders.cpp:235-239` + `PlaybackSession.cpp:301` | `avcodec_send_packet` 遇 EAGAIN 直接 return false，整包丢弃（EAGAIN 语义是先 receive_frame 排空） |
| P1 | `PlaybackSession.cpp:314/381` | `drop_until_sec_` 只在视频帧到达时清除；纯音频/定位到尾部时**音频永久静音**，且 `catching_up` 恒真关闭所有视频分析 |
| P1 | `AudioOutput.cpp:186-201` + `Decoders.cpp:448-451` | 设备采样率/声道回退后不回传解码端，swr 仍按原参数 → 变调 + 声道错位 + 缓冲水位算错 |
| P1 | `Decoders.cpp:448-459` | `swr_alloc_set_opts2` 返回值未检查，失败后 `swr_init(NULL)`；错误返回时 swr/codec ctx 均泄漏 |
| P1 | `Decoders.cpp:48/58/407` | 初始化失败路径 `return false` 未释放 `codec_ctx_`（对比 `:465` 视频路径正确） |
| P1 | `PlaybackSession.cpp:227` | `SeekMode::ExactFrame` 对 `av_seek_frame` 无实际作用，仍是关键帧定位 |
| P2 | `MediaPlayer.cpp:61-75` | 场景切换开启时每帧 `sws_getContext` + 分配灰度缓冲 |
| P2 | `Decoders.cpp:302-321` | 每帧整帧 memcpy + 新建 QImage（1080p ≈ 8MB/帧） |
| P2 | `PlaybackSession.h:192`, `MediaPlayer.cpp:293` | `duration_ms_` / `deadline_us` 跨线程读写无同步，应改 atomic |
| P2 | `MediaPlayer.cpp:361/406` | 封面图分支 video_index 置 -1，但 `has_video` 先算成 true，`MediaModeChanged` 误报 |
| P2 | `Decoders.cpp:172-196` | `hw_pix_fmt_` 计算后完全未用；`get_format` 未校验 device_type、未设 `hw_frames_ctx` |

### 3.2 分析引擎 / 并发 / 任务调度

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 | `MediaPlayer.cpp:1063/1080` | BlockingIo detach 后访问 `task_manager_`（UAF） |
| P0 | `MainWindow.cpp:102/845-857` | 析构后仍 invokeMethod 到已销毁对象 |
| P0 | `AnalysisEngine.cpp:349-369` | MP4 布局扫描/全表解析无取消点 → 第二次分析永久排队 |
| P0 | `QtAnalysisController.cpp:23-29` | 析构 `worker_.join()` 无上限 → 关窗永久阻塞 |
| P1 | `TaskManager.cpp:128-150` | 并发满时**先取消旧任务**再判上限（131 行置 cancel，147 行才判），旧任务被取消却无新任务接管 → UI 停在"分析中" |
| P1 | `QtWorkerOwner.cpp:58-62` | `body()` 抛异常则 `thread->quit()` 不执行 → 线程卡在 exec，`IsActive()` 恒 true 导致后续导出永远排队 |
| P1 | `QtWorkerOwner.cpp:85/92-103` | `entries_` 无锁却支持跨线程调用，vector 扩容让已取得的裸 `Entry*` 失效 |
| P1 | `AnalysisFacade.cpp:36-39` | 过期 generation 结果**仍无条件 emit**，完全依赖每个页面各自过滤；新页面漏一次就旧结果覆盖（建议编排层直接拦截） |
| P1 | `AnalysisEngine.cpp:661/932-1052` | 取消后仍跑完整汇总并发 100% 进度 |
| P2 | `Logger.cpp:53/111` | `current_level_` 无锁读；每行 `flush()` + 直接 `std::cout` |
| P2 | `ConfigManager.cpp:16-38` | 持锁做整文件 IO 与解析，且锁内调 LOG（锁序 Config→Logger） |
| P2 | `TaskManager.cpp:390/199` | `abandoned_` 非原子；`orphans_` 不计入并发上限 |
| P2 | `StreamAnalyzer.cpp:102-104` | 每包 `erase(begin())` O(n)，热路径每次加锁 + 两次 `now()` |
| P2 | `QtWorkerOwner.cpp:28-34/64-68` | 析构脱管后 `deleteLater()` 永不执行，QThread + worker 双重泄漏 |

### 3.3 core/analysis/codec + core/media/codec —— 码流解析

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 | `HevcBitstreamParser.cpp:519`→`:234` | `num_short_term_ref_pic_sets` 无上界 64 → 4GB 分配 / 数十亿次空转 |
| P0 | `H264BitstreamParser.cpp:302` | `num_ref_frames_in_pic_order_cnt_cycle` 无上界（≤255），循环内不检查 `HasError()` |
| P0 | `H264BitstreamParser.cpp:399-407` | `1u<<32` UB + `pic_size_in_map_units_minus1` 无上界 |
| P0 | `VvcBitstreamParser.cpp:463/476/492/605/690` | 多个 ue(v) 计数无上界；`:492` `x+1` 回绕成 0 → `SkipBits(0)` 静默错位 |
| P0 | `VvcBitstreamParser.cpp:183`→`:156`, `Hevc:395/545` | hrd_cpb_cnt / num_layer_sets 无上界；`1 << (a+b)` 有符号左移 UB |
| P1 ✅ | `ExtradataParser.cpp:202-203` | HEVC NAL type 移位错 → HEVC 参数全空（**测试用 MakeNal 绕过，测不到**） |
| P1 | `ExtradataParser.cpp:494-503` | `ParseHvcC` 是桩，不解析 VPS/SPS/PPS 数组 |
| P1 | `ExtradataParser.cpp:736-740` | 长度前缀流错走 `ParseAnnexB`，恒 0 个 NAL；`ConvertLengthPrefixToAnnexB`/`ReadLengthPrefix` 是死代码 |
| P1 | `HevcBitstreamParser.cpp:196-226` | hrd_parameters 位预算错：`tick_divisor_minus2` 按 u(4) 且无条件跳过（应 u(8)、仅 sub_pic_hrd）；scale 按 5 位（应 4）；漏 3 个 5 位字段；子层循环漏读 `cpb_cnt_minus1` |
| P1 | `VvcBitstreamParser.cpp:719/769/392` | SPS/PPS/VPS 无 `HasError()` 检查即置 `present`（VPS 顺序还反了） |
| P1 | `BitstreamAnalyzer.cpp:186` | `PopulateAv1Config` 无条件调用 → 非 AV1 也置 present，`AnyParameterSet()` 据此误判成功并制造假告警 |
| P1 | `ExtradataParser.cpp:71/86` | av1C 判据要求 `data[0]&0x80`，而 OBU 头 forbidden bit 必须为 0 → 恒不满足，裸 OBU 流不可用 |
| P1 | `VvcBitstreamParser.cpp:447` | `static_cast<int>(ReadUE())`，`ReadUE` 可返回 0xFFFFFFFE → 负值分辨率 |
| P2 | `H264BitstreamParser.cpp:113` | `cpb_cnt_minus1>31` 时 return 不置 error，半初始化 VUI 被标 present |
| P2 | `ExtradataParser.cpp:9` | `FindStartCode` 要求 `pos+4<=end`，文件末尾 3 字节起始码搜不到 → 最后一个 NAL 丢失 |
| P2 | `HevcBitstreamParser.cpp:687` | `ParseSeiMessages` 全项目零调用，MDCV/CLLi 只能依赖 FFmpeg |
| P2 | `BitReader.cpp:142` | `(total-byte_pos)*8` 转 int，>256MB 溢出成负 → 误判位数不足 |

### 3.4 core/analysis/container + core/media —— 容器解析

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 ✅ | `IsobmffParser.cpp:537-549` | 嵌套 trak → vector 扩容 → `ctx.trak` 悬垂（堆 UAF） |
| P0 | `EbmlAnalyzer.cpp:787` | master 递归无深度上限 → 栈溢出 |
| P0 | `EbmlAnalyzer.cpp:814/488` | 未知长度叶子不 skip → 全文件错位 + 节点爆炸 OOM |
| P0 | `EbmlAnalyzer.cpp:386/406/427` | VINT 宽度未限制 ≤8 → 每次偏移 1 字节，连锁崩坏 |
| P0 | `EbmlAnalyzer.cpp:879` + `ContainerStructureAnalyzer.cpp:164-179` | 大 MKV 全文件入树再复制一份，无节点上限（MP4 侧有，EBML 侧无） |
| P1 | `IsobmffParser.cpp:38/543` | `stsd`/`meta` 未跳 FullBox 头 → 子 box 错位 8 字节，`si.codec` 取到乱码，宽高/采样率取不到（**UI 直接显示乱码**） |
| P1 | `IsobmffParser.cpp:242-254` + `Mp4SampleTableAnalyzer.cpp:597` | stco/co64 共存时偏移表被拼接、chunk_count 翻倍；`tables_truncated` 未透传 → 撞上限后疯狂误报 |
| P1 | `IsobmffParser.cpp:582-586` | `SttsSampleCount/CttsSampleCount` uint32 累加溢出 → 误判 `kSampleCountMismatch` |
| P1 | `IsobmffParser.cpp:431-442/478` | largesize 用 `pos + size` 判断溢出（应 `size > end - pos`）→ 回绕重复解析 + 伪表 box 反复 64MB 分配 |
| P1 | `FlvStructureAnalyzer.cpp:235/261/268` | `file.read(1).at(0)` 截断时越界；`header_size` 未校验 ≥9 可回退到头部 |
| P1 | `AsfStructureAnalyzer.cpp:140/152/187` | `obj_size - 24` 无符号下溢（校验在 212 行，太晚） |
| P1 | `ContainerStructureAnalyzer.cpp:104-223` | `cancel` 在三条自研解析路径上完全失效，只给了 FFmpeg 回退分支 |
| P2 | `IsobmffParser.cpp:66/425` | 逐盒 seekg + 8 字节读，无缓冲/mmap，fMP4 几十万 moof 时是 IO 轰炸 |
| P2 | `Mp4SampleTableAnalyzer.cpp:493` | `max_samples_per_track==0` 表示不限，却按声明的 sample_count `reserve` → 4G 样本直接 bad_alloc |
| P2 | `EbmlAnalyzer.cpp:480/880` | 每块 `data.mid()` 拷贝、`countBlocks` 全树递归、QDataStream 逐字节虚调用 |

> 已确认无问题：`timescale=0` 全部防护到位；`IsobmffParser::ReadAt` 与 `FileProbe` 的短读判定正确。

### 3.5 core/analysis/quality —— 画质与视觉缺陷

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 | `QualityAnalyzer.cpp:90/102-109` | VMAF 空桩 NaN 静默落库并置 valid |
| P1 ✅ | `VisualDefectAnalyzer.cpp:619-706` | **马赛克（Blockiness）检测是死代码**：`blockiness_score` 算出来了，但 9 类 `SegmentUpdate` 唯独没有它，`SeverityFor:863`/`MinSecondsFor:41` 都为它留了分支 → 花屏永不告警 |
| P1 | `VisualDefectAnalyzer.cpp:244` | 块尺寸 `bs` 单位是采样图像素，未按缩放比换算：1920→256 时源 8×8 块只剩 1.07px，边界差分≈内部差分 → 分数恒趋 0（即使补上段更新也检不出） |
| P1 | `VisualDefectAnalyzer.cpp:762-763` | 缺陷时长少一个采样间隔，单次命中 duration=0 必被丢弃；`end_seconds` 取最后命中样本而非首个未命中 → 时长系统性偏短，1s/3s 分级被低估 |
| P1 | `VisualDefectAnalyzer.cpp:190-211` | 模糊（拉普拉斯方差）未做分辨率归一化，4K 与 480p 用同一阈值 → 高分辨率误报、低分辨率漏报；`blur_min_luma_std=0.5`（0.5 灰阶）闸门形同虚设 |
| P1 | `VisualDefectAnalyzer.cpp:578-581` | 冻结只与前一个采样帧比一次，无多帧确认 → 静态场景 + 轻噪声误报 |
| P1 | `VisualDefectAnalyzer.cpp:815-840` | 闪烁 `(void)ts` 丢弃时间参数，不过滤场景切换 → 快速剪辑误报；2fps 采样对 25/50Hz 工频闪烁物理上无法采样 |
| P1 | `QualityAnalyzer.cpp:182/217` | sws 未设 `sws_setColorspaceDetails`、不读 `color_range` → full-range 素材黑电平偏移约 16 灰阶，黑场/欠曝/过曝在 full-range 上成片误判 |
| P1 | `VisualDefectAnalyzer.cpp:302-309` | 黑边扫描无连续性要求，噪声会让扫描提前停住 → 漏报；`letterbox` 要求两侧都 >0，一侧被截断整帧不判 |
| P1 | `VisualDefectAnalyzer.cpp:218-237` | 梳齿对所有 y 求行差均值，把场内相邻行差混进来稀释信号 → 漏报（应只统计 y%2==1 的场交界） |
| P2 | `QualityAnalyzer.cpp:182/217` | 每帧两次 `sws_getContext/freeContext`，应按 (fmt,w,h) 缓存 |
| P2 | `VisualDefectAnalyzer.cpp:537` | `duration_seconds` 用被 `max_samples` 截断后的首尾算，长视频严重低估，seek 回跳出负值 |
| P2 | `VisualDefectAnalyzer.cpp:288/709` | 每帧重分配 row/col_mean vector；`prev_sample_` 每帧深拷贝 |
| P2 | `MacroblockAnalyzer.cpp:137` | `intra_count = total_mb - mvs.size()`，把 HEVC 的 PU 数当 MB 数减 → intra_count 可为负（靠 max(0,) 掩盖） |
| P2 | `VisualDefectAnalyzer.cpp:731` | 每段首帧复制 RGB 证据图，`max_defects=2000` 时峰值约 50MB |

### 3.6 core/analysis/diagnostics + core/qc —— 诊断与 QC 规则

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 ✅ | `Scte35Analyzer.cpp:420` | `splice_descriptor_tag` 按 16 bit 读（规范 8 bit）→ segmentation 全错（fixture 也写错了） |
| P0 | `SubtitleAnalyzer.cpp:593/559` | 重叠检测只比"上一条"且 `prev_end` 不取运行最大值：A[0,10]、B[2,3]、C[4,5] 时 C 与 A 的真实重叠漏报；`ValidateCues` **从不排序**，乱序输入重叠几乎全漏 |
| P0 | `QcReport.cpp:33-51` + `QcRuleEngine.cpp:163` | 一条 Critical 只扣 30 分 → 70 → 判"警告"，**致命问题永远进不了"不通过"**；`HasBlockingIssue()` 已定义但全项目零调用 |
| P1 | `QcRuleEngine.cpp:37-43` + `QcModels.cpp:427/432/447` | 字幕 `too_short/too_long/too_fast` 用 `NonZero` 判定，threshold（0.5s/8s/21字每秒）**被完全忽略**，UI/模板改阈值无效 |
| P1 | `SubtitleAnalyzer.cpp:486` | `end<start`（非法时长）在带 duration 的封装里被 PTS 静默改写 → `InvalidDuration`（Error 级）永远报不出 |
| P1 | `SubtitleAnalyzer.cpp:530` | `end_seconds > 0.0` 守卫让 `[0.0,0.0]` 零时长 cue 完全跳过检查 |
| P1 | `SubtitleAnalyzer.cpp:552-568` | `else if` 导致已报 NonMonotonic 时不再报 Overlap，重叠计数被吞 |
| P1 | `Scte35Analyzer.cpp:310-312` | 未知 `splice_command_type` 直接 return false，丢弃整段 descriptor loop（规范允许按 command_length 跳过） |
| P1 | `Scte35Analyzer.cpp:461` | CRC 失败仍 `cue.valid = true`，损坏 cue 仍计入统计 |
| P1 | `Scte35Analyzer.cpp:215-217` | splice_schedule 的 GPS 秒写入 `splice_time_seconds`（媒体秒）→ 单位混用 |
| P1 | `QcComparator.cpp:69-72` | `scale = max(1.0, fabs(left))` → `CompareRuns(a,b)` 与 `(b,a)` **结论不对称** |
| P1 | `QcComparator.cpp:216-246` | 码率/GOP 各行 `available` 恒 true，两侧都没跑深度分析时 0 vs 0 判"一致" → 缺项算通过 |
| P1 | `BatchQcRunner.cpp:84-86` | `if (ec) break` → 单个子目录权限失败中止整个遍历且不记错误 |
| P1 | `QcModels.cpp:67-68` | `container.duration_invalid` MinBelow 0.01 → 时长未采集（0）直接 -30 分，缺项算不通过 |
| P2 | `TimecodeInfo.cpp:147-161` | 3 位小数 + `.` 判定与"`.` = drop frame"自相矛盾；进位只加到 seconds，`59.999` → `00:00:60` |
| P2 | `TimecodeAnalyzer.cpp:223` | `first_presentation_seconds` 写了从不读 → 时码↔PTS 一致性校验缺失；`check_drop_frame` 零使用 |
| P2 | `SubtitleAnalyzer.cpp:345` | WebVTT 未剥 BOM（只在包路径剥）→ 带 BOM 文件找不到 `WEBVTT` 头 |
| P2 | `SubtitleAnalyzer.cpp:405-413` | CEA-608 奇数码丢弃末字节、未分离 field1/field2、未处理 0x00 填充与重复 PAC |

### 3.7 core/analysis/streaming —— HLS / DASH

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 | `DashManifestAnalyzer.cpp:228` | `<S r=>` 为负 → 42.9 亿次循环；内层无取消检查，超上限只 return 不 break → 卡死且不可取消 |
| P0 | `DashManifestAnalyzer.cpp:232/644` | `cursor = start + e.d*(r+1)`，`d`/`r` 无符号转换后回绕 → 整条时间轴倒流 |
| P1 | `DashManifestAnalyzer.cpp:553/495` | Rep 级 SegmentTemplate 一旦存在就整段跳过 AS 级继承，timescale 默认 1 → **分片时长放大 90000 倍** |
| P1 | `DashManifestAnalyzer.cpp:210` | `decode_time - presentation_time_offset` 两个 uint64 相减，@t < PTO 时下溢成 1.8e19 → 关键帧对齐全错 |
| P1 | `DashManifestAnalyzer.cpp:196/205` | `JoinPath`→`NormalizePath` 把 `http://cdn/p` 压成 `http:/cdn/p`；`IsRemoteUri` 只查 seg.uri 不查 base_url → 远程包全量报"分片不存在" |
| P1 | `DashManifestAnalyzer.cpp:420-429` | 未解析 minimumUpdatePeriod，live MPD 静默产出 0 分片且无任何 issue |
| P1 | `HlsManifestAnalyzer.cpp:517` | BOM 未剥离 → `t == "#EXTM3U"` 失败 → 整份清单被判"不是 HLS" |
| P1 | `HlsManifestAnalyzer.cpp:528` | 标签比较大小写敏感 → 小写标签的 master 被判为 media → 整份清单解析为空 |
| P1 | `HlsManifestAnalyzer.cpp:185/195/201` | `has_endlist`/`playlist_type_vod`/`media_sequence` 解析了但 Validate 从不检查；EXTINF 累加从不与 tfdt 比对 |
| P1 | `SegmentQcAnalyzer.cpp:328-330` | 统计缺失分片时未跳过 `seg.gap` → EXT-X-GAP 的合法缺失被误报 |
| P1 | `SegmentQcAnalyzer.cpp:451-511` | 只探测前 8 个分片，关键帧列表为空则整项静默跳过且不告知用户 |
| P2 | `DashManifestAnalyzer.cpp:495/514` | timescale 强转 uint32（>2^32 截断为 0）；`<S>` 未校验 d>0，d=0 时 cursor 不前进 |
| P2 | `HlsManifestAnalyzer.cpp:229/221` | BYTERANGE 解析失败保留上次值串到下一片；KEY URI 从不解析 |
| P2 | `SegmentInfo.h:92-95` | `MeasuredBitrateBps` 对 BYTERANGE 分片用整文件大小 → 码率虚高 → 误报带宽超标 |
| P2 | 三个分析器全不联网 | 远程包只置 `remote` 标记，可用性检查全跳过且无提示 |

### 3.8 core/exporter + core/ffmpeg —— 导出与命令工作台

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 ✅ | `MediaExporter.cpp:177` + `:214` | 临时文件后缀让 `av_guess_format` 失败 → **导出 100% 失败** |
| P0 | `FfmpegProcessRunner.cpp:175` | Windows `kill()` 只杀单进程，子进程树残留继续写文件 |
| P1 | `MediaExporter.cpp:272` + `FfmpegInterrupt.cpp:8/31` | 中断回调只装 `&cancel_`，不认 `cancel_token_` → 走 TaskManager 令牌取消（换媒体主路径）时阻塞 IO 打不断 |
| P1 | `MediaExporter.cpp:573-578` | sws 只在首帧创建，可变分辨率流后续帧按旧尺寸 scale → 越界读/花屏（`VideoFrameExporter.cpp:300` 用 `sws_getCachedContext` 是对的） |
| P1 | `FfmpegProcessRunner.cpp:120/158` | 无整体超时 watchdog；`-i pipe:0` 时写 `"q\n"` 会污染 stdin 数据 |
| P1 | `FfmpegCommandParser.cpp:180-185` | 引号内无法转义引号，`drawtext=text='a"b'` 这类滤镜表达式无法表达 |
| P2 | `MediaExporter.cpp:681` | 进度取当前包 pts，音视频交替时**倒退**；`duration_ms==0`（流媒体）时恒 -1，进度永远停在 0 |
| P2 | `MediaExporter.cpp:447/452` | 像素格式取 `supported[0]`（可能是 yuv444p）；mp4 未设 `+faststart` |
| P2 | `FfmpegProcessRunner.cpp:196-224` | `pending_*` 无上限，二进制/无换行输出无限增长并逐行 emit → 跨线程信号风暴 |
| P2 | `FfmpegCommandParser.cpp:285/307` | 输出文件名以 `-` 开头被当选项；未知选项会吞掉紧跟的输出文件 |

### 3.9 core/reporting + infrastructure/serialization —— 报告与 JSON

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 ✅ | `QcReportExporter.cpp:286/301/316` | PDF 对象编号错位 → **所有 PDF 不可读** |
| P1 | `QcReportExporter.cpp:378-382` | `EscapePdfText` 把 WinAnsi 外码点全替换 `?` → 中文报告几乎只剩问号（"致命/错误/警告/提示"、"全局"、verdict 全是中文） |
| P1 | `QcReportExporter.cpp:67-75` | CSV 未对以 `= + - @` 开头的字段加前导 `'` → **Excel 公式注入** |
| P1 | `Json.cpp:78/102` | 不跳过 UTF-8 BOM → 记事本另存的模板直接加载失败，只报"第 1 行附近解析失败" |
| P1 | `QcReportExporter.cpp:467-483` | JSON 丢失 `partial`（抽样扫描）标记；`report.color_hdr` 从未导出；`analysis_elapsed_ms` 被 `run.elapsed_ms` 取代 |
| P1 | `Json.cpp:65-69` | 浮点 `%.10g` 无法无损往返（需 17 位）；`-0.0` 被打印成 `0` 丢符号 |
| P1 | `QcReportExporter.cpp:552-557` | `SingleLine` 只作用于 detail/suggestion，`file_name`/`rule_id`/`title`/`range` 直接进 CSV → 含换行把一行拆两行 |
| P2 | `Json.cpp:225-236` | 孤立代理对编码成 CESU-8 非法 UTF-8（应替换 U+FFFD） |
| P2 | `Json.cpp:250-268` | 数字解析过宽（接受 `+1`、`1.2.3.4`、`1e`），且不检查 `ERANGE` |
| P2 | `Json.h:37` | 无 `JsonValue(int64_t)`，全部走 double，>2^53 失真 |
| P2 | `QcReportExporter.cpp:42-49` | 无目录创建、无临时文件+rename 原子写；`StreamStatsExporter.cpp:84/122/162` 连 `good()` 都不检查 |
| P2 | `StreamStatsExporter.cpp:209-211` | `fps_history` 全零 → `max_fps=0` → NaN 写进 CSS `height: nan px` |

### 3.10 ui —— 界面层

| 级别 | 位置 | 问题 |
|---|---|---|
| P0 ✅ | `PlayerPanel.cpp:218-225` + `:1151-1275` | Raw 序列拖动无节流，主线程逐个像素整帧解码 → 界面冻死 + 弹窗风暴 |
| P1 ✅ | `AnalysisPanel.cpp:593` | `isVisible()` 恒 false → 播放期所有表/曲线一行不刷新 |
| P1 | `DiagnosticsPage.cpp:287-289` | generation 失配时直接 return 不复位按钮 → **"开始分析"永久禁用**，只能重启软件；`ResetForNewFile` 也不 cancel 在跑的扫描 |
| P1 | `DiagnosticsPage.cpp:241-244/358-361` | 自动扫描路径注释写"静默"，实际 `IsRunning()` 时弹模态框；MainWindow 每次打开文件都调 → 弹窗 |
| P1 | `DiagnosticsPage.cpp:661-670` | 帧间隔曲线全量 Add（每帧一点），长视频十几万点，每 120ms 全量重建（码率页同场景用了 `AppendDecimated(...,4000)`） |
| P1 | 图表 6 处 | `ChartSeries::Replace/Append`、`ChartAxis::SetRange` 均不触发重绘，全项目仅 `SubtitleAuxPage.cpp:580` 调了 `chart->update()` → 改了数据不刷新 |
| P1 | `AnalysisPanel.cpp:209-248` | 开关关闭时 Reset 被跳过 → 5 万行记录与曲线不清，重新勾选后新旧数据混在一张表 |
| P1 | `PlayerPanel.cpp:1330-1337` | MV 按钮只调 `SetMacroblockAnalysisEnabled`，不回写 `feature_enabled_[]` → 下次打开文件被置回 false，UI 仍显示勾选 |
| P1 | `ReportingPanel.cpp:384-387` | 单文件分析的"停止"按钮无效（共用 cancel_button_，但 `OnCancelBatch` 只操作 `batch_task_`） |
| P1 | `MainWindow.cpp:720-740` | Raw 图像分支不更新 `SetCurrentVideoPath` → 上一份文件路径与扫描结果残留 |
| P2 | `MainWindow.cpp:770-777` | PCM 分支在主线程跑 FFmpeg 探测（正常路径已后台化） |
| P2 | `FramePacketView.cpp:31/568/903` | 5 万行 QTableWidgetItem + 每次 scrollToBottom + 筛选切换全表重建 |
| P2 | `ContainerStructurePage.cpp:859-875` | 点击触发两次 5000 行样本表重建；树无行数上限且 `expandAll()` |
| P2 | `MetricChartWidget.cpp:495-529` | 每次鼠标移动全量 HitTest + ComputeGeometry |
| P2 | `VideoWidget.cpp:429-433` | paintEvent 内做整帧 SmoothPixmapTransform（应在 SetFrame 时缓存） |
| P2 | `MainWindow.cpp:348-357` | resizeEvent 内调 resize() → 递归 resize 链 |

---

## 四、测试覆盖缺口（共性问题）

审查中发现的"测试写错导致 bug 被掩盖"比"没有测试"更危险：

1. **fixture 跟着错误实现写** — `test_subtitle_timecode_aux.cpp:120` 用 `out.Write(0x02, 16)` 迎合了 16 bit 的 bug；`test_hevc_bitstream_parser.cpp:81` 用 `MakeNal` 手工塞 type 绕开真实解析路径。
2. **断言过弱** — `test_vvc_bitstream_parser.cpp:292 GarbageInputDoesNotCrash` 只断言"不崩"，因 `present` 恒真实际放行了垃圾值。
3. **只测分值不测产出** — `test_visual_defect.cpp:229` 只验 `ComputeFrameMetrics` 的分值，从不验缺陷产出，正好掩盖"马赛克是死代码"。
4. **关键成功路径完全缺失** — `test_media_exporter.cpp` 没有任何成功路径用例，导出 100% 失败也测不到。
5. **零覆盖模块** — EBML、FLV、TS、ASF 无任何单测；QC 对比只 3 例且不测对称性。
6. **无异常输入测试** — 全部单测都是合法码流/清单，**没有一条喂入超大 `ue(v)`、负 `r`、未知长度 element、截断文件**。上述 P0 中的 OOM/死循环/UAF 路径全部裸奔。

**建议**: 补一轮"恶意/畸形输入"模糊测试（libFuzzer 或手写 corpus），覆盖容器、码流、清单、字幕四类解析器；并为导出/PDF 补端到端成功用例 + 产物校验。

---

## 五、修复路线建议

### 第一批：功能归零类（1～2 天）
导出全废、PDF 不可读、HEVC 参数全空、播放期 UI 不刷新、Raw 拖动冻死 —— 都是"用户一上手就撞上"的问题。
`MediaExporter.cpp:177` → `QcReportExporter.cpp:316` → `ExtradataParser.cpp:202` → `AnalysisPanel.cpp:593` → `PlayerPanel.cpp:218`

### 第二批：崩溃与卡死类（3～5 天）
丢失唤醒、中断回调置空、UAF、无取消点、无上界循环。重点是给**所有自研解析器**统一加"上界 + 深度 + 节点数 + 取消点"四件套。
`PlaybackSession.cpp:92` → `MediaPlayer.cpp:293/1063` → `MainWindow.cpp:845` → `AnalysisEngine.cpp:349` → `IsobmffParser.cpp:537` → `EbmlAnalyzer.cpp:787/814/386` → 各 codec parser 的 ue(v) 上界 → `DashManifestAnalyzer.cpp:228`

### 第三批：判定正确性类（1～2 周）
马赛克死代码、字幕重叠漏判、QC 结论脱钩、DASH 继承与下溢、full-range 色彩、各类归一化。这类需要**先补基准测试再改**，否则改完无法验证方向对不对。
`VisualDefectAnalyzer.cpp:619/244/762` → `SubtitleAnalyzer.cpp:593` → `QcReport.cpp:33` → `DashManifestAnalyzer.cpp:553/210` → `QualityAnalyzer.cpp:182`

### 第四批：性能与体验
每帧分配、QTableWidgetItem 换 model、图表抽稀与重绘、日志 flush、IO 缓冲。

---

## 附：复核记录

以下条目已由审查者回读源码逐条确认行号与成因，非推断：

| 条目 | 位置 | 复核结论 |
|---|---|---|
| HEVC NAL type 移位 | `ExtradataParser.cpp:202` | 确认：`(header>>3)&0x3F` 取的是 bit8..3（layer_id），正确为 `(data[0]>>1)&0x3F` |
| 嵌套 trak UAF | `IsobmffParser.cpp:537-549` | 确认：`emplace_back` 后 `ctx.trak = &back()`，扩容即失效 |
| SCTE-35 tag 位宽 | `Scte35Analyzer.cpp:420` | 确认：`Read(16)`，规范为 8 bit |
| 导出格式推断 | `MediaExporter.cpp:177/214` | 确认：`av_guess_format` 三参为空，后缀为 `part-<pid>-<uuid>` |
| PDF 对象编号 | `QcReportExporter.cpp:286/301/316` | 确认：insert 在编号之后，`/Contents 4 0 R` 指向字体字典，`/Kids` 指向内容流 |
| UI 可见性 | `AnalysisPanel.cpp:593` + `MainWindow.cpp:274` | 确认：panel 从未 `addWidget` 进 stack，`isVisible()` 恒 false |
| 丢失唤醒 | `PlaybackSession.cpp:92/263` | 确认：atomic 锁外改 + `cv_.wait(lock, pred)` 锁内求值 |
| 马赛克死代码 | `VisualDefectAnalyzer.cpp:243/619-690` | 确认：算了 score，但段更新列表无 Blockiness |

---

# 修复记录（2026-10-03）

按报告的四批顺序执行。共改动 38 个文件（+1049 / -263），
**全部通过 MSVC `/Zs` 语法检查**（`scripts/syntaxcheck.py`，32 个受影响编译单元全 OK）。

## 第一批：功能归零类

| 问题 | 修复 |
|---|---|
| 导出 100% 失败 | `MediaExporter.cpp` — `open_output` 改用 `av_guess_format(nullptr, opt.format, nullptr)` 显式指定封装；临时文件名不再决定 muxer |
| 打开/导出不响应取消 | `FfmpegInterrupt` 增加 `AvInterruptState::cancel` 与 deadline 双源；`MediaExporter` 复用 `cancel_`，`MediaPlayer::Stop()` 主动置位中断（原来 Stop 后网络源仍卡在 `av_read_frame`） |
| sws 不随分辨率重建 | `StreamCtx` 记录 `sws_src_w/h/fmt`，任一变化即重建 |
| 进度倒退/卡在 0 | 进度改取各流最大 PTS（单调），不再用单流 PTS |
| PDF 一律不可读 | `QcReportExporter` — 对象编号在 **Kids 插入之前**算完；`/Contents`、`/Kids` 引用与 xref 对齐 |
| PDF 中文变 `?` | 改为按 run 生成内容流（WinAnsi 段用 F1，中文段走 UTF-16BE + 内置 CID 字体），不再替换成 `?` |
| CSV 公式注入 | `CsvField` 对以 `=` `+` `-` `@` 开头的字段前置 `'`，并对齐 RFC4180 转义；换行在 `CsvField` 内统一归一（原来只有两个字段走了 `SingleLine`） |
| JSON NaN/Infinity | `AppendNumber` 输出 `null`；解析加嵌套深度上限与非法输入保护 |

## 第二批：崩溃 / 卡死类

| 问题 | 修复 |
|---|---|
| HEVC 分辨率/profile/level 全空 | `ExtradataParser.cpp:202` 改为 `(data[0] >> 1) & 0x3F`；并补完 `ParseHvcC`（原为只跳过 20 字节的桩），长度前缀 extradata 转 Annex-B 后再解析 |
| 嵌套 trak / traf 堆 UAF | `IsobmffParser` 的 `ParseContext` 把 `trak`/`frag` 裸指针改成**下标 + 访问器**，vector 扩容不再失效 |
| largesize 溢出 | 边界一律用减法（`size > end - pos`），避免 `pos + size` 回绕 |
| stsd / meta 是 FullBox | 新增 `FullBoxSkip(type)`，递归子 box 前跳过 version/flags(+entry_count)，不再把头解析成 fourcc 全 0 的垃圾节点 |
| stts/ctts 计数回绕 | 改 64 位累加 + 饱和到 `UINT32_MAX` |
| EBML 未知大小死循环 | 未知 size 元素原来 push 完就 `continue`、一个字节都不消费 → 同一元素被无限解析。现改为：容器按"延伸到父容器末尾"递归，叶子直接 seek 到父末尾 |
| EBML 递归无深度上限 / 无节点上限 | `kMaxDepth=64`、`kMaxNodes=200000`，`Analyze` 入口复位计数 |
| EBML VINT 宽度越界 | `first==0` 时 width 会算成 9；三处 VINT 读取统一加 `width >= 8 → 非法` |
| 各 codec parser ue(v) 上界 | HEVC `num_short_term_ref_pic_sets` / `num_long_term_ref_pics_sps`、`num_layer_sets`、VVC `sps_num_subpics_minus1` / `sps_num_ref_pic_lists` / 虚拟边界 / `subpic_id_len` / HRD `cpb_cnt`、H264 `num_ref_frames_in_pic_order_cnt_cycle` / `pic_size_in_map_units_minus1` / `cpb_cnt` / `num_slice_groups` 全部封顶；`BitReader` 补 `AvailableBits()` |
| VVC SPS/PPS/VPS 错误检查 | `present` 仅在 `!HasError()` 时置位（原来错误码流也返回"解析成功"） |
| Stop/Play 丢失唤醒 | `PlaybackSession` 的状态变更与 `notify_one` 全部移入同一把锁内 |
| 硬解帧被当 CPU 帧 | `VideoDecoder` 增加复用 `sw_frame_`，`GetLastRawFrame()` 返回**下载后的**帧；`Close()` 释放 |
| EAGAIN 丢包 | `SendPacket` 用 `pending_frames_` 队列先收帧腾位置，不再静默丢包 |
| `drop_until_sec_` 只由视频帧清除 | 音频路径同样清除 |

## 第三批：判定正确性类

| 问题 | 修复 |
|---|---|
| SCTE-35 segmentation 全错 | descriptor tag 改 8 bit（规范），循环守卫 `+6`；**同步修正** `test_subtitle_timecode_aux.cpp` 的 fixture（它按 16 bit 写，跟着错误实现跑） |
| 马赛克是死代码 | 补上缺失的 `Blockiness` 段更新；块尺寸按源分辨率/样本宽度缩放比换算，不足 2px 时置 `NoValue`（宁可报"量不到"，也不给假阴性） |
| 缺陷时长少一个采样间隔 | `CloseSegmentLocked` 补 `sample_interval_`（平滑估计） |
| YUV→GRAY8 未做 range 映射 | `ApplyColorRange()`：未声明 range 的 YUV 按 limited 处理，输出 full，绝对亮度阈值不再整体偏移 ~6% |
| 字幕重叠漏判 | `prev_end` 改为单调不减（长字幕后的短字幕不再被漏掉） |
| QC 结论脱钩 | `ComputeQcVerdict(score, has_critical, has_error)`：有 Critical 直接"不通过"，有 Error 封顶"警告"（原来一条 Critical 只扣 30 分 → 判"警告"，永远进不了"不通过"） |
| DASH `<S r=N>` 空转 | 到分片上限即 `break`；`e.d * repeats` 先判溢出 |
| DASH PTO 下溢 | `decode_time - presentation_time_offset` 先比后减 |
| DASH 继承越界 | `period_index` / `adaptation_index` 补上界检查 |
| DASH 远程 BaseURL | `IsRemoteUri` 时不走本地 `JoinPath` |
| EXT-X-GAP 误报缺失 | 缺失分片统计跳过 `seg.gap` |
| 清单 BOM | `ManifestReader` 三处入口统一剥 UTF-8 BOM（带 BOM 的 m3u8 原来被判"不是 HLS 清单"） |

## 第四批：UI 与体验

| 问题 | 修复 |
|---|---|
| 播放期表格/曲线一行不刷新 | `AnalysisPanel::IsAnyPageVisible()` 用"stack 当前页是否属于本面板"替代恒 false 的 `isVisible()` |
| 静默扫描弹模态框 / 按钮永久禁用 | `DiagnosticsPage::StartScan(options, silent)`；打开新文件自动扫描走 silent 分支 |
| Raw 序列拖动卡死 | 拖动预览节流 ~100ms；`ShowRawFrame` 的三处 `QMessageBox` 改状态栏提示（拖一次弹上百个模态框） |
| 时间轴曲线不抽稀 | 上限 4000 点、每组取最大值（保尖峰）；问题标记颜色改取本批**最高**严重度（原来最后一个点决定全批颜色，Critical 会被 Info 染蓝） |
| Reset 被功能开关挡住 | `Reset*List()` 不再受 feature toggle 约束——开关只该管"要不要灌新数据"，不该管"旧数据清不清" |
| 图表改数据不重绘 | `ChartAxis` / `ChartSeries` 持有 owner，所有 setter / `Append` / `Replace` 自动触发 `update()` |

## 遗留（本次未做，建议单独排期）

1. **单测未跑**：沙箱拦截 MSVC 链接器，无法构建测试可执行文件。改动只做了 `/Zs` 语法检查，逻辑正确性待实机验证。
2. **测试迎合错误实现**的模式仍在：建议补一轮畸形输入模糊测试（容器 / 码流 / 清单 / 字幕四类解析器），并给导出与 PDF 补端到端成功用例 + 产物校验。
3. EBML / FLV / TS / ASF 仍是零单测。
4. 马赛克检测在默认档位（160~384 宽降采样）下会因为块尺寸不足 2px 主动放弃 —— 若要真正可用，需要为该项单独开一档高分辨率通路。
