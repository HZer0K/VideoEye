# ============================================================
# FfmpegTool.cmake — 定位「原生 ffmpeg 可执行程序」并按需随包分发
#
# 与 FFmpegPrebuilt.cmake 的区别:
#   那边找的是**开发包**（include/ + lib/），用来链接 libav* 做分析；
#   这边找的是**可执行的 ffmpeg 程序**，供「FFmpeg 命令工作台」页直接跑命令。
#   两者版本可以不同（例如链接 8.1 预编译包、执行系统 6.x 的 ffmpeg），
#   页面会把实际执行的程序路径与 `-version` 输出显示出来，避免误判。
#
# 对外接口:
#   videoeye_deploy_ffmpeg_tool(<target>)   构建期把 ffmpeg 拷到 target 所在目录
#   videoeye_install_ffmpeg_tool()          安装期把 ffmpeg 装到 bin/
#
# 可用开关:
#   -DFFMPEG_TOOL=<path>             强制指定 ffmpeg 可执行程序（优先级最高，不存在则报错）
#   -DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON 把该程序拷进安装树（默认 OFF，理由见下）
#
# 为什么默认**不**随包分发（许可证，不是技术问题）:
#   能下载的 ffmpeg 构建绝大多数是 GPLv3（gyan.dev 的 full/essentials、Debian 与
#   Homebrew 的包都启用了 --enable-gpl）。把一个 GPLv3 的 ffmpeg 可执行文件放进
#   VideoEye 的安装包，等于整个分发物要按 GPLv3 履约 —— 而 VideoEye 自身是 MIT，
#   MIT 的宽松正是它值得被采用的原因。
#
#   我们的选择是：**不替用户做这个决定**。
#     · 默认不分发，页面引导用户自己装一份（用户自用不受分发条款约束，
#       还能拿到含 libx264/libx265 的完整版，功能反而最好）；
#     · 打包者确信自己那份 ffmpeg 是 LGPL 构建、或愿意让分发物整体走 GPLv3 时，
#       用 -DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON 显式打开，并自行完成合规动作
#       （随附 GPL/LGPL 文本与对应源码获取方式）。
#
#   另注：Linux/macOS 上 ffmpeg 由包管理器装在系统路径，把它复制进安装树
#   还会和系统的 .so 版本打架，本来也不该拷。
#   详见 docs/FFMPEG_COMMAND_WORKBENCH.md「许可证：为什么默认不随包分发 ffmpeg」。
# ============================================================

if(DEFINED __VIDEOEYE_FFMPEG_TOOL_INCLUDED)
    return()
endif()
set(__VIDEOEYE_FFMPEG_TOOL_INCLUDED TRUE)

# 生成头文件的落点（各模块都把 VIDEOEYE_GENERATED_INCLUDE_DIR 挂在 PUBLIC include 目录里）
set(VIDEOEYE_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated" CACHE INTERNAL
    "CMake 生成头文件的目录")

set(FFMPEG_TOOL "" CACHE FILEPATH
    "原生 ffmpeg 可执行程序路径（留空则自动探测）")

# 默认 OFF —— 打开它会改变整个分发物的许可证，属于需要人来做决定的事，
# 不该由"恰好在 PATH 里找到一个 ffmpeg"这种偶然情况触发。
set(VIDEOEYE_BUNDLE_FFMPEG_TOOL OFF CACHE BOOL
    "把 ffmpeg 可执行程序拷进安装树（默认 OFF：会让分发物整体按 GPLv3 履约）")

# ---- 1) 探测 ----
# 优先级: 显式 FFMPEG_TOOL > (Windows) 预编译包 bin/ > 系统 PATH
set(_tool "")
set(_tool_bundle FALSE)

if(FFMPEG_TOOL)
    if(NOT EXISTS "${FFMPEG_TOOL}")
        message(FATAL_ERROR
            "FFMPEG_TOOL 指向的 ffmpeg 可执行程序不存在: ${FFMPEG_TOOL}\n"
            "显式指定了一个路径，就只应该有两种结果：用它，或者明确说它为什么不能用。")
    endif()
    set(_tool "${FFMPEG_TOOL}")
elseif(WIN32 AND FFMPEG_BIN_DIR AND EXISTS "${FFMPEG_BIN_DIR}/ffmpeg.exe")
    set(_tool "${FFMPEG_BIN_DIR}/ffmpeg.exe")
