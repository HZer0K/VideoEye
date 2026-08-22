# FindFFMPEG.cmake
# 优先级: 1) FFMPEG_ROOT  2) third_party/ffmpeg-prebuilt  3) pkg-config
# 输出变量: FFMPEG_FOUND, FFMPEG_INCLUDE_DIRS, FFMPEG_LIBRARIES,
#           FFMPEG_LIB_DIR, FFMPEG_LOCAL_DIR, FFMPEG_LOCAL_BUILD, FFMPEG_BIN_DIR

set(FFMPEG_ROOT "" CACHE PATH "FFmpeg prefix (include/lib/bin)")

set(_ff_candidates "")
if(FFMPEG_ROOT)
    list(APPEND _ff_candidates "${FFMPEG_ROOT}")
endif()
list(APPEND _ff_candidates "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-prebuilt")

set(FFMPEG_LOCAL_DIR "")
foreach(_dir IN LISTS _ff_candidates)
    set(_ff_lib "${_dir}/lib/libavcodec.so")
    if(WIN32)
        set(_ff_lib "${_dir}/lib/avcodec.lib")
    endif()
    if(EXISTS "${_dir}/include/libavcodec/avcodec.h" AND EXISTS "${_ff_lib}")
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
    return()
endif()

if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil libswscale libswresample)
    if(FFMPEG_FOUND)
        set(FFMPEG_LOCAL_BUILD OFF)
        set(FFMPEG_LIB_DIR "${FFMPEG_LIBRARY_DIRS}")
        return()
    endif()
endif()

set(FFMPEG_FOUND FALSE)
set(FFMPEG_LOCAL_BUILD OFF)
