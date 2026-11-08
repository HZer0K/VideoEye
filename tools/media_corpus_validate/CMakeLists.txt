# 真实语料校验工具（离线 / nightly），只在 VIDEOEYE_BUILD_CORPUS_VALIDATOR=ON 时构建。
#
# 链接 VideoEyeAnalysis 就够了：它 PUBLIC 带出 Domain / Media / Codec 与 FFmpeg 的
# include+link 闭包（见顶层 CMakeLists 中对 VideoEyeAnalysis 的注释）。
# Logger 走 Infrastructure，因此单独列一条；它没有 PUBLIC 传递到 Analysis。
add_executable(media_corpus_validate main.cpp)

target_link_libraries(media_corpus_validate
    PRIVATE VideoEyeAnalysis
            VideoEyeInfrastructure
)

# Windows 的 FFmpeg / Qt DLL 只按"与可执行文件同目录"查找，统一落到 bin。
set_target_properties(media_corpus_validate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
