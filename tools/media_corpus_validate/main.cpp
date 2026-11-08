// 真实语料校验工具：对语料树里的全部媒体文件跑 VideoEye 的三段解析链路
//   1) FormatDetector 魔数/扩展名格式检测
//   2) ContainerStructureAnalyzer 容器结构解析（含 FFmpeg 通用回退）
//   3) MediaInfoAnalyzer 媒体信息解析（FFmpeg 打开 + 探测 + 文本输出）
// 输出 TSV，供 scripts/validate_corpus.py 与 INDEX.tsv 比对并设阈值门禁。
//
// 这个工具不进默认构建：需要一棵外部语料树（几 GB 到几十 GB），PR 流程不跑它，
// nightly / 本地用 --index/--root 指到语料后由脚本驱动。编译开关:
//   cmake --preset <preset> -DVIDEOEYE_BUILD_CORPUS_VALIDATOR=ON

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/analysis/orchestration/FormatDetector.h"
#include "core/analysis/orchestration/MediaInfoAnalyzer.h"
#include "infrastructure/logging/Logger.h"

extern "C" {
#include <libavutil/log.h>
}

namespace {

using Clock = std::chrono::steady_clock;

struct IndexRow {
    std::string path;
    std::string category;
    std::string container;
    std::string codec;
    std::string duration_s;
    std::string resolution;
    std::string size_bytes;
    std::string parse;
};

// 每次解析配一个超时监视器：到点置位取消标志，分析器通过 FFmpeg 中断回调退出。
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
        armed_ = true;
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
    bool armed_ = false;
    std::atomic<bool> finished_{false};
    std::atomic<bool> timed_out_{false};
};

std::string Escape(std::string value) {
    for (char& c : value) {
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    }
    return value;
}

std::vector<IndexRow> LoadIndex(const std::string& index_path, const std::string& root) {
    std::vector<IndexRow> rows;
    std::ifstream in(index_path, std::ios::binary);
    std::string line;
    std::getline(in, line); // 表头
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t'))
            fields.push_back(field);
        if (fields.size() < 9)
            continue;
        IndexRow row;
        row.path = (std::filesystem::path(root) / fields[0]).string();
        row.category = fields[1];
        // INDEX.tsv 列为: path, category, source, container, codec, duration_s,
        // resolution, size_bytes, parse, defect_heuristic
        row.container = fields[3];
        row.codec = fields[4];
        row.duration_s = fields[5];
        row.resolution = fields[6];
        row.size_bytes = fields[7];
        row.parse = fields[8];
        rows.push_back(row);
    }
    return rows;
}

// 把 MediaInfoAnalyzer 的 "Duration : 1 min 27 s 336 ms" 解析回秒数。
double ParseHumanDuration(const std::string& text) {
    double seconds = 0.0;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && (text[pos] == ' '))
            ++pos;
        size_t number_start = pos;
        while (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.'))
            ++pos;
        if (number_start == pos)
            break;
        double value = std::atof(text.substr(number_start, pos - number_start).c_str());
        while (pos < text.size() && text[pos] == ' ')
            ++pos;
        size_t unit_start = pos;
        while (pos < text.size() && std::isalpha(static_cast<unsigned char>(text[pos])))
            ++pos;
        const std::string unit = text.substr(unit_start, pos - unit_start);
        if (unit == "h")
            seconds += value * 3600.0;
        else if (unit == "min")
            seconds += value * 60.0;
        else if (unit == "s")
            seconds += value;
        else if (unit == "ms")
            seconds += value / 1000.0;
        else
            break;
    }
    return seconds;
}

// 从 MediaInfo 文本中抽取 General 的 Duration、第一条视频/音频的 codec/宽高。
struct MediaInfoFields {
    double duration_s = 0.0;
    std::string video_codec;
    int width = 0;
    int height = 0;
    std::string audio_codec;
    int audio_sample_rate = 0;
    int audio_channels = 0;
};

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

