# 贡献指南 — VideoEye

感谢参与 VideoEye 开发！本指南帮助你快速搭建环境并提交代码。

## 环境要求

### Windows
- Visual Studio 2022（勾选「使用 C++ 的桌面开发」工作负载，含 MSVC + Windows SDK + Ninja）
- vcpkg（用于 Qt6；gtest 只在显式开启 `tests` feature 时安装）

### Linux (Debian/Ubuntu)
- GCC 12+, CMake 3.23+, pkg-config, ninja-build（preset 用的就是 Ninja 生成器）
- 依赖包见下方安装命令

## 一键构建

> `CMakePresets.json` 是构建配置的唯一来源，`build.bat` / `build.sh` 只是它的薄封装。
> 新增构建选项请改 preset，不要往脚本里塞 `-DCMAKE_XXX`。

### Windows
```powershell
build.bat            # Release（等价于 cmake --preset win-release + --build）
build.bat debug      # Debug
build.bat clean      # 清理 build/release 与 build/debug
```

无参数时要先满足：环境变量 `VCPKG_ROOT`，且 `ninja` / `cl.exe` 在 PATH 里
（在「x64 Native Tools Command Prompt for VS 2022」中执行即可）。

想自己铺本机路径就在 gitignore 掉的 `CMakeUserPresets.json` 里写，不要改 `CMakePresets.json`。

`build.bat` 每次都会调一次 `scripts/fetch-ffmpeg.ps1`（幂等：目录与
`cmake/ffmpeg-version.json` 的 **url + sha256 + 目录结构**完全对得上时只看几个文件就退出，
不联网）。只比对版本号是不够的 —— 同一个版本号可能对应重新打包的上游构建，
那样本机会静默继续用旧包，编出来的 ABI 与 CI / 别人机器不一致。

