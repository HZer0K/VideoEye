# ============================================================
# FindFFMPEG.cmake - VideoEye 自带 FFmpeg 查找模块 (多来源 fallback)
# 注意: 文件名必须为 FindFFMPEG.cmake (find_package(FFMPEG) 大小写敏感匹配,
#       Linux 上 FindFFmpeg.cmake 无法被找到)
#
# 查找优先级:
#   1. FFMPEG_ROOT 缓存变量指定的前缀目录 (含 include/lib/bin)
#   2. ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-prebuilt
#      (由 scripts/fetch-ffmpeg.ps1 下载的 gyan.dev full-shared 预编译包)
#   3. 旧布局 build*/ffmpeg_install (兼容早期源码构建)
#   4. pkg-config (Linux 系统包 libavcodec-dev 等)
#
# 输出变量:
#   FFMPEG_FOUND          - 是否找到
#   FFMPEG_INCLUDE_DIRS   - 头文件目录
#   FFMPEG_LIBRARIES      - 链接库名 (avcodec avformat avutil swscale swresample)
#   FFMPEG_LIB_DIR        - 导入库目录 (local build 时)
#   FFMPEG_LOCAL_DIR      - 本地预编译前缀 (local build 时)
#   FFMPEG_LOCAL_BUILD    - 是否使用本地预编译/源码构建
#   FFMPEG_BIN_DIR        - 运行时 DLL 目录 (local build 时)
# ============================================================

# 缓存变量: 用户可通过 -DFFMPEG_ROOT=<path> 指定
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg 预编译安装前缀 (含 include/lib/bin)")

# ---- 1/2/3. 本地前缀目录查找 ----
set(_ff_candidates "")
if(FFMPEG_ROOT)
    list(APPEND _ff_candidates "${FFMPEG_ROOT}")
endif()
list(APPEND _ff_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-prebuilt"
    "${CMAKE_BINARY_DIR}/ffmpeg_install"
    "${CMAKE_CURRENT_SOURCE_DIR}/build-ninja/ffmpeg_install"
    "${CMAKE_CURRENT_SOURCE_DIR}/build-Release/ffmpeg_install"
    "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg_install"
)

set(FFMPEG_LOCAL_DIR "")
foreach(_dir IN LISTS _ff_candidates)
    # 平台校验: Windows 需 .lib 导入库, Linux 需 .so (避免误用另一平台的预编译包)
    if(WIN32)
        set(_ff_lib_marker "${_dir}/lib/avcodec.lib")
    else()
        set(_ff_lib_marker "${_dir}/lib/libavcodec.so")
    endif()
    if(EXISTS "${_dir}/include/libavcodec/avcodec.h" AND EXISTS "${_ff_lib_marker}")
        set(FFMPEG_LOCAL_DIR "${_dir}")
        break()
    endif()
endforeach()

if(FFMPEG_LOCAL_DIR)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_LOCAL_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_LOCAL_DIR}/lib")
    set(FFMPEG_BIN_DIR "${FFMPEG_LOCAL_DIR}/bin")
    set(FFMPEG_LIBRARIES avcodec avformat avutil swscale swresample)
    set(FFMPEG_LOCAL_BUILD ON)
    set(FFMPEG_FOUND TRUE)
    if(NOT FFMPEG_FIND_QUIETLY)
        message(STATUS "FFmpeg: using local build at ${FFMPEG_LOCAL_DIR}")
    endif()
    return()
endif()

# ---- 4. pkg-config (Linux 系统包) ----
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil libswscale libswresample)
    if(FFMPEG_FOUND)
        set(FFMPEG_LOCAL_BUILD OFF)
        # 将 pkg-config 的 -L 目录透传给链接器 (否则 -lavcodec 可能落到系统默认路径的旧版库)
        set(FFMPEG_LIB_DIR "${FFMPEG_LIBRARY_DIRS}")
        if(NOT FFMPEG_FIND_QUIETLY)
            message(STATUS "FFmpeg: using system pkg-config (libdir: ${FFMPEG_LIB_DIR})")
        endif()
        return()
    endif()
endif()

# ---- 未找到 ----
set(FFMPEG_FOUND FALSE)
set(FFMPEG_LOCAL_BUILD OFF)
