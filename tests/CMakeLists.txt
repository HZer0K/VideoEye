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
# 源码里的 #include 都写成"从仓库根算起"（"core/analysis/xxx.h"），所以只要根目录；
# 生成目录放 FfmpegToolConfig.h，给 FFmpeg 命令工作台那两个测试用。
set(TEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${VIDEOEYE_GENERATED_INCLUDE_DIR}
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/orchestration/FormatDetector.cpp
)
target_link_libraries(test_format_detector PRIVATE Qt6::Core)
add_test(NAME FormatDetectorTests COMMAND test_format_detector)

# ============================================================
# 测试 2: ConfigManager (仅依赖 C++ stdlib)
# ============================================================
videoeye_add_test(test_config_manager
    unit/test_config_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/config/ConfigManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/logging/Logger.cpp
)
target_link_libraries(test_config_manager PRIVATE Qt6::Core)
add_test(NAME ConfigManagerTests COMMAND test_config_manager)

# ============================================================
# 测试 3: StreamStatsExporter (流统计快照导出)
#
# 输入是 model::StreamStats(domain 的纯值对象), 所以只链 VideoEyeReporting 就够了 ——
# 不需要 FFmpeg。这一条以前是 ReportExporterTests, 那时的导出器签名带着
# analyzer::StreamStats, 被迫把整个 analysis 层拉进来。
#
# 注意: VideoEyeDomain 这一轮摘掉了 PUBLIC Qt6::Core（domain 现在零 Qt），
# 所以 Qt 的头文件不再从 VideoEyeReporting 透传过来。本测试用 QTemporaryDir /
# QString 建临时文件，必须自己显式链 Qt6::Core —— 别再指望透传。
# ============================================================
videoeye_add_test(test_stream_stats_exporter
    unit/test_stream_stats_exporter.cpp
)
target_link_libraries(test_stream_stats_exporter PRIVATE VideoEyeReporting)
target_link_libraries(test_stream_stats_exporter PRIVATE Qt6::Core)
add_test(NAME StreamStatsExporterTests COMMAND test_stream_stats_exporter)

# ============================================================
# 测试 4: StreamAnalyzer (依赖 FFmpeg)
# ============================================================
videoeye_add_test(test_stream_analyzer
    unit/test_stream_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/StreamStats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/stream/StreamAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/logging/Logger.cpp
)
target_link_libraries(test_stream_analyzer PRIVATE
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME StreamAnalyzerTests COMMAND test_stream_analyzer)

# ============================================================
# 测试 4c: TaskManager (统一后台任务调度) —— 任务 ID / 取消 / 终态 / 过期丢弃
#
# TaskManager 本身只依赖 stdlib; 链 VideoEyeInfrastructure 而不是手写清单,
# 是因为 std::thread 在 Linux 需要显式链 pthread —— 自己写容易漏。
# ============================================================
videoeye_add_test(test_task_manager unit/test_task_manager.cpp)
target_link_libraries(test_task_manager PRIVATE VideoEyeInfrastructure)
add_test(NAME TaskManagerTests COMMAND test_task_manager)

# ============================================================
# 测试 4d: PlaybackSession (播放机械: 状态机 / 资源生命周期)
#
# 只覆盖"不需要真实媒体文件"的行为; 解码循环需要真实文件与音频设备, 不进 gtest。
# ============================================================
videoeye_add_test(test_playback_session unit/test_playback_session.cpp)
target_link_libraries(test_playback_session PRIVATE
    VideoEyePlayback
    # PlaybackSession.h 里有 QImage（抽帧结果要能直接上屏），需要 QtGui 的头文件。
    Qt6::Core Qt6::Gui
)
add_test(NAME PlaybackSessionTests COMMAND test_playback_session)

# ============================================================
# 测试 4b: AnalysisSession (依赖 FFmpeg) —— 评审 Phase 3 抽取的分析会话类
# ============================================================
videoeye_add_test(test_analysis_session
    unit/test_analysis_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/StreamStats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/stream/StreamAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/logging/Logger.cpp
)
target_link_libraries(test_analysis_session PRIVATE
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME AnalysisSessionTests COMMAND test_analysis_session)

