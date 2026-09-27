# ============================================================
# FFmpegRuntimeDeploy.cmake — 把 FFmpeg 动态库放到能被找到的位置, 并校验到位
#
# 用法 (由 FFmpegPrebuilt.cmake 的 videoeye_deploy_ffmpeg() 调用,
#        安装期由 videoeye_install_ffmpeg() 生成的 install(CODE) 复用):
#   cmake -DVE_LIBS="a;b;c" -DVE_DEST=<目标目录> -P FFmpegRuntimeDeploy.cmake
#
# 为什么要做校验这一步:
#   少了 DLL / .so 时程序会在启动时才炸, 报的是 "找不到 xxx.dll" / "library
#   not found", 跟构建完全没关系 —— 排错成本极高。宁可在构建期直接失败并说清楚
#   缺什么。
# ============================================================

if(NOT VE_DEST)
    message(FATAL_ERROR "[ffmpeg-deploy] 未指定目标目录 (VE_DEST)")
endif()

if(NOT VE_LIBS)
    return()
endif()

# ------------------------------------------------------------
# 列出某个库"版本链"上的其余名字
#   libavcodec.so -> libavcodec.so.61 -> libavcodec.so.61.19.100
# 真实文件(最后那一环)由调用方拷, 这里只返回需要补建的名字。
# ------------------------------------------------------------
function(_ffmpeg_deploy_alias_names lib real_name out_var)
    get_filename_component(_dir "${lib}" DIRECTORY)
    get_filename_component(_name "${lib}" NAME)

    if(APPLE)
        string(REGEX MATCH "^[^.]+\\.dylib" _prefix "${_name}")
    else()
        string(REGEX MATCH "^[^.]+\\.so" _prefix "${_name}")
    endif()
    if(NOT _prefix)
        set(_prefix "${_name}")
    endif()

    file(GLOB _chain "${_dir}/${_prefix}*")

    set(_aliases "")
    foreach(_item IN LISTS _chain)
        get_filename_component(_item_name "${_item}" NAME)
        if(NOT _item_name STREQUAL "${real_name}")
            list(APPEND _aliases "${_item_name}")
        endif()
    endforeach()
    if(_aliases)
        list(REMOVE_DUPLICATES _aliases)
    endif()

    set(${out_var} "${_aliases}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${VE_DEST}")

set(_expected "")

foreach(_lib IN LISTS VE_LIBS)
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR
            "[ffmpeg-deploy] 源库文件不存在: ${_lib}\n"
            "FFmpeg 依赖已损坏（多半是下载/解压中途失败）。删掉目录重跑获取脚本即可。")
    endif()

    if(WIN32)
        # Windows 的 DLL 不带版本链, 平铺一份就够
        file(COPY "${_lib}" DESTINATION "${VE_DEST}")
        get_filename_component(_name "${_lib}" NAME)
        list(APPEND _expected "${_name}")
    else()
        # Linux/macOS 的版本化 .so 是一条 symlink 链, 两个坑:
        #   1) 只拷最外层  -> 目标目录里剩一个断链, 运行时照样加载失败
        #      （这就是以前必须写 FOLLOW_SYMLINK_CHAIN 的原因）
        #   2) 只拷最终文件（名字是 libavcodec.so.61.19.100）也不行 —— 可执行文件
        #      里记录的是 SONAME(libavcodec.so.61), 加载器按这个名字找, 对不上就是
        #      "libavcodec.so.61: cannot open shared object file"
        # 所以: 真实文件拷一份, 链上其余名字都建成指向它的相对 symlink。
        # （相对名 + WORKING_DIRECTORY, 整目录拷走也不会断）
        get_filename_component(_real "${_lib}" REALPATH)
        get_filename_component(_real_name "${_real}" NAME)
        file(COPY "${_real}" DESTINATION "${VE_DEST}")
        list(APPEND _expected "${_real_name}")

        _ffmpeg_deploy_alias_names("${_lib}" "${_real_name}" _aliases)
        foreach(_alias IN LISTS _aliases)
            file(REMOVE "${VE_DEST}/${_alias}")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E create_symlink "${_real_name}" "${_alias}"
                WORKING_DIRECTORY "${VE_DEST}"
                RESULT_VARIABLE _symlink_res
                ERROR_QUIET
            )
            # 只看退出码不够: create_symlink 在某些环境里"成功"了却留下一个 0 字节的
            # 空文件(退出码还是 0)。所以校验的是结果 —— 这个名字能不能解析到真实文件。
            get_filename_component(_resolved "${VE_DEST}/${_alias}" REALPATH)
            get_filename_component(_resolved_name "${_resolved}" NAME)
            if(NOT "x${_resolved_name}" STREQUAL "x${_real_name}")
                # 建不了 symlink(文件系统不支持 / 没权限): 退化成复制一份内容,
                # 多占点体积也好过装出来跑不起来。
                configure_file("${_real}" "${VE_DEST}/${_alias}" COPYONLY)
            endif()
            list(APPEND _expected "${_alias}")
        endforeach()
    endif()
endforeach()

# 校验: 名字必须都在, 且不能是断链 —— EXISTS 会跟随 symlink, 指向不存在的文件
# 时返回假, 正好把断链也一起查出来。
set(_missing "")
set(_empty "")
foreach(_name IN LISTS _expected)
    if(NOT EXISTS "${VE_DEST}/${_name}")
        list(APPEND _missing "${_name}")
    else()
        # 0 字节也是坏结果: create_symlink 在个别环境里会留下空文件还返回成功
        file(SIZE "${VE_DEST}/${_name}" _size)
        if(_size EQUAL 0)
            list(APPEND _empty "${_name}")
        endif()
    endif()
endforeach()

if(_missing OR _empty)
    message(FATAL_ERROR
        "[ffmpeg-deploy] 部署后校验失败。\n"
        "目标目录: ${VE_DEST}\n"
        "缺失文件: ${_missing}\n"
        "空文件:   ${_empty}\n"
        "FFmpeg 依赖不完整 —— 请重新获取预编译包, 或用 -DFFMPEG_ROOT 指向完整安装。")
endif()

message(STATUS "[ffmpeg-deploy] ${VE_DEST}: ${_expected}")
