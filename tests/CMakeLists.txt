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
# 架构规则本身也进 ctest
#
# 以前 check_layering.py 只挂在 pre-commit 和 CI 的 lint job 里 —— 本地 ctest 里
# 看不到；而它又有过「打印 [!] 却 return 0」的历史（那等于门是白装的，2026-10-05 修）。
# 放进 ctest 之后规则有了能被机器盯住的落点，两个用例都是秒级。
#
# 这两条不是 gtest 可执行文件，走不了 videoeye_add_test()（那个固定链 GTest::gtest_main），
# 所以直接 add_test 调 python。
find_program(VIDEOEYE_PYTHON_EXE NAMES python3 python py)
if(VIDEOEYE_PYTHON_EXE)
    # 这些脚本大多用「VIDEOEYE_ROOT 或者当前目录」当仓库根，而 add_test 的默认工作
    # 目录是二进制目录（build/test-release/tests）—— 不加 WORKING_DIRECTORY 的话
    # ROOT 会落在 build 里，os.walk 扫到一个不存在的 core/，于是「一个依赖都没查到」
    # 直接被当成「没有跨层反向依赖」放行。这是假绿，不是通过。
    # 改成把工作目录钉在源码根，与 pre-commit / CI 跑脚本时的目录一致。
    #
    # 用法：
    #     ctest -R LayeringRulesTests --preset <本机 test preset>
    # preset 名见 `cmake --list-presets`（本机文件可能叫 test-release，
    # 也可能叫 win-test-release-local，build.bat 会自动探测，别写死）。
    add_test(NAME LayeringRulesTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check_layering.py)
    # 光看退出码不够：check_layering.py 有「打印了 [!] 却 return 0」的前科，
    # 再加一道输出断言，逼它必须真的打印放行文案才算过。
    set_tests_properties(LayeringRulesTests PROPERTIES
        PASS_REGULAR_EXPRESSION "没有跨层反向依赖")
    add_test(NAME LayeringSelfTestTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_check_layering.py)
    # analysis 层不许沾 Qt。查的是比 check_layering 更粗也更该管的一件事：
    # Qt 类型在 core/analysis 内部的使用（QString / QByteArray / QFile / QDataStream /
    # QMap ...）。check_layering 看不见它们，因为那层的 Qt include 全写成尖括号
    # #include <QString>（相对仓库根的引号形式检查会漏掉），而 audit_qt_domain_border
    # 只管"Qt 有没有落进 domain 的 std::string 字段"，是字段级、不是文件级。
    add_test(NAME QtAnalysisBorderTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/audit_qt_analysis_border.py)
    add_test(NAME QtAnalysisBorderSelfTestTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_audit_qt_analysis_border.py)
    # 命名空间跟不跟目录走（2026-10-05 清完 analyzer / utils 之后立的门）。
    # 三条规则：不许再声明 namespace analyzer / utils、不许写回 analyzer:: 或 utils::
    # 形式的引用、videoeye 下的具名子命名空间必须与所在文件路径的上级目录同名。
    # 最后一条会顺带抓出新冒出来的偏差 —— 只有登记进 KNOWN_DEVIATIONS 的才放行。
    add_test(NAME NamespaceLayoutTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/audit_namespace_layout.py)
    add_test(NAME NamespaceLayoutSelfTestTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_audit_namespace_layout.py)
    # Qt 类型不许从生产侧直接落进 domain 的 std::string 字段（字段级污染）。
    # 与 check_layering（include 方向）、audit_qt_analysis_border（analysis 层整体
    # 不许沾 Qt）查的都不是同一件事。CI 的 layering job 一直在跑它，
    # 本地 ctest 以前看不见 —— 2026-10-07 补注册，消掉「CI 红、本地绿」的落差。
    add_test(NAME QtDomainBorderTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/audit_qt_domain_border.py)
    add_test(NAME QtDomainBorderSelfTestTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_audit_qt_domain_border.py)
    # ⚠️ 这几个脚本测试都靠「VIDEOEYE_ROOT 或当前目录」当仓库根（见各脚本顶部
    # ROOT = os.environ.get("VIDEOEYE_ROOT") or os.getcwd()）。WORKING_DIRECTORY 已经
    # 钉在源码根，但开发者 shell 里万一 export 了一个**过时**的 VIDEOEYE_ROOT（旧 checkout），
    # 脚本就会去扫那份旧树、报旧违规，于是 LayeringRulesTests 莫名其妙 FAIL，而自测
    # （test_check_layering.py 里显式 pop 掉 VIDEOEYE_ROOT）却一路绿——极难排查。
    # 这里把 VIDEOEYE_ROOT 强制钉到真实源码根，让测试与机器环境解耦，谁 export 歪了都不影响。
    set_tests_properties(LayeringRulesTests LayeringSelfTestTests
                                            QtAnalysisBorderTests
                                            QtAnalysisBorderSelfTestTests
                                            NamespaceLayoutTests
                                            NamespaceLayoutSelfTestTests
                                            QtDomainBorderTests
                                            QtDomainBorderSelfTestTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "VIDEOEYE_ROOT=${CMAKE_SOURCE_DIR}")
