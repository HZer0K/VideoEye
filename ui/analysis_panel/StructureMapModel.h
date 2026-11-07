#pragma once

// 文件结构图谱的纯数据 / 几何变换层（切片 3 从 FileStructureMapPage.h 拆出）。
//
// 只依赖 Qt 的值类型与 core/domain/model，不创建任何 QWidget：CollectNodes 把结构树
// 投影成归一化区间，Build*Visuals 把区间或时间轴投影成矩形，绘制 / 命中 / SVG 导出
// 全部消费同一份矩形。自绘最容易出的不是崩溃，而是"画的位置和点中的位置不是一回事"、
// "空结果导出成一份看起来正常的空表"，这些做成纯函数才能被单测直接钉住。
//
// ⭐ 完整性口径：图上只画结构骨架。逐包数据在 av_packet_unref 后就丢了，MP4 样本表
// 每轨有上限，EBML 只扫窗口，HLS/DASH 节点没有 size/offset。所有裁剪 / 抽样 / 窗口
// 事实必须由 BuildCoverage 分开落，绝不合成一个"完成" —— 那会让半份结果看起来完整。

#include <QByteArray>
#include <QColor>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {
namespace ui {
namespace structure_map_detail {

// 绘制预算与画布几何常量。页面与模型层共用同一份，别在两边各抄一遍 ——
// 少抄 1 像素就会让命中矩形和 SVG 导出对不上。
inline constexpr int kMaxTreeDepth = 20;
inline constexpr int kMaxMapNodes = 4000;
inline constexpr int kDepthLabelWidth = 40;
inline constexpr int kInfoHeight = 38;
inline constexpr int kAxisHeight = 26;
// 底部「流字节分布」预留高度。2 行 × 14 px + 2 px 间隔：刻意保持与 ComputeLod 的
// 预留值一致，别在这里加高 —— 那会让 LOD 算出的可见行数与实际几何错位。
inline constexpr int kStreamHeight = 30;
inline constexpr int kStreamRowHeight = 14;
inline constexpr int kStreamMaxRows = 2;
inline constexpr int kPlotRight = 12;

// 字节数的人读格式。三个视图、格式画像与详情面板共用一份，别各写一套舍入。
QString FormatBytes(quint64 bytes);
// 流类型配色。旧版只在页面 cpp 里有一份暗色，新的密度条与 SVG 要用同一份，别各抄。
QColor StreamColor(const std::string& type);

// 一个可绘制 / 可导出的节点。x0 / x1 是相对文件字节跨度的归一化区间 [0,1]，
// depth 0 = 顶层元素，parent 是前序展平数组里的父下标（顶层为 -1，用于面包屑）。
struct MapNode {
    QString name;
    QString type;
    quint64 offset = 0;
    quint64 size = 0;
    int depth = 0;
    double x0 = 0.0;
    double x1 = 0.0;
    QString value;
    int parent = -1;
};

// 结构图里同一棵树的两种宽度口径：字节占比严格正比于 size；均衡布局压缩动态范围
// 给小盒子留可见份额。两者的拓扑完全一致，只有 x 区间不同 —— CSV 不依赖模式。
enum class MapLayoutMode {
    ByteProportional = 0,
    Balanced = 1,
};

// 视图：Icicle 用 x 区间 + 深度行；Treemap 用面积；PacketBands 用真实样本 /
// 分片 / EBML 块摘要的密度带；FormatOverview（切片 4）画逐格式事实卡片。
// 四者共用 MapVisual 与命中函数。新增视图一律表尾追加，别改已有下标 ——
// 下拉框 currentIndex 直接映射这个枚举，改中间会让用户选中的视图整体错位。
enum class MapViewKind {
    Icicle = 0,
    Treemap = 1,
    PacketBands = 2,
    FormatOverview = 3,
};

// 按深度优先前序展平结构树（顺序与「文件结构」树页一致，别改成 set / 按名排序）。
// max_depth / max_nodes 是绘制预算：达到上限就停，调用方负责标注"已裁剪"。
std::vector<MapNode> CollectNodes(const model::ContainerStructureResult& result, int max_depth, int max_nodes,
                                  MapLayoutMode mode = MapLayoutMode::ByteProportional);

int TreeMaxDepth(const model::ContainerStructureResult& result);
quint64 FileSpan(const model::ContainerStructureResult& result);

// 结构节点 CSV 正文（不含 BOM，BOM 由 WriteCsvStream 统一写）。
// 无效 / 未分析的结果返回空 —— 空表无法自证"没问题是真没问题还是没跑"。
QByteArray BuildStructureCsv(const model::ContainerStructureResult& result);

// 视窗变换：scale = 1 表示整份文件铺满绘图区；origin 是视窗左端的归一化坐标。
// 绘图与鼠标命中都只用这一份变换，避免"画的和点中的不是一回事"。
struct MapViewport {
    double scale = 1.0;
    double origin = 0.0;

