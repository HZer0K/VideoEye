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
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "core/*.h" "ui/*.h" "utils/*.h")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(${PROJECT_NAME}
    ${CORE_SOURCES}
    ${UI_SOURCES}
    ${UTILS_SOURCES}
    ${HEADERS}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
)

# 音频输出用的是平台原生 API，各平台补一下系统库
if(WIN32)
    # WASAPI: CoCreateInstance 在 ole32
    target_link_libraries(${PROJECT_NAME} PRIVATE ole32)
elseif(APPLE)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    if(AUDIOTOOLBOX_FRAMEWORK)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${AUDIOTOOLBOX_FRAMEWORK})
    endif()
else()
    # ALSA 走 dlopen，只需 libdl
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

# FFmpeg 运行时部署（Windows 拷 DLL / Linux 设 rpath）
videoeye_deploy_ffmpeg(${PROJECT_NAME})

# ============================================================
# Windows 运行时部署 (Qt)
# ============================================================
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copy runtime DLLs (linked targets)"
    )

    find_program(WINDEPLOYQT windeployqt HINTS
        "${CMAKE_PREFIX_PATH}/tools/Qt6/bin" "${CMAKE_PREFIX_PATH}/Qt6/bin" "$ENV{QTDIR}/bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${WINDEPLOYQT}
                --no-opengl-sw --no-translations --no-system-d3d-compiler --no-compiler-runtime
                --dir "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Deploy Qt plugins"
        )
    endif()
endif()

# ============================================================
# 安装
# ============================================================
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
if(WIN32 AND EXISTS "${FFMPEG_RUNTIME_DIR}")
    install(DIRECTORY "${FFMPEG_RUNTIME_DIR}/" DESTINATION bin FILES_MATCHING PATTERN "*.dll")
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
# 摘要
# ============================================================
message(STATUS "")
message(STATUS "VideoEye ${PROJECT_VERSION}  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt6:    ${Qt6_VERSION} (Widgets only)")
message(STATUS "  FFmpeg: ${FFMPEG_PREBUILT_DIR}")
message(STATUS "")
