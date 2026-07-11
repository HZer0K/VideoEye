# VideoEye 优化与功能拓展规划 —— Vulkan 深化应用

> 目标：在「检查当前项目真实状态」的基础上，给出可落地的优化与功能拓展路线。
> 核心发现：**Vulkan 目前几乎完全未被真正使用**——渲染管线代码已写好，但从未接入运行路径，且渲染端完全没有 Windows 支持。本文给出从「接通渲染」到「用 Vulkan 做分析计算」的四阶段路线。

---

## 0. 现状诊断（基于代码实证）

### 0.1 现有 Vulkan 资产（已实现，部分可用）

| 模块 | 文件 | 状态 |
|------|------|------|
| 设备/实例管理 | `core/player/VulkanContext.cpp` | ✅ 完整（实例/设备/队列族/FFmpeg `AV_HWDEVICE_TYPE_VULKAN` 桥接） |
| 渲染管线 | `core/player/VulkanRenderer.cpp` | ✅ 完整（swapchain + compute YUV→RGB + 全屏三角形 present） |
| 表面封装 | `ui/main_window/VulkanVideoWidget.cpp` | ⚠️ 外壳在，逻辑有 bug |
| 着色器 | `core/player/shaders/*.comp/.vert/.frag` + CMake 编译 `.spv` | ✅ 已编译并复制到 `bin/shaders/` |
| HW 解码桥接 | `Decoders.cpp::InitializeWithVulkanDevice` | ✅ 可用，但默认关闭 |

### 0.2 为什么「Vulkan 没用起来」——四个致命断点

```
[断点1] MainWindow 从不创建/挂载 VulkanRenderer
   ui/main_window/MainWindow.cpp:211  →  video_widget_ = new VulkanVideoWidget(...);
   全工程 grep "SetVulkanRenderer" 仅在 MediaPlayer/Widget 定义处出现，MainWindow 从未调用。
   ⇒ MediaPlayer::vulkan_renderer_ 永远为空。

[断点2] 渲染开关与 HW 解码双重耦合
   core/player/MediaPlayer.cpp:762
     if (vulkan_rendering_enabled_ && vulkan_renderer_ && video_decoder_->IsCurrentFrameVulkan())
   ⇒ 只有「Vulkan HW 零拷贝帧」才能走 Vulkan 渲染；而 HW 解码默认关闭（驱动闪退），
     软解帧被排除在外。vulkan_rendering_enabled_ 默认 false（MediaPlayer.h:228 "阶段2"）。

[断点3] Windows 无 Surface 支持
   - VulkanContext::CreateInstance (VulkanContext.cpp:106-112) 实例扩展仅 XCB/Wayland/XLIB，
     无 VK_KHR_win32_surface。
   - VulkanRenderer::CreateSurface (VulkanRenderer.cpp:89-106) 仅 XCB/XLIB，Windows 直接
     LOG_ERROR("Unsupported platform") 返回 false。
   ⇒ 在 Windows（主力开发/使用平台）上根本无法创建可呈现表面。

[断点4] Widget 把 ctx 传成 nullptr
   VulkanVideoWidget.cpp:160  renderer_->Initialize(nullptr, winId(), ...)
   而 VulkanRenderer::Initialize 首句：if (!ctx || !ctx->IsValid()) return false;
   ⇒ 即便走到这里也必然失败。
```

### 0.3 关键利好（减少工作量）

`VulkanRenderer::UploadFrameToTextures`（`VulkanRenderer.cpp:430-466`）**已经支持 CPU 软解
YUV420P/半平面帧的上传**（planar 与 `data[1]/data[2]` 两种布局都处理）。这意味着：

> 只要补上 Windows Surface + 修复接线 + 解除「必须 HW 零拷贝帧」的耦合，
> **软解帧即可直接走 Vulkan 渲染**——无需重写渲染管线。

### 0.4 现状 vs 规划后（目标态）

