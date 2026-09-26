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
- **媒体信息**: 基于 FFmpeg `libavformat` 展示完整元数据（容器 / 视频 / 音频 / 字幕 / 章节）
- **硬件加速**: VAAPI/CUDA/D3D11VA/QSV 等硬件解码（解码后统一转 CPU 图像渲染）
- **音频可视化**: 波形快照、FFT 频谱、纯音频律动
- **场景切换检测**: 灰度直方图 Bhattacharyya 距离实时检测镜头切换
- **画面质量（视觉缺陷）**: 黑场 / 冻结 / 花屏马赛克 / 模糊 / 闪烁 / 过曝欠曝 / 色偏 / 隔行梳齿 / 黑边，带证据缩略图与缺陷列表
- **字幕 / 时码 / 辅助数据**: 字幕 cue（SRT / ASS-SSA / WebVTT / tx3g / CEA-608）重叠与空字幕检查、SMPTE 时码与章节、data 流与 SCTE-35 广告插入点
- **质量评估**: 离线逐帧计算 PSNR / SSIM 及走势图
- **帧导出**: 导出任意帧为 JPG / RGB / YUV，支持打开 .yuv / .rgb 原始图像

## 技术栈

| 组件 | 技术 |
|------|------|
| GUI | Qt 6（只用 QtWidgets） |
| 多媒体 | FFmpeg 8.x（Windows 用锁版本预编译包，Linux/macOS 用系统包） |
| 图表 | 自绘 `MetricChartWidget`（QPainter） |
| 音频输出 | 平台原生（WASAPI / ALSA / AudioQueue） |
| MP4 解析 | 自研 `utils::IsobmffParser` |
| 构建 | CMake 3.23+ / Ninja + CMakePresets |

**依赖只有两个硬依赖：Qt Widgets + FFmpeg。** 其余能力（媒体信息展示、图表、MP4 样本表解析、音频输出）全部自研或走平台原生 API，
不再集成 MediaInfoLib / Bento4 / SDL2 / QtCharts / Vulkan。

Qt 走 vcpkg manifest（Windows）或系统包管理器（apt/brew）；FFmpeg 分平台取源：

| 平台 | FFmpeg 来源 |
|------|------------|
| Windows | `third_party/prebuilt/windows-x64/ffmpeg/`，缺则由 `scripts/fetch-ffmpeg.ps1` 下载（版本 + URL + SHA256 锁在 `cmake/ffmpeg-version.json`，校验不过直接失败） |
| Linux / macOS | `pkg-config` 找系统开发包（`libavcodec-dev` / `brew install ffmpeg`），找不到才回退 `third_party/prebuilt/<platform>/ffmpeg/` |

构建配置的唯一来源是 `CMakePresets.json`；动态库部署由 CMake 完成（Windows 与 exe 同目录，
Linux/macOS 进相邻 `lib/` 并写入 RPATH），构建后会校验确实到位。

## 构建

### Windows (Ninja + MSVC)

