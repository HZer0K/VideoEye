# VideoEye 2.0 快速入门指南

## 🚀 5 分钟快速开始

### 1. 克隆项目

```bash
git clone https://github.com/HZer0K/VideoEye.git
cd VideoEye
```

### 2. 构建项目

> **构建配置的唯一来源是 `CMakePresets.json`。** `build.bat` / `build.sh` / `build_ninja.ps1`
> 都只是它的薄封装（搭好编译器环境 + 取 FFmpeg + 调 `cmake --preset`），不要再往脚本里
> 加 `-DCMAKE_XXX` 参数。

#### Windows (Ninja + MSVC)

```bat
build.bat            :: Release 构建（默认）
build.bat debug      :: Debug 构建
build.bat test       :: Release 构建 + 跑单元测试
build.bat clean      :: 清理构建目录
```

前置：Visual Studio 2022（勾「使用 C++ 的桌面开发」）+ `VCPKG_ROOT` 环境变量。
首次构建由 vcpkg 编译 qtbase（30~60 分钟），之后走缓存增量。

#### Linux (Ubuntu/Debian)

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev

./build.sh release     # 或 ./build.sh debug
./build.sh test        # Debug + 单元测试，编译完自动跑 ctest
JOBS=2 ./build.sh release   # 限制编译并行数（默认吃满所有核）
```

#### macOS

```bash
brew install cmake ninja qt@6 ffmpeg
./build.sh release
```

### 3. 运行

```bash
# Windows
build\release\bin\VideoEye.exe

# Linux / macOS
./run.sh release       # 等价于 build/release/bin/VideoEye
```

> 项目只有两个硬依赖：**Qt Widgets + FFmpeg**。

---

## 📦 构建入口

| 命令 | 说明 |
|------|------|
| `build.bat` / `build.bat release` | Windows Release **推荐** |
| `build.bat debug` | Windows Debug |
| `build.bat test` | Windows Release + 跑单元测试（自带 gtest） |
| `build_ninja.ps1 [-BuildType Debug]` | `build.bat` 的转发器（历史入口，保留兼容） |
| `./build.sh [release\|debug\|test\|clean]` | Linux / macOS 构建（`test` 带 ctest） |
| `./setup.sh` | Linux / macOS 环境自检 + 调用 `build.sh` |
| `./run.sh [release\|debug]` | 构建过就直接跑，没构建过先构建 |

产物路径统一为 `build/<config>/bin/VideoEye[.exe]`：

```
build/release/bin/VideoEye[.exe]
build/debug/bin/VideoEye[.exe]
build/test-release/bin/   ← 带单元测试的构建
```

所有 Windows 脚本最终都指向同一套 preset；本机专有路径（VS 版本 / SDK / cl.exe / ninja）
写在 **gitignore 掉的** `CMakeUserPresets.json` 里，不要提交。

---

## 🔧 依赖说明

### FFmpeg：Windows 用预编译包，Linux / macOS 用系统包

| 平台 | 查找方式 | 说明 |
|------|---------|------|
| Windows | `third_party/prebuilt/windows-x64/ffmpeg/{include,lib,bin}` | 必须有 `.lib` 导入库才能链接；缺失时 `build.bat` 自动调 `scripts/fetch-ffmpeg.ps1` |
| Linux / macOS | `pkg-config` 找系统包（`libavcodec`…） | 找不到才回退到 `third_party/prebuilt/<platform>/ffmpeg/` |

相关开关：

```bash
-DFFMPEG_ROOT=/path/to/ffmpeg          # 强制指定 FFmpeg 根目录
-DVIDEOEYE_FFMPEG_USE_PKGCONFIG=OFF    # 关掉 pkg-config，只用预编译包
-DVIDEOEYE_BUNDLE_FFMPEG=OFF           # 不把动态库复制进产物（用系统库路径运行）
```

**Windows 版本锁定**：版本号、URL、SHA256 全部来自 `cmake/ffmpeg-version.json`（当前 **8.1.2**），
所有脚本读同一个文件。想要升级版本，改这一个文件即可 —— 改完 `sha256` 必须同步更新，
`scripts/fetch-ffmpeg.ps1` 在校验不过时会**直接失败**，不会带着一个可疑的归档继续构建。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1            # 用锁定版本
powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1 -Force     # 强制重下
```

### Qt6

- Windows：vcpkg manifest（`vcpkg.json`），preset 自动触发安装
- Linux：`qt6-base-dev` / macOS：`qt6`

### 运行时部署是怎么做的

