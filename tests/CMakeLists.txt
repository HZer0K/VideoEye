# ============================================================
# VideoEye 单元测试配置
# 使用 GoogleTest 框架 (优先使用已安装包，必要时通过 FetchContent 自动下载)
#
# 注意: 测试源文件 (tests/unit/*.cpp) 尚未提交到仓库
#       启用方式: cmake -DBUILD_TESTING=ON ..
# ============================================================

# 守卫: unit/ 测试源尚未提交时, 跳过测试构建 (避免 configure 因缺源文件失败)
file(GLOB TEST_UNIT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")
if(NOT TEST_UNIT_SOURCES)
    message(STATUS "tests/unit/*.cpp 未找到, 跳过测试构建 (提交测试源文件后自动启用)")
    return()
endif()

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    # 禁止 GoogleTest 覆盖编译器标志
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

# ============================================================
# 公共 include 目录 (与主项目一致)
# ============================================================
set(BENTO4_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Bento4/Source/C++/Core")

set(TEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils
    ${BENTO4_CORE_DIR}
    ${BENTO4_CORE_DIR}/../MetaData
    ${BENTO4_CORE_DIR}/../Codecs
    ${BENTO4_CORE_DIR}/../Crypto
    ${BENTO4_CORE_DIR}/../Adapters
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

# FFmpeg RPATH (用于 local build 场景)
if(FFMPEG_LOCAL_BUILD AND FFMPEG_LOCAL_DIR)
    set(TEST_FFMPEG_RPATH "${FFMPEG_LOCAL_DIR}/bin")
else()
    set(TEST_FFMPEG_RPATH "")
endif()

# ============================================================
# 测试 1: FormatDetector (仅依赖 Qt)
# ============================================================
add_executable(test_format_detector
    unit/test_format_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/FormatDetector.cpp
)
target_include_directories(test_format_detector PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_format_detector PRIVATE
    GTest::gtest_main
    Qt6::Core
)
set_target_properties(test_format_detector PROPERTIES
    AUTOMOC ON
    BUILD_RPATH "${TEST_FFMPEG_RPATH}"
)
add_test(NAME FormatDetectorTests COMMAND test_format_detector)

# ============================================================
# 测试 2: ConfigManager (仅依赖 C++ stdlib)
# ============================================================
add_executable(test_config_manager
    unit/test_config_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ConfigManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
)
target_include_directories(test_config_manager PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_config_manager PRIVATE
    GTest::gtest_main
    Qt6::Core
)
set_target_properties(test_config_manager PROPERTIES
    AUTOMOC ON
    BUILD_RPATH "${TEST_FFMPEG_RPATH}"
)
add_test(NAME ConfigManagerTests COMMAND test_config_manager)

# ============================================================
# 测试 3: ReportExporter (依赖 StreamStats 结构体)
# ============================================================
add_executable(test_report_exporter
    unit/test_report_exporter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ReportExporter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/StreamAnalyzer.cpp
)
target_include_directories(test_report_exporter PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_report_exporter PRIVATE
    GTest::gtest_main
    Qt6::Core
    ${FFMPEG_LIBRARIES}
)
set_target_properties(test_report_exporter PROPERTIES
    AUTOMOC ON
    BUILD_RPATH "${TEST_FFMPEG_RPATH}"
)
add_test(NAME ReportExporterTests COMMAND test_report_exporter)

# ============================================================
# 测试 4: StreamAnalyzer (依赖 FFmpeg)
# ============================================================
add_executable(test_stream_analyzer
    unit/test_stream_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/StreamAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
)
target_include_directories(test_stream_analyzer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_stream_analyzer PRIVATE
    GTest::gtest_main
    ${FFMPEG_LIBRARIES}
)
set_target_properties(test_stream_analyzer PROPERTIES
    BUILD_RPATH "${TEST_FFMPEG_RPATH}"
)
add_test(NAME StreamAnalyzerTests COMMAND test_stream_analyzer)

# ============================================================
# 测试 5: FrameData 模型 (依赖 FFmpeg)
# ============================================================
add_executable(test_frame_data
    unit/test_frame_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/FrameData.cpp
)
target_include_directories(test_frame_data PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_frame_data PRIVATE
    GTest::gtest_main
    ${FFMPEG_LIBRARIES}
)
set_target_properties(test_frame_data PROPERTIES
    BUILD_RPATH "${TEST_FFMPEG_RPATH}"
)
add_test(NAME FrameDataTests COMMAND test_frame_data)

# =================================================
# 测试 6：QualityAnalyzer(依赖 FFmpeg + swscale)
# =================================================
add_executable(test_quality_analyzer
    unit/test_quality_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/QualityAnalyzer.cpp
)
target_include_directories(test_quality_analyzer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_quality_analyzer PRIVATE
    GTest::gtest_main
    ${FFMPEG_LIBRARIES}
)
set_target_properties(test_quality_analyzer PROPERTIES
    BUILD_RPATH "${TEST_FFMPEG_RPATH}"
)
add_test(NAME QualityAnalyzerTests COMMAND test_quality_analyzer)
