# 构建脚本整合方案

> 状态：**P0 / P1 已执行，P2 部分执行**。详见文末「执行记录」。
> 目标不是"把文件数量减下来"，而是让每个文件都有且只有一个职责，且职责边界能被机器盯住。

## 1. 现状盘点

### 1.1 根级入口

| 文件 | 行数 | 实际职责 | 结论 |
|------|------|----------|------|
| `build.bat` | 159 | Windows 真入口：vswhere + vcvars + FFmpeg 检查 + preset 名探测 + configure/build/ctest | 保留 |
| `build.sh` | 137 | Linux/macOS 真入口：preset 薄封装 + 缺依赖提示 + `JOBS` 透传 | 保留，扩到 Windows bash |
| `build_ninja.ps1` | 40（有效 26） | `build.bat` 的转发器，14 行注释在解释自己为什么被废弃 | 删 |
| `setup.sh` | 79 | Linux 依赖自检 + 调 `build.sh` | 移入 `scripts/` |
| `run.sh` | 45 | 没构建过就先构建，再启动 | 保留（或并进 `build.sh run`） |

### 1.2 `scripts/` 下与构建相关

| 文件 | 行数 | 实际职责 | 结论 |
|------|------|----------|------|
| `setup-windows-env.ps1` | ~500 | 一次性：装 VS + CMake + vcpkg + Qt，生成 `CMakeUserPresets.json` | 保留 |
| `run-vcpkg-install.ps1` | ~80 | vcpkg install 封装，本机与 CI 共用 | 保留 |
| `fetch-ffmpeg.ps1` | ~400 | 下载并强校验 FFmpeg 预编译包 | 保留 |
| `build-ninja-sandbox.sh` | 65 | 沙箱内 Windows 构建封装（沙箱禁 `cmd.exe`，跑不了 `build.bat`）+ 日志 GBK/UTF-16 转码 | 并进 `build.sh` |
| `repro_audio_analysis.sh` | 35 | 音频分析复现：手搓 `cmake -S . -B build-debug` | 删或接 preset |
| `run-audioqc-tests.sh` | 52 | 绕开 CMake 直接调 `cl.exe` 编单测 | 删 |

### 1.3 磁盘占用（`build/` 共约 4.1G）

| 路径 | 大小 | 最后修改 | 性质 |
|------|------|----------|------|
| `build/release` | 758M | 2026-10-11 22:27 | **保留** · preset `release` 产物 |
| `build/debug` | 22M | 2026-10-11 23:15 | **保留** · preset `debug` 产物 |
| `build/test-release` | 613M | 2026-10-10 | 保留 · preset `test-release`（注：该目录未开 tests 特性，见 §2.4） |
| `build/_recheck_test` | 839M | 2026-10-11 16:09 | **保留（待正名）** · 当前唯一的验证用目录，但没有对应 preset |
| `build/_recheck` | 388M | 2026-09-27 | 孤儿 |
| `build/_verify_test` | 348M | 2026-09-26 | 孤儿 |
| `build/_f9` | 330M | 2026-09-26 | 孤儿 |
| `build/_verify` | 317M | 2026-09-24 | 孤儿 |
| `build/cfg18` | 5.6M | 2026-10-07 | 孤儿 |
| `build/test` | 7.2M | 2026-10-06 | 孤儿 |
| `build/yamlcheck` | 1.8M | 2026-10-07 | 孤儿 |
| `build/_ide_probe` | 10K | 2026-10-11 | 孤儿 |
| `build/test-cfg{,3,4}` | 17K | 2026-09-01 | 孤儿 |
| `build/generated` | 1.0K | 2026-09-27 | 孤儿 |
| `build/verify-rerun.log` | 332K | — | 孤儿 |
| `build-ninja/`（仓库根） | 324M | 2026-10-09 | 旧布局残留，已被 `.gitignore` |

可回收合计 **约 2.8G**（`build/` 内 2.5G + `build-ninja/` 324M）。

## 2. 问题诊断（根因，不是症状）

### 2.1 preset 名探测是消费端的 workaround

`build.bat:95-102` 在 `win-<x>-local > <x> > win-<x>` 三种拼写里猜真正的 preset 名。
根因在生成侧：`scripts/setup-windows-env.ps1` 输出 `win-<x>-local`，手写 `CMakeUserPresets.json` 可能只写 `<x>`。
2026-10-06 修 `No such preset: "win-release-local"` 时是在消费端猜名字，正确做法是生成侧统一拼写、消费端直接删掉探测。

### 2.2 两个脚本在绕开"preset 是唯一来源"这条自己立的规矩

- `scripts/repro_audio_analysis.sh:21` — `cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug`，
  产出落 `build-debug/`，与 preset 约定的 `build/<config>/` 不一致。