    double ToScreen(double x) const {
        return (x - origin) * scale;
    }
    double FromScreen(double px) const {
        return origin + px / scale;
    }
    double Right() const {
        return origin + 1.0 / scale;
    }
};

MapViewport ClampViewport(MapViewport view);
// anchor_x 是归一化文件坐标；缩放后它在屏幕上的相对位置保持不变。
MapViewport ZoomAt(const MapViewport& view, double anchor_x, double factor);
MapViewport FitInterval(const MapViewport& view, double x0, double x1);

// 绘图区几何（像素）。row_height * rows 是节点带高度，流带与刻度在它下面。
struct MapGeometry {
    int left = 40;
    int top = 38;
    int width = 1;
    int row_height = 26;
    int rows = 1;
};

struct MapLod {
    int rows = 1;
    int max_depth = 0;
    int max_nodes = 512;
};

// 按画布尺寸决定能画几层、最多画几个矩形。尺寸越大预算越高，但始终有下限与上限。
MapLod ComputeLod(int plot_width, int plot_height, int tree_max_depth);

// 节点在指定视窗 / 垂直滚动位置下的矩形。绘制、命中、SVG 导出共用它。
QRect NodeRect(const MapNode& node, const MapGeometry& geom, const MapViewport& view, int first_row);
// 命中最上层可见矩形；没有命中返回 -1。
int NodeAt(const std::vector<MapNode>& nodes, const MapGeometry& geom, const MapViewport& view, QPoint pos,
           int first_row);

// 搜索：匹配 name / type / value，大小写不敏感；空关键字返回空（不是"全部命中"）。
QVector<int> MatchNodes(const std::vector<MapNode>& nodes, const QString& query);
QString BuildNodePath(const std::vector<MapNode>& nodes, int index);
QString DescribeNode(const MapNode& node, quint64 file_span);

// 当前结构图视图的 SVG 快照：与画布共用 NodeRect / MapGeometry，导出即所见。
// 无节点返回空，绝不产一张"看起来像成功导出"的空图。
QByteArray BuildStructureSvg(const std::vector<MapNode>& nodes, const MapGeometry& geom, const MapViewport& view,
                             int first_row, MapLayoutMode mode, bool truncated);

// ---------------------------------------------------------------------------
// 统一视觉矩形（切片 3）
// ---------------------------------------------------------------------------
// node_index >= 0 表示它对应 nodes_ 里那个结构节点；track_index / bin_index >= 0
// 表示它是时间轴包带里的一个密度桶；stream_row / stream_bin >= 0 表示它是结构图
// 底部「流字节分布」里的一个桶。四者互斥。绘制、命中、详情、SVG 都读它，
// 这样三个视图不会出现"某个视图点不中 / 导出的和屏幕不一样"。
struct MapVisual {
    int node_index = -1;
    int track_index = -1;
    int bin_index = -1;
    int fact_index = -1; // 格式概览里的一张事实卡片
    int stream_row = -1;
    int stream_bin = -1;
    // 装饰性矩形（扫描窗口外的斜纹区）参与绘制与 SVG，但不参与命中：
    // 它盖在整块绘图区上，若参与命中会把下面的节点全部挡住。
    bool decorative = false;
    QRect rect;
    QColor fill;
    QColor border;
    QString label;
    bool emphasize = false; // 选中 / 命中 / 异常时的强调边框
    // 样本表被截断后的「未展开」区：斜纹装饰，只画不命中。
    // 不标出来，右侧空白会被读成「这条轨没有数据」。
    bool hatched = false;
    // 包带视图专用：柱体实际高度。命中仍用整个 rect（空桶也能点中查看详情），
    // 但填充只占底部 bar_height，否则"没有样本"会被画成一条满高的假密度带。
    int bar_height = -1;
};

std::vector<MapVisual> BuildIcicleVisuals(const std::vector<MapNode>& nodes, const MapGeometry& geom,
                                          const MapViewport& view, int first_row);
// 递归按「压缩字节权重 + 每个兄弟保底」二分切分，父矩形包含子矩形，回答"有哪些盒子、谁大谁小"。
// ❗ 不用严格真实占比：MP4 里 mdat 常占 99%，元数据会被压成 1px 并在递归内缩后彻底消失。
// 真实字节占比仍由 DescribeNode 逐节点给出，图上口径必须与它区分开。
std::vector<MapVisual> BuildTreemapVisuals(const std::vector<MapNode>& nodes, QRect area, int max_visuals);
// 返回命中的 visual 下标（不是 node 下标）；没有命中返回 -1。
int VisualAt(const std::vector<MapVisual>& visuals, QPoint pos);
QByteArray BuildVisualsSvg(const std::vector<MapVisual>& visuals, QSize canvas_size, MapViewKind kind, bool truncated);

// ---------------------------------------------------------------------------
// 时间轴包带（切片 3）
// ---------------------------------------------------------------------------
// 逐包数据在 av_packet_unref 后就丢了，所以这里的数据源只能是「已持久化的样本表 /
// 分片清单」，且必须把它自身的截断与抽查状态一并带出来，不能画成"全文件逐包"。
struct PacketBandBin {
    double t0 = 0.0;
    double t1 = 0.0;
    quint64 bytes = 0;
    quint64 count = 0;
    int keyframes = 0;
    int anomalies = 0;
};

struct PacketBandTrack {
    QString label;
    int stream_index = -1;
    double duration_seconds = 0.0;
    quint64 total_bytes = 0;
    quint64 total_samples = 0;
    bool truncated = false;
    // false = 清单里的分片没落盘 / 没给字节数，此时按"分片数"而不是字节数画，
    // 并在 coverage_note 里写明换过口径，不能让两种量纲混在一张图上。
    bool has_bytes = false;
    std::vector<PacketBandBin> bins;
};

// 横轴量纲。EBML 块摘要只有「相对 Cluster 的 int16 timecode」，没有 Cluster
// 绝对时间码时无法合成时间轴 —— 只能按文件字节偏移分桶，并如实写进 axis_label。
enum class MapBandAxis {
    TimeSeconds = 0,
    ByteOffset = 1,
};

struct PacketBandData {
    bool available = false;
    QString source;        // "MP4 样本表" / "流媒体分片清单" / "EBML 块摘要" / 无数据时的原因
    QString coverage_note; // 抽样 / 截断 / 未探测的如实说明
    bool sampled = false;  // 是否只是抽样结果
    bool partially_probed = false;
    double duration_seconds = 0.0; // 仅 TimeSeconds 轴有意义
    double axis_max = 0.0;         // 横轴最大值（秒或字节），按 axis 解释
    MapBandAxis axis = MapBandAxis::TimeSeconds;
    QString axis_label; // "时间" / "文件字节偏移"
    std::vector<PacketBandTrack> tracks;