```
┌─────────────────────── 现状（断链） ───────────────────────┐       ┌──────── 规划后 ────────┐
 解码线程                                                  解码线程
   │ (SW 软解)                                                │
   ▼                                                         ▼
 sws_scale → QImage → QLabel::setPixmap  (CPU 回退, 全量)   VulkanContext(实例+设备+Surface)
   │                                                         │
   │  VulkanRenderer 存在但无人创建/绑定  ✗                   ▼
   │  Vulkan 渲染路径恒不触发  ✗                          UploadFrameToTextures(YUV) → compute(YUV2RGB)
   │                                                         │
   │                                                          ▼
   │                                                      PresentFrame → 叠加分析层(MV/块) → swapchain
   │                                                         │
   │                                                      分析计算也走 compute（直方图/PSNR/场景切换）
└─────────────────────────────────────────────────────────┘
```

---

## 1. 优化方向一：把 Vulkan 渲染真正接起来（P0，最高优先）

让文档里宣称的「GPU 渲染管线」从纸面变成现实，且**默认软件解码 + Vulkan 呈现**（稳定，不依赖易闪退的 HW 解码）。

### 1.1 Windows Win32 Surface（打通主力平台）
- `VulkanContext::CreateInstance`：在实例扩展分支增加
  `#elif defined(VK_USE_PLATFORM_WIN32_KHR) → VK_KHR_WIN32_SURFACE_EXTENSION_NAME`。
- `VulkanRenderer::CreateSurface`：实现 `vkCreateWin32SurfaceKHR`（取 `QWidget::winId()`），
  与现有 XCB 分支并列。注意 `VK_USE_PLATFORM_WIN32_KHR` 由 vulkan-headers 在 WIN32 下定义。

### 1.2 MainWindow 接线（填断点1/4）
- 在 `MainWindow` 初始化时：
  1. 创建 `VulkanContext` 并 `Initialize()`；失败则日志提示、走 CPU 回退。
  2. 创建 `VulkanRenderer`，调用 `video_widget_->SetVulkanRenderer(renderer)`，
     **并把 `vulkan_ctx` 一并传入**（修复 nullptr）；
  3. 经 `MediaPlayer::SetVulkanRenderer(renderer)` + `SetVulkanRenderingEnabled(true)` 接通解码线程。
- `VulkanVideoWidget::SetVulkanRenderer` 改为接收 `VulkanContext*`（或让 Widget 持有 ctx 引用），
  在 `showEvent` 且窗口句柄就绪后再 `renderer->Initialize(ctx, winId(), w, h)`。

### 1.3 解除渲染与 HW 解码的耦合（填断点2）
- 改 `MediaPlayer.cpp:762` 的判定：渲染路径对 **SW 帧同样开放**。
  ```cpp
  if (vulkan_rendering_enabled_ && vulkan_renderer_) {
      // SW 帧：从 video_decoder_->GetLastRawFrame() 取 AVFrame 直接上传
      // HW(Vulkan) 零拷贝帧：同上，纹理已由解码器持有
      const AVFrame* raw = video_decoder_->GetLastRawFrame();
      if (raw) { vulkan_renderer_->PresentFrame(raw); }
  } else { /* 原有 sws_scale → QImage 回退 */ }
  ```
- `VulkanRenderer::PresentFrame` 已接受任意 `AVFrame*`（含 YUV420P），无需改。

### 1.4 Shader 路径健壮性（消除相对路径脆弱性）
- 现状：`kShaderDir = "shaders/"`（`VulkanRenderer.cpp:18`）依赖进程 cwd。
- 改为：`QCoreApplication::applicationDirPath() + "/shaders/"`，或把 `.spv` 编译进 `.qrc`
  用 `QFile`/内存加载（见 P4）。这样从任意目录启动都不会找不到着色器。

