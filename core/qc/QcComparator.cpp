#include "core/qc/QcComparator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "core/model/ColorInfo.h"
#include "core/model/HdrMetadataInfo.h"

namespace videoeye {
namespace qc {
namespace {

std::string FixedText(double value, int precision) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(precision) + "f").c_str(), value);
    return std::string(buf);
}

std::string FilenameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

class RowBuilder {
public:
    RowBuilder(std::vector<QcCompareRow>& rows, const QcCompareTolerance& tolerance)
        : rows_(rows), tolerance_(tolerance) {}

    void Text(const char* group, const char* field, const std::string& left,
              const std::string& right) {
        QcCompareRow row;
        row.group = group;
        row.field = field;
        row.left = left;
        row.right = right;
        if (left.empty() && right.empty()) {
            row.diff = QcFieldDiff::Unavailable;
        } else if (left.empty()) {
            row.diff = QcFieldDiff::OnlyRight;
        } else if (right.empty()) {
            row.diff = QcFieldDiff::OnlyLeft;
        } else {
            row.diff = (left == right) ? QcFieldDiff::Same : QcFieldDiff::Different;
        }
        rows_.push_back(std::move(row));
    }

    // available=false 表示这一项两侧都没采集到（例如素材本来就没有音频），不计入差异
    void Number(const char* group, const char* field, int precision, const char* unit,
                bool available, double left, double right, double tolerance) {
        QcCompareRow row;
        row.group = group;
        row.field = field;
        row.numeric = true;
        row.unit = unit ? unit : "";
        if (!available) {
            row.diff = QcFieldDiff::Unavailable;
            rows_.push_back(std::move(row));
            return;
        }
        row.left_value = left;
        row.right_value = right;
        row.delta = right - left;
        row.left = FixedText(left, precision);
        row.right = FixedText(right, precision);
        // 容差取三者最大: 字段自带的量级容差 + 调用方给的绝对值 + 调用方给的相对值
        const double scale = std::max(1.0, std::fabs(left));
        const double limit = std::max(tolerance,
                                      std::max(tolerance_.absolute, tolerance_.relative * scale));
        row.diff = std::fabs(row.delta) <= limit ? QcFieldDiff::Same : QcFieldDiff::Different;
        rows_.push_back(std::move(row));
    }

    void Count(const char* group, const char* field, const char* unit, bool available,
               long long left, long long right) {
        Number(group, field, 0, unit, available, static_cast<double>(left),
               static_cast<double>(right), 0.0);
    }

    void Percent(const char* group, const char* field, bool available, double left, double right,
                 double tolerance) {
        Number(group, field, 2, "%", available, left, right, tolerance);
    }

private:
    std::vector<QcCompareRow>& rows_;
    const QcCompareTolerance& tolerance_;
};

}  // namespace

const char* ToString(QcFieldDiff diff) {
    switch (diff) {
        case QcFieldDiff::Same:        return "一致";
        case QcFieldDiff::Different:   return "不一致";
        case QcFieldDiff::OnlyLeft:    return "仅原文件有";
        case QcFieldDiff::OnlyRight:   return "仅对比文件有";
        case QcFieldDiff::Unavailable: return "无数据";
    }
    return "未知";
}

int QcComparison::DifferentCount() const {
    int count = 0;
    for (const auto& row : rows) {
        if (row.diff == QcFieldDiff::Different) ++count;
    }
    return count;
}

int QcComparison::AvailableRowCount() const {
    int count = 0;
    for (const auto& row : rows) {
        if (row.diff != QcFieldDiff::Unavailable) ++count;
    }
    return count;
}

std::vector<std::string> QcComparison::GroupNames() const {
    std::vector<std::string> groups;
    for (const auto& row : rows) {
        if (std::find(groups.begin(), groups.end(), row.group) == groups.end()) {
            groups.push_back(row.group);
        }
    }
    return groups;
}

