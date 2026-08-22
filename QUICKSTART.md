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

> 首次构建会自动通过 vcpkg 拉取依赖（OpenCV、Qt6、SDL2 等），FFmpeg 使用 gyan.dev full-shared 预编译共享库（含头文件与导入库，`build.bat ninja` 检测缺失时自动下载，版本由 stamp 记录自动追踪）。

#### Linux (Ubuntu/Debian)

```bash
# 安装系统依赖
sudo apt install -y \
    build-essential cmake ninja-build nasm yasm pkg-config \
    qt6-base-dev qt6-charts-dev \
    libopencv-dev libsdl2-dev zlib1g-dev \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev \
    libvulkan-dev glslc

# 一键构建
./build.sh release
```

#### macOS

```bash
brew install cmake ninja nasm qt@6 opencv sdl2 zlib ffmpeg
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
- 通过 CMakePresets + vcpkg toolchain 自动安装依赖（qtbase、qtcharts、sdl2、zlib）
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
优先级 1: third_party/ffmpeg-prebuilt/   ← gyan.dev full-shared（Windows 推荐）
优先级 2: pkg-config                      ← Linux apt / macOS brew（libavcodec-dev 等）
```

**Windows 用户**：
- `build.bat` 检测到 `third_party/ffmpeg-prebuilt/` 缺失时，自动运行 `scripts/fetch-ffmpeg.ps1` 下载
- 预编译库来自 [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) **release-full-shared**（gyan 唯一带 include/ 头文件与 lib/ 导入库的共享包；essentials 仅含 bin/ 不适合链接开发）

**Linux 用户**：
- 直接 `apt install libavcodec-dev libavformat-dev ...` 即可

### vcpkg 依赖（Windows 自动集成）

项目通过 `vcpkg.json` + `CMakePresets.json` 自动声明并安装依赖，无需手动运行 `vcpkg install`：
- `qtbase` / `qtcharts` — Qt6 GUI + 图表
- `sdl2` — 音频输出
- `zlib` — 压缩库（MediaInfoLib 依赖）

> 使用项目自带的 release-only triplet（`scripts/triplets/x64-windows-release.cmake`），只装 release 二进制，省约一半磁盘与安装时间。

### 源码集成依赖

以下库通过 Git submodule + `add_subdirectory` 集成：
- **MediaInfoLib + ZenLib** — 媒体元数据解析
- **Bento4** — MP4 容器深度解析
- **vulkan-headers** — Vulkan 1.4+ 头文件

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

### Q: Vulkan 硬件解码不工作

```bash
vulkaninfo --summary 2>/dev/null || echo "Vulkan 不可用"
sudo apt install libvulkan-dev glslc   # Ubuntu/Debian
# 软件解码正常工作，无需额外操作
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

- [完整文档](README.md)
- [架构设计](docs/ARCHITECTURE.md)
- [项目结构](docs/PROJECT_STRUCTURE.md)
- [UI 优化设计稿](docs/UI_OPTIMIZATION_DESIGN.md)

---

**祝你使用愉快!** 🎉
