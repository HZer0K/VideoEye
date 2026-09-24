# ============================================================
# FFmpegPrebuilt.cmake — 定位 FFmpeg 并生成 FFmpeg::<comp> imported target
#
# 两条查找路径（按平台选默认，可显式覆盖）:
#   A) 预编译包  third_party/prebuilt/<platform>/ffmpeg/{include,lib,bin}
#                <platform> = windows-x64 | linux-x64 | macos-arm64 | macos-x64
#                Windows 默认且唯一走这条路。
#   B) 系统 FFmpeg  通过 pkg-config 定位 libavcodec / libavformat / ...
#                Linux / macOS 默认走这条路（与 CI 装的系统开发包一致）。
#
# 导出目标: FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
#           FFmpeg::swscale FFmpeg::swresample
#
# 可用开关:
#   -DFFMPEG_ROOT=<dir>                    强制使用某个 FFmpeg 根目录
#   -DVIDEOEYE_FFMPEG_USE_PKGCONFIG=ON|OFF Linux/macOS 是否优先用系统 FFmpeg
#   -DVIDEOEYE_BUNDLE_FFMPEG=ON|OFF        是否把动态库复制进构建产物 / 安装目录
#
# 为什么不统一成一种方式:
#   Windows 没有 pkg-config 惯例, 且必须有 .lib 导入库才能链接 —— 用项目自带的
#   gyan.dev 预编译包最省事; Linux/macOS 的包管理器已经装好了 FFmpeg 开发包,
#   再来一份预编译包只会让 ABI 不一致、体积翻倍。
# ============================================================

if(DEFINED __VIDEOEYE_FFMPEG_INCLUDED)
    return()
endif()
set(__VIDEOEYE_FFMPEG_INCLUDED TRUE)

