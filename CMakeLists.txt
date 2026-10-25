cmake_minimum_required(VERSION 3.23)

# ============================================================
# 让 /showIncludes 的探测与解析说同一种语言（必须在 project() 之前）
#
# project() 里 CMake 会跑一次 cl /showIncludes 来**探测**前缀，并把它写进
# rules.ninja 的 `msvc_deps_prefix`；Ninja 之后就用这串去匹配每次编译的输出、
# 记录头文件依赖。探测用的子进程继承本进程环境，所以这里先把 VSLANG 钉成英文：
# 从 VS 中文 UI 启动时它会给 CMake 进程设 VSLANG=2052，cl 打印"注意: 包含文件:"，
# 而 build.bat / 裸 pwsh 构建时 cl 打英文 —— 两边对不上就一条依赖都记不到。
#
# 编译期那一半（`cmake -E env VSLANG=1033` 的 launcher）在下方 MSVC 分支里，
# 两处必须一致，完整成因与症状见那里的长注释。
#
# 用 WIN32 + Ninja 判断而不是 MSVC：project() 之前拿不到编译器 id。这不影响正确性
# —— 本工程在 Windows 上非 MSVC 会直接 FATAL_ERROR（见 project() 之后那段）。
# ============================================================
if(WIN32 AND "${CMAKE_GENERATOR}" MATCHES "Ninja")
    set(ENV{VSLANG} "1033")
endif()

project(VideoEye VERSION 2.0.0 LANGUAGES C CXX)

# Windows 下只能走 MSVC（vcpkg triplet 是 x64-windows）。
# 若 PATH 里混了 MSYS2/MinGW，CMake 会静默选中 c++.exe，之后链接 .lib 时才炸出
# 莫名其妙的错误 —— 这里提前拦住。
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "检测到非 MSVC 编译器: ${CMAKE_CXX_COMPILER}\n"
        "VideoEye 在 Windows 上必须使用 MSVC。\n"
        "请从 \"x64 Native Tools Command Prompt for VS 2022\" 启动，"
        "或使用 cmake --preset release（已内置 cl.exe 路径）。\n"
        "若只是 PATH 里混入了 MSYS2/MinGW，请把它从 PATH 中移除后重新配置。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/permissive- /Zc:__cplusplus /utf-8)
endif()

# ============================================================
# 头文件依赖追踪（Ninja + MSVC 的 /showIncludes 前缀）
#
# 症状：改了一个被几十个 TU 包含的头文件，增量构建却只重编几个文件；
# 跑测试出现 0xC0000374 / 0xC0000409（堆损坏 / 栈缓冲区），重跑一次有时又好了。
#
# 根因：Ninja 的 `deps = msvc` 靠**字符串匹配** cl 打印的
# "Note: including file: <路径>" 来记录依赖，那一串前缀写在 rules.ninja 的
# `msvc_deps_prefix` 里，而它是 configure 时从 cl 的实际输出里探测出来的。
# 一旦 configure 与 build 的 cl **消息语言**不一致（典型场景：在 VS 中文 UI 里
# configure，VS 会给 CMake 进程设 VSLANG=2052，cl 打印"注意: 包含文件:"；
# 之后用 build.bat 或裸 pwsh 构建，cl 又退回英文），前缀就永远匹配不上 ——
# 于是**一条头文件依赖都记不到**，改了头等于没改，链接出来的是新旧结构体混编的
# 二进制。这类问题不会报编译错误，只会表现为随机崩溃。
#
# 修法：两边都钉死成英文，且不依赖调用者的环境。
#   1. 探测侧：文件顶部在 project() 之前 `set(ENV{VSLANG} 1033)`，让 CMake 探测到的
#      前缀就是英文（那一行必须早于 project()，探测在 project() 里跑）；
#   2. 编译侧：编译命令前面加 `cmake -E env VSLANG=1033`，强制 cl 说英文
#      （即使从 VS 中文 UI 构建，父进程的 VSLANG=2052 也会被覆盖）。
#
# 只在 MSVC + Ninja 下生效；Linux/macOS（GCC/Clang + depfile）本来就是精确的。
# 回归验证：`touch core/domain/model/AnalysisResult.h && ninja -n` 应看到几十上百条
# 编译边，而不是个位数。
# ============================================================
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    # 不覆盖用户自己配的 launcher（ccache / sccache 等）：只有空着才填。
    if(NOT CMAKE_C_COMPILER_LAUNCHER)
        set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;VSLANG=1033"
            CACHE STRING "强制 cl 输出英文消息，见 CMakeLists 顶部依赖追踪说明" FORCE)
    endif()
    if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;VSLANG=1033"
            CACHE STRING "强制 cl 输出英文消息，见 CMakeLists 顶部依赖追踪说明" FORCE)
    endif()

    # ---- 守卫：前缀必须是英文，且与上面 launcher 强制出来的语言一致 ----
    #
    # 为什么需要它：探测结果被缓存进 CMakeFiles/<ver>/CMakeCXXCompiler.cmake，
    # 每次 configure 都会把当初探测到的值重新 set 回来。顶部那句 set(ENV{VSLANG})
    # 只对"下一次重新探测"有效，管不到已经在中文 UI 里配过的老树；而生成器在
    # project() 里就把值取走了，之后在 CMakeLists 里改也来不及（实测无效）。
    #
    # 与其让它在半年后表现为"改了头却像没改、测试随机 0xC0000374"，不如当场拦下来：
    # 删掉那份缓存的探测结果，重配一次就恢复。
    string(STRIP "${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}" _ve_showincludes_prefix)
    if(NOT _ve_showincludes_prefix STREQUAL "Note: including file:")
        message(FATAL_ERROR
            "头文件依赖追踪已失效：Ninja 记录的前缀是 \"${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}\"，"
            "而构建时 cl 会打印英文（由 launcher 里的 VSLANG=1033 强制）。\n"
            "两者对不上时一条头文件依赖都记不到 —— 改头文件不会触发重编，"
            "链接出来是不同结构体布局混编的二进制，表现为随机的 0xC0000374 / 0xC0000409。\n"
            "\n"
            "成因：本机曾在 MSVC 中文消息的进程里 configure 过一次（VS 中文 UI 会给 CMake\n"
            "设 VSLANG=2052），探测到的前缀被缓存进了 CMakeFiles/<版本>/CMakeCXXCompiler.cmake。\n"
            "\n"
            "修复（一次性）：删除缓存的编译器探测结果后重新 configure —— \n"
            "  ${CMAKE_BINARY_DIR}/CMakeFiles/${CMAKE_VERSION}/CMakeCXXCompiler.cmake\n"
            "  ${CMAKE_BINARY_DIR}/CMakeFiles/${CMAKE_VERSION}/CMakeCCompiler.cmake\n"
            "或直接删掉整个构建目录重配（CMakeLists 顶部已把 VSLANG 钉成 1033，重配即可自愈）。")
    endif()
endif()

