# VideoEye 工程配置审查报告

> 审查范围：构建编译、依赖引入、团队协作基础设施、跨平台一致性、代码质量工具
> 审查时间：2026-07-25
> 结论：单机可用，**团队开发就绪度不足**，需在依赖可复现性、构建路径解耦、CI 三方面重点整改

## 一、健康度评分（满分 10）

| 维度 | 当前 | 目标 | 差距说明 |
|------|------|------|----------|
| 依赖管理 | 4 | 8 | vcpkg.json 无版本锁定；FFmpeg 手工下载无校验；依赖来源混杂 |
| 构建可复现性 | 4 | 8 | build_ninja.ps1 硬编码本机绝对路径；无 CMakePresets |
| 团队协作基础设施 | 2 | 8 | 无 CI/CD、无 CMakePresets、无 CONTRIBUTING、无 pre-commit |
| 代码质量工具 | 5 | 8 | 有 .clang-format；缺 .clang-tidy / .editorconfig / 提交前检查 |
| 跨平台一致性 | 5 | 8 | 两套脚本分叉，cmake 参数不一致（EXPORT_COMPILE_COMMANDS 等） |
| 文档完善度 | 6 | 8 | 有 README/QUICKSTART，但缺依赖获取脚本化与一键构建说明 |

---

## 二、问题清单（按优先级分级）

### P0 — 阻碍团队上手，必须立即修

#### P0-1. vcpkg.json 无版本锁定，依赖不可复现

**现状**：`vcpkg.json` 只有 6 个裸包名，无 `builtin-baseline`、无版本约束、无 features：

```json
{
  "dependencies": ["opencv4","qtbase","qtmultimedia","qtcharts","sdl2","zlib"]
}
```

**风险**：不同成员的 vcpkg 版本不同 → 装到不同依赖版本 → 构建行为/ABI 不一致，难复现 bug。

**建议**：
- 新增 `vcpkg-configuration.json` 锁定 baseline 版本：
  ```json
  {
    "default-registry": { "kind": "git", "baseline": "2024-xx-xx", "repository": "microsoft/vcpkg" },
    "registries": []
  }
  ```
- vcpkg.json 给关键包加 `version>=` 和 `features`（如 opencv4 按需选 dnn/imgproc 等）。

#### P0-2. build_ninja.ps1 硬编码本机绝对路径

**现状**：
```powershell
$msvcRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.43.34808"
$winKits  = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer   = "10.0.22621.0"
$ninjaExe = "C:\Program Files\...\Ninja\ninja.exe"
```

**风险**：换台机器 / 不同 VS 版本 / Build Tools（非 Community）安装 → 直接失败，团队无法复现。

**建议**：用 `vswhere` 自动探测 VS 安装路径，或直接调 `vcvarsall.bat` 注入环境：
```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot  = & $vswhere -latest -property installationPath
& "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"   # 注入完整 MSVC 环境
```
或更彻底——迁移到 **CMakePresets.json + vcpkg toolchain**，让 CMake 自己拉起编译器，脚本只 `cmake --preset win-release`。

#### P0-3. FFmpeg 依赖获取无脚本化、无 checksum、CMake 硬编码构建目录

**现状**：
- Windows 靠手动从 gyan.dev 下 7z 解压到 `ffmpeg_install/`，无下载脚本、无 SHA256 校验。
- CMakeLists 用硬编码的构建目录名查找 FFmpeg：
  ```cmake
  set(FFMPEG_LOCAL_DIR "${CMAKE_BINARY_DIR}/ffmpeg_install")          # build-xxx/ffmpeg_install
  # ... build-ninja / build-Release / build 三个固定名回退
  ```

**风险**：新成员完全不知道怎么获取 FFmpeg；构建目录名变了就找不到；gyan.dev 更新后版本漂移无感知。

