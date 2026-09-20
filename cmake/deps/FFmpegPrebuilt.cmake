# ============================================================
# FFmpegPrebuilt.cmake — 只做一件事: 找 third_party/prebuilt 里的 FFmpeg
#
# 目录约定:
#   third_party/prebuilt/<platform>/ffmpeg/{include,lib,bin}
#     <platform> = windows-x64 | linux-x64 | macos-arm64 | macos-x64
#
# 导出目标: FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
#            FFmpeg::swscale FFmpeg::swresample
#
# 覆盖方式:
#   cmake -DFFMPEG_ROOT=/path/to/ffmpeg ...
#
# 说明: 本项目不使用 pkg-config / vcpkg 的 ffmpeg port, 避免让使用者
#       自己编译 FFmpeg。缺目录时给出可执行的修复命令, 而不是静默失败。
# ============================================================

set(_ffmpeg_components avcodec avformat avutil swscale swresample)

# ---- 1) 推断当前平台目录 ----
if(WIN32)
    set(_ffmpeg_platform "windows-x64")
elseif(APPLE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(_ffmpeg_platform "macos-arm64")
    else()
        set(_ffmpeg_platform "macos-x64")
    endif()
else()
    set(_ffmpeg_platform "linux-x64")
endif()

# ---- 2) 候选根目录 ----
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg 预编译根目录 (include/lib/bin)。留空则用 third_party/prebuilt/<platform>/ffmpeg")
set(VIDEOEYE_PREBUILT_ROOT "${CMAKE_SOURCE_DIR}/third_party/prebuilt" CACHE PATH "预编译第三方库根目录")

set(_ffmpeg_candidates "")
if(FFMPEG_ROOT)
    list(APPEND _ffmpeg_candidates "${FFMPEG_ROOT}")
endif()
list(APPEND _ffmpeg_candidates
    "${VIDEOEYE_PREBUILT_ROOT}/${_ffmpeg_platform}/ffmpeg"
    "${VIDEOEYE_PREBUILT_ROOT}/ffmpeg"
)

# ---- 3) 选定根目录 ----
set(FFMPEG_PREBUILT_DIR "")
foreach(_dir IN LISTS _ffmpeg_candidates)
    if(EXISTS "${_dir}/include/libavcodec/avcodec.h")
        set(FFMPEG_PREBUILT_DIR "${_dir}")
        break()
    endif()
endforeach()

if(NOT FFMPEG_PREBUILT_DIR)
    message(FATAL_ERROR
        "未找到 FFmpeg 预编译包。\n"
        "期望目录: ${VIDEOEYE_PREBUILT_ROOT}/${_ffmpeg_platform}/ffmpeg/{include,lib,bin}\n"
        "修复方式 (任选其一):\n"
        "  * Windows: powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1 -Version 8.1.2\n"
        "  * Linux/macOS: 手工解包 gyan.dev / evermeet.cx 的预编译包到上述目录\n"
        "                 (项目不携带 .sh 下载脚本，避免与平台包管理器冲突)\n"
        "  * 已有机:  cmake -DFFMPEG_ROOT=/path/to/ffmpeg ...\n")
endif()

set(FFMPEG_INCLUDE_DIR "${FFMPEG_PREBUILT_DIR}/include")
set(FFMPEG_LIBRARY_DIR "${FFMPEG_PREBUILT_DIR}/lib")
set(FFMPEG_RUNTIME_DIR "${FFMPEG_PREBUILT_DIR}/bin")

message(STATUS "FFmpeg (prebuilt): ${FFMPEG_PREBUILT_DIR}")

# ---- 4) 逐个组件建 imported target ----
# 返回: FFMPEG_<comp>_LIB / FFMPEG_<comp>_DLL
function(_ffmpeg_resolve_library comp out_implib out_runtime)
    set(_implib "")
    set(_runtime "")

    if(WIN32)
        # MSVC: lib/<name>.lib (导入库) + bin/<name>-<ver>.dll (运行时)
        if(EXISTS "${FFMPEG_LIBRARY_DIR}/${comp}.lib")
            set(_implib "${FFMPEG_LIBRARY_DIR}/${comp}.lib")
        endif()
        file(GLOB _dlls "${FFMPEG_RUNTIME_DIR}/${comp}-*.dll")
        if(_dlls)
            list(GET _dlls 0 _runtime)
        elseif(EXISTS "${FFMPEG_RUNTIME_DIR}/${comp}.dll")
            set(_runtime "${FFMPEG_RUNTIME_DIR}/${comp}.dll")
        endif()
    elseif(APPLE)
        foreach(_ext dylib a)
            if(EXISTS "${FFMPEG_LIBRARY_DIR}/lib${comp}.${_ext}")
                set(_implib "${FFMPEG_LIBRARY_DIR}/lib${comp}.${_ext}")
                set(_runtime "${FFMPEG_LIBRARY_DIR}/lib${comp}.${_ext}")
                break()
            endif()
        endforeach()
    else()
        foreach(_ext so so.[0-9] so.[0-9][0-9] a)
            file(GLOB _found "${FFMPEG_LIBRARY_DIR}/lib${comp}.${_ext}*")
            if(_found)
                list(GET _found 0 _implib)
                set(_runtime "${_implib}")
                break()
            endif()
        endforeach()
    endif()

    set(${out_implib} "${_implib}" PARENT_SCOPE)
    set(${out_runtime} "${_runtime}" PARENT_SCOPE)
endfunction()

set(FFMPEG_RUNTIME_FILES "")
foreach(comp IN LISTS _ffmpeg_components)
    _ffmpeg_resolve_library(${comp} _implib _runtime)

    if(NOT _implib)
        message(FATAL_ERROR
            "FFmpeg 组件 '${comp}' 的库文件未找到于 ${FFMPEG_LIBRARY_DIR}\n"
            "请确认预编译包完整 (include/ + lib/ + bin/)。")
    endif()

    if(NOT TARGET FFmpeg::${comp})
        add_library(FFmpeg::${comp} SHARED IMPORTED GLOBAL)
        set_target_properties(FFmpeg::${comp} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
        )
        if(WIN32)
            set_target_properties(FFmpeg::${comp} PROPERTIES
                IMPORTED_IMPLIB   "${_implib}"
                IMPORTED_LOCATION "${_runtime}"
            )
        else()
            set_target_properties(FFmpeg::${comp} PROPERTIES
                IMPORTED_LOCATION "${_implib}"
            )
        endif()
    endif()

    if(_runtime)
        list(APPEND FFMPEG_RUNTIME_FILES "${_runtime}")
    endif()
endforeach()

# ---- 5) 方便起见: 老代码里用过的一组变量 ----
set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
set(FFMPEG_LIB_DIR "${FFMPEG_LIBRARY_DIR}")
set(FFMPEG_BIN_DIR "${FFMPEG_RUNTIME_DIR}")
set(FFMPEG_LOCAL_DIR "${FFMPEG_PREBUILT_DIR}")
set(FFMPEG_FOUND TRUE)

# ---- 6) 运行时部署 ----
# Windows: 构建后把 bin/*.dll 拷到可执行文件旁。
function(videoeye_deploy_ffmpeg target)
    if(WIN32)
        file(GLOB _dlls "${FFMPEG_RUNTIME_DIR}/*.dll")
        if(_dlls)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_dlls}
                    "$<TARGET_FILE_DIR:${target}>"
                COMMAND_EXPAND_LISTS
                COMMENT "Copy FFmpeg DLLs to output directory"
            )
        endif()
    else()
        # Linux/macOS: 让可执行文件在同目录 lib/ 下找 .so / .dylib
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH "$ORIGIN/lib;$ORIGIN"
            INSTALL_RPATH "$ORIGIN/lib;$ORIGIN"
        )
    endif()
endfunction()
