# ============================================================
# ThirdPartyNotices.cmake — 第三方许可证文本与声明的安装
#
# 背景（也是这个文件存在的唯一理由）:
#   VideoEye 自身是 MIT，但它链接并**随包分发** FFmpeg 与 Qt6 的二进制。MIT 只覆盖
#   我们自己的源码，不覆盖随附的第三方二进制 —— 只挂一个 MIT 徽章说"本项目是 MIT 软件"
#   对使用者是失真的。LGPL 的硬要求有三条：随附许可证文本、提供对应源码的获取方式、
#   不限制使用者替换这份库。缺任何一条，分发物就不合规。
#
#   以前这些全靠文档里的一句话交代，安装树里一个许可证文件都没有 —— 打包者要么自己
#   补（容易漏），要么直接发出一个不合规的包。这里把它变成构建产物的一部分：
#   每次 install 都会生成 doc/THIRD_PARTY_NOTICES.md 与 doc/licenses/。
#
# 输入:
#   cmake/ffmpeg-version.json  —— FFmpeg 锁定包的许可证元数据（唯一来源）
#   cmake/THIRD_PARTY_NOTICES.md.in —— 声明模板
#   third_party/licenses/      —— 仓库自带的 GPLv3 / LGPLv3 全文
#
# 用法（在 CMakeLists.txt 的安装段之后调用一次）:
#   include(cmake/deps/ThirdPartyNotices.cmake)
#   videoeye_install_third_party_notices()
# ============================================================

if(DEFINED __VIDEOEYE_THIRD_PARTY_NOTICES_INCLUDED)
    return()
endif()
set(__VIDEOEYE_THIRD_PARTY_NOTICES_INCLUDED TRUE)

set(_VE_LICENSE_TEXT_DIR "${CMAKE_SOURCE_DIR}/third_party/licenses")
set(_VE_NOTICES_TEMPLATE "${CMAKE_SOURCE_DIR}/cmake/THIRD_PARTY_NOTICES.md.in")

function(videoeye_install_third_party_notices)

# ---- 1) 读 FFmpeg 锁定包的许可证元数据 ----
# 读不到就退化成"未知"，但**不能**退化成"GPL"或"LGPL"这种猜出来的值 ——
# 猜错比写"未知"更糟：它会让分发物带着一句错误的法律声明出门。
set(_VE_FF_LICENSE_SPDX "unknown")
set(_VE_FF_LICENSE_NAME "unknown（见 cmake/ffmpeg-version.json 的 license 段）")
set(_VE_FF_SOURCE_URL "https://ffmpeg.org/download.html")
set(_VE_FF_LICENSE_FILE_NAME "LICENSE.txt")
set(_VE_FF_BUILD_ID "见 cmake/ffmpeg-version.json")

set(_ve_ff_cfg "${CMAKE_SOURCE_DIR}/cmake/ffmpeg-version.json")
if(EXISTS "${_ve_ff_cfg}")
    file(READ "${_ve_ff_cfg}" _ve_ff_json)
    string(JSON _ve_ff_spdx ERROR_VARIABLE _ve_err GET "${_ve_ff_json}" license spdx)
    if(NOT _ve_err)
        set(_VE_FF_LICENSE_SPDX "${_ve_ff_spdx}")
    endif()
    string(JSON _ve_ff_name ERROR_VARIABLE _ve_err GET "${_ve_ff_json}" license name)
    if(NOT _ve_err)
        set(_VE_FF_LICENSE_NAME "${_ve_ff_name}")
    endif()
    string(JSON _ve_ff_src ERROR_VARIABLE _ve_err GET "${_ve_ff_json}" license sourceUrl)
    if(NOT _ve_err)
        set(_VE_FF_SOURCE_URL "${_ve_ff_src}")
    endif()
    string(JSON _ve_ff_lf ERROR_VARIABLE _ve_err GET "${_ve_ff_json}" license licenseFileName)
    if(NOT _ve_err)
        set(_VE_FF_LICENSE_FILE_NAME "${_ve_ff_lf}")
    endif()
    # 构建标识是 LGPL「对应源码」这一条的关键：只有写上 release tag 与源码 commit，
    # 拿到分发物的人才能定位到这一份二进制是由哪份源码、哪次构建产出的。
    string(JSON _ve_tag ERROR_VARIABLE _ve_err GET "${_ve_ff_json}" windows releaseTag)
    string(JSON _ve_build ERROR_VARIABLE _ve_err2 GET "${_ve_ff_json}" windows buildId)
    if(NOT _ve_err AND NOT _ve_err2)
        set(_VE_FF_BUILD_ID "${_ve_build}（release tag ${_ve_tag}）")
    elseif(NOT _ve_err)
        set(_VE_FF_BUILD_ID "release tag ${_ve_tag}")
    elseif(NOT _ve_err2)
        set(_VE_FF_BUILD_ID "${_ve_build}")
    else()
        set(_VE_FF_BUILD_ID "见 cmake/ffmpeg-version.json")
    endif()
    # 锁定版本号：stamp 缺失时用它兜底（FFMPEG_VERSION 只有 pkg-config 那一路有值）
    string(JSON _ve_ver ERROR_VARIABLE _ve_err GET "${_ve_ff_json}" version)
    if(NOT _ve_err)
        set(_VE_FF_LOCKED_VERSION "${_ve_ver}")
    endif()