# ============================================================
# Fuzz 入口开关（阶段 3.3）
#
# libFuzzer 运行时只随 clang 分发，且本项目只在 Linux 上跑 fuzz（CI 同款）。
# 打开后整个项目带 ASan/UBSan 插桩：库侧用 fuzzer-no-link（插桩但不接 fuzzer
# main），四个入口在 tests/fuzz/ 各自补 -fsanitize=fuzzer 拿到运行时与 main。
# 打开本开关的构建树**不用于发布**，只用于 fuzz 与 sanitizer 验证。
# ============================================================
option(VIDEOEYE_ENABLE_FUZZERS "构建 libFuzzer 入口（仅 Linux + clang）" OFF)
if(VIDEOEYE_ENABLE_FUZZERS)
    # 不满足条件时直接报错而不是静默跳过：否则会是"看起来开了、其实一个 fuzz
    # 目标都没建"的假绿。守卫必须放在最前，早于任何 target 定义。
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "VIDEOEYE_ENABLE_FUZZERS 只支持 Linux: libFuzzer 运行时不提供 Windows 版。\n"
            "当前平台: ${CMAKE_SYSTEM_NAME}")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "VIDEOEYE_ENABLE_FUZZERS 需要 clang（-fsanitize=fuzzer 是 clang 专属）。\n"
            "当前编译器: ${CMAKE_CXX_COMPILER_ID}")
    endif()
    message(STATUS "Fuzz 入口已启用: 全项目 ASan/UBSan 插桩（fuzzer-no-link）")
    # -fno-sanitize-recover=undefined 不是可选项，是这套门禁成立的前提：
    # UBSan 默认是 recover 模式 —— 报一行 "runtime error: signed integer overflow"
    # 然后**继续跑**，进程照常退出 0。于是 job 全绿、日志里躺着十几条 UB：
    # 既没人会去翻 artifact（压根没生成），也就没人知道解析器在溢出。
    # 加上它之后 UB 直接 abort → libFuzzer 当崩溃处理 → 非 0 退出 + 落复现输入。
    # （ASan 本身就是不可恢复的，不需要单独开关。）
    add_compile_options(
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# ============================================================
# 依赖: 只有 Qt Widgets 与 FFmpeg（预编译）
# ============================================================
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Qt6 REQUIRED COMPONENTS Widgets)
include(cmake/deps/FFmpegPrebuilt.cmake)
include(cmake/deps/FfmpegTool.cmake)
include(cmake/deps/RuntimeDeploy.cmake)

# 原生 ffmpeg 可执行程序: 生成的头文件里带着"配置期探测到的路径 / 是否随包分发",
# 「FFmpeg 命令工作台」页据此给出默认值，用户仍可在设置里覆盖。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/config/FfmpegToolConfig.h.in"
    "${VIDEOEYE_GENERATED_INCLUDE_DIR}/videoeye/FfmpegToolConfig.h"
    @ONLY
)

# ============================================================
# 目标
# ============================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 源文件 (CONFIGURE_DEPENDS: 新增/删除文件自动触发重新配置)
#
# 按模块目录分开 glob —— 一个 glob 对应一个 CMake target，目录归属就是依赖边界。
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS "ui/*.cpp")
file(GLOB UI_HEADERS CONFIGURE_DEPENDS "ui/*.h")
# ui/charts 单独成一个 target（见下面 VideoEyeUiCharts）：
# MetricChartWidget 是十几个分析页共用的曲线底座，也是这些页的测试唯一需要
# 额外编进来的 Q_OBJECT 类。独立成库后，测试链库即可，不必每个目标各编一份。
file(GLOB_RECURSE CHART_SOURCES CONFIGURE_DEPENDS "ui/charts/*.cpp" "ui/charts/*.h")
# ui/theme 单独成一个 target（见下面 VideoEyeUiTheme）：
# AppTheme.cpp 有 600 行 QSS，三个组件测试各自把它编进 exe（连同 VideoEyeUi 一共 4 份）。
# 独立成库后只编一份，需要主题工具的测试链库即可。
file(GLOB_RECURSE THEME_SOURCES CONFIGURE_DEPENDS "ui/theme/*.cpp" "ui/theme/*.h")
# ui/reporting_panel 单独成一个 target（见下面 VideoEyeReportingPanel）。
#
# 它是 ui/ 里带后台线程的页面，单独成库后，只需要这一页的测试可以只链它，
# 不必把整个 UI 库拉进来。
file(GLOB_RECURSE REPORTING_PANEL_SOURCES CONFIGURE_DEPENDS
    "ui/reporting_panel/*.cpp" "ui/reporting_panel/*.h")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/reporting_panel/")
# 曲线控件走 VideoEyeUiCharts，避免同一个 Q_OBJECT 类在两个 target 里各跑一次 AUTOMOC。
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/charts/")
# 主题工具走 VideoEyeUiTheme（Reason: 同一份 AppTheme.cpp 不该在 4 个 target 里各编一次）
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/theme/")
# 分析页的四张记录表 / 宏块控件走 VideoEyeUiAnalysisWidgets（见下面同名 target）：
# 它们只吃 Qt + domain + infrastructure，却被 VideoEyeUi 与 5 个组件测试各自编译，
# 同一份 .cpp（连同 moc）编了 6 遍。独立成库后只编一份，测试链库即可。
#
# 刻意只收这 4 个：FramePacketView / StreamOverviewView 分别依赖 FFmpeg 常量与
# VideoEyeReporting，收进来会让"只想测表格"的目标一并拖上分析/报告层。
file(GLOB_RECURSE ANALYSIS_WIDGET_SOURCES CONFIGURE_DEPENDS
    "ui/analysis_panel/AudioFrameTableWidget.cpp" "ui/analysis_panel/AudioFrameTableWidget.h"
    "ui/analysis_panel/MacroblockView.cpp" "ui/analysis_panel/MacroblockView.h"
    "ui/analysis_panel/PacketTableWidget.cpp" "ui/analysis_panel/PacketTableWidget.h"
    "ui/analysis_panel/VideoFrameTableWidget.cpp" "ui/analysis_panel/VideoFrameTableWidget.h")
list(FILTER UI_SOURCES EXCLUDE
    REGEX "ui/analysis_panel/(AudioFrameTableWidget|MacroblockView|PacketTableWidget|VideoFrameTableWidget)\\.")
# 入口不属于 UI 库：主程序只编译 ui/app/main.cpp，其余 ui/*.cpp 进 VideoEyeUi。
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/app/main\\.cpp$")
# 分析页组件（ui/analysis_panel）、码流页、流媒体页与 AnalysisFacade 单独成一个
# target（见下面 VideoEyeUiAnalysisPages）：它们此前全在 VideoEyeUi 里，导致每个
# 页面组件测试只能手列 .cpp 重编（新增子视图忘了同步就是 LNK2019）。
# 独立成库后测试链库即可，页面源清单只在这一个 glob 里维护。
file(GLOB_RECURSE ANALYSIS_PANEL_SOURCES CONFIGURE_DEPENDS
    "ui/analysis_panel/*.cpp" "ui/analysis_panel/*.h"
    "ui/bitstream_panel/*.cpp" "ui/bitstream_panel/*.h"
    "ui/streaming_panel/*.cpp" "ui/streaming_panel/*.h"
    "ui/AnalysisFacade.cpp" "ui/AnalysisFacade.h")
# 四个记录表 / 宏块控件已在 VideoEyeUiAnalysisWidgets（见上面 ANALYSIS_WIDGET_SOURCES），
# 不要在新库里重复 AUTOMOC。
list(FILTER ANALYSIS_PANEL_SOURCES EXCLUDE
    REGEX "ui/analysis_panel/(AudioFrameTableWidget|MacroblockView|PacketTableWidget|VideoFrameTableWidget)\\.")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/analysis_panel/")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/bitstream_panel/")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/streaming_panel/")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/AnalysisFacade\\.")
# AnalysisFacade.h 带 Q_OBJECT，moc 只能在新库里生成一份。
list(FILTER UI_HEADERS EXCLUDE REGEX "ui/AnalysisFacade\\.h")

file(GLOB_RECURSE DOMAIN_SOURCES CONFIGURE_DEPENDS "core/domain/*.cpp" "core/domain/*.h")
file(GLOB_RECURSE INFRA_SOURCES CONFIGURE_DEPENDS "infrastructure/*.cpp" "infrastructure/*.h")
file(GLOB_RECURSE MEDIA_SOURCES CONFIGURE_DEPENDS "core/media/*.cpp" "core/media/*.h")
file(GLOB_RECURSE ANALYSIS_SOURCES CONFIGURE_DEPENDS "core/analysis/*.cpp" "core/analysis/*.h")
# 码流参数集 / slice / SEI 的解析器单独成一个 target（见下面 VideoEyeCodec）：
# 它们是 analysis 里唯一"只吃 domain + media、不碰 FFmpeg 函数"的一块，而解析器
# 单测（H.264/HEVC/AV1/VVC/VP9/音频位流）目前要各自重编一遍同一份 .cpp。
# 独立成库后测试链库即可，既不必重复编译，也不必把整层 analysis 与 FFmpeg 拖进来。
file(GLOB_RECURSE CODEC_SOURCES CONFIGURE_DEPENDS
    "core/analysis/codec/*.cpp" "core/analysis/codec/*.h")
