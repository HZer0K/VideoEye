cmake_minimum_required(VERSION 3.23)
project(VideoEye VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Qt6 需要的 MSVC 选项
if(MSVC)
    add_compile_options(/permissive- /Zc:__cplusplus /utf-8)
endif()

# Linux/macOS 依赖 pkg-config; Windows vcpkg 提供 CONFIG
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
else()
    find_package(PkgConfig QUIET)
endif()

# ===== 依赖查找 =====
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# FFmpeg (Windows 走 FFMPEG_ROOT=third_party/ffmpeg-prebuilt; Linux 走 pkg-config)
find_package(FFMPEG REQUIRED)

# Vulkan
find_package(Vulkan QUIET)
set(VULKAN_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers")

# Qt6 + SDL2 + ZLIB
find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)
find_package(SDL2 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# ===== 第三方源码集成 =====
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_BINARY_DIR}/third_party)

# Bento4 (vcpkg 版本过旧, 源码集成)
set(BUILD_APPS OFF CACHE BOOL "" FORCE)
set(MSVC_FORCE_STATIC_C_RUNTIME OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/Bento4 ${CMAKE_BINARY_DIR}/bento4)
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

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${FFMPEG_INCLUDE_DIRS}
    ${BENTO4_SOURCE_ROOT}/Core
    ${BENTO4_SOURCE_ROOT}/Codecs
    ${BENTO4_SOURCE_ROOT}/Crypto
    ${BENTO4_SOURCE_ROOT}/MetaData
)

if(FFMPEG_LIB_DIR)
    target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIB_DIR})
endif()

# Vulkan 头文件: 优先 bundled
if(EXISTS "${VULKAN_HEADERS_DIR}/include/vulkan/vulkan_core.h")
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${VULKAN_HEADERS_DIR}/include")
elseif(Vulkan_FOUND)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${Vulkan_INCLUDE_DIR}")
endif()

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets   Qt6::Charts
    ${FFMPEG_LIBRARIES}
    SDL2::SDL2
    mediainfo zen ap4
)

if(Vulkan_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
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

# ===== Windows 运行时部署 =====
if(WIN32)
    # 1) 链接目标的 DLL (精确)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copy runtime DLLs (linked targets)"
    )

    # 2) vcpkg 全部 DLL 兜底 (无过滤 — 体积可接受, 简单可靠)
    if(CMAKE_PREFIX_PATH AND EXISTS "${CMAKE_PREFIX_PATH}/bin")
        file(GLOB VCPKG_RUNTIME_DLLS "${CMAKE_PREFIX_PATH}/bin/*.dll")
        if(VCPKG_RUNTIME_DLLS)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${VCPKG_RUNTIME_DLLS}
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMAND_EXPAND_LISTS
                COMMENT "Copy vcpkg runtime DLLs"
            )
        endif()
    endif()

    # 3) FFmpeg DLL
    if(FFMPEG_BIN_DIR AND EXISTS "${FFMPEG_BIN_DIR}")
        file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_BIN_DIR}/*.dll")
        if(FFMPEG_RUNTIME_DLLS)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FFMPEG_RUNTIME_DLLS}
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMAND_EXPAND_LISTS
                COMMENT "Copy FFmpeg DLLs"
            )
        endif()
    endif()

    # 4) windeployqt 部署 Qt 插件
    find_program(WINDEPLOYQT windeployqt HINTS
        "${CMAKE_PREFIX_PATH}/tools/Qt6/bin" "${CMAKE_PREFIX_PATH}/Qt6/bin" "$ENV{QTDIR}/bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${WINDEPLOYQT}
                --no-opengl-sw --no-translations --no-system-d3d-compiler --no-compiler-runtime
                --dir "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Deploy Qt plugins"
        )
    endif()
endif()

# ===== 安装 =====
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

if(WIN32)
    if(FFMPEG_LOCAL_DIR AND EXISTS "${FFMPEG_LOCAL_DIR}/bin")
        install(DIRECTORY "${FFMPEG_LOCAL_DIR}/bin/" DESTINATION bin FILES_MATCHING PATTERN "*.dll")
    endif()
    if(CMAKE_PREFIX_PATH AND EXISTS "${CMAKE_PREFIX_PATH}/bin")
        install(DIRECTORY "${CMAKE_PREFIX_PATH}/bin/" DESTINATION bin FILES_MATCHING PATTERN "*.dll")
    endif()
endif()

if(GLSLC AND EXISTS "${CMAKE_BINARY_DIR}/shaders")
    install(DIRECTORY "${CMAKE_BINARY_DIR}/shaders/" DESTINATION bin/shaders FILES_MATCHING PATTERN "*.spv")
endif()

# ===== 测试 =====
option(BUILD_TESTING "Build unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ===== 摘要 =====
message(STATUS "")
message(STATUS "VideoEye ${PROJECT_VERSION}  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt6:    ${Qt6_VERSION}")
if(Vulkan_FOUND)
    message(STATUS "  Vulkan: YES")
else()
    message(STATUS "  Vulkan: NO  (software rendering fallback)")
endif()
message(STATUS "")
