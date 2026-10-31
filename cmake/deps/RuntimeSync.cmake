# ============================================================
# RuntimeSync.cmake — 把运行时依赖同步到输出目录（幂等 + 按需闭包扫描）
#
# 用法 (脚本模式):
#   cmake -DVE_LIST_FILE=<已知 DLL 清单文件>      # file(GENERATE) 写出的换行/分号分隔清单
#         -DVE_DEST=<输出目录>
#         -DVE_SCAN_INPUTS="<要扫的 DLL>;..."      # 闭包扫描的入口（一般就是 Qt 那几个）
#         -DVE_SEARCH_DIRS="<依赖搜索目录>;..."
#         -DVE_SCAN_SCRIPT=<RuntimeDepsScan.cmake>
#         [-DVE_FORCE_SCAN=ON]
#         [-DVE_QUIET=ON]
#         -P RuntimeSync.cmake
#
# 为什么不是「一个带 OUTPUT 的 custom command」:
#   清单里的路径是生成表达式（$<TARGET_RUNTIME_DLLS>）求值出来的，configure 阶段拿不到
#   具体文件名，写不进 OUTPUT / DEPENDS。于是改成"每次构建都跑这个脚本"，由脚本自己
#   做增量判定 —— 语义上等价于 stamp，只是判定发生在脚本里而不是 ninja 的依赖图里。
#
# 两步各自的代价（实测，67 个测试目标 / 683 条清单项 / Qt6 三件套 / 237MB DLL）:
#   1) 已知 DLL 同步: 原来是"每个文件起一个 cmake 子进程"，683 次 ≈ 38s。
#      现在是去重（683 -> 12）+ file(COPY_FILE ... ONLY_IF_DIFFERENT) 分块 memcmp ≈ 0.3s。
#   2) 闭包扫描: file(GET_RUNTIME_DEPENDENCIES) 会递归解析几百个系统 DLL，
#      单独一次 ≈ 37s —— 只要每次都跑，无改动构建就永远是 70s 起步。
#      用 stamp 记住「输入签名 + 登记过的部署物」，两者都没变就整段跳过。
#
# 实测无改动构建: 71s -> 0.7s。
#
# 自愈: stamp 里记录的文件被删掉（或者签名变了：换 Qt、换 vcpkg、改搜索目录），
#   下一次构建会自动重扫并把文件补回来。删了部署物不会留下"看起来正常"的残局。
# ============================================================

if(NOT VE_DEST)
    message(FATAL_ERROR "RuntimeSync.cmake 需要 -DVE_DEST=<输出目录>")
endif()

function(_sync_log msg)
    if(NOT VE_QUIET)
        message(STATUS "${msg}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${VE_DEST}")

