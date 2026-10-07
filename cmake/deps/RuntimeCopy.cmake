# ============================================================
# RuntimeCopy.cmake — 把「已知 DLL 清单」拷到运行时目录
#
# 由 videoeye-runtime 目标以脚本方式调用:
#   cmake -DVE_LIST_FILE=<清单文件> -DVE_DEST=<目标目录> -P RuntimeCopy.cmake
#
# 为什么不直接 `${CMAKE_COMMAND} -E copy_if_different <dll...> <dest>`:
#   1) 分批后的每一批都含 $<TARGET_RUNTIME_DLLS:...> 这类生成表达式。静态库目标
#      （以及纯静态链接的可执行文件）展开后是**空**的，整条命令就退化成
#      `copy_if_different <dest>` —— 参数不足，cmake 直接报错，构建死在部署这一步，
#      而且报错信息只是一条 usage，看不出是哪个目标。
#   2) 不分批则命令行会随测试目标数线性膨胀，撞上 Windows CreateProcess 的
#      32767 字符上限（67 个测试目标时约 3.4 万字符）。
#   清单文件由 file(GENERATE) 在生成期写出（生成表达式此时已求值），脚本逐条
#   判空、判存在再拷：空批次自然变成"什么都不做"，命令行也只剩一个参数。
# ============================================================

if(NOT VE_LIST_FILE)
    message(FATAL_ERROR "RuntimeCopy.cmake 需要 -DVE_LIST_FILE=<清单文件>")
endif()
if(NOT VE_DEST)
    message(FATAL_ERROR "RuntimeCopy.cmake 需要 -DVE_DEST=<目标目录>")
endif()

if(NOT EXISTS "${VE_DEST}")
    file(MAKE_DIRECTORY "${VE_DEST}")
endif()

if(NOT EXISTS "${VE_LIST_FILE}")
    # 清单缺失不算致命: 闭包扫描那一步仍会补齐 Qt 依赖。
    message(STATUS "运行时部署: 清单文件不存在, 跳过已知 DLL 拷贝 (${VE_LIST_FILE})")
    return()
endif()

file(READ "${VE_LIST_FILE}" _dll_list)
string(STRIP "${_dll_list}" _dll_list)
if(_dll_list STREQUAL "")
    return()
endif()

# 清单是分号分隔的一条字符串; 空的生成表达式会留下空元素, 跳过即可。
foreach(_dll IN LISTS _dll_list)
    if(_dll AND EXISTS "${_dll}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "${VE_DEST}"
                        RESULT_VARIABLE _copy_rc)
        if(NOT _copy_rc EQUAL 0)
            message(WARNING "运行时部署: 拷贝失败 ${_dll}")
        endif()
    endif()
endforeach()