# ============================================================
# 测试 5: FrameData 模型 (依赖 FFmpeg)
# ============================================================
videoeye_add_test(test_frame_data
    unit/test_frame_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/player/FrameData.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/BitrateGopResult.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/quality/BitrateGopAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/BitratePoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/GopInfo.cpp
)
add_test(NAME BitrateGopAnalyzerTests COMMAND test_bitrate_gop_analyzer)

# =================================================
# 测试 7：QualityAnalyzer(依赖 FFmpeg + swscale)
# =================================================
videoeye_add_test(test_quality_analyzer
    # 这个文件在 tests/ 根目录（历史遗留），其余测试都在 unit/ 下
    ${CMAKE_CURRENT_SOURCE_DIR}/test_quality_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/quality/QualityAnalyzer.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/quality/AudioQcAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/AudioQcResult.cpp
)
add_test(NAME AudioQcAnalyzerTests COMMAND test_audio_qc_analyzer)

# =================================================
# 测试 9：音频 QC 规则 (AudioQcAnalyzer + QcRuleEngine，不依赖 FFmpeg)
# =================================================
# 说明: QcRuleEngine 会牵出几乎所有 model + analyzer, 清单长且易漏 —— 直接链
#       VideoEyeAnalysis。
videoeye_add_test(test_audio_qc_rules
    unit/test_audio_qc_rules.cpp
)
target_link_libraries(test_audio_qc_rules PRIVATE VideoEyeAnalysis)
add_test(NAME AudioQcRulesTests COMMAND test_audio_qc_rules)

# =================================================
# 测试 10：色彩与 HDR 元数据 (ColorHdrAnalyzer 的行构造 + QcRuleEngine)
# 说明: BuildColorRows / BuildHdrRows 定义在 ColorHdrAnalyzer.cpp（依赖 FFmpeg 头文件），
#       所以本目标需要链接 FFmpeg；规则判定本身仍是纯模型层，无需解码任何文件。
# =================================================
videoeye_add_test(test_color_hdr_rules
    unit/test_color_hdr_rules.cpp
)
target_link_libraries(test_color_hdr_rules PRIVATE VideoEyeAnalysis)
add_test(NAME ColorHdrRulesTests COMMAND test_color_hdr_rules)

# =================================================
# 测试 11：MP4/fMP4 容器一致性 (Mp4SampleTableAnalyzer 的纯逻辑校验)
# 说明: 只测 Validate()——手工构造合成样本表，不需要 Qt 或真实文件。
#       解析路径走自研 core/media/container/IsobmffParser.h（纯 stdlib），无需任何第三方容器库。
# =================================================
videoeye_add_test(test_mp4_sample_table
    unit/test_mp4_sample_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/container/Mp4SampleTableAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/container/IsobmffParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/logging/Logger.cpp
)
add_test(NAME Mp4SampleTableTests COMMAND test_mp4_sample_table)

# =================================================
# 测试 12：ISOBMFF 真实解析路径 (utils::IsobmffParser)
# 说明: 内存里拼一个最小合法的 MP4 再落盘解析，覆盖 stts/ctts/stz2 的字节级读取。
#       与测试 11 互补 —— 那边只测 Validate() 的静态校验，看不见解析阶段的问题。
# =================================================
videoeye_add_test(test_isobmff_parser
    unit/test_isobmff_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/container/IsobmffParser.cpp
)
add_test(NAME IsobmffParserTests COMMAND test_isobmff_parser)

# =================================================
# 测试 13：H.264 码流解析 (profile/level 名称 + SPS/PPS + extradata 格式识别)
# 说明: 纯 stdlib，不需要 Qt / FFmpeg。
# =================================================
videoeye_add_test(test_h264_bitstream_parser
    unit/test_h264_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/H264BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/BitstreamInfo.cpp
)
add_test(NAME H264BitstreamParserTests COMMAND test_h264_bitstream_parser)

# =================================================
# 测试 14：HEVC 码流解析 (profile/tier/level 名称 + VPS/SPS/PPS + hvcC 识别)
# =================================================
videoeye_add_test(test_hevc_bitstream_parser
    unit/test_hevc_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/HevcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/BitstreamInfo.cpp
)
add_test(NAME HevcBitstreamParserTests COMMAND test_hevc_bitstream_parser)