# 内容相同判据 —— 分两级，先便宜后准确:
#   1) 大小不同 -> 一定不同（直接覆盖）
#   2) 大小相同且 mtime 相同 -> 视为同一份。file(COPY) 会保留源时间戳，所以
#      "上次就是从这份源拷过去的"表现为 mtime 一致；升级 Qt / vcpkg 时源 mtime 必变。
#   3) 其余情况再比 SHA256（目标目录里 237MB 的 DLL 全量哈希约 1.5s，
#      而稳态构建不该为了一次都没变的文件付这个代价）。
#
# 代价取舍说明: 同一秒内被换成"同大小、不同内容"的文件会被漏判 —— 与它换来的是
# 无改动构建从 ~2.4s 降到 ~0.2s。真正的兜底在 RuntimeDepsScan.cmake（那边是精确
# 哈希），闭包补出来的那批文件始终按内容核对。
function(_sync_same_content a b out)
    file(SIZE "${a}" _sa)
    file(SIZE "${b}" _sb)
    if(NOT _sa EQUAL _sb)
        set(${out} FALSE PARENT_SCOPE)
        return()
    endif()
    file(TIMESTAMP "${a}" _ta "%Y%m%d%H%M%S" UTC)
    file(TIMESTAMP "${b}" _tb "%Y%m%d%H%M%S" UTC)
    if(_ta STREQUAL _tb)
        set(${out} TRUE PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${a}" _ha)
    file(SHA256 "${b}" _hb)
    if(_ha STREQUAL _hb)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ------------------------------------------------------------
# 1) 已知 DLL: 去重 + 按内容同步
# ------------------------------------------------------------
set(_synced 0)
set(_unchanged 0)
set(_skipped 0)
set(_new 0)
set(_present 0)

if(VE_LIST_FILE AND EXISTS "${VE_LIST_FILE}")
    file(READ "${VE_LIST_FILE}" _raw)
    string(STRIP "${_raw}" _raw)
    # 清单是分号分隔的一条字符串；$<TARGET_RUNTIME_DLLS> 展开成空的目标会留下空元素。
    if(NOT _raw STREQUAL "")
        list(REMOVE_DUPLICATES _raw)
        foreach(_src IN LISTS _raw)
            if(_src STREQUAL "" OR NOT EXISTS "${_src}")
                if(NOT _src STREQUAL "")
                    math(EXPR _skipped "${_skipped} + 1")
                endif()
                continue()
            endif()
            get_filename_component(_name "${_src}" NAME)
            set(_dst "${VE_DEST}/${_name}")
            if(EXISTS "${_dst}")
                math(EXPR _present "${_present} + 1")
            else()
                math(EXPR _new "${_new} + 1")
                _sync_log("[runtime-sync]   新增 ${_name}")
            endif()
            if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
                # COPY_FILE ONLY_IF_DIFFERENT: "比较 + 必要时写" 一次做完，比较是
                # 分块 memcmp（目标目录里 237MB 的 DLL 约 0.3s），比在 CMake 层
                # 各算一次 SHA256（约 2s）快一个量级。
                file(COPY_FILE "${_src}" "${_dst}" ONLY_IF_DIFFERENT RESULT _copy_err)
                if(_copy_err)
                    message(FATAL_ERROR "[runtime-sync] 同步失败 ${_src} -> ${_dst}: ${_copy_err}")
                endif()
            else()
                # CMake 3.23~3.25 没有 COPY_FILE，退回自己判:
                # 内容相同就不写，不同才覆盖。
                if(EXISTS "${_dst}")
                    _sync_same_content("${_src}" "${_dst}" _same)
                    if(_same)
                        math(EXPR _unchanged "${_unchanged} + 1")
                        continue()
                    endif()
                endif()
                file(COPY "${_src}" DESTINATION "${VE_DEST}")
                math(EXPR _synced "${_synced} + 1")
            endif()
        endforeach()
    endif()
else()
    message(STATUS "[runtime-sync] 清单文件不存在, 跳过已知 DLL 同步 (${VE_LIST_FILE})")
endif()

# ------------------------------------------------------------
# 2) 闭包扫描: 只在输入变化 / 上次的产物缺失时执行
# ------------------------------------------------------------
# 目标目录里现存的 DLL 全都是"部署物"：它们一起进 stamp，删掉任意一个都会触发重扫。
#
# 为什么不能只信闭包扫描自己报的清单: 扫描解析 Qt6Core.dll 的依赖时，
# Windows 会**先在 Qt6Core.dll 自己所在的目录**里找，也就是这个目标目录 ——
# 于是"已经部署好"的稳态下它报回来的是 0 项。只记这 0 项的话，用户手工删掉
# zlib1.dll 之后没有任何东西能发现，程序下次启动就 0xc0000135。
#
# 注意要在同步/扫描**之后**再列: 刚补出来的文件也得算进去。
function(_sync_collect_deployed out)
    file(GLOB _dlls "${VE_DEST}/*.dll")
    set(_names "")
    foreach(_dll IN LISTS _dlls)
        get_filename_component(_name "${_dll}" NAME)
        list(APPEND _names "${_name}")
    endforeach()
    set(${out} "${_names}" PARENT_SCOPE)
endfunction()

set(_scan_inputs "")
foreach(_in IN LISTS VE_SCAN_INPUTS)
    if(EXISTS "${_in}")
        list(APPEND _scan_inputs "${_in}")
    endif()
endforeach()

set(_sig_parts "")
foreach(_in IN LISTS _scan_inputs)
    file(SIZE "${_in}" _sz)
    file(TIMESTAMP "${_in}" _ts "%Y%m%d%H%M%S" UTC)
    string(APPEND _sig_parts "${_in}|${_sz}|${_ts};")
endforeach()
string(APPEND _sig_parts "dirs:${VE_SEARCH_DIRS}")
string(SHA256 _signature "${_sig_parts}")

set(_stamp "${VE_DEST}/.videoeye-runtime-sync.txt")
set(_header "videoeye-runtime-sync v1:${_signature}")
set(_scan_needed TRUE)
set(_stamp_files "")

if(EXISTS "${_stamp}" AND NOT VE_FORCE_SCAN)
    file(STRINGS "${_stamp}" _stamp_lines)
    list(LENGTH _stamp_lines _stamp_len)
    if(_stamp_len GREATER 0)
        list(GET _stamp_lines 0 _stamp_head)
        if(_stamp_head STREQUAL _header)
            set(_scan_needed FALSE)
            foreach(_line IN LISTS _stamp_lines)
                if(_line MATCHES "^file=(.+)$")
                    if(NOT EXISTS "${VE_DEST}/${CMAKE_MATCH_1}")
                        # 部署物被删了 —— 不能只看 stamp 还在就跳过。
                        set(_scan_needed TRUE)
                        break()
                    endif()
                    list(APPEND _stamp_files "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endif()
    endif()
endif()

if(NOT _scan_inputs)
    # 没有可扫的入口（例如纯静态构建）: 不调用扫描脚本（它对空输入是 FATAL_ERROR），
    # 只落一个空 stamp，免得每次构建都重走一遍判断。
    file(WRITE "${_stamp}" "${_header}\n")
    _sync_log("[runtime-sync] 闭包扫描: 无输入，跳过")
elseif(_scan_needed)
    if(NOT VE_SCAN_SCRIPT OR NOT EXISTS "${VE_SCAN_SCRIPT}")
        message(FATAL_ERROR "[runtime-sync] 找不到闭包扫描脚本: ${VE_SCAN_SCRIPT}")
    endif()
    set(_manifest "${_stamp}.new")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DVE_INPUTS=${_scan_inputs}"
                "-DVE_DEST=${VE_DEST}"
                "-DVE_SEARCH_DIRS=${VE_SEARCH_DIRS}"
                "-DVE_MANIFEST_OUT=${_manifest}"
                -P "${VE_SCAN_SCRIPT}"
        RESULT_VARIABLE _scan_rc
        OUTPUT_VARIABLE _scan_out
        ERROR_VARIABLE _scan_err)
    if(NOT _scan_rc EQUAL 0)
        message(FATAL_ERROR "[runtime-sync] 闭包扫描失败 (exit ${_scan_rc}):\n${_scan_out}\n${_scan_err}")
    endif()
    _sync_log("${_scan_out}")

    set(_stamp_body "${_header}\n")
    set(_manifest_files "")
    if(EXISTS "${_manifest}")
        file(STRINGS "${_manifest}" _manifest_files)
        file(REMOVE "${_manifest}")
    endif()
    _sync_collect_deployed(_deployed_names)
    set(_recorded "")
    foreach(_name IN LISTS _manifest_files _deployed_names)
        list(APPEND _recorded "${_name}")
    endforeach()
    list(REMOVE_DUPLICATES _recorded)
    foreach(_name IN LISTS _recorded)
        string(APPEND _stamp_body "file=${_name}\n")
    endforeach()
    file(WRITE "${_stamp}" "${_stamp_body}")
    list(LENGTH _recorded _closure_len)
    _sync_log("[runtime-sync] 闭包扫描: 已执行，登记 ${_closure_len} 个部署物")
else()
    list(LENGTH _stamp_files _closure_len)
    _sync_log("[runtime-sync] 闭包扫描: 输入未变化，跳过（缓存 ${_closure_len} 项）")
    # 刷新 stamp 里登记的部署物（内容不同才写）: 目标目录多出/少掉文件时，
    # 下一次构建的判断才跟得上 —— 否则"删文件"这件事永远没人发现。
    _sync_collect_deployed(_deployed_names)
    set(_stamp_body "${_header}\n")
    foreach(_name IN LISTS _deployed_names)
        string(APPEND _stamp_body "file=${_name}\n")
    endforeach()
    set(_stamp_old "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _stamp_old)
    endif()
    if(NOT _stamp_old STREQUAL _stamp_body)
        file(WRITE "${_stamp}" "${_stamp_body}")
    endif()
endif()

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
    # COPY_FILE 不回话"到底写没写"，这里只统计新增/已存在两项。
    message(STATUS
        "[runtime-sync] 已知 DLL: 新增 ${_new} / 已存在 ${_present}（内容不同时自动覆盖）-> ${VE_DEST}")
else()
    message(STATUS
        "[runtime-sync] 已知 DLL: 新增 ${_new} / 已存在 ${_present} / 写入 ${_synced} / 未变 ${_unchanged} / 缺失源 ${_skipped} -> ${VE_DEST}")
endif()