else()
    message(STATUS "未找到 python，跳过分层规则测试（CI 与 pre-commit 仍会跑）")
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

# =================================================
# 测试 4e：OpenController 封面图判定的边界（评审 P0-1）
# 说明: 旧实现先取 streams[video_index] 再判 video_index < 0，音频-only 文件里
#       video_index == -1，越界读可能直接崩溃。判定抽成 IsCoverArtStream() 后，
#       用内存合成的 AVFormatContext 覆盖 音频-only / 空上下文 / 越界 / 普通视频 /
#       带封面图 五种输入，不需要真实媒体样本。
# =================================================
videoeye_add_test(test_open_controller_cover_art unit/test_open_controller_cover_art.cpp)
target_link_libraries(test_open_controller_cover_art PRIVATE
    VideoEyePlayback
    Qt6::Core Qt6::Gui
)
add_test(NAME OpenControllerCoverArtTests COMMAND test_open_controller_cover_art)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/domain/model/FrameData.cpp
    # 深拷贝要按 av_pix_fmt_desc_get 算色度平面行数，属 player 层（domain 禁 FFmpeg）
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/player/FrameDataCopy.cpp
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
# 测试 18b：AnalysisEngine 的失败终态一致性（复查 P2）
# 说明: 断言失败分支把 scan_status 改成 Failed —— 它的默认值是 Complete，
#       只发 on_failed 不动状态会让"回调说失败、结果对象说完成"同时成立。
#       失败分支不走 on_finished，所以结果对象只能靠 Run() 的 out_result 出参取。
#       清单分支（解析失败）不需要真实媒体文件；打开失败那条用"文件不存在"触发。
# =================================================
videoeye_add_test(test_streaming_scan_status
    unit/test_streaming_scan_status.cpp
)
target_link_libraries(test_streaming_scan_status PRIVATE VideoEyeAnalysis)
add_test(NAME StreamingManifestScanStatusTests COMMAND test_streaming_scan_status)

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
# StopWorker() 在"工作线程没按时响应停止"时会打一条 ERROR —— 那是这类问题唯一的可观测
# 出口: 它跑在析构路径上, 不适合抛异常, 调用方也普遍不查返回值。只要被测实现还在用
# Logger, 这条边就必须在这里显式声明 —— 本目标刻意只链了算法那两个源文件, 拿不到基础设施。
target_link_libraries(test_visual_defect PRIVATE VideoEyeInfrastructure)
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
# 测试 26b：QcRunner 单文件执行器（生命周期错误路径 / 预算内结算 / join_budget_ms=0 超时 detach）
# 说明: 用"垃圾内容 + .mp4 扩展名"的假文件触发引擎打开失败与超时, 不需要真实媒体样本;
#       超时用例返回后测试继续存活, 是 ASan 对"detach 后悬空访问引擎"的回归落点。
# 本目标只链 VideoEyeQc（不带 Qt）—— 同时也是"QC 模块零 Qt 依赖"的编译证据。
# =================================================
videoeye_add_test(test_qc_runner
    unit/test_qc_runner.cpp
)
target_link_libraries(test_qc_runner PRIVATE VideoEyeQc)
add_test(NAME QcRunnerTests COMMAND test_qc_runner)

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
# 测试 42：播放器的事务式异步打开（评审项 6）
#
# 说明: 打开/探测以前跑在 UI 线程上, 网络源/大文件会把界面整段冻住。现在拆成
#       Prepare(后台线程) -> Commit(UI 线程) 两阶段事务。这里锁定三条契约 ——
#       ① OpenResult 的采用/丢弃是原子的 ② OpenAsync 无论成败都恰好一个终态
#       ③ 被后续打开取代的旧结果必须丢弃(不发 OpenFinished)。
#       慢输入用 BlockingTcpEndpoint 造, 需要事件循环, 不需要 QPA 平台插件。
# =================================================
videoeye_add_test(test_player_open_async
    unit/test_player_open_async.cpp
)
target_link_libraries(test_player_open_async PRIVATE
    VideoEyePlayback
    Qt6::Core Qt6::Gui)
