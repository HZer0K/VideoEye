# VideoEye 2.0 - 现代化视频流分析软件

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Qt](https://img.shields.io/badge/Qt-6-green.svg)](https://www.qt.io/)
[![FFmpeg](https://img.shields.io/badge/FFmpeg-6.0-red.svg)](https://ffmpeg.org/)

## 项目简介

VideoEye 是一款开源的视频流分析软件,支持多种视频输入源(HTTP、RTMP、RTSP及本地文件),提供实时视频流分析和图形化展示功能。

**⚠️ 注意**: 当前正在进行现代化重构,从 MFC + 老旧FFmpeg迁移到 Qt6 + 现代FFmpeg。

## ✨ 主要特性

### 核心功能
- 🎬 **多源输入**: 支持 HTTP、RTMP、RTSP 协议及本地文件
- 🎥 **实时播放**: 流畅的视频播放体验
- 📊 **流分析**: 实时分析码流统计信息与可视化曲线
- 🧩 **视频帧分析**: 单独标签页显示 I/P/B 帧类型、序号、PTS、时间戳（追加显示不覆盖）
- 🔍 **帧分析**: 直方图分析（可选）
- 👤 **人脸检测**: 基于 OpenCV 的实时人脸检测（可选）
- 🎵 **纯音频律动**: 打开仅音频文件时在视频区域显示音频律动
- 🎶 **音频波形与频谱**: 在分析面板实时查看音频波形快照与频谱快照
- 📈 **数据可视化**: 图表化展示分析结果
- 🖼️ **导出每一帧**: 支持将视频的每一帧导出为 JPG / RGB / YUV
- 🧾 **打开原始图像**: 支持直接打开 .yuv / .rgb 原始图像（需输入宽高）

### 技术特性
- ✅ 现代化 C++17 代码
- ✅ 跨平台支持 (Windows / Linux / macOS)
- ✅ 多线程解码
- ✅ 硬件加速支持 (可选)
- ✅ 响应式 UI 设计

## 🏗️ 技术栈

| 组件 | 技术 | 版本 |
|------|------|------|
| GUI框架 | Qt 6 | 6.0+ |
| 多媒体 | FFmpeg | 6.0+ |
| 计算机视觉 | OpenCV | 4.8+ |
| 音频输出 | SDL 2 | 2.0+ |
| 构建系统 | CMake | 3.20+ |
| 编程语言 | C++ | C++17 |

## 📦 安装依赖

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake \
    qt6-base-dev qt6-multimedia-dev qt6-charts-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libopencv-dev \
    libsdl2-dev
```

### macOS

```bash
brew install cmake qt@6 ffmpeg opencv sdl2
```

### Windows

推荐使用 [vcpkg](https://vcpkg.io/) 管理依赖:

```bash
# 安装 vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.bat  # Windows
# ./bootstrap-vcpkg.sh  # Linux/macOS

# 安装依赖
./vcpkg install ffmpeg opencv4 sdl2 qt6-base qt6-multimedia qt6-charts
```

## 🔨 构建指南

### CMake 构建

```bash
# 克隆项目
git clone https://github.com/yourusername/VideoEye.git
cd VideoEye

# 创建构建目录
mkdir build && cd build

# 配置项目
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)  # Linux/macOS
# 或者使用 Ninja
# cmake .. -G Ninja && ninja

# 运行
./bin/VideoEye
```

### Visual Studio (Windows)

```bash
# 生成 Visual Studio 项目
cmake .. -G "Visual Studio 17 2022" -A x64

# 在 Visual Studio 中打开并构建
devenv VideoEye.sln /build Release
```

## 📁 项目结构

```
VideoEye/
├── core/                    # 核心业务层
│   ├── player/             # 播放器引擎
│   │   ├── MediaPlayer.cpp/h      # 媒体播放器
│   │   ├── Decoders.cpp/h         # 音视频解码器
│   │   └── SyncManager.cpp/h      # 音视频同步
│   ├── analyzer/           # 分析引擎
│   │   ├── StreamAnalyzer.cpp/h   # 流分析
│   │   ├── FrameAnalyzer.cpp/h    # 帧分析
│   │   └── FaceDetector.cpp/h     # 人脸检测
│   ├── io/                 # 输入输出
│   └── model/              # 数据模型
├── ui/                     # UI层
│   ├── main_window/        # 主窗口
│   ├── player_controls/    # 播放控制
│   └── analysis_panel/     # 分析面板
├── utils/                  # 工具类
├── tests/                  # 测试
├── resources/              # 资源文件
├── cmake/                  # CMake配置
├── CMakeLists.txt          # 主CMake文件
└── README.md
```

## 🚀 使用指南

### 基本使用

1. **打开文件**: 点击"文件" -> "打开文件" 或按 `Ctrl+O`
2. **打开网络流**: 点击"文件" -> "打开URL" 输入 RTMP/RTSP/HTTP 地址
3. **播放控制**: 使用底部控制栏的播放/暂停/停止按钮
4. **查看信息**: 在"流信息"标签页查看详细的流信息

### 分析面板

- 菜单 `分析` 提供开关：流分析 / 视频帧分析 / 直方图 / 人脸检测
- 分析面板包含标签页：流分析 / 视频帧 / 音频帧 / 包分析 / 异常事件 / 同步分析 / 统一时间轴 / 音频可视化 / 直方图 / 人脸检测

### 音频分析复现脚本

```bash
chmod +x scripts/repro_audio_analysis.sh
./scripts/repro_audio_analysis.sh /path/to/media.mp3
```

- 脚本会使用现有 CMake 配置完成构建，并以命令行参数方式自动打开媒体文件
- 建议在 GUI 中重点检查 `分析面板` 下的 `音频帧`、`音频可视化`、`统一时间轴` 三个标签页

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

## 🔧 开发指南

### 代码规范

项目使用 clang-format 进行代码格式化,配置文件见 `.clang-format`。

```bash
# 格式化代码
clang-format -i src/*.cpp src/*.h
```

### 添加新功能

1. 在 `core/` 下创建模块
2. 在 `ui/` 下创建对应的 UI 组件
3. 使用 Qt 信号槽机制连接业务逻辑和 UI
4. 编写单元测试

### 调试技巧

```bash
# Debug 构建
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 使用 GDB 调试
gdb ./bin/VideoEye

# 内存泄漏检测 (Linux)
valgrind --leak-check=full ./bin/VideoEye
```

## 📝 迁移指南 (从旧版本)

如果你是从旧版 VideoEye (MFC版本) 迁移:

### 主要变化

1. **构建系统**: Visual Studio → CMake
2. **GUI框架**: MFC → Qt 6
3. **FFmpeg API**: 旧API → 新API (send/receive)
4. **内存管理**: 手动管理 → 智能指针
5. **字符编码**: MultiByte → UTF-8

### 保留旧代码

旧版代码保留在 `legacy/` 目录中作为参考。

## 🧪 测试

```bash
# 运行测试
cd build
ctest --output-on-failure

# 生成测试覆盖率报告
cmake .. -DCODE_COVERAGE=ON
make
ctest
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
```

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


---

**VideoEye 2.0** - 让视频流分析更简单! 🎥📊
