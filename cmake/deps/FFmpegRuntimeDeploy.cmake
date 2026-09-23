# ============================================================
# FFmpegRuntimeDeploy.cmake — 构建后把 FFmpeg 动态库放到能被找到的位置, 并校验到位
#
# 用法 (由 FFmpegPrebuilt.cmake 的 videoeye_deploy_ffmpeg() 调用):
#   cmake -DVE_LIBS="a;b;c" -DVE_DEST=<目标目录> -P FFmpegRuntimeDeploy.cmake
#
# 为什么要做校验这一步:
#   少了 DLL 时程序会在启动时才炸, 报的是 "找不到 xxx.dll" / "library not found"，
#   跟构建完全没关系 —— 排错成本极高。宁可在构建期直接失败并说清楚缺什么。
# ============================================================

if(NOT VE_DEST)
    message(FATAL_ERROR "[ffmpeg-deploy] 未指定目标目录 (VE_DEST)")
endif()

if(NOT VE_LIBS)
    return()
endif()

file(MAKE_DIRECTORY "${VE_DEST}")

foreach(_lib IN LISTS VE_LIBS)
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR
            "[ffmpeg-deploy] 源库文件不存在: ${_lib}\n"
            "FFmpeg 依赖已损坏（多半是下载/解压中途失败）。删掉目录重跑获取脚本即可。")
    endif()

    # FOLLOW_SYMLINK_CHAIN 很重要:
    #   libavcodec.so -> libavcodec.so.61 -> libavcodec.so.61.19.100
    # 只拷最外层会得到一个断链, 运行时照样加载失败。
    if(WIN32)
        file(COPY "${_lib}" DESTINATION "${VE_DEST}")
    else()
        file(COPY "${_lib}" DESTINATION "${VE_DEST}" FOLLOW_SYMLINK_CHAIN)
    endif()
endforeach()

set(_missing "")
foreach(_lib IN LISTS VE_LIBS)
    get_filename_component(_name "${_lib}" NAME)
    if(NOT EXISTS "${VE_DEST}/${_name}")
        list(APPEND _missing "${_name}")
    endif()
endforeach()

if(_missing)
    message(FATAL_ERROR
        "[ffmpeg-deploy] 部署后校验失败。\n"
        "目标目录: ${VE_DEST}\n"
        "缺失文件: ${_missing}\n"
        "FFmpeg 依赖不完整 —— 请重新获取预编译包, 或用 -DFFMPEG_ROOT 指向完整安装。")
endif()
