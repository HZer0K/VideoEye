# ============================================================
# RuntimeDepsScan.cmake — 递归解析动态库的运行时依赖并补齐缺失项
#
# 用法 (脚本模式, 不能在项目配置阶段调用):
#   cmake -DVE_INPUTS="exe1;dll2"          # 要调查的文件（可执行文件与动态库，按扩展名自动分类）
#         -DVE_DEST=<目标目录>             # 补全后的依赖也放这里
#         -DVE_SEARCH_DIRS="dir1;dir2"     # 项目自带依赖所在的目录
#         [-DVE_DRY_RUN=ON]                # 只打印要补齐的项, 不复制
#         -P RuntimeDepsScan.cmake
#
# 为什么需要它:
#   $<TARGET_RUNTIME_DLLS> 只包含 CMake 链接图里认识的 imported target。
#   Qt6Core.dll 自己依赖的 zlib1.dll / double-conversion.dll / pcre2-16.dll
#   不在链接图里；vcpkg 布局下 windeployqt 也扫不出来（它认为 Qt6Core.dll
#   "已是最新"）。结果就是构建 100% 成功、程序一启动就
#   0xc0000135 (STATUS_DLL_NOT_FOUND) —— 报的错和构建毫无关系，极难排查。
#
#   file(GET_RUNTIME_DEPENDENCIES) 直接读 PE 导入表并递归求闭包，不需要
#   dumpbin / objdump 之类的外部工具，Windows / Linux / macOS 通吃。
#
# 关键约定: 只补齐"落在 VE_SEARCH_DIRS 里"的依赖。
#   系统会解析出几百个 C:/WINDOWS/system32/*.dll —— 它们是操作系统提供的，
#   拷进程序目录会造成 DLL 地狱。用目录前缀过滤比正则更可靠。
# ============================================================

if(NOT VE_INPUTS)
    message(FATAL_ERROR "[runtime-deps] 未指定 VE_INPUTS")
endif()
if(NOT VE_DEST)
    message(FATAL_ERROR "[runtime-deps] 未指定 VE_DEST")
endif()

# Windows 下 DIRECTORIES 是唯一的搜索来源：不给就什么都解析不到。
if(WIN32 AND NOT VE_SEARCH_DIRS)
    message(FATAL_ERROR
        "[runtime-deps] Windows 必须提供 VE_SEARCH_DIRS "
        "(Qt / FFmpeg / vcpkg 的 bin 目录)")
endif()

file(MAKE_DIRECTORY "${VE_DEST}")

# ---- 归一化搜索目录: 去尾斜杠 + 转小写 (Windows 路径大小写不敏感) ----
set(_scan_dirs "")
set(_scan_lens "")
foreach(_dir IN LISTS VE_SEARCH_DIRS)
    if(NOT IS_DIRECTORY "${_dir}")
        continue()
    endif()
    file(REAL_PATH "${_dir}" _real)
    file(TO_CMAKE_PATH "${_real}" _real)
    while(_real MATCHES "/$")
        string(LENGTH "${_real}" _n)
        math(EXPR _n "${_n} - 1")
        string(SUBSTRING "${_real}" 0 ${_n} _real)
    endwhile()
    string(TOLOWER "${_real}" _low)
    string(LENGTH "${_low}" _len)
    if(_len GREATER 0)
        list(APPEND _scan_dirs "${_low}")
        list(APPEND _scan_lens "${_len}")
    endif()
endforeach()

if(NOT _scan_dirs)
    message(FATAL_ERROR
        "[runtime-deps] VE_SEARCH_DIRS 里没有一个存在的目录: ${VE_SEARCH_DIRS}")
endif()

# 判断是否属于项目自身的依赖树（用前缀比较，避免路径里的正则元字符）
function(_scan_in_project lib out_var)
    file(REAL_PATH "${lib}" _real)
    file(TO_CMAKE_PATH "${_real}" _real)
    string(TOLOWER "${_real}" _low)
    set(_idx 0)
    set(_hit FALSE)
    foreach(_dir IN LISTS _scan_dirs)
        list(GET _scan_lens ${_idx} _len)
        math(EXPR _idx "${_idx} + 1")
        string(LENGTH "${_low}" _lib_len)
        if(_lib_len LESS _len)
            continue()
        endif()
        string(SUBSTRING "${_low}" 0 ${_len} _head)
        if(_head STREQUAL _dir)
            set(_hit TRUE)
            break()
        endif()
    endforeach()
    set(${out_var} ${_hit} PARENT_SCOPE)