### Linux
```bash
# 系统依赖（FFmpeg 走系统开发包，由 pkg-config 查找）
sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev

./build.sh release            # 等价于 cmake --preset linux-release && cmake --build --preset linux-release
./scripts/setup-linux-env.sh  # 依赖自检 + 调用 build.sh
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
| FFmpeg | `scripts/fetch-ffmpeg.ps1`（BtbN 预编译 **`win64-lgpl-shared`，LGPLv3**，版本/URL/SHA256/许可证元数据锁在 `cmake/ffmpeg-version.json`） | 系统包 + `pkg-config`（找不到才回退 `third_party/prebuilt/<platform>/ffmpeg/`；显式给 `-DFFMPEG_ROOT` 时以它为准） |
| GoogleTest（仅测试） | vcpkg feature `tests` | apt (`libgtest-dev`) / brew (`googletest`) |

**项目只有 Qt Widgets + FFmpeg 两个硬依赖。** 媒体信息（FFmpeg `libavformat`）、图表
（`ui/charts/MetricChartWidget`，QPainter 自绘）、MP4 样本表（`core/media/container/IsobmffParser`）、
音频输出（WASAPI / ALSA / AudioQueue）全部自研或走平台原生 API，不再引入
MediaInfoLib / Bento4 / SDL2 / QtCharts / Vulkan。

FFmpeg（Windows）通过 `scripts/fetch-ffmpeg.ps1` 从 BtbN/FFmpeg-Builds 获取 `win64-lgpl-shared` 预编译包（**LGPLv3**，不含 `libx264`/`libx265`），版本号、URL、SHA256 与许可证元数据的唯一来源是 `cmake/ffmpeg-version.json`（校验不过直接失败，不会带着可疑归档继续构建），下载完成后会在 `third_party/prebuilt/windows-x64/ffmpeg/.videoeye-ffmpeg.json` 留一份 stamp 记录实际装的是哪个版本。vcpkg 依赖版本由 `vcpkg-configuration.json`（baseline `2025-04-16`）锁定，确保团队成员依赖一致。

> **许可义务**：分发二进制包时必须带上 `install` 生成的 `doc/THIRD_PARTY_NOTICES.md` 与
> `doc/licenses/`（FFmpeg LGPLv3 + GPLv3 全文、Qt6 copyright、VideoEye MIT）。
> Linux/macOS 用系统 FFmpeg，多数发行版构建带 GPL 组件 —— 那两个平台的分发物仍受 GPL 约束，
> 需要 LGPL-clean 时请自行改用 LGPL 变体。细节见 `docs/FFMPEG_COMMAND_WORKBENCH.md` 第 8 节。

### FFmpeg 支持矩阵

Windows 是锁版本的预编译包，版本永远够；Linux/macOS 用的是**系统** FFmpeg，版本由发行版
决定 —— 这是三平台唯一一处"同一份代码可能编得过/编不过"的地方。所以这里把要求写死，
并由 `cmake/deps/FfmpegApiProbe.cmake` 在 configure 阶段强制执行。

**硬性要求：FFmpeg ≥ 7.1（libavcodec ≥ 61.19）。** 判定方式是**符号探测**而不是猜市场
版本号：直接试着编译三个片段，看代码真正依赖的符号在不在。缺哪个就在报错里点名哪个、
需要哪个版本、怎么换一份够新的，避免一路编到 `AnalysisPipeline.cpp` 才炸出看不懂的宏错误。

| 需要的符号 | 引入版本 | 用途 |
|-----------|---------|------|
| `AV_FRAME_FLAG_KEY` | libavutil 59.39 / FFmpeg **7.1** | 关键帧判定（`AVFrame::key_frame` 字段已废弃） |
| `av_packet_side_data_get()` | libavcodec 61 / FFmpeg 7.0 | HDR / 色彩元数据读取 |
| `AV_CODEC_ID_VVC` | libavcodec 61 / FFmpeg 7.0 | VVC/H.266 分支 |

| 平台 | FFmpeg 来源 | 已验证版本 | 状态 |
|------|------------|-----------|------|
| Windows | 锁定预编译包（`cmake/ffmpeg-version.json`） | 8.1.3（libavcodec 62.28.103） | CI 构建 + 单测 + 冒烟 |
| Linux | 系统 `pkg-config` | 取决于发行版 | CI 每次跑；**低于 7.1 会直接在 configure 失败** |
| macOS | Homebrew / 系统包 | 取决于版本 | **无 CI 验证**（见 README 平台矩阵） |

发行版实际情况（写在这里是因为它们正是"换台机器就编不过"的来源）：

- Ubuntu 24.04 的 `libavcodec-dev` 是 **6.1**，**不满足要求** —— 需要自行准备 ≥ 7.1 的
  FFmpeg 并用 `-DFFMPEG_ROOT=<path>` 指过去。
- Debian 12 是 5.1，同样不满足。
- Homebrew 的 `ffmpeg` 滚动更新，通常是够的，但没人替我们验证过。

**编译选项造成的差异**（版本够了也可能行为不同，这几条必须知道）：

| 选项 | 影响 |
|------|------|
| `--enable-gpl`（多数发行版默认开） | 分发物受 GPL 约束；见 `docs/FFMPEG_COMMAND_WORKBENCH.md` 第 8 节 |
| `libx264` / `libx265` 是否编入 | 影响**导出**：`MediaExporter` 的候选链是 `libx264 → libopenh264 → mpeg4`，Windows 锁定包是 LGPL 构建，实际走 `libopenh264` |
| `libvvdec` / `libvvenc` 是否编入 | 影响**播放** VVC。注意区分：`AV_CODEC_ID_VVC` 存在只说明能识别该编码，不代表有解码器。VideoEye 的 VVC 码流解析是自研的，不依赖解码器 |
| 网络协议 / demuxer 开关 | RTSP / RTMP / HLS / DASH 需要对应的 protocol 与 demuxer；裁剪过的构建会让"打开网络流"在运行时失败 |

**运行时还有一层检查**：`OpenController::CheckRuntimeVersion()` 比对编译期头文件的
libavcodec 主版本号与运行时库的主版本号，不一致时给出明确错误而不是让它在解码处崩 ——
因为"用 7.1 的头文件编、运行时加载 6.1 的 .so"在 Linux 上完全可能发生。

跳过门禁只在一种情况下合理：正在给还不支持的发行版做移植。
`-DVIDEOEYE_FFMPEG_SKIP_API_CHECK=ON` 会打 WARNING 后继续。

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

> CI 会复查这两项（范围都只限**本次改动/新增**的文件，历史文件不追溯）：
> `format` job 对改动的 C++ 文件跑 `clang-format --dry-run --Werror`，不过直接红；
> Linux 测试 job 对改动的 `.cpp` 跑两步 `clang-tidy`：
> 1. 完整 `.clang-tidy` 配置**只报告**（存量风格告警太多，不拦 job）；
> 2. 一组高信噪比 bugprone 检查（use-after-move / dangling-handle /
>    infinite-loop / sizeof-expression / unused-raii 等）以 `--warnings-as-errors`
>    **拦截**。命中即视为缺陷，修复或用带理由的 `// NOLINT(check-name)` 标注。
>
> 本地复现硬门：
> ```bash
> clang-tidy -p build/test-debug --checks='-*,bugprone-use-after-move,bugprone-dangling-handle,bugprone-infinite-loop,bugprone-sizeof-expression,bugprone-unused-raii,bugprone-move-forwarding-reference,bugprone-suspicious-memset-usage,bugprone-undefined-memory-manipulation,bugprone-string-integer-assignment,bugprone-suspicious-semicolon,bugprone-unhandled-exception-at-new' --warnings-as-errors='*' <改动的.cpp>
> ```