# 在 file scope 里先算好脚本路径。
# 不能在 function 内部用 CMAKE_CURRENT_LIST_DIR —— 函数体读到的是"调用方"的目录,
# 那样会拼出 <源码根>/FFmpegRuntimeDeploy.cmake 这种不存在的路径。
set(_FFMPEG_DEPLOY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/FFmpegRuntimeDeploy.cmake")

set(_ffmpeg_components avcodec avformat avutil swscale swresample)

set(FFMPEG_ROOT "" CACHE PATH
    "FFmpeg 根目录 (include/lib/bin)。留空则按平台策略查找")
set(VIDEOEYE_PREBUILT_ROOT "${CMAKE_SOURCE_DIR}/third_party/prebuilt" CACHE PATH
    "预编译第三方库根目录")

if(WIN32)
    # Windows 没有可用的 pkg-config 生态, 强制走预编译包
    option(VIDEOEYE_FFMPEG_USE_PKGCONFIG "使用 pkg-config 查找系统 FFmpeg" OFF)
else()
    option(VIDEOEYE_FFMPEG_USE_PKGCONFIG "使用 pkg-config 查找系统 FFmpeg" ON)
endif()

option(VIDEOEYE_BUNDLE_FFMPEG "把 FFmpeg 动态库复制进构建产物与安装目录" ON)

# ---- 1) 平台 → 预编译目录名 ----
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

# ---- 2) RPATH 约定 ----
# 可执行文件在 bin/，动态库在相邻的 lib/ —— 构建树与安装树采用同样的相对布局,
# 所以同一条 rpath 两边都能用:
#   * Linux : $ORIGIN 是"可执行文件所在目录"
#   * macOS : @loader_path 是"加载者所在目录"（比 @executable_path 更普适）
# 具体拼接见 videoeye_ffmpeg_rpath()。

# ---- 3) 通用小工具 ----
# 判断库文件是静态还是动态。Windows 的 .lib 是 DLL 的导入库, 按动态算。
function(_ffmpeg_library_kind path out_kind)
    if(WIN32)
        set(_kind "SHARED")
    else()
        get_filename_component(_ext "${path}" LAST_EXT)
        if(_ext STREQUAL ".a" OR _ext STREQUAL ".lib")
            set(_kind "STATIC")
        else()
            set(_kind "SHARED")
        endif()
    endif()
    set(${out_kind} "${_kind}" PARENT_SCOPE)
endfunction()

# 在预编译根目录里找某个组件的库文件
#   Windows: lib/<comp>.lib (导入库) + bin/<comp>-<ver>.dll (运行时)
#   其他   : lib/lib<comp>.{so,so.<ver>,dylib,a}
function(_ffmpeg_resolve_prebuilt comp root out_implib out_runtime)
    set(_implib "")
    set(_runtime "")
    set(_libdir "${root}/lib")
    set(_bindir "${root}/bin")

    if(WIN32)
        if(EXISTS "${_libdir}/${comp}.lib")
            set(_implib "${_libdir}/${comp}.lib")
        endif()
        # DLL 文件名带版本号 (avcodec-62.dll), 只能 glob
        file(GLOB _dlls "${_bindir}/${comp}-*.dll")
        if(_dlls)
            list(GET _dlls 0 _runtime)
        elseif(EXISTS "${_bindir}/${comp}.dll")
            set(_runtime "${_bindir}/${comp}.dll")
        endif()
    elseif(APPLE)
        foreach(_ext dylib a)
            if(EXISTS "${_libdir}/lib${comp}.${_ext}")
                set(_implib "${_libdir}/lib${comp}.${_ext}")
                set(_runtime "${_implib}")
                break()
            endif()
        endforeach()
    else()
        if(EXISTS "${_libdir}/lib${comp}.so")
            set(_implib "${_libdir}/lib${comp}.so")
        elseif(EXISTS "${_libdir}/lib${comp}.a")
            set(_implib "${_libdir}/lib${comp}.a")
        else()
            file(GLOB _versioned "${_libdir}/lib${comp}.so.[0-9]*")
            if(_versioned)
                list(SORT _versioned)
                list(REVERSE _versioned)
                list(GET _versioned 0 _implib)
            endif()
        endif()
        set(_runtime "${_implib}")
    endif()

    set(${out_implib} "${_implib}" PARENT_SCOPE)
    set(${out_runtime} "${_runtime}" PARENT_SCOPE)
endfunction()

# 静态库不会自动带上自己的传递依赖, 这里补齐一份通用兜底
set(_ffmpeg_static_extra_libs "")
if(NOT WIN32)
    find_package(Threads QUIET)
    if(Threads_FOUND)
        list(APPEND _ffmpeg_static_extra_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND _ffmpeg_static_extra_libs ${CMAKE_DL_LIBS})
    endif()
    list(APPEND _ffmpeg_static_extra_libs m)
endif()

# ============================================================
# 分支 A: 预编译包
# ============================================================
function(_ffmpeg_try_prebuilt out_ok)
    set(_candidates "")
    if(FFMPEG_ROOT)
        list(APPEND _candidates "${FFMPEG_ROOT}")
    endif()
    list(APPEND _candidates
        "${VIDEOEYE_PREBUILT_ROOT}/${_ffmpeg_platform}/ffmpeg"
        "${VIDEOEYE_PREBUILT_ROOT}/ffmpeg"
    )

    set(_root "")
    foreach(_dir IN LISTS _candidates)
        if(EXISTS "${_dir}/include/libavcodec/avcodec.h")
            set(_root "${_dir}")
            break()
        endif()
    endforeach()

    if(NOT _root)
        set(${out_ok} FALSE PARENT_SCOPE)
        return()
    endif()

    set(_libs "")
    set(_runtimes "")
    set(_missing "")
    set(_missing_dll "")
    foreach(_comp IN LISTS _ffmpeg_components)
        _ffmpeg_resolve_prebuilt(${_comp} "${_root}" _implib _runtime)
        if(NOT _implib)
            list(APPEND _missing ${_comp})
        else()
            # _runtimes 必须和 _ffmpeg_components 一一对应（下面按组件下标读取）
            list(APPEND _libs "${_implib}")
            list(APPEND _runtimes "${_runtime}")
            if(WIN32 AND NOT _runtime)
                list(APPEND _missing_dll ${_comp})
            endif()
        endif()
    endforeach()

    if(_missing)
        message(FATAL_ERROR
            "FFmpeg 预编译包不完整: ${_root}\n"
            "缺少组件 ${_missing} 的库文件。\n"
            "必须是开发包（include/ + lib/ + bin/），只有 runtime DLL 的包不能用于链接。")
    endif()

    # Windows 只有 .lib 没有 DLL 是最坑的一种半包: 链接一定过, 运行时 0xc0000135。
    # 与其拖到启动才炸，不如在 configure 阶段就说清楚。
    if(_missing_dll)
        message(FATAL_ERROR
            "FFmpeg 预编译包缺少运行时 DLL: ${_root}\n"
            "组件 ${_missing_dll} 有导入库(lib/*.lib) 但 bin/ 下找不到对应的 dll。\n"
            "gyan.dev 的包分 shared / shared-libs / dev 几种，需要的是带 bin/*.dll 的那份；\n"
            "重新获取: powershell -ExecutionPolicy Bypass -File scripts\\fetch-ffmpeg.ps1 -Force")
    endif()

    set(FFMPEG_SOURCE "prebuilt" PARENT_SCOPE)
    set(FFMPEG_VERSION "" PARENT_SCOPE)
    set(FFMPEG_PREBUILT_DIR "${_root}" PARENT_SCOPE)
    set(FFMPEG_LOCAL_DIR "${_root}" PARENT_SCOPE)
    set(FFMPEG_INCLUDE_DIRS "${_root}/include" PARENT_SCOPE)
    set(FFMPEG_LIBRARY_DIRS "${_root}/lib" PARENT_SCOPE)
    set(FFMPEG_LIB_DIR "${_root}/lib" PARENT_SCOPE)
    set(FFMPEG_BIN_DIR "${_root}/bin" PARENT_SCOPE)
    set(FFMPEG_COMPONENT_LIBRARIES "${_libs}" PARENT_SCOPE)
    set(FFMPEG_COMPONENT_RUNTIMES "${_runtimes}" PARENT_SCOPE)
    set(FFMPEG_COMPONENT_EXTRA_LIBS "" PARENT_SCOPE)
    set(FFMPEG_COMPONENT_KINDS "" PARENT_SCOPE)

    if(WIN32)
        # Windows 要整包拷（avcodec 还会依赖 swresample 之外的系统 DLL 不在此列，
        # 但诸如 libx264 之类的运行时 DLL 必须跟着走）
        file(GLOB _all_dlls "${_root}/bin/*.dll")
        set(FFMPEG_WINDOWS_DLLS "${_all_dlls}" PARENT_SCOPE)
        set(FFMPEG_RUNTIME_FILES "${_all_dlls}" PARENT_SCOPE)
    else()
        set(FFMPEG_WINDOWS_DLLS "" PARENT_SCOPE)
        set(FFMPEG_RUNTIME_FILES "${_runtimes}" PARENT_SCOPE)
    endif()

    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()

# ============================================================
# 分支 B: 系统 FFmpeg (pkg-config)
# ============================================================
# pkg_check_modules 的 <prefix>_LINK_LIBRARIES 多数发行版给绝对路径,
# 也有给出 "-lavcodec" 短形式的 —— 两种都处理。
function(_ffmpeg_collect_pc_libs comp out_implib out_extra)
    set(_files "")
    set(_extra "")
    set(_dirs "${PC_FFMPEG_${comp}_LIBRARY_DIRS}")

    foreach(_entry IN LISTS PC_FFMPEG_${comp}_LINK_LIBRARIES)
        if(IS_ABSOLUTE "${_entry}" AND EXISTS "${_entry}")
            list(APPEND _files "${_entry}")
        elseif(_entry MATCHES "^-l(.+)")
            find_library(_pc_lib_${comp}_${CMAKE_MATCH_1}
                NAMES "${CMAKE_MATCH_1}" HINTS ${_dirs})
            if(_pc_lib_${comp}_${CMAKE_MATCH_1})
                list(APPEND _files "${_pc_lib_${comp}_${CMAKE_MATCH_1}}")
            else()
                list(APPEND _extra "${_entry}")
            endif()
        elseif(_entry MATCHES "^-")
            # -pthread / -Wl,xxx 之类直接透传给链接器
            list(APPEND _extra "${_entry}")
        endif()
    endforeach()

    if(NOT _files)
        # 兜底: 在 pkg-config 给的目录里按名字直接找
        find_library(_pc_fallback_${comp} NAMES "${comp}" HINTS ${_dirs})
        if(_pc_fallback_${comp})
            list(APPEND _files "${_pc_fallback_${comp}}")
        endif()
    endif()

    set(_implib "")
    set(_rest "")
    if(_files)
        list(GET _files 0 _implib)
        list(LENGTH _files _n)
        if(_n GREATER 1)
            list(SUBLIST _files 1 -1 _rest)
        endif()
    endif()
    list(APPEND _rest ${_extra})

    set(${out_implib} "${_implib}" PARENT_SCOPE)
    set(${out_extra} "${_rest}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_try_pkgconfig out_ok)
    find_package(PkgConfig QUIET)
    if(NOT PKG_CONFIG_FOUND)
        set(${out_ok} FALSE PARENT_SCOPE)
        return()
    endif()

    set(_missing "")
    foreach(_comp IN LISTS _ffmpeg_components)
        pkg_check_modules(PC_FFMPEG_${_comp} QUIET "lib${_comp}")
        if(NOT PC_FFMPEG_${_comp}_FOUND)
            list(APPEND _missing "lib${_comp}")
        endif()
    endforeach()
    if(_missing)
        set(${out_ok} FALSE PARENT_SCOPE)
        return()
    endif()

    set(_libs "")
    set(_runtimes "")
    set(_deploy_libs "")
    set(_kinds "")
    set(_incdirs "")
    set(_libdirs "")
    set(_version "")

    foreach(_comp IN LISTS _ffmpeg_components)
        _ffmpeg_collect_pc_libs(${_comp} _implib _extra)
        if(NOT _implib)
            set(${out_ok} FALSE PARENT_SCOPE)
            return()
        endif()

        list(APPEND _libs "${_implib}")
        _ffmpeg_library_kind("${_implib}" _kind)
        list(APPEND _kinds "${_kind}")
        # FFMPEG_COMPONENT_RUNTIMES 是按组件下标读的，所以必须和 _ffmpeg_components
        # 一一对应: 静态库也要占位（值就用 implib，反正没人会去部署它）。
        # 真正需要部署的动态库单独放 _deploy_libs。
        list(APPEND _runtimes "${_implib}")
        if(_kind STREQUAL "SHARED")
            list(APPEND _deploy_libs "${_implib}")
        endif()
        # 附加依赖（-pthread / 第三方 codec 库等）按组件存到一个变量里
        set(_ffmpeg_extra_${_comp} "${_extra}" PARENT_SCOPE)

        if(PC_FFMPEG_${_comp}_INCLUDE_DIRS)
            list(APPEND _incdirs ${PC_FFMPEG_${_comp}_INCLUDE_DIRS})
        endif()
        if(PC_FFMPEG_${_comp}_LIBRARY_DIRS)
            list(APPEND _libdirs ${PC_FFMPEG_${_comp}_LIBRARY_DIRS})
        endif()
        if(NOT _version AND PC_FFMPEG_${_comp}_VERSION)
            set(_version "${PC_FFMPEG_${_comp}_VERSION}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES _incdirs)
    list(REMOVE_DUPLICATES _libdirs)

    set(FFMPEG_SOURCE "pkg-config" PARENT_SCOPE)
    set(FFMPEG_VERSION "${_version}" PARENT_SCOPE)
    set(FFMPEG_PREBUILT_DIR "" PARENT_SCOPE)
    set(FFMPEG_LOCAL_DIR "" PARENT_SCOPE)
    set(FFMPEG_INCLUDE_DIRS "${_incdirs}" PARENT_SCOPE)
    set(FFMPEG_LIBRARY_DIRS "${_libdirs}" PARENT_SCOPE)
    if(_libdirs)
        list(GET _libdirs 0 _first_libdir)
    else()
        set(_first_libdir "")
    endif()
    set(FFMPEG_LIB_DIR "${_first_libdir}" PARENT_SCOPE)
    set(FFMPEG_BIN_DIR "${_first_libdir}" PARENT_SCOPE)
    set(FFMPEG_COMPONENT_LIBRARIES "${_libs}" PARENT_SCOPE)
    set(FFMPEG_COMPONENT_RUNTIMES "${_runtimes}" PARENT_SCOPE)
    set(FFMPEG_COMPONENT_KINDS "${_kinds}" PARENT_SCOPE)
    set(FFMPEG_RUNTIME_FILES "${_deploy_libs}" PARENT_SCOPE)
    set(FFMPEG_WINDOWS_DLLS "" PARENT_SCOPE)

    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()

# ============================================================
# 6) 选一条路
# ============================================================
set(_pc_ok FALSE)
if(VIDEOEYE_FFMPEG_USE_PKGCONFIG AND NOT WIN32)
    _ffmpeg_try_pkgconfig(_pc_ok)
endif()

if(NOT _pc_ok)
    _ffmpeg_try_prebuilt(_prebuilt_ok)
    if(NOT _prebuilt_ok)
        if(WIN32)
            message(FATAL_ERROR
                "未找到 FFmpeg 预编译包。\n"
                "期望目录: ${VIDEOEYE_PREBUILT_ROOT}/${_ffmpeg_platform}/ffmpeg/{include,lib,bin}\n"
                "修复方式:\n"
                "  powershell -ExecutionPolicy Bypass -File scripts\\fetch-ffmpeg.ps1\n")
        else()
            message(FATAL_ERROR
                "未找到 FFmpeg。已尝试两条路径:\n"
                "  1) pkg-config: 缺少 ${_ffmpeg_components} 对应的 lib*.pc\n"
                "  2) 预编译包:   ${VIDEOEYE_PREBUILT_ROOT}/${_ffmpeg_platform}/ffmpeg/{include,lib,bin}\n"
                "修复方式 (任选其一):\n"
                "  Debian/Ubuntu: sudo apt install -y libavcodec-dev libavformat-dev libavutil-dev \\\n"
                "                 libswscale-dev libswresample-dev\n"
                "  macOS:         brew install ffmpeg\n"
                "  已有机:        cmake -DFFMPEG_ROOT=/path/to/ffmpeg ...\n")
        endif()
    endif()
endif()

set(FFMPEG_FOUND TRUE)

if(FFMPEG_SOURCE STREQUAL "pkg-config")
    set(_origin_label "system (pkg-config ${FFMPEG_VERSION})")
else()
    set(_origin_label "prebuilt (${FFMPEG_LOCAL_DIR})")
endif()
message(STATUS "FFmpeg: ${_origin_label}")

# ============================================================
# 7) 建 imported target
# ============================================================
set(_ffmpeg_index 0)
foreach(_comp IN LISTS _ffmpeg_components)
    list(GET FFMPEG_COMPONENT_LIBRARIES ${_ffmpeg_index} _implib)
    # 运行时只在 Windows 上有意义（MSVC 的 IMPORTED_LOCATION 要指向 DLL）。
    # 其他平台上 FFMPEG_COMPONENT_RUNTIMES 只是按组件占位的列表，读它没有意义，
    # 而且 pkg-config 静态库场景下它还可能短于组件数 —— 越界会让 list(GET) 直接报错。
    set(_runtime "")
    if(WIN32)
        list(LENGTH FFMPEG_COMPONENT_RUNTIMES _runtime_count)
        if(_ffmpeg_index LESS _runtime_count)
            list(GET FFMPEG_COMPONENT_RUNTIMES ${_ffmpeg_index} _runtime)
        endif()
    endif()
    math(EXPR _ffmpeg_index "${_ffmpeg_index} + 1")

    if(TARGET FFmpeg::${_comp})
        continue()
    endif()

    _ffmpeg_library_kind("${_implib}" _kind)
    add_library(FFmpeg::${_comp} ${_kind} IMPORTED GLOBAL)

    if(WIN32)
        # MSVC: 链接吃 .lib 导入库, IMPORTED_LOCATION 指向 DLL
        # （$<TARGET_RUNTIME_DLLS> 与 install 都依赖它）
        set_target_properties(FFmpeg::${_comp} PROPERTIES
            IMPORTED_IMPLIB   "${_implib}"
            IMPORTED_LOCATION "${_runtime}"
        )
    else()
        set_target_properties(FFmpeg::${_comp} PROPERTIES
            IMPORTED_LOCATION "${_implib}"
        )
    endif()

    set_target_properties(FFmpeg::${_comp} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIRS}"
    )

    if(_kind STREQUAL "STATIC")
        # 静态库不会自动带上自己的传递依赖
        set(_extra "${_ffmpeg_extra_${_comp}}")
        if(NOT _extra)
            set(_extra "${_ffmpeg_static_extra_libs}")
        else()
            list(APPEND _extra ${_ffmpeg_static_extra_libs})
            list(REMOVE_DUPLICATES _extra)
        endif()
        set_property(TARGET FFmpeg::${_comp} APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES ${_extra})
    endif()
endforeach()

# ============================================================
# 8) 兼容旧变量名
# ============================================================
set(FFMPEG_LIBRARY_DIR "${FFMPEG_LIB_DIR}")
set(FFMPEG_RUNTIME_DIR "${FFMPEG_BIN_DIR}")

# ============================================================
# 9) 运行时部署
# ============================================================
# Windows: DLL 必须与可执行文件同目录（Windows 的 DLL 查找规则如此）
# 其他:    动态库放相邻的 lib/, 用相对 rpath 指过去
#
# 注意: 这里所有变量都在函数体内部重算, 不能依赖 file scope 的临时变量 ——
# 函数被调用时的作用域是"调用方"的, 从 tests/ 子目录调用时那些变量并不可见。
function(videoeye_ffmpeg_rpath out_var)
    set(_rpath "")
    if(WIN32)
        set(_rpath "")
    elseif(APPLE)
        set(_rpath "@loader_path/../lib;@loader_path")
    else()
        set(_rpath "\$ORIGIN/../lib;\$ORIGIN")
    endif()
    # 原始库目录也带上: 关掉 VIDEOEYE_BUNDLE_FFMPEG 时靠它兜底
    foreach(_dir IN LISTS FFMPEG_LIBRARY_DIRS)
        list(APPEND _rpath "${_dir}")
    endforeach()
    set(${out_var} "${_rpath}" PARENT_SCOPE)
endfunction()

# 只设 rpath, 不复制文件（给单元测试这类"与主程序共处一个输出目录"的目标用）
function(videoeye_set_ffmpeg_rpath target)
    if(WIN32)
        return()
    endif()
    videoeye_ffmpeg_rpath(_rpath)
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH   "${_rpath}"
        INSTALL_RPATH "${_rpath}"
    )
endfunction()

# 复制动态库 + 校验 + 设 rpath（给主程序用）
#
# Windows 分支刻意什么都不做: DLL 由顶层的 videoeye-runtime 统一部署
# (cmake/deps/RuntimeDeploy.cmake)。如果这里也拷一份，两个完善独立的部署步骤
# 会往同一个目录写同一批 av*.dll，ninja 并行时就有概率撞上文件锁。
function(videoeye_deploy_ffmpeg target)
    if(WIN32)
        videoeye_set_ffmpeg_rpath(${target})
        return()
    endif()

    set(_libs "${FFMPEG_RUNTIME_FILES}")
    set(_dest "${CMAKE_BINARY_DIR}/lib")

    if(VIDEOEYE_BUNDLE_FFMPEG AND _libs)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    "-DVE_LIBS=${_libs}"
                    "-DVE_DEST=${_dest}"
                    -P "${_FFMPEG_DEPLOY_SCRIPT}"
            VERBATIM
            COMMENT "Deploy FFmpeg runtime for $<TARGET_FILE_NAME:${target}>"
        )
    endif()

    videoeye_set_ffmpeg_rpath(${target})
endfunction()

# 安装 FFmpeg 动态库。
# Windows 扁平布局 → bin/；其他 FHS 布局 → lib/，由 INSTALL_RPATH 指过去。
function(videoeye_install_ffmpeg)
    if(NOT VIDEOEYE_BUNDLE_FFMPEG)
        return()
    endif()
    if(WIN32 AND FFMPEG_WINDOWS_DLLS)
        install(FILES ${FFMPEG_WINDOWS_DLLS} DESTINATION bin)
    elseif(NOT WIN32 AND FFMPEG_RUNTIME_FILES)
        install(FILES ${FFMPEG_RUNTIME_FILES} DESTINATION lib)
    endif()
endfunction()
