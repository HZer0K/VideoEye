# ============================================================
# RuntimeDeploy.cmake — Windows 运行时部署（DLL 树 + Qt 插件）
#
# 对外接口:
#   videoeye_register_runtime_target(<target> [额外 DLL...])
#       登记一个可执行文件。构建时它会依赖 videoeye-runtime，
#       所以不管 ninja 先链接谁、开几个并发，部署都只跑一次。
#   videoeye_finalize_windows_runtime(<dest 目录>)
#       在所有目标都登记完之后调用一次，真正组装部署步骤。
#   VIDEOEYE_RUNTIME_DEPS_SCAN_SCRIPT / VIDEOEYE_RUNTIME_DEST
#       供 install(CODE) 复用同一套脚本与同一份搜索目录。
#
# 为什么集中到一个 custom target 而不是每个目标自己的 POST_BUILD:
#   18 个测试可执行文件 + 主程序在同一个输出目录里，各自的 POST_BUILD 都去拷
#   同一批 av*.dll / Qt6*.dll。ninja 并行起来就是几个进程同时写同一个文件，
#   Windows 上直接 "file COPY ... Permission denied"。<｜hy_place▁holder▁no▁813｜>一个人干，
#   靠依赖关系保证顺序，既没有竞态也快。
#
# 为什么不能只靠 $<TARGET_RUNTIME_DLLS>:
#   它只覆盖 CMake 链接图里认识的 imported target。Qt6Core.dll 自己依赖的
#   zlib1.dll / double-conversion.dll / pcre2-16.dll 不在链接图里，且 vcpkg
#   布局下 windeployqt 也扫不出来（它认为 Qt6Core.dll「已是最新」）—— 于是
#   构建 100% 成功、程序一启动就 0xc0000135 (STATUS_DLL_NOT_FOUND)。
#   这类错误与代码毫无关系，排查成本极高。
#
# 注意 CACHE INTERNAL: 这些函数会从 tests/ 子目录被调用，普通变量在函数体里
# 读到的是「调用方作用域」，拼出来会是源码根目录下的错误路径。缓存变量才是全局的。
# ============================================================

if(DEFINED __VIDEOEYE_RUNTIME_DEPLOY_INCLUDED)
    return()
endif()
set(__VIDEOEYE_RUNTIME_DEPLOY_INCLUDED TRUE)

set(VIDEOEYE_RUNTIME_DEPS_SCAN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/RuntimeDepsScan.cmake"
    CACHE INTERNAL "运行时依赖闭包扫描脚本")

