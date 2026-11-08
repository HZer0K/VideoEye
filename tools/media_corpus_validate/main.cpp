// 真实语料校验工具：对语料树里的全部媒体文件跑 VideoEye 的三段解析链路
//   1) FormatDetector 魔数/扩展名格式检测
//   2) ContainerStructureAnalyzer 容器结构解析（含 FFmpeg 通用回退）
//   3) MediaInfoAnalyzer 媒体信息解析（FFmpeg 打开 + 探测 + 文本输出）
//   4) truth_*：同一个进程内用**同版本 libavformat** 独立再打开一次，直读原始字段
// 输出 TSV，供 scripts/validate_corpus.py 与 INDEX.tsv 比对并设阈值门禁。
//
// 为什么要 truth_*（切片 2）：结论语义门禁原先拿 PATH 里的 ffprobe 6.1 当真值，
// 而产品链接 8.1.3，11 条已知差异全是「工具版本噪音」。预编译包只给 DLL 不给
// ffprobe.exe，所以改成 validator 自己用同一套 8.1.3 头文件/库直读原始
// AVFormatContext 字段。取值路径与 MediaInfoAnalyzer 的文本组装 + 文本抽取完全
// 独立（一个读 par->width，一个经 FormatFromContext 写文本再 ParseMediaInfoText
// 读回来），因此仍能抓到格式化/抽取层的错误；但它不再是另一个 FFmpeg 版本。
//
// ⚠️ 新列一律追加到表尾，原有 29 列的顺序一个都不能动（离线脚本按列名读，
//    但历史 results.tsv 与人工诊断都按列号，顺序漂移会让所有旧结论失效）。
//
// 这个工具不进默认构建：需要一棵外部语料树（几 GB 到几十 GB），PR 流程不跑它，
// nightly / 本地用 --index/--root 指到语料后由脚本驱动。编译开关:
//   cmake --preset <preset> -DVIDEOEYE_BUILD_CORPUS_VALIDATOR=ON

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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/analysis/orchestration/FormatDetector.h"
#include "core/analysis/orchestration/MediaInfoAnalyzer.h"
#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "infrastructure/logging/Logger.h"
#include "tools/media_corpus_validate/MediaInfoFields.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
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

// ---- 同版本 libavformat 独立真值 ----------------------------------------
//
// 这里刻意**不**复用 MediaInfoAnalyzer 的 Open()：那会变成自证。自己 alloc/open/
// 探测一次，直接读 AVFormatContext / AVCodecParameters 的原始字段，再与
// 「FormatFromContext 写文本 → ParseMediaInfoText 读回来」的 mi_* 列比对。
// 验证目标是结论展示链路（单位、时基、字段错配、格式化精度），不是 FFmpeg 探测
// 本身的正确性；因此探测参数与产品保持一致，避免把参数差异误当成产品缺陷。

std::string AvformatVersionString() {
    const unsigned v = avformat_version();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    return buf;
}

std::string CodecTruthName(AVCodecID id) {
    const char* name = avcodec_get_name(id);
    return (name && *name) ? name : "";
}

int AudioBitDepthTruth(const AVCodecParameters* par) {
    if (par->bits_per_raw_sample > 0)
        return par->bits_per_raw_sample;
    const AVSampleFormat sample_fmt = static_cast<AVSampleFormat>(par->format);
    if (av_sample_fmt_is_planar(sample_fmt) || par->format >= 0)
        return av_get_bytes_per_sample(sample_fmt) * 8;
    return 0;
}

// 与 FormatFromContext 的旋转判定同口径（只认 90/180/270 三种标准矩阵）。
// 矩阵算法本身的正确性由 MediaInfoFormatTests 守，这里比的是「文本抽取有没有读对」。
double RotationTruth(const AVCodecParameters* par) {
    const AVPacketSideData* sd =
        av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 36)
        return 0.0;
    const int32_t* m = reinterpret_cast<const int32_t*>(sd->data);
    if (m[0] == 0 && m[1] == 65536)
        return 90.0;
    if (m[0] == 0 && m[1] == -65536)
        return 270.0;
    if (m[0] == -65536 && m[1] == 0)
        return 180.0;
    return 0.0;
}

struct TruthOutcome {
    corpus_fields::TruthFields fields;
    bool ok = false;
};

