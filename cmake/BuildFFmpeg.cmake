# BuildFFmpeg.cmake - 从源码编译 FFmpeg 并集成到项目中
#
# 使用 ExternalProject_Add 在构建时自动编译 FFmpeg 共享库
# 仅构建项目需要的 5 个库：libavcodec, libavformat, libavutil, libswscale, libswresample
#
# 导出变量:
#   FFMPEG_SOURCE_INCLUDE_DIR  - FFmpeg 头文件目录
#   FFMPEG_SOURCE_LIB_DIR      - FFmpeg 共享库目录
#   FFMPEG_LIBRARIES           - FFmpeg 库文件列表

include(ExternalProject)

# FFmpeg 源码目录
set(FFMPEG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FFmpeg")

# 检查源码是否存在
if(NOT EXISTS "${FFMPEG_SOURCE_DIR}/configure")
    message(FATAL_ERROR
        "FFmpeg 源码未找到: ${FFMPEG_SOURCE_DIR}\n"
        "请先执行: git clone --depth 1 --branch n8.1 https://git.ffmpeg.org/ffmpeg.git third_party/FFmpeg"
    )
endif()

# FFmpeg 安装前缀（构建目录内）
set(FFMPEG_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg_install")
set(FFMPEG_SOURCE_INCLUDE_DIR "${FFMPEG_INSTALL_PREFIX}/include")
set(FFMPEG_SOURCE_LIB_DIR "${FFMPEG_INSTALL_PREFIX}/lib")

# 构建标记文件，用于判断是否需要重新构建
set(FFMPEG_BUILD_STAMP "${CMAKE_BINARY_DIR}/ffmpeg_install_stamp/ffmpeg-built")

# 检测 Vulkan 头文件版本是否满足 FFmpeg 要求 (>= 1.3.277)
set(FFMPEG_ENABLE_VULKAN OFF)
set(VULKAN_CFLAGS "")

# 优先检查 bundled Vulkan-Headers
set(_VK_HEADER_FILE "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include/vulkan/vulkan_core.h")
    set(_VK_HEADER_FILE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include/vulkan/vulkan_core.h")
    set(VULKAN_CFLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include")
elseif(Vulkan_FOUND AND EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h")
    set(_VK_HEADER_FILE "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h")
endif()

if(_VK_HEADER_FILE)
    file(STRINGS "${_VK_HEADER_FILE}" _vk_ver_str
         REGEX "^#define VK_HEADER_VERSION ")
    if(_vk_ver_str)
        string(REGEX MATCH "[0-9]+" _vk_ver "${_vk_ver_str}")
        if(_vk_ver GREATER_EQUAL 277)
            set(FFMPEG_ENABLE_VULKAN ON)
            message(STATUS "Vulkan header version ${_vk_ver} >= 277, enabling Vulkan in FFmpeg")
        else()
            message(STATUS "Vulkan header version ${_vk_ver} < 277, disabling Vulkan in FFmpeg")
        endif()
    endif()
endif()

if(NOT FFMPEG_ENABLE_VULKAN)
    # Fallback: check pkg-config version
    pkg_check_modules(VULKAN_PC vulkan)
    if(VULKAN_PC_FOUND AND VULKAN_PC_VERSION VERSION_GREATER_EQUAL "1.3.277")
        set(FFMPEG_ENABLE_VULKAN ON)
        message(STATUS "pkg-config vulkan ${VULKAN_PC_VERSION} >= 1.3.277, enabling")
    endif()
endif()

# 构建 FFmpeg 的 configure 参数
set(FFMPEG_CONFIGURE_ARGS
    --prefix=${FFMPEG_INSTALL_PREFIX}
    --enable-shared
    --disable-static
    --disable-doc
    --disable-programs
    --disable-avdevice
    --disable-avfilter
    --disable-network
    --disable-debug
    --disable-encoders
    --disable-muxers
    --disable-demuxers
    --enable-demuxer=mov,mp4,m4a
    --enable-demuxer=matroska,webm
    --enable-demuxer=avi
    --enable-demuxer=flv
    --enable-demuxer=mpegts
    --enable-demuxer=mpegps
    --enable-demuxer=h264,hevc,av1
    --enable-demuxer=aac,mp3,flac,opus,vorbis
    --enable-demuxer=wav,ogg
    --enable-demuxer=rawvideo,pcm_*
    --enable-demuxer=image2
    --enable-demuxer=rtsp,http,hls
    --disable-parsers
    --enable-parser=h264,hevc,av1,aac,mpegaudio,flac,opus,vorbis
    --enable-parser=mpegvideo,mpeg4video,vp8,vp9
    --enable-parser=png,jpeg2000,bmp
    --disable-decoders
    --enable-decoder=h264,hevc,av1,vp8,vp9
    --enable-decoder=mpeg1video,mpeg2video,mpeg4,msmpeg4v3
    --enable-decoder=aac,mp3,flac,opus,vorbis,ac3,eac3
    --enable-decoder=pcm_*,adpcm_*
    --enable-decoder=png,jpeg2000,mjpeg
    --enable-decoder=rawvideo
    --enable-decoder=webp
    --enable-hwaccels
    --disable-bsfs
    --disable-filters
    --disable-protocols
    --enable-protocol=file,pipe,http,https,rtsp,rtp,tcp,udp
    --enable-protocol=hls,crypto,data
    --disable-indevs
    --disable-outdevs
)

# 条件启用 Vulkan
if(FFMPEG_ENABLE_VULKAN)
    list(APPEND FFMPEG_CONFIGURE_ARGS
        --enable-hwaccel=vulkan
        --enable-vulkan
    )
    # 如果使用 bundled Vulkan headers，传递 include 路径
    if(VULKAN_CFLAGS)
        list(APPEND FFMPEG_CONFIGURE_ARGS
            "--extra-cflags=${VULKAN_CFLAGS}"
        )
    endif()
    # 链接 libvulkan
    list(APPEND FFMPEG_CONFIGURE_ARGS
        --extra-ldflags=-lvulkan
    )
endif()

# 将参数列表转换为空格分隔的字符串
string(REPLACE ";" " " FFMPEG_CONFIGURE_STRING "${FFMPEG_CONFIGURE_ARGS}")

# 使用 ExternalProject 构建 FFmpeg
# 设置 TMPDIR 到构建目录内，避免沙箱环境无法写入 /tmp
set(FFMPEG_TMPDIR "${CMAKE_BINARY_DIR}/ffmpeg_tmp")
file(MAKE_DIRECTORY "${FFMPEG_TMPDIR}")

ExternalProject_Add(ffmpeg_build
    SOURCE_DIR "${FFMPEG_SOURCE_DIR}"
    CONFIGURE_COMMAND bash -c "cd <SOURCE_DIR> && TMPDIR=${FFMPEG_TMPDIR} ./configure ${FFMPEG_CONFIGURE_STRING}"
    BUILD_COMMAND bash -c "cd <SOURCE_DIR> && TMPDIR=${FFMPEG_TMPDIR} make -j8"
    INSTALL_COMMAND bash -c "cd <SOURCE_DIR> && TMPDIR=${FFMPEG_TMPDIR} make install"
    BUILD_IN_SOURCE 1
    BUILD_BYPRODUCTS
        "${FFMPEG_SOURCE_LIB_DIR}/libavcodec.so"
        "${FFMPEG_SOURCE_LIB_DIR}/libavformat.so"
        "${FFMPEG_SOURCE_LIB_DIR}/libavutil.so"
        "${FFMPEG_SOURCE_LIB_DIR}/libswscale.so"
        "${FFMPEG_SOURCE_LIB_DIR}/libswresample.so"
    INSTALL_DIR "${FFMPEG_INSTALL_PREFIX}"
)

# 创建 IMPORTED 目标以便链接
# 由于 ExternalProject 在构建时执行，我们需要用 IMPORTED 库来处理

add_library(ffmpeg::avcodec SHARED IMPORTED GLOBAL)
add_library(ffmpeg::avformat SHARED IMPORTED GLOBAL)
add_library(ffmpeg::avutil SHARED IMPORTED GLOBAL)
add_library(ffmpeg::swscale SHARED IMPORTED GLOBAL)
add_library(ffmpeg::swresample SHARED IMPORTED GLOBAL)

# 设置导入库位置
set_target_properties(ffmpeg::avcodec PROPERTIES
    IMPORTED_LOCATION "${FFMPEG_SOURCE_LIB_DIR}/libavcodec.so"
    IMPORTED_NO_SONAME ON
)
set_target_properties(ffmpeg::avformat PROPERTIES
    IMPORTED_LOCATION "${FFMPEG_SOURCE_LIB_DIR}/libavformat.so"
    IMPORTED_NO_SONAME ON
)
set_target_properties(ffmpeg::avutil PROPERTIES
    IMPORTED_LOCATION "${FFMPEG_SOURCE_LIB_DIR}/libavutil.so"
    IMPORTED_NO_SONAME ON
)
set_target_properties(ffmpeg::swscale PROPERTIES
    IMPORTED_LOCATION "${FFMPEG_SOURCE_LIB_DIR}/libswscale.so"
    IMPORTED_NO_SONAME ON
)
set_target_properties(ffmpeg::swresample PROPERTIES
    IMPORTED_LOCATION "${FFMPEG_SOURCE_LIB_DIR}/libswresample.so"
    IMPORTED_NO_SONAME ON
)

# 设置库文件列表供外部使用
set(FFMPEG_LIBRARIES
    "${FFMPEG_SOURCE_LIB_DIR}/libavcodec.so"
    "${FFMPEG_SOURCE_LIB_DIR}/libavformat.so"
    "${FFMPEG_SOURCE_LIB_DIR}/libavutil.so"
    "${FFMPEG_SOURCE_LIB_DIR}/libswscale.so"
    "${FFMPEG_SOURCE_LIB_DIR}/libswresample.so"
)

# 查找 FFmpeg 依赖的系统库
find_library(ZLIB_LIBRARY NAMES z)
find_library(PTHREAD_LIBRARY NAMES pthread)
find_library(DL_LIBRARY NAMES dl)
find_library(MATH_LIBRARY NAMES m)

set(FFMPEG_SYSTEM_LIBS "")
if(ZLIB_LIBRARY)
    list(APPEND FFMPEG_SYSTEM_LIBS ${ZLIB_LIBRARY})
endif()
if(PTHREAD_LIBRARY)
    list(APPEND FFMPEG_SYSTEM_LIBS ${PTHREAD_LIBRARY})
endif()
if(DL_LIBRARY)
    list(APPEND FFMPEG_SYSTEM_LIBS ${DL_LIBRARY})
endif()
if(MATH_LIBRARY)
    list(APPEND FFMPEG_SYSTEM_LIBS ${MATH_LIBRARY})
endif()

message(STATUS "FFmpeg 源码编译配置:")
message(STATUS "  源码目录: ${FFMPEG_SOURCE_DIR}")
message(STATUS "  安装前缀: ${FFMPEG_INSTALL_PREFIX}")
message(STATUS "  头文件目录: ${FFMPEG_SOURCE_INCLUDE_DIR}")
message(STATUS "  库文件目录: ${FFMPEG_SOURCE_LIB_DIR}")
