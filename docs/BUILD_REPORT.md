# VideoEye 2.0 编译运行报告

## ✅ 跨平台编译验证通过

### Windows 构建 (Ninja + MSVC)

| 组件 | 版本 | 状态 |
|------|------|------|
| OS | Windows 11 | ✅ |
| 编译器 | MSVC 14.43.34808 | ✅ |
| 构建工具 | Ninja (VS 2022 内置) | ✅ |
| CMake | 3.27+ | ✅ |
| Qt6 | 6.x (vcpkg) | ✅ |
| OpenCV | 4.x (vcpkg) | ✅ |
| SDL2 | 2.x (vcpkg) | ✅ |
| FFmpeg | 8.1.2 (gyan.dev full shared) | ✅ |
| Vulkan | 1.4.309 (bundled headers) | ✅ |

#### 构建过程

```powershell
# 使用 Ninja 构建脚本（推荐）
.\build.bat ninja
```

**build_ninja.ps1 自动完成**:
1. 设置 MSVC 环境变量 (PATH / INCLUDE / LIB)
2. vcpkg 依赖检查 (manifest 模式)
3. CMake 配置 (Ninja 生成器)
4. 编译 (ninja -j4)
5. 复制运行时 DLL (vcpkg + FFmpeg + Qt6 插件)

**编译结果**:
```
[464/464] Linking CXX executable bin\VideoEye.exe
```

- 可执行文件: `build-ninja/bin/VideoEye.exe`
- 大小: ~13.5 MB
- 运行时 DLL: 自动复制到 `bin/` 目录

### Linux 构建 (WSL Debian)

| 组件 | 版本 | 状态 |
|------|------|------|
| OS | Debian 12 (WSL) | ✅ |
| 编译器 | GCC 12.2.0 | ✅ |
| 构建工具 | GNU Make | ✅ |
| CMake | 3.25.1 | ✅ |
| Qt6 | 6.4.2 (apt) | ✅ |
| OpenCV | 4.6.0 (apt) | ✅ |
| SDL2 | 2.26.5 (apt) | ✅ |
| FFmpeg | 62.x (apt pkg-config) | ✅ |
| Vulkan | 1.x (apt) | ✅ |

#### 构建过程

```bash
# 安装依赖
sudo apt install -y \
    build-essential cmake pkg-config nasm yasm \
    qt6-base-dev qt6-multimedia-dev qt6-charts-dev \
    libopencv-dev libsdl2-dev zlib1g-dev \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev \
    libvulkan-dev glslc

# 构建
./build.sh release
```

**CMake 配置输出**:
```
-- FFmpeg: using system pkg-config
-- FFmpeg found: avcodec;avformat;avutil;swscale;swresample
-- Configuring done
-- Generating done
```

**编译结果**:
```
[100%] Built target VideoEye
```

- 可执行文件: `build-release/bin/VideoEye`
- 大小: ~16 MB
- 链接库验证: `ldd` 无缺失库

### FFmpeg 依赖查找策略

CMakeLists.txt 实现三级 fallback 查找：

```
1. find_package(FFMPEG)                    ← vcpkg manifest 模式
2. 已有预编译共享库 (ffmpeg_install/)       ← gyan.dev full shared 构建 (8.1.2)
3. pkg_check_modules(FFMPEG REQUIRED ...)   ← Linux apt 安装
```

- **Windows**: 使用 `build-ninja/ffmpeg_install/` 的预编译共享库 (gyan.dev full shared 8.1.2)
- **Linux**: 使用 apt 安装的系统 FFmpeg (通过 pkg-config)

### Bento4 集成

Bento4 从裸 GLOB 源文件改为独立 CMakeLists.txt + `add_subdirectory`：
- `third_party/Bento4/CMakeLists.txt` 规范化源文件收集
- 按平台选择 Win32/Posix 系统文件
- 导出 `bento4` 静态库 target

### 跨平台编译器选项

| 平台 | Debug | Release | 额外选项 |
|------|-------|---------|----------|
| MSVC | `/Zi /Ob0 /Od /RTC1` | `/O2` | `/permissive- /Zc:__cplusplus /utf-8` |
| GCC/Clang | `-g -O0` | `-O3` | — |

### 构建脚本

| 脚本 | 平台 | 功能 |
|------|------|------|
| `build.bat` | Windows | 入口脚本，支持 `ninja` / `release` / `debug` 参数 |
| `build_ninja.ps1` | Windows | Ninja + MSVC 构建脚本，自动设置环境变量 |
| `build.sh` | Linux/macOS | 构建脚本，含依赖检查 |
| `setup.sh` | Linux/macOS | 环境初始化 + 构建 |

### 运行

```bash
# Windows
build-ninja\bin\VideoEye.exe

# Linux
./build-release/bin/VideoEye
```

---

## 📊 项目统计

### 代码规模

| 模块 | 文件数 | 说明 |
|------|--------|------|
| core/ | 52 | 核心业务层 (player 18 含着色器 + analyzer 24 + model 10) |
| ui/ | 9 | UI 层 (theme 2 + main_window 4 + analysis_panel 2 + app 1) |
| utils/ | 6 | 工具类 (Logger / ConfigManager / ReportExporter) |
| tests/ | 1 | 测试配置 (CMakeLists.txt，源文件待补全) |
| 第三方集成 | 3 | CMakeLists.txt (Bento4 + ZenLib/MediaInfoLib + vulkan-headers) |
| **总计** | **71** | |

### 编译产物

| 平台 | 构建目标 | 产物大小 | 编译单元 |
|------|----------|----------|----------|
| Windows (Ninja) | `VideoEye.exe` | ~13.5 MB | 464 |
| Linux (Make) | `VideoEye` (ELF) | ~16 MB | 175+ |

---

**✅ 跨平台编译验证完成！** Windows MSVC + Ninja 和 Linux GCC + Make 均通过。