MediaInfoFields ParseMediaInfoText(const std::string& text) {
    MediaInfoFields fields;
    std::string section;
    bool have_duration = false;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty())
            continue;
        if (trimmed.compare(0, 4, "Vide") == 0 && trimmed.find(':') == std::string::npos) {
            section = "video";
            continue;
        }
        if (trimmed.compare(0, 5, "Audio") == 0 && trimmed.find(':') == std::string::npos) {
            section = "audio";
            continue;
        }
        if (trimmed.compare(0, 7, "General") == 0 && trimmed.find(':') == std::string::npos) {
            section = "general";
            continue;
        }
        const size_t colon = trimmed.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = Trim(trimmed.substr(0, colon));
        const std::string value = Trim(trimmed.substr(colon + 1));
        if (key == "Duration") {
            if (section != "general" || have_duration)
                continue;
            fields.duration_s = ParseHumanDuration(value);
            have_duration = true;
        } else if (section == "video") {
            if (key == "Format" && fields.video_codec.empty())
                fields.video_codec = value;
            else if (key == "Width")
                fields.width = std::atoi(value.c_str());
            else if (key == "Height")
                fields.height = std::atoi(value.c_str());
        } else if (section == "audio") {
            if (key == "Format" && fields.audio_codec.empty())
                fields.audio_codec = value;
            else if (key == "Sample rate")
                fields.audio_sample_rate = std::atoi(value.c_str());
            else if (key == "Channels")
                fields.audio_channels = std::atoi(value.c_str());
        }
    }
    return fields;
}

std::string JoinStreams(const std::vector<videoeye::model::ContainerStreamInfo>& streams) {
    std::string out;
    for (size_t i = 0; i < streams.size(); ++i) {
        if (i > 0)
            out += ";";
        out += streams[i].type + ":" + streams[i].codec + ":" + streams[i].details;
    }
    return out;
}

struct Record {
    std::string line;
    long long elapsed_ms = 0;
};

std::mutex g_progress_mutex;
std::ofstream g_progress;

void Progress(const std::string& phase, const std::string& path) {
    if (!g_progress.is_open())
        return;
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    g_progress << phase << "\t" << path << "\n";
    g_progress.flush();
}

void ProcessOne(const IndexRow& row, long long timeout_ms, std::vector<Record>& out) {
    const auto t0 = Clock::now();
    Progress("start", row.path);
    std::string fields;
    auto add = [&fields](const std::string& value) { fields += Escape(value) + "\t"; };

    add(row.path);
    add(row.category);
    add(row.container);
    add(row.codec);
    add(row.duration_s);
    add(row.resolution);
    add(row.size_bytes);
    add(row.parse);

    // ---- 1) 格式检测 ----
    const auto detected = videoeye::FormatDetector::Detect(row.path);
    add(videoeye::FormatDetector::FormatName(detected));
    add(std::to_string(static_cast<int>(detected)));
    Progress("format", row.path);

    // ---- 2) 容器结构解析 ----
    TimeoutFlag container_timeout;
    container_timeout.Arm(timeout_ms);
    videoeye::model::ContainerStructureResult structure;
    videoeye::ContainerStructureAnalyzer analyzer;
    const bool struct_ok = analyzer.Analyze(row.path, structure, container_timeout.flag());
    container_timeout.Finish();
    Progress("struct", row.path);

    add(struct_ok ? "1" : "0");
    add(structure.valid ? "1" : "0");
    add(structure.format_name);
    add(structure.error_message);
    add(std::to_string(structure.streams.size()));
    add(JoinStreams(structure.streams));
    add(structure.summary);
    add(container_timeout.TimedOut() ? "1" : "0");

    // ---- 3) 媒体信息解析 ----
    MediaInfoFields media_fields;
    bool media_ok = false;
    bool media_timed_out = false;
    // 清单不再跳过：MediaInfoAnalyzer 已在入口拒绝 HLS/DASH 清单（见
    // MediaInfoAnalyzer::Open 的 LooksLikeStreamingManifest 守卫）。这里照常调用，
    // 超时/崩溃就是回归 —— 清单必须在毫秒级返回 "请在流媒体分析页查看"。
    {
        TimeoutFlag media_timeout;
        media_timeout.Arm(timeout_ms);
        videoeye::MediaInfoAnalyzer media_info;
        media_ok = media_info.Open(row.path, media_timeout.flag());
        media_timeout.Finish();
        Progress("media", row.path);

        add(media_ok ? "1" : "0");
        add(media_ok ? "" : media_info.GetLastError());
        if (media_ok)
            media_fields = ParseMediaInfoText(media_info.GetCompleteInfo());
        media_timed_out = media_timeout.TimedOut();
    }
    char duration_buf[64];
    std::snprintf(duration_buf, sizeof(duration_buf), "%.3f", media_fields.duration_s);
    add(duration_buf);
    add(media_fields.video_codec);
    add(std::to_string(media_fields.width));
    add(std::to_string(media_fields.height));
    add(media_fields.audio_codec);
    add(std::to_string(media_fields.audio_sample_rate));
    add(std::to_string(media_fields.audio_channels));
    add(media_timed_out ? "1" : "0");

    const auto t1 = Clock::now();
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    add(std::to_string(elapsed));

    Record record;
    record.line = fields;
    record.elapsed_ms = elapsed;
    out.push_back(std::move(record));
    Progress("done", row.path);
}

} // namespace