> **前置要求**：Visual Studio 2022 (带 C++/CMake/Ninja） + vcpkg (设置环境变量 `VCPKG_ROOT`)

```powershell
# 一键构建（自动加载 MSVC 环境 + 自动下载 FFmpeg 预编译库 + 自动安装 vcpkg 依赖）
.\build.bat           # Release 构建
.\build.bat debug     # Debug 构建
.\build.bat test      # Release 构建 + 跑单元测试
```

产物：`build\release\bin\VideoEye.exe` / `build\debug\bin\VideoEye.exe`

> **vcpkg 说明：项目使用 `vcpkg.json` manifest + `CMakePresets.json` 自动集成 Qt6；首次构建较慢（qtbase 编译需要 30~60 分钟），vcpkg 会缓存后增量构建秒级完成。

### Linux / macOS

```bash
# Ubuntu/Debian 系统依赖（FFmpeg 用系统开发包，由 pkg-config 查找）
sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev

# macOS
brew install cmake ninja qt@6 ffmpeg

# 构建
./build.sh             # Release
./build.sh debug      # Debug
```

产物：`build/release/bin/VideoEye` / `build/debug/bin/VideoEye`

## 使用指南

1. **打开**: `Ctrl+O` 打开文件 / `Ctrl+U` 打开 URL
2. **播放控制**: 底部控制栏播放/暂停/停止 (`Space` / `Esc`)
3. **分析**: 左侧边栏切换分析模块（媒体信息、流分析、视频帧、音频帧、数据包、异常事件、同步分析、时间轴、音频响度、直方图、容器结构、场景切换、画面质量、质量评估），各模块顶部配有独立「启用分析」开关
4. **导出帧**: `文件` → `导出视频帧...`（jpg / rgb / yuv）
5. **原始图像**: 打开 `.yuv`（YUV420P）/ `.rgb`（RGB24）时输入宽高

## 项目结构

```
VideoEye/
├── core/                 # 核心业务层
│   ├── player/           # 播放引擎 (MediaPlayer / 解码器 / 音频输出)
│   ├── analyzer/         # 分析引擎 (容器结构 / 场景切换 / 质量评估)
│   └── model/            # 数据模型
├── ui/                   # UI 层 (主题 / 主窗口 / 分析面板 / 自绘图表)
├── utils/                # 工具类 (Logger / ConfigManager / ReportExporter / IsobmffParser)
├── third_party/prebuilt/ # FFmpeg 预编译包（不入库，脚本下载）
├── docs/                 # 文档
├── vcpkg.json            # vcpkg 依赖清单
└── build.bat / build.sh  # 构建脚本
```

## 文档

| 文档 | 内容 |
|------|------|
| [QUICKSTART.md](QUICKSTART.md) | 快速入门：构建、打开媒体、使用各分析页 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 环境要求、构建选项、代码规范 |
| [docs/BITSTREAM_ANALYSIS.md](docs/BITSTREAM_ANALYSIS.md) | 编码码流解析（H.264 / HEVC / AV1 / VVC） |
| [docs/DIAGNOSTICS_QC.md](docs/DIAGNOSTICS_QC.md) | 诊断扫描与 QC 报告、规则集与导出 |
| [docs/AUDIO_QC.md](docs/AUDIO_QC.md) | 音频 QC：响度、真峰值、削波、相位 |
| [docs/BITRATE_GOP_ANALYSIS.md](docs/BITRATE_GOP_ANALYSIS.md) | 滑动窗口码率、GOP 重建与异常识别 |
| [docs/COLOR_HDR_ANALYSIS.md](docs/COLOR_HDR_ANALYSIS.md) | 色彩与 HDR 元数据快照与告警 |
| [docs/MP4_SAMPLE_TABLE.md](docs/MP4_SAMPLE_TABLE.md) | MP4/fMP4 样本表一致性校验 |
| [docs/HLS_DASH_SEGMENT.md](docs/HLS_DASH_SEGMENT.md) | HLS/DASH 流媒体包检测（manifest + segment + 多码率 ladder） |
| [docs/VISUAL_QC.md](docs/VISUAL_QC.md) | 画面质量与视觉缺陷检测（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边） |
| [docs/SUBTITLE_TIMECODE_AUX.md](docs/SUBTITLE_TIMECODE_AUX.md) | 字幕 cue、SMPTE 时码 / 章节、data 流与 SCTE-35 插入点 |

## 测试

用带测试的 preset（`BUILD_TESTING=ON`；Windows 上另带 `VCPKG_MANIFEST_FEATURES=tests` 让 vcpkg 装 gtest）：

```bash
# Windows：推荐 win-test-release（Debug 需要重编整套 debug triplet 的 Qt，约 1 小时）
cmake --preset win-test-release
cmake --build --preset win-test-release
ctest --preset win-test-release

# Linux / macOS
cmake --preset linux-test-debug
cmake --build --preset linux-test-debug
ctest --preset linux-test-debug
```

共 18 个可执行文件 + 17 组 ctest 用例，覆盖码流解析（H.264/HEVC/AV1/VVC）、MP4 样本表、
ISOBMFF、QC 规则、码率/GOP、色彩 HDR、导出器等纯逻辑路径。

## 贡献

欢迎提交 Issue 和 Pull Request。Fork 本仓库 → 创建特性分支 → 提交更改 → 开启 Pull Request。

## 开源协议

本项目采用 [MIT](LICENSE) 协议。

## 致谢

- 原项目作者: [雷霄骅 Lei Xiaohua](https://github.com/leixiaohua1020)
- [FFmpeg](https://ffmpeg.org/) · [Qt](https://www.qt.io/) · [vcpkg](https://github.com/microsoft/vcpkg)