list(FILTER ANALYSIS_SOURCES EXCLUDE REGEX "core/analysis/codec/")
# 容器 / 流媒体解析器单独成一个 target（见下面 VideoEyeAnalysisPackage）：
# 它们只吃 domain / media / infrastructure，不 include 任何 FFmpeg 或 Qt 头，而对应的
# 单测刻意不链整层 analysis（以免把 FFmpeg 拖进来）—— 以前只能各自手列 .cpp 重编。
# ContainerStructureAnalyzer（分发器，自己 include libavformat）与 FormatDetector
# 仍留在 VideoEyeAnalysis：前者需要 FFmpeg，后者只有一份、不值得为它拆目录。
file(GLOB_RECURSE ANALYSIS_PACKAGE_SOURCES CONFIGURE_DEPENDS
    "core/analysis/container/*.cpp" "core/analysis/container/*.h"
    "core/analysis/streaming/*.cpp" "core/analysis/streaming/*.h")
list(FILTER ANALYSIS_PACKAGE_SOURCES EXCLUDE REGEX "ContainerStructureAnalyzer\\.")
list(FILTER ANALYSIS_SOURCES EXCLUDE REGEX "core/analysis/(container|streaming)/")
# quality 里的三个纯 C++ 分析器（音频 QC / 码率 GOP / 视觉缺陷）单独成一个 target
# （见下面 VideoEyeAnalysisQuality）：它们只吃 domain / infrastructure，一行 libav
# include 都没有，却因同属 VideoEyeAnalysis 被三个单测各自手列 .cpp 重编。
# 拆出去之后测试链库即可，也不必为了一次单测把 FFmpeg 拖进来。
file(GLOB_RECURSE ANALYSIS_QUALITY_SOURCES CONFIGURE_DEPENDS
    "core/analysis/quality/AudioQcAnalyzer.cpp" "core/analysis/quality/AudioQcAnalyzer.h"
    "core/analysis/quality/BitrateGopAnalyzer.cpp" "core/analysis/quality/BitrateGopAnalyzer.h"
    "core/analysis/quality/VisualDefectAnalyzer.cpp" "core/analysis/quality/VisualDefectAnalyzer.h")
list(FILTER ANALYSIS_SOURCES EXCLUDE
    REGEX "core/analysis/quality/(AudioQcAnalyzer|BitrateGopAnalyzer|VisualDefectAnalyzer)\\.")
# 分发器仍在 analysis 层：从上面那次目录级排除里单独取回来。
file(GLOB_RECURSE ANALYSIS_DISPATCH_SOURCES CONFIGURE_DEPENDS
    "core/analysis/container/ContainerStructureAnalyzer.cpp"
    "core/analysis/container/ContainerStructureAnalyzer.h")
list(APPEND ANALYSIS_SOURCES ${ANALYSIS_DISPATCH_SOURCES})
file(GLOB_RECURSE PLAYBACK_SOURCES CONFIGURE_DEPENDS "core/player/*.cpp" "core/player/*.h")
file(GLOB_RECURSE EXPORTER_SOURCES CONFIGURE_DEPENDS "core/exporter/*.cpp" "core/exporter/*.h")
# core/qc 拆成两个 target（见下面 VideoEyeQcRules / VideoEyeQc）：
# 规则表 / 模板 / 报告格式是纯 std:: + domain 的"判定口径"，一行 Qt、FFmpeg 都没有；
# 只有执行器（QcRunner / BatchQcRunner）才需要 AnalysisEngine。拆开之后，
# "只想按规则表评一份 AnalysisResult"的调用方（诊断页、规则单测、报告格式枚举）
# 不必再为了链接执行器把整层 analysis 与 FFmpeg 拖进编译/链接图。
file(GLOB_RECURSE QC_SOURCES CONFIGURE_DEPENDS "core/qc/*.cpp" "core/qc/*.h")
file(GLOB_RECURSE QC_EXECUTOR_SOURCES CONFIGURE_DEPENDS "core/qc/*.cpp" "core/qc/*.h")
list(FILTER QC_SOURCES EXCLUDE REGEX "core/qc/(QcRunner|BatchQcRunner)\\.")
list(FILTER QC_EXECUTOR_SOURCES INCLUDE REGEX "core/qc/(QcRunner|BatchQcRunner)\\.")
file(GLOB_RECURSE REPORTING_SOURCES CONFIGURE_DEPENDS "core/reporting/*.cpp" "core/reporting/*.h")
file(GLOB_RECURSE FFMPEGTOOL_SOURCES CONFIGURE_DEPENDS "core/ffmpeg/*.cpp" "core/ffmpeg/*.h")
file(GLOB_RECURSE QTADAPTER_SOURCES CONFIGURE_DEPENDS "core/qt/*.cpp" "core/qt/*.h")
file(GLOB_RECURSE FFMPEG_IO_SOURCES CONFIGURE_DEPENDS "core/ffmpeg_io/*.cpp" "core/ffmpeg_io/*.h")

# ui/*.h 里有 Q_OBJECT：头文件和源文件都只挂给 VideoEyeUi，同一个类只会生成
# 一份 moc_*.cpp；主程序不再单独编译 UI 源/头，避免 LNK2005 与 ODR 违规。

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Threads REQUIRED)

# ============================================================
# 模块目标
#
# 分层的大前提是**依赖方向单向**（箭头指向被依赖方，反向一律不允许）:
#
#     domain          纯结果/值类型 (无 FFmpeg, 无 analyzer)
#     infrastructure  配置 / 序列化 / 日志 / 线程
#     ffmpeg_io       FFmpeg 阻塞 IO 的中断/超时 (叶子模块: 分析/播放/导出共用)
#     media           容器字节级解析 + 码流参数集解析 (codec / container / probe / streaming)
#     analysis        分析器与编排
#     qc              规则 / 批处理 / 对比
#     reporting       报告导出
#     playback        播放会话 / 解码 / 抽帧
#     exporter        转码导出
#     ffmpegtools     命令行工作台的执行器与字典
#
# 之所以要让每一层都真成为一个 target（而不是只建目录）：目录只是命名习惯，编译器
# 不认识 —— 能被机器检查的只有 target_link_libraries 那条边，缺了它跨层调用会在链接期
# 报 undefined reference。
#
# 但要清楚这条约束**拦不住什么**：所有模块的 include 目录都是仓库根（见下面的
# videoeye_add_module），所以任何模块写 #include "core/xxx.h" 都能编译通过。链接期能
# 拦的只有"没声明这条边的调用"，而主程序 ui 显式链了全部模块 —— 对 ui 层来说约束力为零。
# 依赖**方向**的检查在 scripts/check_layering.py（CI 与 pre-commit 都跑），别指望 CMake。
#
# 由 CMake 能真正约束的一条，是 PUBLIC/PRIVATE 的选择：**公共头**里出现的类型与系统依赖，
# 其所属 target 必须挂在 PUBLIC 上（否则 include 这个头的外部目标拿不到它的 include 目录）。
# 反例见 VideoEyePlayback -> VideoEyeQtAdapters（P2-8）。这条不再"只能靠人读代码守"：
# scripts/audit_link_visibility.py 检查"公共头闭包需要的 Qt / FFmpeg 依赖有没有从 PUBLIC
# 边透出去"（CI / pre-commit / ctest 都跑）。
# ============================================================
function(videoeye_add_module name)
    add_library(${name} STATIC ${ARGN})
    # 所有模块的 include 都写成从仓库根算起（"core/analysis/xxx.h"），所以只需要根目录。
    target_include_directories(${name} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${VIDEOEYE_GENERATED_INCLUDE_DIR}
    )
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
endfunction()