# BlockingTcpEndpoint.h 走 winsock，Windows 上需要显式 ws2_32。
if(WIN32)
    target_link_libraries(test_player_open_async PRIVATE ws2_32)
endif()
add_test(NAME PlayerOpenAsyncTests COMMAND test_player_open_async)

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
    #ReportingPanel 已经从 UI_SOURCES 里剔出去了（ui/reporting_panel 独立成库，
    # 否则同一份 Q_OBJECT 会被主程序和这个测试各编一份），所以这里补链它 ——
    # AnalysisPanel / MainWindow 都直接引用 ReportingPanel。
    VideoEyeReportingPanel
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
    # FramePacketView 的四个子页（视频帧/GOP、包、音频帧）都已各自抽成独立组件，
    # 本目标手工列源（不走 UI_SOURCES），漏了会在链接期报符号未解析。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/VideoFrameTableWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/PacketTableWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioFrameTableWidget.cpp
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
# 测试 33：报告页关闭时仍有阻塞任务（评审第 5 项）
# 说明: 面板的关停路径（取消 + alive=false + 带预算回收）过去零覆盖，而它是 ui/ 里
#       唯一带后台线程的一页 —— 卡在阻塞 IO 上关界面，历史上是"无限等待"级别的缺陷。
#       面板已从 ui/*.cpp 剔出成 VideoEyeReportingPanel（Q_OBJECT 类编两份 = ODR 违规），
#       测试才能把它链进来。跑这个用例要 offscreen 平台，见下面的 ENVIRONMENT。
# =================================================
videoeye_add_test(test_reporting_panel_lifecycle
    unit/test_reporting_panel_lifecycle.cpp
)
target_link_libraries(test_reporting_panel_lifecycle PRIVATE
    VideoEyeReportingPanel
    Qt6::Widgets
)
# BlockingTcpEndpoint.h 走 winsock，Windows 上需要显式 ws2_32（FFmpeg 的导入库不带它）。
if(WIN32)
    target_link_libraries(test_reporting_panel_lifecycle PRIVATE ws2_32)
