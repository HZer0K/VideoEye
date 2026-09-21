# VideoEye 2.0 快速入门指南

## 🚀 5 分钟快速开始

### 1. 克隆项目

```bash
git clone --recursive https://github.com/HZer0K/VideoEye.git
cd VideoEye
```

### 2. 构建项目

#### Windows (Ninja + MSVC)

```powershell
# 推荐方式：使用 Ninja 构建脚本（自动配置 MSVC 环境）
.\build.bat ninja
```

> 首次构建会自动通过 vcpkg 拉取 Qt6，FFmpeg 使用 gyan.dev full-shared 预编译共享库（含头文件与导入库，`build.bat ninja` 检测缺失时自动下载，版本由 stamp 记录自动追踪）。
> 项目只有两个硬依赖：**Qt Widgets + FFmpeg**。

#### Linux (Ubuntu/Debian)

```bash
# 安装系统依赖
sudo apt install -y \
    build-essential cmake ninja-build pkg-config \
    qt6-base-dev
# FFmpeg 用预编译包放到 third_party/prebuilt/linux-x64/ffmpeg/{include,lib,bin}

# 一键构建
./build.sh release
```

#### macOS

```bash
brew install cmake ninja qt@6
# FFmpeg 预编译包放到 third_party/prebuilt/mac-arm64|mac-x64/ffmpeg/{include,lib,bin}
./build.sh release
```

### 3. 运行

```bash
# Windows
build-ninja\bin\VideoEye.exe

# Linux / macOS
build-release/bin/VideoEye
```

---

## 📦 构建方式详解

### Windows 构建

| 命令 | 说明 |
|------|------|
| `build.bat` | Release 构建（Ninja + MSVC） **推荐** |
| `build.bat debug` | Debug 构建 |
| `cmake --preset win-release` | 直接用 CMake preset（需已加载 MSVC 环境） |
| `cmake --build build/release` | 仅编译，不重新 configure |

**build.bat** 自动完成：
- 用 `vswhere.exe` 找到并加载 Visual Studio 2022 的 `vcvars64.bat` 环境
- FFmpeg 缺失自动调用 `scripts/fetch-ffmpeg.ps1` 下载 gyan.dev full-shared
- 通过 CMakePresets + vcpkg toolchain 自动安装依赖（qtbase；gtest 只在显式开启 `tests` feature 时安装）
- CMake configure + build + 运行时 DLL 部署

### Linux / macOS 构建

| 命令 | 说明 |
|------|------|
| `./build.sh` | Release 构建（默认） **推荐** |
| `./build.sh debug` | Debug 构建 |
| `cmake --preset linux-release` | 直接用 preset |
| `cmake --build build/release` | 仅编译 |

---

## 🔧 依赖说明

### FFmpeg 依赖管理

CMake 采用**两级 fallback** 自动查找 FFmpeg：

```
third_party/prebuilt/<platform>/ffmpeg/   ← 唯一查找路径（{include,lib,bin}）
  <platform> = windows-x64 | linux-x64 | mac-x64 | mac-arm64
```

**Windows 用户**：
- `build.bat` 检测到 `third_party/prebuilt/windows-x64/ffmpeg/` 缺失时，自动运行 `scripts/fetch-ffmpeg.ps1` 下载
- 预编译库来自 [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) **release-full-shared**（gyan 唯一带 include/ 头文件与 lib/ 导入库的共享包；essentials 仅含 bin/ 不适合链接开发）
- CMake 侧由 `cmake/deps/FFmpegPrebuilt.cmake` 统一生成 `FFmpeg::avcodec` 等 imported target 并部署运行时

**Linux / macOS 用户**：
- 把预编译包（或自行构建的 install 前缀）放到对应的 `third_party/prebuilt/<platform>/ffmpeg/` 即可

### vcpkg 依赖（Windows 自动集成）