**建议**：
1. 写 `scripts/fetch-ffmpeg.ps1`（Windows）/ `scripts/fetch-ffmpeg.sh`（Linux 可选）：自动下载 + 校验 SHA256 + 解压。
2. CMake 用**可配置变量**替代硬编码路径：
   ```cmake
   set(FFMPEG_ROOT "${FFMPEG_ROOT}" CACHE PATH "FFmpeg install prefix")
   if(EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
       set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
       set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")
   endif()
   ```
3. 文档化"FFmpeg 为外部预编译依赖"的获取步骤。

#### P0-4. 根目录散落编译产物 *.obj

**现状**：源码根目录存在 4 个 obj 文件：
```
FrameAnalyzer.obj  (183 KB)
Logger.obj          (213 KB)
scene_test.obj      (37 KB)
SceneChangeAnalyzer.obj (44 KB)
```

**风险**：虽被 `.gitignore` 忽略，但污染工作区；`scene_test.obj` 暗示有测试代码未纳入 tests/ 目录管理。说明曾有手工 `cl /c` 调试残留，构建输出目录配置可能不正确。

**建议**：删除这 4 个 obj；确认 CMake 的 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 等不会让中间产物落到源码根；把 scene_test 纳入 tests/ 正式管理。

---

### P1 — 影响团队效率，应尽快修

#### P1-5. 无 CMakePresets.json，脚本参数分叉

**现状**：build_ninja.ps1 / build.sh / setup.sh 各自硬编码 cmake 参数，已经出现不一致：
- `build.sh` 开 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`，`build_ninja.ps1` 不开。
- vcpkg prefix path 传递方式各脚本不同。

**建议**：新增 `CMakePresets.json` 统一配置，脚本退化为薄封装：
```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "win-release",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build-ninja",
      "toolchainFile": "${sourceDir}/vcpkg_installed/x64-windows/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "BUILD_TESTING": "OFF" }
    },
    {
      "name": "linux-release",
      "generator": "Unix Makefiles",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "CMAKE_EXPORT_COMPILE_COMMANDS": "ON" }
    }
  ],
  "buildPresets": [
    { "name": "win-release", "configurePreset": "win-release" },
    { "name": "linux-release", "configurePreset": "linux-release" }
  ]
}
```

#### P1-6. 无 CI/CD，PR 无法自动验证

**现状**：无 `.github/workflows/`，跨平台构建通过与否完全靠手工，无人保证 main 分支随时可构建。

**建议**：加 `.github/workflows/build.yml`，Windows + Linux matrix，每个 PR / push 自动构建：
```yaml
strategy:
  matrix:
    os: [windows-latest, ubuntu-latest]
steps:
  - uses: actions/checkout@v4
    with: { submodules: recursive }
  - name: vcpkg cache / apt install
  - run: cmake --preset ${{ matrix.os == 'windows' && 'win' || 'linux' }}-release
  - run: cmake --build --preset ${{ ... }}