# =================================================
# 测试 15：BitstreamAnalyzer（容器侧快照 + 容器/码流对比分级）
# 说明: 这是「参数集解析」页的数据来源。只需要 FFmpeg 头文件（AVCodecID 枚举），
#       不调用任何 avcodec 函数，因此不必链接 FFmpeg 库。
# =================================================
videoeye_add_test(test_bitstream_analyzer
    unit/test_bitstream_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/BitstreamAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/H264BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/HevcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/Av1BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/VvcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/BitstreamInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/BitReader.cpp
)
add_test(NAME BitstreamAnalyzerTests COMMAND test_bitstream_analyzer)

# =================================================
# 测试 16：AV1 码流解析（序列头 OBU + av1C 配置记录）
# 说明: 与测试 13/14 同构，比特流由 _smoke/gen_av1_seqhdr.py 逐位生成。
# =================================================
videoeye_add_test(test_av1_bitstream_parser
    unit/test_av1_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/Av1BitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/BitstreamInfo.cpp
)
add_test(NAME Av1BitstreamParserTests COMMAND test_av1_bitstream_parser)

# =================================================
# 测试 17：VVC (H.266) 码流解析（VPS/SPS/PPS + vvcC 记录 + NAL header 位域）
# 说明: VVC 的 NAL type 在第 2 字节高 5 位，和 HEVC 完全不同 —— 这一条
#       单独有测试兜底，避免以后改 ExtradataParser 时静默读错类型。
# =================================================
# =================================================
# 测试 18：HLS / DASH 流媒体包解析与校验
# 说明: 纯 stdlib，不需要 Qt / FFmpeg。
#       SegmentQcAnalyzer 会链到 Mp4SampleTableAnalyzer（fMP4 分片探测走自研
#       IsobmffParser），那一支同样不含 Qt，只是多了 Logger。
#       临时文件用 std::filesystem 造在 TEMP 下。
# =================================================
videoeye_add_test(test_streaming_package
    unit/test_streaming_package.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/streaming/HlsManifestAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/streaming/DashManifestAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/streaming/SegmentQcAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/container/Mp4SampleTableAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/container/IsobmffParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/logging/Logger.cpp
)
add_test(NAME StreamingPackageTests COMMAND test_streaming_package)

# =================================================
# 测试 19：流媒体包 QC 规则（QcRuleEngine 的 container.hls.* / container.dash.* /
#          container.streaming.* 三个前缀分支）
# 说明: 依赖清单与测试 10（色彩/HDR 规则）同构，再叠加三个清单分析器。
# =================================================
videoeye_add_test(test_streaming_qc_rules
    unit/test_streaming_qc_rules.cpp
)
target_link_libraries(test_streaming_qc_rules PRIVATE VideoEyeAnalysis)
add_test(NAME StreamingQcRulesTests COMMAND test_streaming_qc_rules)

# =================================================
# 测试 21：视觉缺陷检测（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边）
# 说明: VisualDefectAnalyzer 只吃 model::FrameSample 里的降采样像素，不依赖 FFmpeg（
#       AVFrame -> FrameSample 的缩放在 QualityAnalyzer::BuildSample），所以这里全部用
#       合成图，不需要任何真实媒体文件。
# =================================================
videoeye_add_test(test_visual_defect
    unit/test_visual_defect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/quality/VisualDefectAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/VisualDefect.cpp
)
add_test(NAME VisualDefectTests COMMAND test_visual_defect)

# =================================================
# 测试 22：字幕 / 时码 / SCTE-35 解析与校验（功能 9）
# 说明: SRT/ASS/WebVTT/tx3g 的文本解析、SMPTE 时码换算（含 drop-frame）、
#       SCTE-35 splice_info_section 位流解析全部是纯逻辑，用合成输入验证；
#       唯一需要 FFmpeg 的是 ParsePacketPayload 里的 codec id 分派，
#       所以这个目标要链 avcodec/avformat。
# =================================================
videoeye_add_test(test_subtitle_timecode_aux
    unit/test_subtitle_timecode_aux.cpp
)
target_link_libraries(test_subtitle_timecode_aux PRIVATE VideoEyeAnalysis)
add_test(NAME SubtitleTimecodeAuxTests COMMAND test_subtitle_timecode_aux)

