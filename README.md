# VideoEye 2.0 - 现代化视频流分析软件

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Qt](https://img.shields.io/badge/Qt-6-green.svg)](https://www.qt.io/)
[![FFmpeg](https://img.shields.io/badge/FFmpeg-8.1%2B-red.svg)](https://ffmpeg.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()

## 项目简介

VideoEye 是一款开源的视频流分析软件，支持 HTTP、RTMP、RTSP 网络流及本地文件输入，提供实时码流分析、帧级信息、容器结构解析与图形化展示。采用 C++17 + Qt6 技术栈，深色专业风格 UI，跨平台支持 Windows / Linux / macOS。

## 主要特性

- **多源输入**: HTTP / RTMP / RTSP 网络流及本地文件
- **流分析**: 实时统计 FPS、码率、关键帧并可视化曲线
- **视频帧分析**: 单独标签页展示 I/P/B 帧类型、序号、PTS、时间戳
- **容器结构分析**: 统一调度解析 MP4/MOV、MKV/WebM、AVI、FLV、MPEG-TS、ASF/WMV、OGG
- **媒体信息**: 基于 [MediaInfo](https://github.com/MediaArea/MediaInfoLib) 展示完整元数据
- **硬件加速**: Vulkan/VAAPI/CUDA/D3D11VA/QSV 等硬件解码，Vulkan GPU 渲染管线（零拷贝 YUV→RGB）
- **音频可视化**: 波形快照、FFT 频谱、纯音频律动
- **场景切换检测**: 灰度直方图 Bhattacharyya 距离实时检测镜头切换
- **质量评估**: 离线逐帧计算 PSNR / SSIM 及走势图
- **帧导出**: 导出任意帧为 JPG / RGB / YUV，支持打开 .yuv / .rgb 原始图像

## 技术栈

| 组件 | 技术 |
|------|------|
| GUI | Qt 6 |
| 多媒体 | FFmpeg 8.1+ |
| GPU 渲染 | Vulkan 1.3+ |
| 媒体元数据 | MediaInfoLib（源码集成） |
| 计算机视觉 | OpenCV 4.8+ |
| 音频输出 | SDL 2 |
| MP4 解析 | Bento4（源码集成） |
| 构建 | CMake 3.20+ / Ninja |

依赖采用混合管理：通用库（Qt/OpenCV/SDL2）走 vcpkg / apt；MediaInfoLib、Bento4 有定制改动走源码集成；FFmpeg 按 vcpkg → `third_party/ffmpeg-prebuilt/` → pkg-config 三级 fallback 自动选择。

## 构建

### Windows (Ninja + MSVC)

```powershell
# 安装依赖 (自动读取 vcpkg.json manifest)
vcpkg install --triplet x64-windows-release --host-triplet x64-windows-release --overlay-triplets=scripts/triplets --x-manifest-root=. --x-install-root=vcpkg_installed

# 构建 (自动设置 MSVC 环境; 缺失时自动下载 FFmpeg 预编译库)
.\build.bat ninja
```

产物: `build-ninja\bin\VideoEye.exe`

### Linux / macOS

```bash
# Ubuntu/Debian 依赖
sudo apt install -y build-essential cmake ninja-build nasm pkg-config \
    qt6-base-dev qt6-charts-dev libopencv-dev libsdl2-dev zlib1g-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libvulkan-dev glslc
# macOS
brew install cmake ninja nasm qt@6 opencv sdl2 zlib ffmpeg

./build.sh release
```

产物: `build-release/bin/VideoEye`

## 使用指南

1. **打开**: `Ctrl+O` 打开文件 / `Ctrl+U` 打开 URL
2. **播放控制**: 底部控制栏播放/暂停/停止 (`Space` / `Esc`)
3. **分析**: 左侧边栏切换分析模块（媒体信息、流分析、视频帧、音频帧、数据包、异常事件、同步分析、时间轴、音频响度、直方图、容器结构、场景切换、质量评估），各模块顶部配有独立「启用分析」开关
4. **导出帧**: `文件` → `导出视频帧...`（jpg / rgb / yuv）
5. **原始图像**: 打开 `.yuv`（YUV420P）/ `.rgb`（RGB24）时输入宽高

## 项目结构

```
VideoEye/
├── core/                 # 核心业务层
│   ├── player/           # 播放引擎 (MediaPlayer / 解码器 / Vulkan 渲染 / 音频可视化)
│   ├── analyzer/         # 分析引擎 (容器结构 / 场景切换 / 质量评估)
│   └── model/            # 数据模型
├── ui/                   # UI 层 (主题 / 主窗口 / 分析面板)
├── utils/                # 工具类 (Logger / ConfigManager / ReportExporter)
├── third_party/          # 第三方库 (Bento4 / MediaInfoLib / Vulkan Headers)
├── docs/                 # 文档
├── vcpkg.json            # vcpkg 依赖清单
└── build.bat / build.sh  # 构建脚本
```

## 测试

```bash
cmake -B build-debug -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug && cd build-debug && ctest --output-on-failure
```

## 贡献

欢迎提交 Issue 和 Pull Request。Fork 本仓库 → 创建特性分支 → 提交更改 → 开启 Pull Request。

## 开源协议

本项目采用 [MIT](LICENSE) 协议。

## 致谢

- 原项目作者: [雷霄骅 Lei Xiaohua](https://github.com/leixiaohua1020)
- [FFmpeg](https://ffmpeg.org/) · [Qt](https://www.qt.io/) · [OpenCV](https://opencv.org/) · [SDL](https://www.libsdl.org/) · [MediaInfoLib](https://github.com/MediaArea/MediaInfoLib) · [Bento4](https://github.com/axiomatic-systems/Bento4) · [vcpkg](https://github.com/microsoft/vcpkg)
