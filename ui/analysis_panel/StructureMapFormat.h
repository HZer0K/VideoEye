#pragma once

// 文件结构图谱的「格式画像」层（切片 4）。
//
// 为什么单独一层：切片 1~3 的三视图对 MP4 很准，对其它容器却会悄悄失真 ——
//   * MP4/MOV   顶层 box 有真实 offset/size，字节布局可信；
//   * MKV/WebM  元素树只读前 32 MiB，尾部 Cues / Tags 根本不在树里；
//               而 diagnostics.mkv 是全文件 O(1) 体检，两者口径不同，不能混说；
//   * MPEG-TS   结构树是「Program → ES → PES 采样」，除 root 外 size 全 0，
//               按字节画面积图只剩一条大色块；transport 只有扫描窗口汇总；
//   * HLS/DASH  清单节点压根没有 size/offset，等宽是投影不是实测；
//   * AVI/FLV/… 结构树受元素上限约束，体检扫描另有窗口；
//   * RAW_ES    没有容器层，树是 FFmpeg 通用信息的回退。
//
// 这一层只做三件事：把「本格式能画什么、哪些没画、依据是什么」变成可单测的
// 纯数据；把它画成第四视图的事实卡片；在字节轴上标出真实扫描窗口。
// ⭐ 所有事实都必须来自 ContainerStructureResult 里已有的字段，一条都不许推。

#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QStringList>

#include <vector>

#include "ui/analysis_panel/StructureMapModel.h"

namespace videoeye {
namespace ui {
namespace structure_map_detail {

enum class MapFormatFamily {
    Unknown = 0,
    Isobmff,         // MP4 / MOV / fMP4
    Ebml,            // MKV / WebM
    TransportStream, // MPEG-TS
    LegacyBinary,    // AVI / FLV / ASF / OGG / MXF / MPEG-PS
    Manifest,        // HLS / DASH
    RawElementary,   // 裸流
    Generic,         // FFmpeg 通用 / 未识别
};

// 一张事实卡片。value 必须是实测值或明确的「未产出」，note 写口径。
// level: 0 = 中性 / 1 = 需要留意（窗口、抽样、缺失）/ 2 = 异常事实。
struct FormatFact {
    QString group;
    QString label;
    QString value;
    QString note;
    int level = 0;
};

struct FormatProfile {
    MapFormatFamily family = MapFormatFamily::Unknown;
    QString family_label;        // "ISO-BMFF · MP4" 这类显示名
    QString structure_note;      // 这棵树到底代表什么（一句话钉住口径）
    bool has_byte_layout = true; // 节点有没有可比较的 size（决定字节占比 / 树图可用性）

    // 扫描窗口：只有拿到精确字节数 / 包数才置 has_precise_window，才允许画边界。
    // 靠 scan_complete=false 推断的「可能没扫完」只在文字里说，不画一条假的分界线。
    bool window_limited = false;
    bool has_precise_window = false;
    double window_fraction = 1.0; // [0,1]，相对整份文件的覆盖比例
    QString window_label;

    QStringList coverage_notes;    // 合进 MapCoverage::facts 的少量口径事实
    std::vector<FormatFact> facts; // 第四视图的卡片
};

FormatProfile BuildFormatProfile(const model::ContainerStructureResult& result);

// 返回空串 = 该视图可用。理由要具体到「少了什么数据」，不说「本格式不支持」。
QString ViewUnsupportedReason(const FormatProfile& profile, MapViewKind kind);
bool ViewSupported(const FormatProfile& profile, MapViewKind kind);
QStringList FormatCoverageFacts(const FormatProfile& profile);

// 第四视图：按 group 分列的事实卡片，绘制 / 命中 / SVG 共用 MapVisual。
std::vector<MapVisual> BuildFormatVisuals(const FormatProfile& profile, QRect area);
QString DescribeFormatFact(const FormatProfile& profile, int index);

// 字节轴的扫描窗口叠加。返回 0 或 1 个 decorative 矩形（不参与命中）。
// 只在 Icicle + has_precise_window 时调用，别把它贴到 treemap / 卡片视图上。
std::vector<MapVisual> BuildWindowOverlay(const FormatProfile& profile, const MapGeometry& geom,
                                          const MapViewport& view);

// JSON 导出里的 profile 段落，与 CSV / SVG 用同一份事实，不另写一套口径。
QJsonObject FormatProfileJson(const FormatProfile& profile);

} // namespace structure_map_detail
} // namespace ui
} // namespace videoeye