# =================================================
# 测试 23：字幕 / 时码 / 章节 / SCTE-35 的 QC 规则（subtitle.* / timecode.* /
#          chapter.* / scte35.*）
# 说明: 测的是"分析器发现的问题有没有被规则引擎转成报告条目、级别对不对"。
#       规则表被人改了 id 或严重度时，只有这一层能发现。
# =================================================
videoeye_add_test(test_aux_qc_rules
    unit/test_aux_qc_rules.cpp
)
target_link_libraries(test_aux_qc_rules PRIVATE VideoEyeAnalysis)
add_test(NAME AuxQcRulesTests COMMAND test_aux_qc_rules)

# =================================================
# 测试 20：VVC (H.266) 码流解析（VPS/SPS/PPS + vvcC 记录 + NAL header 位域）
# 说明: VVC 的 NAL type 在第 2 字节高 5 位，和 HEVC 完全不同 —— 这一条
#       单独有测试兜底，避免以后改 ExtradataParser 时静默读错类型。
# =================================================
videoeye_add_test(test_vvc_bitstream_parser
    unit/test_vvc_bitstream_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/VvcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/ExtradataParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/BitstreamInfo.cpp
)
add_test(NAME VvcBitstreamParserTests COMMAND test_vvc_bitstream_parser)

# =================================================
# 测试 24：JSON 工具（极简读写，不依赖 Qt / FFmpeg）
# =================================================
videoeye_add_test(test_json
    unit/test_json.cpp
)
target_link_libraries(test_json PRIVATE VideoEyeInfrastructure)
add_test(NAME JsonTests COMMAND test_json)

# =================================================
# 测试 25：QC 模板（内置 5 套 / 序列化往返 / 覆盖项 / 未知规则检测）
# 说明: QcProfile 是纯模型层（只依赖 QcRule 与规则表），无需解码文件。
# =================================================
videoeye_add_test(test_qc_profile
    unit/test_qc_profile.cpp
)
target_link_libraries(test_qc_profile PRIVATE VideoEyeQc)
add_test(NAME QcProfileTests COMMAND test_qc_profile)

# =================================================
# 测试 26：批量 QC 扫描（目录发现 / 扩展名过滤 / 取消后无残留线程 / 并发上限）
# 说明: BatchQcRunner 只做调度，用假分析函数即可，不触碰任何媒体文件。
# =================================================
videoeye_add_test(test_batch_qc_runner
    unit/test_batch_qc_runner.cpp
)
target_link_libraries(test_batch_qc_runner PRIVATE VideoEyeQc)
add_test(NAME BatchQcRunnerTests COMMAND test_batch_qc_runner)

# =================================================
# 测试 27：双文件对比（转码前后 / 同参数校验）
# 说明: CompareRuns 读 QcRunResult.report 字段即可产生对比行，无需完整 analysis。
# =================================================
videoeye_add_test(test_qc_comparator
    unit/test_qc_comparator.cpp
)
target_link_libraries(test_qc_comparator PRIVATE VideoEyeQc)
add_test(NAME QcComparatorTests COMMAND test_qc_comparator)

# =================================================
# 测试 28：报告导出（CSV 每 issue 一行 / JSON schema 稳定 / 批量汇总 / 扩展名）
# 说明: 只测 CSV/JSON/汇总这些纯 std::string 路径；PDF 走 QPdfWriter 不在本测试覆盖。
# =================================================
videoeye_add_test(test_qc_report_exporter
    unit/test_qc_report_exporter.cpp
)
target_link_libraries(test_qc_report_exporter PRIVATE VideoEyeReporting)
add_test(NAME QcReportExporterTests COMMAND test_qc_report_exporter)