- `scripts/run-audioqc-tests.sh:5` — MSVC 版本号 `14.44.35207` 硬编码，VS 一升级全废；
  且它绕开 CMake 直接 `cl.exe`，是 gtest 还没进 ctest 那个时代的产物。

### 2.3 三个名字还留着已废弃的旧布局

`build_ninja.ps1`、`scripts/build-ninja-sandbox.sh`、仓库根 `build-ninja/` 都指向 2026-09-23 之前的
"根目录一个构建目录"布局。名字本身就在误导后来的人（项目记忆里现在还留着一堆 `build-ninja/` 的旧记录）。

### 2.4 `build/test-release` 与 `build/_recheck_test` 的关系（实测修正）

写方案时判断"`build/test-release` 没开 tests 特性"——**这个判断是错的**。实测它的
`CMakeCache.txt` 里 `VCPKG_MANIFEST_FEATURES=tests` 和 `GTest_DIR` 都在，gtest 也确实装在
`vcpkg_installed/x64-windows-release/share/gtest/`。

真正卡住配置的是 **`VCPKG_MANIFEST_INSTALL`**：本机 `vcpkg.exe` 起不来子进程
（`ERROR_PIPE_BUSY`），任何没关掉 manifest install 的 preset 都会在 configure 阶段挂掉。
`build/_recheck_test` 当初能建起来，正是因为手搓命令时带了 `-DVCPKG_MANIFEST_INSTALL=OFF`。

也就是说：**验证目录没有 preset 对应这件事，掩盖了真正的故障点**。把
`VCPKG_MANIFEST_INSTALL=OFF` 写进 `CMakeUserPresets.json` 的本机 preset，正式的
`test-release` 就能用，`_recheck_test` 这个手搓目录就该退场。

### 2.5 ⚠️ 约束：`build.bat` 的 vswhere/vcvars 分支不能砍

`.github/workflows/build.yml` 用 `shell: cmd` 跑 `build.bat release`。CI checkout 出来的仓库
**不含 `CMakeUserPresets.json`**（已 gitignore），所以 CI 走的是
"无 local preset → 靠 `VCPKG_ROOT` 环境变量 + vcvars 注入"这条路。

结论：**"把 `build.sh` 扩成跨平台"只能覆盖"本机有 `CMakeUserPresets.json`"的场景，
不能替代 `build.bat`**。两个入口会长期共存，但职责可以划清：

- `build.bat` — Windows，自带 MSVC 环境引导（cmd / CI / 无 user preset 的机器）
- `build.sh` — 有 user preset 或 Linux/macOS（bash 环境，含 Windows 的 Git Bash）

## 3. 目标分层

```
第 0 层  配置（唯一来源，不动）
         CMakePresets.json        通用，无本机路径
         CMakeUserPresets.json    本机 VS/SDK，gitignore

第 1 层  构建（2 个入口，职责按"谁来注入 MSVC 环境"划分）
         build.bat                Windows cmd / CI，自带 vcvars 引导
         build.sh                 bash 环境（Linux/macOS + Windows Git Bash）

第 2 层  环境准备（一次性，全部归入 scripts/）
         scripts/setup-windows-env.ps1   装 VS+vcpkg+Qt，生成 user preset
         scripts/run-vcpkg-install.ps1   本机与 CI 共用
         scripts/fetch-ffmpeg.ps1        FFmpeg 预编译包
         scripts/setup-linux-env.sh      （原根级 setup.sh）

第 3 层  启动（1 个）
         run.sh                   构建过就跑，没构建过先构建
```

## 4. 迁移步骤

### P0 — 纯删除与归位，不改任何构建行为

| # | 动作 | 验证 |
|---|------|------|
| 1 | 删 `build_ninja.ps1` | 全项目 grep 无引用 |
| 2 | 删 `scripts/run-audioqc-tests.sh` | 其覆盖的两个用例已在 ctest 里 |
| 3 | `scripts/repro_audio_analysis.sh` 改用 `cmake --preset linux-debug`，或直接删 | 跑一次确认产物落 `build/debug` |
| 4 | `setup.sh` → `scripts/setup-linux-env.sh` | `bash scripts/setup-linux-env.sh --skip-deps` |
| 5 | 同步 `QUICKSTART.md` / `CONTRIBUTING.md` / `README.md` 的入口表格 | 文档 grep 无已删脚本名 |

风险：低。全部是"删掉没有第二处引用的东西"。

### P1 — `build.sh` 扩成跨平台，吃掉 sandbox 脚本

`build.sh` 增加平台分支：

