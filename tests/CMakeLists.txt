# ============================================================
# VideoEye 单元测试配置
#
# 启用方式（两者等价，preset 是推荐入口）:
#   cmake --preset win-test-debug      # Windows (vcpkg feature "tests" 会装上 gtest)
#   cmake --preset linux-test-debug    # Linux / macOS（用系统 gtest）
#   cmake -B build/test-debug -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug  # 手配
# 运行: ctest --preset <同名 preset>
#
# GoogleTest 默认要求本地已有（vcpkg 或系统包）。联网兜底必须显式开:
#   -DVIDEOEYE_FETCH_GTEST=ON
# 这么设计是为了让"本机装了 gtest / 没装 gtest"两种环境行为一致 —— 以前
# find_package(GTest QUIET) 失败就直接 FetchContent 下载，CI 离线时会变成
# 一个毫无征兆的漫长等待。
# ============================================================

# 守卫: unit/ 下没有测试源时跳过（避免 configure 因缺源文件失败）
file(GLOB TEST_UNIT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")
if(NOT TEST_UNIT_SOURCES)
    message(STATUS "tests/unit/*.cpp 未找到, 跳过测试构建")
    return()
endif()

option(VIDEOEYE_FETCH_GTEST "本地找不到 GoogleTest 时联网下载 (FetchContent)" OFF)

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    if(VIDEOEYE_FETCH_GTEST)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.15.2
        )
        # 禁止 GoogleTest 覆盖编译器标志（MSVC 下尤其重要）
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    else()
        message(FATAL_ERROR
            "未找到本地 GoogleTest，且未开启 VIDEOEYE_FETCH_GTEST。请任选其一:\n"
            "  * Windows: cmake --preset win-test-debug  (preset 自带 VCPKG_MANIFEST_FEATURES=tests)\n"
            "             或先跑 scripts/run-vcpkg-install.ps1 -Config debug -Feature tests\n"
            "  * Linux:   sudo apt install -y libgtest-dev\n"
            "  * macOS:   brew install googletest\n"
            "  * 离线兜底: cmake -DBUILD_TESTING=ON -DVIDEOEYE_FETCH_GTEST=ON ...\n")
    endif()
endif()

enable_testing()

# ============================================================
# Windows: 找到 gtest 运行时 DLL
#
# vcpkg 的 x64-windows gtest 是动态库，而 GTest::gtest_main 链接的是
# manual-link/ 下的 .lib —— CMake 不知道运行时还需要 gtest.dll / gtest_main.dll，
# 不会自动拷到测试可执行文件旁边。结果就是 ctest 一启动全部报
# 0xc0000135 (STATUS_DLL_NOT_FOUND)，一个和代码毫无关系的错。
# ============================================================
set(VIDEOEYE_GTEST_DLLS "")
if(WIN32)
    # 探测策略: 先看 imported target 有没有直接给出 DLL, 没有再从导入库
    # <root>/lib/gtest.lib 推出同级 bin/, 最后兜底扫 CMAKE_PREFIX_PATH。
    set(_gtest_dll_dirs "")
    set(_gtest_lib_file "")
    foreach(_mod gtest gtest_main)
        if(TARGET GTest::${_mod})
            get_target_property(_loc GTest::${_mod} IMPORTED_LOCATION)
            if(_loc)
                get_filename_component(_ext "${_loc}" EXT)
                if(_ext STREQUAL ".dll")
                    get_filename_component(_dir "${_loc}" DIRECTORY)
                    list(APPEND _gtest_dll_dirs "${_dir}")
                else()
                    set(_gtest_lib_file "${_loc}")
                endif()
            endif()
        endif()
    endforeach()

    if(_gtest_lib_file)
        get_filename_component(_lib_dir "${_gtest_lib_file}" DIRECTORY)
        get_filename_component(_root "${_lib_dir}" DIRECTORY)
        foreach(_candidate "${_root}/bin" "${_lib_dir}")
            if(EXISTS "${_candidate}/gtest.dll" OR EXISTS "${_candidate}/gtest_main.dll")
                list(APPEND _gtest_dll_dirs "${_candidate}")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _gtest_dll_dirs)
        foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
            if(EXISTS "${_prefix}/bin/gtest.dll" OR EXISTS "${_prefix}/bin/gtest_main.dll")
                list(APPEND _gtest_dll_dirs "${_prefix}/bin")
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES _gtest_dll_dirs)

    foreach(_dir IN LISTS _gtest_dll_dirs)
        foreach(_mod gtest gtest_main)
            if(EXISTS "${_dir}/${_mod}.dll")
                list(APPEND VIDEOEYE_GTEST_DLLS "${_dir}/${_mod}.dll")
            endif()
        endforeach()
    endforeach()
    if(VIDEOEYE_GTEST_DLLS)
        list(REMOVE_DUPLICATES VIDEOEYE_GTEST_DLLS)
        message(STATUS "gtest runtime: ${VIDEOEYE_GTEST_DLLS}")
    else()
        message(STATUS "未找到 gtest 运行时 DLL（若是静态版 gtest 则无需处理）")
    endif()
endif()

