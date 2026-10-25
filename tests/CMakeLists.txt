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
    # 链接可见性（2026-10-07 立的门）: 公共头闭包需要的 Qt / FFmpeg 依赖必须由该
    # target 的 PUBLIC 链接闭包提供 —— 挂错成 PRIVATE 时模块内部照样编译（include
    # 目录是仓库根），只有外部目标 include 它才会炸（P2-8 的 Playback -> QtAdapters）。
    # 立门时顺带修掉了三处历史不一致（FfmpegTools / QtAdapters / ReportingPanel）。
    add_test(NAME LinkVisibilityTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/audit_link_visibility.py)
    add_test(NAME LinkVisibilitySelfTestTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_audit_link_visibility.py)
    # Qt 类型不许从生产侧直接落进 domain 的 std::string 字段（字段级污染）。
    # 与 check_layering（include 方向）、audit_qt_analysis_border（analysis 层整体
    # 不许沾 Qt）查的都不是同一件事。CI 的 layering job 一直在跑它，
    # 本地 ctest 以前看不见 —— 2026-10-07 补注册，消掉「CI 红、本地绿」的落差。
    add_test(NAME QtDomainBorderTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/audit_qt_domain_border.py)
    add_test(NAME QtDomainBorderSelfTestTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_audit_qt_domain_border.py)
    # 阶段 3.1: 异常语料与生成脚本的一致性校验。
    # tests/corpus/ 里的每个畸形样本都由 generate_corpus.py 逐字节构造（这样做是为了让
    # "改坏了哪个字段"在 review 里可读）。--check 逐文件对比磁盘字节与脚本产物，
    # 改了脚本忘了重新生成时这条直接红，避免语料悄悄与脚本脱节。
    # 脚本用自身路径定位 corpus 目录，不依赖 WORKING_DIRECTORY / VIDEOEYE_ROOT。
    add_test(NAME CorpusGeneratorCheck
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/generate_corpus.py --check)
    # MSVC /showIncludes 前缀守卫：构建目录里 CMake 探测到的前缀，必须与 cl.exe
    # 此刻实际吐的前缀一致，否则 ninja 剥 /showIncludes 静默失败 —— 改了头文件
    # 不重编，链接出陈旧对象（Windows 上表现为 0xc0000409 / 0xc0000374 这种
    # "看起来像代码写坏了"的崩溃，实际是构建产物串味）。钉语言的启动器见
    # CMakePresets.json 的 windows-base。非 MSVC 环境脚本自己 SKIP，不会误红。
    add_test(NAME MsvcShowIncludesPrefixTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check_msvc_showincludes_prefix.py)
    add_test(NAME MsvcShowIncludesPrefixSelfTestTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_check_msvc_showincludes_prefix.py)
    # 真实封装样本（.ts/.m2ts/.avi/.ogg）的结构校验。与 CorpusGeneratorCheck 的分工：
    # 那边是逐字节比对的畸形样本，这边是 ffmpeg 产出的真实文件，只能做语义校验
    # （同步字节步长 / 容器魔数），因为 ffmpeg 输出不是字节可复现的。
    # 本机没生成过这些样本时脚本自己 SKIP，不阻塞没有 ffmpeg 的机器。
    add_test(NAME RealCorpusCheck
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/generate_real_corpus.py --check)
    add_test(NAME TestSummaryReadmeCheck
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/summarize_tests.py --check-readme)
    add_test(NAME TestSummarySelfTests
             COMMAND ${VIDEOEYE_PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_summarize_tests.py)
    # ⚠️ 这几个脚本测试都靠「VIDEOEYE_ROOT 或当前目录」当仓库根（见各脚本顶部
    # ROOT = os.environ.get("VIDEOEYE_ROOT") or os.getcwd()）。WORKING_DIRECTORY 已经
    # 钉在源码根，但开发者 shell 里万一 export 了一个**过时**的 VIDEOEYE_ROOT（旧 checkout），
    # 脚本就会去扫那份旧树、报旧违规，于是 LayeringRulesTests 莫名其妙 FAIL，而自测
    # （test_check_layering.py 里显式 pop 掉 VIDEOEYE_ROOT）却一路绿——极难排查。
    # 这里把 VIDEOEYE_ROOT 强制钉到真实源码根，让测试与机器环境解耦，谁 export 歪了都不影响。
    #
    # ❗ PYTHONIOENCODING 必须钉成 utf-8: 这几个脚本的输出全是中文，而 Windows runner
    # 的控制台代码页是 cp1252(en-US)。python 往 ctest 的管道里写时会直接抛
    # UnicodeEncodeError 崩掉，ctest 只看到 "Required regular expression not found"
    # （真正的 traceback 在紧接着的下一行，很容易被当成"脚本没输出"而查错方向），
    # 于是整组脚本测试在 CI 的 Windows 侧一路红，而本机（GBK / UTF-8 控制台）永远绿。
    # 钉死 stdout/stderr 编码之后就与控制台代码页无关了。
    set_tests_properties(LayeringRulesTests LayeringSelfTestTests
                                            QtAnalysisBorderTests
                                            QtAnalysisBorderSelfTestTests
                                            NamespaceLayoutTests
                                            NamespaceLayoutSelfTestTests
                                            LinkVisibilityTests
                                            LinkVisibilitySelfTestTests
                                            QtDomainBorderTests
                                            QtDomainBorderSelfTestTests
                                            MsvcShowIncludesPrefixTests
                                            MsvcShowIncludesPrefixSelfTestTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "VIDEOEYE_ROOT=${CMAKE_SOURCE_DIR};PYTHONIOENCODING=utf-8;PYTHONUTF8=1")
    # 这几个同样会打印中文，但工作目录各管各的（CorpusGenerator 靠脚本自身路径定位
    # corpus），所以只补编码、不动 WORKING_DIRECTORY。
    set_tests_properties(CorpusGeneratorCheck RealCorpusCheck
                         TestSummaryReadmeCheck TestSummarySelfTests PROPERTIES
        ENVIRONMENT "PYTHONIOENCODING=utf-8;PYTHONUTF8=1")
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
# 合成媒体工厂：多个测试共用，单独编一份
#
# 以前 4 个目标各自把 TestMediaFactory.cpp 编进 exe —— 它带 avcodec / avformat
# 的编码头，每份都要从头编一次。做成测试专用静态库后只编一遍，链接器按需取成员。
# ============================================================
add_library(videoeye_test_media_factory STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/support/TestMediaFactory.cpp)
target_include_directories(videoeye_test_media_factory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(videoeye_test_media_factory PUBLIC
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample)

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
# 测试 2: PerfMetrics (长期性能指标口径)
#
# 只链 infrastructure（纯 stdlib，不碰 Qt 业务逻辑），不整层拉 core ——
# 指标通道必须能被任何层（core / ui）直接引用，一旦拖上依赖就没人敢在热路径上埋点。
# ============================================================
videoeye_add_test(test_perf_metrics
    unit/test_perf_metrics.cpp
)
target_link_libraries(test_perf_metrics PRIVATE VideoEyeInfrastructure Qt6::Core)
add_test(NAME PerfMetricsTests COMMAND test_perf_metrics)

# ============================================================
# 需要 PerfMetrics / Logger 的测试一律链接 VideoEyeInfrastructure
#
# 以前这些目标各自手列 PerfMetrics.cpp + Logger.cpp（Logger 一度被 15 个 target
# 重复编译）。基础设施已经是静态库，链它即可 —— 链接器只取用到的成员，编译只发生一次。
# 别再把手列源码和链库混用：同一份 .cpp 进两个编译单元会撞 LNK2005 重复定义。
# ============================================================

# ============================================================
# 测试 2c: ChartDecimation (曲线抽稀统一口径)
#
# ChartSeries 用 owner=nullptr 构造（NotifyUpdate 判空），所以本测试不需要
# QApplication；曲线控件的实现与 moc 在 VideoEyeUiCharts 里，本目标链它即可。
# ============================================================
videoeye_add_test(test_chart_decimation
    unit/test_chart_decimation.cpp
)
target_link_libraries(test_chart_decimation PRIVATE
    VideoEyeInfrastructure
    VideoEyeUiCharts Qt6::Widgets Qt6::Gui Qt6::Core)
add_test(NAME ChartDecimationTests COMMAND test_chart_decimation)

# ============================================================
# 测试 2d: RecordTableModel (十万级明细表的 model/view 底座)
#
# 纯 Qt Core + Gui（QColor），不需要 QApplication。
# ============================================================
videoeye_add_test(test_record_table_model
    unit/test_record_table_model.cpp
)
target_link_libraries(test_record_table_model PRIVATE
    VideoEyeInfrastructure
    Qt6::Gui Qt6::Core)
add_test(NAME RecordTableModelTests COMMAND test_record_table_model)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/stream/StreamAnalyzer.cpp
)
target_link_libraries(test_stream_analyzer PRIVATE
    VideoEyeDomain
    VideoEyeInfrastructure
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME StreamAnalyzerTests COMMAND test_stream_analyzer)

# ============================================================
# 测试 4b: MediaInfoAnalyzer::FormatFromContext (依赖 FFmpeg)
# 说明: 打开链路与媒体信息页共用同一个文本组装入口（评审 P1：消除第四遍读文件）。
#       用合成的 AVFormatContext 验证空指针保护与文本骨架，不读真实媒体文件。
# ============================================================
videoeye_add_test(test_mediainfo_format
    unit/test_mediainfo_format.cpp
)
target_link_libraries(test_mediainfo_format PRIVATE VideoEyeAnalysis)
add_test(NAME MediaInfoFormatTests COMMAND test_mediainfo_format)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/stream/StreamAnalyzer.cpp
)
target_link_libraries(test_analysis_session PRIVATE
    VideoEyeDomain
    VideoEyeInfrastructure
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME AnalysisSessionTests COMMAND test_analysis_session)

