# 贡献指南 — VideoEye

感谢参与 VideoEye 开发！本指南帮助你快速搭建环境并提交代码。

## 环境要求

### Windows
- Visual Studio 2022（勾选「使用 C++ 的桌面开发」工作负载，含 MSVC + Windows SDK + Ninja）
- vcpkg（用于 Qt6；gtest 只在显式开启 `tests` feature 时安装）

### Linux (Debian/Ubuntu)
- GCC 12+, CMake 3.21+, pkg-config, make
- 依赖包见下方安装命令

## 一键构建

### Windows
```powershell
# 1. 安装 vcpkg 依赖（首次）
#    追加 --x-feature=tests 才会安装 gtest（跑单元测试时）
vcpkg install --triplet x64-windows-release --host-triplet x64-windows-release \
  --overlay-triplets=scripts/triplets --overlay-ports=scripts/overlay-ports --x-manifest-root=. --x-install-root=vcpkg_installed

# 2. 构建（脚本自动探测 MSVC、自动获取 FFmpeg、复制 DLL）
powershell -ExecutionPolicy Bypass -File build_ninja.ps1
```

或使用 CMake Presets（需先在「Developer PowerShell for VS 2022」中运行）：
```powershell
cmake --preset win-release
cmake --build --preset win-release
```

### Linux
```bash
# 1. 安装系统依赖
sudo apt install -y build-essential cmake pkg-config ninja-build qt6-base-dev

# 2. 准备 FFmpeg 预编译包（唯一查找路径，不再走 pkg-config）
#    third_party/prebuilt/linux-x64/ffmpeg/{include,lib,bin}

# 3. 构建
./setup.sh            # 一键：检查依赖 + 编译
# 或
cmake --preset linux-release && cmake --build --preset linux-release
```

## 依赖说明

| 依赖 | Windows 来源 | Linux 来源 |
|------|-------------|-----------|
| Qt6 (Widgets) | vcpkg | apt (qt6-base-dev) |
| FFmpeg | `scripts/fetch-ffmpeg.ps1`（gyan.dev 预编译） | 预编译包放到 `third_party/prebuilt/linux-x64/ffmpeg/` |

**项目只有 Qt Widgets + FFmpeg 两个硬依赖。** 媒体信息（FFmpeg `libavformat`）、图表
（`ui/charts/MetricChartWidget`，QPainter 自绘）、MP4 样本表（`utils/IsobmffParser`）、
音频输出（WASAPI / ALSA / AudioQueue）全部自研或走平台原生 API，不再引入
MediaInfoLib / Bento4 / SDL2 / QtCharts / Vulkan。

FFmpeg（Windows）通过 `scripts/fetch-ffmpeg.ps1` 从 gyan.dev 获取预编译包，版本记录在 `third_party/ffmpeg-prebuilt/.videoeye-ffmpeg.json`。vcpkg 依赖版本由 `vcpkg-configuration.json`（baseline `2025-04-16`）锁定，确保团队成员依赖一致。

## 代码风格

- 格式化：`.clang-format`（LLVM 风格，4 空格缩进，120 列宽）
- 静态检查：`.clang-tidy`（启用 modernize/bugprone/readability/performance）
- 编辑器统一：`.editorconfig`

提交前自动检查（推荐）：
```bash
bash scripts/setup-hooks.sh    # 安装 pre-commit hook（检查暂存文件格式）
```

手动格式化：
```bash
clang-format -i $(git diff --name-only -- '*.cpp' '*.h')
```

## 提交流程

1. Fork 仓库并创建特性分支：`git checkout -b feature/your-feature`
2. 确保通过本地构建：Windows 用 `build_ninja.ps1`，Linux 用 `cmake --build --preset linux-release`
3. 运行格式检查：`clang-format --dry-run --Werror $(git diff --name-only)`
4. 提交（遵循约定式提交）：
   ```
   feat: 新增 XXX 分析
   fix: 修复 XXX 崩溃
   refactor: 重构 XXX
   docs: 更新文档
   ```
5. 推送并发起 PR，CI 会自动在 Windows + Linux 双平台构建验证。

## CI

`.github/workflows/build.yml` 会在每个 PR 上运行双平台构建。提交前请确保本地构建通过，避免浪费 CI 资源。
