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

> 首次构建会自动通过 vcpkg 拉取依赖（OpenCV、Qt6、SDL2 等），FFmpeg 使用已有源码编译产物。

#### Linux (Ubuntu/Debian)

```bash
# 安装系统依赖
sudo apt install -y \
    build-essential cmake ninja-build nasm yasm pkg-config \
    qt6-base-dev qt6-multimedia-dev qt6-charts-dev \
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
| `.\build.bat ninja` | Ninja + MSVC 构建（推荐） |
| `.\build.bat release` | Visual Studio 生成器 Release |
| `.\build.bat debug` | Visual Studio 生成器 Debug |
| `powershell -File build_ninja.ps1` | 直接运行 Ninja 构建脚本 |

**build_ninja.ps1** 自动完成：
- 设置 MSVC 环境变量 (PATH / INCLUDE / LIB)
- CMake 配置 (Ninja 生成器)
- 编译 (ninja -j4)
- 复制 vcpkg DLL + Qt6 插件 + FFmpeg DLL 到 bin/

> **注意**: Windows 上 MSBuild 可能被标记为 LOLBin，使用 Ninja 生成器可绕过此限制。

### Linux 构建

| 命令 | 说明 |
|------|------|
| `./build.sh release` | Release 构建 (默认) |
| `./build.sh debug` | Debug 构建 |
| `./setup.sh` | 完整初始化 + 构建 |
| `./setup.sh --build-only` | 仅编译，跳过依赖检查 |

---

## 🔧 依赖说明

### FFmpeg 依赖管理

CMake 采用**三级 fallback**策略自动查找 FFmpeg：

```
优先级 1: find_package(FFMPEG)        ← vcpkg 已安装 ffmpeg 包
优先级 2: 源码编译产物                  ← build-ninja/ffmpeg_install/ 等
优先级 3: pkg-config                   ← Linux apt 安装 (libavcodec-dev 等)
```

**Windows 用户**:
- 如果 vcpkg 的 FFmpeg 不含 vulkan feature，构建脚本会自动使用已有的源码编译产物
- 首次需要通过 `build_ninja.ps1` 构建 FFmpeg（脚本内集成）

**Linux 用户**:
- 直接 `apt install libavcodec-dev libavformat-dev ...` 即可
- 无需从源码编译 FFmpeg

### vcpkg 依赖 (Windows)

项目根目录的 `vcpkg.json` 声明了以下依赖：
- `opencv4` — 计算机视觉
- `qtbase` / `qtmultimedia` / `qtcharts` — Qt6 模块
- `sdl2` — 音频输出
- `zlib` — 压缩库 (MediaInfoLib 依赖)

### 源码集成依赖

以下库通过 Git submodule + `add_subdirectory` 集成：
- **MediaInfoLib + ZenLib** — 有定制改动，源码集成保留灵活性
- **Bento4** — vcpkg 版本过旧 (1.5.1 vs 1.6.0)，独立 CMakeLists.txt 集成
- **vulkan-headers** — Vulkan 1.4.309 头文件，替代系统旧版

---

## 🧪 运行测试

```bash
# Debug 构建
./build.sh debug

# 运行测试
cd build-debug && ctest --output-on-failure
# 预期: 6 suites, 115 cases passed
```

---

## ❓ 常见问题

### Q: Windows 构建报 "MSBuild is blocked"

使用 Ninja 构建器绕过：
```powershell
.\build.bat ninja
```

### Q: vcpkg install 报 FFmpeg vulkan feature 错误

这是 vcpkg 版本的 FFmpeg 不支持 vulkan feature。CMake 会自动回退到已有的源码编译产物。确保 `build-ninja/ffmpeg_install/` 目录存在。

### Q: 运行时找不到 DLL

Windows 上运行 `build_ninja.ps1` 会自动复制所有需要的 DLL。如果仍缺失：
```powershell
# 检查 vcpkg_installed 目录
ls vcpkg_installed\x64-windows\bin\*.dll
```

### Q: CMake 找不到 FFmpeg (Linux)

```bash
# 确认 pkg-config 能找到 FFmpeg
pkg-config --modversion libavcodec libavformat libavutil libswscale libswresample

# 如果缺失，安装开发包
sudo apt install -y libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

### Q: Vulkan 硬件解码不工作

```bash
# 检查 Vulkan 驱动
vulkaninfo --summary 2>/dev/null || echo "Vulkan 不可用"

# 安装 Vulkan SDK
sudo apt install libvulkan-dev glslc

# 若驱动版本太低，FFmpeg 会自动跳过 Vulkan 硬件解码
# 软件解码正常工作，无需额外操作
```

### Q: 构建内存不足

```bash
# Linux: 限制并行编译数
JOBS=2 ./build.sh release

# Windows: 修改 build_ninja.ps1 中的 -j4 为 -j2
```

---

## 📚 下一步

- [完整文档](README.md)
- [架构设计](docs/ARCHITECTURE.md)
- [项目结构](docs/PROJECT_STRUCTURE.md)
- [UI 优化设计稿](docs/UI_OPTIMIZATION_DESIGN.md)

---

**祝你使用愉快!** 🎉