# =================================================
# 测试 30：诊断结果流转（评审 P0 契约）
# 锁定 AnalysisFacade::SetResult -> result() -> Evaluate 使用同一份结果,
# 防止 DiagnosticsPage 漏写结果导致报告/评分/问题表全部为空。
# =================================================
videoeye_add_test(test_diagnostics_result_flow
    unit/test_diagnostics_result_flow.cpp
)
target_sources(test_diagnostics_result_flow PRIVATE ../ui/AnalysisFacade.cpp)
target_link_libraries(test_diagnostics_result_flow PRIVATE
    VideoEyeQtAdapters
    VideoEyeAnalysis
    VideoEyeDomain
    Qt6::Core)
add_test(NAME DiagnosticsResultFlowTests COMMAND test_diagnostics_result_flow)

# =================================================
# 测试 31：MediaExporter 错误传播（评审 P1）
# 损坏输入必须上报 ExportError, 绝不误报 ExportFinished。
# =================================================
videoeye_add_test(test_media_exporter
    unit/test_media_exporter.cpp
)
target_link_libraries(test_media_exporter PRIVATE
    VideoEyeExporter
    Qt6::Core)
add_test(NAME MediaExporterTests COMMAND test_media_exporter)

# =================================================
# 测试 38：QtAnalysisController 线程句柄回收（复查 P1-1）
# 说明: 上一轮分析结束的 WorkerExited 被消费后若没有 join，句柄会一直 joinable，
#       于是下一次 StartAnalysis 误判"旧线程还活着"而把请求排进 pending_ 永久等待。
#       用极小 HLS 清单做输入（纯 stdlib 清单解析，快且确定），完整穿过
#       "工作线程 -> 引擎 -> 回调 -> 排队投递" 这条链。不需要 QPA 平台插件。
# =================================================
videoeye_add_test(test_qt_analysis_controller
    unit/test_qt_analysis_controller.cpp
)
target_link_libraries(test_qt_analysis_controller PRIVATE
    VideoEyeQtAdapters
    VideoEyeAnalysis
    VideoEyeDomain
    Qt6::Core)
add_test(NAME QtAnalysisControllerLifecycleTests COMMAND test_qt_analysis_controller)

# =================================================
# 测试 39：FFmpeg 阻塞 IO 的中断/超时工具（复查 P1-3）
# 说明: core/ffmpeg_io 是叶子模块, 只依赖 FFmpeg 公共头 —— 分析/播放/导出共用它
#       把阻塞中的 avformat_* 踢出来。这里锁定它的判定语义(取消/超时/关闭超时)。
# =================================================
videoeye_add_test(test_ffmpeg_interrupt
    unit/test_ffmpeg_interrupt.cpp
)
target_link_libraries(test_ffmpeg_interrupt PRIVATE VideoEyeFfmpegIo)
add_test(NAME FfmpegInterruptTests COMMAND test_ffmpeg_interrupt)

# =================================================
# 测试 40：换媒体必须终止进行中的导出（复查 P1-2）
# 说明: 用"只监听、从不回包"的本地端口做输入, 让旧导出稳定卡在阻塞 IO 里,
#       再 Open() 一个新文件 —— 要求旧任务被真正终止(新导出请求不会被永久排队),
#       且旧任务的终态信号不得串回新媒体上下文。需要事件循环, 不需要 QPA 平台插件。
# =================================================
videoeye_add_test(test_media_switch_export
    unit/test_media_switch_export.cpp
)
target_link_libraries(test_media_switch_export PRIVATE
    VideoEyePlayback
    Qt6::Core)
add_test(NAME MediaSwitchExportTests COMMAND test_media_switch_export)

# 上面两个用例用 BlockingTcpEndpoint.h 起本地监听端口做"可控阻塞输入"，
# 直接用 winsock，需要显式链 ws2_32（FFmpeg 的导入库不会替我们带进来）。
if(WIN32)
    target_link_libraries(test_media_exporter PRIVATE ws2_32)
    target_link_libraries(test_media_switch_export PRIVATE ws2_32)
endif()

