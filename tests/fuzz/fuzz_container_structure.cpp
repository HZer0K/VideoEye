// 容器字节级解析 fuzz 入口（阶段 3.3）。
//
// 覆盖全部手写容器解析器：ISOBMFF(MP4/MOV) / EBML(MKV/WebM) / AVI / FLV / ASF /
// OGG / MPEG-PS / MXF / MPEG-TS，以及 MP4 的样本表（stbl 全表交叉校验）。
//
// 为什么这九个要挤在一个入口里：它们都是**路径型**接口（QC 与 UI 都按文件组织），
// 没有"从内存解析"的入口。如果拆成九个 target，每个都要把同一份 MP4 语料跑一遍，
// 覆盖率并不增加，反而要维护九份 write-temp 胶水。同一个输入依次喂给九个解析器，
// 一次回调就能打到所有路径 —— 而且它们本来就共享"魔数不对就立刻返回 false"的
// 快速失败，多跑几个的边际成本只有首字节比较。
//
// 补齐的是真实缺口：TS(1480 行) / AVI(404) / OGG(311) / MXF(360) / MPEG-PS(438)
// / Mp4SampleTable(766) 之前一条 fuzz 都没有，而这六个恰恰是纯手写二进制解析、
// 边界最多（188/192/204 包长自适应、跨包 PSI 组装、失步重同步、KLV BER 长度、
// RIFF chunk 嵌套）。TS 更是网络流主战场。
//
// 终态约束：每个解析器只允许返回成功 / 失败 / 取消（取消标志恒假——为的是让
// "带取消参数的实现路径"也被编译并执行到）。不允许崩溃、无限循环或按输入声明
// 分配无界内存 —— 后者由各解析器内部的 ParserLimits 预算兜底，本入口再把输入
// 卡在 1 MiB，并显式收窄 TS 的扫描包数。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include "core/analysis/container/AsfStructureAnalyzer.h"
#include "core/analysis/container/AviStructureAnalyzer.h"
#include "core/analysis/container/EbmlAnalyzer.h"
#include "core/analysis/container/FlvStructureAnalyzer.h"
#include "core/analysis/container/Mp4SampleTableAnalyzer.h"
#include "core/analysis/container/MpegPsStructureAnalyzer.h"
#include "core/analysis/container/MxfStructureAnalyzer.h"
#include "core/analysis/container/OggStructureAnalyzer.h"
#include "core/analysis/container/TsStructureAnalyzer.h"
#include "core/media/container/IsobmffParser.h"

namespace {

constexpr size_t kMaxInputBytes = 1u << 20; // 1 MiB
const std::atomic<bool> kNeverCancel{false};

// TS 默认扫 50 万包（约 94 MB）是为了对准真实收录文件；fuzz 输入最多 1 MiB
// （约 5578 个 188 字节包），所以这里把上限压到 2000 包：
//   * 仍然覆盖 PAT/PMT 组装、PCR 采样、连续计数、失步重同步这些逻辑分支；
//   * 又不让"每秒几百次回调 × 每次扫全文"把 fuzz 吞吐拖垮（吞吐掉一个数量级
//     等于覆盖面积掉一个数量级）。
videoeye::TsStructureOptions FuzzTsOptions() {
    videoeye::TsStructureOptions options;
    options.max_scan_packets = 2000;
    return options;
}

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

    // ---- ISOBMFF ----
    videoeye::IsobmffFile mp4;
    videoeye::IsobmffParser::Parse(path, mp4);

    // 样本表是**第二次独立解析**（直接读原子拿偏移与时间，为了算 stbl 一致性），
    // 和 IsobmffParser 走的是不同代码路径，必须单独覆盖。
    videoeye::model::Mp4SampleTableResult mp4_samples;
    videoeye::Mp4SampleTableAnalyzer mp4_sample_analyzer;
    mp4_sample_analyzer.AnalyzeFile(path, mp4_samples, videoeye::Mp4SampleTableOptions{}, &kNeverCancel);

    // ---- EBML (MKV / WebM) ----
    videoeye::model::EbmlAnalysisResult ebml;
    videoeye::EbmlAnalyzer ebml_analyzer;
    ebml_analyzer.Analyze(path, ebml, &kNeverCancel);

    // ---- 其余走统一 ContainerStructureResult 的解析器 ----
    videoeye::model::ContainerStructureResult flv;
    videoeye::FlvStructureAnalyzer flv_analyzer;
    flv_analyzer.Analyze(path, flv, &kNeverCancel);

    videoeye::model::ContainerStructureResult asf;
    videoeye::AsfStructureAnalyzer asf_analyzer;
    asf_analyzer.Analyze(path, asf, &kNeverCancel);

    videoeye::model::ContainerStructureResult avi;
    videoeye::AviStructureAnalyzer avi_analyzer;
    avi_analyzer.Analyze(path, avi, &kNeverCancel);

    videoeye::model::ContainerStructureResult ogg;
    videoeye::OggStructureAnalyzer ogg_analyzer;
    ogg_analyzer.Analyze(path, ogg, &kNeverCancel);

    videoeye::model::ContainerStructureResult mpeg_ps;
    videoeye::MpegPsStructureAnalyzer ps_analyzer;
    ps_analyzer.Analyze(path, mpeg_ps, &kNeverCancel);

    videoeye::model::ContainerStructureResult mxf;
    videoeye::MxfStructureAnalyzer mxf_analyzer;
    mxf_analyzer.Analyze(path, mxf, &kNeverCancel);

    videoeye::model::ContainerStructureResult ts;
    videoeye::TsStructureAnalyzer ts_analyzer;
    ts_analyzer.Analyze(path, ts, &kNeverCancel, FuzzTsOptions());

    return 0;
}