### clang-format 装哪个版本（重要）

**必须 18.1.x，且本机与 CI 用同一份二进制。** 理由不是洁癖：clang-format 的换行位置、
注释对齐、include 排序跨大版本都会变，19 认为合规的文件 18 会判不合规。装 20（`winget
install LLVM.LLVM` 拿到的就是最新版）的结果是"本机一直绿、推上去 CI 一直红"，或者反过来
把 19 的格式推上去、CI 用 18 再改回来 —— 两种都是白跑。

```bash
# 本机 / CI 统一用它（PyPI 的 wheel，Windows / Linux 同一个版本号）
python -m pip install "clang-format==18.1.8"
# wheel 里的真二进制在 site-packages/clang_format/data/bin/，把它放进 PATH；
# 不想动 PATH 就设 CLANG_FORMAT=<该二进制的绝对路径>
clang-format --version    # 期望 18.1.x
```

pre-commit 的第 5 道门会核对版本号，不一致时打印警告但不拦截（拦下来只会逼人
`--no-verify`）；没装时同样只警告 —— 所以**"钩子绿"不等于"格式过关"，装好才算数**。

## 提交流程

1. Fork 仓库并创建特性分支：`git checkout -b feature/your-feature`
2. 确保通过本地构建：Windows 用 `build.bat release`（测试用 `build.bat test`），Linux 用 `cmake --build --preset linux-release`
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

| workflow | 触发 | 做什么 |
|---|---|---|
| `build.yml` | 每个 PR / push | 分层与格式门禁、Windows + Linux 构建、单测、asan/tsan |
| `release.yml` | push tag `v*` | 产出安装包 / SHA256 / SBOM，建**草稿** Release |
| `fuzz.yml` | 每周一定时 + 手动 + 解析层改动 | 构建 libFuzzer 入口并各跑一段（默认 120s），崩溃/超时/OOM 会把复现输入传成 artifact |

`fuzz.yml` 的关键参数别删：`-timeout=10`（抓死循环）、`-rss_limit_mb=2048`（抓无界分配）、
`-max_len=65536`（保证跑得完）。历史上码流解析器出过"空转数十亿次"和"4GB 分配"，
只有这两个参数在，fuzz 才真的能抓到它们。手动触发时可指定每个入口跑多久。

`.github/workflows/build.yml` 会在每个 PR 上运行双平台构建。提交前请确保本地构建通过，避免浪费 CI 资源。

本机跑 fuzz（需要 Linux + clang）：

```bash
cmake --preset linux-fuzz
cmake --build --preset linux-fuzz
./build/fuzz/tests/fuzz/FuzzBitstreamExtradata -max_total_time=120 -timeout=10 -rss_limit_mb=2048
```

### UBSan 门禁

`linux-test-asan` 是 ASan + UBSan 一起开的，但 `halt_on_error=0`：Qt 的头文件被 include 进我们的
TU，里面的内联代码也带插桩，开 halt 会整片红在 Qt 噪声上。代价是 UB 不会让进程退出 —— **ctest 不会
变红**，日志里有没有那几行全看运气。