| 平台 | 动态库放哪 | 靠什么找到 |
|------|-----------|-----------|
| Windows | 与可执行文件同目录 `bin/*.dll` | Windows DLL 查找规则 + POST_BUILD 复制 |
| Linux | 相邻 `lib/*.so` | RPATH `$ORIGIN/../lib` |
| macOS | 相邻 `lib/*.dylib` | RPATH `@loader_path/../lib` |

构建树和安装树是同一套相对布局（`bin/` + `lib/`），所以一条 RPATH 两边都能用。
**每次构建后都会重新校验动态库确实落到位**，缺失会直接让构建失败 —— 避免拖到运行时
才报"找不到 xxx.dll"这种和构建无关的错。`install` 也会把 FFmpeg 动态库一起装上。

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

单元测试用独立的 preset（`BUILD_TESTING=ON`；Windows 上额外带 `VCPKG_MANIFEST_FEATURES=tests`
把 gtest 交给 vcpkg 安装）：

```bash
# Windows（也可以用 build.bat test 一步到位）
#   推荐 release：debug 需要重编一整套 debug triplet 的 Qt，约 1 小时
cmake --preset win-test-release
cmake --build --preset win-test-release
ctest --preset win-test-release

# Linux / macOS（也可以一步到位: ./build.sh test）
cmake --preset linux-test-debug
cmake --build --preset linux-test-debug
ctest --preset linux-test-debug
```

GoogleTest 默认要求本地已有；确需联网拉取时显式加 `-DVIDEOEYE_FETCH_GTEST=ON`。

在 Windows 上手工确认 vcpkg 侧：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-vcpkg-install.ps1 -Config release -Feature tests
```

---

## ❓ 常见问题

### Q: Windows 报 find_package(Qt6) 失败 / 找不到 Qt6

多数是首次构建 vcpkg 正在后台编译 qtbase（30~60 分钟），或 `VCPKG_ROOT` 没设对。
判断 Qt6 的 cmake 配置是否真的装好了：

```powershell
Test-Path "vcpkg_installed\x64-windows-release\share\Qt6\Qt6Config.cmake"
```

> 只看 `include/` 存在不存在会误判 —— vcpkg 安装失败时会把 qtbase 从 installed tree 摘掉，
> 但 zlib/freetype 还在。

### Q: 运行时找不到 DLL（Windows）

动态库由 `cmake/deps/RuntimeDeploy.cmake` 统一部署到可执行文件同目录：先拷 CMake 链接图里
已知的 Qt / FFmpeg DLL，再用 `file(GET_RUNTIME_DEPENDENCIES)` 递归扫 PE 导入表把 Qt 私有的
`zlib1.dll` / `pcre2-16.dll` / `double-conversion.dll` 这类「不在链接图里」的补齐，
最后 windeployqt 补 Qt 插件（`platforms/`）。少了任何一步都会是启动时的
`0xc0000135`，而不是构建期报错。

核对：

```powershell
ls build\release\bin\*.dll
ls build\release\bin\platforms
```

漏了什么可以用扫描脚本自查（`-DVE_DRY_RUN=ON` 只列要补的文件，不动磁盘）：

```powershell
cmake -DVE_INPUTS="build\release\bin\VideoEye.exe" ^
      -DVE_DEST="build\release\bin" ^
      "-DVE_SEARCH_DIRS=third_party\prebuilt\windows-x64\ffmpeg\bin;vcpkg_installed\x64-windows-release\bin" ^
      -DVE_DRY_RUN=ON -P cmake\deps\RuntimeDepsScan.cmake
```

### Q: CMake 找不到 FFmpeg（Linux）

Linux 走 pkg-config，先确认开发包在不在：

```bash
pkg-config --modversion libavcodec libavformat libavutil libswscale libswresample
# 缺失则：
sudo apt install -y libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

### Q: Linux 上程序起不来，报 library not found

动态库在 `build/release/lib`，靠 RPATH `$ORIGIN/../lib` 找。检查文件在不在：

```bash
ls build/release/lib
readelf -d build/release/bin/VideoEye | grep -i rpath
```

### Q: 硬件解码不工作

FFmpeg 的 hwaccel 由 `Decoders.cpp` 按平台枚举（VAAPI / D3D11VA / DXVA2 / QSV / CUDA / VideoToolbox），
任一后端初始化失败都会自动回退软件解码，功能不受影响。排查时看日志里的 `hwaccel` 关键字。

### Q: 构建内存不足 / OOM

```bash
# Linux: 限制并行数
JOBS=2 ./build.sh debug

# Windows: 通过 build 参数控制
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