# =================================================
# 测试 41：扫描失败必须复位所有共用页面（复查 P1-4）
#
# 说明: 码率/GOP、音频 QC、色彩/HDR 三页共用同一次扫描，它们的扫描态由面板转发
#       维护；而 DiagnosticsPage 失败时以前只重置自己、**不发任何终态信号** ——
#       于是失败后这三页永远停在"扫描中"（取消按钮亮着、"开始分析"永久禁用），
#       和诊断页显示的"失败"互相矛盾。这里跑一次真实的失败扫描（损坏输入），
#       断言三个页面都回到 Idle，并且一次扫描只产生一个终态事件。
#
# 为什么把整个 ui/ 都编进来: 被测对象是 AnalysisPanel 本体，而 UI 目前只编译进
#   主程序，没有独立的静态库可链。所以这里取 UI_SOURCES 去掉自带 main() 的
#   ui/app/main.cpp。每个 Q_OBJECT 头都有同名 .cpp，AUTOMOC 靠同名匹配就能生成
#   moc，无需再列头文件（列了反而会和主程序各生成一份）。
#   用 offscreen 平台跑，不需要显示器。
# =================================================
set(VIDEOEYE_UI_TEST_SOURCES ${UI_SOURCES})
list(FILTER VIDEOEYE_UI_TEST_SOURCES EXCLUDE REGEX "ui/app/main\\.cpp$")
videoeye_add_test(test_scan_terminal_state
    unit/test_scan_terminal_state.cpp
    ${VIDEOEYE_UI_TEST_SOURCES}
)
# 与主程序一致: ui/ 内部有按子目录写的 include，需要把 ui/ 自己加进来。
target_include_directories(test_scan_terminal_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui)
target_link_libraries(test_scan_terminal_state PRIVATE
    VideoEyeQtAdapters
    VideoEyePlayback
    VideoEyeExporter
    VideoEyeReporting
    VideoEyeQc
    VideoEyeFfmpegTools
    VideoEyeAnalysis
    VideoEyeMedia
    VideoEyeDomain
    VideoEyeInfrastructure
    Qt6::Widgets Qt6::Gui Qt6::Core)
