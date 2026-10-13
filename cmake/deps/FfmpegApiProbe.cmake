# ============================================================
# FfmpegApiProbe.cmake — FFmpeg 版本识别 + 编译期 API 门禁
#
# 为什么要这个模块:
#   项目代码用到了一批**只在新版 FFmpeg 里存在**的符号:
#     * AV_FRAME_FLAG_KEY           (libavutil 59.39 / FFmpeg 7.1 起)
#     * av_packet_side_data_get()   (libavcodec 61 / FFmpeg 7.0 起)
#     * AV_CODEC_ID_VVC             (libavcodec 61 / FFmpeg 7.0 起)
#   Windows 走锁定好的预编译包, 版本永远够; 但 Linux/macOS 用的是**系统**
#   FFmpeg, 版本由发行版决定 —— Ubuntu 24.04 是 6.1, Debian 12 是 5.1,
#   Homebrew 则是滚动的新版。同一份代码在不同机器上会出现"这边编得过、
#   那边编不过", 而报错形态是一堆看不懂的模板/宏展开错误。
#
#   所以这里不猜"市场版本号", 而是直接探测**代码真正依赖的那几个符号**在不在。
#   缺哪个就说缺哪个、需要哪个 FFmpeg 版本 —— 比 "FFmpeg too old" 有用得多。
#
# 检测方式:
#   只编译、不链接 (CMAKE_TRY_COMPILE_TARGET_TYPE = STATIC_LIBRARY)。
#   原因: 探测阶段还没建 FFmpeg:: 的 imported target, 而且探测的是宏与函数声明,
#   不需要解析符号 —— 强行链接只会让结果取决于"这台机器能不能链到 FFmpeg",
#   而那不是我们要问的问题。
# ============================================================

include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