TruthOutcome ReadTruth(const std::string& path, long long timeout_ms,
                       const std::shared_ptr<std::atomic<bool>>& cancel) {
    TruthOutcome out;
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt)
        return out;

    videoeye::ffmpeg_io::AvInterruptState interrupt;
    interrupt.cancel = cancel.get();
    const int64_t budget_us = static_cast<int64_t>(timeout_ms) * 1000;
    videoeye::ffmpeg_io::AttachInterrupt(fmt, interrupt, budget_us);
    fmt->probesize = 32 * 1024 * 1024;
    fmt->max_analyze_duration = 5 * AV_TIME_BASE;

    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        // avformat_open_input 失败时会自行释放并把 *ps 置空；再 free(NULL) 是安全的，
        // 与 MediaInfoAnalyzer::Open 的写法保持一致。
        avformat_free_context(fmt);
        return out;
    }
    interrupt.deadline_us = av_gettime() + budget_us;
    // 与产品一致：探测失败仍保留已探到的部分信息，不把「真值」整体判成缺失。
    (void)avformat_find_stream_info(fmt, nullptr);

    corpus_fields::TruthFields& f = out.fields;
    if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE)
        f.duration_s = static_cast<double>(fmt->duration) / static_cast<double>(AV_TIME_BASE);
    if (fmt->iformat && fmt->iformat->name)
        f.format = fmt->iformat->name;
    f.stream_count = static_cast<int>(fmt->nb_streams);
    if (fmt->bit_rate > 0)
        f.overall_bitrate_bps = fmt->bit_rate;

    bool have_video = false;
    bool have_audio = false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (!st || !st->codecpar)
            continue;
        const AVCodecParameters* par = st->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !have_video) {
            have_video = true;
            f.video_codec = CodecTruthName(par->codec_id);
            f.width = par->width;
            f.height = par->height;
            if (const char* profile = avcodec_profile_name(par->codec_id, par->profile))
                f.video_profile = profile;
            if (par->level != AV_LEVEL_UNKNOWN && par->level != 0)
                f.video_level = par->level;
            if (const char* pix = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format)))
                f.pix_fmt = pix;
            f.frame_rate = av_q2d(st->avg_frame_rate);
            if (st->nb_frames > 0)
                f.frame_count = st->nb_frames;
            if (par->bit_rate > 0)
                f.video_bitrate_bps = par->bit_rate;
            if (par->color_space != AVCOL_SPC_UNSPECIFIED)
                if (const char* n = av_color_space_name(par->color_space))
                    f.color_space = n;
            if (par->color_primaries != AVCOL_PRI_UNSPECIFIED)
                if (const char* n = av_color_primaries_name(par->color_primaries))
                    f.color_primaries = n;
            if (par->color_trc != AVCOL_TRC_UNSPECIFIED)
                if (const char* n = av_color_transfer_name(par->color_trc))
                    f.color_transfer = n;
            f.rotation = RotationTruth(par);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && !have_audio) {
            have_audio = true;
            f.audio_codec = CodecTruthName(par->codec_id);
            if (const char* n = av_get_sample_fmt_name(static_cast<AVSampleFormat>(par->format)))
                f.sample_fmt = n;
            f.sample_rate = par->sample_rate;
            f.channels = par->ch_layout.nb_channels;
            f.bit_depth = AudioBitDepthTruth(par);
            if (par->bit_rate > 0)
                f.audio_bitrate_bps = par->bit_rate;
        }
    }

    avformat_close_input(&fmt);
    out.ok = true;
    return out;
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
    std::string line; // 前三段 + 扩展 mi_*；truth_* 由第二阶段追加
    std::string path;
    bool need_truth = false;
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

void ProcessOne(const IndexRow& row, long long timeout_ms, bool truth_enabled, std::vector<Record>& out) {
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
    corpus_fields::MediaInfoFields media_fields;
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
            media_fields = corpus_fields::ParseMediaInfoText(media_info.GetCompleteInfo());
        media_timed_out = media_timeout.TimedOut();
    }
    // 原有 7 个 mi_* 列的顺序是切片 1 定的，与 FieldNames() 的前 7 项不同，
    // 必须按 LegacyMiColumnOrder() 映射，不能图省事直接取 columns[0..6]。
    const auto mi_columns = corpus_fields::ToColumns(media_fields);
    for (const size_t index : corpus_fields::LegacyMiColumnOrder())
        add(mi_columns[index]);
    add(media_timed_out ? "1" : "0");

    const auto t1 = Clock::now();
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    add(std::to_string(elapsed));

    // ---- 4) 追加扩展 mi_* 列（表尾追加，表头与这里必须逐列同序）----
    for (const auto& value : corpus_fields::ExtendedColumns(media_fields))
        add(value);

    Record record;
    record.line = fields;
    record.path = row.path;
    record.need_truth = truth_enabled && media_ok;
    record.elapsed_ms = elapsed;
    out.push_back(std::move(record));
    Progress("parsed", row.path);
}

