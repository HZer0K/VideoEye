#include "core/analysis/orchestration/AnalysisInputSession.h"

#include <cstdint>
#include <fstream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

// 具体分析器只出现在这里: 它们是本 cpp 的执行者，不进会话的公开头 ——
// 改一个分析器不该让所有 include 了 AnalysisInputSession.h 的地方重编。
#include "core/analysis/container/Mp4SampleTableAnalyzer.h"
#include "core/analysis/orchestration/AnalysisTerminalState.h"
#include "core/media/probe/FileProbe.h"
#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"

namespace videoeye {
namespace analyzer {
namespace {

constexpr int64_t kMaxLayoutScanBytes = 8 * 1024 * 1024;  // moov/mdat 顺序扫描上限

// 大端读取（MP4 box header）
uint32_t ReadBe32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool IsMp4Family(const std::string& format_name) {
    static const char* kNames[] = {"mov", "mp4", "m4v", "3gp", "3g2", "isom", "quicktime", "f4v"};
    for (const char* name : kNames) {
        if (format_name == name) return true;
    }
    return false;
}

// 扫描顶层 box 顺序，判断 moov 是否在 mdat 之后（未 faststart）
bool ScanMoovAfterMdat(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    int64_t offset = 0;
    while (offset < kMaxLayoutScanBytes) {
        unsigned char header[16] = {0};
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        const std::streamsize got = file.gcount();
        if (got < 8) return false;

        const uint32_t size32 = ReadBe32(header);
        const std::string box_type(reinterpret_cast<char*>(header + 4), 4);

        int64_t box_size = static_cast<int64_t>(size32);
        int64_t header_size = 8;
        if (size32 == 1) {  // 64 位 largesize
            if (got < 16) return false;
            const uint64_t large = (static_cast<uint64_t>(ReadBe32(header + 8)) << 32) |
                                   static_cast<uint64_t>(ReadBe32(header + 12));
            if (large < 16) return false;
            box_size = static_cast<int64_t>(large);
            header_size = 16;
        } else if (size32 == 0) {
            return false;  // 延伸到文件尾，无法继续遍历
        }

        if (box_size < header_size) return false;

        if (box_type == "moov") return false;
        if (box_type == "mdat") return true;

        offset += box_size;
    }
    return false;
}

}  // namespace

AnalysisInputSession::AnalysisInputSession(const std::string& file_path, const AnalysisOptions& options,
                                           std::atomic<bool>* cancel_source)
    : file_path_(file_path), options_(options), cancel_source_(cancel_source) {}

AnalysisInputSession::~AnalysisInputSession() {
    // 兜底: 走到这里的路径（例如扫描中途异常返回）都必须已经关过。
    // 重复 close 是安全的 —— avformat_close_input 会先把 *ps 置空。
    Close();
}

bool AnalysisInputSession::IsCancelledExit(int ret) const {
    return cancel_source_ != nullptr &&
           cancel_source_->load(std::memory_order_acquire) &&
           ret == AVERROR_EXIT;
}

void AnalysisInputSession::Close() {
    if (fmt_ != nullptr) {
        avformat_close_input(&fmt_);
        fmt_ = nullptr;
    }
}

AnalysisInputSession::Outcome AnalysisInputSession::Open(model::AnalysisResult& result) {
    fmt_ = avformat_alloc_context();
    if (fmt_ == nullptr) {
        // 同样是失败终态: 只发回调不改状态, 结果对象会停在默认的 Complete。
        // 这里连 FFmpeg 错误码都没有（分配在 avformat 之外就失败了），记 -1。
        MarkFailed(result, "无法分配解封装上下文: " + file_path_);
        return Outcome::Failed;
    }

    // 中断回调：打开/探测阶段带绝对超时；扫描阶段只响应取消（见下方重置）。
    // 用引擎给的 cancel_source_ 而不是任何自备标志: 接了外部取消源（后台任务令牌）时，
    // FFmpeg 的中断回调也必须看同一颗，否则"用户点取消"对 avformat_open_input 是隐形的 ——
    // 取消了半天还卡在打开上。
    interrupt_.cancel = cancel_source_;
    ffmpeg_io::AttachInterrupt(fmt_, interrupt_, ffmpeg_io::kOpenTimeoutUs);

    int open_ret = 0;
    {
        VE_PERF("avformat_open_input");
        open_ret = avformat_open_input(&fmt_, file_path_.c_str(), nullptr, nullptr);
    }
    if (open_ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(open_ret, errbuf, sizeof(errbuf));
        Close();
        std::string msg = "无法打开文件: " + file_path_ + " (" + errbuf + ")";
        // 定向诊断: FFmpeg 通用报错往往不含可操作的修复建议 (如 fMP4 分片缺 init 段)
        const std::string extra = utils::DiagnoseUnopenableFile(file_path_);
        if (!extra.empty()) msg += "。" + extra;
        // 打开阶段被中断回调打断 = 用户取消（含"文件根本不存在也超时"的网络源场景），
        // 不能反过来告诉用户"无法打开文件"
        if (IsCancelledExit(open_ret)) return Outcome::Cancelled;
        result.scan_error_code = open_ret;  // 真 AVERROR 优先，MarkFailed 不会覆盖它
        MarkFailed(result, msg);
        return Outcome::Failed;
    }

    int find_ret = 0;
    {
        VE_PERF("avformat_find_stream_info");
        // 探测阶段允许更长时间，但仍受取消/超时约束
        interrupt_.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
        find_ret = avformat_find_stream_info(fmt_, nullptr);
    }
    if (find_ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(find_ret, errbuf, sizeof(errbuf));
        Close();
        if (IsCancelledExit(find_ret)) return Outcome::Cancelled;
        result.scan_error_code = find_ret;  // 同上：真 AVERROR 优先
        MarkFailed(result, "无法解析流信息: " + file_path_ + " (" + std::string(errbuf) +
                               ", 文件可能损坏或截断)");
        return Outcome::Failed;
    }

    // 进入逐包扫描：关闭绝对截止时间，仅由取消标记中断，
    // 避免长本地文件的正常读取被早期打开超时误杀。
    interrupt_.deadline_us = 0;

    FillContainerFacts(result);
    BuildStreamDigests(result);
    return Outcome::Ok;
}

void AnalysisInputSession::FillContainerFacts(model::AnalysisResult& result) {
    result.container_format = fmt_->iformat && fmt_->iformat->name ? fmt_->iformat->name : "";
    result.duration_seconds = (fmt_->duration > 0)
                                  ? static_cast<double>(fmt_->duration) / static_cast<double>(AV_TIME_BASE)
                                  : 0.0;
    result.file_size_bytes = fmt_->pb ? avio_size(fmt_->pb) : 0;
    result.overall_bitrate_bps = (fmt_->bit_rate > 0) ? fmt_->bit_rate : 0;
    result.seekable = (fmt_->pb == nullptr) ? false : ((fmt_->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0);

    if (options_.detect_container_layout && IsMp4Family(result.container_format)) {
        result.moov_after_mdat = ScanMoovAfterMdat(file_path_);
    }

    // MP4/fMP4 容器一致性校验（自研 IsobmffParser 解析 stbl / moof，与 FFmpeg demux 独立）
    // 只对 MP4 家族执行：其它格式 ISOBMFF 解析必然失败，白跑一遍还要多开一次文件句柄。
    if (options_.analyze_mp4_sample_table && IsMp4Family(result.container_format)) {
        Mp4SampleTableAnalyzer mp4_analyzer;
        bool mp4_ok = false;
        {
            VE_PERF("Mp4SampleTableAnalyzer::AnalyzeFile(诊断扫描)");
            mp4_ok = mp4_analyzer.AnalyzeFile(file_path_, result.mp4_samples,
                                              options_.mp4_sample_table_options);
        }
        if (mp4_ok) {
            result.mp4_samples_analyzed = true;
        } else if (!result.mp4_samples.error_message.empty()) {
            // 解析失败不致命，记一条日志即可（诊断仍走 FFmpeg 那条通路）
            LOG_WARN("MP4 样本表分析失败: " + result.mp4_samples.error_message);
        }
    }
}

void AnalysisInputSession::BuildStreamDigests(model::AnalysisResult& result) {
    // ---- 流摘要 ----
    const double file_duration = result.duration_seconds;
    result.streams.reserve(fmt_->nb_streams);
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        AVStream* st = fmt_->streams[i];
        model::StreamDigest digest;
        digest.index = static_cast<int>(i);
        digest.media_type = static_cast<int>(st->codecpar->codec_type);
        const AVCodecID codec_id = st->codecpar->codec_id;
        const char* codec_name = avcodec_get_name(codec_id);
        digest.codec_name = codec_name ? codec_name : "";
        const char* profile = avcodec_profile_name(codec_id, st->codecpar->profile);
        digest.profile_name = profile ? profile : "";
        digest.width = st->codecpar->width;
        digest.height = st->codecpar->height;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
            digest.avg_fps = av_q2d(st->avg_frame_rate);
        }
        digest.sample_rate = st->codecpar->sample_rate;
        digest.channels = st->codecpar->ch_layout.nb_channels;
        digest.bitrate_bps = (st->codecpar->bit_rate > 0) ? st->codecpar->bit_rate : 0;
        const double tb = av_q2d(st->time_base);
        if (st->start_time != AV_NOPTS_VALUE && tb > 0.0) {
            digest.start_seconds = static_cast<double>(st->start_time) * tb;
        }
        if (st->duration != AV_NOPTS_VALUE && tb > 0.0) {
            digest.duration_seconds = static_cast<double>(st->duration) * tb;
        } else {
            digest.duration_seconds = file_duration;
        }
        result.streams.push_back(std::move(digest));
    }
}

}  // namespace analyzer
}  // namespace videoeye
