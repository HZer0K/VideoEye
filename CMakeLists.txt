cmake_minimum_required(VERSION 3.20)
project(VideoEye VERSION 2.0.0 LANGUAGES CXX)

# 设置C++标准为C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置默认构建类型为Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译器选项
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# 添加自定义 CMake 模块路径
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 查找依赖包 - 使用 pkg-config
find_package(PkgConfig REQUIRED)

# FFmpeg（使用 pkg-config，保证“编译期头文件/链接库/运行期加载库”一致）
pkg_check_modules(FFmpeg REQUIRED
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)

# 某些环境（例如手工安装到 /usr/local）可能只有 *.so.<major> 而缺少 libxxx.so 的开发软链，
# 这会导致链接阶段错误地回退到系统库版本，最终产生“头文件版本 != 运行库版本”的崩溃。
# 这里在 pkg-config 给出的库目录里强制解析出实际存在的 lib*.so* 文件并用绝对路径链接。
function(_videoeye_pick_ffmpeg_lib out_var lib_base)
    unset(_picked CACHE)
    find_library(_picked NAMES ${lib_base} PATHS ${FFmpeg_LIBRARY_DIRS} NO_DEFAULT_PATH)
    if (NOT _picked)
        file(GLOB _candidates "${FFmpeg_LIBRARY_DIRS}/lib${lib_base}.so.*")
        list(SORT _candidates)
        list(LENGTH _candidates _len)
        if (_len GREATER 0)
            math(EXPR _idx "${_len} - 1")
            list(GET _candidates ${_idx} _picked)
        endif()
    endif()
    if (NOT _picked)
        message(FATAL_ERROR "FFmpeg library 'lib${lib_base}' not found in: ${FFmpeg_LIBRARY_DIRS}")
    endif()
    set(${out_var} "${_picked}" PARENT_SCOPE)
endfunction()

_videoeye_pick_ffmpeg_lib(FFMPEG_AVCODEC_PATH avcodec)
_videoeye_pick_ffmpeg_lib(FFMPEG_AVFORMAT_PATH avformat)
_videoeye_pick_ffmpeg_lib(FFMPEG_AVUTIL_PATH avutil)
_videoeye_pick_ffmpeg_lib(FFMPEG_SWSCALE_PATH swscale)
_videoeye_pick_ffmpeg_lib(FFMPEG_SWRESAMPLE_PATH swresample)

set(FFmpeg_LIBRARIES
    ${FFMPEG_AVCODEC_PATH}
    ${FFMPEG_AVFORMAT_PATH}
    ${FFMPEG_AVUTIL_PATH}
    ${FFMPEG_SWSCALE_PATH}
    ${FFMPEG_SWRESAMPLE_PATH}
)

# OpenCV
find_package(OpenCV REQUIRED COMPONENTS core imgproc objdetect)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Charts)

# SDL2
pkg_check_modules(SDL2 REQUIRED sdl2)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 收集源文件
file(GLOB_RECURSE CORE_SOURCES "core/*.cpp")
file(GLOB_RECURSE UI_SOURCES "ui/*.cpp")
file(GLOB_RECURSE UTILS_SOURCES "utils/*.cpp")

# 收集头文件
file(GLOB_RECURSE HEADERS "core/*.h" "ui/*.h" "utils/*.h")

# Qt MOC处理
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 创建可执行文件
add_executable(${PROJECT_NAME}
    ${CORE_SOURCES}
    ${UI_SOURCES}
    ${UTILS_SOURCES}
    ${HEADERS}
)

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${FFmpeg_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Charts
    ${FFmpeg_LIBRARIES}
    ${OpenCV_LIBS}
    ${SDL2_LIBRARIES}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "${FFmpeg_LIBRARY_DIRS}"
    INSTALL_RPATH "${FFmpeg_LIBRARY_DIRS}"
    INSTALL_RPATH_USE_LINK_PATH ON
)

# 编译定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ${Qt6_DEFINITIONS}
)

# 安装规则
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# 复制资源文件
install(DIRECTORY resources/
    DESTINATION share/VideoEye/resources
)

# 打印配置信息
message(STATUS "VideoEye Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  FFmpeg Version: ${FFmpeg_VERSION}")
message(STATUS "  FFmpeg Include: ${FFmpeg_INCLUDE_DIRS}")
message(STATUS "  FFmpeg Libs: ${FFmpeg_LIBRARIES}")
message(STATUS "  OpenCV Version: ${OpenCV_VERSION}")
message(STATUS "  Qt Version: ${Qt6_VERSION}")