# ------------------------------------------------------------
# 搜索目录: 项目自带依赖 DLL 的所在位置
#   * CMAKE_PREFIX_PATH 下的 bin/ 与 debug/bin/（vcpkg / Qt 都遵循这个布局）
#   * Qt6 包根目录的 bin/（Qt6_DIR = <root>/lib/cmake/Qt6）
#   * FFmpeg 预编译包的 bin/
# ------------------------------------------------------------
function(videoeye_windows_runtime_search_dirs out_var)
    set(_dirs "")

    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        foreach(_sub "bin" "debug/bin")
            if(EXISTS "${_prefix}/${_sub}")
                list(APPEND _dirs "${_prefix}/${_sub}")
            endif()
        endforeach()
    endforeach()

    if(Qt6_DIR)
        # <root>/lib/cmake/Qt6 -> <root>/bin
        get_filename_component(_qt "${Qt6_DIR}/../../.." ABSOLUTE)
        if(EXISTS "${_qt}/bin")
            list(APPEND _dirs "${_qt}/bin")
        endif()
    endif()

    if(FFMPEG_BIN_DIR AND EXISTS "${FFMPEG_BIN_DIR}")
        list(APPEND _dirs "${FFMPEG_BIN_DIR}")
    endif()

    list(REMOVE_DUPLICATES _dirs)
    set(${out_var} "${_dirs}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------
# 登记一个运行时目标。第二个参数起是"只有这个目标才用得到"的 DLL
# （例如单元测试的 gtest.dll / gtest_main.dll）。
# ------------------------------------------------------------
function(videoeye_register_runtime_target target)
    if(NOT WIN32)
        return()
    endif()
    if(ARGN)
        set_property(GLOBAL APPEND PROPERTY VIDEOEYE_RUNTIME_EXTRA_DLLS ${ARGN})
    endif()
    set_property(GLOBAL APPEND PROPERTY VIDEOEYE_RUNTIME_TARGETS "${target}")
endfunction()

# ------------------------------------------------------------
# 组装部署步骤。必须在所有 videoeye_register_runtime_target() 之后调用。
#
# 执行顺序:
#   1. 建输出目录
#   2. 拷贝"已知"的 DLL: FFmpeg 整包 + 各 target 链接到的 Qt / FFmpeg 库
#      + 登记的额外 DLL (gtest)
#   3. 对拷过去的 Qt DLL 跑依赖闭包 —— 把 zlib1 / pcre2-16 /
#      double-conversion 这类"只有读 PE 导入表才知道"的项补齐
#      (FFmpeg 整包已经拷全了，没必要再扫；省一次全树遍历)
#   4. 主程序单独跑 windeployqt 补 Qt 插件 (platforms/qwindows.dll 等)
# ------------------------------------------------------------
function(videoeye_finalize_windows_runtime dest_dir)
    if(NOT WIN32)
        return()
    endif()

    get_property(_targets GLOBAL PROPERTY VIDEOEYE_RUNTIME_TARGETS)
    if(NOT _targets)
        return()
    endif()
    list(REMOVE_DUPLICATES _targets)

    videoeye_windows_runtime_search_dirs(_search_dirs)
    if(NOT _search_dirs)
        message(WARNING
            "[runtime-deploy] 没找到任何依赖目录，运行时依赖不会被补齐。\n"
            "请检查 CMAKE_PREFIX_PATH / FFMPEG_ROOT。")
        return()
    endif()

    set(VIDEOEYE_RUNTIME_DEST "${dest_dir}" CACHE INTERNAL "Windows 运行时部署目标目录")
    set(VIDEOEYE_RUNTIME_SEARCH_DIRS "${_search_dirs}" CACHE INTERNAL "Windows 运行时依赖搜索目录")

    # ---- 2) 已知 DLL ----
    set(_known "")
    if(VIDEOEYE_BUNDLE_FFMPEG AND FFMPEG_WINDOWS_DLLS)
        list(APPEND _known ${FFMPEG_WINDOWS_DLLS})
    endif()
    foreach(_t IN LISTS _targets)
        # $<TARGET_RUNTIME_DLLS> 在这时会展开成 Qt6Gui.dll / Qt6Widgets.dll 等
        list(APPEND _known "$<TARGET_RUNTIME_DLLS:${_t}>")
    endforeach()
    get_property(_extra GLOBAL PROPERTY VIDEOEYE_RUNTIME_EXTRA_DLLS)
    if(_extra)
        list(REMOVE_DUPLICATES _extra)
        list(APPEND _known ${_extra})
    endif()

    # ---- 3) 闭包扫描的输入: 拷过去的 Qt DLL ----
    # FFmpeg 整包 bin/ 已经拷全了，没必要再扫一遍；gtest 没有额外依赖。
    # 需要扫的就是 Qt —— zlib1 / pcre2-16 / double-conversion 只在这一层才现形。
    set(_qt_modules Qt6::Core Qt6::Gui Qt6::Widgets)
    set(_qt_scan_inputs "")
    foreach(_m IN LISTS _qt_modules)
        if(TARGET ${_m})
            list(APPEND _qt_scan_inputs "${dest_dir}/$<TARGET_FILE_NAME:${_m}>")
        endif()
    endforeach()

    add_custom_target(videoeye-runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dest_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_known} "${dest_dir}"
        COMMAND ${CMAKE_COMMAND}
                "-DVE_INPUTS=${_qt_scan_inputs}"
                "-DVE_DEST=${dest_dir}"
                "-DVE_SEARCH_DIRS=${_search_dirs}"
                -P "${VIDEOEYE_RUNTIME_DEPS_SCAN_SCRIPT}"
        COMMAND_EXPAND_LISTS
        COMMENT "Deploy Windows runtime dependencies -> ${dest_dir}"
        VERBATIM
    )

    foreach(_t IN LISTS _targets)
        add_dependencies(${_t} videoeye-runtime)
    endforeach()
endfunction()