# --- domain: 结果类型 / 值对象，谁都能用，自己不依赖任何人 ---
videoeye_add_module(VideoEyeDomain ${DOMAIN_SOURCES})
# domain 层零 Qt、零 FFmpeg：全部模型字段都是 std:: 容器（vector/map/string/optional），
# 编译期不拉 Qt6::Core，任何上层想只依赖 domain 就能编译（测试、命令行工具等）。
# Qt 类型与 domain 之间的转换统一收在 core/qt/ 与各生产侧的显式 toStdString() / fromStdString()。
# Q_DECLARE_METATYPE 之类的 Qt 反射需求集中在 core/qt/DomainMetatypes.h。

# --- infrastructure: 配置 / JSON / 日志 / 后台任务 ---
videoeye_add_module(VideoEyeInfrastructure ${INFRA_SOURCES})
target_link_libraries(VideoEyeInfrastructure PUBLIC Threads::Threads)

# --- ffmpeg_io: FFmpeg 阻塞 IO 的中断/超时（叶子模块） ---
# 只依赖 FFmpeg 公共头，不 include 任何 core/ 层 —— 因此 analysis / playback /
# exporter 三条链路都能安全地依赖它，不用把"取消能不能及时生效"各写一遍
# （以前它住在 analysis 里，导出层够不着，于是导出的取消在网络输入下完全失效）。
videoeye_add_module(VideoEyeFfmpegIo ${FFMPEG_IO_SOURCES})
target_link_libraries(VideoEyeFfmpegIo
    PUBLIC FFmpeg::avformat
           FFmpeg::avutil
)

# --- media: 容器与码流的字节级解析 ---
videoeye_add_module(VideoEyeMedia ${MEDIA_SOURCES})
target_link_libraries(VideoEyeMedia
    PUBLIC VideoEyeDomain
    PRIVATE VideoEyeInfrastructure
)

# --- codec: 码流参数集 / slice / SEI 解析（analysis 的叶子层）---
# 与 media 的关系: media 解容器字节，codec 解容器里/裸流里的编码层语法。
# 这一层只用 FFmpeg 的 AVCodecID 枚举做分派（头文件级依赖），不调用任何 avcodec
# 函数 —— 所以 FFmpeg 只挂 PRIVATE，链它的测试不会被迫链接 FFmpeg 运行库。
videoeye_add_module(VideoEyeCodec ${CODEC_SOURCES})
target_link_libraries(VideoEyeCodec
    PUBLIC VideoEyeDomain
           VideoEyeMedia
    PRIVATE VideoEyeInfrastructure
            FFmpeg::avcodec
)

# --- analysis-package: 容器 / 流媒体解析器（analysis 的叶子层）---
# 这一层不 include 任何 FFmpeg 或 Qt 头：解析器只读文件字节、产出 domain 结果类型。
# 单独成库的直接收益是"只测解析器"的目标可以链它，而不必为了一次单测把整层 analysis
# 与 FFmpeg 拖进来，也避免同一份 TsStructureAnalyzer.cpp / HlsManifestAnalyzer.cpp
# 在每个测试目标里各编一遍。
videoeye_add_module(VideoEyeAnalysisPackage ${ANALYSIS_PACKAGE_SOURCES})
target_link_libraries(VideoEyeAnalysisPackage
    PUBLIC VideoEyeDomain
           # EbmlAnalyzer.h 的公共接口带着 core/media/detail/ParserLimits.h。
           VideoEyeMedia
    PRIVATE VideoEyeInfrastructure
)

# --- analysis-quality: 三个不碰 FFmpeg 的分析器（analysis 的叶子层）---
# AudioQcAnalyzer / BitrateGopAnalyzer / VisualDefectAnalyzer 只 include domain 结果
# 类型、infrastructure 的 Logger 与 analysis 的 header-only 文本工具，没有任何
# libav 调用。单独成库后，对应单测链库即可（以前各编一份 .cpp），也不会被迫把
# 整层 analysis 与 FFmpeg 拖进测试的链接闭包。
videoeye_add_module(VideoEyeAnalysisQuality ${ANALYSIS_QUALITY_SOURCES})
target_link_libraries(VideoEyeAnalysisQuality
    PUBLIC VideoEyeDomain
    PRIVATE VideoEyeInfrastructure
)

# --- analysis: 各分析器与 orchestration ---
videoeye_add_module(VideoEyeAnalysis ${ANALYSIS_SOURCES})
# FFmpeg 与 Qt 挂 PUBLIC 而不是 PRIVATE: 这一层有多个公共头文件直接 include libav*
# （stream/StreamAnalyzer.h、container/ContainerStructureAnalyzer.h、
#   quality/QualityAnalyzer.h、quality/MacroblockAnalyzer.h），任何 include 它们的人
# 都必须拿到 FFmpeg 的 include 目录，而静态库的 PRIVATE 依赖只传 link、不传目录。
# 同理 VideoEyeMedia 也是 PUBLIC: codec/BitstreamAnalyzer.h 的公开接口带着
# utils::ExtradataFormat（AnalyzeWithFormat），成员也是 utils::NalUnit / ObuUnit，
# 头文件本身就要求调用方能看见 core/media/codec/ExtradataTypes.h。
# 想要降级成 PRIVATE，得先把这些 media 类型从该头文件里挪走（转 domain 或 pimpl）。
target_link_libraries(VideoEyeAnalysis
    PUBLIC VideoEyeDomain
           VideoEyeMedia
           # 分析编排的内部头（orchestration/detail/AnalysisPipeline.h）直接持有 codec 层的解析器
           # ParameterSetTracker / VideoEsInspector / InBandParameterSets），
           # 所以这条边必须是 PUBLIC。
           VideoEyeCodec
           FFmpeg::avcodec
           FFmpeg::avformat
           FFmpeg::avutil
           FFmpeg::swscale
           FFmpeg::swresample
        # 这里已经完全不挂 Qt 了（Gui 更早摘掉，Core 于 analysis 去 Qt 时摘掉）：
        # core/analysis 的公开头文件现在只吐 std::string / std::vector / 结果类型，
        # 一层 Qt 符号都不漏，所以谁链 VideoEyeAnalysis 都不该白拿 Qt 的 include 目录
        # 和编译期宏（UNICODE / _UNICODE 等）。
        #
        # 想要 Qt 的调用方必须自己显式链(Qt6::Core / Qt6::Widgets)—— 别再从 analysis
        # 白拿。这一条现在是 scripts/audit_qt_analysis_border.py 守着的：
        # core/analysis 里再出现任何 Qt 名字就算违规，审计与编译双重拦下。
    PRIVATE VideoEyeInfrastructure
            # 容器 / 流媒体解析器与 FormatDetector 已下沉到叶子库；analysis 的编排
            # 只在 .cpp 里调用它们（公共头不 include），所以这条边保持 PRIVATE。
            VideoEyeAnalysisPackage
            # 三个 FFmpeg-free 分析器（音频 QC / 码率 GOP / 视觉缺陷）
            VideoEyeAnalysisQuality
            # 分析器的打开/探测阶段装 FFmpeg 中断回调（core/ffmpeg_io）
            VideoEyeFfmpegIo
)