else()
    find_program(_ffmpeg_in_path NAMES ffmpeg ffmpeg.exe
        HINTS /usr/local/bin /opt/homebrew/bin /opt/local/bin)
    if(_ffmpeg_in_path)
        set(_tool "${_ffmpeg_in_path}")
    endif()
endif()

# ---- 2) 是否随包分发 ----
# 只有显式打开的开关才算数。以前"预编译包 bin/ 里恰好有 ffmpeg.exe"就会自动随包，
# 那等于把许可证决定交给了一次下载的偶然结果。
# 每次配置都先清空：它是 INTERNAL 缓存（本次扫描的产物），上一轮留下的值
# 绝不能在"这一轮没开随包"时继续生效 —— 那会把过期的 DLL 清单带进安装树。
set(VIDEOEYE_FFMPEG_TOOL_EXTRA_FILES "" CACHE INTERNAL
    "随 ffmpeg CLI 一起分发的运行时依赖（DLL）")

if(VIDEOEYE_BUNDLE_FFMPEG_TOOL)
    # 只拷可执行文件这件事**只在 Windows 上成立**：那边 DLL 就躺在同一个 bin/ 里，
    # 扫一遍导入表就能补齐。Linux 的 ffmpeg 依赖系统 .so（/usr/lib/x86_64-linux-gnu），
    # macOS 依赖 @rpath 与一堆 Homebrew dylib 还有签名 —— 复制过去既跑不起来，
    # 又会和系统里的 libav* 打架。与其交付一个必然坏的安装树，不如直接说不行。
    if(NOT WIN32)
        message(FATAL_ERROR
            "VIDEOEYE_BUNDLE_FFMPEG_TOOL 当前只支持 Windows。\n"
            "Linux/macOS 请使用系统安装的 ffmpeg（本页会自动在 PATH 与常见目录里找它），\n"
            "或自行保证运行环境的动态库路径 —— 本项目的随包逻辑不处理 .so/dylib 闭包。")
    endif()

    if(NOT _tool)
        message(FATAL_ERROR
            "VIDEOEYE_BUNDLE_FFMPEG_TOOL=ON 但没有可随包的 ffmpeg 可执行程序。\n"
            "请同时用 -DFFMPEG_TOOL=<path> 指定一份。")
    endif()
    set(_tool_bundle TRUE)

    # 预编译包里的那份依赖同目录的 av*.dll，会被 videoeye-runtime 一并铺到旁边；
    # 系统 PATH 里的共享构建可能带一堆私有 DLL，只拷 exe 过去必然 0xc0000135。
    set(_bundle_safe FALSE)
    if(WIN32 AND FFMPEG_BIN_DIR)
        get_filename_component(_tool_dir "${_tool}" DIRECTORY)
        get_filename_component(_prebuilt_bin "${FFMPEG_BIN_DIR}" ABSOLUTE)
        get_filename_component(_tool_dir_abs "${_tool_dir}" ABSOLUTE)
        if(_tool_dir_abs STREQUAL _prebuilt_bin)
            set(_bundle_safe TRUE)
        endif()
    endif()

    message(WARNING
        "[ffmpeg-tool] 将随包分发 ${_tool}\n"
        "  许可证：能下载到的 ffmpeg 构建绝大多数是 GPLv3。把它放进安装包，\n"
        "  整个分发物需按 GPLv3 履约（随附许可证文本 + 提供对应源码）。\n"
        "  若这份 ffmpeg 是 LGPL 构建，请一并随附 LGPL 文本与库的源码获取方式。\n"
        "  详见 docs/FFMPEG_COMMAND_WORKBENCH.md。")
    if(WIN32 AND NOT _bundle_safe)
        message(WARNING
            "[ffmpeg-tool] ${_tool} 不在预编译包 bin/ 里，它可能依赖自身目录下的私有 DLL。\n"
            "  下面会按导入表把它真正依赖的那些 DLL 一起纳入分发；若仍有未解析项会明确列出。")
    endif()

    # ---- 2.1) 依赖闭包：只拷 exe 是不够的 ----
    # 自定义 ffmpeg（共享构建）通常依赖同目录的 av*.dll / zlib / libwinpthread 等，
    # 缺一个就是 0xc0000135，而"能启动的 ffmpeg"和"能跑的 ffmpeg"差的就是这一步。
    # 扫的是**这一个 exe** 的导入表，只在 DIRECTORIES 里找，不牵扯系统目录之外的东西。
    set(_tool_extra "")
    get_filename_component(_tool_dir "${_tool}" DIRECTORY)
    set(_tool_scan_dirs "${_tool_dir}")
    if(FFMPEG_BIN_DIR)
        list(APPEND _tool_scan_dirs "${FFMPEG_BIN_DIR}")
    endif()
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${_tool}"
        RESOLVED_DEPENDENCIES_VAR _tool_resolved
        UNRESOLVED_DEPENDENCIES_VAR _tool_unresolved
        CONFLICTING_DEPENDENCIES_PREFIX _tool_conflict
        DIRECTORIES ${_tool_scan_dirs}
        PRE_EXCLUDE_REGEXES "^api-ms-win-.*" "^ext-ms-.*"
        POST_EXCLUDE_REGEXES ".*/[Ss][Yy][Ss][Tt][Ee][Mm]32/.*" ".*/[Ss][Yy][Ss][Ww][Oo][Ww]64/.*"
    )
    foreach(_dep IN LISTS _tool_resolved)
        # 只带"落在 ffmpeg 自己目录（或预编译包 bin/）里"的那些:
        # 系统 DLL（kernel32 / ucrtbase …）每台机器都有，跟着分发反而会踩版本坑。
        get_filename_component(_dep_dir "${_dep}" DIRECTORY)
        set(_keep FALSE)
        foreach(_dir IN LISTS _tool_scan_dirs)
            if(_dep_dir STREQUAL "${_dir}")
                set(_keep TRUE)
            endif()
        endforeach()
        if(_keep)
            list(APPEND _tool_extra "${_dep}")
        endif()
    endforeach()
    if(_tool_extra)
        list(REMOVE_DUPLICATES _tool_extra)
    endif()

    if(_tool_unresolved)
        message(WARNING
            "[ffmpeg-tool] ${_tool} 有未能解析的运行时依赖，随包后在用户机器上可能无法启动:\n"
            "  ${_tool_unresolved}\n"
            "  请改用自带依赖的构建（或静态构建），或把依赖所在目录加进 FFMPEG_BIN_DIR。")
    endif()
    if(_tool_conflict_FILENAMES)
        message(WARNING
            "[ffmpeg-tool] 依赖解析出现同名冲突: ${_tool_conflict_FILENAMES}\n"
            "  实际拷贝的是下面列出的绝对路径，请确认它们来自同一个构建。")
    endif()

    set(VIDEOEYE_FFMPEG_TOOL_EXTRA_FILES "${_tool_extra}" CACHE INTERNAL
        "随 ffmpeg CLI 一起分发的运行时依赖（DLL）")
    if(_tool_extra)
        message(STATUS "[ffmpeg-tool] 随 ffmpeg 一起分发的依赖: ${_tool_extra}")
    endif()
