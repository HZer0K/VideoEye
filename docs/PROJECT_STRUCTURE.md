# VideoEye 2.0 项目结构

## 📁 目录结构

```
VideoEye/
├── 📖 文档文件
│   ├── README.md                  # 项目说明文档
│   ├── QUICKSTART.md              # 快速入门指南
│   ├── BUILD_REPORT.md            # 编译运行报告
│   ├── IMPLEMENTATION_SUMMARY.md  # 实施总结
│   └── docs/                      # 架构文档
│       ├── ARCHITECTURE.md        # 架构设计
│       ├── PROJECT_STRUCTURE.md   # 本文件
│       └── UI_OPTIMIZATION_DESIGN.md  # UI 优化设计
│
├── 🔧 构建配置
│   ├── CMakeLists.txt             # CMake 主配置 (跨平台)
│   ├── vcpkg.json                 # vcpkg 依赖清单
│   ├── .clang-format              # 代码格式化配置
│   ├── .gitignore                 # Git 忽略规则
│   ├── .gitmodules                # Git 子模块配置
│   ├── build.sh                   # Linux 构建脚本
│   ├── build.bat                  # Windows 构建入口 (ninja/release/debug)
│   └── build_ninja.ps1            # Ninja + MSVC 构建脚本
│
├── 📂 源代码目录
│   ├── core/                      # 核心业务层 ⭐
│   ├── ui/                        # UI 层 (深色主题) ⭐
│   ├── utils/                     # 工具类
│   └── tests/                     # 测试代码 (GoogleTest)
│
├── 📚 第三方库
│   └── third_party/
│       ├── Bento4/                # MP4 解析引擎 (源码集成 + CMakeLists.txt)
│       ├── MediaInfoLib/          # MediaInfoLib 源码 (有定制改动)
│       ├── ZenLib/                # ZenLib 源码 (MediaInfo 依赖)
│       ├── vulkan-headers/        # Vulkan 1.4 头文件 (submodule)
│       └── CMakeLists.txt         # ZenLib + MediaInfoLib 构建配置
│
├── 🛠️ 工具脚本
│   ├── run.sh                     # Linux 快速启动脚本
│   ├── setup.sh                   # Linux 环境初始化脚本
│   └── scripts/                   # 辅助脚本
│
└── 🏗️ 构建输出 (git忽略)
    ├── build-ninja/               # Ninja 编译输出 (Windows)
    ├── build-Release/             # MSVC 编译输出 (Windows)
    └── build/                     # Make 编译输出 (Linux)
```

---

## 🎯 核心源代码结构

### core/ - 核心业务层

```
core/
├── model/              # 数据模型
│   ├── FrameData.h/cpp    # 帧数据结构
│   ├── ContainerStructureInfo.h  # 容器结构统一模型
│   ├── AnalysisEvent.h    # 分析事件
│   ├── EbmlInfo.h         # EBML 数据模型
│   ├── Mp4BoxInfo.h       # MP4 Box 数据模型
│   ├── PacketInfo.h       # 包信息
│   ├── SyncSample.h       # 同步样本
│   └── TimelineEvent.h    # 时间线事件
│
├── player/             # 播放器引擎
│   ├── MediaPlayer.h/cpp  # 媒体播放器 (薄协调层)
│   ├── Decoders.h/cpp     # 音视频解码器 (含硬件加速)
│   ├── PlaybackClock.h    # 播放节奏控制 (header-only)
│   ├── StreamInfoExtractor.h/cpp  # 流元数据提取
│   ├── AudioVisualizer.h/cpp      # 音频可视化 (FFT 频谱)
│   ├── VideoFrameExporter.h/cpp   # 视频帧导出
│   ├── VulkanContext.h/cpp        # Vulkan 上下文管理
│   ├── VulkanRenderer.h/cpp       # Vulkan 渲染器
│   └── shaders/                   # SPIR-V 着色器
│       ├── yuv2rgb.comp           # YUV→RGB 计算着色器
│       ├── present.vert           # 顶点着色器
│       └── present.frag           # 片段着色器
│
├── analyzer/           # 分析引擎
│   ├── StreamAnalyzer.h/cpp           # 流分析
│   ├── FrameAnalyzer.h/cpp            # 帧分析
│   ├── MediaInfoAnalyzer.h/cpp        # 媒体信息分析
│   ├── Mp4BoxAnalyzer.h/cpp           # MP4/MOV Box 结构解析
│   ├── EbmlAnalyzer.h/cpp             # MKV/WebM EBML 结构解析
│   ├── FormatDetector.h/cpp           # 容器格式魔数检测
│   ├── ContainerStructureAnalyzer.h/cpp # 容器结构统一调度器
│   ├── AviStructureAnalyzer.h/cpp     # AVI RIFF 结构解析
│   ├── FlvStructureAnalyzer.h/cpp     # FLV Tag 结构解析
│   ├── TsStructureAnalyzer.h/cpp      # MPEG-TS 结构解析
│   ├── AsfStructureAnalyzer.h/cpp     # ASF/WMV Object 结构解析
│   └── OggStructureAnalyzer.h/cpp     # OGG Page 结构解析
│
└── model/              # 数据模型
    ├── FrameData.h/cpp    # 帧数据结构
    ├── ContainerStructureInfo.h  # 容器结构统一模型
    ├── AnalysisEvent.h    # 分析事件
    ├── AudioVisualizationFrame.h  # 音频可视化帧数据
    ├── EbmlInfo.h         # EBML 数据模型
    ├── Mp4BoxInfo.h       # MP4 Box 数据模型
    ├── PacketInfo.h       # 包信息
    ├── SyncSample.h       # 同步样本
    └── TimelineEvent.h    # 时间线事件
```