项目通过 `vcpkg.json` + `CMakePresets.json` 自动声明并安装依赖，无需手动运行 `vcpkg install`：
- `qtbase` — Qt6 GUI（只用 QtWidgets，feature 已按需裁剪）
- `tests`（可选 feature）— `gtest`，只在要跑单元测试时才装

```powershell
# 默认依赖（不含 gtest）
vcpkg install --triplet x64-windows-release --host-triplet x64-windows-release `
  --overlay-triplets=scripts/triplets --overlay-ports=scripts/overlay-ports `
  --x-manifest-root=. --x-install-root=vcpkg_installed

# 需要单元测试时追加 --x-feature=tests
```

> 使用项目自带的 release-only triplet（`scripts/triplets/x64-windows-release.cmake`），只装 release 二进制，省约一半磁盘与安装时间。

### 自研替代（不再引入第三方库）

| 原依赖 | 现在的实现 |
|--------|-----------|
| MediaInfoLib / ZenLib | FFmpeg `libavformat`（`core/analyzer/MediaInfoAnalyzer`） |
| QtCharts | 自绘 `ui/charts/MetricChartWidget`（QPainter 折线/柱状/散点） |
| Bento4 | 自研 `utils/IsobmffParser`（ISOBMFF box 树 + sample table） |
| SDL2 | 平台原生音频（WASAPI / ALSA / AudioQueue，`core/player/AudioOutput`） |
| Vulkan | 移除，统一 CPU / QImage 渲染 |

---

## 🧪 运行测试

```bash
# Debug 构建 (启用测试)
cmake -B build/debug -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug

# 运行测试
cd build/debug && ctest --output-on-failure
```

> **注意**：测试源文件 (`tests/unit/*.cpp`) 尚未提交。CMakeLists.txt 已配置编译框架。

---

## ❓ 常见问题

### Q: Windows 报 find_package(Qt6) 失败 / 找不到 Qt6

这通常是**首次构建 vcpkg 正在后台编译 qtbase**（需要 30~60 分钟），或 `VCPKG_ROOT` 环境变量没设对。耐心等 qtbase 编译完成后重跑：
```powershell
# 确认 Qt6 cmake 配置是否已存在
Test-Path "vcpkg_installed\x64-windows-release\share\Qt6\Qt6Config.cmake"
```

### Q: 运行时找不到 DLL（Windows）

`build.bat` 已在构建阶段自动复制 vcpkg DLL、Qt6 插件、FFmpeg DLL 到 `build\release\bin\`。如仍缺失：
```powershell
ls build\release\bin\*.dll
```

### Q: CMake 找不到 FFmpeg（Linux）

```bash
pkg-config --modversion libavcodec libavformat libavutil libswscale libswresample
# 如果缺失：
sudo apt install -y libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

### Q: 硬件解码不工作

```bash
# FFmpeg 的 hwaccel 由 Decoders.cpp 按平台枚举（VAAPI / D3D11VA / DXVA2 / QSV / CUDA / VideoToolbox），
# 任一后端初始化失败都会自动回退软件解码，功能不受影响。
# 排查时看日志里的 "hwaccel" 关键字即可。
```

### Q: 构建内存不足 / OOM

```bash
# Linux: 限制并行数
JOBS=2 ./build.sh debug

# Windows: 通过 preset 传参
cmake --build build/release -- -j2
```

---

## 📚 下一步

- [项目说明与文档导航](README.md)
- [构建与贡献](CONTRIBUTING.md)
- [编码码流解析](docs/BITSTREAM_ANALYSIS.md)
- [诊断与 QC 报告](docs/DIAGNOSTICS_QC.md)
- [音频 QC](docs/AUDIO_QC.md)
- [码率与 GOP 分析](docs/BITRATE_GOP_ANALYSIS.md)
- [色彩与 HDR 元数据分析](docs/COLOR_HDR_ANALYSIS.md)
- [MP4/fMP4 容器一致性校验](docs/MP4_SAMPLE_TABLE.md)

---

**祝你使用愉快!** 🎉
