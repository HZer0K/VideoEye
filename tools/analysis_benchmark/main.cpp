// 解析性能与规模基准工具（离线 / nightly），不进默认构建。
//
// 目的不是"跑得快不快"，而是让"慢了多少"有可比较的口径：
//   * 每个文件分别计时 FormatDetector / ContainerStructureAnalyzer / MediaInfoAnalyzer，
//     输出到 CSV，scripts/benchmark_analysis.py 用中位数与基线 JSON 比对；
//   * 同时记录峰值驻留内存与进程句柄数 —— 批量跑几千个文件时，句柄只增不减
//     或 RSS 随文件数线性上涨都是泄漏信号，单看耗时发现不了；
//   * 每个阶段可挂超时监视（复用分析器已有的取消标志），4 h / >4 GB 这类
//     极端样本即使卡住也能被中断并如实记成 timed_out，而不是把整轮基准挂死。
//
// 编译开关: cmake --preset <preset> -DVIDEOEYE_BUILD_ANALYSIS_BENCHMARK=ON

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/analysis/orchestration/FormatDetector.h"
#include "core/analysis/orchestration/MediaInfoAnalyzer.h"
#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/PerfMetrics.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
#include <libavutil/log.h>
}

namespace {

using Clock = std::chrono::steady_clock;

// 与 tools/media_corpus_validate 同构：到点置位取消标志，分析器通过 FFmpeg
// 中断回调退出。基准里保留它是为了规模样本 —— 正常文件根本用不到。
class TimeoutFlag {
public:
    TimeoutFlag() : flag_(std::make_shared<std::atomic<bool>>(false)) {
    }

    std::shared_ptr<std::atomic<bool>> flag() const {
        return flag_;
    }

    void Arm(long long timeout_ms) {
        if (timeout_ms <= 0)
            return;
        worker_ = std::thread([this, timeout_ms] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [this] { return finished_.load(std::memory_order_acquire); })) {
                flag_->store(true, std::memory_order_release);
                timed_out_.store(true, std::memory_order_release);
            }
        });
    }

    void Finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    bool TimedOut() const {
        return timed_out_.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> timed_out_{false};
};

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// 进程当前打开的句柄 / 文件描述符数量。
// Windows 走 GetProcessHandleCount（kernel32，无需 psapi）；Linux 数 /proc/self/fd。
// 失败返回 -1：CSV 里写 -1 表示"这一列没采到"，别当成 0。
long long CurrentHandleCount() {
#ifdef _WIN32
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count))
        return -1;
    return static_cast<long long>(count);
#else
    std::error_code ec;
    long long count = 0;
    std::filesystem::directory_iterator it("/proc/self/fd", ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        ++count;
        it.increment(ec);
    }
    return ec ? -1 : count;
#endif
}

std::string CsvField(const std::string& value) {
    bool needs_quote = false;
    for (const char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote)
        return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    out += '"';
    return out;
}

struct Options {
    std::string out_path;
    int repeat = 1;
    int warmup = 0;
    long long timeout_ms = 0;
    bool skip_structure = false;
    bool skip_media = false;
    bool verbose = false;
    std::vector<std::string> inputs;
};

void PrintUsage() {
    std::cerr << "usage: analysis_benchmark --out <csv> [--repeat N] [--warmup N] [--timeout-ms N]\n"
                 "                         [--skip-structure] [--skip-media] [--verbose] <file-or-dir>...\n";
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take_value = [&](std::string& target) {
            if (i + 1 >= argc)
                return false;
            target = argv[++i];
            return true;
        };
        if (arg == "--out") {
            if (!take_value(options.out_path))
                return false;
        } else if (arg == "--repeat") {
            std::string value;
            if (!take_value(value))
                return false;
            options.repeat = std::max(1, std::atoi(value.c_str()));
        } else if (arg == "--warmup") {
            std::string value;
            if (!take_value(value))
                return false;
            options.warmup = std::max(0, std::atoi(value.c_str()));
        } else if (arg == "--timeout-ms") {
            std::string value;
            if (!take_value(value))
                return false;
            options.timeout_ms = std::max(0LL, static_cast<long long>(std::atoll(value.c_str())));
        } else if (arg == "--skip-structure") {
            options.skip_structure = true;
        } else if (arg == "--skip-media") {
            options.skip_media = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            return false;
        } else {
            options.inputs.push_back(arg);
        }
    }
    return !options.out_path.empty() && !options.inputs.empty();
}