# --- playback: 播放会话 / 解码 / 抽帧 / 可视化 ---
videoeye_add_module(VideoEyePlayback ${PLAYBACK_SOURCES})
# MediaPlayer::StartMediaExport 直接用 exporter::MediaExporter，这条边一直存在，
# 只是以前大家都链 VideoEyeCore 所以看不出来。
# 同理：MediaPlayer.h / PlaybackSession.h 里有 AVFormatContext* 与 QImage。
target_link_libraries(VideoEyePlayback
    PUBLIC VideoEyeDomain
           FFmpeg::avcodec
           FFmpeg::avformat
           FFmpeg::avutil
           FFmpeg::swscale
           FFmpeg::swresample
           Qt6::Core
           Qt6::Gui
           # PUBLIC: MediaPlayer.h 里直接持有 ffmpeg_io::AvInterruptState 成员
           # （打开/探测阶段的中断状态，生命周期必须覆盖上下文 —— 见该成员注释）
           VideoEyeFfmpegIo
           # PUBLIC(不是 PRIVATE): VideoEyePlayback 的**公共头**直接 include 了
           # core/qt/QtWorkerOwner.h —— MediaPlayer.h / ExportController.h 都把它当作
           # 成员类型使用。任何 include 这两个头的目标都必须拿到 VideoEyeQtAdapters 的
           # include 目录与编译定义；PRIVATE 只会把这条边藏进静态库内部，外部目标要么
           # 编译不过、要么白拿 Qt 的 include/宏。评审 P2-8 点名的正是这条"声明成
           # PRIVATE 但公共头依赖它"的不一致。
           # （它是**头文件依赖**才必须 PUBLIC；Analysis/Exporter/Infrastructure 只出现在
           #   实现里，保持 PRIVATE，别顺手一起放大出去。）
           VideoEyeQtAdapters
    PRIVATE VideoEyeAnalysis
            VideoEyeExporter
            VideoEyeInfrastructure
            # OpenController.cpp 的 DiagnoseUnopenableFile 直接用 core/media/probe/FileProbe.h。
            # 以前靠 VideoEyeAnalysis 的 PUBLIC 边间接拿到，属于"能编过但没声明"的隐式依赖。
            VideoEyeMedia
)
if(WIN32)
    # WASAPI: CoCreateInstance 在 ole32
    target_link_libraries(VideoEyePlayback PRIVATE ole32)
elseif(APPLE)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    if(AUDIOTOOLBOX_FRAMEWORK)
        target_link_libraries(VideoEyePlayback PRIVATE ${AUDIOTOOLBOX_FRAMEWORK})
    endif()
else()
    # ALSA 走 dlopen，只需 libdl
    target_link_libraries(VideoEyePlayback PRIVATE ${CMAKE_DL_LIBS})
endif()

# --- exporter: 转码 / remux 导出 ---
videoeye_add_module(VideoEyeExporter ${EXPORTER_SOURCES})
target_link_libraries(VideoEyeExporter
    PUBLIC VideoEyeDomain
           FFmpeg::avcodec
           FFmpeg::avformat
           FFmpeg::avutil
           FFmpeg::swscale
           FFmpeg::swresample
           Qt6::Core
    PRIVATE VideoEyeInfrastructure
            # 导出的取消必须能打断 avformat_open_input / av_read_frame（core/ffmpeg_io）
            VideoEyeFfmpegIo
)

# --- qc-rules: 规则引擎 / 默认规则 / 模板映射 / 报告格式 ---
# 这一层只吃 domain 与 infrastructure（模板 JSON），不链分析层：QcRuleEngine 的输入是
# 已经算好的 model::AnalysisResult，它自己不打开任何文件、不碰 FFmpeg。
videoeye_add_module(VideoEyeQcRules ${QC_SOURCES})
target_link_libraries(VideoEyeQcRules
    PUBLIC VideoEyeDomain
    PRIVATE VideoEyeInfrastructure
)

# --- qc: 单文件 / 批量执行器 ---
# core/qc 一层 Qt 符号都没有：QcRunner 直接跑 AnalysisEngine 回调（不依赖事件循环），
# 报告与规则全是 std:: 类型。所以这里不再链 Qt6::Core —— 命令行 / headless / 单测
# 链它就真的不带 Qt 编译环境；需要 Qt 的调用方（UI）自己显式链。
# 回归证据 = 三个只链 VideoEyeQc 的测试目标：test_qc_runner / test_batch_qc_runner /
# test_batch_result_model（后两个还会经 PUBLIC 边拿到 QcRules），它们能编过就证明这条边可以不要。
videoeye_add_module(VideoEyeQc ${QC_EXECUTOR_SOURCES})
target_link_libraries(VideoEyeQc
    PUBLIC VideoEyeQcRules
           VideoEyeDomain
    PRIVATE VideoEyeAnalysis
            VideoEyeInfrastructure
)

# reporting 现在只吃纯结果类型：QC 报告走 QcExportBundle，流统计快照走
# model::StreamStats（已从 StreamAnalyzer.h 下放到 domain）。因此这一层不需要
# FFmpeg 的头文件 —— 上一次它挂 FFmpeg，是因为老的 ReportExporter 签名里带着
# analyzer::StreamStats，而 StreamAnalyzer.h 真的 include 了 libav*。
videoeye_add_module(VideoEyeReporting ${REPORTING_SOURCES})
target_link_libraries(VideoEyeReporting
    PUBLIC VideoEyeDomain
    # 只链规则层：导出器需要 BatchQcRun / 汇总计数，它们在 QcRules 里；
    # 执行器（VideoEyeQc）会经 VideoEyeAnalysis 把 FFmpeg 带进链接闭包。
    PRIVATE VideoEyeQcRules
            VideoEyeInfrastructure
)

# --- ffmpegtools: 原生 ffmpeg 命令行工作台 ---
videoeye_add_module(VideoEyeFfmpegTools ${FFMPEGTOOL_SOURCES})
# Qt6::Core 挂 PUBLIC（2026-10-07 由 audit_link_visibility 定位后修正）: core/ffmpeg 的
# 公共头本身就用 Qt 类型 —— FfmpegToolLocator.h / FfmpegCommandCatalog.h /
# FfmpegCommandParser.h / FfmpegCommandExplainer.h 里是 QString / QStringList，
# FfmpegProcessRunner.h 还有 QObject / QProcess。挂 PRIVATE 只把依赖藏在库内部，
# 外部目标 include 这些头时拿不到 Qt 的 include 目录与编译定义。
target_link_libraries(VideoEyeFfmpegTools
    PUBLIC Qt6::Core
    PRIVATE VideoEyeInfrastructure
)

# --- qt adapters: 把不带 QObject 的执行引擎接进 Qt 信号/线程 (见 core/qt/) ---
if(QTADAPTER_SOURCES)
    videoeye_add_module(VideoEyeQtAdapters ${QTADAPTER_SOURCES})
    # Qt6::Core 挂 PUBLIC: core/qt 的公共头就是 Qt 类型（QtWorkerOwner.h 的 QObject /
    # QThread、QtAnalysisController.h 的 QObject / QString、DomainMetatypes.h 的
    # QMetaType），PRIVATE 会把这条边藏进静态库内部 —— P2-8 时外部目标
    # （VideoEyePlayback）include 这些头拿不到 Qt 的 include 目录，与"公共头依赖必须
    # 从 PUBLIC 边透出"是同一条规则（scripts/audit_link_visibility.py 守）。
    target_link_libraries(VideoEyeQtAdapters
        PUBLIC VideoEyeDomain
               Qt6::Core
        PRIVATE VideoEyeAnalysis
                VideoEyeInfrastructure
    )
endif()

# --- reporting panel: 「报告与批量 QC」页 ---
# ui/ 其余部分现在统一进 VideoEyeUi；这一页仍保持独立 target，让只测它的目标
# 可以少链一整层 UI。它本身带后台线程，独立成库也便于单独验证线程语义。
if(REPORTING_PANEL_SOURCES)
    videoeye_add_module(VideoEyeReportingPanel ${REPORTING_PANEL_SOURCES})
    # Qt6::Widgets 挂 PUBLIC: 这个库就是为了"被主程序与测试各自 include
    # ReportingPanel.h"而拆出来的，而 ReportingPanel.h 本身就是 QWidget 系
    # （QCheckBox / QTableWidget 等 14 个 Qt 头）。挂 PRIVATE 的话，链它的目标
    # 编译这一页时直接找不到 Qt 的 include 目录。
    # VideoEyeQc 挂 PUBLIC: ReportingPanel.h 的公共接口直接暴露 qc::QcProfile /
    # qc::QcRunResult（成员与私有方法签名），include 这个头的目标必须能看见 core/qc
    # 的类型。挂在 PRIVATE 只是靠静态库的 link 传播"看起来能编"，与"公共头依赖必须
    # 从 PUBLIC 边透出"的规则不一致（audit_link_visibility.py 只校验 Qt / FFmpeg
    # 这类系统依赖，模块间的这条边不归它管）。
    target_link_libraries(VideoEyeReportingPanel
        PUBLIC Qt6::Widgets
               VideoEyeQc
        PRIVATE VideoEyeReporting
                VideoEyeInfrastructure
    )