endif()
add_test(NAME ReportingPanelLifecycleTests COMMAND test_reporting_panel_lifecycle)
set_tests_properties(ReportingPanelLifecycleTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
# =================================================
# 测试 34：ContainerStructureAnalyzer 四条分发路径的取消传播（复查 P2）
# 说明: EBML(MKV/WebM) 这一路以前没把取消令牌传给 ExtractEbmlStreamInfo, valid 在
#       它返回后立刻置位, 元素计数递归连一次检查都没有 —— 取消只有恰好落在
#       EbmlAnalyzer 自己的循环里才生效。这里把 MP4 / MKV / HLS / DASH 压成同一组
#       断言: 取消后不得产出 valid 结果、不得回退 FFmpeg、且取消必须发生在格式分派
#       之后(否则测到的只是入口守卫)。取消点由"不取消跑一遍"的耗时校准, 不靠固定
#       sleep —— 免得快机器/慢机器上落点漂移。
#       样本全部内存合成后落盘, 不依赖任何真实媒体文件。
# =================================================
videoeye_add_test(test_container_cancel_paths
    unit/test_container_cancel_paths.cpp
)
target_link_libraries(test_container_cancel_paths PRIVATE VideoEyeAnalysis Qt6::Core)
add_test(NAME ContainerCancelPathTests COMMAND test_container_cancel_paths)

# =================================================
# 测试 33：取消 / 异常 / 畸形码流的回归（VVC QP 表无上界、容器解析器不传取消令牌、
#孤儿线程不计并发预算、Qt worker 异常逃逸）
# 说明: 这里刻意只链用到的几个 .cpp，不拉整层 VideoEyeAnalysis —— IsobmffParser 在
#       core/media、VvcBitstreamParser 在 core/analysis/codec，都不该为一次单测
#       把 FFmpeg 拖进来。TaskManager 与 Cancellation 是纯基础设施，单独链。
# =================================================
videoeye_add_test(test_cancel_and_robustness
    unit/test_cancel_and_robustness.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/codec/VvcBitstreamParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/codec/BitReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/media/container/IsobmffParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/qt/QtWorkerOwner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/concurrency/TaskManager.cpp
)
target_link_libraries(test_cancel_and_robustness PRIVATE Qt6::Core VideoEyeDomain VideoEyeInfrastructure)
add_test(NAME CancelAndRobustnessTests COMMAND test_cancel_and_robustness)

# =================================================
# 测试 40：analysis 去 Qt 留下的三件过渡形态（AnalysisTextUtil / BytesOrder /
# SeqFileReader）
# 说明: 这三个头都是 analysis 层内部的 Qt 替身，写它们的初衷是"迁移期唯一允许的
# 过渡形态"（scripts/audit_qt_analysis_border.py 只放行它们）。替身自己也会漂 ——
# 尤其是 StrCat 的 %% 语义和 SeqFileReader "EOF 之后还能 seek 回去"这两处，
# 出了岔子都只在打开真实媒体文件时才看得见，所以钉成单测。
#
# 这里刻意**不链任何 VideoEye*/Qt 目标**：被测的三件东西全是 header-only，
# 这条用例因此也顺带成了"analysis 的过渡头不拖依赖"的证据。
# =================================================
videoeye_add_test(test_analysis_text_util
    unit/test_analysis_text_util.cpp
)
add_test(NAME AnalysisTextUtilTests COMMAND test_analysis_text_util)

# =================================================
# 测试 48：画面质量页（评审 P3「组件可单独测试只兑现了一部分」的补测）
# 说明: VisualDefectPage 是全面板唯一同时具备 ①UI<->options 双向往返 ②档位枚举->下拉
#       userData 一对一映射 ③只在 pending 上置脏、定时器到点才重绘 三条链的组件。
#       这三条断了都不崩，只会静默用错参数或界面停在旧值。
#       只编 VisualDefectPage 与它唯一的外部依赖 MetricChartWidget（header-only 的
#       AnalysisPageSupport 不用编），不整层拉 analysis —— 单测不该为一次界面检查
#       拖进 FFmpeg。控件加了 objectName（VisualDefectPreset / VisualDefectBlurThreshold /
#       VisualDefectFreezeThreshold / VisualDefectCaptureRgb / VisualDefectEnable），
#       测试才不必靠创建顺序猜 findChildren 的下标。
# =================================================
videoeye_add_test(test_visual_defect_page
    unit/test_visual_defect_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/VisualDefectPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_visual_defect_page PRIVATE
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME VisualDefectPageTests COMMAND test_visual_defect_page)
set_tests_properties(VisualDefectPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 49：文件结构页（容器结构 / MP4 样本表 / EBML 详情的页路由）
# 说明: 这个页同时接通用结构树、MP4 box 表、MP4 样本表、EBML 详情表四种数据源，
#       并按 format 在 QStackedWidget 上切到正确详情页。几条路由写错一个分支界面就
#       停在旧页/空表且不崩，属于最难手动点测发现的回归。直接编页面源 +
#       MetricChartWidget（AnalysisPageSupport.h 的 TableBatch 在那里），不整层拉
#       analysis / FFmpeg。VE_PERF 走 infrastructure 的 Logger，所以补链
#       VideoEyeInfrastructure。用 offscreen 平台跑，不需要显示器。
# =================================================
videoeye_add_test(test_container_structure_page
    unit/test_container_structure_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/ContainerStructurePage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/Mp4SampleTableWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_container_structure_page PRIVATE
    VideoEyeDomain
    VideoEyeInfrastructure
    VideoEyeReporting
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME ContainerStructurePageTests COMMAND test_container_structure_page)
set_tests_properties(ContainerStructurePageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 50：播放控制栏（音量/静音本地 handler + 控件初值）
# 说明: ControlBarWidget 从 PlayerPanel 抽出后只保留音量/静音两个纯本地 handler，
#       状态全压在组件内部（last_volume_）。一旦改错不会崩，只会让"静音后恢复"恢复成
#       错误音量、或提示文案与滑块值分家。本目标只编组件源；注入 MediaPlayer 的那条
#       用例要走到 SetVolume，所以链 VideoEyePlayback（它 PUBLIC 带出 domain/Qt/FFmpeg）。
#       用 offscreen 平台跑，不需要显示器。
# =================================================
videoeye_add_test(test_control_bar_widget
    unit/test_control_bar_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/player/ControlBarWidget.cpp
)
target_link_libraries(test_control_bar_widget PRIVATE
    VideoEyePlayback
    Qt6::Widgets)
add_test(NAME ControlBarWidgetTests COMMAND test_control_bar_widget)
set_tests_properties(ControlBarWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 51：音频可视化渲染器（未注入目标的防护 + 音量/频谱/波形落到渲染目标）
# 说明: AudioVizRenderer 把音量/频谱/波形画到注入的 VideoWidget 上。渲染目标
#       VideoWidget 还没有独立静态库（属 UI_SOURCES），所以这里显式编它的源；
#       VideoWidget.cpp include 了 Logger 头，补链 VideoEyeInfrastructure；
#       渲染结果靠 QWidget::render() 取像素比较，需要 offscreen 平台。
# =================================================
videoeye_add_test(test_audio_viz_renderer
    unit/test_audio_viz_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/player/AudioVizRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_window/VideoWidget.cpp
)
target_link_libraries(test_audio_viz_renderer PRIVATE
    VideoEyeDomain
    VideoEyeInfrastructure
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME AudioVizRendererTests COMMAND test_audio_viz_renderer)
set_tests_properties(AudioVizRendererTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 52：Raw 图像序列（扩展名守卫 / 分帧越界防护 / 导航状态回写）
# 说明: 裸流参数对话框（QInputDialog）必须人工输入，不进单测；这里覆盖的是它前后
#       的纯逻辑分支。UpdateRawNavigationState 要回写 ControlBarWidget，所以连它的源
#       一起编（ControlBarWidget.cpp 里的 SetVolume 调用链仍需 VideoEyePlayback 提供符号）。
#       用 offscreen 平台跑，不需要显示器。
# =================================================
videoeye_add_test(test_raw_image_sequence
    unit/test_raw_image_sequence.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/player/RawImageSequence.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/player/ControlBarWidget.cpp
    # RawImageSequence::ShowRawFrame 直接调 VideoWidget::SetFrame（非内联），
    # 渲染目标还没有独立静态库，所以把它的源一起编进来。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_window/VideoWidget.cpp
)
target_link_libraries(test_raw_image_sequence PRIVATE
    VideoEyePlayback
    VideoEyeInfrastructure
    Qt6::Widgets)
add_test(NAME RawImageSequenceTests COMMAND test_raw_image_sequence)
set_tests_properties(RawImageSequenceTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 53：音频帧表子页（脏标志 + 增量刷新 + 单元格 + 汇总聚合）
# 说明: 组件自身的记录缓存/脏标志/同步游标三者对不上时，只会静默少行或汇总错。
#       AnalysisPageSupport.h（header-only）里带 MetricChartWidget.h 的 Q_OBJECT，
#       AUTOMOC 会为它生成 moc，所以把 MetricChartWidget.cpp 一并编进来（与测试 48/49/37
#       同处理），不整层拉 analysis / FFmpeg。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_audio_frame_table_widget
    unit/test_audio_frame_table_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioFrameTableWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_audio_frame_table_widget PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME AudioFrameTableWidgetTests COMMAND test_audio_frame_table_widget)
set_tests_properties(AudioFrameTableWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 54：视频帧表子页（GOP 派生 + 双表刷新 + 帧类型筛选 + Reset 通知）
# 说明: GOP 分段是从帧记录 pict_type 推导的派生结果，段边界/计数错了界面不崩只停旧值。
#       组件源直接 include <libavcodec/avcodec.h> 取 AV_PICTURE_TYPE_* 常量（编译期，无需
#       链 FFmpeg 库）；同测试 53，补编 MetricChartWidget.cpp 满足 AUTOMOC。
# =================================================
videoeye_add_test(test_video_frame_table_widget
    unit/test_video_frame_table_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/VideoFrameTableWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_video_frame_table_widget PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME VideoFrameTableWidgetTests COMMAND test_video_frame_table_widget)
set_tests_properties(VideoFrameTableWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 55：FFmpeg 指令字典（分类/搜索过滤 + 滤镜插入补 -vf/-af + 无匹配禁用）
# 说明: 词条来自静态表 FfmpegCommandCatalog（core/ffmpeg，纯 Qt），组件自己搜；插入
#       语义（滤镜补 -vf/-af、末尾空格）是唯一会静默写错命令的规则。只编组件源 +
#       词典目录源，不启动任何子进程、不链 FFmpeg。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_ffmpeg_dictionary_widget
    unit/test_ffmpeg_dictionary_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ffmpeg_panel/FfmpegDictionaryWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/ffmpeg/FfmpegCommandCatalog.cpp
)
target_link_libraries(test_ffmpeg_dictionary_widget PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME FfmpegDictionaryWidgetTests COMMAND test_ffmpeg_dictionary_widget)
set_tests_properties(FfmpegDictionaryWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 56：包表子页（增量刷新 + 筛选重建/增量同语义 + 单元格/汇总 + Reset）
# 说明: 包表是帧表↔包表 PTS 互跳的记录侧，切筛选的「全量重建」与常规「增量补差」
#       必须共用同一份筛选语义和同步游标，错位只会静默少行/错行。组件源直接拿
#       AV_PKT_FLAG_* / AVMEDIA_TYPE_* 常量（编译期常量，无需链 FFmpeg 库）；
#       同测试 53，补编 MetricChartWidget.cpp 满足 AUTOMOC。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_packet_table_widget
    unit/test_packet_table_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/PacketTableWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_packet_table_widget PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME PacketTableWidgetTests COMMAND test_packet_table_widget)
set_tests_properties(PacketTableWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 57：FFmpeg 输出与解释区（状态行 + 命令解释页 + 双通道日志 + 终态文案）
# 说明: 这一区从 FfmpegPanel 抽出，把「命令一改就重算解释」「stderr 行加 [err] 前缀」
#       「失败切错误页」「优雅停止与被强杀分开报」四条规则压在组件内部，写错都不崩、
#       只静默显示旧值或错值。只编组件源 + 解释链的三个目录源（Parser / Explainer /
#       Catalog）+ AppTheme（组件用 monoFont），不启动任何子进程、不链 FFmpeg。
#       用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_ffmpeg_output_widget
    unit/test_ffmpeg_output_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ffmpeg_panel/FfmpegOutputWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/theme/AppTheme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/ffmpeg/FfmpegCommandParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/ffmpeg/FfmpegCommandExplainer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/ffmpeg/FfmpegCommandCatalog.cpp
)
target_link_libraries(test_ffmpeg_output_widget PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME FfmpegOutputWidgetTests COMMAND test_ffmpeg_output_widget)
set_tests_properties(FfmpegOutputWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 58：MainWindow 导出职责（进度框三态 + 双通道量程 + 取消路由 + 提示文案）
# 说明: 导出三入口与十条播放器导出信号接线从 MainWindow 抽成 ExportCoordinator。
#       进度框「建 / 复用 / 收」、帧导出 vs 音视频导出的量程差异、取消按钮路由都用
#       invokeMethod 驱动私有槽验证（先例 test_reporting_panel_lifecycle）；信号本身
#       是 MediaPlayer 侧契约，不从这里发射。编组件源 + MediaExportDialog（入口引用
#       的对话框）+ 真实 MediaPlayer（空闲 Cancel 安全，仅作非空依赖，不打开媒体）。
#       弹模态框的分支（三个导出入口与 Canceled / Error 收尾）不进单测。
#       用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_export_coordinator
    unit/test_export_coordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_window/ExportCoordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/dialogs/MediaExportDialog.cpp
)
target_link_libraries(test_export_coordinator PRIVATE
    VideoEyePlayback
    Qt6::Core Qt6::Gui Qt6::Widgets)
# VideoEyePlayback 的阻塞打开走 winsock，Windows 上需要显式 ws2_32。
if(WIN32)
    target_link_libraries(test_export_coordinator PRIVATE ws2_32)
endif()
add_test(NAME ExportCoordinatorTests COMMAND test_export_coordinator)
set_tests_properties(ExportCoordinatorTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 59：场景切换页（增量补表幂等 + 汇总聚合 + Reset 归零游标）
# 说明: AppendResult 与 FlushPending 之间隔着面板的批量节拍，同步游标
#       table_synced_count_ 算错（重复补行 / 漏行）或 Reset 忘归零都不会崩，
#       只会静默少行多行、换文件后第一批切换点永远不显示。断言表格增量行与
#       3 位小数汇总文案。编页面源 + MetricChartWidget（AnalysisPageSupport.h
#       经它引入 Q_OBJECT 头，AUTOMOC 需要）+ AppTheme（formatTime 时间戳文案），
#       链 VideoEyeDomain。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_scene_change_page
    unit/test_scene_change_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/SceneChangePage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/theme/AppTheme.cpp
)
target_link_libraries(test_scene_change_page PRIVATE
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME SceneChangePageTests COMMAND test_scene_change_page)
set_tests_properties(SceneChangePageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 60：宏块分析页（MV 像素精度换算 + HEVC/CTU 术语 + 500 行上限）
# 说明: 运动矢量按 motion_scale 换算像素、scale=0 按 1 兜底、术语随编码切换
#       （HEVC→CTU、块大小表 14 行分区）、I 帧预览文案、行数上限 500 —— 写错都不崩，
#       只会静默显示假像素值 / 错误术语。编页面源 + MetricChartWidget（同测试 59 的
#       AUTOMOC 依赖），链 VideoEyeDomain。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_macroblock_view
    unit/test_macroblock_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/MacroblockView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_macroblock_view PRIVATE
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME MacroblockViewTests COMMAND test_macroblock_view)
set_tests_properties(MacroblockViewTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 61：色彩与 HDR 页（缺失值高亮 + 异常表类别过滤 + 扫描态互斥）
# 说明: 「缺失 / 未标注」橙色加粗掉了只会变普通黑字；异常表收进非 ColorHdr 类别
#       会把视频 / 音频问题塞进色彩页；扫描态不同步会让页面卡在「扫描中」。三条
#       都是静默回归。三张表里的色彩 / HDR 表表头相同，测试经页内唯一 QTabWidget
#       按 tab 索引定位。编页面源 + MetricChartWidget（同测试 59 的 AUTOMOC 依赖），
#       链 VideoEyeDomain（BuildColorRows / BuildHdrRows 在 domain）。用 offscreen 跑。
# =================================================
videoeye_add_test(test_color_hdr_page
    unit/test_color_hdr_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/ColorHdrPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/charts/MetricChartWidget.cpp
)
target_link_libraries(test_color_hdr_page PRIVATE
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME ColorHdrPageTests COMMAND test_color_hdr_page)
set_tests_properties(ColorHdrPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