# ============================================================
# 测试 5: FrameData 模型 (依赖 FFmpeg)
# ============================================================
videoeye_add_test(test_frame_data
    unit/test_frame_data.cpp
    # 深拷贝要按 av_pix_fmt_desc_get 算色度平面行数，属 player 层（domain 禁 FFmpeg）
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/player/FrameDataCopy.cpp
)
target_link_libraries(test_frame_data PRIVATE
    VideoEyeDomain
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
add_test(NAME FrameDataTests COMMAND test_frame_data)

# =================================================
# 测试 6：BitrateGopAnalyzer (不依赖 FFmpeg / Qt，喂合成样本)
# =================================================
videoeye_add_test(test_bitrate_gop_analyzer
    unit/test_bitrate_gop_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/quality/BitrateGopAnalyzer.cpp
)
target_link_libraries(test_bitrate_gop_analyzer PRIVATE VideoEyeDomain)
add_test(NAME BitrateGopAnalyzerTests COMMAND test_bitrate_gop_analyzer)

# =================================================
# 测试 6b：码率/GOP 全链路回归（真实媒体 + 完整分析）
# 说明: test_bitrate_gop_analyzer 喂合成样本钉分析器内部逻辑；这里用
#       TestMediaFactory 现场生成的确定性 H.264+AAC MP4 跑完整
#       AnalysisEngine::Run，decode_frame_types 开/关各一遍（解码路径 /
#       parser 路径），断言帧数、字节数、GOP 数与独立 demux 循环逐一对齐 ——
#       专防"一个语义帧被计两次"类回归（曾因解码循环内外各调一次 OnFrame）。
#       FFmpeg 的 include 与库从 VideoEyeAnalysis 的 PUBLIC 依赖传来。
# =================================================
videoeye_add_test(test_bitrate_gop_pipeline
    unit/test_bitrate_gop_pipeline.cpp
)
target_link_libraries(test_bitrate_gop_pipeline PRIVATE
    VideoEyeAnalysis
    videoeye_test_media_factory)
add_test(NAME BitrateGopPipelineRegressionTests COMMAND test_bitrate_gop_pipeline)

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
)
target_link_libraries(test_audio_qc_analyzer PRIVATE VideoEyeDomain)
add_test(NAME AudioQcAnalyzerTests COMMAND test_audio_qc_analyzer)

# =================================================
# 测试 9：音频 QC 规则 (AudioQcAnalyzer + QcRuleEngine，不依赖 FFmpeg)
# =================================================
# 说明: AudioQcAnalyzer 本身在 analysis 层；规则判定只需要链接 VideoEyeQcRules。
videoeye_add_test(test_audio_qc_rules
    unit/test_audio_qc_rules.cpp
)
target_link_libraries(test_audio_qc_rules PRIVATE VideoEyeAnalysis VideoEyeQcRules)
add_test(NAME AudioQcRulesTests COMMAND test_audio_qc_rules)

# =================================================
# 测试 9-B：音频位流头解析（ADTS / LOAS / AC-3 / E-AC-3 / Opus / AAC ASC）
# 说明: parser 是纯 C++17（不碰 FFmpeg / Qt），测试数据按规范字段顺序逐位拼出来。
# =================================================
videoeye_add_test(test_audio_bitstream_parser
    unit/test_audio_bitstream_parser.cpp
)
target_link_libraries(test_audio_bitstream_parser PRIVATE VideoEyeCodec)
add_test(NAME AudioBitstreamParserTests COMMAND test_audio_bitstream_parser)

# =================================================
# 测试 9-B2：MP3 / FLAC / AAC-PCE 帧头解析（阶段 D2）
# 说明: 期望值全部来自规范（ISO/IEC 11172-3、RFC 9639、ISO/IEC 14496-3 表 4.5），
#       不由实现生成 —— 只与实现共享假设的合成数据测不出语义错误。
# =================================================
videoeye_add_test(test_audio_frame_parsers
    unit/test_audio_frame_parsers.cpp
)
target_link_libraries(test_audio_frame_parsers PRIVATE VideoEyeCodec)
add_test(NAME AudioFrameParsersTests COMMAND test_audio_frame_parsers)

# =================================================
# 测试 9-B4：AAC SBR/PS 同步扩展 + LATM numSubFrames 帧计数（阶段 D3）
# 说明: SBR/PS 走 ASC 末尾的 0x2B7/0x548 同步扩展（AOT 之外的显式信令路径）；
#       LATM 部分验证"一个 AudioMuxElement 含 numSubFrames+1 帧"的计数口径。
#       包字节均按 ECMA-391 / ISO-IEC 14496-3 §1.6.2.1 逐位拼出。
# =================================================
videoeye_add_test(test_latm_packet_accounting
    unit/test_latm_packet_accounting.cpp
)
target_link_libraries(test_latm_packet_accounting PRIVATE VideoEyeAnalysis)
add_test(NAME LatmPacketAccountingTests COMMAND test_latm_packet_accounting)

# =================================================
# 测试 9-B3：参数集全程追踪（阶段 1：配置时间线）
# 说明: 覆盖"同 id 替换 / 换 id 新增 / 重复发送不算变化 / 引用未传输 / 长度前缀封装 /
#       上限截断 / 幂等 Finish"七类，正反例成对出现。
# =================================================
videoeye_add_test(test_parameter_timeline
    unit/test_parameter_timeline.cpp
)
target_link_libraries(test_parameter_timeline PRIVATE VideoEyeAnalysis)
add_test(NAME ParameterTimelineTests COMMAND test_parameter_timeline)

# =================================================
# 测试 9-B5：帧级 POC 追踪（阶段：帧级码流结构）
# 说明: A 组用按 H.264 7.3.2.1.1 / 7.3.3 / 8.2.1 逐位拼出的样本覆盖正反例
#       （IBBP 重排 / IDR 重置不算重排 / 多 slice / type 2 与 frame_num 回绕 /
#        缺帧 / POC 重复 / 无 SPS / type 1 明确不支持 / extradata 路径 / 明文上限）；
#       B 组用 libx264 现场编码的真实 MP4，拿编码器自己写的 PTS 当参考输出，
#       验证"按 POC 排出来的显示顺序 == 真实显示顺序"。
# =================================================
videoeye_add_test(test_h264_poc_tracker
    unit/test_h264_poc_tracker.cpp
)
target_link_libraries(test_h264_poc_tracker PRIVATE
    VideoEyeAnalysis
    videoeye_test_media_factory)