endif()

# --- ui/charts: 曲线控件（分析页与测试共用）---
# 十几个测试目标此前各自把 MetricChartWidget.cpp 编进 exe —— 同一个 Q_OBJECT 类
# 在 16 个 target 里各跑一次 AUTOMOC（实现与 moc 都被重复编译）。独立成静态库后
# 只编一份，测试链库即可，链接期也只取用得到的那一份 moc。
# Qt6::Widgets 挂 PUBLIC: 公共头就是 QWidget 系。
if(CHART_SOURCES)
    videoeye_add_module(VideoEyeUiCharts ${CHART_SOURCES})
    target_link_libraries(VideoEyeUiCharts PUBLIC Qt6::Widgets)
endif()

# --- ui/theme: 主题工具（调色板 / 字体 / QSS / formatTime）---
# AppTheme.cpp 是 600 行 QSS，三个组件测试此前各自把它编进 exe。独立成库后只编一份，
# 测试链库即可；它本身不依赖任何 core 层。
# Qt6::Gui 挂 PUBLIC: AppTheme.h 的公共接口就是 QColor / QFont / QString；
# Qt6::Widgets 只需 PRIVATE（applyDarkTheme() 里用 QApplication）。
if(THEME_SOURCES)
    videoeye_add_module(VideoEyeUiTheme ${THEME_SOURCES})
    target_link_libraries(VideoEyeUiTheme
        PUBLIC Qt6::Gui
        PRIVATE Qt6::Widgets
    )
endif()

# --- ui/analysis_panel 记录表 / 宏块控件（VideoEyeUi 与组件测试共用）---
# 这 4 个控件此前由 VideoEyeUi 与 5 个测试目标各自编一份 .cpp + moc。独立成库后
# 只编一份：VideoEyeUi 与测试都链它，AUTOMOC 也只在它这里跑。
# Qt6::Widgets 挂 PUBLIC: 公共头就是 QWidget 系；domain / infrastructure 也是
# 公共头依赖（PacketTableWidget.h -> PacketInfo.h，RecordTableModel.h -> PerfMetrics.h）。
videoeye_add_module(VideoEyeUiAnalysisWidgets ${ANALYSIS_WIDGET_SOURCES})
target_link_libraries(VideoEyeUiAnalysisWidgets
    PUBLIC VideoEyeDomain
           VideoEyeInfrastructure
           Qt6::Widgets
    # 两个表格的 .cpp 只取 AV_PICTURE_TYPE_* / AV_PKT_FLAG_* / AVMEDIA_TYPE_* 这类
    # 编译期常量、不调用 avcodec 函数，所以挂 PRIVATE 就够了；链它的测试不必
    # 自己再声明 FFmpeg 依赖（以前正是为此在测试里手写 include 目录注释）。
    # AnalysisPageSupport.h（图表批量提交）只在 .cpp 里用 —— 同样 PRIVATE。
    PRIVATE VideoEyeUiCharts
            FFmpeg::avcodec
)

# --- ui/analysis pages: 分析面板各页 / 码流页 / 流媒体页（与 VideoEye 主库分开）---
# 这一层是"页面组件 + 建页工厂 + AnalysisFacade"，不含主窗口与播放器。单独成库
# 之后，页面组件测试链库即可，不必再手列 .cpp；新增一个子视图也不会因为测试
# CMake 没同步而在链接期报 LNK2019（ARCHITECTURE §5.3 记录的历史坑）。
#
# PUBLIC 边只放**公共头直接需要**的模块：页面头 include Qt Widgets、domain 结果
# 类型与图表控件（MetricChartWidget）。工厂头已经改成前向声明各页类型，页面头也
# 不再 include ReportingPanel.h / 记录表控件头 —— 这两块只在 .cpp 里用，挂 PRIVATE
# 即可（PRIVATE 依赖仍以 LINK_ONLY 形式传播链接，测试链本库不会缺符号）。
if(ANALYSIS_PANEL_SOURCES)
    videoeye_add_module(VideoEyeUiAnalysisPages ${ANALYSIS_PANEL_SOURCES})
    # ui 内部子目录 include（"analysis_panel/..."）与 VideoEyeUi 保持同一套。
    target_include_directories(VideoEyeUiAnalysisPages PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/ui
    )
    target_link_libraries(VideoEyeUiAnalysisPages
        PUBLIC Qt6::Widgets
               VideoEyeDomain
               VideoEyeUiCharts
        PRIVATE VideoEyeReportingPanel
                VideoEyeUiAnalysisWidgets
                VideoEyeAnalysis
                VideoEyeQc
                VideoEyeQcRules
                VideoEyeReporting
                VideoEyeMedia
                VideoEyeInfrastructure
                VideoEyeUiTheme
                # FramePacketView.cpp 只取 AV_PICTURE_TYPE_* 等编译期常量
                FFmpeg::avcodec
    )
    # AnalysisFacade.cpp 用 QtAnalysisController（core/qt）。
    if(QTADAPTER_SOURCES)
        target_link_libraries(VideoEyeUiAnalysisPages PRIVATE VideoEyeQtAdapters)
    endif()
endif()

# --- ui: 主程序界面层 ---
# 所有 ui/*.cpp（入口除外）编成一个静态库，主程序只保留 ui/app/main.cpp。
# 好处: UI 组件测试链同一份实现，不必再把整层源码编进测试 exe；同一个 Q_OBJECT
# 类只在一个 target 里跑 AUTOMOC，ODR / LNK2005 从结构上消失。
if(UI_SOURCES)
    videoeye_add_module(VideoEyeUi ${UI_SOURCES} ${UI_HEADERS})
    # ui 内部有按子目录写的 include（"main_window/MainWindow.h" 等），测试也要
    # include 这些头，所以 ui/ 挂 PUBLIC。
    target_include_directories(VideoEyeUi PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/ui
    )
    # Qt6::Widgets 挂 PUBLIC: 公共头就是 QWidget 系。
    # FFmpeg 挂 PRIVATE: UI 公共头只出现 player::MediaPlayer*（MediaPlayerFwd.h），
    # 不再 include MediaPlayer.h；需要 FFmpeg 的只是 .cpp 的编译，PRIVATE 已足够。
    # core 模块挂 PRIVATE: UI 库不对外提供它们的 API；主程序与测试各自显式链
    # 需要的模块，避免把 VideoEyeUi 变成第二个"全部 PUBLIC"的聚合层。
    target_link_libraries(VideoEyeUi
        PUBLIC Qt6::Widgets
        PRIVATE FFmpeg::avcodec
                FFmpeg::avformat
                FFmpeg::avutil
                FFmpeg::swscale
                FFmpeg::swresample
                VideoEyePlayback
                VideoEyeExporter
                VideoEyeReporting
                VideoEyeQc
                VideoEyeUiCharts
                VideoEyeUiTheme
                VideoEyeUiAnalysisWidgets
                VideoEyeUiAnalysisPages
                VideoEyeReportingPanel
                VideoEyeFfmpegTools
                VideoEyeAnalysis
                VideoEyeMedia
                VideoEyeDomain
                VideoEyeInfrastructure
    )
    if(QTADAPTER_SOURCES)
        target_link_libraries(VideoEyeUi PRIVATE VideoEyeQtAdapters)
    endif()
endif()