    quint64 TotalCount() const {
        quint64 n = 0;
        for (const PacketBandTrack& track : tracks)
            n += track.total_samples;
        return n;
    }
    int BinCount() const {
        int n = 0;
        for (const PacketBandTrack& track : tracks)
            n += static_cast<int>(track.bins.size());
        return n;
    }
};

// max_bins 是每轨的时间桶数（也是绘制预算）。没有可用时间轴时 available=false，
// 并写明原因 —— 绝不退化成一条凭空生成的"平均密度带"。
PacketBandData BuildPacketBands(const model::ContainerStructureResult& result, int max_bins);
std::vector<MapVisual> BuildPacketVisuals(const PacketBandData& bands, QRect area);
QString DescribeBandBin(const PacketBandData& bands, int track_index, int bin_index);

// ---------------------------------------------------------------------------
// 流的真实字节分布（结构图底部）
// ---------------------------------------------------------------------------
// ❗ 流（track）本身没有 offset/size —— ContainerStreamInfo 只有 index/type/codec/details。
// 早期实现把 N 条流按画布宽度等分横铺，画在与上方结构盒共用的字节坐标轴上，
// 读起来就是「video 占前半文件、audio 占后半文件」，纯属把图例当成了数据。
// 真实答案在 Mp4Sample::offset（stco/co64 + stsz）：样本按时间交错存放在 mdat 里，
// 两条流共享同一段字节区间，且都在 ftyp/moov 之后。没有样本偏移就一个矩形都不画。
struct StreamByteBin {
    quint64 bytes = 0;
    quint64 count = 0;
    quint64 keyframes = 0;
    // 桶内样本的真实文件字节范围（实测值，不是由桶宽反推的公式值）。
    quint64 min_offset = 0;
    quint64 max_end = 0;
};

struct StreamByteRow {
    QString label;
    std::string type;
    int stream_index = -1;
    quint64 total_bytes = 0;
    quint64 total_count = 0;
    bool truncated = false;
    // ❗ 每轨样本表按 max_samples_per_track 只展开「前 N 个」。音频包更密，
    // 同样 N 个样本覆盖的文件范围就比视频短；右侧空白是「未展开」不是「无数据」。
    quint64 declared_samples = 0;   // stsz_sample_count（权威总数）
    quint64 expanded_samples = 0;   // samples.size()
    quint64 unexpanded_samples = 0; // declared - expanded
    quint64 last_expanded_end = 0;  // 已展开样本实测最大末端
    double expanded_x1 = 1.0;       // 布局坐标上「已展开」的右边界
    std::vector<StreamByteBin> bins;
};

struct StreamByteRows {
    bool available = false;
    QString source;
    QString coverage_note;
    QString truncation_note; // 简短汇总，画布口径行用；空 = 没有截断
    bool any_truncated = false;
    quint64 file_span = 0;
    quint64 min_offset = 0;           // 实测最小样本偏移
    quint64 max_offset = 0;           // 实测最大样本末端（offset + size）
    quint64 out_of_range_samples = 0; // offset >= file_span 的样本，单列出来不静默吞掉
    int bins = 0;
    std::vector<StreamByteRow> rows;

