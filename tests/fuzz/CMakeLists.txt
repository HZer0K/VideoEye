# ============================================================
# libFuzzer 入口（阶段 3.3）
#
# 只在 VIDEOEYE_ENABLE_FUZZERS=ON 时进入本目录；平台守卫（仅 Linux + clang）
# 与全项目 ASan/UBSan 插桩（fuzzer-no-link）在顶层 CMakeLists.txt 统一处理，
# 这里只负责把四个入口各自链上 fuzzer 运行时（提供 main）。
#
# 四个入口的分工：
#   FuzzBitstreamExtradata  码流参数集 + extradata 封装（BitstreamAnalyzer / ExtradataParser）
#   FuzzContainerStructure  容器字节级解析（MP4 / EBML / FLV / ASF，路径型解析器经临时文件）
#   FuzzManifest            HLS / DASH 清单文本
#   FuzzSubtitleAux         字幕文本 + SCTE-35 二进制段
#
# 每个入口只允许三种终态：解析成功 / 解析失败 / 被取消 —— 不允许崩溃、
# 不允许无限循环、不允许无界分配（输入尺寸上限 + 各解析器内部 ParserLimits 预算）。
# ============================================================

function(videoeye_add_fuzzer name)
    add_executable(${name} ${ARGN})
    # 只链分析层：它 PUBLIC 透出 VideoEyeMedia / VideoEyeDomain / FFmpeg 的
    # include 目录与链接边。**刻意不链任何 Qt 目标** —— fuzz 入口必须能在
    # headless 容器里构建与运行（用户要求"不依赖 GUI"）。
    target_link_libraries(${name} PRIVATE VideoEyeAnalysis)
    # fuzzer 运行时：入口需要 -fsanitize=fuzzer 才能得到 libFuzzer 的 main
    # （编译侧的 address/undefined 插桩已由顶层统一加，这里只补链接这一条边）。
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

videoeye_add_fuzzer(FuzzBitstreamExtradata fuzz_bitstream_extradata.cpp)
videoeye_add_fuzzer(FuzzContainerStructure fuzz_container_structure.cpp)
videoeye_add_fuzzer(FuzzManifest fuzz_manifest.cpp)
videoeye_add_fuzzer(FuzzSubtitleAux fuzz_subtitle_aux.cpp)