# ============================================================
# 迁移期兼容层已删除
#
# VideoEyeCore 曾经是唯一的静态库(core/ + utils/ 塞在一起), 分层之后退化成一个
# "把十层全部 PUBLIC 出去"的 INTERFACE target。它让每个调用方都能编译, 代价是
# 依赖关系重新变得不可见 —— 上次正是因为它, Playback -> Exporter 这条边一直没暴露。
# 现在主程序与全部测试都显式链自己需要的模块, 这个 target 也就没有存在意义了。
# 如果哪天确实需要"全部模块", 请显式列出来, 别再引入聚合层。
# ============================================================

# 主题资源（复选框勾选图标等）：QSS 画不出勾选符号，用 qrc 把 PNG 编进可执行文件。
# 只给主程序 —— 测试不应用深色主题，链接时少一份资源无关紧要。
qt6_add_resources(VIDEOEYE_THEME_RESOURCES ui/theme/theme.qrc)

add_executable(${PROJECT_NAME}
    ui/app/main.cpp
    ${VIDEOEYE_THEME_RESOURCES}
)
# 入口只负责 QApplication + MainWindow，界面实现全部来自 VideoEyeUi。
# 模块依赖仍显式列出（VideoEyeUi 内部走 PRIVATE 边），让"界面到底依赖什么"保持可见。
target_link_libraries(${PROJECT_NAME} PRIVATE
    VideoEyeUi
    VideoEyePlayback
    VideoEyeExporter
    VideoEyeReporting
    VideoEyeQc
    VideoEyeReportingPanel
    VideoEyeFfmpegTools
    VideoEyeAnalysis
    VideoEyeMedia
    VideoEyeDomain
    VideoEyeInfrastructure
    Qt6::Widgets
)
if(QTADAPTER_SOURCES)
    target_link_libraries(${PROJECT_NAME} PRIVATE VideoEyeQtAdapters)
endif()

# FFmpeg 运行时部署 (Windows 拷 DLL / Linux、macOS 拷 .so、.dylib 并设 rpath)
videoeye_deploy_ffmpeg(${PROJECT_NAME})
# 原生 ffmpeg 可执行程序 (Windows 随包分发时拷到可执行文件旁)
videoeye_deploy_ffmpeg_tool(${PROJECT_NAME})

# ============================================================
# Windows 运行时部署 (Qt)
# ============================================================
if(WIN32)
    # Windows 没有 rpath，DLL 只能靠"与可执行文件同目录"。
    # 具体的部署步骤在 add_subdirectory(tests) 之后由
    # videoeye_finalize_windows_runtime() 统一组装 —— 这里只登记目标。
    videoeye_register_runtime_target(${PROJECT_NAME})

    # Qt 插件 (platforms/qwindows.dll 等) 只有主程序需要, 单元测试不跑 Qt 事件循环。
    # 搜索路径要覆盖 Qt 的三种常见布局: 官方安装包(Tools/Qt/bin)、vcpkg(<root>/tools/Qt6/bin)、
    # Linux/macOS(<root>/libexec 或 lib/qt6/libexec)。
    set(_windeployqt_hints "")
    if(Qt6_DIR)
        # <root>/lib/cmake/Qt6 -> <root>
        get_filename_component(_qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
        list(APPEND _windeployqt_hints
            "${_qt_root}/tools/Qt6/bin" "${_qt_root}/bin"
            "${_qt_root}/libexec" "${_qt_root}/lib/qt6/libexec")
    endif()
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _windeployqt_hints
            "${_prefix}/tools/Qt6/bin" "${_prefix}/bin" "${_prefix}/libexec")
    endforeach()
    if(DEFINED ENV{QTDIR})
        list(APPEND _windeployqt_hints "$ENV{QTDIR}/bin")
    endif()

    find_program(WINDEPLOYQT windeployqt HINTS ${_windeployqt_hints})
    if(WINDEPLOYQT)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${WINDEPLOYQT}
                --no-opengl-sw --no-translations --no-system-d3d-compiler --no-compiler-runtime
                --dir "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Deploy Qt plugins"
            VERBATIM
        )
    else()
        message(STATUS "未找到 windeployqt，跳过 Qt 插件部署（仅影响打包分发，不影响构建）")
    endif()

    # offscreen 平台插件补一份。
    #
    # windeployqt 只带"当前桌面需要的那个平台插件"（Windows 上就是 qwindows），于是
    # 无显示器环境（CI、远程会话、干净的构建机）里跑 QApplication 会在平台插件加载阶段
    # 直接 qFatal 崩掉，报错还看不出跟测试有什么关系。offscreen 是 Qt 官方的无头平台
    # 插件，显式补上之后 UI 单元测试用 QT_QPA_PLATFORM=offscreen 就能稳定跑。
    set(_qt_extra_platform_plugins "")
    list(APPEND _qt_plugin_root_candidates
        "${_qt_root}/plugins" "${_qt_root}/Qt6/plugins"
        "${Qt6_DIR}/../../plugins" "${Qt6_DIR}/../../Qt6/plugins")
    # 从 Qt6::Core 的导入库/库文件位置反推 kit 根：不同发行布局层级不同
    # （官方安装包 <Qt>/<ver>/<kit>/plugins；vcpkg <installed>/<triplet>/Qt6/plugins），
    # 硬算层级迟早会踩到另一种布局。
    if(TARGET Qt6::Core)
        get_target_property(_qt_core_loc Qt6::Core IMPORTED_IMPLIB_RELEASE)
        if(NOT _qt_core_loc)
            get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION_RELEASE)
        endif()
        if(_qt_core_loc)
            get_filename_component(_qt_lib_dir "${_qt_core_loc}" DIRECTORY)
            get_filename_component(_qt_kit_root "${_qt_lib_dir}" DIRECTORY)
            list(PREPEND _qt_plugin_root_candidates "${_qt_kit_root}/plugins")
        endif()
    endif()
    foreach(_plugin_root ${_qt_plugin_root_candidates})
        foreach(_platform_plugin qoffscreen qminimal)
            if(EXISTS "${_plugin_root}/platforms/${_platform_plugin}.dll")
                list(APPEND _qt_extra_platform_plugins "${_plugin_root}/platforms/${_platform_plugin}.dll")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _qt_extra_platform_plugins)
    if(_qt_extra_platform_plugins)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_qt_extra_platform_plugins}
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/platforms"
            COMMENT "Deploy extra Qt platform plugins (offscreen/minimal)"
            VERBATIM
        )
    else()
        message(STATUS "未找到 offscreen/minimal 平台插件，UI 测试将使用默认平台")
    endif()
endif()

# ============================================================
# 安装
# ============================================================
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
# FFmpeg 动态库: Windows 扁平布局进 bin/, Linux/macOS 进 lib/ (由 INSTALL_RPATH 指过去)
videoeye_install_ffmpeg()
# 原生 ffmpeg 可执行程序: 只装 Windows 随包的那份; Linux/macOS 用系统 ffmpeg, 不塞进安装树
videoeye_install_ffmpeg_tool()

# 第三方许可证文本与声明。
# MIT 只覆盖 VideoEye 自己的源码，不覆盖随包分发的 FFmpeg / Qt6 二进制 —— 安装树里
# 必须有这两家的许可证全文与一份 THIRD_PARTY_NOTICES，否则发出去的包不合规。
include(cmake/deps/ThirdPartyNotices.cmake)
videoeye_install_third_party_notices()