# 真实码流语料（tests/corpus/codec/real_x264_bframes_gop.mp4）。
# 带 B 帧的真实样本是"POC = 显示顺序"这条结论唯一有区分度的参考 ——
# 没有 B 帧时解码顺序与显示顺序天然相同，怎么算都对。
# ❗ 必须是 ffmpeg 命令行 + libx264 预生成的入库语料：预编译 FFmpeg 没有
# libx264，TestMediaFactory 会降级到 libopenh264（不出 B 帧），现场生成
# 等于把这条用例悄悄变成空跑。
target_compile_definitions(test_h264_poc_tracker PRIVATE
    VIDEOEYE_CODEC_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus/codec")
add_test(NAME H264PocTrackerTests COMMAND test_h264_poc_tracker)

# =================================================
# 测试 9-B4：证据与覆盖状态（阶段 5）
# 说明: 重点测 MarkConclusive 的口径边界 —— 这是全仓唯一能产生"假通过"的地方。
#       99% 覆盖率阈值、总量未知、失败计数三条边界都有正反例。
# =================================================
videoeye_add_test(test_dimension_status
    unit/test_dimension_status.cpp
)
target_link_libraries(test_dimension_status PRIVATE VideoEyeDomain)
add_test(NAME DimensionStatusTests COMMAND test_dimension_status)

# =================================================
# 测试 9-C：音频位流规则（audio.bitstream.* / audio.dialnorm.*）
# 说明: 直接构造 AudioBitstreamResult，验证"没跑过不报 / 结构化计数 / dialnorm 守卫"。
# =================================================
videoeye_add_test(test_audio_bitstream_qc
    unit/test_audio_bitstream_qc.cpp
)
target_link_libraries(test_audio_bitstream_qc PRIVATE VideoEyeQcRules)
add_test(NAME AudioBitstreamRulesTests COMMAND test_audio_bitstream_qc)

# =================================================
# 测试 9-D：隐藏字幕解码（CEA-608 状态机 + CEA-708 DTVCC）
# 说明: 两个解码器都是纯 C++17（不碰 FFmpeg / Qt）。测试自己拼 cc_data 字节流，
#       重点验"上屏时机 / 通道与服务分离 / 丢包计数"，而不是字符串抽取。
# =================================================
videoeye_add_test(test_cea_caption_decoder
    unit/test_cea_caption_decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/diagnostics/Cea608Decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/diagnostics/Cea708Decoder.cpp
)
target_link_libraries(test_cea_caption_decoder PRIVATE VideoEyeDomain)
add_test(NAME CeaCaptionDecoderTests COMMAND test_cea_caption_decoder)

# =================================================
# 测试 10：色彩与 HDR 元数据（QcRuleEngine）
# 说明: BuildColorRows / BuildHdrRows 已下放 domain（ColorHdrResult.cpp），
#       规则判定本身就是纯模型层 —— 本目标只链规则库，不再需要分析层与 FFmpeg。
# =================================================
videoeye_add_test(test_color_hdr_rules
    unit/test_color_hdr_rules.cpp
)
target_link_libraries(test_color_hdr_rules PRIVATE VideoEyeQcRules)
add_test(NAME ColorHdrRulesTests COMMAND test_color_hdr_rules)

# =================================================
# 测试 11：MP4/fMP4 容器一致性 (Mp4SampleTableAnalyzer 的纯逻辑校验)
# 说明: 只测 Validate()——手工构造合成样本表，不需要 Qt 或真实文件。
#       解析路径走自研 core/media/container/IsobmffParser.h（纯 stdlib），无需任何第三方容器库。
# =================================================
videoeye_add_test(test_mp4_sample_table
    unit/test_mp4_sample_table.cpp
)
target_link_libraries(test_mp4_sample_table PRIVATE
    VideoEyeAnalysisPackage
    VideoEyeMedia
    VideoEyeInfrastructure)
add_test(NAME Mp4SampleTableTests COMMAND test_mp4_sample_table)

# =================================================
# 测试 12：ISOBMFF 真实解析路径 (utils::IsobmffParser)
# 说明: 内存里拼一个最小合法的 MP4 再落盘解析，覆盖 stts/ctts/stz2 的字节级读取。
#       与测试 11 互补 —— 那边只测 Validate() 的静态校验，看不见解析阶段的问题。
# =================================================
videoeye_add_test(test_isobmff_parser
    unit/test_isobmff_parser.cpp
)
target_link_libraries(test_isobmff_parser PRIVATE VideoEyeMedia)
add_test(NAME IsobmffParserTests COMMAND test_isobmff_parser)

# =================================================
# 测试 13：H.264 码流解析 (profile/level 名称 + SPS/PPS + extradata 格式识别)
# 说明: 纯 stdlib，不需要 Qt / FFmpeg。
# =================================================
videoeye_add_test(test_h264_bitstream_parser
    unit/test_h264_bitstream_parser.cpp
)
target_link_libraries(test_h264_bitstream_parser PRIVATE VideoEyeCodec)
add_test(NAME H264BitstreamParserTests COMMAND test_h264_bitstream_parser)

# =================================================
# 测试 14：HEVC 码流解析 (profile/tier/level 名称 + VPS/SPS/PPS + hvcC 识别)
# =================================================
videoeye_add_test(test_hevc_bitstream_parser
    unit/test_hevc_bitstream_parser.cpp
)
target_link_libraries(test_hevc_bitstream_parser PRIVATE VideoEyeCodec)
add_test(NAME HevcBitstreamParserTests COMMAND test_hevc_bitstream_parser)

# =================================================
# 测试 15：BitstreamAnalyzer（容器侧快照 + 容器/码流对比分级）
# 说明: 这是「参数集解析」页的数据来源。只需要 FFmpeg 头文件（AVCodecID 枚举），
#       不调用任何 avcodec 函数，因此不必链接 FFmpeg 库。
#       带内参数集（InBandParameterSets）也在这里测：它和 extradata 入口共用
#       同一套解析器，只是"数据从哪来"不同。
# =================================================
videoeye_add_test(test_bitstream_analyzer
    unit/test_bitstream_analyzer.cpp
)
target_link_libraries(test_bitstream_analyzer PRIVATE VideoEyeCodec)
target_compile_definitions(test_bitstream_analyzer PRIVATE
    VIDEOEYE_CODEC_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus/codec")
add_test(NAME BitstreamAnalyzerTests COMMAND test_bitstream_analyzer)

# =================================================
# 测试 16：AV1 码流解析（序列头 OBU + av1C 配置记录）
# 说明: 与测试 13/14 同构，比特流由 _smoke/gen_av1_seqhdr.py 逐位生成。
# =================================================
videoeye_add_test(test_av1_bitstream_parser
    unit/test_av1_bitstream_parser.cpp
)
target_link_libraries(test_av1_bitstream_parser PRIVATE VideoEyeCodec)
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
)
target_link_libraries(test_streaming_package PRIVATE
    VideoEyeAnalysisPackage
    VideoEyeMedia
    VideoEyeInfrastructure)
add_test(NAME StreamingPackageTests COMMAND test_streaming_package)

# =================================================
# 测试 18c：HLS / DASH 的业务语义标签（广告 / 墙钟 / 快进档 / DRM / 事件流）
# 说明: 与 test_streaming_package 分工 —— 那份守"分片与码率阶梯"，这份守
#       "时间轴与业务标记"（EXT-X-DATERANGE / PROGRAM-DATE-TIME / I-FRAME-STREAM-INF /
#       START / SKIP，以及 DASH 的 ContentProtection / UTCTiming / EventStream /
#       Role / EssentialProperty）。纯文本解析，不需要任何媒体文件。
# =================================================
videoeye_add_test(test_streaming_descriptors
    unit/test_streaming_descriptors.cpp
)
target_link_libraries(test_streaming_descriptors PRIVATE VideoEyeAnalysisPackage)
add_test(NAME StreamingDescriptorTests COMMAND test_streaming_descriptors)

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
target_link_libraries(test_streaming_qc_rules PRIVATE VideoEyeAnalysisPackage VideoEyeQcRules)
add_test(NAME StreamingQcRulesTests COMMAND test_streaming_qc_rules)

# =================================================
# 测试 19b：MPEG-TS 传输层 QC 规则（container.ts.* 前缀分支）
# 说明: 规则只读 AnalysisResult::ts_transport，样本全部手工构造 —— 不需要真实
#       TS 文件。「解析器算得对不对」在 test_container_structure_analyzers 里测，
#       这里只测「规则有没有把数字转成报告、级别与阈值对不对」。
# =================================================
videoeye_add_test(test_container_ts_qc_rules
    unit/test_container_ts_qc_rules.cpp
)
target_link_libraries(test_container_ts_qc_rules PRIVATE VideoEyeQcRules)
add_test(NAME ContainerTsQcRulesTests COMMAND test_container_ts_qc_rules)

# =================================================
# 测试 19d：MPEG-TS PES 载荷级（最长无 IDR / PID 断流）
# 说明: 只读 PES 头看不出"一路没有 IDR"和"某个 PID 中途断了"。样本是手工构造的
#       TS（PAT + PMT + 视频/音频 PES 序列），不需要真实媒体文件。
# =================================================
videoeye_add_test(test_ts_pes_payload
    unit/test_ts_pes_payload.cpp
)
target_link_libraries(test_ts_pes_payload PRIVATE VideoEyeAnalysisPackage)
add_test(NAME TsPesPayloadTests COMMAND test_ts_pes_payload)

# =================================================
# 测试 19c：AVI / FLV / ASF / OGG 容器级 QC 规则
# 说明: 规则只读 AnalysisResult::container_diagnostics，样本全部手工构造 ——
#       不需要真实媒体文件。「解析器算得对不对」在 test_container_structure_analyzers 里测。
# =================================================
videoeye_add_test(test_container_legacy_qc_rules
    unit/test_container_legacy_qc_rules.cpp
)
target_link_libraries(test_container_legacy_qc_rules PRIVATE VideoEyeQcRules)
add_test(NAME ContainerLegacyQcRulesTests COMMAND test_container_legacy_qc_rules)

# =================================================
# 测试 21：视觉缺陷检测（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边）
# 说明: VisualDefectAnalyzer 只吃 model::FrameSample 里的降采样像素，不依赖 FFmpeg（
#       AVFrame -> FrameSample 的缩放在 QualityAnalyzer::BuildSample），所以这里全部用
#       合成图，不需要任何真实媒体文件。
# =================================================
videoeye_add_test(test_visual_defect
    unit/test_visual_defect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/analysis/quality/VisualDefectAnalyzer.cpp
)
target_link_libraries(test_visual_defect PRIVATE VideoEyeDomain VideoEyeInfrastructure)
# StopWorker() 在"工作线程没按时响应停止"时会打一条 ERROR —— 那是这类问题唯一的可观测
# 出口: 它跑在析构路径上, 不适合抛异常, 调用方也普遍不查返回值。这条依赖由上面的
# VideoEyeInfrastructure 提供。
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
target_link_libraries(test_aux_qc_rules PRIVATE VideoEyeQcRules)
add_test(NAME AuxQcRulesTests COMMAND test_aux_qc_rules)

# =================================================
# 测试 20：VVC (H.266) 码流解析（VPS/SPS/PPS + vvcC 记录 + NAL header 位域）
# 说明: VVC 的 NAL type 在第 2 字节高 5 位，和 HEVC 完全不同 —— 这一条
#       单独有测试兜底，避免以后改 ExtradataParser 时静默读错类型。
# =================================================
videoeye_add_test(test_vvc_bitstream_parser
    unit/test_vvc_bitstream_parser.cpp
)
target_link_libraries(test_vvc_bitstream_parser PRIVATE VideoEyeCodec)
add_test(NAME VvcBitstreamParserTests COMMAND test_vvc_bitstream_parser)

# =================================================
# 测试 23-B：VP9 码流解析（关键帧 uncompressed header）
# 说明: VP9 没有参数集，序列配置写在关键帧头部；测试数据由测试文件内的
#       逐位 BitWriter 按规范 6.2 生成，不参考 parser 实现。
# =================================================
videoeye_add_test(test_vp9_bitstream_parser
    unit/test_vp9_bitstream_parser.cpp
)
target_link_libraries(test_vp9_bitstream_parser PRIVATE VideoEyeCodec)
add_test(NAME Vp9BitstreamParserTests COMMAND test_vp9_bitstream_parser)

# =================================================
# 测试 23-C：码流能力门禁判据（Profile / 色度 / 位深）
# 说明: 判据抽成 header-only 就是为了能直接单测。C2 放宽 4:2:2 / 4:4:4
#       之后，贡献流（RExt 4:2:2 10bit）不再被一律判成 Partial。
# =================================================
videoeye_add_test(test_bitstream_capability_rules
    unit/test_bitstream_capability_rules.cpp
)
add_test(NAME BitstreamCapabilityRulesTests COMMAND test_bitstream_capability_rules)

# =================================================
# 测试 23-D：slice header / SEI 自研解析（C4：逐帧 QP + HDR10+/DV/MDCV/CLL/胶片颗粒）
# 说明: 以前这些全靠 FFmpeg 的 packet side data；本套用例从码流字节自己解一遍。
# =================================================
videoeye_add_test(test_video_es_inspector
    unit/test_video_es_inspector.cpp
)
target_link_libraries(test_video_es_inspector PRIVATE VideoEyeCodec)
# VideoEsInspector 内部按 AVCodecID 分支，需要 FFmpeg 的枚举与链接符号
target_link_libraries(test_video_es_inspector PRIVATE FFmpeg::avcodec FFmpeg::avutil)
add_test(NAME VideoEsInspectorTests COMMAND test_video_es_inspector)

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
target_link_libraries(test_qc_profile PRIVATE VideoEyeQcRules)
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
# 真实封装样本目录：端到端用例要用它验证 AnalysisEngine -> FillTsTransport
# 这条链路（容器解析器自己的正确性是另一回事，见 ContainerStructureAnalyzerTests）。
target_compile_definitions(test_qc_runner PRIVATE
    VIDEOEYE_CONTAINER_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus/container")
add_test(NAME QcRunnerTests COMMAND test_qc_runner)

# =================================================
# 测试 27：双文件对比（转码前后 / 同参数校验）
# 说明: CompareRuns 读 QcRunResult.report 字段即可产生对比行，无需完整 analysis。
# =================================================
videoeye_add_test(test_qc_comparator
    unit/test_qc_comparator.cpp
)
target_link_libraries(test_qc_comparator PRIVATE VideoEyeQcRules)
add_test(NAME QcComparatorTests COMMAND test_qc_comparator)

# =================================================
# 测试 28：报告导出（CSV / JSON / HTML / TXT / PDF 的产物矩阵 + 扩展名 + 批量汇总）
# 说明: 五种格式都不再只断言"文件写出来了" —— JSON 重新解析、CSV 数行与转义、
#       HTML/TXT 查关键字段、PDF 走 tests/support/PdfVerifier 校验对象关系与
#       xref。验证器是纯 std::string 实现，不引入 qpdf / poppler 之类的外部依赖。
# =================================================
videoeye_add_test(test_qc_report_exporter
    unit/test_qc_report_exporter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/support/PdfVerifier.cpp
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
    # 导出报告 E2E 用 QcReportExporter::ExportReport（诊断页导出按钮的同一条纯函数路径）
    VideoEyeReporting
    Qt6::Core)
# 取消路径用 BlockingTcpEndpoint.h 起本地监听端口，Windows 上需要显式 ws2_32。
if(WIN32)
    target_link_libraries(test_diagnostics_result_flow PRIVATE ws2_32)
endif()
add_test(NAME DiagnosticsResultFlowTests COMMAND test_diagnostics_result_flow)

# =================================================
# 测试 31：MediaExporter 错误传播 + 成功路径（评审 P1 / 阶段 1）
# 损坏输入必须上报 ExportError, 绝不误报 ExportFinished;
# 成功路径用 tests/support/TestMediaFactory 现场生成的 H.264+AAC 样本跑通
# remux / 转码 / 音频-only, 并把产物重新交给 FFmpeg 打开验证流数量与帧数。
# 工厂自己要用 avcodec/avformat/swresample 编码，FFmpeg 的 include 与库都从
# VideoEyeExporter 的 PUBLIC 依赖传过来，不必在这里重复列。
# =================================================
videoeye_add_test(test_media_exporter
    unit/test_media_exporter.cpp
)
target_link_libraries(test_media_exporter PRIVATE
    VideoEyeExporter
    videoeye_test_media_factory
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
# 阻塞取消用例用 BlockingTcpEndpoint.h 起本地监听端口，Windows 上需要显式 ws2_32。
if(WIN32)
    target_link_libraries(test_qt_analysis_controller PRIVATE ws2_32)
endif()
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
    VideoEyeInfrastructure
    videoeye_test_media_factory
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
# 为什么链 VideoEyeUi: 被测对象是 AnalysisPanel 本体。UI 已独立成静态库，
#   这里只编测试用例源文件并链库，不再把整层 ui/ 重复编进测试 exe —— 同一个
#   Q_OBJECT 类也只有 VideoEyeUi 里那一份 moc。用 offscreen 平台跑，不需要显示器。
# =================================================
videoeye_add_test(test_scan_terminal_state
    unit/test_scan_terminal_state.cpp
)
target_link_libraries(test_scan_terminal_state PRIVATE
    VideoEyeUi
    VideoEyeQtAdapters
    #ReportingPanel 独立成库；AnalysisPanel / MainWindow 都直接引用它，这里补链。
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
# 取消路径用例用 BlockingTcpEndpoint.h 起本地监听端口，Windows 上需要显式 ws2_32。
if(WIN32)
    target_link_libraries(test_scan_terminal_state PRIVATE ws2_32)
endif()
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
)
target_link_libraries(test_event_timeline_view PRIVATE
    VideoEyeDomain
    VideoEyeUiCharts
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
#       用 offscreen 平台跑，不需要显示器；这里只编 FramePacketView /
#       StreamOverviewView，四个子页走 VideoEyeUiAnalysisWidgets 库。
# =================================================
videoeye_add_test(test_stream_views
    unit/test_stream_views.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/FramePacketView.cpp
    # 视频帧 / 包 / 音频帧 / 宏块四个子页已抽成 VideoEyeUiAnalysisWidgets，
    # 这里链库即可，不再手列它们的源。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/StreamOverviewView.cpp
)
# FramePacketView.cpp 直接用 AV_PICTURE_TYPE_* / AV_PKT_FLAG_* / AVMEDIA_TYPE_*
# （FFmpeg 公共常量），所以这里要拿到 avcodec 的 include 目录。
target_link_libraries(test_stream_views PRIVATE
    VideoEyeDomain
    VideoEyeInfrastructure
    VideoEyeReporting
    VideoEyeUiCharts
    VideoEyeUiAnalysisWidgets
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
)
target_include_directories(test_ffmpeg_process PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ffmpeg_process PRIVATE
    GTest::gtest
    # FfmpegProcessRunner 就在 VideoEyeFfmpegTools 里，别再手列一份源码。
    VideoEyeFfmpegTools
    Qt6::Core)
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
target_link_libraries(test_reporting_helpers PRIVATE VideoEyeReporting
    VideoEyeQc)
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
# 测试 32：七个容器结构分析器的参数化健壮性（评审第 10 项）
# 说明: Asf / Avi / Flv / Ogg / Ts / MpegPs / Mxf 的 Analyze() 签名完全一致，却长期零覆盖。
#       这里用 TEST_P 跑"七分析器 × 三种输入"：文件不存在 / 魔数对不上 /
#       最小合法文件。样本字节全部内存合成，不依赖任何真实媒体文件。
#
#       只链 VideoEyeAnalysisPackage（叶子库）而不是整个 VideoEyeAnalysis:
#       解析器只依赖 domain 的值类型与 infrastructure 的取消/日志，链整层会把 FFmpeg
#       和一众分析器拖进来；叶子库已把这份解析器源码只编一份，这里不必再手列 .cpp。
#       Ogg 不校验魔数（刻意的容错），单独一个用例把这个差异固化下来。
# =================================================
videoeye_add_test(test_container_structure_analyzers
    unit/test_container_structure_analyzers.cpp
)
target_link_libraries(test_container_structure_analyzers PRIVATE
    VideoEyeAnalysisPackage Qt6::Core VideoEyeDomain)
# 真实封装样本（tests/corpus/container/real_*.{ts,m2ts,avi,ogg}）的目录。
# 上面那批畸形样本只能证明"解析器不崩"，证明不了"真实文件上算出来的数是对的"。
target_compile_definitions(test_container_structure_analyzers PRIVATE
    VIDEOEYE_CONTAINER_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus/container")
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/qt/QtWorkerOwner.cpp
)
target_link_libraries(test_cancel_and_robustness PRIVATE
    VideoEyeCodec
    VideoEyeDomain
    VideoEyeInfrastructure
    Qt6::Core)
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
#       只编 VisualDefectPage 并链 VideoEyeUiCharts（header-only 的
#       AnalysisPageSupport 不用编），不整层拉 analysis —— 单测不该为一次界面检查
#       拖进 FFmpeg。控件加了 objectName（VisualDefectPreset / VisualDefectBlurThreshold /
#       VisualDefectFreezeThreshold / VisualDefectCaptureRgb / VisualDefectEnable），
#       测试才不必靠创建顺序猜 findChildren 的下标。
# =================================================
videoeye_add_test(test_visual_defect_page
    unit/test_visual_defect_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/VisualDefectPage.cpp
)
target_link_libraries(test_visual_defect_page PRIVATE
    VideoEyeDomain
    VideoEyeInfrastructure
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME VisualDefectPageTests COMMAND test_visual_defect_page)
set_tests_properties(VisualDefectPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 49：文件结构页（容器结构 / MP4 样本表 / EBML 详情的页路由）
# 说明: 这个页同时接通用结构树、MP4 box 表、MP4 样本表、EBML 详情表四种数据源，
#       并按 format 在 QStackedWidget 上切到正确详情页。几条路由写错一个分支界面就
#       停在旧页/空表且不崩，属于最难手动点测发现的回归。直接编页面源并链
#       VideoEyeUiCharts（AnalysisPageSupport.h 的 TableBatch 在那里），不整层拉
#       analysis / FFmpeg。VE_PERF 走 infrastructure 的 Logger，所以补链
#       VideoEyeInfrastructure。用 offscreen 平台跑，不需要显示器。
# =================================================
videoeye_add_test(test_container_structure_page
    unit/test_container_structure_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/ContainerStructurePage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/Mp4SampleTableWidget.cpp
)
target_link_libraries(test_container_structure_page PRIVATE
    VideoEyeDomain
    VideoEyeInfrastructure
    VideoEyeReporting
    VideoEyeUiCharts
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
#       moc 与实现都在 VideoEyeUiCharts 里，链库即可，不整层拉 analysis / FFmpeg。
#       用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_audio_frame_table_widget
    unit/test_audio_frame_table_widget.cpp
)
target_link_libraries(test_audio_frame_table_widget PRIVATE
    VideoEyeUiAnalysisWidgets
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME AudioFrameTableWidgetTests COMMAND test_audio_frame_table_widget)
set_tests_properties(AudioFrameTableWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 54：视频帧表子页（GOP 派生 + 双表刷新 + 帧类型筛选 + Reset 通知）
# 说明: GOP 分段是从帧记录 pict_type 推导的派生结果，段边界/计数错了界面不崩只停旧值。
#       组件在 VideoEyeUiAnalysisWidgets 里（它自己 include <libavcodec/avcodec.h>
#       取 AV_PICTURE_TYPE_* 常量，编译期，不链 FFmpeg 库）；曲线控件的实现与 moc
#       走 VideoEyeUiCharts。
# =================================================
videoeye_add_test(test_video_frame_table_widget
    unit/test_video_frame_table_widget.cpp
)
target_link_libraries(test_video_frame_table_widget PRIVATE
    VideoEyeUiAnalysisWidgets
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME VideoFrameTableWidgetTests COMMAND test_video_frame_table_widget)
set_tests_properties(VideoFrameTableWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 55：FFmpeg 指令字典（分类/搜索过滤 + 滤镜插入补 -vf/-af + 无匹配禁用）
# 说明: 词条来自静态表 FfmpegCommandCatalog（core/ffmpeg，纯 Qt），组件自己搜；插入
#       语义（滤镜补 -vf/-af、末尾空格）是唯一会静默写错命令的规则。只编组件源，
#       词典走 VideoEyeFfmpegTools 库，不启动任何子进程、不链 FFmpeg。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_ffmpeg_dictionary_widget
    unit/test_ffmpeg_dictionary_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ffmpeg_panel/FfmpegDictionaryWidget.cpp
)
# 词条表 FfmpegCommandCatalog 在 VideoEyeFfmpegTools（纯 Qt，不含 FFmpeg），链库即可。
target_link_libraries(test_ffmpeg_dictionary_widget PRIVATE
    VideoEyeFfmpegTools
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME FfmpegDictionaryWidgetTests COMMAND test_ffmpeg_dictionary_widget)
set_tests_properties(FfmpegDictionaryWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 56：包表子页（增量刷新 + 筛选重建/增量同语义 + 单元格/汇总 + Reset）
# 说明: 包表是帧表↔包表 PTS 互跳的记录侧，切筛选的「全量重建」与常规「增量补差」
#       必须共用同一份筛选语义和同步游标，错位只会静默少行/错行。组件在
#       VideoEyeUiAnalysisWidgets 里（AV_PKT_FLAG_* / AVMEDIA_TYPE_* 是编译期常量）；
#       曲线控件的实现与 moc 走 VideoEyeUiCharts。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_packet_table_widget
    unit/test_packet_table_widget.cpp
)
target_link_libraries(test_packet_table_widget PRIVATE
    VideoEyeUiAnalysisWidgets
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME PacketTableWidgetTests COMMAND test_packet_table_widget)
set_tests_properties(PacketTableWidgetTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 57：FFmpeg 输出与解释区（状态行 + 命令解释页 + 双通道日志 + 终态文案）
# 说明: 这一区从 FfmpegPanel 抽出，把「命令一改就重算解释」「stderr 行加 [err] 前缀」
#       「失败切错误页」「优雅停止与被强杀分开报」四条规则压在组件内部，写错都不崩、
#       只静默显示旧值或错值。只编组件源；解释链（Parser / Explainer / Catalog）
#       与 AppTheme（组件用 monoFont）分别走 VideoEyeFfmpegTools / VideoEyeUiTheme 库，
#       不启动任何子进程、不链 FFmpeg。
#       用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_ffmpeg_output_widget
    unit/test_ffmpeg_output_widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ffmpeg_panel/FfmpegOutputWidget.cpp
)
# 解释链（Parser / Explainer / Catalog）在 VideoEyeFfmpegTools 里，链库即可。
target_link_libraries(test_ffmpeg_output_widget PRIVATE
    VideoEyeUiTheme
    VideoEyeFfmpegTools
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
#       3 位小数汇总文案。编页面源并链 VideoEyeUiCharts（AnalysisPageSupport.h
#       经它引入 Q_OBJECT 头）+ VideoEyeUiTheme（formatTime 时间戳文案），
#       链 VideoEyeDomain。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_scene_change_page
    unit/test_scene_change_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/SceneChangePage.cpp
)
target_link_libraries(test_scene_change_page PRIVATE
    VideoEyeDomain
    VideoEyeUiCharts
    VideoEyeUiTheme
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME SceneChangePageTests COMMAND test_scene_change_page)
set_tests_properties(SceneChangePageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 60：宏块分析页（MV 像素精度换算 + HEVC/CTU 术语 + 500 行上限）
# 说明: 运动矢量按 motion_scale 换算像素、scale=0 按 1 兜底、术语随编码切换
#       （HEVC→CTU、块大小表 14 行分区）、I 帧预览文案、行数上限 500 —— 写错都不崩，
#       只会静默显示假像素值 / 错误术语。组件在 VideoEyeUiAnalysisWidgets 里，
#       链它即可（图表控件走 VideoEyeUiCharts）。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_macroblock_view
    unit/test_macroblock_view.cpp
)
target_link_libraries(test_macroblock_view PRIVATE
    VideoEyeUiAnalysisWidgets
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME MacroblockViewTests COMMAND test_macroblock_view)
set_tests_properties(MacroblockViewTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 61：色彩与 HDR 页（缺失值高亮 + 异常表类别过滤 + 扫描态互斥）
# 说明: 「缺失 / 未标注」橙色加粗掉了只会变普通黑字；异常表收进非 ColorHdr 类别
#       会把视频 / 音频问题塞进色彩页；扫描态不同步会让页面卡在「扫描中」。三条
#       都是静默回归。三张表里的色彩 / HDR 表表头相同，测试经页内唯一 QTabWidget
#       按 tab 索引定位。编页面源并链 VideoEyeUiCharts（同测试 59 的曲线控件依赖），
#       链 VideoEyeDomain（BuildColorRows / BuildHdrRows 在 domain）。用 offscreen 跑。
# =================================================
videoeye_add_test(test_color_hdr_page
    unit/test_color_hdr_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/ColorHdrPage.cpp
)
target_link_libraries(test_color_hdr_page PRIVATE
    VideoEyeDomain
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME ColorHdrPageTests COMMAND test_color_hdr_page)
set_tests_properties(ColorHdrPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 62：音频 QC 页（削波/静音表填充 + 响度偏离分档 + 规则判定着色）
# 说明: 削波峰值 <=0 要回落 -120 dBFS、静音表 3/3/3/1 位小数、响度偏离按
#       1/3 LU 两档边界切换达标/偏离/超标、规则表的严重度→文案→颜色映射，
#       改错都是静默显示假值。页面内两张表表头「起始」相同，测试经页内唯一
#       QTabWidget 按 tab 索引 + 列数定位。编页面源并链 VideoEyeUiCharts
#       （AnalysisPageSupport.h 经它引入 Q_OBJECT 头），
#       链 VideoEyeDomain。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_audio_qc_page
    unit/test_audio_qc_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioQcPage.cpp
    # 本页的 5 个子页已各自拆成独立组件（响度与电平 / 静音与削波 / 声道与相位 /
    # 规则结果 / 音轨对照）。本目标手工列源（不走 UI_SOURCES），漏了会在链接期
    # 报符号未解析。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioLoudnessView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioEventView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioChannelView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioVerdictView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/AudioTrackView.cpp
)
target_link_libraries(test_audio_qc_page PRIVATE
    VideoEyeDomain
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME AudioQcPageTests COMMAND test_audio_qc_page)
set_tests_properties(AudioQcPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 63：码率与 GOP 页（GOP 表高亮 + 异常表 2000 行上限 + 窗口联动汇总）
# 说明: 超长 GOP 的时长列橙色高亮、异常表上限 2000、帧类型未解析时汇总换
#       文案、目标峰值手动/自动（自动=均值×2）分支、窗口下拉联动汇总文案，
#       写错都不崩只静默出错。页面内两个无后缀 spin 用 decimals 区分（0=目标
#       峰值 / 1=GOP 秒上限）。编页面源并链 VideoEyeUiCharts（同测试 62 的
#       曲线控件依赖）+ VideoEyeUiTheme（formatTime 时间戳文案），链 VideoEyeDomain。
#       用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_bitrate_gop_page
    unit/test_bitrate_gop_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/BitrateGopPage.cpp
)
target_link_libraries(test_bitrate_gop_page PRIVATE
    VideoEyeDomain
    VideoEyeUiCharts
    VideoEyeUiTheme
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME BitrateGopPageTests COMMAND test_bitrate_gop_page)
set_tests_properties(BitrateGopPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 64：字幕/时码/辅助页（流过滤 + 时码信号 + SCTE-35 表 + 正阈值守卫）
# 说明: 字幕流过滤叠加（流 + 仅问题 cue）、时码就绪信号只在 has_primary 时
#       发射（drop-frame 文案差异）、SCTE-35 event_id 十六进制补零与 CRC 三态、
#       规则阈值只认正存储位（<=0 不覆盖默认），四处静默回归。页内四张表用
#       tab 索引区分同列表头。编页面源并链 VideoEyeUiCharts（同测试 62 的
#       曲线控件依赖），链 VideoEyeDomain。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_subtitle_aux_page
    unit/test_subtitle_aux_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/SubtitleAuxPage.cpp
)
target_link_libraries(test_subtitle_aux_page PRIVATE
    VideoEyeDomain
    VideoEyeUiCharts
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME SubtitleAuxPageTests COMMAND test_subtitle_aux_page)
set_tests_properties(SubtitleAuxPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 65：诊断与报告页（扫描终态三值 + 规则表改写）
# 说明: 失败也是终态（不发 -> 共用同一扫描的几页永久停在扫描态）、silent 失败
#       不弹框、HLS 清单成功扫描后问题表与 qcReport 逐行一致、规则表勾选/阈值
#       改写落规则集且非法输入还原，都是静默回归。本页只保留「问题清单 / 规则与
#       阈值」两个子页 —— 实时时间轴的样本/曲线/导出已收敛到「事件与时间轴」页。
#       本页自己持有 AnalysisFacade（扫描真跑），所以编
#       AnalysisFacade.cpp + Logger（ScopedTimer/VE_PERF 走 LOG_INFO）；链
#       QtAdapters（分析协调器）/Analysis/Reporting（报告导出）/Infrastructure。
#       用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_diagnostics_page
    unit/test_diagnostics_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/DiagnosticsPage.cpp
    # 本页的 5 个子页已各自拆成独立组件（问题清单 / 规则与阈值 / 证据与覆盖 /
    # 参数变化 / 帧顺序）。本目标手工列源（不走 UI_SOURCES），漏了会在链接期
    # 报符号未解析。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/DiagnosticsIssueView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/QcRuleTableView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/EvidenceCoverageView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/ParameterTimelineView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/analysis_panel/FrameOrderView.cpp
)
target_sources(test_diagnostics_page PRIVATE ../ui/AnalysisFacade.cpp)
target_link_libraries(test_diagnostics_page PRIVATE
    VideoEyeQtAdapters
    VideoEyeAnalysis
    VideoEyeQc
    VideoEyeReporting
    VideoEyeInfrastructure
    VideoEyeUiCharts
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
# 取消路径用例用 BlockingTcpEndpoint.h 起本地监听端口，Windows 上需要显式 ws2_32。
if(WIN32)
    target_link_libraries(test_diagnostics_page PRIVATE ws2_32)
endif()
add_test(NAME DiagnosticsPageTests COMMAND test_diagnostics_page)
set_tests_properties(DiagnosticsPageTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 66：CSV 导出助手（AnalysisPageSupport.h 的统一写入端）
# 说明: 这段写入端（UTF-8 BOM + 编码 + 关闭）以前在 20 来处各抄一份，BOM 有的
#       写有的没写 —— 漏的那一半用 Excel 打开中文列乱码，只有真导出一次才看得见。
#       收成纯函数后在这里钉住：BOM 必须在、同名文件必须截断、打不开必须返回
#       false。不建 QApplication、不弹对话框，只编 header-only 的助手本身。
# =================================================
videoeye_add_test(test_csv_export_helper
    unit/test_csv_export_helper.cpp
)
target_link_libraries(test_csv_export_helper PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME CsvExportHelperTests COMMAND test_csv_export_helper)

# =================================================
# 测试 67：异常语料回归（阶段 3.1/3.2 的确定性防线）
# 说明: tests/corpus/ 下的样本由 generate_corpus.py 逐字节生成（截断/空/极短/
#       错误长度字段/极大 ue(v)/负 DASH r/未知长度 EBML/深层嵌套/错误 largesize/
#       错误 FLV header/ASF 短对象/BOM+小写 HLS/不完整字幕 cue/错误 SCTE-35 CRC）。
#       伞形用例把每个样本喂给全部自研解析器：断言不崩溃、单样本 5 秒内终止、
#       产物列表有界；专项用例钉住各解析器的明确语义（含"错误状态不得产出
#       valid 结果"）。语料路径经 VIDEOEYE_CORPUS_DIR 注入，测试不依赖工作目录。
#       TIMEOUT 300 是兜底：万一某个解析器又失去循环上界，ctest 直接杀掉并报红，
#       而不是把整条流水线挂死。
# =================================================
videoeye_add_test(test_corpus_robustness
    unit/test_corpus_robustness.cpp
)
target_compile_definitions(test_corpus_robustness PRIVATE
    VIDEOEYE_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_link_libraries(test_corpus_robustness PRIVATE VideoEyeAnalysis)
add_test(NAME CorpusRobustnessTests COMMAND test_corpus_robustness)
set_tests_properties(CorpusRobustnessTests PROPERTIES TIMEOUT 300)

# =================================================
# 测试 68：支持的格式与编解码器（帮助菜单）
# 说明: 页面上每一行都必须由 av_*_iterate() 现场问出来 —— 换一份 FFmpeg 构建，
#       清单就该跟着变，写死的常量表只会骗人。这里钉住三件事：
#         1) 枚举结果来自本进程链接的库（h264/aac/file 都在，方向没错）；
#         2) 同名 demuxer/muxer、decoder/encoder 合成一行（avi 双向都置位），
#            而 mp4 这种"读挂别名表、写是独立注册"的按库原样分两行；
#         3) 搜索与过滤只动行隐藏 —— 「仅能解码」的可见行数必须正好等于
#            数据里的解码器条数（这条同时兜住"过滤后导出仍写全量"的错）。
#       只编两个新源文件（探针 + 对话框），不拉整个 ui/，用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_format_support
    unit/test_format_support.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/dialogs/FfmpegSupportProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/dialogs/FormatSupportDialog.cpp
)
target_include_directories(test_format_support PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui)
# FFmpeg 的 include 目录与库都从 VideoEyePlayback 的 PUBLIC 依赖传过来，
# 不必在这里重复列（FfmpegSupportProbe.cpp 是唯一 include libav* 的 TU）。
target_link_libraries(test_format_support PRIVATE
    VideoEyePlayback
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME FormatSupportTests COMMAND test_format_support)
set_tests_properties(FormatSupportTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 69：日志落点（ui/app/LogDirectory.cpp）
# 说明: 日志目录从 exe 同目录迁到用户级数据目录，修的是"装到 Program Files 后
#       启动异常却拿不到日志"这个最需要日志的场景。这条路径跑在 main() 里、
#       QApplication 构造之后，没有 UI 测试覆盖，所以在这里钉住两件事：
#         1) 解析结果落在用户级目录而不是程序目录（且目录被真实创建出来）；
#         2) 超限才滚动、未超限一个字节都不动（滚动写反会每次启动都丢日志）。
#       只编 LogDirectory.cpp 一个源文件，不需要 GUI —— 但 QTemporaryDir 与
#       QStandardPaths 需要 Qt Core，按项目惯例用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_log_directory
    unit/test_log_directory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/app/LogDirectory.cpp
)
target_link_libraries(test_log_directory PRIVATE Qt6::Core)
add_test(NAME LogDirectoryTests COMMAND test_log_directory)
set_tests_properties(LogDirectoryTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试 70：线程体统一收口（core/domain/task/ThreadBody.h）
# 说明: 这是"所有线程入口用同一个异常捕获与收尾模板"这条约定的落点 ——
#       GuardBody / ResolveTerminal / ScopeExit 各有一条用例，外加一条真线程集成
#       （不套收口的话那条用例会直接把测试进程带走）。放在这里还有一个附带作用：
#       它编译一遍"Header 能被 ui 之外的所有层 include"这件事 —— core/qc、
#       core/qt、core/player 都按分层规则看不见 infrastructure/，所以协议与模板
#       必须一起住在 core/domain/task。
# 只依赖 stdlib + gtest；链 VideoEyeInfrastructure 是为了 Linux 上的 pthread。
# =================================================
videoeye_add_test(test_thread_body unit/test_thread_body.cpp)
target_link_libraries(test_thread_body PRIVATE VideoEyeInfrastructure)
add_test(NAME ThreadBodyTests COMMAND test_thread_body)

# =================================================
# 测试: 首页分析摘要的组装口径 (ui/main_window/overview_analysis.h)
#
# 从 MainWindow::RefreshOverviewAnalysis 抽出来的纯函数：什么算"扫描失败"、
# 「最重要的异常」按什么顺序取前几条。以前住在 UI 回调里测不到。
# header-only + QString/QVector，只需 VideoEyeDomain（QcReport::CountBySeverity
# 的实现）与 Qt6::Core，不需要 QApplication。
# =================================================
videoeye_add_test(test_overview_analysis unit/test_overview_analysis.cpp)
target_link_libraries(test_overview_analysis PRIVATE VideoEyeDomain Qt6::Core)
add_test(NAME OverviewAnalysisTests COMMAND test_overview_analysis)

# =================================================
# 测试: 打开会话的分派与待收尾登记口径 (ui/main_window/open_session.h)
#
# 从 MainWindow::OpenMedia / OnPlayerOpenFinished 抽出来的两条判定：一个 source 该走
# 哪条打开路径、异步回调是不是过期的。错了都不崩，只会静默走错分支。
# 只需 Qt6::Core（QString），不需要 QApplication。
# =================================================
videoeye_add_test(test_open_session unit/test_open_session.cpp)
target_link_libraries(test_open_session PRIVATE Qt6::Core)
add_test(NAME OpenSessionTests COMMAND test_open_session)

# =================================================
# 测试: 主窗口的两条尺寸/布局口径 (ui/main_window/window_layout.h)
#
# 从 UpdateMinimumWindowSize / showEvent 抽出来的纯口径：最小尺寸是「几条栏 +
# 两个内容区」的硬加总（漏一条就允许窗口比内容矮），小屏阈值决定起手用哪个布局
# 预设（判反的话 1366x768 上一打开就超出可用高度 —— 这条以前真出过）。
# 不碰 QWidget，只需要 QSize。
# =================================================
videoeye_add_test(test_window_layout unit/test_window_layout.cpp)
target_link_libraries(test_window_layout PRIVATE Qt6::Core)
add_test(NAME WindowLayoutTests COMMAND test_window_layout)

# =================================================
# 测试: 侧栏导航的 Qt 侧机制 (ui/main_window/sidebar_navigation.{h,cpp})
#
# 这块原先住在 MainWindow.cpp 顶部的匿名命名空间里。它守的是三条「错了就是静默
# 错位」的不变量：页面项带 UserRole=stack 下标而分组标题不带（切换与徽标唯一的
# 寻址依据）、分组标题不可选中、铺条目全程静音。纯排布口径在 NavigationModelTests
# 里，这里只测 Qt 侧的读写。用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_sidebar_navigation
    unit/test_sidebar_navigation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_window/sidebar_navigation.cpp
)
target_link_libraries(test_sidebar_navigation PRIVATE
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME SidebarNavigationTests COMMAND test_sidebar_navigation)
set_tests_properties(SidebarNavigationTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# =================================================
# 测试: 批量 QC 结果的展示口径 (ui/reporting_panel/batch_result_model.h)
#
# 从 ReportingPanel 的 RunBatch 完成段 / ShowBatchDetail 抽出来的三条口径：整批终态
# （超时算没跑完）、汇总行十一个计数、表格行数值列必须存数值而不是字符串。
# 只需 Qt6::Core（QString）与 VideoEyeQc（BatchQcItemResult 的实现），无 QWidget。
# =================================================
videoeye_add_test(test_batch_result_model unit/test_batch_result_model.cpp)
target_link_libraries(test_batch_result_model PRIVATE VideoEyeQc Qt6::Core)
add_test(NAME BatchResultModelTests COMMAND test_batch_result_model)

# =================================================
# 报告导出口径 + ReportExportController。
# 控制器带 Q_OBJECT，所以把它的 .cpp 一并编进本测试（AUTOMOC 靠同名匹配生成 moc）。
# 不链 VideoEyeReportingPanel —— 那个库里已经有同一份 .cpp，链进来就是同一个类
# 两份实现（ODR），症状是随机的 0xc0000374。
# =================================================
videoeye_add_test(test_report_export
    unit/test_report_export.cpp
    ../ui/reporting_panel/ReportExportController.cpp
)
target_link_libraries(test_report_export PRIVATE
    VideoEyeReporting
    VideoEyeQc
    VideoEyeDomain
    VideoEyeInfrastructure
    Qt6::Core)
add_test(NAME ReportExportTests COMMAND test_report_export)

# =================================================
# 报告页任务协调器（启停 / 回收 / 终态归还 / 异常收尾）。
# 同样不链 VideoEyeReportingPanel（ODR），直接编 .cpp。QueuedConnection 要真投递，
# 所以链 Qt6::Core 并在测试里建一个 QCoreApplication（不需要 offscreen 平台）。
# =================================================
videoeye_add_test(test_reporting_task_coordinator
    unit/test_reporting_task_coordinator.cpp
    ../ui/reporting_panel/ReportingTaskCoordinator.cpp
)
target_link_libraries(test_reporting_task_coordinator PRIVATE
    VideoEyeInfrastructure
    VideoEyeDomain
    Qt6::Core)
add_test(NAME ReportingTaskCoordinatorTests COMMAND test_reporting_task_coordinator)

# =================================================
# 测试 62：播放器信号总线（导出 ExportBus / 容器 ContainerBus / 分析 AnalysisBus）
# 说明: 导出的十条、容器结构的两条、实时分析的 23 条信号从 MediaPlayer 门面
#       搬到各自的总线。
#       断言在元对象层面做（门面 indexOfSignal == -1、总线契约齐全），
#       并用"无媒体抽帧必经总线报 Error""打不开的文件必经总线报 Failed"
#       验证两个控制器的发射口都改对了 —— 发错地方不会崩，只会静默失效
#       （进度框永远不弹 / 结构页一直空着）。
#       不用 offscreen（不涉及控件），QCoreApplication 足够。
# =================================================
videoeye_add_test(test_player_signal_buses
    unit/test_player_signal_buses.cpp
)
target_link_libraries(test_player_signal_buses PRIVATE
    VideoEyePlayback
    Qt6::Core Qt6::Gui)
if(WIN32)
    target_link_libraries(test_player_signal_buses PRIVATE ws2_32)
endif()
add_test(NAME PlayerSignalBusTests COMMAND test_player_signal_buses)

# =================================================
# 测试 63：分析页目录的顺序契约 (ui/analysis_panel/AnalysisPageCatalog.h)
#
# 页面在外部 QStackedWidget 里的下标是硬契约 —— 有回归用例按 stack 下标取控件
# （tabs->widget(0/1/3)）。以前这个顺序只是 SetupUI 里十几个 SetupXxx() 调用次序的
# 副产品，调序一下标全错位却没人看得见；现在它是一张数据表，这里钉住顺序、
# id 唯一、以及 (分组, 组内序号) 不撞车。
# 目录是 header-only 且不碰页面组件，所以不需要 offscreen，只链 Qt6::Core。
# =================================================
videoeye_add_test(test_analysis_page_catalog unit/test_analysis_page_catalog.cpp)
target_link_libraries(test_analysis_page_catalog PRIVATE Qt6::Core)
add_test(NAME AnalysisPageCatalogTests COMMAND test_analysis_page_catalog)

# =================================================
# 测试 64：解析层回归（UTF-8 路径 / 主轨选择 / 时间轴降噪 / 封面图）
#
# TimelineAnalyzer.cpp 直接编进本目标，不链 VideoEyeAnalysis ——
# 它只依赖 core/domain，拉整层会让这批"纯解析"用例跟着 FFmpeg 一起编。
# =================================================
videoeye_add_test(test_parser_review_regressions
    unit/test_parser_review_regressions.cpp
    ../core/analysis/diagnostics/TimelineAnalyzer.cpp
)
target_link_libraries(test_parser_review_regressions PRIVATE VideoEyeMedia VideoEyeDomain)
add_test(NAME ParserReviewRegressionTests COMMAND test_parser_review_regressions)

# =================================================
# 测试 65：分析流水线边界（封面图 / 按流 GOP / 容器位深未知 / >8 声道）
#
# 这条用例要跑真实的 demux + 解码 + 重采样，所以链整个 VideoEyeAnalysis
# （FFmpeg 的 include 与库从它的 PUBLIC 依赖传来）。
# =================================================
videoeye_add_test(test_analysis_pipeline_edges unit/test_analysis_pipeline_edges.cpp)
target_link_libraries(test_analysis_pipeline_edges PRIVATE VideoEyeAnalysis)
add_test(NAME AnalysisPipelineEdgeTests COMMAND test_analysis_pipeline_edges)

videoeye_add_test(test_realtime_analysis_controller unit/test_realtime_analysis_controller.cpp)
target_link_libraries(test_realtime_analysis_controller PRIVATE VideoEyePlayback Qt6::Core Qt6::Gui)
if(WIN32)
    target_link_libraries(test_realtime_analysis_controller PRIVATE ws2_32)
endif()
add_test(NAME RealtimeAnalysisControllerTests COMMAND test_realtime_analysis_controller)

videoeye_add_test(test_macroblock_analyzer unit/test_macroblock_analyzer.cpp)
target_link_libraries(test_macroblock_analyzer PRIVATE VideoEyeAnalysis)
add_test(NAME MacroblockAnalyzerTests COMMAND test_macroblock_analyzer)

# =================================================
# 测试 66：流媒体包面板（P2 拆分后的子视图契约）
#
# 说明: StreamingPanel 这一页原先 700 余行、且没有任何测试。拆分后四块展示内容
#       各自成为独立子视图（结构树 / 码率阶梯 / 分片时间轴 / 问题表），JSON
#       序列化也抽成自由函数。本测试钉住四块内容的填充口径：
#         - DASH 的 Period - AdaptationSet - Representation - Segment 层级、
#           HLS 的 variant / I-FRAME / rendition / 直播清单细节与 DATERANGE；
#         - 阶梯表的分辨率回退 "-"、kbps 取整、初始化段有无；
#         - 时间轴参考层粗体 + 偏差 > 0.05 s 的高亮与 tooltip；
#         - 问题表严重度倒序与整行着色、IssueCount == 行数；
#         - 导出 JSON 的 check_scope（抽样口径）与关键字段。
#       另测 Clear / 解析失败 / 扫描态三处复位。子视图源手工列出（不走
#       UI_SOURCES），链 VideoEyeDomain（ToString 与模型）与 Qt Widgets，
#       用 offscreen 平台跑。
# =================================================
videoeye_add_test(test_streaming_panel
    unit/test_streaming_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/streaming_panel/StreamingPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/streaming_panel/StreamingStructureView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/streaming_panel/StreamingLadderView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/streaming_panel/StreamingTimelineView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/streaming_panel/StreamingIssueView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/streaming_panel/StreamingPackageJson.cpp
)
target_link_libraries(test_streaming_panel PRIVATE
    VideoEyeDomain
    Qt6::Core Qt6::Gui Qt6::Widgets)
add_test(NAME StreamingPanelTests COMMAND test_streaming_panel)
set_tests_properties(StreamingPanelTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