# ============================================================
# 公共 include 目录 (与主项目一致)
# ============================================================
set(TEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils
    ${FFMPEG_INCLUDE_DIRS}
)

# ============================================================
# 公共配置函数: 创建测试可执行文件并套上统一设置
# 各测试自己的额外依赖（Qt6::Core / FFmpeg::*）仍用 target_link_libraries 追加。
# ============================================================
function(videoeye_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(${name} PRIVATE GTest::gtest_main)
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
    # 测试可执行文件与主程序在同一个输出目录:
    #   Windows  —— Qt / FFmpeg DLL 已经被主程序的 POST_BUILD 拷过去了；
    #               gtest.dll 没人管，这里单独补一步
    #   Linux/macOS —— 靠相对 rpath 找 build/lib 里的 FFmpeg 动态库
    # 测试可执行文件与主程序共用一个输出目录:
    #   Linux/macOS —— 靠相对 rpath 找 build/lib 里的 FFmpeg 动态库
    #   Windows     —— 由 videoeye-runtime 一次性铺好 DLL（见下面注释）
    videoeye_set_ffmpeg_rpath(${name})

    # Windows: 不再各做一份 POST_BUILD。18 个目标同时拷同一批 av*.dll 会
    # 互相撞文件锁 ("Permission denied")，所以统一登记到顶层的
    # videoeye-runtime，顺带把"只有测试才用得到"的 gtest 运行时交上去。
    videoeye_register_runtime_target(${name} ${VIDEOEYE_GTEST_DLLS})
endfunction()

# ============================================================
# 测试 1: FormatDetector (仅依赖 Qt)
# ============================================================
videoeye_add_test(test_format_detector
    unit/test_format_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/FormatDetector.cpp
)
target_link_libraries(test_format_detector PRIVATE Qt6::Core)
add_test(NAME FormatDetectorTests COMMAND test_format_detector)

# ============================================================
# 测试 2: ConfigManager (仅依赖 C++ stdlib)
# ============================================================
videoeye_add_test(test_config_manager
    unit/test_config_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ConfigManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
)
target_link_libraries(test_config_manager PRIVATE Qt6::Core)
add_test(NAME ConfigManagerTests COMMAND test_config_manager)

# ============================================================
# 测试 3: ReportExporter (依赖 StreamStats 结构体)
# ============================================================
videoeye_add_test(test_report_exporter
    unit/test_report_exporter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ReportExporter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/StreamAnalyzer.cpp
    # ReportExporter 的「色彩与 HDR」章节复用 ColorHdrAnalyzer 的行构造
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/ColorHdrAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/ColorInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/HdrMetadataInfo.cpp
    # ReportExporter 还会输出 QC 报告：QcModels 提供 ToString(...) 重载，
    # 少了它链接期会报一堆 "无法解析的外部符号"。
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/QcModels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/QcReport.cpp
)
target_link_libraries(test_report_exporter PRIVATE
    Qt6::Core
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME ReportExporterTests COMMAND test_report_exporter)

# ============================================================
# 测试 4: StreamAnalyzer (依赖 FFmpeg)
# ============================================================
videoeye_add_test(test_stream_analyzer
    unit/test_stream_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/StreamAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
)
target_link_libraries(test_stream_analyzer PRIVATE
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME StreamAnalyzerTests COMMAND test_stream_analyzer)

# ============================================================
# 测试 5: FrameData 模型 (依赖 FFmpeg)
# ============================================================
videoeye_add_test(test_frame_data
    unit/test_frame_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/FrameData.cpp
)
target_link_libraries(test_frame_data PRIVATE
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME FrameDataTests COMMAND test_frame_data)

# =================================================
# 测试 6：BitrateGopAnalyzer (不依赖 FFmpeg / Qt，喂合成样本)
# =================================================
videoeye_add_test(test_bitrate_gop_analyzer
    unit/test_bitrate_gop_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/BitrateGopAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitratePoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/GopInfo.cpp
)
add_test(NAME BitrateGopAnalyzerTests COMMAND test_bitrate_gop_analyzer)

# =================================================
# 测试 7：QualityAnalyzer(依赖 FFmpeg + swscale)
# =================================================
videoeye_add_test(test_quality_analyzer
    # 这个文件在 tests/ 根目录（历史遗留），其余测试都在 unit/ 下
    ${CMAKE_CURRENT_SOURCE_DIR}/test_quality_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/QualityAnalyzer.cpp
)
target_link_libraries(test_quality_analyzer PRIVATE
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME QualityAnalyzerTests COMMAND test_quality_analyzer)

# =================================================
# 测试 8：AudioQcAnalyzer (不依赖 FFmpeg / Qt，喂合成 PCM)
# =================================================
videoeye_add_test(test_audio_qc_analyzer
    unit/test_audio_qc_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/AudioQcAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/AudioQcResult.cpp
)
add_test(NAME AudioQcAnalyzerTests COMMAND test_audio_qc_analyzer)

# =================================================
# 测试 9：音频 QC 规则 (AudioQcAnalyzer + QcRuleEngine，不依赖 FFmpeg)
# =================================================
videoeye_add_test(test_audio_qc_rules
    unit/test_audio_qc_rules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/AudioQcAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/QcRuleEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/AnalysisTask.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/AudioQcResult.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/QcModels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/QcReport.cpp
    # QcRuleEngine 会用到色彩/HDR 模型的成员函数 (EffectiveBitDepth / CompatibilityText ...)
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/ColorInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/HdrMetadataInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/BitrateGopAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitratePoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/GopInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/TimelineDiagnostic.cpp
)
add_test(NAME AudioQcRulesTests COMMAND test_audio_qc_rules)

# =================================================
# 测试 10：色彩与 HDR 元数据 (ColorHdrAnalyzer 的行构造 + QcRuleEngine)
# 说明: BuildColorRows / BuildHdrRows 定义在 ColorHdrAnalyzer.cpp（依赖 FFmpeg 头文件），
#       所以本目标需要链接 FFmpeg；规则判定本身仍是纯模型层，无需解码任何文件。
# =================================================
videoeye_add_test(test_color_hdr_rules
    unit/test_color_hdr_rules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/ColorHdrAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/QcRuleEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/AnalysisTask.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/BitrateGopAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/AudioQcAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/QcModels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/QcReport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/ColorInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/HdrMetadataInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/AudioQcResult.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitratePoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/GopInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/TimelineDiagnostic.cpp
)
target_link_libraries(test_color_hdr_rules PRIVATE
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME ColorHdrRulesTests COMMAND test_color_hdr_rules)

# =================================================
# 测试 11：MP4/fMP4 容器一致性 (Mp4SampleTableAnalyzer 的纯逻辑校验)
# 说明: 只测 Validate()——手工构造合成样本表，不需要 Qt 或真实文件。
#       解析路径走自研 utils/IsobmffParser（纯 stdlib），无需任何第三方容器库。
# =================================================
videoeye_add_test(test_mp4_sample_table
    unit/test_mp4_sample_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/Mp4SampleTableAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/IsobmffParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/Logger.cpp
)
add_test(NAME Mp4SampleTableTests COMMAND test_mp4_sample_table)

# =================================================
# 测试 12：ISOBMFF 真实解析路径 (utils::IsobmffParser)
# 说明: 内存里拼一个最小合法的 MP4 再落盘解析，覆盖 stts/ctts/stz2 的字节级读取。
#       与测试 11 互补 —— 那边只测 Validate() 的静态校验，看不见解析阶段的问题。
# =================================================
videoeye_add_test(test_isobmff_parser
    unit/test_isobmff_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/IsobmffParser.cpp
)
add_test(NAME IsobmffParserTests COMMAND test_isobmff_parser)

# =================================================
# 测试 13：H.264 码流解析 (profile/level 名称 + SPS/PPS + extradata 格式识别)
# 说明: 纯 stdlib，不需要 Qt / FFmpeg。
# =================================================
videoeye_add_test(test_h264_bitstream_parser
    unit/test_h264_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/H264BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitstreamInfo.cpp
)
add_test(NAME H264BitstreamParserTests COMMAND test_h264_bitstream_parser)

# =================================================
# 测试 14：HEVC 码流解析 (profile/tier/level 名称 + VPS/SPS/PPS + hvcC 识别)
# =================================================
videoeye_add_test(test_hevc_bitstream_parser
    unit/test_hevc_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/HevcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitstreamInfo.cpp
)
add_test(NAME HevcBitstreamParserTests COMMAND test_hevc_bitstream_parser)

# =================================================
# 测试 15：BitstreamAnalyzer（容器侧快照 + 容器/码流对比分级）
# 说明: 这是「参数集解析」页的数据来源。只需要 FFmpeg 头文件（AVCodecID 枚举），
#       不调用任何 avcodec 函数，因此不必链接 FFmpeg 库。
# =================================================
videoeye_add_test(test_bitstream_analyzer
    unit/test_bitstream_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/BitstreamAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/H264BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/HevcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/Av1BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/VvcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitstreamInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/BitReader.cpp
)
add_test(NAME BitstreamAnalyzerTests COMMAND test_bitstream_analyzer)

# =================================================
# 测试 16：AV1 码流解析（序列头 OBU + av1C 配置记录）
# 说明: 与测试 13/14 同构，比特流由 _smoke/gen_av1_seqhdr.py 逐位生成。
# =================================================
videoeye_add_test(test_av1_bitstream_parser
    unit/test_av1_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/Av1BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitstreamInfo.cpp
)
add_test(NAME Av1BitstreamParserTests COMMAND test_av1_bitstream_parser)

# =================================================
# 测试 17：VVC (H.266) 码流解析（VPS/SPS/PPS + vvcC 记录 + NAL header 位域）
# 说明: VVC 的 NAL type 在第 2 字节高 5 位，和 HEVC 完全不同 —— 这一条
#       单独有测试兜底，避免以后改 ExtradataParser 时静默读错类型。
# =================================================
videoeye_add_test(test_vvc_bitstream_parser
    unit/test_vvc_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analyzer/VvcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/model/BitstreamInfo.cpp
)
add_test(NAME VvcBitstreamParserTests COMMAND test_vvc_bitstream_parser)