- `uname` 含 `MINGW`/`MSYS`/`CYGWIN` → 视为 Windows：
  - `unset VCPKG_ROOT`（系统变量可能指向 VS 自带的错误 vcpkg，会覆盖 preset）
  - preset 前缀用 `win-`，并沿用与 `build.bat` 相同的 `win-<x>-local > <x> > win-<x>` 探测
  - 日志落盘后按 UTF-16 / UTF-8 / GBK 顺序转码再打印（这段直接搬 `build-ninja-sandbox.sh:47-62`）
- 其他 → 现有 `linux-*` 逻辑不变

完成后删 `scripts/build-ninja-sandbox.sh`。

风险：中。`build.bat` 与 `build.sh` 会出现两份 preset 探测逻辑 —— 建议同时做 P2-1，
让探测只存在一份或直接消失。

### P2 — 消除 workaround，给验证目录正名

| # | 动作 | 说明 |
|---|------|------|
| 1 | `setup-windows-env.ps1` 固定输出一种 preset 拼写 | 建议统一为 `<x>`（短且与 CMakePresets 的语义一致），生成侧改完后 `build.bat` 的探测循环可整体删除 |
| 2 | 新增 `win-test-verify` preset，`binaryDir` 指向 `build/_recheck_test` | 让当前唯一的验证目录有名字可依，不再靠手写 cmake 命令活着 |
| 3 | `build.bat clean` / `build.sh clean` 改为清 `build/` 下**全部**子目录 | 现在只写死清 `build/release`、`build/debug`、`build/test-debug` 三个，孤儿目录永远清不掉 |
| 4 | `.gitignore` 补 `build/_*`、`build/*-cfg*` | 防止新孤儿被误提交 |

风险：P2-1 会让老机器上手写的 `CMakeUserPresets.json` 与生成器产出不一致，
需要跑一次生成器重新对齐；建议在文档里写明"改 preset 拼写后请重跑 `setup-windows-env.ps1`"。

## 5. 影响面清单（动手前必须同步）

| 位置 | 引用内容 |
|------|----------|
| `.github/workflows/build.yml` | `build.bat release`（L256）、`build.bat test`（L370）、`scripts/run-vcpkg-install.ps1`（L248/L364） |
| `QUICKSTART.md` | L14、L21-24、L36-38、L55、L66-72、L93、L109-113、L151、L157、L168、L239 |
| `CONTRIBUTING.md` | L17、L22-24、L31、L39-40、L59、L67、L95 |
| `README.md` | L46、L164、L175-177、L195-196 |
| `docs/FFMPEG_COMMAND_WORKBENCH.md` | L189（`fetch-ffmpeg.ps1`） |
| `.workbuddy/memory/*.md` | 大量 `build_ninja.ps1` / `build-ninja/` 历史记录（历史日志，不建议回改） |

## 7. 执行记录（2026-10-11）

### P0 — 已完成

| # | 动作 | 结果 |
|---|------|------|
| 1 | 删 `build_ninja.ps1` | 已删；CMakePresets.json / QUICKSTART / CONTRIBUTING / PROJECT_STRUCTURE / DEVELOPMENT_NOTES / run-vcpkg-install.ps1 六处引用已同步 |
| 2 | 删 `scripts/run-audioqc-tests.sh` | 已删；其两个用例（`AudioQcAnalyzerTests` / `AudioQcRulesTests`）已在 ctest 里，docs/AUDIO_QC.md 的跑法改成 `ctest -R "AudioQc"` |
| 3 | `repro_audio_analysis.sh` 接 preset | 改用 `cmake --preset linux-debug`，产物落 `build/debug`；去掉了可被 `BUILD_DIR` 环境变量指到别处的口子 |
| 4 | `setup.sh` → `scripts/setup-linux-env.sh` | 已移；脚本内 `DIR` 换成 `PROJECT_ROOT`（`cd scripts/..`），避免依赖调用位置 |
| 5 | 文档同步 | 见上，另补 README 目录树的 scripts/ 说明 |

### P1 — 已完成

`build.sh` 重写为跨平台入口，新增：平台判定（`uname` 含 `MINGW`/`MSYS`/`CYGWIN` 视为 Windows）、
`win-*` / `linux-*` preset 前缀映射、preset 短名→通用名两级回退、Windows 下 `unset VCPKG_ROOT`、
MSVC 日志落盘后按 UTF-16 / UTF-8 / GBK 顺序转码、`clean` 清 `build/` 下全部子目录。
`scripts/build-ninja-sandbox.sh` 已删（能力已并入）。

实测：本机 Git Bash 下 `./build.sh test` 走通 configure → build → ctest 全流程，
preset 正确解析为短名 `test-release`。

### P2 — 部分完成

