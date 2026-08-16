# VideoEye release-only 覆盖 triplet (x64-windows-release)
# 用法: vcpkg install --triplet x64-windows-release --overlay-triplets=scripts/triplets ...
#
# 仅构建/安装 release 二进制 (VCPKG_BUILD_TYPE=release), 避免 ~2GB 的 debug 冗余,
# 安装根目录为 vcpkg_installed/x64-windows-release。
# 其余设置与官方 x64-windows.cmake 一致 (动态 CRT + 动态库链接)。
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
