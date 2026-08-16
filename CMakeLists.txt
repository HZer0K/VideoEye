cmake_minimum_required(VERSION 3.20)
project(VideoEye VERSION 2.0.0 LANGUAGES C CXX)

# 设置C++标准为C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置默认构建类型为Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译器选项 (跨平台)
if(MSVC)
    # /Z7 (调试信息嵌入 obj) 替代 /Zi (PDB), 兼容 ccache 和 Ninja 并行构建
    set(CMAKE_CXX_FLAGS_DEBUG "/Z7 /Ob0 /Od /RTC1 /DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    # Qt6 要求
    add_compile_options(/permissive- /Zc:__cplusplus /utf-8)
else()
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# 查找依赖包 - 使用 pkg-config (Windows 上 vcpkg 提供 CONFIG 包, pkg-config 非必需)
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
else()
    find_package(PkgConfig QUIET)
endif()

# ---- FFmpeg: 自带 cmake/FindFFmpeg.cmake 多来源查找 ----
# 优先级: FFMPEG_ROOT → third_party/ffmpeg-prebuilt → build*/ffmpeg_install → pkg-config
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(FFMPEG_AUTO_FETCH "Windows 下 FFmpeg 缺失时自动下载预编译包 (scripts/fetch-ffmpeg.ps1)" ON)
set(FFMPEG_VERSION "" CACHE STRING "锁定 FFmpeg 预编译版本号 (如 8.1.1); 留空则下载 latest")

find_package(FFMPEG)
if(NOT FFMPEG_FOUND AND FFMPEG_AUTO_FETCH AND WIN32)
    message(STATUS "FFmpeg not found, fetching gyan.dev prebuilt (scripts/fetch-ffmpeg.ps1)...")
    if(FFMPEG_VERSION)
        set(_ff_fetch_args -Version ${FFMPEG_VERSION})
    else()
        set(_ff_fetch_args)
    endif()
    execute_process(
        COMMAND powershell -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch-ffmpeg.ps1"
            ${_ff_fetch_args}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _ff_fetch_result
    )
    if(_ff_fetch_result EQUAL 0)
        find_package(FFMPEG)   # 重新查找 (模块会重新执行)
    else()
        message(WARNING "FFmpeg 自动获取失败 (fetch-ffmpeg.ps1 exit ${_ff_fetch_result})")
    endif()
endif()
if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR "FFmpeg not found. 运行 scripts/fetch-ffmpeg.ps1 获取预编译包，"
        "或设置 -DFFMPEG_ROOT=<path>，或通过 vcpkg/apt 安装。")
endif()
message(STATUS "FFmpeg found: ${FFMPEG_LIBRARIES}")

# Vulkan - 硬件加速和渲染
# 使用 third_party/vulkan-headers (1.4.309) 替代系统旧版头文件
set(VULKAN_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers")
if(EXISTS "${VULKAN_HEADERS_DIR}/include/vulkan/vulkan_core.h")
    set(VULKAN_CUSTOM_HEADERS ON)
    message(STATUS "Using bundled Vulkan headers: ${VULKAN_HEADERS_DIR}")
else()
    set(VULKAN_CUSTOM_HEADERS OFF)
endif()

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan library found: ${Vulkan_LIBRARY}")
else()
    message(STATUS "Vulkan library not found, building without Vulkan support")
endif()

# OpenCV (仅 core+imgproc, 项目未使用 objdetect)
# vcpkg 的 opencv 依赖 protobuf; 使用自定义 triplet (如 x64-windows-release) 时,
# vcpkg protobuf 按"交叉编译"处理导致 protobuf::protoc 目标位置为空,
# 这里预置 Protobuf_PROTOC_EXECUTABLE 指向 vcpkg 安装的 protoc 以通过其配置检查。
if(WIN32 AND CMAKE_PREFIX_PATH)
    find_program(PROTOBUF_PROTOC_EXECUTABLE protoc
        PATHS "${CMAKE_PREFIX_PATH}/tools/protobuf"
        NO_DEFAULT_PATH)
    if(PROTOBUF_PROTOC_EXECUTABLE)
        set(Protobuf_PROTOC_EXECUTABLE "${PROTOBUF_PROTOC_EXECUTABLE}" CACHE FILEPATH "protoc executable")
    endif()
endif()
find_package(OpenCV REQUIRED COMPONENTS core imgproc)

# Qt6 (仅链接实际使用的模块; QtMultimedia 已移除 — 代码中无 QMediaPlayer/QAudioOutput 使用,
# 音频输出走 SDL2, 图表走 QtCharts)
find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)

