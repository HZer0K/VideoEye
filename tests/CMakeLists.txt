# ============================================================
# VideoEye 单元测试配置
# 使用 GoogleTest 框架 (通过 FetchContent 自动下载)
# ============================================================

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
# 禁止 GoogleTest 覆盖编译器标志
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# ============================================================
# 公共 include 目录 (与主项目一致)
# ============================================================
set(TEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils
    ${BENTO4_DIR}
    ${BENTO4_DIR}/../MetaData
    ${BENTO4_DIR}/../Codecs
    ${BENTO4_DIR}/../Crypto
    ${BENTO4_DIR}/../Adapters
    ${FFMPEG_SOURCE_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

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
    BUILD_RPATH "${FFMPEG_SOURCE_LIB_DIR}"
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
    BUILD_RPATH "${FFMPEG_SOURCE_LIB_DIR}"
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
    ${FFMPEG_SYSTEM_LIBS}
)
set_target_properties(test_report_exporter PROPERTIES
    AUTOMOC ON
    BUILD_RPATH "${FFMPEG_SOURCE_LIB_DIR}"
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
    ${FFMPEG_SYSTEM_LIBS}
)
set_target_properties(test_stream_analyzer PROPERTIES
    BUILD_RPATH "${FFMPEG_SOURCE_LIB_DIR}"
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
    ${FFMPEG_SYSTEM_LIBS}
)
set_target_properties(test_frame_data PROPERTIES
    BUILD_RPATH "${FFMPEG_SOURCE_LIB_DIR}"
)
add_test(NAME FrameDataTests COMMAND test_frame_data)

# ============================================================
# 测试 6: FrameAnalyzer (依赖 OpenCV + FFmpeg)
# ============================================================
add_executable(test_frame_analyzer
    unit/test_frame_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/FrameAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/FrameData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
)
target_include_directories(test_frame_analyzer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_frame_analyzer PRIVATE
    GTest::gtest_main
    ${FFMPEG_LIBRARIES}
    ${FFMPEG_SYSTEM_LIBS}
    ${OpenCV_LIBS}
)
set_target_properties(test_frame_analyzer PROPERTIES
    BUILD_RPATH "${FFMPEG_SOURCE_LIB_DIR}"
)
add_test(NAME FrameAnalyzerTests COMMAND test_frame_analyzer)
