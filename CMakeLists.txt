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
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Ob0 /Od /RTC1 /DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    # Qt6 要求
    add_compile_options(/permissive- /Zc:__cplusplus /utf-8)
else()
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# 添加自定义 CMake 模块路径
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 查找依赖包 - 使用 pkg-config
find_package(PkgConfig REQUIRED)

# FFmpeg - 多来源查找策略
# 优先级: 1) vcpkg/apt 包管理器  2) 已有源码编译产物  3) 系统 pkg-config (Linux)
find_package(FFMPEG QUIET)
if(NOT FFMPEG_FOUND)
    # 检查已有的源码编译产物 (Windows: build-ninja/ffmpeg_install, Linux: build/ffmpeg_install)
    set(FFMPEG_LOCAL_DIR "${CMAKE_BINARY_DIR}/ffmpeg_install")
    if(NOT EXISTS "${FFMPEG_LOCAL_DIR}/include/libavcodec/avcodec.h")
        # 尝试从源码目录查找
        set(FFMPEG_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-ninja/ffmpeg_install")
    endif()
    if(NOT EXISTS "${FFMPEG_LOCAL_DIR}/include/libavcodec/avcodec.h")
        set(FFMPEG_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-Release/ffmpeg_install")
    endif()
    if(NOT EXISTS "${FFMPEG_LOCAL_DIR}/include/libavcodec/avcodec.h")
        set(FFMPEG_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg_install")
    endif()

    if(EXISTS "${FFMPEG_LOCAL_DIR}/include/libavcodec/avcodec.h")
        # 使用已有源码编译产物
        set(FFMPEG_INCLUDE_DIRS "${FFMPEG_LOCAL_DIR}/include")
        if(WIN32)
            set(FFMPEG_LIBRARIES avcodec avformat avutil swscale swresample)
            set(FFMPEG_LIB_DIR "${FFMPEG_LOCAL_DIR}/lib")
        else()
            set(FFMPEG_LIBRARIES avcodec avformat avutil swscale swresample)
            set(FFMPEG_LIB_DIR "${FFMPEG_LOCAL_DIR}/lib")
        endif()
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_LOCAL_BUILD ON)
        message(STATUS "FFmpeg: using local build artifacts at ${FFMPEG_LOCAL_DIR}")
    elseif(PkgConfig_FOUND)
        # Linux fallback: 通过 pkg-config 查找
        pkg_check_modules(FFMPEG REQUIRED
            libavcodec libavformat libavutil libswscale libswresample)
        message(STATUS "FFmpeg: using system pkg-config")
    else()
        message(FATAL_ERROR "FFmpeg not found. Install via vcpkg, apt, or build from source.")
    endif()
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

# OpenCV
find_package(OpenCV REQUIRED COMPONENTS core imgproc objdetect)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Charts)

# SDL2
pkg_check_modules(SDL2 REQUIRED sdl2)

# ZLIB (MediaInfoLib 依赖)
find_package(ZLIB REQUIRED)

# MediaInfo 第三方库 (ZenLib + MediaInfoLib 源码集成, 方便客制化)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_BINARY_DIR}/third_party)

# Bento4 (源码集成, vcpkg 版本过旧)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/Bento4 ${CMAKE_BINARY_DIR}/bento4)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 收集源文件
file(GLOB_RECURSE CORE_SOURCES "core/*.cpp")
file(GLOB_RECURSE UI_SOURCES "ui/*.cpp")
file(GLOB_RECURSE UTILS_SOURCES "utils/*.cpp")

# 收集头文件
file(GLOB_RECURSE HEADERS "core/*.h" "ui/*.h" "utils/*.h")

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

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${FFMPEG_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)
# FFmpeg local build 需要额外的 lib 搜索路径
if(FFMPEG_LOCAL_BUILD AND FFMPEG_LIB_DIR)
    target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIB_DIR})
    # 运行时 RPATH，确保能找到 FFmpeg DLL/.so
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "${FFMPEG_LOCAL_DIR}/bin"
        INSTALL_RPATH "${FFMPEG_LOCAL_DIR}/bin"
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
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Charts
    ${FFMPEG_LIBRARIES}
    ${OpenCV_LIBS}
    ${SDL2_LIBRARIES}
    mediainfo
    zen
    bento4
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
# 从系统 PATH 及常见 Vulkan SDK 路径查找 glslc
find_program(GLSLC glslc
    PATHS /opt/vulkansdk/*/bin
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

# 安装规则
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# 测试支持
option(BUILD_TESTING "构建单元测试" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# 复制资源文件
install(DIRECTORY resources/
    DESTINATION share/VideoEye/resources
)

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