### 1.5 默认与稳定性策略
- 默认：**软件解码 + Vulkan 呈现**（稳定、跨驱动）。
- HW 解码（Vulkan/D3D11…）保留为**显式开关**，沿用既有异常兜底——不默认开启，避免「打开即闪退」。
- 保留 `IsVulkanAvailable()` 探测与 CPU 回退，驱动不兼容时自动降级。

**验收**：Windows 播放视频，`VulkanVideoWidget::IsVulkanActive() == true`；任务管理器 GPU 占用上升、`sws_scale` CPU 占用下降；Resize/最小化恢复后 swapchain 自重建正常。

---

## 2. 优化方向二：用 Vulkan 做分析可视化（P1，最能体现「用起来」）

VideoEye 是**分析工具**，Vulkan 的最大价值不是「更顺地放视频」，而是**把分析结果画在画面上**。

### 2.1 运动矢量 / 宏块叠加层（杀手锏）
- 现状：`MacroblockAnalyzer` 已能从 SW 解码帧提取 `MotionVectorInfo`（含 src/dst 坐标、块大小、预测类型）。
  数据只在右侧面板以数字/统计呈现（`AnalysisPanel`）。
- 方案：新增 overlay 渲染管线（独立 pipeline + 实例缓冲）。
  - 数据流：`MacroblockInfoReady` → 收集当前帧 MVs → 上传到 `VkBuffer`（instanced vertex）。
  - 在 `PresentFrame` 之后追加一次 overlay draw：画 **MV 箭头 + 块网格 + 分区大小着色**。
  - 预测类型/幅度用颜色编码（P 绿、B 蓝、幅度越大越红）。
- 价值：用户能在视频上**直接看到运动补偿结构**，这是纯 CPU 面板无法提供的洞察。

```
 MacroblockAnalyzer.AnalyzeFrame(AVFrame)
        │  MotionVectorInfo[]  (src_x,src_y,dst_x,dst_y,w,h,type)
        ▼
 [每帧] 上传 instance buffer ──► overlay pipeline (line-list / instanced)
        │                               ▲
 PresentFrame(YUV→RGB) ────────────────┘  (同一 swapchain image, 追加 subpass/draw)
        ▼
   swapchain 呈现「视频 + MV 叠加」
```

### 2.2 帧类型 / 关键帧标记叠加
- I/P/B 帧类型、关键帧位置以画面角标/边框色实时标注（复用现有 `VideoFrameInfoReady` 数据）。

---

## 3. 优化方向三：Vulkan Compute 加速分析计算（P2）

把分析计算从 CPU/OpenCV 搬到 GPU，与渲染共享同一设备上下文，**零拷贝**读取已上传的 Y 纹理。

### 3.1 GPU 直方图 + 场景切换加速
- 新增 compute shader：对 `y_texture_` 做原子计数，输出 luma/chroma 直方图（`vec4 计数桶`）。
- 输出直接喂给现有 `SceneChangeAnalyzer::Feed(int, double, const vector<float>&)`（接口已兼容）。
- 替代 `FrameAnalyzer::ComputeHistogram` 的 CPU 路径：**免去 AVFrame→CPU→OpenCV 回读**。

### 3.2 GPU 帧差 / 运动强度
- compute 比较相邻帧 Y 纹理，输出绝对差图/梯度，用于：
  - 场景切换 score 的更鲁棒估计（像素级 diff 优于直方图距离，对闪光/渐变更敏感）。
  - 运动强度指标（宏块分析的补充，无需 MV side data 即可估计）。

### 3.3 性能收益
- 当前逐帧 `sws_scale` + OpenCV 直方图 + 宏块分析串行在解码线程；GPU 直方图/帧差可在
  present 时并行计算，解码线程仅负责「投喂 AVFrame」，CPU 占用显著下降。

---

## 4. 优化方向四：GPU 质量评估 + 离屏导出（P3）