add_test(NAME ScanTerminalStateTests COMMAND test_scan_terminal_state)
set_tests_properties(ScanTerminalStateTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 32：EventTimelineView 三个开关的初始状态（评审 P1-4）
# 说明: 控件是在 AnalysisPanel 注入功能开关钩子之前建的，注入后必须回写，
#       否则界面显示「启用分析」而面板里其实是关的。用 offscreen 平台跑，
#       不需要显示器；只构造控件、不渲染，因此直接编 EventTimelineView 的源。
# =================================================
videoeye_add_test(test_event_timeline_view
    unit/test_event_timeline_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/EventTimelineView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_event_timeline_view PRIVATE
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME EventTimelineViewToggleTests COMMAND test_event_timeline_view)
set_tests_properties(EventTimelineViewToggleTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 37：码流/宏块新组件（评审 P2-8）
# 说明: 与测试 32 同一类坑 —— 「启用分析」控件在组件构造时就建好了，而面板注入
#       feature 钩子发生在那之后，注入时必须把真实状态回写控件。另外守住
#       FramePacketView → StreamOverviewView 的 GOP 摘要桥接：这条链断了不会崩，
#       只会让「最大GOP大小」与 GOP 分布曲线永远停在上一次的值，最难在界面上发现。
#       用 offscreen 平台跑，不需要显示器；直接编这三个组件的源。
# =================================================
videoeye_add_test(test_stream_views
    unit/test_stream_views.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/FramePacketView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/StreamOverviewView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/MacroblockView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
# FramePacketView.cpp 直接用 AV_PICTURE_TYPE_* / AV_PKT_FLAG_* / AVMEDIA_TYPE_*
# （FFmpeg 公共常量），所以这里要拿到 avcodec 的 include 目录。
target_link_libraries(test_stream_views PRIVATE
    VideoEyeDomain
    VideoEyeReporting
    FFmpeg::avcodec
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME StreamViewsToggleTests COMMAND test_stream_views)
set_tests_properties(StreamViewsToggleTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 29：FFmpeg 命令工作台（命令行解析 / 指令字典 / 命令解释）
# 说明: 只测"文本 → 参数数组 → 解释"这条链，不启动任何子进程 ——
#       真跑 ffmpeg 依赖机器上有没有装，属于集成验收而不是单元测试。
#       FfmpegProcessRunner 用了 QProcess，所以额外链 Qt6::Core。
# =================================================
videoeye_add_test(test_ffmpeg_command
    unit/test_ffmpeg_command.cpp
)
target_link_libraries(test_ffmpeg_command PRIVATE VideoEyeFfmpegTools)
target_link_libraries(test_ffmpeg_command PRIVATE Qt6::Core)
add_test(NAME FfmpegCommandTests COMMAND test_ffmpeg_command)

# =================================================
# 测试 30：FFmpeg 进程执行器（QProcess 生命周期）
#
# 说明: 覆盖面是"纯逻辑测试碰不到"的那一层 —— 启动失败怎么报、stdout/stderr
#       有没有分流、点停止时到底有没有往 stdin 写 q、停完会不会误杀下一次运行、
#       工作目录会不会残留。这些都只发生在真实子进程上，光看代码看不出来。
#
#       为什么手工 add_executable 而不是用 videoeye_add_test(): 这个测试文件
#       自己写 main()（子进程模式要在进 gtest 之前接管），而 videoeye_add_test()
#       固定链 GTest::gtest_main，两个 main 会直接冲突。所以这里链 GTest::gtest。
# =================================================
add_executable(test_ffmpeg_process
    unit/test_ffmpeg_process.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/ffmpeg/FfmpegProcessRunner.cpp
)
target_include_directories(test_ffmpeg_process PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ffmpeg_process PRIVATE GTest::gtest Qt6::Core)
set_target_properties(test_ffmpeg_process PROPERTIES AUTOMOC ON)
videoeye_set_ffmpeg_rpath(test_ffmpeg_process)
videoeye_register_runtime_target(test_ffmpeg_process ${VIDEOEYE_GTEST_DLLS})
add_test(NAME FfmpegProcessTests COMMAND test_ffmpeg_process)

# =================================================
# 测试 31：报告页助手（导出路径唯一性 / 不可写目录导出失败 / 线程回收 / 扫描状态映射）
# 说明: ReportingPanel 的导出与线程逻辑抽到 ui/reporting_panel/{report_path,analysis_task}.h，
#       这里只测纯逻辑，不需要 Qt 事件循环或真实媒体文件。QcRuleEngine 的扫描状态映射
#       扫描状态映射读 QcRuleEngine 的规则表，所以链 VideoEyeReporting（它带 Qc）。
# =================================================
videoeye_add_test(test_reporting_helpers
    unit/test_reporting_helpers.cpp
)
target_link_libraries(test_reporting_helpers PRIVATE VideoEyeReporting)
target_link_libraries(test_reporting_helpers PRIVATE Qt6::Core)
add_test(NAME ReportingHelpersTests COMMAND test_reporting_helpers)

# =================================================
# 测试 32：五个容器结构分析器的参数化健壮性（评审第 10 项）
# 说明: Asf / Avi / Flv / Ogg / Ts 的 Analyze() 签名完全一致，却长期零覆盖。
#       这里用 TEST_P 跑"五分析器 × 三种输入"：文件不存在 / 魔数对不上 /
#       最小合法文件。样本字节全部内存合成，不依赖任何真实媒体文件。
#
#       只链这五个 .cpp 而不是整个 VideoEyeAnalysis: 它们只依赖 Qt 的文件 IO
#       加 domain 的值类型，链整层会把 FFmpeg 和一众分析器拖进来，编译慢一个量级。
#       Ogg 不校验魔数（刻意的容错），单独一个用例把这个差异固化下来。
# =================================================
videoeye_add_test(test_container_structure_analyzers
    unit/test_container_structure_analyzers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/container/AsfStructureAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/container/AviStructureAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/container/FlvStructureAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/container/OggStructureAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/container/TsStructureAnalyzer.cpp
)
target_link_libraries(test_container_structure_analyzers PRIVATE Qt6::Core VideoEyeDomain)
add_test(NAME ContainerStructureAnalyzerTests COMMAND test_container_structure_analyzers)