所以 `build.yml` 里另有一道 `UBSan gate`：

- **报告从哪来**：`VE_UBSAN_LOG_DIR` 指向的目录（CI 是 `$GITHUB_WORKSPACE/build/ubsan`），
  UBSan 自己写 `ubsan.<exe>.<pid>`。不能只 grep ctest 日志 —— ctest 只在测试失败时回显输出，
  测试通过时那几行被直接吞掉（`--test-output-size-passed` 也救不回来，实测过），
  门禁会变成永远绿的摆设。
- **判据**：只认落在本仓库源码里的 `runtime error:`，按路径排除 `build/`、`third_party/`、
  `vcpkg_installed/`。同一位置多次出现会合并计数，命中会以 GitHub annotation 标到对应行。
- **首轮摸底**：把仓库变量 `UBSAN_GATE_MODE` 设成 `warn`，照样在 job summary 里列全部命中、
  照样标注释，但不判红。存量清完删掉该变量（或设回 `strict`）即收紧。

本机想收集报告：

```bash
export VE_UBSAN_LOG_DIR=/tmp/ve-ubsan && mkdir -p "$VE_UBSAN_LOG_DIR"
ctest --preset linux-test-asan
ls "$VE_UBSAN_LOG_DIR"
```

不设 `VE_UBSAN_LOG_DIR` 时 preset 里会展开成 `log_path=/ubsan`（不可写），报告退回 stderr ——
交互式调试反而看得见，但一经过 ctest 就没了。

### TSan 门禁

`linux-test-tsan` 原来的问题是反过来的：Qt 不是 TSan-clean 的库（实测一轮 135 个用例产生 68 条竞态，
SUMMARY 全在 `libQt6Core` / `libQt6Gui` 内部的引用计数、`QMetaType::destroy`、渐变光栅化里），
测试进程以 66 退出 → 5 个测试常年红，真属于我们的竞争淹在里面没人看。

现在 `TSAN_OPTIONS` 里加了 `exitcode=0`（噪声不再判红），判据移到 `TSan gate`，和 UBSan 是同一套
`scripts/sanitizer_gate.py`，只是收窄方式不同：

- **报告从哪来**：`VE_TSAN_LOG_DIR`（CI 是 `$GITHUB_WORKSPACE/build/tsan`），TSan 写
  `tsan.<exe>.<pid>`。理由同 UBSan：测试通过时 ctest 不回显输出。
- **判据按"访问点"收窄**：每个报告按 section 切分，取最内层一帧（跳过 libtsan 的拦截器帧）当访问点 ——
  落在 `core/`、`ui/`、`infrastructure/`、`tests/` 里就红；落在 `/usr/include/c++` 或 Qt 头里就放行
  （那是别人内联进我们 TU 的代码）。**分配点/建线程点不参与判定**：那 68 条 Qt 竞态的栈里几乎都有
  我们的帧，但那是"我们调用了 Qt"，不是"冲突访问发生在我们的代码里"。
- **自证不空转**：CI 里有两段探针，一个拿必然溢出的代码验 UBSan 的 `log_path`，一个拿必然竞态的
  程序同时验 TSan 的 `log_path` 与 `exitcode=0`。报告去不了文件、或者 `exitcode` 没生效，job 直接红 ——
  否则门禁最可能的失效方式是"静默变绿"。
- **首轮摸底**：仓库变量 `TSAN_GATE_MODE=warn`，与 `UBSAN_GATE_MODE` 同语义。

本机收集报告：

```bash
export VE_TSAN_LOG_DIR=/tmp/ve-tsan && mkdir -p "$VE_TSAN_LOG_DIR"
ctest --preset linux-test-tsan
python3 scripts/sanitizer_gate.py --kind tsan --log-dir "$VE_TSAN_LOG_DIR" --workspace "$PWD"
```

只有"abort 整个测试二进制"的那几种（QProcess fork、libtsan 线程注册表 CHECK）才用
`tests/unit/SanitizerSkip.h` 的 `SKIP_UNDER_TSAN` 跳过；普通竞态一律走门禁，不许跳过。

想验证门禁本身有没有判别力：`python3 scripts/test_sanitizer_gate.py`（CI 的 layering job 也在跑）。
