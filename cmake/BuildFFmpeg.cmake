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
    if(VULKAN_CFLAGS)
        list(APPEND FFMPEG_CONFIGURE_ARGS
            "--extra-cflags=${VULKAN_CFLAGS}"
        )
    endif()
    # Vulkan 导入库：Windows 上 MinGW 的 ld 无法直接使用 MSVC 的 .lib，
    # 需要用 gendef/dlltool 从 vulkan-1.dll 生成 .a 格式导入库
    # Linux 上直接使用系统 libvulkan.so，不需要此步骤
    if(WIN32)
        set(VULKAN_IMPORT_DIR "${CMAKE_BINARY_DIR}/ffmpeg_tmp/vulkan_import")
        file(MAKE_DIRECTORY "${VULKAN_IMPORT_DIR}")
    endif()
endif()

# 将参数列表转换为空格分隔的字符串
string(REPLACE ";" " " FFMPEG_CONFIGURE_STRING "${FFMPEG_CONFIGURE_ARGS}")

# 平台相关: 库文件前缀和后缀
if(WIN32 AND MSVC)
    set(FFMPEG_LIB_SUFFIX ".lib")
    set(FFMPEG_LIB_PREFIX "")
else()
    set(FFMPEG_LIB_SUFFIX ".so")
    set(FFMPEG_LIB_PREFIX "lib")
endif()

# 使用 ExternalProject 构建 FFmpeg
# 设置 TMPDIR 到构建目录内，避免沙箱环境无法写入 /tmp
set(FFMPEG_TMPDIR "${CMAKE_BINARY_DIR}/ffmpeg_tmp")
file(MAKE_DIRECTORY "${FFMPEG_TMPDIR}")

# 查找可用的 bash
# Windows: MSYS2 > Git Bash (避免 WSL bash, 无法访问 Windows 文件系统)
# Linux: 系统默认 bash
if(WIN32)
    find_program(BASH_EXECUTABLE
        NAMES bash
        PATHS
            "D:/APP/Msys64/usr/bin"
            "C:/msys64/usr/bin"
            "C:/Program Files/Git/bin"
            "C:/Program Files/Git/usr/bin"
        NO_DEFAULT_PATH
    )
    if(NOT BASH_EXECUTABLE)
        find_program(BASH_EXECUTABLE bash)
    endif()
    if(NOT BASH_EXECUTABLE)
        message(FATAL_ERROR "bash not found, required for FFmpeg build on Windows. Install MSYS2 or Git for Windows.")
    endif()
else()
    find_program(BASH_EXECUTABLE bash)
    if(NOT BASH_EXECUTABLE)
        message(FATAL_ERROR "bash not found")
    endif()
endif()
message(STATUS "Using bash: ${BASH_EXECUTABLE}")

# 转换 Windows 路径为 Unix 格式 (仅 Windows 需要)
function(to_unix_path OUT_VAR IN_PATH)
    if(WIN32)
        string(REGEX REPLACE "^([A-Za-z]):/" "/\\1/" _unix "${IN_PATH}")
        string(TOLOWER "${_unix}" _unix)
        set(${OUT_VAR} "${_unix}" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "${IN_PATH}" PARENT_SCOPE)
    endif()
endfunction()

to_unix_path(FFMPEG_SOURCE_DIR_UNIX "${FFMPEG_SOURCE_DIR}")
to_unix_path(FFMPEG_TMPDIR_UNIX "${FFMPEG_TMPDIR}")
to_unix_path(FFMPEG_INSTALL_PREFIX_UNIX "${FFMPEG_INSTALL_PREFIX}")

# 为 configure 命令转换路径参数
# 替换 configure 参数中的 Windows 路径为 Unix 路径
# 注意：顺序很重要，先替换更具体的路径（CMAKE_CURRENT_SOURCE_DIR 是其他路径的前缀）
set(FFMPEG_CONFIGURE_ARGS_UNIX ${FFMPEG_CONFIGURE_ARGS})
to_unix_path(CMAKE_CURRENT_SOURCE_DIR_UNIX "${CMAKE_CURRENT_SOURCE_DIR}")
list(TRANSFORM FFMPEG_CONFIGURE_ARGS_UNIX REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR_UNIX}")
list(TRANSFORM FFMPEG_CONFIGURE_ARGS_UNIX REPLACE "${FFMPEG_INSTALL_PREFIX}" "${FFMPEG_INSTALL_PREFIX_UNIX}")

# Vulkan 导入库路径（仅 Windows: MinGW 需要 .a 格式，不能用 MSVC 的 .lib）
if(FFMPEG_ENABLE_VULKAN AND WIN32)
    to_unix_path(VULKAN_IMPORT_DIR_UNIX "${VULKAN_IMPORT_DIR}")
    list(APPEND FFMPEG_CONFIGURE_ARGS_UNIX
        "--extra-ldflags=-L${VULKAN_IMPORT_DIR_UNIX} -lvulkan-1"
    )
