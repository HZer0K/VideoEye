cmake_minimum_required(VERSION 3.20)
project(VideoEye VERSION 2.0.0 LANGUAGES CXX)

# 设置C++标准为C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置默认构建类型为Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译器选项
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# 查找依赖包
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
find_package(OpenCV REQUIRED COMPONENTS core imgproc objdetect)
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Charts)
find_package(SDL2 REQUIRED)

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
    resources/resources.qrc
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

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Charts
    ${FFMPEG_LIBRARIES}
    ${OpenCV_LIBS}
    SDL2::SDL2
)

# 编译定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ${FFMPEG_DEFINITIONS}
    ${Qt6_DEFINITIONS}
)

# 安装规则
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# 复制资源文件
install(DIRECTORY resources/
    DESTINATION share/VideoEye/resources
)

# 打印配置信息
message(STATUS "VideoEye Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  FFmpeg Version: ${FFMPEG_VERSION}")
message(STATUS "  OpenCV Version: ${OpenCV_VERSION}")
message(STATUS "  Qt Version: ${Qt6_VERSION}")