### 4.1 PSNR / SSIM 的 GPU 计算
- 现状：`QualityAnalyzer`（离线双软解 + OpenCV）逐帧算 PSNR/SSIM，内存与 CPU 开销大。
- 方案：离屏渲染主/参考两路到 RGB 纹理 → compute 算逐像素差/Luma → 归约得 PSNR/SSIM。
- 价值：省去双路 BGRA `cv::Mat` 内存，并行度更高，大视频更稳。

### 4.2 帧导出升级（VideoFrameExporter）
- 新增 Vulkan 离屏渲染路径：`render to image → PNG/JPG`，可携带滤镜/叠加层
  （如导出带 MV 标注的关键帧），比当前 `sws_scale → QImage` 更灵活。

---

## 5. 优化方向五：工程与性能加固（P4，贯穿各阶段）

| 项 | 现状 | 改进 |
|----|------|------|
| 着色器加载 | 运行时依赖 cwd 下的 `shaders/` | 编译进 `.qrc` 内存加载，或 `applicationDirPath()` 解析；无 glslc 也能跑 |
| glslc 依赖 | CMake 找不到只 WARNING，运行时缺 `.spv` 静默失败 | 提供「预编译 `.spv` 内嵌」作为 fallback，构建日志明确状态 |
| 解码/渲染解耦 | 渲染与解码在同一线程串行 | 渲染交由 Vulkan 队列；解码线程仅投喂，降低卡顿 |
| 测试 | `tests/` 6 目标已配，源文件待补 | 补 `FormatDetector`/`FrameAnalyzer` 单测；新增渲染管线冒烟测试 |
| 文档一致性 | `IMPLEMENTATION_SUMMARY.md` 宣称「Vulkan 零拷贝管线」已可用 | 修正为「管线已实现，渲染端待接入（见本规划 P0）」 |

---

## 6. 实施路线图（分阶段、可独立交付）

| 阶段 | 内容 | 优先级 | 风险 | 交付物 |
|------|------|--------|------|--------|
| **P0** | Win32 Surface + MainWindow 接线 + 解耦 SW 帧渲染 + 着色器路径修复 | 🔴 最高 | 低（API 成熟，已有 Linux 路径可对照） | Windows 上视频经 Vulkan 呈现 |
| **P1** | MV/宏块/帧类型 overlay 叠加层 | 🟠 高 | 中（实例缓冲与 present 时序） | 画面直接叠加运动矢量 |
| **P2** | GPU 直方图 + 场景切换/帧差 compute | 🟡 中 | 中（原子计数精度） | 场景切换/直方图全 GPU |
| **P3** | GPU PSNR/SSIM + 离屏导出 | 🟢 较低 | 中（离屏 renderpass） | 质量评估与导出加速 |
| **P4** | 着色器内嵌 + 测试补全 + 文档修正 | 🟢 持续 | 低 | 工程稳健 |

**建议起点**：先做 P0（半天~一天工作量，收益最大——让「GPU 渲染」从文档变成肉眼可见的事实），
再做 P1（最能体现「用 Vulkan 做分析」的差异化价值）。

---

## 7. 风险与回退

- **Win32 Surface 驱动兼容**：`VK_ERROR_INCOMPATIBLE_DRIVER` → 已有 `IsVulkanAvailable()` 探测，
  失败自动降级 CPU 回退；P0 默认 SW+Vulkan present，HW 解码不默认开。
- **HW 解码闪退**：沿用既有异常兜底，HW 仅作为可选项。
- **MV 可视化需 SW 解码**：开启宏块分析时自动走 SW 解码（已有逻辑 `macroblock_analysis_enabled_` 跳过 HW），
  与 P1 自然兼容。
- **回退总开关**：保留 `vulkan_rendering_enabled_` 与 `IsVulkanActive()`，任何阶段异常都可一键退回 CPU。

---

*本规划基于 2026-07-11 对 `core/player/*`、`ui/main_window/*`、`CMakeLists.txt`、`docs/IMPLEMENTATION_SUMMARY.md`
的代码核查，所有结论均指向具体文件与行号。*