endif()

string(REPLACE ";" " " FFMPEG_CONFIGURE_STRING_UNIX "${FFMPEG_CONFIGURE_ARGS_UNIX}")

# Bash 环境: Windows 需要 MSYS2 UCRT64 环境, Linux 直接使用系统环境
if(WIN32)
    # 从 BASH_EXECUTABLE 推导 MSYS2 根目录
    get_filename_component(_bash_bin "${BASH_EXECUTABLE}" DIRECTORY)
    get_filename_component(_msys2_root "${_bash_bin}/../.." ABSOLUTE)
    to_unix_path(MSYS2_USR_BIN_UNIX "${_msys2_root}/usr/bin")
    to_unix_path(MSYS2_UCRT64_BIN_UNIX "${_msys2_root}/ucrt64/bin")
    set(BASH_ENV "export MSYSTEM=UCRT64 && export PATH=${MSYS2_UCRT64_BIN_UNIX}:${MSYS2_USR_BIN_UNIX}:${MSYS2_USR_BIN_UNIX}/../local/bin:/bin")
else()
    set(BASH_ENV "")
endif()

# 构建 Vulkan 导入库的预处理命令（仅 Windows MinGW 兼容）
if(FFMPEG_ENABLE_VULKAN AND WIN32)
    set(VULKAN_GENLIB_CMD "mkdir -p ${VULKAN_IMPORT_DIR_UNIX} && cd ${VULKAN_IMPORT_DIR_UNIX} && gendef /c/Windows/System32/vulkan-1.dll && dlltool -d vulkan-1.def -l libvulkan-1.a -D vulkan-1.dll && ")
else()
    set(VULKAN_GENLIB_CMD "")
endif()

