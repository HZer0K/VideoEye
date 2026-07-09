# VideoEye 2.0 - 现代化视频流分析软件

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Qt](https://img.shields.io/badge/Qt-6-green.svg)](https://www.qt.io/)
[![FFmpeg](https://img.shields.io/badge/FFmpeg-7.1%2B-red.svg)](https://ffmpeg.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()

## 项目简介

VideoEye 是一款开源的视频流分析软件，支持多种视频输入源（HTTP、RTMP、RTSP 及本地文件），提供实时视频流分析和图形化展示功能。采用现代化 C++17 + Qt6 技术栈，深色专业风格 UI，跨平台支持 Windows / Linux / macOS。

## ✨ 主要特性

### 核心功能
- 🎬 **多源输入**: 支持 HTTP、RTMP、RTSP 协议及本地文件
- 🎥 **实时播放**: 流畅的视频播放体验
- 📊 **流分析**: 实时分析码流统计信息与可视化曲线
- 🧩 **视频帧分析**: 单独标签页显示 I/P/B 帧类型、序号、PTS、时间戳
- 📋 **媒体信息**: 使用 [MediaInfo](https://github.com/MediaArea/MediaInfoLib) 显示完整的媒体文件元数据
- 🎵 **纯音频律动**: 打开仅音频文件时在视频区域显示音频律动
- 🎶 **音频波形与频谱**: 实时查看音频波形快照与频谱快照（基于 FFT 高性能计算）
- 📈 **数据可视化**: 图表化展示分析结果
- 🖼️ **导出每一帧**: 支持将视频的每一帧导出为 JPG / RGB / YUV
- 🧾 **打开原始图像**: 支持直接打开 .yuv / .rgb 原始图像
- 📦 **多格式容器结构分析**: 统一调度解析 MP4/MOV、MKV/WebM、AVI、FLV、MPEG-TS、ASF/WMV、OGG 七种容器格式
- ⚡ **硬件解码加速**: 支持 Vulkan/VAAPI/CUDA/VDPAU/VideoToolbox/D3D11VA/DXVA2/QSV 多种硬件加速
- 🖥️ **Vulkan 渲染管线**: GPU 零拷贝 YUV→RGB 转换 + 全屏呈现

### UI 特性
- 🎨 **深色专业风格**: GitHub Dark 色板，降低长时间分析视觉疲劳
- 📐 **侧边栏导航**: 分析模块从横向 Tab 迁移到左侧垂直导航列表
- 🔢 **等宽数值显示**: FPS、码率、时间码使用 JetBrains Mono 等宽字体
- 🏷️ **信息叠加层**: 视频区域顶部叠加分辨率、编码、FPS、状态信息

### 技术特性
- ✅ 现代化 C++17 代码
- ✅ 跨平台支持 (Windows / Linux / macOS)
- ✅ 多线程解码
- ✅ Vulkan 硬件解码 + GPU 渲染管线（动态版本协商，无兼容驱动时自动回退）
- ✅ FFT 频谱计算 (Cooley-Tukey 算法，预计算表优化)
- ✅ 混合依赖管理策略 (vcpkg/apt + 源码集成)
- ✅ Ninja + MSVC 构建 (Windows) / GCC Make (Linux)

## 🏗️ 技术栈

| 组件 | 技术 | 版本 | 管理方式 |
|------|------|------|----------|
| GUI 框架 | Qt 6 | 6.0+ | vcpkg / apt |
| 多媒体 | FFmpeg | 7.1+ | vcpkg / apt / 源码编译 |
| GPU 渲染 | Vulkan | 1.3+ | 系统 SDK / apt |
| 媒体元数据 | MediaInfoLib | 26.05 | 源码集成 (有定制) |
| 计算机视觉 | OpenCV | 4.8+ | vcpkg / apt |
| 音频输出 | SDL 2 | 2.0+ | vcpkg / apt |
| MP4 容器解析 | Bento4 | 1.6+ | 源码集成 (CMakeLists.txt) |
| 构建系统 | CMake | 3.20+ | — |
| 构建工具 | Ninja / Make | — | — |
| 编程语言 | C++ | C++17 | — |

### 依赖管理策略

项目采用**混合依赖管理**策略，兼顾便利性与定制能力：

| 依赖 | 管理方式 | 原因 |
|------|----------|------|
| FFmpeg | vcpkg / apt / 源码编译 | 三级 fallback 查找，自动选择最优来源 |
| OpenCV | vcpkg / apt | 通用库，无需定制 |
| Qt6 | vcpkg / apt | 通用库，无需定制 |
| SDL2 | vcpkg / apt | 通用库，无需定制 |
| zlib | vcpkg / apt | MediaInfoLib 依赖 |
| MediaInfoLib + ZenLib | 源码集成 | 有定制改动（排除 HTTP_Client、编译宏等） |
| Bento4 | 源码集成 | vcpkg 版本过旧 (1.5.1 vs 1.6.0) |
| Vulkan Headers | Git submodule | 1.4.309，替代系统旧版头文件 |

**FFmpeg 查找优先级** (CMakeLists.txt 自动选择):
1. `find_package(FFMPEG)` — vcpkg 已安装的 ffmpeg 包
2. 已有源码编译产物 — `build-ninja/ffmpeg_install/` 等
3. 系统 `pkg-config` — Linux apt 安装的 libavcodec-dev 等

## 📦 安装依赖

### Linux (Ubuntu/Debian)

```bash
sudo apt install -y \
    build-essential cmake ninja-build nasm yasm pkg-config \
    qt6-base-dev qt6-multimedia-dev qt6-charts-dev \
    libopencv-dev libsdl2-dev zlib1g-dev \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev \
    libvulkan-dev glslc
```

### macOS

```bash
brew install cmake ninja nasm qt@6 opencv sdl2 zlib ffmpeg
brew install vulkan-sdk  # 可选: Vulkan 硬件加速
```

### Windows (vcpkg)

```powershell
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# 在项目根目录运行 vcpkg install（自动读取 vcpkg.json manifest）
cd VideoEye
vcpkg install
```

> **FFmpeg 说明**: Windows 上如果 vcpkg 的 FFmpeg 版本不支持 vulkan feature，CMake 会自动回退到已有的源码编译产物（`build-ninja/ffmpeg_install/`）。首次需要通过 `build_ninja.ps1` 构建 FFmpeg。

## 🔨 构建指南

### Windows (Ninja + MSVC)

```powershell
# 推荐方式: 使用 Ninja 构建脚本（自动设置 MSVC 环境变量）
.\build.bat ninja

# 或直接运行 PowerShell 脚本
powershell -ExecutionPolicy Bypass -File build_ninja.ps1

# 也可以使用 Visual Studio 生成器
.\build.bat release
```

构建产物在 `build-ninja/bin/VideoEye.exe`，运行时 DLL 由构建脚本自动复制。

### Linux / macOS

```bash
# 一键构建
./build.sh release

# Debug 构建
./build.sh debug
```

构建产物在 `build-release/bin/VideoEye`。

### 环境初始化 (Linux)

```bash
# 检查依赖 + 初始化子模块 + 构建
./setup.sh

# 选项
./setup.sh --debug        # Debug 构建
./setup.sh --skip-deps    # 跳过依赖检查
./setup.sh --build-only   # 仅编译
```

### 手动构建

```bash
# 克隆项目（含子模块）
git clone --recursive https://github.com/HZer0K/VideoEye.git
cd VideoEye

# Linux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Windows (需要 MSVC 环境)
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<vcpkg_installed_path>
ninja
```

### 运行

- **Linux/macOS**: `build-release/bin/VideoEye`
- **Windows**: `build-ninja\bin\VideoEye.exe`

## 📁 项目结构

```
VideoEye/
├── core/                        # 核心业务层
│   ├── player/                  # 播放器引擎
│   │   ├── MediaPlayer.cpp/h    # 媒体播放器 (薄协调层)
│   │   ├── Decoders.cpp/h       # 音视频解码器 (含 Vulkan/VAAPI/CUDA 硬件加速)
│   │   ├── VulkanContext.cpp/h  # Vulkan 设备上下文管理
│   │   ├── VulkanRenderer.cpp/h # Vulkan GPU 渲染管线 (YUV→RGB compute + present)
│   │   ├── PlaybackClock.h      # 播放节奏控制
│   │   ├── StreamInfoExtractor  # 流元数据提取
│   │   ├── AudioVisualizer      # 音频可视化 (FFT 频谱)
│   │   ├── VideoFrameExporter   # 视频帧导出
│   │   └── shaders/             # Vulkan 着色器
│   ├── analyzer/                # 分析引擎 (MP4/MKV/AVI/FLV/TS/ASF/OGG)
│   └── model/                   # 数据模型
├── ui/                          # UI层
│   ├── theme/                   # 深色主题模块 (AppTheme.h/cpp)
│   ├── main_window/             # 主窗口 (AppBar + Sidebar + ContentArea + ControlBar)
│   │   ├── MainWindow.h/cpp
│   │   └── VulkanVideoWidget.h/cpp  # 视频渲染 + 叠加信息层
│   └── analysis_panel/          # 分析面板 (QStackedWidget 模式)
├── utils/                       # 工具类 (Logger / ConfigManager / ReportExporter)
├── tests/                       # 测试配置 (GoogleTest, 源文件待补全)
├── third_party/                 # 第三方库
│   ├── Bento4/                  # MP4 解析 (源码集成 + CMakeLists.txt)
│   ├── vulkan-headers/          # Vulkan 1.4 头文件 (submodule)
│   ├── ZenLib/                  # ZenLib 源码 (submodule)
│   ├── MediaInfoLib/            # MediaInfoLib 源码 (submodule)
│   └── CMakeLists.txt           # ZenLib + MediaInfoLib 构建配置
├── docs/                        # 文档
├── vcpkg.json                   # vcpkg 依赖清单
├── CMakeLists.txt               # 主 CMake 配置
├── build.bat                    # Windows 构建脚本 (支持 ninja/release/debug)
├── build_ninja.ps1              # Ninja + MSVC 构建脚本
├── build.sh                     # Linux 构建脚本
├── setup.sh                     # Linux 环境初始化脚本
└── README.md
```

## 🚀 使用指南

### 基本使用

1. **打开文件**: 点击顶部栏"打开文件"按钮或按 `Ctrl+O`
2. **打开网络流**: 点击"打开URL"输入 RTMP/RTSP/HTTP 地址
3. **播放控制**: 使用底部控制栏的播放/暂停/停止按钮
4. **分析导航**: 点击左侧边栏的分析模块切换分析视图
5. **查看信息**: 在分析面板查看流分析、帧信息、容器结构等

### 分析面板

左侧边栏提供以下分析模块导航：

1. **媒体信息** — MediaInfo 解析的完整文件元数据
2. **流分析** — FPS / 码率 / 关键帧统计卡片 + 走势图表
3. **视频帧** — I/P/B 帧类型、序号、PTS、时间戳
4. **音频帧** — 音频包信息
5. **数据包** — 包级别分析
6. **异常事件** — 解码异常监测
7. **同步分析** — 音视频同步
8. **时间轴** — 统一时间轴
9. **音频响度** — 波形与频谱
10. **直方图** — 帧直方图分析
11. **容器结构** — MP4 Box 树 / MKV EBML / AVI RIFF 等

每个分析模块顶部配有独立的「启用分析」开关，可灵活关闭单个功能以降低性能开销。

### 导出视频帧

1. 打开一个包含视频流的媒体文件
2. 点击 `文件` -> `导出视频帧...`
3. 选择输出目录与导出格式（jpg / rgb / yuv）
4. 支持显示导出进度并可随时终止

### 打开 .yuv / .rgb 原始图像

1. 点击 `文件` -> `打开文件`，选择 `.yuv` 或 `.rgb`
2. 输入宽度与高度
3. 支持格式：
   - `.rgb`: RGB24（RGB888，packed）
   - `.yuv`: YUV420P（I420：Y + U + V，宽高必须为偶数）

### 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+O` | 打开文件 |
| `Ctrl+U` | 打开URL |
| `Space` | 播放/暂停 |
| `Esc` | 停止 |
| `Ctrl+Q` | 退出 |

## 🧪 测试

```bash
# 构建 Debug 版本 (启用测试)
cmake -B build-debug -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug

# 运行测试
cd build-debug && ctest --output-on-failure
```

> **注意**: 测试源文件 (`tests/unit/*.cpp`) 尚未提交到仓库。CMakeLists.txt 已配置好 6 个测试目标的编译规则，待补全源文件后即可运行。

| 测试套件 | 被测模块 | 用例数 (规划) |
|---------|---------|--------|
| test_format_detector | FormatDetector | 27 |
| test_config_manager | ConfigManager | 26 |
| test_report_exporter | ReportExporter | 14 |
| test_stream_analyzer | StreamAnalyzer | 17 |
| test_frame_data | FrameData | 15 |
| test_frame_analyzer | FrameAnalyzer | 16 |

## 🎨 UI 设计

项目采用深色专业风格 UI，设计稿文件名为 `VideoEye UI Optimization`。

- **色板**: GitHub Dark — 背景 `#0D1117`、卡片 `#161B22`、边框 `#30363D`、强调色 `#58A6FF`
- **字体**: 界面文字 Inter，数值/时间码 JetBrains Mono
- **布局**: Top App Bar + Left Sidebar + Video Area + Control Bar + Analysis Panel + Status Bar
- **主题模块**: `ui/theme/AppTheme.h/cpp` 集中式 QSS + QPalette 管理

## 📝 迁移指南 (从旧版本)

如果你是从旧版 VideoEye (MFC版本) 迁移:

| 变化 | 旧版本 | 新版本 |
|------|--------|--------|
| 构建系统 | Visual Studio 2010 | CMake 3.20+ / Ninja |
| GUI 框架 | MFC (Windows only) | Qt6 (跨平台) |
| FFmpeg API | 旧API (avcodec_decode) | 新API (send/receive) |
| 内存管理 | 手动 new/delete | 智能指针 + RAII |
| 字符编码 | MultiByte (乱码) | UTF-8 |
| UI 风格 | 系统默认 | 深色专业风格 |
| 依赖管理 | 散落 DLL | vcpkg + 源码集成混合 |

## 🤝 贡献指南

欢迎提交 Issue 和 Pull Request!

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启 Pull Request

## 📄 开源协议

本项目采用 MIT 协议 - 查看 [LICENSE](LICENSE) 文件了解详情

## 👥 原项目

- **雷霄骅 Lei Xiaohua** - 初始作者 - [leixiaohua1020](https://github.com/leixiaohua1020)

## 🙏 致谢

- [FFmpeg](https://ffmpeg.org/) - 强大的多媒体框架
- [Qt](https://www.qt.io/) - 跨平台GUI框架
- [OpenCV](https://opencv.org/) - 计算机视觉库
- [SDL](https://www.libsdl.org/) - 多媒体库
- [MediaInfoLib](https://github.com/MediaArea/MediaInfoLib) - 媒体元数据解析库 (BSD-2-Clause)
- [ZenLib](https://github.com/MediaArea/ZenLib) - 跨平台基础库 (MediaInfo 依赖)
- [Bento4](https://github.com/axiomatic-systems/Bento4) - MP4 容器解析库 (GPL-2.0)
- [vcpkg](https://github.com/microsoft/vcpkg) - C++ 包管理器

---

**VideoEye 2.0** - 让视频流分析更简单! 🎥📊