| # | 动作 | 结果 |
|---|------|------|
| 1 | 生成器统一 preset 拼写 | 已完成。`setup-windows-env.ps1` 现在生成 `release` / `debug` / `test-release` / `test-debug` 短名；`build.bat` 探测从三变体简化为两级；`CMakePresets.json` vendor 新增 `naming-local` 条目固化约定 |
| 2 | 验证目录正名 | **未完成，且结论被推翻** —— 见下节 |
| 3 | `clean` 清全部 | 已完成（`build.bat` 与 `build.sh` 都改成遍历 `build/` 下所有一级子目录） |
| 4 | `.gitignore` 补规则 | **不需要** —— 现有 `.gitignore` 已整体忽略 `build/` 与 `build-ninja/`，孤儿目录进不了仓库。真正的问题只是没人清理磁盘 |

### ⚠️ 执行中发现：`win-test-release` 在 gtest 1.16.0 下有 12 个测试崩溃

给 `build/test-release` 正名时，发现它全量重建后仍是 **12/86 失败**，
而 `_recheck_test` 是 **86/86 全绿**。逐项排除后的结论：

| | `_recheck_test` | `build/test-release` |
|---|---|---|
| gtest 来源 | FetchContent（**v1.15.2**，静态） | vcpkg `x64-windows-release`（**v1.16.0**，动态） |
| 结果 | 86/86 通过 | 74/86，12 个失败 |

已排除的非原因：陈旧 `.obj`（全量重建后失败数一模一样）、Qt 平台插件
（两个目录 `bin/platforms/` 都有 `qwindows` / `qoffscreen` / `qminimal`）、
CRT 链接方式（自定义 triplet 是 `VCPKG_CRT_LINKAGE dynamic`，与项目 /MD 一致）、
gtest 链接方式（`GTest::gtest_main` 目标，CMake 会自动带 `GTEST_LINKED_AS_SHARED_LIBRARY`）。

失败集中在异步 / UI 类用例：`QtAnalysisControllerLifecycleTests`(SEGFAULT)、
`ScanTerminalStateTests` 与 `DiagnosticsResultFlowTests`(0xc0000374 堆损坏)、
`PlayerOpenAsyncTests` / `AudioQcPageTests` / `BitrateGopPageTests` / `SubtitleAuxPageTests`(0xc0000409)、
`StreamingManifestScanStatusTests` / `StreamingQcRulesTests` / `ContainerCancelPathTests`。

**这正是 §2.4 说的事：`_recheck_test` 不是"好用"，它只是碰巧停在 gtest 1.15.2 上，
把一个真实缺陷盖住了。** 而 `win-test-release` 是 CI 在用的 preset ——
这个缺陷在 CI 上是否同样复现，需要单独跟进（本机与 runner 的 vcpkg 版本可能不同）。

因此 **`build/_recheck_test` 本轮不删**，它仍是唯一全绿的基线。
下一步建议（不在本轮范围）：把 gtest 版本在 `vcpkg-configuration.json` 里锁到 1.15.2 复测，
若确认是 1.16 的兼容性问题，再决定是锁版本还是修代码。

### 磁盘清理实际结果

已删（约 1.4G）：`build/_f9` `_verify` `_verify_test` `_recheck` `test` `_ide_probe`
`test-cfg` `test-cfg3` `test-cfg4` `generated` `verify-rerun.log`。

保留：`build/release`(758M)、`build/debug`(22M)、`build/test-release`(669M，重建后)、
`build/_recheck_test`(840M，唯一全绿基线)。

`build-ninja/`(324M) **已删**（2026-10-11 确认：`bin/VideoEye.exe` 停在 2026-09-23、零脚本引用、已 gitignore）。
至此 `build/` 从 4.1G 降到 2.3G。

**建议删除（约 2.5G）：**

```
build/_f9            330M   2026-09-26
build/_verify        317M   2026-09-24
build/_verify_test   348M   2026-09-26
build/_recheck       388M   2026-09-27
build/cfg18          5.6M   2026-10-07
build/test           7.2M   2026-10-06
build/yamlcheck      1.8M   2026-10-07
build/_ide_probe     10K    2026-10-11
build/test-cfg       5.0K   2026-09-01
build/test-cfg3      5.0K   2026-09-01
build/test-cfg4      7.0K   2026-09-01
build/generated      1.0K   2026-09-27
build/verify-rerun.log 332K
```

**建议删除，但需你确认（324M）：**

```
build-ninja/         324M   2026-10-09
```

它在 `.gitignore` 里、不被 git 跟踪，是旧布局产物。但最后修改时间是 10-09，
比它被废弃的时间晚 —— 需确认是否还有会话在用，确认后再删。

**明确保留：**

```
build/release        758M   当前主产物
build/debug          22M
build/test-release   613M   注：本机这份未开 tests 特性，跑不了测试
build/_recheck_test  839M   当前唯一能跑测试的验证目录，等 P2-2 给它 preset 后重命名
```