int main(int argc, char** argv) {
    std::string index_path;
    std::string root;
    std::string out_path = "validation_results.tsv";
    std::string progress_path;
    int jobs = 8;
    int limit = 0;
    int skip = 0;
    long long timeout_ms = 20000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--index" && i + 1 < argc)
            index_path = argv[++i];
        else if (arg == "--root" && i + 1 < argc)
            root = argv[++i];
        else if (arg == "--out" && i + 1 < argc)
            out_path = argv[++i];
        else if (arg == "--progress" && i + 1 < argc)
            progress_path = argv[++i];
        else if (arg == "--jobs" && i + 1 < argc)
            jobs = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--limit" && i + 1 < argc)
            limit = std::max(0, std::atoi(argv[++i]));
        else if (arg == "--skip" && i + 1 < argc)
            skip = std::max(0, std::atoi(argv[++i]));
        else if (arg == "--timeout-ms" && i + 1 < argc)
            timeout_ms = std::atoll(argv[++i]);
    }

    if (index_path.empty() || root.empty()) {
        std::cerr << "usage: media_corpus_validate --index <INDEX.tsv> --root <corpus-root>\n"
                     "       [--out results.tsv] [--jobs N] [--limit N] [--skip N]\n"
                     "       [--timeout-ms N] [--progress progress.log]\n"
                     "通常由 scripts/validate_corpus.py 驱动，不必手敲。\n";
        return 2;
    }

    videoeye::Logger::GetInstance().SetLevel(videoeye::LogLevel::ERROR_LEVEL);
    av_log_set_level(AV_LOG_QUIET);
    if (!progress_path.empty())
        g_progress.open(progress_path, std::ios::app);

    std::vector<IndexRow> rows = LoadIndex(index_path, root);
    if (skip > 0) {
        if (skip >= static_cast<int>(rows.size()))
            rows.clear();
        else
            rows.erase(rows.begin(), rows.begin() + skip);
    }
    if (limit > 0 && static_cast<int>(rows.size()) > limit)
        rows.resize(static_cast<size_t>(limit));
    if (rows.empty()) {
        std::cerr << "no rows loaded from " << index_path << "\n";
        return 2;
    }

    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    const size_t total = rows.size();

    // 每个线程本地收集，最后合并，避免共享 vector 竞争。
    std::vector<std::vector<Record>> per_thread(static_cast<size_t>(jobs));
    std::vector<std::thread> threads;
    for (int t = 0; t < jobs; ++t) {
        threads.emplace_back([&, t]() {
            while (true) {
                const size_t i = next.fetch_add(1);
                if (i >= total)
                    break;
                ProcessOne(rows[i], timeout_ms, per_thread[static_cast<size_t>(t)]);
                const size_t completed = done.fetch_add(1) + 1;
                if (completed % 200 == 0) {
                    std::fprintf(stderr, "[%zu/%zu]\n", completed, total);
                }
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    std::vector<Record> merged;
    merged.reserve(total);
    for (auto& bucket : per_thread) {
        for (auto& record : bucket)
            merged.push_back(std::move(record));
    }

    std::ofstream out(out_path, std::ios::binary);
    out << "path\tcategory\tidx_container\tidx_codec\tidx_duration_s\tidx_resolution\tidx_size_bytes\tidx_parse\t"
           "detected_format\tdetected_enum\tstruct_ok\tstruct_valid\tstruct_format_name\tstruct_error\t"
           "struct_stream_count\tstruct_streams\tstruct_summary\tstruct_timed_out\tmi_ok\tmi_error\tmi_duration_s\t"
           "mi_video_codec\tmi_width\tmi_height\tmi_audio_codec\tmi_sample_rate\tmi_channels\tmi_timed_out\telapsed_"
           "ms\n";
    for (const auto& record : merged)
        out << record.line << "\n";
    out.close();

    std::fprintf(stderr, "wrote %zu records to %s\n", merged.size(), out_path.c_str());
    return 0;
}