endif()

# ---- 2) 描述本次构建实际用的 FFmpeg ----
# prebuilt 与 system 两条路的结论完全不同：前者我们知道确切的许可证，后者取决于
# 使用者的发行版怎么编的 —— 声明里必须如实说明，不能拿锁定包的许可证去套系统库。
set(FFMPEG_NOTICE_VERSION "unknown")
set(FFMPEG_NOTICE_SOURCE "unknown")
set(FFMPEG_NOTICE_LICENSE "${_VE_FF_LICENSE_SPDX}")
set(FFMPEG_NOTICE_LICENSE_FILE "doc/licenses/（见下）")
set(FFMPEG_NOTICE_SOURCE_URL "${_VE_FF_SOURCE_URL}")
set(FFMPEG_NOTICE_LINKAGE "动态库（av*.dll / libav*.so / libav*.dylib）")
set(FFMPEG_NOTICE_BUILD_ID "${_VE_FF_BUILD_ID}")
set(FFMPEG_NOTICE_EXTRA "")

if(FFMPEG_SOURCE STREQUAL "prebuilt")
    set(FFMPEG_NOTICE_SOURCE "预编译包 ${FFMPEG_PREBUILT_DIR}")
    # 预编译包没地方写版本号（pkg-config 才有），用目录里的 stamp 兜底
    if(EXISTS "${FFMPEG_PREBUILT_DIR}/.videoeye-ffmpeg.json")
        file(READ "${FFMPEG_PREBUILT_DIR}/.videoeye-ffmpeg.json" _ve_stamp)
        string(JSON _ve_stamp_ver ERROR_VARIABLE _ve_err GET "${_ve_stamp}" FFmpegVersion)
        if(NOT _ve_err AND _ve_stamp_ver)
            set(FFMPEG_NOTICE_VERSION "${_ve_stamp_ver}")
        endif()
    endif()
    if(FFMPEG_NOTICE_VERSION STREQUAL "unknown")
        if(FFMPEG_VERSION)
            set(FFMPEG_NOTICE_VERSION "见 cmake/ffmpeg-version.json（版本 ${FFMPEG_VERSION}）")
        elseif(_VE_FF_LOCKED_VERSION)
            set(FFMPEG_NOTICE_VERSION
                "${_VE_FF_LOCKED_VERSION}（libavcodec ${FFMPEG_LIBAVCODEC_VERSION}）")
        else()
            set(FFMPEG_NOTICE_VERSION "libavcodec ${FFMPEG_LIBAVCODEC_VERSION}")
        endif()
    endif()

    # 包里的许可证文本（LGPLv3 / GPLv3 全文），缺了它就是不合规
    set(_ve_ff_lic "")
    foreach(_cand "${_VE_FF_LICENSE_FILE_NAME}" "LICENSE.txt" "LICENSE" "COPYING.txt" "COPYING")
        if(EXISTS "${FFMPEG_PREBUILT_DIR}/${_cand}")
            set(_ve_ff_lic "${FFMPEG_PREBUILT_DIR}/${_cand}")
            break()
        endif()
    endforeach()
    if(_ve_ff_lic)
        install(FILES "${_ve_ff_lic}"
                DESTINATION doc/licenses
                RENAME "FFmpeg-${_VE_FF_LICENSE_SPDX}.txt")
        set(FFMPEG_NOTICE_LICENSE_FILE "doc/licenses/FFmpeg-${_VE_FF_LICENSE_SPDX}.txt")
    else()
        # 缺文件不该静默过去：装出来的包会少一份必备文件，而构建一切正常。
        message(WARNING
            "FFmpeg 预编译包里没有许可证文件（找过 LICENSE.txt/LICENSE/COPYING*）: ${FFMPEG_PREBUILT_DIR}\n"
            "分发 FFmpeg 二进制必须随附其许可证文本。请确认 scripts/fetch-ffmpeg.ps1 的清理步骤没有把它删掉。")
        set(FFMPEG_NOTICE_LICENSE_FILE "缺失！请检查 FFmpeg 预编译包")
    endif()