# 从 libavcodec/version.h 里读出 MAJOR/MINOR/MICRO, 得到 "62.28.103" 这样的三元组。
# 读不到返回空串 —— 调用方要能容忍空值(只在摘要里少显示一项, 不该因此失败)。
function(videoeye_ffmpeg_read_libavcodec_version root out_triple)
    # ⚠️ FFmpeg 7 之后把 MAJOR 拆到了同目录的 version_major.h（version.h 里只剩
    # MINOR/MICRO）。只读 version.h 会得到空的主版本号 —— 两个文件都要看。
    set(_lines "")
    set(_found_header FALSE)
    foreach(_header IN ITEMS
            "${root}/include/libavcodec/version.h"
            "${root}/include/libavcodec/version_major.h"
            "${root}/libavcodec/version.h"
            "${root}/libavcodec/version_major.h")
        if(NOT EXISTS "${_header}")
            continue()
        endif()
        set(_found_header TRUE)
        file(STRINGS "${_header}" _part
             REGEX "^#[ \t]*define[ \t]+LIBAVCODEC_VERSION_(MAJOR|MINOR|MICRO)")
        list(APPEND _lines ${_part})
    endforeach()
    if(NOT _found_header)
        set(${out_triple} "" PARENT_SCOPE)
        return()
    endif()

    set(_maj "")
    set(_min "")
    set(_mic "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "LIBAVCODEC_VERSION_MAJOR[ \t]+([0-9]+)")
            set(_maj "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "LIBAVCODEC_VERSION_MINOR[ \t]+([0-9]+)")
            set(_min "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "LIBAVCODEC_VERSION_MICRO[ \t]+([0-9]+)")
            set(_mic "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    if(_maj STREQUAL "")
        set(${out_triple} "" PARENT_SCOPE)
        return()
    endif()
    if(_min STREQUAL "")
        set(_min "0")
    endif()
    if(_mic STREQUAL "")
        set(_mic "0")
    endif()
    set(${out_triple} "${_maj}.${_min}.${_mic}" PARENT_SCOPE)
endfunction()

# 在给定 include 目录集合里定位 libavcodec/version.h 并解析版本。
# 系统 FFmpeg 的 pkg-config 只给"市场版本号"(如 7.1), 要做跨发行版比较还是得
# 落到 libavcodec 的三元组上。
function(videoeye_ffmpeg_detect_libavcodec_version include_dirs out_triple)
    foreach(_dir IN LISTS include_dirs)
        videoeye_ffmpeg_read_libavcodec_version("${_dir}" _t)
        if(NOT _t STREQUAL "")
            set(${out_triple} "${_t}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_triple} "" PARENT_SCOPE)
endfunction()

# libavcodec 主版本号 → FFmpeg 发布系列。只用于给人看的提示文案,
# 不参与任何判定(判定全部走符号探测)。
function(videoeye_ffmpeg_series_from_major major out_series)
    if(major GREATER_EQUAL 62)
        set(_s "FFmpeg 8.x")
    elseif(major EQUAL 61)
        set(_s "FFmpeg 7.x")
    elseif(major EQUAL 60)
        set(_s "FFmpeg 6.x")
    elseif(major EQUAL 59)
        set(_s "FFmpeg 5.x")
    elseif(major EQUAL 58)
        set(_s "FFmpeg 4.x")
    else()
        set(_s "FFmpeg (libavcodec ${major}.x)")
    endif()
    set(${out_series} "${_s}" PARENT_SCOPE)
endfunction()

# 编译期探测单个 C++ 片段。只编译不链接。
#
# ⚠️ check_cxx_source_compiles 的结果存进**缓存变量**：三条探测若共用一个名字,
# 第二条起会直接复用第一条的结果(全部 TRUE), 门禁形同虚设。所以调用方必须
# 给每条探测一个独立的 cache_var 名字, 并且在探测前 unset —— 换了 FFMPEG_ROOT
# 之后缓存里的旧结论会一直生效, 那正是这个门禁要防的事。
function(_ve_ffmpeg_try_compile snippet cache_var out_ok)
    unset(${cache_var} CACHE)
    set(_saved_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(_saved_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(_saved_quiet "${CMAKE_REQUIRED_QUIET}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
    set(CMAKE_REQUIRED_INCLUDES "${FFMPEG_INCLUDE_DIRS}")
    set(CMAKE_REQUIRED_QUIET TRUE)
    check_cxx_source_compiles("${snippet}" ${cache_var})
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_saved_type}")
    set(CMAKE_REQUIRED_INCLUDES "${_saved_includes}")
    set(CMAKE_REQUIRED_QUIET "${_saved_quiet}")
    set(${out_ok} ${${cache_var}} PARENT_SCOPE)
endfunction()

# 需要的符号清单: 名字 / 探测片段 / 缺失时的说明(给用户的修复指引)
# 顺序按"最可能缺"排 —— 老发行版上 AV_FRAME_FLAG_KEY 通常是第一个炸的。
function(videoeye_ffmpeg_required_api out_names_out out_hints)
    set(_names "")
    set(_hints "")

    list(APPEND _names "AV_FRAME_FLAG_KEY")
    list(APPEND _hints
"AV_FRAME_FLAG_KEY  —— 关键帧标记位（libavutil 59.39 / FFmpeg 7.1 起）。
    旧版本只有 AVFrame::key_frame 字段; VideoEye 按新语义读取 flags。")

    list(APPEND _names "av_packet_side_data_get")
    list(APPEND _hints
"av_packet_side_data_get()  —— 按类型取 packet side data（libavcodec 61 / FFmpeg 7.0 起）。
    HDR/色彩元数据的读取路径依赖它。")

    list(APPEND _names "AV_CODEC_ID_VVC")
    list(APPEND _hints
"AV_CODEC_ID_VVC  —— VVC/H.266 编码标识（libavcodec 61 / FFmpeg 7.0 起）。
    缺了它 VVC 分支无法编译。")

    set(${out_names_out} "${_names}" PARENT_SCOPE)
    set(${out_hints} "${_hints}" PARENT_SCOPE)
endfunction()

# ============================================================
# 门禁入口
# ============================================================
# 在 FFMPEG_INCLUDE_DIRS / FFMPEG_SOURCE / FFMPEG_VERSION 都定下来之后调用。
#
# 行为:
#   * 全部符号都在  -> STATUS 一行, 顺带打印 libavcodec 三元组与来源
#   * 缺任意一个    -> FATAL_ERROR, 列出缺的符号、需要的最低版本、以及
#                      在 Linux/macOS 上怎么换一份够新的 FFmpeg
#   * -DVIDEOEYE_FFMPEG_SKIP_API_CHECK=ON 可跳过(仅限明确知道自己在做什么的人,
#     例如给还不支持的发行版做移植时的临时开关)
function(videoeye_check_ffmpeg_api)
    if(VIDEOEYE_FFMPEG_SKIP_API_CHECK)
        message(WARNING
            "[ffmpeg] 已跳过 API 门禁 (-DVIDEOEYE_FFMPEG_SKIP_API_CHECK=ON)。\n"
            "  当前这份 FFmpeg 未经验证, 编译可能以难以理解的方式失败。")
        return()
    endif()

    if(NOT FFMPEG_INCLUDE_DIRS)
        message(FATAL_ERROR
            "[ffmpeg] videoeye_check_ffmpeg_api() 在 FFMPEG_INCLUDE_DIRS 确定之前被调用了。")
    endif()

    # 探测片段: FFmpeg 头必须包在 extern "C" 里（随包预编译头的 include/ 里
    # 一处 extern "C" 都没有, 不包的话 MSVC 按 C++ 修饰找符号, 编译能过、
    # 链接期爆一堆 LNK2019）。
    #
    # __STDC_CONSTANT_MACROS 不能省: libavutil/common.h 里有一句
    #   #error missing -D__STDC_CONSTANT_MACROS
    # 只有先 include 到 <inttypes.h> 的路径才碰巧躲过它 —— 单独 include
    # codec_id.h 这类"轻"头时会直接编译失败, 看起来像"这个符号不存在"。
    set(_snippet_key "
#define __STDC_CONSTANT_MACROS
extern \"C\" {
#include <libavutil/frame.h>
}
int main() { const int f = AV_FRAME_FLAG_KEY; return f ? 0 : 1; }
")
    set(_snippet_side_data "
#define __STDC_CONSTANT_MACROS
extern \"C\" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
}
int main() {
    const AVPacketSideData* sd = nullptr;
    const AVPacketSideData* got = av_packet_side_data_get(sd, 0, AV_PKT_DATA_STRINGS_METADATA);
    return got == nullptr ? 0 : 1;
}
")
    set(_snippet_vvc "
#define __STDC_CONSTANT_MACROS
extern \"C\" {
#include <libavcodec/codec_id.h>
}
int main() { const enum AVCodecID id = AV_CODEC_ID_VVC; return id == AV_CODEC_ID_NONE ? 1 : 0; }
")

    set(_missing "")
    set(_missing_hints "")

    _ve_ffmpeg_try_compile("${_snippet_key}" VE_FFMPEG_HAVE_FRAME_FLAG_KEY _have_key)
    if(NOT _have_key)
        list(APPEND _missing "AV_FRAME_FLAG_KEY")
        list(APPEND _missing_hints
             "AV_FRAME_FLAG_KEY  —— 关键帧标记位（libavutil 59.39 / FFmpeg 7.1 起）")
    endif()

    _ve_ffmpeg_try_compile("${_snippet_side_data}" VE_FFMPEG_HAVE_PACKET_SIDE_DATA_GET _have_sd)
    if(NOT _have_sd)
        list(APPEND _missing "av_packet_side_data_get")
        list(APPEND _missing_hints
             "av_packet_side_data_get()  —— packet side data 查询（libavcodec 61 / FFmpeg 7.0 起）")
    endif()

    _ve_ffmpeg_try_compile("${_snippet_vvc}" VE_FFMPEG_HAVE_CODEC_ID_VVC _have_vvc)
    if(NOT _have_vvc)
        list(APPEND _missing "AV_CODEC_ID_VVC")
        list(APPEND _missing_hints
             "AV_CODEC_ID_VVC  —— VVC/H.266 编码标识（libavcodec 61 / FFmpeg 7.0 起）")
    endif()

    if(_missing)
        string(REPLACE ";" "\n  · " _missing_text "${_missing}")
        string(REPLACE ";" "\n  · " _hint_text "${_missing_hints}")
        set(_origin "")
        if(FFMPEG_SOURCE STREQUAL "pkg-config")
            set(_origin "系统 FFmpeg（pkg-config 定位, 版本 ${FFMPEG_VERSION}）")
        else()
            set(_origin "预编译包 ${FFMPEG_LOCAL_DIR}")
        endif()
        string(REPLACE ";" " " _incdirs "${FFMPEG_INCLUDE_DIRS}")
        message(FATAL_ERROR
"FFmpeg 缺少 VideoEye 需要的 API（这是硬性要求, 不是警告）:

  ${_origin}
  头文件目录: ${_incdirs}
  libavcodec: ${FFMPEG_LIBAVCODEC_VERSION}

  缺少:
  · ${_missing_text}

  各符号的最低版本:
  · ${_hint_text}

  实测结论: VideoEye 编译期要求 **FFmpeg 7.1 及以上**（libavcodec >= 61.19）。
  发行版自带的 FFmpeg 常常更老, 例如 Ubuntu 24.04 是 6.1、Debian 12 是 5.1。

  换一份够新的 FFmpeg（任选其一）:
    · 给一个自己准备的 FFmpeg 开发包: cmake -DFFMPEG_ROOT=/path/to/ffmpeg ...
      （要求 include/libavcodec/version.h + lib/, Windows 还要 bin/*.dll）
    · 让系统 FFmpeg 走别的源（Ubuntu 可用 deadsnakes 之外的第三方源 / 自行编译
      并 --prefix 到 /opt/ffmpeg, 再配合 -DFFMPEG_ROOT=/opt/ffmpeg）
    · 明确知道自己在做什么时临时跳过: -DVIDEOEYE_FFMPEG_SKIP_API_CHECK=ON

  支持矩阵见 CONTRIBUTING.md 的「FFmpeg 支持矩阵」一节。")
    endif()

    set(_series "")
    if(FFMPEG_LIBAVCODEC_VERSION MATCHES "^([0-9]+)\\.")
        videoeye_ffmpeg_series_from_major("${CMAKE_MATCH_1}" _series)
    endif()
    message(STATUS
        "FFmpeg API 门禁: 通过（${_series}, libavcodec ${FFMPEG_LIBAVCODEC_VERSION}, 来源 ${FFMPEG_SOURCE}）")
endfunction()