// ---- 第二阶段：同版本 libavformat 独立真值采集 ---------------------------
//
// ❗ 不要把它塞回 ProcessOne。四段解析挤在同一批 8 线程里时，真值采集的
// FFmpeg 打开/探测会直接抢走同一线程后续文件的 struct/media CPU 预算。
// 分两阶段后可用性判据只反映前三条链路，真值采集再慢也不会污染它们。
// 注意这不能消除「20 s 单阶段预算贴着 orange.jxl 固有耗时」的边界 flaky，
// 那由 validate_corpus.py --timeout-ms 按实测余量解决。
void CollectTruth(std::vector<Record>& records, long long timeout_ms, int jobs, bool truth_enabled) {
    const std::string version = AvformatVersionString();
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < jobs; ++t) {
        threads.emplace_back([&] {
            while (true) {
                const size_t i = next.fetch_add(1);
                if (i >= records.size())
                    break;
                Record& record = records[i];
                TruthOutcome truth;
                if (truth_enabled && record.need_truth) {
                    TimeoutFlag truth_timeout;
                    truth_timeout.Arm(timeout_ms);
                    truth = ReadTruth(record.path, timeout_ms, truth_timeout.flag());
                    truth_timeout.Finish();
                    if (truth_timeout.TimedOut())
                        truth.ok = false;
                }
                if (!truth.ok)
                    truth.fields = corpus_fields::TruthFields{}; // 没拿到可靠真值就不要半截数字
                std::string tail;
                auto add = [&tail](const std::string& value) { tail += Escape(value) + "\t"; };
                for (const auto& value : corpus_fields::ToColumns(truth.fields))
                    add(value);
                add(truth.ok ? "1" : "0");
                add(version);
                record.line += tail;
                Progress("done", record.path);
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
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
    long long timeout_ms = 30000; // 与 validate_corpus.py 一致；慢样本见脚本里的实测说明
    // truth 列始终出现在表头（离线脚本按列名读，表结构必须稳定）；--no-truth
    // 只是跳过真值采集的 IO，用于只想快速跑可用性门禁的场合。
    bool truth_enabled = true;

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
        else if (arg == "--no-truth")
            truth_enabled = false;
    }

    if (index_path.empty() || root.empty()) {
        std::cerr << "usage: media_corpus_validate --index <INDEX.tsv> --root <corpus-root>\n"
                     "       [--out results.tsv] [--jobs N] [--limit N] [--skip N]\n"
                     "       [--timeout-ms N] [--progress progress.log] [--no-truth]\n"
                     "       (--no-truth 跳过同版本 libavformat 真值采集，列仍保留但为空)\n"
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
                ProcessOne(rows[i], timeout_ms, truth_enabled, per_thread[static_cast<size_t>(t)]);
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

    // 三段解析全部结束后才采真值：阶段隔离，避免与 struct/media 抢 CPU 预算。
    CollectTruth(merged, timeout_ms, jobs, truth_enabled);

    std::ofstream out(out_path, std::ios::binary);
    out << "path\tcategory\tidx_container\tidx_codec\tidx_duration_s\tidx_resolution\tidx_size_bytes\tidx_parse\t"
           "detected_format\tdetected_enum\tstruct_ok\tstruct_valid\tstruct_format_name\tstruct_error\t"
           "struct_stream_count\tstruct_streams\tstruct_summary\tstruct_timed_out\tmi_ok\tmi_error\tmi_duration_s\t"
           "mi_video_codec\tmi_width\tmi_height\tmi_audio_codec\tmi_sample_rate\tmi_channels\tmi_timed_out\telapsed_"
           "ms\t";
    for (const auto& name : corpus_fields::ExtendedFieldNames())
        out << "mi_" << name << "\t";
    for (const auto& name : corpus_fields::FieldNames())
        out << "truth_" << name << "\t";
    out << "truth_ok\ttruth_avformat_version\n";
    for (const auto& record : merged)
        out << record.line << "\n";
    out.close();

    std::fprintf(stderr, "wrote %zu records to %s\n", merged.size(), out_path.c_str());
    return 0;
}
