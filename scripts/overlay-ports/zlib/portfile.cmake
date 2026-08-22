# overlay port: 基于 vcpkg baseline (fd22bbac) 的官方 zlib 1.3.1 port,
# 手动安装 zlib.pc 但不调用 vcpkg_fixup_pkgconfig()。
# 原因: vcpkg_fixup_pkgconfig 会触发 vcpkg_acquire_msys 下载固定版本的
# MSYS2 包 (pkgconf), 而 MSYS2 滚动镜像会删除旧包 → 下载 404 导致构建失败。
# 但 zlib.pc 仍需存在: 下游包 (如 libpng) 的 portfile 调用
# vcpkg_fixup_pkgconfig() 检查 pkg-config --exists libpng16,
# libpng16.pc 依赖 zlib.pc → 缺失则构建失败。
# 解决: 从 CMake 构建树手动复制 zlib.pc 到 lib/pkgconfig/,
# 路径由 CMAKE_INSTALL_PREFIX (= CURRENT_PACKAGES_DIR) 自动生成, 无需 fixup。
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO madler/zlib
    REF v${VERSION}
    SHA512 8c9642495bafd6fad4ab9fb67f09b268c69ff9af0f4f20cf15dfc18852ff1f312bd8ca41de761b3f8d8e90e77d79f2ccacd3d4c5b19e475ecf09d021fdfe9088
    HEAD_REF master
    PATCHES
        0001-Prevent-invalid-inclusions-when-HAVE_-is-set-to-0.patch
        0002-build-static-or-shared-not-both.patch
        0003-android-and-mingw-fixes.patch
)

# This is generated during the cmake build
file(REMOVE "${SOURCE_PATH}/zconf.h")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSKIP_INSTALL_FILES=ON
        -DZLIB_BUILD_EXAMPLES=OFF
    OPTIONS_DEBUG
        -DSKIP_INSTALL_HEADERS=ON
)

vcpkg_cmake_install()
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_copy_pdbs()

# 手动安装 zlib.pc (SKIP_INSTALL_FILES=ON 阻止了 CMake 自动安装)
# 下游包 (libpng, freetype 等) 的 vcpkg_fixup_pkgconfig 需要 zlib.pc
set(_zlib_pc "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/zlib.pc")
if(EXISTS "${_zlib_pc}")
    file(INSTALL "${_zlib_pc}" DESTINATION "${CURRENT_PACKAGES_DIR}/lib/pkgconfig")
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/zconf.h" "ifdef ZLIB_DLL" "if 0")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/zconf.h" "ifdef ZLIB_DLL" "if 1")
endif()

file(COPY "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