endif()

# 找不到 ffmpeg 是**正常的**：构建 VideoEye 只需要 libav* 开发包，执行 ffmpeg 命令
# 是终端用户的事，页面会引导他们安装。这里刻意用 STATUS 而不是 WARNING ——
# 否则每个没有 ffmpeg 的 CI/开发机都会被一条红字误导成"配置有问题"。
if(NOT _tool)
    message(STATUS
        "[ffmpeg-tool] 构建机上没有原生 ffmpeg 可执行程序 —— 构建不受影响\n"
        "  （分析能力用的是链接进来的 libav*，不是这个程序）。\n"
        "  「FFmpeg 命令工作台」页会显示为\"未找到 ffmpeg\"并引导终端用户安装；\n"
        "  想让本机的这一页直接可用: Windows 装 winget install --id Gyan.FFmpeg -e，\n"
        "  Debian/Ubuntu: sudo apt install -y ffmpeg，macOS: brew install ffmpeg。")
elseif(NOT _tool_bundle)
    # 口径必须与 docs/FFMPEG_COMMAND_WORKBENCH.md 第 8 节一致:
    # 不随包 ffmpeg.exe 只是**不额外分发一个 GPL 程序**，不等于分发物整体是 MIT ——
    # 链接进来并随包的 av*.dll 本身就有自己的条款，由 FFmpeg 包决定。
    message(STATUS
        "[ffmpeg-tool] 找到 ${_tool}，按默认策略**不**随包分发（安装树里不会有它）；\n"
        "  终端用户由工作台页引导自行安装。\n"
        "  注意: 这不额外增加新的义务，但也**不等于分发物整体是 MIT** —— 最终许可证取决于\n"
        "  链接进来并随包的那份 FFmpeg（当前 Windows 包是 GPLv3），见 docs/FFMPEG_COMMAND_WORKBENCH.md 第 8 节。\n"
        "  确实要随包打包: -DFFMPEG_TOOL=<path> -DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON（注意 GPLv3 义务）。")
