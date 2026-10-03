cmake_minimum_required(VERSION 3.23)
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

file(GLOB_RECURSE DOMAIN_SOURCES CONFIGURE_DEPENDS "core/domain/*.cpp" "core/domain/*.h")
file(GLOB_RECURSE INFRA_SOURCES CONFIGURE_DEPENDS "infrastructure/*.cpp" "infrastructure/*.h")
file(GLOB_RECURSE MEDIA_SOURCES CONFIGURE_DEPENDS "core/media/*.cpp" "core/media/*.h")
file(GLOB_RECURSE ANALYSIS_SOURCES CONFIGURE_DEPENDS "core/analysis/*.cpp" "core/analysis/*.h")
file(GLOB_RECURSE PLAYBACK_SOURCES CONFIGURE_DEPENDS "core/player/*.cpp" "core/player/*.h")
file(GLOB_RECURSE EXPORTER_SOURCES CONFIGURE_DEPENDS "core/exporter/*.cpp" "core/exporter/*.h")
file(GLOB_RECURSE QC_SOURCES CONFIGURE_DEPENDS "core/qc/*.cpp" "core/qc/*.h")
file(GLOB_RECURSE REPORTING_SOURCES CONFIGURE_DEPENDS "core/reporting/*.cpp" "core/reporting/*.h")
file(GLOB_RECURSE FFMPEGTOOL_SOURCES CONFIGURE_DEPENDS "core/ffmpeg/*.cpp" "core/ffmpeg/*.h")
file(GLOB_RECURSE QTADAPTER_SOURCES CONFIGURE_DEPENDS "core/qt/*.cpp" "core/qt/*.h")
file(GLOB_RECURSE FFMPEG_IO_SOURCES CONFIGURE_DEPENDS "core/ffmpeg_io/*.cpp" "core/ffmpeg_io/*.h")

# ui/*.h 里有 Q_OBJECT，如果同时挂给静态库和主程序，AUTOMOC 会各生成一份
# moc_*.cpp，链接期撞 LNK2005 重复定义 —— 所以 ui 头文件只给主程序自己用。

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
           FFmpeg::avcodec
           FFmpeg::avformat
           FFmpeg::avutil
           FFmpeg::swscale
           FFmpeg::swresample
           Qt6::Core
           Qt6::Gui
    PRIVATE VideoEyeInfrastructure
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
    PRIVATE VideoEyeAnalysis
            VideoEyeExporter
            VideoEyeInfrastructure
            # MediaPlayer 用 core/qt/QtWorkerOwner.h 统一持有导出用的 QThread
            VideoEyeQtAdapters
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

# --- qc: 规则引擎 / 批处理 / 对比 ---
videoeye_add_module(VideoEyeQc ${QC_SOURCES})
target_link_libraries(VideoEyeQc
    PUBLIC VideoEyeDomain
    PRIVATE VideoEyeAnalysis
            VideoEyeInfrastructure
            Qt6::Core
)

# reporting 现在只吃纯结果类型：QC 报告走 QcExportBundle，流统计快照走
# model::StreamStats（已从 StreamAnalyzer.h 下放到 domain）。因此这一层不需要
# FFmpeg 的头文件 —— 上一次它挂 FFmpeg，是因为老的 ReportExporter 签名里带着
# analyzer::StreamStats，而 StreamAnalyzer.h 真的 include 了 libav*。
videoeye_add_module(VideoEyeReporting ${REPORTING_SOURCES})
target_link_libraries(VideoEyeReporting
    PUBLIC VideoEyeDomain
    PRIVATE VideoEyeQc
            VideoEyeInfrastructure
)

# --- ffmpegtools: 原生 ffmpeg 命令行工作台 ---
videoeye_add_module(VideoEyeFfmpegTools ${FFMPEGTOOL_SOURCES})
target_link_libraries(VideoEyeFfmpegTools
    PRIVATE VideoEyeInfrastructure
            Qt6::Core
)

# --- qt adapters: 把不带 QObject 的执行引擎接进 Qt 信号/线程 (见 core/qt/) ---
if(QTADAPTER_SOURCES)
    videoeye_add_module(VideoEyeQtAdapters ${QTADAPTER_SOURCES})
    target_link_libraries(VideoEyeQtAdapters
        PUBLIC VideoEyeDomain
        PRIVATE VideoEyeAnalysis
                VideoEyeInfrastructure
                Qt6::Core
    )
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

add_executable(${PROJECT_NAME}
    ${UI_SOURCES}
    ${UI_HEADERS}
)
# ui/ 只给 GUI 自己用(ui 内部有按子目录写的 include)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
# UI 是唯一需要全部模块的目标，把每一层显式列出来 —— 以前这里只写 VideoEyeCore,
# 于是"界面到底依赖了什么"这条信息是藏在兼容层里的。
target_link_libraries(${PROJECT_NAME} PRIVATE
    VideoEyePlayback
    VideoEyeExporter
    VideoEyeReporting
    VideoEyeQc
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

# Windows 运行时部署要等所有可执行文件都登记完才能组装。
# 但 add_custom_target 必须在所有目标存在之后 —— 放这里是最后一步。
if(WIN32)
    videoeye_finalize_windows_runtime("$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endif()

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
