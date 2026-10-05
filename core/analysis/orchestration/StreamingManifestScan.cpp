#include "core/analysis/orchestration/StreamingManifestScan.h"

#include <string>

#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"
#include "core/analysis/streaming/SegmentQcAnalyzer.h"
#include "core/media/streaming/ManifestText.h"
#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"

namespace videoeye {
namespace analyzer {

bool StreamingManifestScan::CancelRequested(const std::atomic<bool>* cancel_source) {
    return cancel_source != nullptr && cancel_source->load(std::memory_order_acquire);
}

void StreamingManifestScan::Run(const std::string& file_path, const AnalysisOptions& options,
                                model::AnalysisResult& result, const AnalysisCallbacks& callbacks,
                                const std::atomic<bool>* cancel_source) {
    VE_PERF("StreamingManifestScan::Run");
    const bool is_dash = (result.file_extension == "mpd");
    result.container_format = is_dash ? "dash" : "hls";

    model::StreamingPackageResult& pkg = result.streaming_package;
    bool ok = false;
    {
        VE_PERF("流媒体清单解析");
        if (is_dash) {
            DashManifestAnalyzer dash;
            ok = dash.AnalyzeFile(file_path, pkg, DashManifestOptions{}, cancel_source);
        } else {
            HlsManifestAnalyzer hls;
            ok = hls.AnalyzeFile(file_path, pkg, HlsManifestOptions{}, cancel_source);
        }
    }

    // 取消优先于失败判定：解析被中断时 pkg 里只有半份数据，此时报"无法解析清单"
    // 会把用户主动取消说成文件有问题。与逐包扫描路径一致：保留已扫到的部分，
    // 以 scan_status=Cancelled + completed=false 收尾。
    if (CancelRequested(cancel_source)) {
        NotifyCancelled(callbacks, result);
        return;
    }

    if (!ok) {
        // 清单解析失败也是失败终态：pkg 里只有半截数据，scan_status 必须跟着改成
        // Failed（默认值 Complete 会让"回调报失败 + 结果报完成"同时成立）。
        MarkFailed(result, pkg.error_message.empty() ? ("无法解析清单: " + file_path)
                                                     : pkg.error_message);
        NotifyFailed(callbacks, result.error_message);
        return;
    }

    {
        VE_PERF("SegmentQcAnalyzer::Analyze");
        // 必须接住返回值: Analyze 是三态的, 丢掉它、下面再无条件置 streaming_analyzed /
        // Complete, 等于把"QC 阶段失败"和"QC 阶段被取消"一律报成完整成功 ——
        // 界面上会显示一份只有半截 issues 的分析结果为"分析完成"。
        const SegmentQcAnalyzer::StageStatus qc =
            SegmentQcAnalyzer::Analyze(pkg, options.streaming_package_options, cancel_source);
        if (qc == SegmentQcAnalyzer::StageStatus::kFailed) {
            // 走失败收尾: pkg 里已经填了 error_message / issues, 交给 NotifyFailed 报出去。
            // 与清单解析失败同理: 状态必须一起改, 否则结果是 Failed 的回调 + Complete 的对象。
            MarkFailed(result, pkg.error_message.empty() ? ("分片级校验失败: " + file_path)
                                                         : pkg.error_message);
            NotifyFailed(callbacks, result.error_message);
            return;
        }
        if (qc == SegmentQcAnalyzer::StageStatus::kCancelled) {
            NotifyCancelled(callbacks, result);
            return;
        }
    }
    if (CancelRequested(cancel_source)) {
        NotifyCancelled(callbacks, result);
        return;
    }
    result.streaming_analyzed = true;

    int64_t manifest_size = 0;
    utils::manifest::FileSizeOf(file_path, manifest_size);
    result.file_size_bytes = manifest_size;
    // 时长取清单声明值：分片本体不 demux，拿不到更精确的数字
    if (is_dash) {
        result.duration_seconds = pkg.media_presentation_duration_s;
    } else {
        double longest = 0.0;
        for (const model::MediaPlaylistInfo& pl : pkg.playlists) {
            if (pl.total_duration_seconds > longest) longest = pl.total_duration_seconds;
        }
        result.duration_seconds = longest;
    }
    result.seekable = true;
    result.scan_status = model::AnalysisStatus::Complete;

    LOG_INFO("流媒体清单分析完成: kind=" + std::to_string(static_cast<int>(pkg.kind)) +
             " ladder=" + std::to_string(pkg.ladder.size()) +
             " segments=" + std::to_string(pkg.TotalSegments()) +
             " issues=" + std::to_string(pkg.issues.size()));
    NotifyProgress(callbacks, 100.0, "清单分析完成");
    NotifyFinished(callbacks, true, result);
}

}  // namespace analyzer
}  // namespace videoeye