    quint64 TotalCount() const {
        quint64 n = 0;
        for (const StreamByteRow& row : rows)
            n += row.total_count;
        return n;
    }
};

// ❗ 文件字节偏移 -> icicle 布局坐标的分段线性投影。
// Balanced（默认）下顶层 x 是「压缩权重 + 兄弟保底」算出的份额，不是真实字节位置；
// 流带若直接拿 offset/file_span 当屏幕坐标，就会与上方结构盒错位（数据看起来落进头部）。
// ByteProportional 下投影近似恒等，两种布局因此共用同一条变换链。
struct MapLayoutProjection {
    struct Segment {
        double raw0 = 0.0;
        double raw1 = 0.0;
        double layout0 = 0.0;
        double layout1 = 0.0;
    };
    std::vector<Segment> segments;

    double Project(double nx) const;
    bool valid() const {
        return !segments.empty();
    }
};
MapLayoutProjection BuildLayoutProjection(const std::vector<MapNode>& nodes);

// ⭐ 分桶发生在「布局坐标」而不是「真实字节坐标」上：均衡布局会压缩动态调整范围，
// 按字节等分的桶跨过 box 边界后会被拉成长条，看起来又像数据铺满头部。
// 每个样本先经 projection 投影再归桶，桶内另记真实字节 min/max 供详情显示。
// projection 为空 = 恒等（字节占比视图 / 纯函数单测的默认口径）。
StreamByteRows BuildStreamByteRows(const model::ContainerStructureResult& result, int max_bins,
                                   const MapLayoutProjection& projection = MapLayoutProjection{});
// band_area 与上方结构盒共用同一段 x 轴；view 参与变换，缩放平移后不会错位。
// 只输出「该桶确实有样本」的矩形 —— 空桶画成通栏底色正是用户误读的来源。
std::vector<MapVisual> BuildStreamByteVisuals(const StreamByteRows& rows, QRect band_area, const MapViewport& view,
                                              int row_height);
QString DescribeStreamByteBin(const StreamByteRows& rows, int row_index, int bin_index);

// ---------------------------------------------------------------------------
// 完整性（切片 3）
// ---------------------------------------------------------------------------
// ⭐ 五个维度分开落：分析成功 / 结论完整 / 扫描窗口 / 抽样 / 裁剪 / 错误原因。
// 别把它们合成一个派生值 —— 空表可能是真没问题、打开失败、被取消或超时。
struct MapCoverage {
    bool analyzed = false;
    QString error_reason;
    bool conclusion_complete = false;
    bool window_limited = false;
    bool sampled = false;
    bool truncated = false;
    QStringList facts;

    QString SummaryText() const {
        return facts.join(QStringLiteral(" · "));
    }
};

MapCoverage BuildCoverage(const model::ContainerStructureResult& result, bool nodes_truncated, bool depth_truncated);

// 结构数据 JSON（含 coverage 元信息）。无效 / 空树返回空，与 CSV / SVG 同一反向守护。
QByteArray BuildStructureJson(const model::ContainerStructureResult& result, const MapCoverage& coverage);

} // namespace structure_map_detail
} // namespace ui
} // namespace videoeye