QcComparison CompareRuns(const QcRunResult& left, const QcRunResult& right,
                         const QcCompareTolerance& tolerance) {
    QcComparison comparison;
    comparison.left_path = left.analysis.file_path.empty() ? left.report.file_path
                                                           : left.analysis.file_path;
    comparison.right_path = right.analysis.file_path.empty() ? right.report.file_path
                                                             : right.analysis.file_path;
    comparison.left_container = left.analysis.container_format;
    comparison.right_container = right.analysis.container_format;
    comparison.left_score = left.report.score;
    comparison.right_score = right.report.score;
    comparison.left_verdict = left.report.verdict;
    comparison.right_verdict = right.report.verdict;

    const analyzer::AnalysisResult& a = left.analysis;
    const analyzer::AnalysisResult& b = right.analysis;
    RowBuilder rows(comparison.rows, tolerance);

    // ---- 文件级差异：人类最关心的"叫什么名、判定如何、评多少分" ----
    // 放在最前面，保证即便两侧 analysis 都为空，对比表也至少能体现文件名/评分差异。
    rows.Text("文件", "文件名", left.report.file_name, right.report.file_name);
    rows.Text("结论", "判定", left.report.verdict, right.report.verdict);
    rows.Number("结论", "评分", 1, "", true, left.report.score, right.report.score,
                std::max(0.5, tolerance.absolute));
    rows.Number("文件", "时长", 3, "s", true, left.report.duration_seconds,
                right.report.duration_seconds, std::max(0.005, tolerance.absolute));

    // ---- 容器 ----
    rows.Text("容器", "容器格式", a.container_format, b.container_format);
    rows.Number("容器", "时长", 3, "s", true, a.duration_seconds, b.duration_seconds,
                std::max(0.005, tolerance.absolute));
    rows.Number("容器", "文件大小", 1, "MB", true,
                static_cast<double>(a.file_size_bytes) / 1048576.0,
                static_cast<double>(b.file_size_bytes) / 1048576.0,
                std::max(0.05, tolerance.absolute));
    rows.Number("容器", "整体码率", 1, "kbps", true,
                static_cast<double>(a.overall_bitrate_bps) / 1000.0,
                static_cast<double>(b.overall_bitrate_bps) / 1000.0,
                std::max(1.0, tolerance.absolute));
    rows.Count("容器", "视频流数", "条", true, a.VideoStreamCount(), b.VideoStreamCount());
    rows.Count("容器", "音频流数", "条", true, a.AudioStreamCount(), b.AudioStreamCount());
    rows.Count("容器", "包总数", "个", true, a.total_packets, b.total_packets);
    {
        // MP4 家族的 moov 位置：影响点播能不能边下边播
        const bool mp4_like = !a.container_format.empty() &&
                              (a.container_format == b.container_format);
        const std::string left_text = a.moov_after_mdat ? "moov 在 mdat 之后" : "moov 在前";
        const std::string right_text = b.moov_after_mdat ? "moov 在 mdat 之后" : "moov 在前";
        QcCompareRow row;
        row.group = "容器";
        row.field = "moov 位置";
        if (mp4_like) {
            row.left = left_text;
            row.right = right_text;
            row.diff = (a.moov_after_mdat == b.moov_after_mdat) ? QcFieldDiff::Same
                                                                : QcFieldDiff::Different;
        } else {
            row.diff = QcFieldDiff::Unavailable;
        }
        comparison.rows.push_back(std::move(row));
    }

    // ---- 视频 ----
    const analyzer::StreamDigest* va = a.FirstVideoStream();
    const analyzer::StreamDigest* vb = b.FirstVideoStream();
    if (va != nullptr || vb != nullptr) {
        rows.Text("视频", "编码", va ? va->codec_name : "", vb ? vb->codec_name : "");
        rows.Text("视频", "Profile", va ? va->profile_name : "", vb ? vb->profile_name : "");
        rows.Text("视频", "分辨率",
                  va ? (std::to_string(va->width) + "x" + std::to_string(va->height)) : "",
                  vb ? (std::to_string(vb->width) + "x" + std::to_string(vb->height)) : "");
        rows.Number("视频", "平均帧率", 3, "fps", va && vb, va ? va->avg_fps : 0.0,
                    vb ? vb->avg_fps : 0.0, std::max(0.01, tolerance.absolute));
        rows.Number("视频", "视频码率", 1, "kbps", va && vb,
                    va ? static_cast<double>(va->bitrate_bps) / 1000.0 : 0.0,
                    vb ? static_cast<double>(vb->bitrate_bps) / 1000.0 : 0.0,
                    std::max(1.0, tolerance.absolute));
        rows.Count("视频", "帧数", "帧", va && vb, va ? va->frame_count : 0,
                   vb ? vb->frame_count : 0);
        rows.Count("视频", "关键帧数", "个", va && vb, va ? va->key_frame_count : 0,
                   vb ? vb->key_frame_count : 0);
    }

    // ---- 码率与 GOP ----
    {
        rows.Number("码率与 GOP", "平均视频码率", 1, "kbps", true, a.bitrate_gop.avg_bitrate_kbps,
                    b.bitrate_gop.avg_bitrate_kbps, std::max(1.0, tolerance.absolute));
        rows.Number("码率与 GOP", "峰值视频码率", 1, "kbps", true, a.bitrate_gop.peak_bitrate_kbps,
                    b.bitrate_gop.peak_bitrate_kbps, std::max(1.0, tolerance.absolute));
        rows.Number("码率与 GOP", "P95 码率", 1, "kbps", true, a.bitrate_gop.p95_bitrate_kbps,
                    b.bitrate_gop.p95_bitrate_kbps, std::max(1.0, tolerance.absolute));
        rows.Number("码率与 GOP", "峰值/均值", 3, "倍", true, a.bitrate_gop.peak_to_mean_ratio,
                    b.bitrate_gop.peak_to_mean_ratio, 0.02);
        rows.Number("码率与 GOP", "平均帧大小", 1, "B", true, a.bitrate_gop.AverageFrameBytes(),
                    b.bitrate_gop.AverageFrameBytes(), 0.5);

        const bool types_known = a.bitrate_gop.frame_types_known && b.bitrate_gop.frame_types_known;
        rows.Percent("码率与 GOP", "I 帧占比", types_known, a.bitrate_gop.IFrameRatio() * 100.0,
                     b.bitrate_gop.IFrameRatio() * 100.0, 0.05);
        rows.Percent("码率与 GOP", "P 帧占比", types_known, a.bitrate_gop.PFrameRatio() * 100.0,
                     b.bitrate_gop.PFrameRatio() * 100.0, 0.05);
        rows.Percent("码率与 GOP", "B 帧占比", types_known, a.bitrate_gop.BFrameRatio() * 100.0,
                     b.bitrate_gop.BFrameRatio() * 100.0, 0.05);

        rows.Number("码率与 GOP", "GOP 平均时长", 3, "s", true, a.bitrate_gop.gop_duration_mean,
                    b.bitrate_gop.gop_duration_mean, 0.005);
        rows.Number("码率与 GOP", "最长 GOP", 3, "s", true, a.bitrate_gop.gop_duration_max,
                    b.bitrate_gop.gop_duration_max, 0.005);
        rows.Number("码率与 GOP", "关键帧间隔均值", 3, "s", true, a.bitrate_gop.key_interval_mean,
                    b.bitrate_gop.key_interval_mean, 0.005);
        rows.Number("码率与 GOP", "关键帧间隔不规则度", 3, "", true,
                    a.bitrate_gop.key_interval_irregularity,
                    b.bitrate_gop.key_interval_irregularity, 0.01);
        rows.Count("码率与 GOP", "GOP 数量", "个", true,
                   static_cast<long long>(a.bitrate_gop.gops.size()),
                   static_cast<long long>(b.bitrate_gop.gops.size()));
    }

    // ---- 音频 ----
    const analyzer::StreamDigest* aa = a.FirstAudioStream();
    const analyzer::StreamDigest* ab = b.FirstAudioStream();
    if (aa != nullptr || ab != nullptr) {
        rows.Text("音频", "编码", aa ? aa->codec_name : "", ab ? ab->codec_name : "");
        rows.Count("音频", "采样率", "Hz", aa && ab, aa ? aa->sample_rate : 0,
                   ab ? ab->sample_rate : 0);
        rows.Count("音频", "声道数", "个", aa && ab, aa ? aa->channels : 0,
                   ab ? ab->channels : 0);
        rows.Number("音频", "音频码率", 1, "kbps", aa && ab,
                    aa ? static_cast<double>(aa->bitrate_bps) / 1000.0 : 0.0,
                    ab ? static_cast<double>(ab->bitrate_bps) / 1000.0 : 0.0, 0.5);
    }
    {
        const bool both = a.audio_qc.analyzed && a.audio_qc.has_audio && b.audio_qc.analyzed &&
                          b.audio_qc.has_audio;
        rows.Number("音频", "积分响度", 2, "LUFS", both, a.audio_qc.integrated_lufs,
                    b.audio_qc.integrated_lufs, std::max(0.1, tolerance.absolute));
        rows.Number("音频", "真峰值", 2, "dBTP", both, a.audio_qc.true_peak_dbtp,
                    b.audio_qc.true_peak_dbtp, 0.1);
        rows.Number("音频", "响度范围 LRA", 2, "LU", both, a.audio_qc.loudness_range_lu,
                    b.audio_qc.loudness_range_lu, 0.2);
        rows.Number("音频", "短时响度峰值", 2, "LUFS", both, a.audio_qc.short_term_max_lufs,
                    b.audio_qc.short_term_max_lufs, 0.2);
        rows.Number("音频", "RMS", 2, "dBFS", both, a.audio_qc.rms_dbfs, b.audio_qc.rms_dbfs, 0.1);
        rows.Number("音频", "采样峰值", 2, "dBFS", both, a.audio_qc.sample_peak_dbfs,
                    b.audio_qc.sample_peak_dbfs, 0.1);
        rows.Percent("音频", "静音占比", both, a.audio_qc.silence_ratio * 100.0,
                     b.audio_qc.silence_ratio * 100.0, 0.05);
        rows.Count("音频", "削波样本数", "个", both, a.audio_qc.clipping_sample_count,
                   b.audio_qc.clipping_sample_count);
    }

    // ---- 色彩与 HDR ----
    {
        const bool both = a.color_hdr.analyzed && b.color_hdr.analyzed;
        rows.Text("色彩/HDR", "原色", a.color_hdr.color.primaries_name,
                  b.color_hdr.color.primaries_name);
        rows.Text("色彩/HDR", "传递函数", a.color_hdr.color.transfer_name,
                  b.color_hdr.color.transfer_name);
        rows.Text("色彩/HDR", "矩阵系数", a.color_hdr.color.matrix_name,
                  b.color_hdr.color.matrix_name);
        rows.Text("色彩/HDR", "量化范围", a.color_hdr.color.range_name,
                  b.color_hdr.color.range_name);
        rows.Text("色彩/HDR", "像素格式", a.color_hdr.color.pixel_format.name,
                  b.color_hdr.color.pixel_format.name);
        rows.Text("色彩/HDR", "色度采样", a.color_hdr.color.pixel_format.chroma_subsampling,
                  b.color_hdr.color.pixel_format.chroma_subsampling);
        rows.Count("色彩/HDR", "位深", "bit", both, a.color_hdr.color.pixel_format.bit_depth,
                   b.color_hdr.color.pixel_format.bit_depth);
        rows.Count("色彩/HDR", "编码 level", "", both, a.color_hdr.color.level,
                   b.color_hdr.color.level);

        rows.Text("色彩/HDR", "HDR 格式", a.color_hdr.hdr.format_name,
                  b.color_hdr.hdr.format_name);
        const std::string left_cll =
            a.color_hdr.hdr.content_light.HasAny()
                ? (std::to_string(a.color_hdr.hdr.content_light.max_cll) + " / " +
                   std::to_string(a.color_hdr.hdr.content_light.max_fall))
                : "";
        const std::string right_cll =
            b.color_hdr.hdr.content_light.HasAny()
                ? (std::to_string(b.color_hdr.hdr.content_light.max_cll) + " / " +
                   std::to_string(b.color_hdr.hdr.content_light.max_fall))
                : "";
        rows.Text("色彩/HDR", "MaxCLL / MaxFALL", left_cll, right_cll);

        std::string left_dynamic;
        if (a.color_hdr.hdr.dolby_vision.present) left_dynamic += "Dolby Vision ";
        if (a.color_hdr.hdr.has_hdr10_plus) left_dynamic += "HDR10+ ";
        if (a.color_hdr.hdr.has_hdr_vivid) left_dynamic += "HDR Vivid ";
        std::string right_dynamic;
        if (b.color_hdr.hdr.dolby_vision.present) right_dynamic += "Dolby Vision ";
        if (b.color_hdr.hdr.has_hdr10_plus) right_dynamic += "HDR10+ ";
        if (b.color_hdr.hdr.has_hdr_vivid) right_dynamic += "HDR Vivid ";
        rows.Text("色彩/HDR", "动态元数据", left_dynamic, right_dynamic);
    }

    return comparison;
}

}  // namespace qc
}  // namespace videoeye