# SDL2 (CONFIG 模式, 获得 SDL2::SDL2 导入目标以便自动收集运行时 DLL; 替代 pkg-config)
find_package(SDL2 CONFIG REQUIRED)

# ZLIB (MediaInfoLib 依赖)
find_package(ZLIB REQUIRED)

# MediaInfo 第三方库 (ZenLib + MediaInfoLib 源码集成, 方便客制化)
# 源码集成第三方库 Unity Build 选项 (默认 OFF — MediaInfoLib 全局变量冲突,
# ZenLib 与 Windows min/max 宏冲突, 详见 third_party/CMakeLists.txt)
option(VIDEOEYE_UNITY_BUILD "源码集成第三方库使用 Unity Build (实验性, 默认 OFF)" OFF)

# ccache 加速 (若安装; MSVC 需 ccache >= 4.8)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache: ${CCACHE_PROGRAM}")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_BINARY_DIR}/third_party)

# Bento4 (源码集成, vcpkg 版本过旧)
set(BUILD_APPS OFF CACHE BOOL "" FORCE)
# Bento4 默认强制 /MT 静态 CRT, 与项目其余部分(/MD 动态 CRT)不匹配会导致 LNK2038,
# 这里显式关闭以保持一致
set(MSVC_FORCE_STATIC_C_RUNTIME OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/Bento4 ${CMAKE_BINARY_DIR}/bento4)
if(VIDEOEYE_UNITY_BUILD)
    set_property(TARGET ap4 PROPERTY UNITY_BUILD ON)
    set_property(TARGET ap4 PROPERTY UNITY_BUILD_BATCH_SIZE 32)
endif()
set(BENTO4_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Bento4/Source/C++)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 收集源文件 (CONFIGURE_DEPENDS: 新增/删除文件自动触发重新配置)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "core/*.cpp")
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS "ui/*.cpp")
file(GLOB_RECURSE UTILS_SOURCES CONFIGURE_DEPENDS "utils/*.cpp")

# 收集头文件
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "core/*.h" "ui/*.h" "utils/*.h")

# Qt MOC处理
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 创建可执行文件
add_executable(${PROJECT_NAME}
    ${CORE_SOURCES}
    ${UI_SOURCES}
    ${UTILS_SOURCES}
    ${HEADERS}
)

# 告警级别 (仅主项目; 第三方源码集成库不额外开告警以免噪音)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
    # 系统头文件 (vcpkg/Qt/SDK 等 <...> 包含) 不触发告警
    target_compile_options(${PROJECT_NAME} PRIVATE /external:anglebrackets /external:W0)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${FFMPEG_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${BENTO4_SOURCE_ROOT}/Core
    ${BENTO4_SOURCE_ROOT}/Codecs
    ${BENTO4_SOURCE_ROOT}/Crypto
    ${BENTO4_SOURCE_ROOT}/MetaData
)
# FFmpeg lib 搜索路径 (local build 或 pkg-config 的 -L 目录; 防止链接到系统默认路径的旧版库)
if(FFMPEG_LIB_DIR)
    target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIB_DIR})
endif()
# local build 的运行时 RPATH (构建期指向 FFmpeg bin; 安装期用 $ORIGIN 相对查找)
if(FFMPEG_LOCAL_BUILD AND FFMPEG_LOCAL_DIR)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "${FFMPEG_LOCAL_DIR}/bin"
        INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH TRUE
    )
endif()
# 优先使用 bundled Vulkan 头文件，然后才是系统路径
if(VULKAN_CUSTOM_HEADERS)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
        "${VULKAN_HEADERS_DIR}/include"
    )
elseif(Vulkan_FOUND AND Vulkan_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
        "${Vulkan_INCLUDE_DIR}"
    )