endif()

set(VIDEOEYE_FFMPEG_TOOL "${_tool}" CACHE FILEPATH "原生 ffmpeg 可执行程序路径" FORCE)
set(VIDEOEYE_FFMPEG_TOOL_BUNDLE ${_tool_bundle} CACHE BOOL "是否随包分发 ffmpeg 可执行程序" FORCE)

if(_tool)
    get_filename_component(VIDEOEYE_FFMPEG_TOOL_NAME "${_tool}" NAME)
else()
    set(VIDEOEYE_FFMPEG_TOOL_NAME "")
endif()
set(VIDEOEYE_FFMPEG_TOOL_NAME "${VIDEOEYE_FFMPEG_TOOL_NAME}" CACHE STRING
    "ffmpeg 可执行程序文件名" FORCE)

if(_tool_bundle)
    set(_bundled_literal 1)
else()
    set(_bundled_literal 0)
endif()
set(VIDEOEYE_FFMPEG_TOOL_BUNDLED_LITERAL "${_bundled_literal}" CACHE INTERNAL
    "VIDEOEYE_FFMPEG_TOOL_BUNDLED 的字面值 (0/1)")

if(_tool)
    set(_bundle_hint "")
    if(_tool_bundle)
        set(_bundle_hint " (随包分发)")
    endif()
    message(STATUS "FFmpeg CLI: ${_tool}${_bundle_hint}")
endif()

# ---- 2.5) 构建期探测到的绝对路径该不该优先于 PATH ----
# 那个路径是**构建机**的事实。开发机自己跑它最合适；但安装版如果还认它，
# 用户新装/升级了 PATH 里的 ffmpeg 也仍会用构建机那一份 —— 安装包凭空依赖了一台
# 它不该知道的开发机。所以发布档（Release）让 PATH 与常见安装目录优先，
# 构建期路径降级为"实在找不到时的兜底"；Debug 保持优先，方便开发机上直接可用。
set(_prefer_buildtime_default OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_prefer_buildtime_default ON)
endif()
set(VIDEOEYE_FFMPEG_TOOL_PREFER_BUILDTIME ${_prefer_buildtime_default} CACHE BOOL
    "让构建期探测到的 ffmpeg 绝对路径优先于 PATH（默认随 Debug 打开）")
if(VIDEOEYE_FFMPEG_TOOL_PREFER_BUILDTIME)
    set(_prefer_buildtime_literal 1)
else()
    set(_prefer_buildtime_literal 0)
endif()
set(VIDEOEYE_FFMPEG_TOOL_PREFER_BUILDTIME_LITERAL "${_prefer_buildtime_literal}" CACHE INTERNAL
    "VIDEOEYE_FFMPEG_TOOL_PREFER_BUILDTIME 的字面值 (0/1)")

# ---- 3) 构建期部署: 拷到目标可执行文件旁边 ----
function(videoeye_deploy_ffmpeg_tool target)
    if(NOT VIDEOEYE_FFMPEG_TOOL_BUNDLE OR NOT VIDEOEYE_FFMPEG_TOOL)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VIDEOEYE_FFMPEG_TOOL}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM
        COMMENT "Deploy ffmpeg CLI -> $<TARGET_FILE_DIR:${target}>"
    )
    # 依赖 DLL 单独一条: 数量不定（可能为空），不能塞进上面那条 COMMAND 里
    foreach(_extra IN LISTS VIDEOEYE_FFMPEG_TOOL_EXTRA_FILES)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_extra}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
        )
    endforeach()
endfunction()

# ---- 4) 安装期部署: 装到 bin/ ----
function(videoeye_install_ffmpeg_tool)
    if(NOT VIDEOEYE_FFMPEG_TOOL_BUNDLE OR NOT VIDEOEYE_FFMPEG_TOOL)
        return()
    endif()
    install(PROGRAMS "${VIDEOEYE_FFMPEG_TOOL}" ${VIDEOEYE_FFMPEG_TOOL_EXTRA_FILES}
            DESTINATION bin)
endfunction()
