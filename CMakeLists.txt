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
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "core/*.cpp")
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS "ui/*.cpp")
file(GLOB_RECURSE UTILS_SOURCES CONFIGURE_DEPENDS "utils/*.cpp")
# 头文件按归属分开挂: ui/*.h 里有 Q_OBJECT，如果同时挂给静态库和主程序，
# AUTOMOC 会各生成一份 moc_*.cpp，链接期撞 LNK2005 重复定义。
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "core/*.h" "utils/*.h")
file(GLOB_RECURSE UI_HEADERS CONFIGURE_DEPENDS "ui/*.h")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ============================================================
# 核心静态库: core/ + utils/
#
# 把业务逻辑与 UI 分开, 只编一次就能同时喂给 GUI 主程序与单元测试
# (tests/ 直接 target_link_libraries(xxx PRIVATE VideoEyeCore), 不必像早期那样
#  手工列一串源文件 —— 每加一个 analyzer 就可能 LNK2019)。
# ============================================================
add_library(VideoEyeCore STATIC
    ${CORE_SOURCES}
    ${UTILS_SOURCES}
    ${HEADERS}
)
set_target_properties(VideoEyeCore PROPERTIES OUTPUT_NAME videoeye_core AUTOMOC ON)
# 注意这里没有 ui/: core/ 与 utils/ 不 include 任何 ui 头文件, 把它挂成 PUBLIC
# 等于告诉每个使用者(含 tests)"你可以 include ui 下的东西" —— 而那些头文件
# 带 Q_OBJECT, 一旦有人真的 include 了, 就会踩到 AUTOMOC 重复生成 moc_*.cpp 的坑。
# ui/*.cpp 用的是 "ui/xxx/yyy.h" 这种从源码根开始的写法, 根目录已经在搜索路径里。
target_include_directories(VideoEyeCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${VIDEOEYE_GENERATED_INCLUDE_DIR}
)
target_link_libraries(VideoEyeCore PUBLIC
    Qt6::Core
    Qt6::Gui
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
)

# 音频输出用的是平台原生 API，各平台补一下系统库
if(WIN32)
    # WASAPI: CoCreateInstance 在 ole32
    target_link_libraries(VideoEyeCore PUBLIC ole32)
elseif(APPLE)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    if(AUDIOTOOLBOX_FRAMEWORK)
        target_link_libraries(VideoEyeCore PUBLIC ${AUDIOTOOLBOX_FRAMEWORK})
    endif()
else()
    # ALSA 走 dlopen，只需 libdl
    target_link_libraries(VideoEyeCore PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(${PROJECT_NAME}
    ${UI_SOURCES}
    ${UI_HEADERS}
)
# ui/ 只给 GUI 自己用(ui 内部有按子目录写的 include)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    VideoEyeCore
    Qt6::Widgets
)

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