if(WIN32)
    # 安装树同样要跑一遍依赖闭包 —— 打包出去的东西比 build 目录更容易漏文件,
    # 而且漏了之后只有目标机器上才会暴露。
    videoeye_windows_runtime_search_dirs(_install_search_dirs)

    # 安装期要扫的可执行文件（GUI，以及 Windows 下随包分发的 ffmpeg.exe）
    #
    # 注意 \${CMAKE_INSTALL_PREFIX}: 必须让它在安装时才求值。写成 ${CMAKE_INSTALL_PREFIX}
    # 的话, 这里烤进去的是配置期的默认值(C:/Program Files (x86)/VideoEye), 于是
    # `cmake --install build/release --prefix <别的目录>` 会去扫一个并不存在的路径,
    # 报 "输入文件不存在" —— 安装到默认前缀时恰好对得上, 所以这个坑一直没暴露。
    set(_install_inputs "\${CMAKE_INSTALL_PREFIX}/bin/$<TARGET_FILE_NAME:${PROJECT_NAME}>")
    # 随包分发的 ffmpeg.exe 也要扫: 它可能不是我们预编译包里的那份(用户显式指定),
    # 那样它依赖的 DLL 未必在 FFMPEG_WINDOWS_DLLS 里。
    if(VIDEOEYE_FFMPEG_TOOL_BUNDLE AND VIDEOEYE_FFMPEG_TOOL_NAME)
        string(APPEND _install_inputs
            ";\${CMAKE_INSTALL_PREFIX}/bin/${VIDEOEYE_FFMPEG_TOOL_NAME}")
    endif()

    # Qt 插件这一步必须在配置期就决定好。
    # install(CODE) 里的脚本是安装时才执行的，那时 WINDEPLOYQT 这个变量并不存在，
    # 写成 if(WINDEPLOYQT) 会被静默判成 false —— 结果就是装出来的程序
    # 起不来（缺 platforms/qwindows.dll），而 install 本身还报成功。
    set(_install_qt_plugins_step "")
    if(WINDEPLOYQT)
        set(_install_qt_plugins_step "
        execute_process(
            COMMAND \"${WINDEPLOYQT}\"
                    --no-opengl-sw --no-translations --no-system-d3d-compiler --no-compiler-runtime
                    --dir \"\${CMAKE_INSTALL_PREFIX}/bin\"
                    \"\${CMAKE_INSTALL_PREFIX}/bin/$<TARGET_FILE_NAME:${PROJECT_NAME}>\"
            RESULT_VARIABLE _res2
            OUTPUT_VARIABLE _out2
            ERROR_VARIABLE _err2)
        message(STATUS \"\${_out2}\")
        if(NOT _res2 EQUAL 0)
            message(FATAL_ERROR \"Qt 插件部署失败: \${_err2}\")
        endif()
        ")
    endif()

    install(CODE "
        execute_process(
            COMMAND \"\${CMAKE_COMMAND}\"
                    \"-DVE_INPUTS=${_install_inputs}\"
                    \"-DVE_DEST=\${CMAKE_INSTALL_PREFIX}/bin\"
                    \"-DVE_SEARCH_DIRS=${_install_search_dirs}\"
                    -P \"${VIDEOEYE_RUNTIME_DEPS_SCAN_SCRIPT}\"
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
        message(STATUS \"\${_out}\")
        if(NOT _res EQUAL 0)
            message(FATAL_ERROR \"运行时依赖部署失败: \${_err}\")
        endif()
        ${_install_qt_plugins_step}
    ")
endif()

# ============================================================
# 测试
# ============================================================
option(BUILD_TESTING "Build unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================
# fuzz 入口（阶段 3.3）
#
# 独立于 BUILD_TESTING：libFuzzer 不需要 GTest，也不该被"跑不跑单测"这一个
# 开关绑住。源码在 tests/fuzz/，只链接分析/媒体层（无 Qt，headless 可跑）；
# 平台守卫与 ASan/UBSan 插桩见文件开头的 VIDEOEYE_ENABLE_FUZZERS 块。
# ============================================================
if(VIDEOEYE_ENABLE_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

# Windows 运行时部署要等所有可执行文件都登记完才能组装。
# 但 add_custom_target 必须在所有目标存在之后 —— 放这里是最后一步。
if(WIN32)
    videoeye_finalize_windows_runtime("$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endif()

# ============================================================
# 打包（CPack）
# ============================================================
# 一个前提：打包的输入必须是 install 出来的树，不能是 build/ 目录。
# build/ 里的可执行文件在 Windows 上靠同目录的 DLL、在 Linux 上靠 rpath 指到
# build/../lib 才能跑起来；只有 install 规则铺出来的那棵树才是"换台机器也能跑"的
# （Qt 插件、依赖闭包、lib/ 都在这一步完成）。所以 release.yml 一律先 install 再 cpack。
#
# 具体用哪个生成器由 CI 决定（cpack -G ...），这里只给出默认值与平台无关的那些
# 字段 —— 默认给的是"一定能成"的归档格式，安装器（NSIS / DEB）需要额外工具，
# 由 workflow 检测工具存在后再追加。
set(CPACK_PACKAGE_NAME "VideoEye")
set(CPACK_PACKAGE_VENDOR "VideoEye contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "视频码流分析工具：参数集解析 / 容器结构 / QC 体检 / 逐帧导出")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "VideoEye")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/HZer0K/VideoEye")
set(CPACK_VERBATIM_VARIABLES YES)
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_THREADS 0)

# 文件名里的平台段要规范: 不直接拿 CMAKE_SYSTEM_PROCESSOR —— 它在 MSVC 下是
# 大写 AMD64、在 M1 上是 arm64，拼进文件名既难看又不一致。
set(VIDEOEYE_PKG_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
string(TOLOWER "${VIDEOEYE_PKG_ARCH}" VIDEOEYE_PKG_ARCH)
if(VIDEOEYE_PKG_ARCH MATCHES "^(amd64|x86_64)$")
    set(VIDEOEYE_PKG_ARCH "x86_64")
elseif(VIDEOEYE_PKG_ARCH MATCHES "^(arm64|aarch64)$")
    set(VIDEOEYE_PKG_ARCH "arm64")
endif()
if(WIN32)
    set(VIDEOEYE_PKG_OS "windows")
elseif(APPLE)
    set(VIDEOEYE_PKG_OS "macos")
else()
    set(VIDEOEYE_PKG_OS "linux")
endif()
set(VIDEOEYE_PKG_PLATFORM "${VIDEOEYE_PKG_OS}-${VIDEOEYE_PKG_ARCH}")
set(CPACK_PACKAGE_FILE_NAME "VideoEye-${PROJECT_VERSION}-${VIDEOEYE_PKG_PLATFORM}")

# NSIS 只接受 .txt / .rtf 的许可证文件，仓库里的 LICENSE 没有扩展名 ——
# 拷一份带扩展名的出来，避免换生成器时才炸。
configure_file("${CMAKE_SOURCE_DIR}/LICENSE"
               "${CMAKE_BINARY_DIR}/VideoEye-LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/VideoEye-LICENSE.txt")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")

if(WIN32)
    # 默认只出 ZIP（免安装版），NSIS 需要 makensis，由 release.yml 检测后追加
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
    set(CPACK_NSIS_HELP_LINK "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_HOMEPAGE_URL}/issues")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_CREATE_DESKTOP_LINKS "VideoEye")
    set(CPACK_PACKAGE_EXECUTABLES "VideoEye;VideoEye")
elseif(APPLE)
    # macOS 目前产出的是可执行程序 + lib/ 的目录树，不是 .app bundle
    # （主程序没有设 MACOSX_BUNDLE），所以这里只出归档，不发 .dmg。
    set(CPACK_GENERATOR "TGZ")
else()
    set(CPACK_GENERATOR "TGZ")
    # 依赖交给 dpkg-shlibdeps 自动算：写死 libavcodec60 这种名字在 Ubuntu 22.04/24.04
    # 上就是错的（那里是 58 / 60 之外的版本号），而自动推导永远跟着实际链接走。
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
endif()

include(CPack)

# ============================================================
# 摘要
# ============================================================
message(STATUS "")
message(STATUS "VideoEye ${PROJECT_VERSION}  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt6:    ${Qt6_VERSION} (Widgets only)")
if(FFMPEG_SOURCE STREQUAL "pkg-config")
    message(STATUS "  FFmpeg: system ${FFMPEG_VERSION} (pkg-config)")
else()
    message(STATUS "  FFmpeg: prebuilt ${FFMPEG_LOCAL_DIR}")
endif()
message(STATUS "  Bundle: VIDEOEYE_BUNDLE_FFMPEG=${VIDEOEYE_BUNDLE_FFMPEG}")
message(STATUS "")