else()
    set(FFMPEG_NOTICE_VERSION "${FFMPEG_VERSION}")
    set(FFMPEG_NOTICE_SOURCE "系统 FFmpeg（pkg-config 定位）")
    set(FFMPEG_NOTICE_BUILD_ID "无（锁定包只用于 Windows；这份由系统包管理器提供）")
    set(FFMPEG_NOTICE_LICENSE
        "取决于发行版/包管理器的构建配置（常见为 GPL-3.0-or-later 或 LGPL-3.0-or-later）")
    set(FFMPEG_NOTICE_LICENSE_FILE "由系统包管理器提供（如 /usr/share/doc/ffmpeg/copyright）")
    string(CONCAT FFMPEG_NOTICE_EXTRA
        "> **注意**：本构建使用的是系统 FFmpeg，不是 `cmake/ffmpeg-version.json` 里锁定的那个包。\n"
        "> 上面锁定包的许可证（${_VE_FF_LICENSE_SPDX}）**不能**代表系统里这一份。\n"
        "> 发行版与 Homebrew 的 ffmpeg 多数启用了 `--enable-gpl`（含 libx264/libx265），\n"
        "> 需要 LGPL-clean 的分发物时请改用 LGPL 变体或自行静态链接，并在分发前复核。\n")
endif()

# ---- 3) Qt ----
set(QT_NOTICE_VERSION "${Qt6_VERSION}")
set(QT_NOTICE_LICENSE_FILE "doc/licenses/Qt6-copyright.txt")

# Qt 的许可证文本在 vcpkg 布局下是 <prefix>/share/qtbase/copyright。
# Qt6_DIR 形如 <prefix>/lib/cmake/Qt6 —— 往上三级就是 prefix。
set(_ve_qt_prefix "")
if(Qt6_DIR)
    get_filename_component(_ve_qt_prefix "${Qt6_DIR}" DIRECTORY)   # .../lib/cmake
    get_filename_component(_ve_qt_prefix "${_ve_qt_prefix}" DIRECTORY)  # .../lib
    get_filename_component(_ve_qt_prefix "${_ve_qt_prefix}" DIRECTORY)  # <prefix>
endif()

set(_ve_qt_copyright "")
foreach(_cand
    "${_ve_qt_prefix}/share/qtbase/copyright"
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/qtbase/copyright"
    "${_ve_qt_prefix}/LICENSE.LGPL3"
)
    if(EXISTS "${_cand}")
        set(_ve_qt_copyright "${_cand}")
        break()
    endif()
endforeach()
if(NOT _ve_qt_copyright)
    file(GLOB _ve_qt_glob "${CMAKE_SOURCE_DIR}/vcpkg_installed/*/share/qtbase/copyright")
    if(_ve_qt_glob)
        list(GET _ve_qt_glob 0 _ve_qt_copyright)
    endif()
endif()

if(_ve_qt_copyright)
    install(FILES "${_ve_qt_copyright}" DESTINATION doc/licenses RENAME "Qt6-copyright.txt")
else()
    set(QT_NOTICE_LICENSE_FILE "缺失！请检查 Qt 安装（vcpkg 布局下应为 <prefix>/share/qtbase/copyright）")
    message(WARNING
        "未找到 Qt6 的许可证文本（share/qtbase/copyright）。\n"
        "分发 Qt 二进制需要随附其许可证；请确认 Qt 安装完整。")
endif()

# ---- 4) 仓库自带的 GPLv3 / LGPLv3 全文 ----
# LGPLv3 是 GPLv3 的"附加许可"，只给 LGPLv3 文本等于只给了一半 —— 两份都要随附。
# 这两个文件不依赖任何下载，任何构建机上都有，所以无条件安装。
foreach(_lic GPL-3.0.txt LGPL-3.0.txt)
    if(EXISTS "${_VE_LICENSE_TEXT_DIR}/${_lic}")
        install(FILES "${_VE_LICENSE_TEXT_DIR}/${_lic}" DESTINATION doc/licenses)
    else()
        message(WARNING "缺少许可证全文 third_party/licenses/${_lic}，安装树会不完整。")
    endif()
endforeach()

# ---- 5) 生成并安装 THIRD_PARTY_NOTICES.md ----
set(VIDEOEYE_NOTICE_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT VIDEOEYE_NOTICE_BUILD_TYPE)
    set(VIDEOEYE_NOTICE_BUILD_TYPE "未指定")
endif()
if(WIN32)
    set(VIDEOEYE_NOTICE_PLATFORM "Windows (${CMAKE_SYSTEM_PROCESSOR})")
elseif(APPLE)
    set(VIDEOEYE_NOTICE_PLATFORM "macOS (${CMAKE_SYSTEM_PROCESSOR})")
else()
    set(VIDEOEYE_NOTICE_PLATFORM "Linux (${CMAKE_SYSTEM_PROCESSOR})")
endif()
string(TIMESTAMP VIDEOEYE_NOTICE_TIMESTAMP "%Y-%m-%d %H:%M:%S")

if(EXISTS "${_VE_NOTICES_TEMPLATE}")
    configure_file("${_VE_NOTICES_TEMPLATE}"
                   "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.md" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.md" DESTINATION doc)
else()
    message(WARNING "缺少声明模板 ${_VE_NOTICES_TEMPLATE}，安装树里不会有 THIRD_PARTY_NOTICES.md。")
endif()

# 本项目的 MIT 正文也进安装树 —— 分发物里至少要能找到自己那份许可证
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION doc/licenses RENAME "VideoEye-MIT.txt")
endif()

endfunction()