```

#### P1-7. 构建脚本职责重叠混乱

**现状**：4 个脚本职责交叉——`build.bat`（分发到 ninja/vs）、`build_ninja.ps1`（ninja）、`build.sh`（linux 编译）、`setup.sh`（linux 初始化+编译）。缺统一入口和新成员指引。

**建议**：统一到 CMakePresets + 一个薄封装；README 顶部给"各平台一行构建命令"。

#### P1-8. INSTALL_RPATH 指向构建目录，install 规则不完整

**现状**：
```cmake
set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "${FFMPEG_LOCAL_DIR}/bin"
    INSTALL_RPATH "${FFMPEG_LOCAL_DIR}/bin"   # 构建目录，install 后无效
)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)   # 只装 exe，没装依赖 DLL
```

**风险**：`cmake --install` 产物无法直接运行（缺 DLL / RPATH 失效）。

**建议**：
- Linux 用 `INSTALL_RPATH "$ORIGIN"`。
- Windows 用 `windeployqt` + `install(FILES ...)` 复制 FFmpeg/vcpkg DLL 到安装目录。

#### P1-9. vcpkg.json 未声明 FFmpeg，依赖来源混杂

**现状**：FFmpeg 不在 vcpkg.json（用预编译包），实际依赖来自 4 个渠道：vcpkg（Win）、apt（Linux）、源码子模块（MediaInfo/Bento4）、预编译包（FFmpeg）。团队心智成本高。

**建议**：二选一——
- 方案A：vcpkg 装 ffmpeg（8.1.2 有 vulkan feature，可统一管理），减少渠道。
- 方案B：文档明确"FFmpeg 为外部预编译依赖"并脚本化获取（见 P0-3）。

---

### P2 — 工程加固，锦上添花

#### P2-10. file(GLOB_RECURSE) 收集源文件

**现状**：CMakeLists 用 `file(GLOB_RECURSE CORE_SOURCES "core/*.cpp")` 等。CMake 官方不推荐 GLOB——新增/删除源文件不会自动触发重新配置。

**建议**：加 `CONFIGURE_DEPENDS`（`file(GLOB_RECURSE CONFIGURE_DEPENDS ...)`）或显式列出源文件。

#### P2-11. 缺 .clang-tidy / .editorconfig / pre-commit

**现状**：只有 `.clang-format`，无静态检查、无编辑器统一配置、无提交前格式保障。

**建议**：加 `.clang-tidy`（启用 modernize-/bugprone-/readability- 等）、`.editorconfig`、git pre-commit hook 跑 clang-format。

#### P2-12. tests/ 近乎空置

**现状**：`BUILD_TESTING` 默认 OFF，`tests/CMakeLists.txt`（5.8 KB）有框架但无实际测试。

**建议**：补关键模块单测（FrameAnalyzer、MacroblockStats、SceneChangeAnalyzer），CI 中开启 BUILD_TESTING。

#### P2-13. OpenCV 组件可能冗余

**现状**：`find_package(OpenCV REQUIRED COMPONENTS core imgproc objdetect)` 要求 objdetect，需确认是否真用到（项目主要是视频分析，未必需要目标检测）。

**建议**：按实际使用裁剪，减少依赖编译/体积。

#### P2-14. 缺 CONTRIBUTING.md 与依赖获取文档

**建议**：补 CONTRIBUTING.md，含：各平台一键构建命令、FFmpeg 获取脚本用法、vcpkg 初始化、代码风格约定、提交流程。

---

## 三、改进路线图（建议执行顺序）

| 阶段 | 任务 | 预期收益 |
|------|------|----------|
| 第1步 | 删根目录 *.obj；加 `vcpkg-configuration.json` 锁版本 | 立即消除污染 + 依赖可复现 |
| 第2步 | 写 `scripts/fetch-ffmpeg.ps1`；CMake 改用 `FFMPEG_ROOT` 变量 | FFmpeg 获取自动化、可校验 |
| 第3步 | 加 `CMakePresets.json`；build_ninja.ps1 改用 vswhere 自动探测 MSVC | 路径解耦，换机器可跑 |
| 第4步 | 加 `.github/workflows/build.yml` 双平台 CI | main 随时可构建 |
| 第5步 | 补 .clang-tidy / .editorconfig / pre-commit / CONTRIBUTING | 代码质量与协作规范 |

## 四、亮点（值得保留）

- `.gitignore` 规则完整合理，构建产物/IDE/Qt 生成文件/vcpkg_installed 均已忽略。
- 4 个 git submodule（vulkan-headers/Bento4/ZenLib/MediaInfoLib）管理规范，有 `.gitmodules`。
- 跨平台编译器选项按 MSVC 条件正确分流。
- FFmpeg 多来源查找策略（vcpkg → 本地产物 → pkg-config）容错性好。
- Shader 编译（SPIR-V）跨平台路径查找完善，缺失时优雅降级。
- 有 `.clang-format`（LLVM 风格，120 列，4 空格），代码风格基线已建立。
