# 真实语料校验工具（离线 / nightly），只在 VIDEOEYE_BUILD_CORPUS_VALIDATOR=ON 时构建。
#
# 链接 VideoEyeAnalysis 就够了：它 PUBLIC 带出 Domain / Media / Codec 与 FFmpeg 的
# include+link 闭包（见顶层 CMakeLists 中对 VideoEyeAnalysis 的注释）。
# Logger 走 Infrastructure，因此单独列一条；它没有 PUBLIC 传递到 Analysis。
# 切片 2 新增 truth_* 真值采集，自己调 avformat_open_input / find_stream_info，
# 并复用 FfmpegInterrupt 的超时/取消回调 —— 后者的实现在叶子库 VideoEyeFfmpegIo，
# PRIVATE 依赖不传符号给上层，所以这里必须显式列出来（否则 LNK2019）。
add_executable(media_corpus_validate main.cpp)

target_include_directories(media_corpus_validate PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(media_corpus_validate
    PRIVATE VideoEyeAnalysis
            VideoEyeInfrastructure
            VideoEyeFfmpegIo
            FFmpeg::avformat
            FFmpeg::avcodec
            FFmpeg::avutil
)

# Windows 的 FFmpeg / Qt DLL 只按"与可执行文件同目录"查找，统一落到 bin。
set_target_properties(media_corpus_validate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