endif()

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Charts
    ${FFMPEG_LIBRARIES}
    ${OpenCV_LIBS}
    SDL2::SDL2
    mediainfo
    zen
    ap4
)
if(Vulkan_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
else()
    # 即使 find_package(Vulkan) 失败，仍可尝试直接链接 libvulkan
    find_library(VULKAN_LIB_RAW vulkan)
    if(VULKAN_LIB_RAW AND VULKAN_CUSTOM_HEADERS)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${VULKAN_LIB_RAW})
        message(STATUS "Linked Vulkan directly: ${VULKAN_LIB_RAW}")
    endif()
endif()

# 编译定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ${Qt6_DEFINITIONS}
)
if(Vulkan_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_VULKAN=1)
endif()

# Shader 编译 (Vulkan SPIR-V)
# 从系统 PATH 及常见 Vulkan SDK 路径查找 glslc (Windows/Linux 均覆盖)
find_program(GLSLC glslc
    PATHS $ENV{VULKAN_SDK}/Bin
          "C:/VulkanSDK/*/Bin"
          /opt/vulkansdk/*/bin
          /usr/local/bin
          /usr/bin
)
if(NOT GLSLC)
    message(WARNING "glslc not found - Vulkan shader compilation disabled")
    message(WARNING "Install: apt install glslc  or  download Vulkan SDK from https://vulkan.lunarg.com/")
endif()
if(GLSLC)
    set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core/player/shaders")
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

    set(SHADER_FILES
        "${SHADER_SRC_DIR}/yuv2rgb.comp"
        "${SHADER_SRC_DIR}/present.vert"
        "${SHADER_SRC_DIR}/present.frag"
    )

    foreach(SHADER_FILE ${SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
        get_filename_component(SHADER_EXT ${SHADER_FILE} EXT)
        set(OUTPUT_FILE "${SHADER_OUT_DIR}/${SHADER_NAME}${SHADER_EXT}.spv")
        add_custom_command(
            OUTPUT ${OUTPUT_FILE}
            COMMAND ${GLSLC} -O -o ${OUTPUT_FILE} ${SHADER_FILE}
            DEPENDS ${SHADER_FILE}
            COMMENT "Compiling shader ${SHADER_NAME}${SHADER_EXT}"
        )
        list(APPEND SPIRV_OUTPUTS ${OUTPUT_FILE})
    endforeach()

    add_custom_target(spirv_shaders ALL DEPENDS ${SPIRV_OUTPUTS})
    add_dependencies(${PROJECT_NAME} spirv_shaders)

    # 复制 SPIR-V 文件到运行目录
    add_custom_command(TARGET spirv_shaders POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_OUT_DIR} "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
        COMMENT "Copying SPIR-V shaders to output directory"
    )
else()
    message(STATUS "glslc not found - shader compilation disabled")
    message(STATUS "Install Vulkan SDK or glslc to enable Vulkan rendering")
endif()

# ---- 运行时部署 (Windows): DLL 收集 + Qt 插件 ----
# 策略:
#   1) TARGET_RUNTIME_DLLS 尝试精确收集链接导入目标的运行时 DLL
#   2) vcpkg bin 全量拷贝兜底: vcpkg 的 CMake config 未完整声明传递依赖
#      (如 Qt6 的 harfbuzz/libpng/double-conversion/pcre2/zlib), 需兜底
#   3) FFmpeg 为裸库名链接, 显式拷贝其预编译包 bin 下全部 DLL
#   4) windeployqt 部署 Qt 插件与附加模块
if(WIN32)
    # 1) 链接目标的运行时 DLL (精确模式, 可能不完整)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying linked runtime DLLs (Qt6/OpenCV/SDL2)"
    )

    # 2) vcpkg bin 兜底拷贝: 覆盖 TARGET_RUNTIME_DLLS 遗漏的传递依赖
    #    (harfbuzz, libpng16, double-conversion, pcre2, zlib, freetype 等)
    #    排除项目不需要的模块 DLL (PostgreSQL/protobuf/D-Bus/SQLite/Test 等)
    set(VCPKG_DLL_EXCLUDE_REGEX
        "(libpq|libecpg|libpgtypes|libodbc|libodbccp)"
        "|(libprotobuf|protobuf-lite|abseil|absl)"
        "|(dbus-1|dbus-1-3)"
        "|(sqlite3)"
        "|(Qt6Sql|Qt6Test|Qt6Concurrent|Qt6Xml|Qt6DBus|Qt6PrintSupport)"
        "|(Qt6Network|Qt6WebSockets|Qt6Mqtt|Qt6SerialPort)"
        "|(cryptography|crypt32|ncrypt)"
    )
    if(CMAKE_PREFIX_PATH AND EXISTS "${CMAKE_PREFIX_PATH}/bin")
        file(GLOB VCPKG_RUNTIME_DLLS "${CMAKE_PREFIX_PATH}/bin/*.dll")
        set(VCPKG_FILTERED_DLLS "")
        foreach(_dll ${VCPKG_RUNTIME_DLLS})
            if(NOT _dll MATCHES "${VCPKG_DLL_EXCLUDE_REGEX}")
                list(APPEND VCPKG_FILTERED_DLLS "${_dll}")
            endif()
        endforeach()
        if(VCPKG_FILTERED_DLLS)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${VCPKG_FILTERED_DLLS}
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMAND_EXPAND_LISTS
                COMMENT "Copying vcpkg runtime DLLs (filtered, excludes unused modules)"
            )
        endif()
    endif()

    # 3) FFmpeg 运行时 DLL (仅 DLL, 不含 ffmpeg.exe/ffplay.exe 等工具)
    if(FFMPEG_LOCAL_BUILD AND FFMPEG_BIN_DIR AND EXISTS "${FFMPEG_BIN_DIR}")
        file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_BIN_DIR}/*.dll")
        if(FFMPEG_RUNTIME_DLLS)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FFMPEG_RUNTIME_DLLS}
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMAND_EXPAND_LISTS
                COMMENT "Copying FFmpeg runtime DLLs"
            )
        endif()
    endif()

    # 4) Qt 插件部署
    find_program(WINDEPLOYQT windeployqt
        HINTS
            "${CMAKE_PREFIX_PATH}/tools/Qt6/bin"
            "${CMAKE_PREFIX_PATH}/Qt6/bin"
            "$ENV{QTDIR}/bin"
    )
    if(WINDEPLOYQT)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${WINDEPLOYQT}
                --no-opengl-sw --no-translations --no-system-d3d-compiler
                --no-compiler-runtime
                --dir "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Deploying Qt runtime via windeployqt"
        )
    else()
        message(WARNING "windeployqt not found - Qt 插件未自动部署")
    endif()
endif()

# 安装规则
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Windows: 安装 FFmpeg 运行时 DLL 到可执行文件同目录
if(WIN32 AND FFMPEG_LOCAL_BUILD AND EXISTS "${FFMPEG_LOCAL_DIR}/bin")
    install(DIRECTORY "${FFMPEG_LOCAL_DIR}/bin/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll")
endif()

# Windows: 安装 vcpkg 运行时 DLL (排除不需要的模块)
if(WIN32 AND CMAKE_PREFIX_PATH AND EXISTS "${CMAKE_PREFIX_PATH}/bin")
    install(DIRECTORY "${CMAKE_PREFIX_PATH}/bin/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
        REGEX ".*(libpq|libecpg|libpgtypes|libodbc|protobuf|abseil|absl|dbus|sqlite3|Qt6Sql|Qt6Test|Qt6Concurrent|Qt6Xml|Qt6DBus|Qt6PrintSupport|Qt6Network|Qt6WebSockets|Qt6Mqtt|Qt6SerialPort).*" EXCLUDE
    )
endif()

# 安装 SPIR-V shaders
if(GLSLC AND EXISTS "${CMAKE_BINARY_DIR}/shaders")
    install(DIRECTORY "${CMAKE_BINARY_DIR}/shaders/"
        DESTINATION bin/shaders
        FILES_MATCHING PATTERN "*.spv")
endif()

# 测试支持 (默认关闭, 测试文件待补全)
option(BUILD_TESTING "构建单元测试" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# 打印配置信息
message(STATUS "VideoEye Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
if(FFMPEG_LOCAL_BUILD)
    message(STATUS "  FFmpeg: local build (${FFMPEG_LOCAL_DIR})")
else()
    message(STATUS "  FFmpeg: package manager / system")
endif()
message(STATUS "  FFmpeg Include: ${FFMPEG_INCLUDE_DIRS}")
message(STATUS "  FFmpeg Libs: ${FFMPEG_LIBRARIES}")
message(STATUS "  OpenCV Version: ${OpenCV_VERSION}")
message(STATUS "  Qt Version: ${Qt6_VERSION}")
