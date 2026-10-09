// 容器字节级解析 fuzz 入口（阶段 3.3）：MP4 / EBML / FLV / ASF。
//
// 这四个解析器都是**路径型**接口（QC 与 UI 都按文件组织），所以这里先把输入落到
// 临时文件再解析。临时文件路径带进程号：-jobs=N 并行 fuzz 时各进程各写各的，
// 不会互相踩；单进程内回调串行，覆盖写即可。进程退出时删除。
//
// 终态约束：每个解析器只允许返回成功 / 失败 / 取消（取消标志恒假——为的是让
// "带取消参数的实现路径"也被编译并执行到）。不允许崩溃、无限循环或按输入声明
// 分配无界内存 —— 后者由各解析器内部的 ParserLimits 预算兜底，本入口再把输入
// 卡在 1 MiB。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include "core/analysis/container/AsfStructureAnalyzer.h"
#include "core/analysis/container/EbmlAnalyzer.h"
#include "core/analysis/container/FlvStructureAnalyzer.h"
#include "core/media/container/IsobmffParser.h"

namespace {

constexpr size_t kMaxInputBytes = 1u << 20; // 1 MiB
const std::atomic<bool> kNeverCancel{false};

// 进程级临时文件：首次调用时定型路径，进程退出时删除。
struct TempInput {
    std::string path;

    TempInput() {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec)
            dir = std::filesystem::current_path(ec);
        path = (dir / ("videoeye_fuzz_container_" + std::to_string(::getpid()) + ".bin")).string();
    }

    ~TempInput() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

TempInput& Temp() {
    static TempInput instance;
    return instance;
}

bool WriteTemp(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxInputBytes)
        return 0;
    const std::string& path = Temp().path;
    if (!WriteTemp(path, data, size))
        return 0;

    videoeye::IsobmffFile mp4;
    videoeye::IsobmffParser::Parse(path, mp4);

    videoeye::model::EbmlAnalysisResult ebml;
    videoeye::EbmlAnalyzer ebml_analyzer;
    ebml_analyzer.Analyze(path, ebml, &kNeverCancel);

    videoeye::model::ContainerStructureResult flv;
    videoeye::FlvStructureAnalyzer flv_analyzer;
    flv_analyzer.Analyze(path, flv, &kNeverCancel);

    videoeye::model::ContainerStructureResult asf;
    videoeye::AsfStructureAnalyzer asf_analyzer;
    asf_analyzer.Analyze(path, asf, &kNeverCancel);

    return 0;
}