endfunction()

# ---- 按扩展名把输入分成 EXECUTABLES / LIBRARIES ----
# file(GET_RUNTIME_DEPENDENCIES) 对这两类的解析规则不同：LIBRARIES 会先在自己的
# 所在目录里找依赖。全塞进 EXECUTABLES 也能跑出结果，但和接口语义不符，
# 而且一旦 CMake 将来收紧校验就会直接报错。
set(_exes "")
set(_libs "")
foreach(_input IN LISTS VE_INPUTS)
    if(NOT EXISTS "${_input}")
        message(FATAL_ERROR "[runtime-deps] 输入文件不存在: ${_input}")
    endif()
    get_filename_component(_name "${_input}" NAME)
    # 注意 .so 可能带版本号 (libavcodec.so.62)，所以末尾用 ($|\.) 而不是 $
    if(_name MATCHES "\\.(so|dylib|dll)($|\\.)")
        list(APPEND _libs "${_input}")
    else()
        list(APPEND _exes "${_input}")
    endif()
endforeach()

set(_kind_args "")
if(_exes)
    list(APPEND _kind_args EXECUTABLES ${_exes})
endif()
if(_libs)
    list(APPEND _kind_args LIBRARIES ${_libs})
endif()
if(NOT _kind_args)
    message(FATAL_ERROR "[runtime-deps] VE_INPUTS 为空")
endif()

file(GET_RUNTIME_DEPENDENCIES
    ${_kind_args}
    RESOLVED_DEPENDENCIES_VAR   _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    CONFLICTING_DEPENDENCIES_PREFIX _conflict
    DIRECTORIES ${VE_SEARCH_DIRS}
)

# CMake 在"同文件名、多路径"时会直接报错。典型场景: 第二次构建时 zlib1.dll
# 既在上轮铺好的输出目录里、又在 vcpkg 的 bin/ 里（Windows 的解析规则会先看
# 被依赖 DLL 自己所在的目录）。冲突本身无害 —— 两边内容一样，而且下面还会
# 跳过"目标目录已有同名文件"的项。
if(_conflict_FILENAMES)
    message(STATUS "[runtime-deps] 忽略同名多路径依赖 ${_conflict_FILENAMES}")
endif()

# 解析不到的通常是系统库（Windows 的 API Set、Linux 的 libc 等），只在 VERBOSE 下提示
if(_unresolved)
    message(VERBOSE "[runtime-deps] 未解析（视为系统提供）: ${_unresolved}")
endif()

set(_needed "")
foreach(_lib IN LISTS _resolved)
    if(NOT EXISTS "${_lib}")
        continue()
    endif()
    _scan_in_project("${_lib}" _is_project)
    if(NOT _is_project)
        continue()
    endif()
    get_filename_component(_name "${_lib}" NAME)
    if(EXISTS "${VE_DEST}/${_name}")
        continue()
    endif()
    list(APPEND _needed "${_lib}")
endforeach()

list(REMOVE_DUPLICATES _needed)

if(VE_DRY_RUN)
    message(STATUS "[runtime-deps] dest : ${VE_DEST}")
    message(STATUS "  missing (would copy):")
    foreach(_lib IN LISTS _needed)
        message(STATUS "    ${_lib}")
    endforeach()
    return()
endif()

set(_copied 0)
foreach(_lib IN LISTS _needed)
    file(COPY "${_lib}" DESTINATION "${VE_DEST}")
    math(EXPR _copied "${_copied} + 1")
endforeach()
message(STATUS "[runtime-deps] 补齐 ${_copied} 个依赖到 ${VE_DEST}")

# 复制后就地校验 —— 这是这条链路唯一真正有价值的一步:
# 缺 DLL 的后果只会在运行时爆发, 而那时报错信息跟构建完全无关。
set(_missing "")
foreach(_lib IN LISTS _needed)
    get_filename_component(_name "${_lib}" NAME)
    if(NOT EXISTS "${VE_DEST}/${_name}")
        list(APPEND _missing "${_name}")
    endif()
endforeach()

if(_missing)
    message(FATAL_ERROR
        "[runtime-deps] 部署后校验失败。\n"
        "目标目录: ${VE_DEST}\n"
        "缺失文件: ${_missing}")
endif()