# 生成 post-install 脚本
# Windows: 为每个 DLL 生成 .def 文件供 MSVC 使用 (gendef 是 MSYS2 工具)
# Linux: 不需要 .def 文件，直接生成 .so
if(WIN32)
    set(_GENDEF_SCRIPT "${CMAKE_BINARY_DIR}/ffmpeg_gendef.sh")
    file(WRITE "${_GENDEF_SCRIPT}"
"#!/bin/bash
cd ${FFMPEG_INSTALL_PREFIX_UNIX}/bin
for f in av*.dll sw*.dll; do
    [ -f \"\$f\" ] || continue
    echo \"Generating .def for \$f\"
    gendef \"\$f\" 2>/dev/null
done
")
    to_unix_path(_GENDEF_SCRIPT_UNIX "${_GENDEF_SCRIPT}")
    set(FFMPEG_INSTALL_SUFFIX "&& bash ${_GENDEF_SCRIPT_UNIX}")
else()
    set(FFMPEG_INSTALL_SUFFIX "")
endif()

# 构建 FFmpeg 的 configure/build/install 命令前缀
# Windows: 需要先设置 MSYS2 环境
# Linux: 直接执行
if(WIN32)
    set(FFMPEG_CMD_PREFIX "${BASH_ENV} && ")
else()
    set(FFMPEG_CMD_PREFIX "")
endif()

ExternalProject_Add(ffmpeg_build
    SOURCE_DIR "${FFMPEG_SOURCE_DIR}"
    CONFIGURE_COMMAND ${BASH_EXECUTABLE} -c "${FFMPEG_CMD_PREFIX}${VULKAN_GENLIB_CMD}cd ${FFMPEG_SOURCE_DIR_UNIX} && TMPDIR=${FFMPEG_TMPDIR_UNIX} ./configure ${FFMPEG_CONFIGURE_STRING_UNIX}"
    BUILD_COMMAND ${BASH_EXECUTABLE} -c "${FFMPEG_CMD_PREFIX}cd ${FFMPEG_SOURCE_DIR_UNIX} && TMPDIR=${FFMPEG_TMPDIR_UNIX} make -j8"
    INSTALL_COMMAND ${BASH_EXECUTABLE} -c "${FFMPEG_CMD_PREFIX}cd ${FFMPEG_SOURCE_DIR_UNIX} && TMPDIR=${FFMPEG_TMPDIR_UNIX} make install ${FFMPEG_INSTALL_SUFFIX}"
    BUILD_IN_SOURCE 1
    BUILD_BYPRODUCTS
        "${FFMPEG_INSTALL_PREFIX}/lib/${FFMPEG_LIB_PREFIX}avcodec${FFMPEG_LIB_SUFFIX}"
        "${FFMPEG_INSTALL_PREFIX}/lib/${FFMPEG_LIB_PREFIX}avformat${FFMPEG_LIB_SUFFIX}"
        "${FFMPEG_INSTALL_PREFIX}/lib/${FFMPEG_LIB_PREFIX}avutil${FFMPEG_LIB_SUFFIX}"
        "${FFMPEG_INSTALL_PREFIX}/lib/${FFMPEG_LIB_PREFIX}swscale${FFMPEG_LIB_SUFFIX}"
        "${FFMPEG_INSTALL_PREFIX}/lib/${FFMPEG_LIB_PREFIX}swresample${FFMPEG_LIB_SUFFIX}"
    INSTALL_DIR "${FFMPEG_INSTALL_PREFIX}"
)

# ── 生成 MSVC 导入库 (.lib) 从 MinGW 的 .def 文件 ──
# MinGW 产生的 .dll.a 无法被 MSVC link.exe 使用，必须用 lib.exe 生成 .lib
if(WIN32 AND MSVC)
    # 找到 MSVC 的 lib.exe（和 CMAKE_LINKER 同目录）
    get_filename_component(_msvc_bin_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(MSVC_LIB_EXE NAMES lib.exe PATHS "${_msvc_bin_dir}" NO_DEFAULT_PATH)
    if(NOT MSVC_LIB_EXE)
        find_program(MSVC_LIB_EXE NAMES lib.exe)
    endif()
    message(STATUS "MSVC lib.exe: ${MSVC_LIB_EXE}")

    if(MSVC_LIB_EXE)
        # 生成 CMake 脚本用于创建 .lib 导入库
        set(_GENLIB_SCRIPT "${CMAKE_BINARY_DIR}/ffmpeg_gen_msvc_lib.cmake")
        file(WRITE "${_GENLIB_SCRIPT}"
"set(INSTALL_DIR \"${FFMPEG_INSTALL_PREFIX}\")
file(GLOB DLL_FILES \"\${INSTALL_DIR}/bin/av*.dll\" \"\${INSTALL_DIR}/bin/sw*.dll\")
foreach(DLL \${DLL_FILES})
    get_filename_component(DLL_NAME \${DLL} NAME_WE)
    set(DEF_FILE \"\${INSTALL_DIR}/bin/\${DLL_NAME}.def\")
    set(LIB_FILE \"\${INSTALL_DIR}/lib/\${DLL_NAME}.lib\")
    if(EXISTS \"\${DEF_FILE}\")
        execute_process(
            COMMAND \"${MSVC_LIB_EXE}\" /machine:x64 /def:\"\${DEF_FILE}\" /out:\"\${LIB_FILE}\"
            RESULT_VARIABLE _res
        )
        message(STATUS \"Created \${LIB_FILE} (exit \${_res})\")
    endif()
endforeach()
")
        # 将生成步骤添加为 install 后的钩子
        ExternalProject_Add_Step(ffmpeg_build msvc_import_libs
            COMMAND ${CMAKE_COMMAND} -P "${_GENLIB_SCRIPT}"
            DEPENDEES install
            COMMENT "Generating MSVC import libraries from FFmpeg DLLs..."
        )
    else()
        message(WARNING "MSVC lib.exe not found, cannot generate FFmpeg import libraries")
    endif()
endif()

# ── 导入的目标和链接库 ──
add_library(ffmpeg::avcodec SHARED IMPORTED GLOBAL)
add_library(ffmpeg::avformat SHARED IMPORTED GLOBAL)
add_library(ffmpeg::avutil SHARED IMPORTED GLOBAL)
add_library(ffmpeg::swscale SHARED IMPORTED GLOBAL)
add_library(ffmpeg::swresample SHARED IMPORTED GLOBAL)

# 库文件属性 (FFMPEG_LIB_PREFIX/SUFFIX 已在前面定义)
if(WIN32 AND MSVC)
    set(FFMPEG_IMPLIB_PROP IMPORTED_IMPLIB)
else()
    set(FFMPEG_IMPLIB_PROP IMPORTED_LOCATION)
endif()

foreach(_lib avcodec avformat avutil swscale swresample)
    set_target_properties(ffmpeg::${_lib} PROPERTIES
        ${FFMPEG_IMPLIB_PROP} "${FFMPEG_SOURCE_LIB_DIR}/${FFMPEG_LIB_PREFIX}${_lib}${FFMPEG_LIB_SUFFIX}"
        IMPORTED_NO_SONAME ON
    )
endforeach()

set(FFMPEG_LIBRARIES
    "${FFMPEG_SOURCE_LIB_DIR}/${FFMPEG_LIB_PREFIX}avcodec${FFMPEG_LIB_SUFFIX}"
    "${FFMPEG_SOURCE_LIB_DIR}/${FFMPEG_LIB_PREFIX}avformat${FFMPEG_LIB_SUFFIX}"
    "${FFMPEG_SOURCE_LIB_DIR}/${FFMPEG_LIB_PREFIX}avutil${FFMPEG_LIB_SUFFIX}"
    "${FFMPEG_SOURCE_LIB_DIR}/${FFMPEG_LIB_PREFIX}swscale${FFMPEG_LIB_SUFFIX}"
    "${FFMPEG_SOURCE_LIB_DIR}/${FFMPEG_LIB_PREFIX}swresample${FFMPEG_LIB_SUFFIX}"
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