**已实现**: ✅ model, player, analyzer

---

### ui/ - 用户界面层

```
ui/
├── theme/                        # 深色主题模块
│   └── AppTheme.h/cpp            # GitHub Dark 色板 + QSS + QPalette ⭐
│
├── main_window/                  # 主窗口
│   ├── MainWindow.h/cpp          # Top App Bar + Sidebar + ContentArea + ControlBar ⭐
│   └── VulkanVideoWidget.h/cpp   # Vulkan 视频渲染 + 叠加信息层 ⭐
│
├── analysis_panel/               # 分析面板
│   └── AnalysisPanel.h/cpp       # QStackedWidget + 侧边栏导航 (11 个分析模块) ⭐
│
└── app/                          # 应用入口
    └── main.cpp                  # main() 函数
```

**已实现**: ✅ theme, main_window, analysis_panel, app  
**待实现**: ⏳ settings

---

### utils/ - 工具类

```
utils/
├── Logger.h/cpp           # 日志系统
├── ConfigManager.h/cpp    # 配置管理
└── ReportExporter.h/cpp   # 报告导出
```

---

### tests/ - 测试代码

```
tests/
└── CMakeLists.txt        # 测试构建配置 (GoogleTest FetchContent)
```

**测试框架**: GoogleTest v1.15.2 (FetchContent 自动下载)  
**启用方式**: `cmake -DBUILD_TESTING=ON ..` (默认关闭)  
**注意**: 测试源文件 (`tests/unit/*.cpp`) 尚未提交到仓库，CMakeLists.txt 已配置好 6 个测试目标的编译规则，待补全源文件后即可运行。

---

## 📊 依赖管理策略

| 依赖 | 管理方式 | 说明 |
|------|---------|------|
| Qt6 | vcpkg (Win) / apt (Linux) | Widgets, Multimedia, Charts |
| OpenCV | vcpkg (Win) / apt (Linux) | core, imgproc, objdetect |
| SDL2 | vcpkg (Win) / apt (Linux) | 音频输出 |
| FFmpeg | 多来源查找 | vcpkg → 源码编译产物 → pkg-config |
| Bento4 | 源码集成 (add_subdirectory) | vcpkg 版本过旧 (1.5.1 vs 1.6.0) |
| MediaInfoLib | 源码集成 (add_subdirectory) | 有定制改动 |
| ZenLib | 源码集成 (add_subdirectory) | MediaInfoLib 依赖 |
| Vulkan | bundled headers + 系统 lib | 1.4.309 头文件 (submodule) |
| zlib | vcpkg (Win) / apt (Linux) | MediaInfoLib 依赖 |

---

## 📈 代码统计

| 目录 | 文件数 | 状态 |
|------|--------|------|
| core/model/ | 10 | ✅ 已实现 |
| core/player/ | 18 | ✅ 已实现 (含 Vulkan 渲染 + 硬件解码 + 3 着色器) |
| core/analyzer/ | 24 | ✅ 已实现 (7 种容器格式 + 统一调度) |
| ui/ | 9 | ✅ 已实现 (深色主题) |
| utils/ | 6 | ✅ 已实现 |
| tests/ | 1 | ⏳ CMakeLists 就绪，源文件待补全 |
| **总计** | **68** | |

---

## 🗂️ 文件分类说明

### 🟢 核心文件

- `CMakeLists.txt` — 跨平台构建配置
- `core/player/MediaPlayer.cpp` — 播放器核心
- `core/player/VulkanRenderer.cpp` — Vulkan 渲染器
- `core/analyzer/StreamAnalyzer.cpp` — 流分析引擎
- `ui/main_window/MainWindow.cpp` — 主窗口 UI
- `ui/theme/AppTheme.cpp` — 深色主题

### 🔧 构建脚本

- `build.bat` — Windows 构建入口 (支持 ninja/release/debug)
- `build_ninja.ps1` — Ninja + MSVC 构建 (绕过 MSBuild LOLBin 限制)
- `build.sh` — Linux 构建 (GCC + Make)
- `setup.sh` — Linux 环境初始化

---

## 🚀 下一步优化建议

### 短期

1. ✅ ~~整理根目录~~ (已完成)
2. ✅ ~~完善视频帧显示~~ (已完成)
3. ✅ ~~添加音频播放~~ (已完成)
4. ✅ ~~实现基础分析功能~~ (已完成)
5. ✅ ~~多格式容器结构分析~~ (已完成)
6. ✅ ~~深色主题 UI 重构~~ (已完成)
7. ✅ ~~跨平台构建 (Windows + Linux)~~ (已完成)
8. ✅ ~~清理 legacy/ 和死代码~~ (已完成)

### 中期

1. ⏳ 补全单元测试源文件 (tests/unit/*.cpp)
2. ⏳ 添加更多分析算法
3. ⏳ 完善 UI 交互细节

### 长期

1. ⏳ 添加 CI/CD
2. ⏳ 发布正式版本