std::vector<std::string> CollectFiles(const std::vector<std::string>& inputs) {
    std::vector<std::string> files;
    for (const auto& input : inputs) {
        std::filesystem::path path(input);
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            // 手工推进迭代器而不是 range-for：构造失败（权限、断链）时 ec 非空，
            // range-for 会拿它当 end 比较，看起来"目录为空"，静默漏掉整棵树。
            std::filesystem::recursive_directory_iterator it(
                path, std::filesystem::directory_options::skip_permission_denied, ec);
            const std::filesystem::recursive_directory_iterator end;
            while (!ec && it != end) {
                std::error_code entry_ec;
                if (it->is_regular_file(entry_ec))
                    files.push_back(it->path().string());
                it.increment(ec);
            }
        } else if (std::filesystem::is_regular_file(path, ec)) {
            files.push_back(path.string());
        } else {
            std::cerr << "skip (not a file): " << input << "\n";
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

struct Row {
    int iter = 0;
    std::string path;
    long long size_bytes = 0;
    std::string format;
    bool struct_ok = false;
    bool struct_valid = false;
    size_t struct_streams = 0;
    size_t struct_elements = 0;
    bool media_ok = false;
    std::string media_error;
    double format_ms = 0.0;
    double struct_ms = 0.0;
    double media_ms = 0.0;
    double total_ms = 0.0;
    double rss_current_mb = 0.0;
    double rss_peak_mb = 0.0;
    long long handles = -1;
    bool struct_timed_out = false;
    bool media_timed_out = false;
};

std::string FormatRow(const Row& row) {
    std::string line;
    auto add = [&line](const std::string& value) {
        line += CsvField(value);
        line += ',';
    };
    add(std::to_string(row.iter));
    add(row.path);
    add(std::to_string(row.size_bytes));
    add(row.format);
    add(row.struct_ok ? "1" : "0");
    add(row.struct_valid ? "1" : "0");
    add(std::to_string(row.struct_streams));
    add(std::to_string(row.struct_elements));
    add(row.media_ok ? "1" : "0");
    add(row.media_error);
    add(std::to_string(row.format_ms));
    add(std::to_string(row.struct_ms));
    add(std::to_string(row.media_ms));
    add(std::to_string(row.total_ms));
    add(std::to_string(row.rss_current_mb));
    add(std::to_string(row.rss_peak_mb));
    add(std::to_string(row.handles));
    add(row.struct_timed_out ? "1" : "0");
    add(row.media_timed_out ? "1" : "0");
    line += '\n';
    return line;
}

const char* kHeader = "iter,path,size_bytes,format,struct_ok,struct_valid,struct_streams,struct_elements,media_ok,"
                      "media_error,format_ms,struct_ms,media_ms,total_ms,rss_current_mb,rss_peak_mb,handles,"
                      "struct_timed_out,media_timed_out\n";

Row MeasureOnce(const std::string& path, const Options& options, int iter) {
    Row row;
    row.iter = iter;
    row.path = path;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    row.size_bytes = ec ? 0 : static_cast<long long>(size);

    const auto total_start = Clock::now();

    // ---- 1) 格式检测 ----
    auto stage_start = Clock::now();
    const auto detected = videoeye::FormatDetector::Detect(path);
    row.format = videoeye::FormatDetector::FormatName(detected);
    row.format_ms = ElapsedMs(stage_start);

    // ---- 2) 容器结构解析 ----
    if (!options.skip_structure) {
        TimeoutFlag timeout;
        timeout.Arm(options.timeout_ms);
        videoeye::model::ContainerStructureResult structure;
        videoeye::ContainerStructureAnalyzer analyzer;
        stage_start = Clock::now();
        row.struct_ok = analyzer.Analyze(path, structure, timeout.flag());
        row.struct_ms = ElapsedMs(stage_start);
        timeout.Finish();
        row.struct_valid = structure.valid;
        row.struct_streams = structure.streams.size();
        row.struct_elements = structure.element_tree.size();
        row.struct_timed_out = timeout.TimedOut();
    }

    // ---- 3) 媒体信息解析 ----
    // 分析器每次 Open 都会重新 alloc / free AVFormatContext；基准反复跑同一文件，
    // 顺带就是一次"打开-关闭是否配平"的抽查。
    if (!options.skip_media) {
        TimeoutFlag timeout;
        timeout.Arm(options.timeout_ms);
        videoeye::MediaInfoAnalyzer analyzer;
        stage_start = Clock::now();
        row.media_ok = analyzer.Open(path, timeout.flag());
        row.media_ms = ElapsedMs(stage_start);
        timeout.Finish();
        if (!row.media_ok)
            row.media_error = analyzer.GetLastError();
        row.media_timed_out = timeout.TimedOut();
    }

    row.total_ms = ElapsedMs(total_start);
    row.rss_current_mb = videoeye::PerfMetrics::CurrentRssMb();
    row.rss_peak_mb = videoeye::PerfMetrics::PeakRssMb();
    row.handles = CurrentHandleCount();
    return row;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage();
        return 2;
    }

    const std::vector<std::string> files = CollectFiles(options.inputs);
    if (files.empty()) {
        std::cerr << "no media files found\n";
        return 2;
    }

    // FFmpeg 探测失败时会对每个坏文件打一串日志，基准跑几千个文件时足以淹没进度。
    av_log_set_level(AV_LOG_QUIET);
    // 分析器自身的 INFO 日志同理：默认只留 ERROR，--verbose 才逐文件打印。
    videoeye::Logger::GetInstance().SetLevel(options.verbose ? videoeye::LogLevel::INFO
                                                             : videoeye::LogLevel::ERROR_LEVEL);

    std::ofstream out(options.out_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "cannot open output: " << options.out_path << "\n";
        return 2;
    }
    out << kHeader;

    std::fprintf(stderr, "benchmark: %zu files, warmup=%d repeat=%d timeout_ms=%lld\n", files.size(), options.warmup,
                 options.repeat, options.timeout_ms);

    // 句柄增量按"单文件"统计而不是全进程 start/end：每种容器第一次打开都会
    // 惰性加载少量模块句柄（实测 wav +2、mp4 +4），那是初始化不是泄漏；
    // 稳定泄漏的表现是同一个文件重复 N 次后稳定上涨。
    long long max_file_handle_delta = 0;
    const long long handles_start = CurrentHandleCount();
    for (const auto& file : files) {
        // 预热不计入 CSV：第一次跑同一文件包含冷缓存，混进中位数会把基线带偏。
        for (int i = 0; i < options.warmup; ++i)
            MeasureOnce(file, options, -1);

        const long long file_handles_before = CurrentHandleCount();
        for (int i = 0; i < options.repeat; ++i)
            out << FormatRow(MeasureOnce(file, options, i));
        out.flush();
        const long long file_handles_after = CurrentHandleCount();
        if (file_handles_before >= 0 && file_handles_after >= 0)
            max_file_handle_delta = std::max(max_file_handle_delta, file_handles_after - file_handles_before);
    }

    const long long handles_end = CurrentHandleCount();
    out.close();
    std::fprintf(stderr, "handles: files=%zu max_file_delta=%lld start=%lld end=%lld\n", files.size(),
                 max_file_handle_delta, handles_start, handles_end);
    std::fprintf(stderr, "wrote %zu rows to %s\n", files.size() * static_cast<size_t>(options.repeat),
                 options.out_path.c_str());
    return 0;
}
