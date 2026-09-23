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

> `CMakePresets.json` 是构建配置的唯一来源，`build.bat` / `build.sh` / `build_ninja.ps1`
> 只是它的薄封装。新增构建选项请改 preset，不要往脚本里塞 `-DCMAKE_XXX`。

### Windows
```powershell
build.bat            # Release（等价于 cmake --preset win-release + --build）
build.bat debug      # Debug
build.bat clean      # 清理 build/release 与 build/debug
```

无参数时要先满足：环境变量 `VCPKG_ROOT`，且 `ninja` / `cl.exe` 在 PATH 里
（在「x64 Native Tools Command Prompt for VS 2022」中执行即可）。

想自己铺本机路径就在 gitignore 掉的 `CMakeUserPresets.json` 里写，不要改 `CMakePresets.json`。
FFmpeg 缺失时会自动调用 `scripts/fetch-ffmpeg.ps1`。

### Linux
```bash
# 系统依赖（FFmpeg 走系统开发包，由 pkg-config 查找）
sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev

./build.sh release            # 等价于 cmake --preset linux-release && cmake --build --preset linux-release
./setup.sh                    # 依赖自检 + 调用 build.sh
```

## 单元测试

```bash
cmake --preset linux-test-debug         # Windows: win-test-release / win-test-debug
cmake --build --preset linux-test-debug
ctest --preset linux-test-debug
```

Windows 的 `-test-` preset 额外带 `VCPKG_MANIFEST_FEATURES=tests`，gtest 由 vcpkg 安装。
建议用 `win-test-release`：Debug 配置需要重编整套 debug triplet 的 Qt，慢得多。

## 依赖说明

| 依赖 | Windows 来源 | Linux / macOS 来源 |
|------|-------------|-----------|
| Qt6 (Widgets) | vcpkg manifest | apt (`qt6-base-dev`) / brew (`qt@6`) |
| FFmpeg | `scripts/fetch-ffmpeg.ps1`（gyan.dev 预编译，版本锁在 `cmake/ffmpeg-version.json`） | 系统包 + `pkg-config`（找不到才回退 `third_party/prebuilt/<platform>/ffmpeg/`） |
| GoogleTest（仅测试） | vcpkg feature `tests` | apt (`libgtest-dev`) / brew (`googletest`) |

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
