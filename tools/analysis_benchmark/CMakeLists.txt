# 解析性能与规模基准工具（离线 / nightly），只在 VIDEOEYE_BUILD_ANALYSIS_BENCHMARK=ON 时构建。
#
# 与 media_corpus_validate 一样只链 VideoEyeAnalysis（PUBLIC 带出 Domain / Media /
# FFmpeg 闭包）；PerfMetrics 在 Infrastructure，单独列一条。
add_executable(analysis_benchmark main.cpp)

target_link_libraries(analysis_benchmark
    PRIVATE VideoEyeAnalysis
            VideoEyeInfrastructure
)

# Windows 的 FFmpeg / Qt DLL 只按"与可执行文件同目录"查找，统一落到 bin。
set_target_properties(analysis_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
