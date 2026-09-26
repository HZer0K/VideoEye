#pragma once

// 视觉缺陷（人眼可见的画面问题）的数据模型。
//
// 与"元数据级 QC"（容器/码率/色彩规则，见 docs/DIAGNOSTICS_QC.md）的区别:
// 那一类只看容器与码流字段，这一类必须真的看画面内容 ——
// 黑场、冻结、马赛克、模糊、闪烁、过曝欠曝、色偏、隔行梳齿、黑边。
// 检测算法在 core/analyzer/VisualDefectAnalyzer.h。

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/QualityMetric.h"

namespace videoeye {
namespace model {

// 缺陷类型。枚举值顺序即 UI 表格里的默认顺序。
enum class VisualDefectType {
    BlackFrame = 0,     // 黑场
    FreezeFrame,        // 冻结帧（画面静止但音频还在走）
    Blockiness,         // 花屏 / 马赛克（块效应）
    Blur,               // 模糊
    Flicker,            // 闪烁
    OverExposure,       // 过曝
    UnderExposure,      // 欠曝
    ColorCast,          // 色偏
    InterlaceCombing,   // 隔行梳齿（隔行素材当逐行处理）
    Letterbox,          // 上下黑边
    Pillarbox,          // 左右黑边
};

// 严重度。与 QC 规则引擎的 Severity 保持同一套语义，便于以后接入报告。
enum class VisualDefectSeverity {
    Info = 0,
    Warning,
    Error,
    Critical,
};

// 证据缩略图：缺陷起始时刻的一帧小图（RGB24）。
// 为什么不存全分辨率: 播放时每帧留一张 1080p 图内存撑不住，而 QC 报告里
// 只需要"能看出这条缺陷长什么样"的缩略图即可。
struct EvidenceFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;   // RGB24，行优先，无对齐

    bool valid() const {
        return width > 0 && height > 0 &&
               rgb.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    }
};

// 一条缺陷（时间段，不是单帧）。
struct VisualDefect {
    int id = 0;
    VisualDefectType type = VisualDefectType::BlackFrame;
    VisualDefectSeverity severity = VisualDefectSeverity::Info;

    double start_seconds = 0.0;
    double end_seconds = 0.0;
    int start_frame = -1;
    int end_frame = -1;

    // 触发这条缺陷的指标极值（黑场=黑像素比例最大帧，模糊=最模糊帧，...）
    double score = kQualityNoValue;
    // 当时用的阈值。写进报告，方便事后复核"为什么这条算缺陷"。
    double threshold = kQualityNoValue;

    std::string description;   // 中文简述，含关键数值
    EvidenceFrame evidence;    // 可能为空（未采样到 / 采样时 rgb 关闭）

    double DurationSeconds() const { return end_seconds - start_seconds; }
};

// 一段素材的视觉缺陷报告。
struct VisualDefectReport {
    std::vector<VisualDefect> defects;
    std::vector<FrameQualityMetric> samples;

    // 全片有效画面区域（各采样帧 active_area 的中位数）。
    // 单看某一帧会把"刚好全黑的一帧"误判成整片都是黑边，所以取中位数。
    ActivePictureArea effective_area;

    int analyzed_frames = 0;    // 真正参与分析的采样帧数
    int dropped_frames = 0;     // 因队列满被丢弃的帧数（实时播放压力大的证据）
    int total_frames = 0;       // 送进来的帧数（含被抽样跳过的）
    double duration_seconds = 0.0;
    bool analyzed = false;

    int CountByType(VisualDefectType type) const;
    int CountBySeverity(VisualDefectSeverity severity) const;
    bool HasDefect() const { return !defects.empty(); }
};

// 显示用名称（UI 表格 / 报告导出）
const char* ToString(VisualDefectType type);
const char* ToString(VisualDefectSeverity severity);

// 缺陷类型的稳定字符串 id，用于 CSV / JSON 导出与规则匹配，
// 避免把中文本直接写进文件（跨 locale 与 Excel 打开都容易出问题）。
const char* DefectTypeCode(VisualDefectType type);

}  // namespace model
}  // namespace videoeye
