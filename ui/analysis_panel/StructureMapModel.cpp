#include "ui/analysis_panel/StructureMapModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <string>

#include "ui/analysis_panel/StructureMapFormat.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {
namespace structure_map_detail {

namespace {

constexpr int kMinMapNodes = 512;
constexpr int kMinRowHeight = 22;
constexpr int kMaxRows = 12;
constexpr double kBalancedExponent = 0.35;
constexpr double kMinSiblingShare = 0.05;
constexpr double kMaxViewScale = 256.0;

double Clamp01(double v) {
    return std::max(0.0, std::min(1.0, v));
}

QByteArray CsvQuote(const QString& text) {
    QString escaped = text;
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QByteArray("\"") + escaped.toUtf8() + QByteArray("\"");
}

QString XmlEscape(const QString& text) {
    QString out = text;
    out.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
    out.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
    out.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
    out.replace(QStringLiteral("\""), QStringLiteral("&quot;"));
    out.replace(QStringLiteral("'"), QStringLiteral("&apos;"));
    return out;
}

QString HtmlEscape(const QString& text) {
    return XmlEscape(text);
}

QColor NodeFill(const QString& name) {
    const QString lower = name.toLower();
    if (lower.contains(QStringLiteral("mdat")) || lower.contains(QStringLiteral("media data")))
        return QColor(theme::color::kBgSurface);
    if (lower.contains(QStringLiteral("moof")) || lower.contains(QStringLiteral("fragment")))
        return QColor(QStringLiteral("#3A2A12"));
    if (lower.contains(QStringLiteral("moov")) || lower.contains(QStringLiteral("trak")) ||
        lower.contains(QStringLiteral("stbl")) || lower.contains(QStringLiteral("ftyp")) ||
        lower.contains(QStringLiteral("head")) || lower.contains(QStringLiteral("meta")))
        return QColor(QStringLiteral("#12263A"));
    return QColor(theme::color::kBgCard);
}

MapNode MakeNode(const model::ContainerElement& element, int depth, double x0, double x1) {
    MapNode node;
    node.name = QString::fromStdString(element.name);
    node.type = QString::fromStdString(element.type);
    node.offset = element.offset;
    node.size = element.size;
    node.depth = depth;
    node.x0 = Clamp01(x0);
    node.x1 = std::max(node.x0, Clamp01(x1));
    node.value = QString::fromStdString(element.value);
    return node;
}

// 兄弟份额：均衡布局用 pow(size, 0.35) 归一化 + 保底；字节占比用 size 本身，
// 全 0 时退化为等分。返回值的和恒为 1（浮点误差在调用处用 min(hi,...) 收口）。
std::vector<double> SiblingShares(const std::vector<model::ContainerElement>& list, MapLayoutMode mode) {
    const size_t n = list.size();
    std::vector<double> shares(n, 0.0);
    if (n == 0)
        return shares;
    const double floor_share =
        mode == MapLayoutMode::Balanced ? std::min(kMinSiblingShare, 0.5 / static_cast<double>(n)) : 0.0;
    const double pool = 1.0 - floor_share * static_cast<double>(n);
    std::vector<double> weights(n, 0.0);
    double total = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double size = static_cast<double>(list[i].size);
        weights[i] = mode == MapLayoutMode::Balanced ? (size > 0.0 ? std::pow(size, kBalancedExponent) : 1.0) : size;
        total += weights[i];
    }
    for (size_t i = 0; i < n; ++i) {
        const double proportional = total > 0.0 ? weights[i] / total : 1.0 / static_cast<double>(n);
        shares[i] = floor_share + pool * proportional;
    }
    return shares;
}

QJsonValue JsonUInt(quint64 value) {
    if (value > static_cast<quint64>(std::numeric_limits<qint64>::max()))
        return QJsonValue(QString::number(value)); // 超出 JSON 整数精度时如实给字符串
    return QJsonValue(static_cast<qint64>(value));
}

} // namespace

QString FormatBytes(quint64 bytes) {
    const double v = static_cast<double>(bytes);
    if (v >= 1024.0 * 1024.0 * 1024.0)
        return QString::number(v / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QStringLiteral(" GiB");
    if (v >= 1024.0 * 1024.0)
        return QString::number(v / (1024.0 * 1024.0), 'f', 2) + QStringLiteral(" MiB");
    if (v >= 1024.0)
        return QString::number(v / 1024.0, 'f', 2) + QStringLiteral(" KiB");
    return QString::number(bytes) + QStringLiteral(" B");
}

// ---------------------------------------------------------------------------
// 结构树 -> 归一化区间
// ---------------------------------------------------------------------------

std::vector<MapNode> CollectNodes(const model::ContainerStructureResult& result, int max_depth, int max_nodes,
                                  MapLayoutMode mode) {
    std::vector<MapNode> out;
    if (max_nodes <= 0 || max_depth < 0 || result.element_tree.empty())
        return out;
    out.reserve(std::min<size_t>(result.element_tree.size(), static_cast<size_t>(max_nodes)));

    std::function<void(const std::vector<model::ContainerElement>&, int, double, double, int)> visit_children;
    visit_children = [&](const std::vector<model::ContainerElement>& list, int depth, double lo, double hi,
                         int parent_index) {
        if (list.empty() || depth > max_depth || static_cast<int>(out.size()) >= max_nodes)
            return;
        const std::vector<double> shares = SiblingShares(list, mode);
        const double width = hi - lo;
        double cursor = lo;
        for (size_t i = 0; i < list.size(); ++i) {
            if (static_cast<int>(out.size()) >= max_nodes)
                return;
            const double x0 = cursor;
            const double x1 = std::min(hi, cursor + shares[i] * width);
            cursor = x1;
            MapNode node = MakeNode(list[i], depth, x0, x1);
            node.parent = parent_index;
            const int index = static_cast<int>(out.size());
            out.push_back(node);
            visit_children(list[i].children, depth + 1, x0, x1, index);
        }
    };

    if (mode == MapLayoutMode::Balanced) {
        const std::vector<double> shares = SiblingShares(result.element_tree, mode);
        double cursor = 0.0;
        for (size_t i = 0; i < result.element_tree.size(); ++i) {
            if (static_cast<int>(out.size()) >= max_nodes)
                break;
            const double x0 = cursor;
            const double x1 = std::min(1.0, cursor + shares[i]);
            cursor = x1;
            MapNode node = MakeNode(result.element_tree[i], 0, x0, x1);
            const int index = static_cast<int>(out.size());
            out.push_back(node);
            visit_children(result.element_tree[i].children, 1, x0, x1, index);
        }
        return out;
    }

    // 字节占比：顶层按真实 offset/size 定位（不是按 size 依次排），子层在父区间内
    // 按份额排布。用 double 求跨度避开 uint64 加法溢出，畸形样本也不会算成负数。
    double min_offset = 1e300;
    double max_end = 0.0;
    for (const model::ContainerElement& top : result.element_tree) {
        min_offset = std::min(min_offset, static_cast<double>(top.offset));
        max_end = std::max(max_end, static_cast<double>(top.offset) + static_cast<double>(top.size));
    }
    const double file_span = max_end - min_offset;
    const bool has_byte_layout = file_span > 0.0;
    const double top_count = static_cast<double>(result.element_tree.size());

    double cursor = 0.0;
    for (const model::ContainerElement& top : result.element_tree) {
        if (static_cast<int>(out.size()) >= max_nodes)
            break;
        double x0 = cursor;
        double x1 = std::min(1.0, cursor + 1.0 / top_count);
        if (has_byte_layout && top.size > 0) {
            x0 = Clamp01((static_cast<double>(top.offset) - min_offset) / file_span);
            x1 = Clamp01((static_cast<double>(top.offset) + static_cast<double>(top.size) - min_offset) / file_span);
            // 大小被裁成 0 / offset 落在区间外的畸形节点仍要给它一条可见的缝，
            // 否则图上会凭空少一个顶层元素。
            if (x1 <= x0) {
                x0 = cursor;
                x1 = std::min(1.0, cursor + 1.0 / top_count);
            }
        }
        cursor = std::max(cursor, x1);
        const int index = static_cast<int>(out.size());
        out.push_back(MakeNode(top, 0, x0, x1));
        visit_children(top.children, 1, x0, x1, index);
    }
    return out;
}

int TreeMaxDepth(const model::ContainerStructureResult& result) {
    std::function<int(const std::vector<model::ContainerElement>&, int)> walk;
    walk = [&](const std::vector<model::ContainerElement>& list, int depth) {
        int deepest = depth;
        for (const model::ContainerElement& element : list)
            deepest = std::max(deepest, walk(element.children, depth + 1));
        return deepest;
    };
    int deepest = -1;
    for (const model::ContainerElement& top : result.element_tree)
        deepest = std::max(deepest, walk(top.children, 0));
    return deepest;
}

quint64 FileSpan(const model::ContainerStructureResult& result) {
    double max_end = 0.0;
    for (const model::ContainerElement& top : result.element_tree)
        max_end = std::max(max_end, static_cast<double>(top.offset) + static_cast<double>(top.size));
    if (max_end >= static_cast<double>(UINT64_MAX))
        return UINT64_MAX;
    return static_cast<quint64>(max_end);
}

QByteArray BuildStructureCsv(const model::ContainerStructureResult& result) {
    // 反向守护：无效结果 / 空树不产表。只有"命中才出一行"的格式自证不了完整性，
    // 一个空 CSV 分不清"真没有节点"还是"分析失败 / 没运行 / 被取消"。
    if (!result.valid)
        return {};
    const std::vector<MapNode> nodes = CollectNodes(result, kMaxTreeDepth, 200000);
    if (nodes.empty())
        return {};

    QByteArray csv = "name,type,offset,size,depth,value\n";
    for (const MapNode& node : nodes) {
        csv += CsvQuote(node.name) + "," + CsvQuote(node.type) + "," + QByteArray::number(node.offset) + "," +
               QByteArray::number(node.size) + "," + QByteArray::number(node.depth) + "," + CsvQuote(node.value) + "\n";
    }
    return csv;
}

MapViewport ClampViewport(MapViewport view) {
    view.scale = std::max(1.0, std::min(kMaxViewScale, view.scale));
    view.origin = std::max(0.0, std::min(1.0 - 1.0 / view.scale, view.origin));
    return view;
}

MapViewport ZoomAt(const MapViewport& view, double anchor_x, double factor) {
    MapViewport next = view;
    const double wanted = std::max(1.0, std::min(kMaxViewScale, view.scale * factor));
    // (anchor - origin) * scale 在缩放前后不变 => 锚点仍停在同一屏幕位置。
    next.origin = anchor_x - (anchor_x - view.origin) * view.scale / wanted;
    next.scale = wanted;
    return ClampViewport(next);
}

MapViewport FitInterval(const MapViewport& view, double x0, double x1) {
    (void)view;
    const double width = std::max(0.02, x1 - x0);
    MapViewport next;
    next.scale = std::max(1.0, std::min(kMaxViewScale, 1.0 / width));
    next.origin = x0;
    return ClampViewport(next);
}

MapLod ComputeLod(int plot_width, int plot_height, int tree_max_depth) {
    MapLod lod;
    const int usable = std::max(0, plot_height - kInfoHeight - kAxisHeight - kStreamHeight);
    const int fit_rows = usable / kMinRowHeight;
    const int layers = std::min(std::max(1, tree_max_depth + 1), kMaxRows);
    lod.rows = std::max(1, std::min(fit_rows, layers));
    lod.max_depth = lod.rows - 1;
    const double budget = static_cast<double>(std::max(1, plot_width)) * lod.rows / 6.0;
    lod.max_nodes = static_cast<int>(std::max<double>(kMinMapNodes, std::min<double>(kMaxMapNodes, budget)));
    return lod;
}

QRect NodeRect(const MapNode& node, const MapGeometry& geom, const MapViewport& view, int first_row) {
    const double sx0 = view.ToScreen(node.x0);
    const double sx1 = view.ToScreen(node.x1);
    const int x = geom.left + static_cast<int>(std::lround(sx0 * geom.width));
    const int x2 = geom.left + static_cast<int>(std::lround(sx1 * geom.width));
    const int row = node.depth - first_row;
    const int y = geom.top + row * geom.row_height;
    return QRect(x, y, std::max(2, x2 - x - 2), geom.row_height - 3);
}

int NodeAt(const std::vector<MapNode>& nodes, const MapGeometry& geom, const MapViewport& view, QPoint pos,
           int first_row) {
    if (pos.x() < geom.left || pos.x() > geom.left + geom.width || pos.y() < geom.top ||
        pos.y() >= geom.top + geom.rows * geom.row_height)
        return -1;
    // 前序里父在子前面；同一行不会有重叠，最后一个命中就是画在最上面的那个。
    int hit = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const int row = nodes[i].depth - first_row;
        if (row < 0 || row >= geom.rows)
            continue;
        if (NodeRect(nodes[i], geom, view, first_row).contains(pos))
            hit = static_cast<int>(i);
    }
    return hit;
}

QVector<int> MatchNodes(const std::vector<MapNode>& nodes, const QString& query) {
    QVector<int> hits;
    if (query.trimmed().isEmpty())
        return hits;
    const QString needle = query.trimmed().toLower();
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name.toLower().contains(needle) || nodes[i].type.toLower().contains(needle) ||
            nodes[i].value.toLower().contains(needle))
            hits.append(static_cast<int>(i));
    }
    return hits;
}

QString BuildNodePath(const std::vector<MapNode>& nodes, int index) {
    if (index < 0 || index >= static_cast<int>(nodes.size()))
        return {};
    QStringList parts;
    int cursor = index;
    int guard = 0;
    while (cursor >= 0 && guard++ < 1024) {
        parts.prepend(nodes[cursor].name);
        cursor = nodes[cursor].parent;
    }
    return parts.join(QStringLiteral(" › "));
}

QString DescribeNode(const MapNode& node, quint64 file_span) {
    const double pct = file_span > 0 ? 100.0 * static_cast<double>(node.size) / static_cast<double>(file_span) : 0.0;
    const quint64 end = node.offset > UINT64_MAX - node.size ? UINT64_MAX : node.offset + node.size;
    QString html = QStringLiteral("<div style='color:%1'><b style='font-size:13px'>%2</b><br>"
                                  "<span style='color:%3'>%4</span><br><br>"
                                  "<table cellspacing='2'>"
                                  "<tr><td style='color:%3'>类型</td><td>%5</td></tr>"
                                  "<tr><td style='color:%3'>层级</td><td>L%6</td></tr>"
                                  "<tr><td style='color:%3'>偏移</td><td>0x%7 (%8)</td></tr>"
                                  "<tr><td style='color:%3'>大小</td><td>%9 (%10 B)</td></tr>"
                                  "<tr><td style='color:%3'>区间</td><td>%11 – %12</td></tr>"
                                  "<tr><td style='color:%3'>占比</td><td>%13%</td></tr>")
                       .arg(QString::fromLatin1(theme::color::kTextPrimary), HtmlEscape(node.name),
                            QString::fromLatin1(theme::color::kTextSecondary), HtmlEscape(node.type),
                            QString::fromLatin1(theme::color::kTextSecondary))
                       .arg(node.depth)
                       .arg(node.offset, 0, 16)
                       .arg(node.offset)
                       .arg(FormatBytes(node.size))
                       .arg(node.size)
                       .arg(node.offset)
                       .arg(end)
                       .arg(pct, 0, 'f', 3);
    if (!node.value.isEmpty())
        html += QStringLiteral("<tr><td style='color:%1'>值</td><td>%2</td></tr>")
                    .arg(QString::fromLatin1(theme::color::kTextSecondary), HtmlEscape(node.value));
    html += QStringLiteral("</table></div>");
    return html;
}

QByteArray BuildStructureSvg(const std::vector<MapNode>& nodes, const MapGeometry& geom, const MapViewport& view,
                             int first_row, MapLayoutMode mode, bool truncated) {
    // 反向守护：没有事实就不产图。一张空白 SVG 分不清"没节点"和"分析失败"。
    if (nodes.empty())
        return {};
    const int svg_w = geom.left + geom.width + kPlotRight;
    const int svg_h = geom.top + geom.rows * geom.row_height + kAxisHeight;
    QByteArray svg = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + QByteArray::number(svg_w) + "\" height=\"" +
           QByteArray::number(svg_h) + "\" viewBox=\"0 0 " + QByteArray::number(svg_w) + " " +
           QByteArray::number(svg_h) + "\">\n";
    svg += "<!-- layout=" + QByteArray(mode == MapLayoutMode::Balanced ? "balanced" : "byte-proportional") +
           " truncated=" + QByteArray(truncated ? "yes" : "no") + " scale=" + QByteArray::number(view.scale, 'f', 3) +
           " -->\n";
    svg += "<rect width=\"100%\" height=\"100%\" fill=\"" + QByteArray(theme::color::kBg) + "\"/>\n";
    int drawn = 0;
    for (const MapNode& node : nodes) {
        const int row = node.depth - first_row;
        if (row < 0 || row >= geom.rows)
            continue;
        const QRect box = NodeRect(node, geom, view, first_row);
        if (box.right() < geom.left || box.left() > geom.left + geom.width)
            continue;
        svg += "<rect x=\"" + QByteArray::number(box.x()) + "\" y=\"" + QByteArray::number(box.y()) + "\" width=\"" +
               QByteArray::number(box.width()) + "\" height=\"" + QByteArray::number(box.height()) + "\" fill=\"" +
               NodeFill(node.name).name().toUtf8() + "\" stroke=\"" + QByteArray(theme::color::kBorder) + "\"/>\n";
        if (box.width() > 40) {
            svg += "<text x=\"" + QByteArray::number(box.x() + 4) + "\" y=\"" +
                   QByteArray::number(box.y() + box.height() / 2 + 4) + "\" fill=\"" +
                   QByteArray(theme::color::kTextPrimary) + "\" font-size=\"11\">" + XmlEscape(node.name).toUtf8() +
                   "</text>\n";
        }
        ++drawn;
    }
    svg += "</svg>\n";
    return drawn > 0 ? svg : QByteArray();
}

// ---------------------------------------------------------------------------
// 统一视觉矩形：三个视图的绘制 / 命中 / 导出共用
// ---------------------------------------------------------------------------

std::vector<MapVisual> BuildIcicleVisuals(const std::vector<MapNode>& nodes, const MapGeometry& geom,
                                          const MapViewport& view, int first_row) {
    std::vector<MapVisual> out;
    out.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const MapNode& node = nodes[i];
        const int row = node.depth - first_row;
        if (row < 0 || row >= geom.rows)
            continue;
        const QRect box = NodeRect(node, geom, view, first_row);
        if (box.right() < geom.left || box.left() > geom.left + geom.width)
            continue;
        MapVisual visual;
        visual.node_index = static_cast<int>(i);
        visual.rect = box;
        visual.fill = NodeFill(node.name);
        visual.border = QColor(theme::color::kBorder);
        visual.label = box.width() > 44 ? node.name : QString();
        out.push_back(visual);
    }
    return out;
}

namespace {

struct TreemapItem {
    int index = -1;
    double weight = 1.0;
};

// ❗ 不能拿原始 size 当权重：MP4 里 mdat 常占 99%，ftyp/moov 会被压成 1px，
// 递归内缩两次后子层直接塌到不可见 —— 切片 1 在 icicle 上踩过的坑，treemap 一样会踩。
// 用与均衡布局同一口径的 pow(size, 0.35) 压缩动态范围，再给每个兄弟一份保底，
// 让"有哪些盒子"和"谁大谁小"都还看得出来。真实占比仍写在节点详情里。
std::vector<TreemapItem> MakeTreemapItems(const std::vector<int>& indices, const std::vector<MapNode>& nodes) {
    std::vector<TreemapItem> items;
    items.reserve(indices.size());
    double sum = 0.0;
    for (int index : indices) {
        const double raw =
            nodes[index].size > 0 ? std::pow(static_cast<double>(nodes[index].size), kBalancedExponent) : 1.0;
        items.push_back({index, raw});
        sum += raw;
    }
    if (items.empty())
        return items;
    const double floor = 0.08 * sum / static_cast<double>(items.size());
    for (TreemapItem& item : items)
        item.weight += floor;
    return items;
}

// 按权重近半二分 + 沿长边交替切分。比经典 slice-and-dice 更接近方形，
// 又比完整 squarify 少一次排序与回溯；父矩形一定包含子矩形（子区内缩）。
void LayoutTreemapGroup(const std::vector<TreemapItem>& items, QRect area, const std::vector<MapNode>& nodes,
                        const std::vector<std::vector<int>>& children, std::vector<MapVisual>& out, int& budget,
                        int depth) {
    if (items.empty() || budget <= 0 || area.width() < 2 || area.height() < 2)
        return;

    if (items.size() == 1) {
        const int index = items[0].index;
        if (index < 0 || index >= static_cast<int>(nodes.size()))
            return;
        --budget;
        MapVisual visual;
        visual.node_index = index;
        visual.rect = area;
        visual.fill = NodeFill(nodes[index].name);
        visual.border = QColor(theme::color::kBorder);
        visual.label = (area.width() > 42 && area.height() > 16) ? nodes[index].name : QString();
        out.push_back(visual);
        if (depth >= 6 || children[index].empty())
            return;
        const QRect inner = area.adjusted(3, 15, -3, -3);
        if (inner.width() < 8 || inner.height() < 8)
            return;
        const std::vector<TreemapItem> kids = MakeTreemapItems(children[index], nodes);
        LayoutTreemapGroup(kids, inner, nodes, children, out, budget, depth + 1);
        return;
    }

    double total = 0.0;
    for (const TreemapItem& item : items)
        total += item.weight;
    size_t split = 1;
    double best_diff = std::numeric_limits<double>::max();
    double acc = 0.0;
    for (size_t i = 0; i + 1 < items.size(); ++i) {
        acc += items[i].weight;
        const double diff = std::abs((total - acc) - acc);
        if (diff < best_diff) {
            best_diff = diff;
            split = i + 1;
        }
    }
    double left_weight = 0.0;
    for (size_t i = 0; i < split; ++i)
        left_weight += items[i].weight;
    const double ratio = total > 0.0 ? std::max(0.02, std::min(0.98, left_weight / total)) : 0.5;

    QRect first = area;
    QRect second = area;
    if (area.width() >= area.height()) {
        const int cut = std::max(1, std::min(area.width() - 1, static_cast<int>(std::lround(area.width() * ratio))));
        first = QRect(area.left(), area.top(), cut, area.height());
        second = QRect(area.left() + cut, area.top(), area.width() - cut, area.height());
    } else {
        const int cut = std::max(1, std::min(area.height() - 1, static_cast<int>(std::lround(area.height() * ratio))));
        first = QRect(area.left(), area.top(), area.width(), cut);
        second = QRect(area.left(), area.top() + cut, area.width(), area.height() - cut);
    }
    const std::vector<TreemapItem> left(items.begin(), items.begin() + static_cast<ptrdiff_t>(split));
    const std::vector<TreemapItem> right(items.begin() + static_cast<ptrdiff_t>(split), items.end());
    LayoutTreemapGroup(left, first, nodes, children, out, budget, depth);
    LayoutTreemapGroup(right, second, nodes, children, out, budget, depth);
}

} // namespace

std::vector<MapVisual> BuildTreemapVisuals(const std::vector<MapNode>& nodes, QRect area, int max_visuals) {
    if (nodes.empty() || area.width() < 8 || area.height() < 8 || max_visuals <= 0)
        return {};
    std::vector<std::vector<int>> children(nodes.size());
    std::vector<int> root_indices;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent >= 0 && nodes[i].parent < static_cast<int>(nodes.size()))
            children[nodes[i].parent].push_back(static_cast<int>(i));
        else
            root_indices.push_back(static_cast<int>(i));
    }
    if (root_indices.empty())
        return {};
    const std::vector<TreemapItem> roots = MakeTreemapItems(root_indices, nodes);
    std::vector<MapVisual> out;
    out.reserve(std::min<size_t>(nodes.size(), static_cast<size_t>(max_visuals)));
    int budget = std::min(max_visuals, kMaxMapNodes);
    LayoutTreemapGroup(roots, area, nodes, children, out, budget, 0);
    return out;
}

int VisualAt(const std::vector<MapVisual>& visuals, QPoint pos) {
    // 后画的盖在前面（子矩形 / 上方的桶），所以取最后一个命中。
    // decorative（扫描窗口叠加 / 组标题）只负责画，绝不能把下面的节点挡住。
    int hit = -1;
    for (size_t i = 0; i < visuals.size(); ++i) {
        if (!visuals[i].decorative && visuals[i].rect.contains(pos))
            hit = static_cast<int>(i);
    }
    return hit;
}

QByteArray BuildVisualsSvg(const std::vector<MapVisual>& visuals, QSize canvas_size, MapViewKind kind, bool truncated) {
    if (visuals.empty() || canvas_size.width() < 8 || canvas_size.height() < 8)
        return {};
    const char* view_name = kind == MapViewKind::Treemap          ? "treemap"
                            : kind == MapViewKind::FormatOverview ? "format-overview"
                            : kind == MapViewKind::Icicle         ? "structure-map"
                                                                  : "packet-bands";
    QByteArray svg = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + QByteArray::number(canvas_size.width()) +
           "\" height=\"" + QByteArray::number(canvas_size.height()) + "\" viewBox=\"0 0 " +
           QByteArray::number(canvas_size.width()) + " " + QByteArray::number(canvas_size.height()) + "\">\n";
    svg += "<!-- view=" + QByteArray(view_name) + " truncated=" + QByteArray(truncated ? "yes" : "no") + " -->\n";
    svg += "<rect width=\"100%\" height=\"100%\" fill=\"" + QByteArray(theme::color::kBg) + "\"/>\n";
    int drawn = 0;
    for (const MapVisual& visual : visuals) {
        const QRect box = visual.rect;
        if (box.width() <= 0 || box.height() <= 0)
            continue;
        const int fill_h =
            visual.bar_height >= 0 ? std::max(1, std::min(box.height(), visual.bar_height)) : box.height();
        const int fill_y = visual.bar_height >= 0 ? box.bottom() + 1 - fill_h : box.y();
        svg += "<rect x=\"" + QByteArray::number(box.x()) + "\" y=\"" + QByteArray::number(fill_y) + "\" width=\"" +
               QByteArray::number(box.width()) + "\" height=\"" + QByteArray::number(fill_h) + "\" fill=\"" +
               visual.fill.name().toUtf8() + "\" stroke=\"" + visual.border.name().toUtf8() + "\"/>\n";
        if (visual.label.isEmpty() || box.width() <= 30 || fill_h < 12)
            ++drawn;
        else {
            svg += "<text x=\"" + QByteArray::number(box.x() + 4) + "\" y=\"" + QByteArray::number(box.y() + 13) +
                   "\" fill=\"" + QByteArray(theme::color::kTextPrimary) + "\" font-size=\"11\">" +
                   XmlEscape(visual.label).toUtf8() + "</text>\n";
            ++drawn;
        }
    }
    svg += "</svg>\n";
    return drawn > 0 ? svg : QByteArray();
}

// ---------------------------------------------------------------------------
// 时间轴包带：只用已持久化的样本表 / 分片清单，不假装逐包还在
// ---------------------------------------------------------------------------

PacketBandData BuildPacketBands(const model::ContainerStructureResult& result, int max_bins) {
    const int bins = std::max(8, std::min(256, max_bins));
    PacketBandData out;
    if (!result.valid) {
        out.source = QStringLiteral("未分析");
        out.coverage_note = QStringLiteral("结构分析未成功，无包级时间轴数据");
        return out;
    }

    const model::Mp4SampleTableResult& samples = result.mp4_samples;
    if (samples.valid && samples.TotalSamples() > 0) {
        bool any_timescale = false;
        bool any_truncated = false;
        for (const model::Mp4TrackSampleTable& track : samples.tracks) {
            if (track.samples.empty())
                continue;
            PacketBandTrack band;
            band.stream_index = static_cast<int>(out.tracks.size());
            band.label = QStringLiteral("#%1 %2 %3")
                             .arg(band.stream_index)
                             .arg(QString::fromStdString(track.type), QString::fromStdString(track.codec));
            band.truncated = track.samples_truncated;
            any_truncated = any_truncated || track.samples_truncated;
            const uint32_t timescale = track.media_timescale;
            any_timescale = any_timescale || timescale > 0;
            band.duration_seconds = track.DurationSeconds();
            if (band.duration_seconds <= 0.0 && timescale > 0) {
                for (const model::Mp4Sample& sample : track.samples)
                    band.duration_seconds = std::max(band.duration_seconds,
                                                     sample.CtsSeconds(timescale) + sample.DurationSeconds(timescale));
            }
            if (band.duration_seconds <= 0.0)
                continue; // 没有可换算的时间轴，绝不用"按样本数均分"编一条
            band.bins.resize(static_cast<size_t>(bins));
            for (int i = 0; i < bins; ++i) {
                band.bins[i].t0 = band.duration_seconds * i / bins;
                band.bins[i].t1 = band.duration_seconds * (i + 1) / bins;
            }
            for (const model::Mp4Sample& sample : track.samples) {
                const double t = sample.CtsSeconds(timescale);
                const int index = std::max(0, std::min(bins - 1, static_cast<int>(t / band.duration_seconds * bins)));
                PacketBandBin& bin = band.bins[static_cast<size_t>(index)];
                bin.bytes += sample.size;
                ++bin.count;
                if (sample.keyframe)
                    ++bin.keyframes;
                if (sample.IsAnomalous())
                    ++bin.anomalies;
                band.total_bytes += sample.size;
                ++band.total_samples;
            }
            band.has_bytes = band.total_bytes > 0;
            out.duration_seconds = std::max(out.duration_seconds, band.duration_seconds);
            out.tracks.push_back(std::move(band));
        }
        if (!any_timescale) {
            out.tracks.clear();
            out.duration_seconds = 0.0;
            out.source = QStringLiteral("MP4 样本表缺少时基");
            out.coverage_note = QStringLiteral("mdhd.timescale 为 0，样本时间戳无法换算成秒，不生成时间轴");
            return out;
        }
        out.available = !out.tracks.empty() && out.duration_seconds > 0.0 && out.TotalCount() > 0;
        out.source = QStringLiteral("MP4 样本表（stsz / stts / ctts / stss）");
        out.sampled = any_truncated;
        QStringList notes;
        notes << QStringLiteral("逐包数据在 demux 时已释放，本视图是样本表的时间桶聚合，不是逐包明细");
        if (any_truncated)
            notes << QStringLiteral("样本表已截断（每轨有上限），右侧时间段密度为抽样结果");
        out.coverage_note = notes.join(QStringLiteral("；"));
        out.axis = MapBandAxis::TimeSeconds;
        out.axis_label = QStringLiteral("时间（秒）");
        out.axis_max = out.duration_seconds;
        return out;
    }

    const model::StreamingPackageResult& streaming = result.streaming_package;
    if (streaming.valid && streaming.TotalSegments() > 0) {
        std::vector<const std::vector<model::SegmentInfo>*> lists;
        for (const model::MediaPlaylistInfo& playlist : streaming.playlists)
            lists.push_back(&playlist.segments);
        for (const model::DashRepresentationInfo& representation : streaming.representations)
            lists.push_back(&representation.segments);
        double duration = 0.0;
        for (const std::vector<model::SegmentInfo>* list : lists) {
            for (const model::SegmentInfo& segment : *list)
                duration = std::max(duration, segment.EndSeconds());
        }
        if (duration <= 0.0) {
            out.source = QStringLiteral("流媒体清单没有可用时长");
            out.coverage_note = QStringLiteral("分片未给出 start/duration，无法构造时间轴");
            return out;
        }
        int list_index = 0;
        for (const std::vector<model::SegmentInfo>* list : lists) {
            PacketBandTrack band;
            band.stream_index = list_index++;
            band.label = QStringLiteral("列表 #%1").arg(band.stream_index);
            band.duration_seconds = duration;
            band.bins.resize(static_cast<size_t>(bins));
            for (int i = 0; i < bins; ++i) {
                band.bins[i].t0 = duration * i / bins;
                band.bins[i].t1 = duration * (i + 1) / bins;
            }
            for (const model::SegmentInfo& segment : *list) {
                if (segment.partial || segment.preload_hint)
                    continue;
                const int index =
                    std::max(0, std::min(bins - 1, static_cast<int>(segment.start_seconds / duration * bins)));
                PacketBandBin& bin = band.bins[static_cast<size_t>(index)];
                if (segment.exists && segment.file_size_bytes > 0)
                    bin.bytes += static_cast<quint64>(segment.file_size_bytes);
                ++bin.count;
                if (segment.starts_with_keyframe)
                    ++bin.keyframes;
                if (segment.gap || segment.container_parse_failed)
                    ++bin.anomalies;
                band.total_bytes +=
                    (segment.exists && segment.file_size_bytes > 0) ? static_cast<quint64>(segment.file_size_bytes) : 0;
                ++band.total_samples;
            }
            band.has_bytes = band.total_bytes > 0;
            if (band.total_samples > 0)
                out.tracks.push_back(std::move(band));
        }
        out.duration_seconds = duration;
        out.available = !out.tracks.empty();
        out.source = QStringLiteral("流媒体清单分片（HLS / DASH）");
        out.partially_probed = !streaming.SegmentProbeComplete();
        out.sampled = out.partially_probed || streaming.truncated;
        QStringList notes;
        notes << QStringLiteral("按清单声明的分片起点与时长分桶，不是逐包实测");
        if (out.partially_probed)
            notes << QStringLiteral("分片抽查 %1 / %2，未探测的分片没有字节数")
                         .arg(streaming.ProbedSegments())
                         .arg(streaming.TotalSegments());
        if (streaming.truncated)
            notes << QStringLiteral("清单解析已达上限被截断");
        bool any_bytes = false;
        for (const PacketBandTrack& track : out.tracks)
            any_bytes = any_bytes || track.has_bytes;
        if (!any_bytes)
            notes << QStringLiteral("分片未落盘，柱高改按分片数计（不是字节量）");
        out.coverage_note = notes.join(QStringLiteral("；"));
        out.axis = MapBandAxis::TimeSeconds;
        out.axis_label = QStringLiteral("时间（秒）");
        out.axis_max = out.duration_seconds;
        return out;
    }

    // ---- EBML 块摘要：只能按「文件字节偏移」分桶 ----
    // EbmlBlockSummary::timecode 是相对 Cluster 的 int16，而模型里没有每个 Cluster
    // 的绝对 Timecode，硬换算成秒等于编时间轴。所以横轴改用实测的 block_offset，
    // 并把「这不是时间轴」写进 source / coverage_note / axis_label 三处。
    const model::EbmlAnalysisResult& ebml = result.ebml_detail;
    if (ebml.valid && !ebml.blocks.empty()) {
        quint64 max_end = 0;
        for (const model::EbmlBlockSummary& block : ebml.blocks) {
            const quint64 end = block.block_offset + static_cast<quint64>(std::max(0, block.data_size));
            max_end = std::max(max_end, end);
        }
        const quint64 span = ebml.file_bytes > 0 ? static_cast<quint64>(ebml.file_bytes) : max_end;
        if (span > 0) {
            std::map<int, size_t> by_track;
            for (const model::EbmlBlockSummary& block : ebml.blocks) {
                size_t slot = 0;
                auto it = by_track.find(block.track_number);
                if (it == by_track.end()) {
                    if (out.tracks.size() >= 8)
                        continue; // 卡片预算：多余轨道合并丢弃，并在 note 里说明
                    PacketBandTrack band;
                    band.stream_index = static_cast<int>(out.tracks.size());
                    band.label = QStringLiteral("轨道 %1").arg(block.track_number);
                    band.duration_seconds = static_cast<double>(span);
                    band.has_bytes = true;
                    band.bins.resize(static_cast<size_t>(bins));
                    for (int i = 0; i < bins; ++i) {
                        band.bins[i].t0 = static_cast<double>(span) * i / bins;
                        band.bins[i].t1 = static_cast<double>(span) * (i + 1) / bins;
                    }
                    slot = out.tracks.size();
                    by_track[block.track_number] = slot;
                    out.tracks.push_back(std::move(band));
                } else {
                    slot = it->second;
                }
                PacketBandTrack& band = out.tracks[slot];
                const int index = std::max(
                    0, std::min(bins - 1, static_cast<int>(static_cast<double>(block.block_offset) / span * bins)));
                PacketBandBin& bin = band.bins[static_cast<size_t>(index)];
                const quint64 bytes = static_cast<quint64>(std::max(0, block.data_size));
                bin.bytes += bytes;
                ++bin.count;
                if (block.keyframe)
                    ++bin.keyframes;
                band.total_bytes += bytes;
                ++band.total_samples;
            }
            out.axis = MapBandAxis::ByteOffset;
            out.axis_label = QStringLiteral("文件字节偏移");
            out.axis_max = static_cast<double>(span);
            out.available = !out.tracks.empty() && out.TotalCount() > 0;
            out.source = QStringLiteral("EBML 块摘要（按文件字节偏移，不是时间轴）");
            out.sampled = ebml.blocks.size() >= 1000 || ebml.block_nodes_truncated || ebml.scan_truncated;
            out.partially_probed = ebml.scan_truncated;
            QStringList notes;
            notes << QStringLiteral("横轴是 block_offset 的字节区间，不是时间：块内 timecode 是相对 Cluster 的 int16，"
                                    "没有 Cluster 绝对时间码可换算");
            notes << QStringLiteral("块摘要最多 1000 条，密度是抽样结果，不代表全片逐块分布");
            if (ebml.block_nodes_truncated)
                notes << QStringLiteral("块节点另按上限未挂进结构树（计数仍精确）");
            if (ebml.scan_truncated)
                notes << QStringLiteral("元素树只扫了开头窗口，窗口外的块没有摘要");
            if (by_track.size() >= 8)
                notes << QStringLiteral("轨道数超过 8 条，图中只显示前 8 条");
            out.coverage_note = notes.join(QStringLiteral("；"));
            return out;
        }
    }

    const FormatProfile profile = BuildFormatProfile(result);
    out.source = QStringLiteral("本格式没有可用的包级时间轴数据");
    out.coverage_note = QStringLiteral("%1；逐包数据在 demux 时已释放，也没有样本表 / 分片清单 / EBML 块摘要可聚合；"
                                       "不生成推测密度带")
                            .arg(profile.structure_note);
    return out;
}

std::vector<MapVisual> BuildPacketVisuals(const PacketBandData& bands, QRect area) {
    if (!bands.available || bands.tracks.empty() || area.width() < 20 || area.height() < 20)
        return {};
    const size_t shown = std::min<size_t>(bands.tracks.size(), 8);
    const int row_h = std::max(20, area.height() / static_cast<int>(shown));
    std::vector<MapVisual> out;
    for (size_t t = 0; t < shown; ++t) {
        const PacketBandTrack& track = bands.tracks[t];
        if (track.bins.empty())
            continue;
        const QRect row(area.left(), area.top() + static_cast<int>(t) * row_h, area.width(), std::max(16, row_h - 6));
        double peak = 0.0;
        for (const PacketBandBin& bin : track.bins) {
            const double value = track.has_bytes ? static_cast<double>(bin.bytes) : static_cast<double>(bin.count);
            peak = std::max(peak, value);
        }
        if (peak <= 0.0)
            peak = 1.0;
        const int bin_w = std::max(2, row.width() / static_cast<int>(track.bins.size()));
        for (size_t b = 0; b < track.bins.size(); ++b) {
            const PacketBandBin& bin = track.bins[b];
            const double value = track.has_bytes ? static_cast<double>(bin.bytes) : static_cast<double>(bin.count);
            MapVisual visual;
            visual.track_index = static_cast<int>(t);
            visual.bin_index = static_cast<int>(b);
            visual.rect =
                QRect(row.left() + static_cast<int>(b) * bin_w, row.top(), std::max(1, bin_w - 1), row.height());
            visual.bar_height = std::max(1, static_cast<int>(std::lround(row.height() * value / peak)));
            if (bin.anomalies > 0)
                visual.fill = QColor(QStringLiteral("#5A1E1E"));
            else if (bin.keyframes > 0)
                visual.fill = QColor(QStringLiteral("#1D4A6B"));
            else
                visual.fill = QColor(QStringLiteral("#12263A"));
            visual.border = QColor(theme::color::kBg);
            if (bin_w > 34 && bin.count > 0)
                visual.label = QString::number(bin.count);
            out.push_back(visual);
        }
    }
    return out;
}

QString DescribeBandBin(const PacketBandData& bands, int track_index, int bin_index) {
    if (track_index < 0 || track_index >= static_cast<int>(bands.tracks.size()))
        return {};
    const PacketBandTrack& track = bands.tracks[static_cast<size_t>(track_index)];
    if (bin_index < 0 || bin_index >= static_cast<int>(track.bins.size()))
        return {};
    const PacketBandBin& bin = track.bins[static_cast<size_t>(bin_index)];
    const bool by_bytes = bands.axis == MapBandAxis::ByteOffset;
    const QString title = by_bytes ? QStringLiteral("字节区间详情") : QStringLiteral("时间桶详情");
    const QString axis_name = by_bytes ? QStringLiteral("字节区间") : QStringLiteral("时间窗");
    const QString window = by_bytes
                               ? QStringLiteral("%1 – %2").arg(FormatBytes(static_cast<quint64>(std::max(0.0, bin.t0))),
                                                               FormatBytes(static_cast<quint64>(std::max(0.0, bin.t1))))
                               : QStringLiteral("%1 – %2 s").arg(bin.t0, 0, 'f', 2).arg(bin.t1, 0, 'f', 2);
    const QString record_name =
        by_bytes ? QStringLiteral("EBML 块")
                 : (bands.source.contains(QStringLiteral("分片")) ? QStringLiteral("分片") : QStringLiteral("样本"));
    return QStringLiteral("<div style='color:%1'><b style='font-size:13px'>%2</b><br>"
                          "<span style='color:%3'>%4</span><br><br>"
                          "<table cellspacing='2'>"
                          "<tr><td style='color:%3'>%5</td><td>%6</td></tr>"
                          "<tr><td style='color:%3'>%7</td><td>%8</td></tr>"
                          "<tr><td style='color:%3'>字节</td><td>%9 (%10 B)</td></tr>"
                          "<tr><td style='color:%3'>关键帧</td><td>%11</td></tr>"
                          "<tr><td style='color:%3'>异常</td><td>%12</td></tr>"
                          "</table><div style='color:%3;margin-top:8px'>口径：%13</div>"
                          "<div style='color:%3'>%14</div></div>")
        .arg(QString::fromLatin1(theme::color::kTextPrimary), title, QString::fromLatin1(theme::color::kTextSecondary),
             HtmlEscape(track.label), axis_name, window, record_name)
        .arg(bin.count)
        .arg(FormatBytes(bin.bytes))
        .arg(bin.bytes)
        .arg(bin.keyframes)
        .arg(by_bytes ? QStringLiteral("不适用（块摘要没有异常字段）") : QString::number(bin.anomalies),
             HtmlEscape(track.has_bytes ? QStringLiteral("按字节数") : QStringLiteral("按条目数（字节数不可得）")),
             HtmlEscape(bands.axis_label));
}

// ---------------------------------------------------------------------------
// 流的真实字节分布：只认 Mp4Sample::offset，绝不按画布宽度编区间
// ---------------------------------------------------------------------------

QColor StreamColor(const std::string& type) {
    if (type == "video")
        return QColor(QStringLiteral("#1F6FEB"));
    if (type == "audio")
        return QColor(QStringLiteral("#238636"));
    if (type == "subtitle")
        return QColor(QStringLiteral("#8957E5"));
    return QColor(QStringLiteral("#484F58"));
}

StreamByteRows BuildStreamByteRows(const model::ContainerStructureResult& result, int max_bins,
                                   const MapLayoutProjection& projection) {
    const int bins = std::max(8, std::min(256, max_bins));
    StreamByteRows out;
    out.bins = bins;
    out.file_span = FileSpan(result);
    if (!result.valid) {
        out.source = QStringLiteral("未分析");
        out.coverage_note = QStringLiteral("结构分析未成功，不绘制流区间");
        return out;
    }
    const model::Mp4SampleTableResult& samples = result.mp4_samples;
    if (!samples.valid || samples.TotalSamples() == 0 || out.file_span == 0) {
        out.source = QStringLiteral("没有逐样本文件偏移");
        out.coverage_note =
            QStringLiteral("流是逻辑轨道，本身不带 offset/size；样本实际交错存放于 mdat，此处不代表字节区间，故不绘制");
        return out;
    }

    bool any_truncated = false;
    bool have_extent = false;
    for (const model::Mp4TrackSampleTable& track : samples.tracks) {
        if (track.samples.empty())
            continue;
        StreamByteRow row;
        row.stream_index = static_cast<int>(out.rows.size());
        row.type = track.type;
        row.label = QStringLiteral("#%1 %2 %3")
                        .arg(row.stream_index)
                        .arg(QString::fromStdString(track.type), QString::fromStdString(track.codec));
        row.truncated = track.samples_truncated;
        any_truncated = any_truncated || track.samples_truncated;
        row.declared_samples = track.stsz_sample_count;
        row.expanded_samples = track.samples.size();
        row.unexpanded_samples =
            row.declared_samples > row.expanded_samples ? row.declared_samples - row.expanded_samples : 0;
        row.bins.resize(static_cast<size_t>(bins));
        for (const model::Mp4Sample& sample : track.samples) {
            if (sample.offset >= out.file_span)
                ++out.out_of_range_samples;
            // 先投影到布局坐标再归桶：均衡布局下按字节等分会让单个桶跨过 box 边界，
            // 在屏幕上被拉成一条从 ftyp 到 mdat 的长条（看起来又像数据铺满头部）。
            const double raw_x = static_cast<double>(sample.offset) / static_cast<double>(out.file_span);
            const double layout_x = projection.Project(raw_x);
            const int index = std::max(0, std::min(bins - 1, static_cast<int>(layout_x * bins)));
            StreamByteBin& bin = row.bins[static_cast<size_t>(index)];
            bin.bytes += sample.size;
            ++bin.count;
            if (sample.keyframe)
                ++bin.keyframes;
            const quint64 end = sample.offset + sample.size;
            row.last_expanded_end = std::max(row.last_expanded_end, end);
            bin.min_offset = bin.count == 1 ? sample.offset : std::min(bin.min_offset, sample.offset);
            bin.max_end = bin.count == 1 ? end : std::max(bin.max_end, end);
            row.total_bytes += sample.size;
            ++row.total_count;
            out.min_offset = have_extent ? std::min(out.min_offset, sample.offset) : sample.offset;
            out.max_offset = have_extent ? std::max(out.max_offset, end) : end;
            have_extent = true;
        }
        // 未展开区起点必须与上方结构盒、桶共用同一条布局投影链，否则斜纹会落错位置。
        if (out.file_span > 0 && row.last_expanded_end > 0) {
            const double raw = static_cast<double>(row.last_expanded_end) / static_cast<double>(out.file_span);
            row.expanded_x1 = projection.Project(raw);
        }
        out.rows.push_back(std::move(row));
    }
    if (out.rows.empty()) {
        out.source = QStringLiteral("没有逐样本文件偏移");
        out.coverage_note = QStringLiteral("样本表没有展开的样本，不绘制流区间");
        return out;
    }
    out.available = true;
    out.any_truncated = any_truncated;
    out.source = QStringLiteral("MP4 样本表（stco / co64 + stsz）");
    QStringList notes;
    notes << QStringLiteral(
        "横轴与上方结构盒共用布局坐标（按当前布局投影），是字节位置而非时间轴；样本交错存放于 mdat，两轨并不各占一段");
    if (any_truncated) {
        QStringList per_track;
        for (const StreamByteRow& row : out.rows) {
            if (row.truncated)
                per_track << QStringLiteral("#%1 %2/%3")
                                 .arg(row.stream_index)
                                 .arg(row.expanded_samples)
                                 .arg(row.declared_samples);
        }
        out.truncation_note = per_track.join(QStringLiteral("、"));
        // ❗ 别把「只展开前 N 个」说成「已截断，密度为抽样结果」就完事：
        // 音频包更密时同样 N 个样本只覆盖半份文件，图上右侧空白会被读成「没有音频」。
        notes << QStringLiteral("样本表按每轨上限截断（展开 %1）；右侧斜纹是未展开的样本，不代表没有数据")
                     .arg(out.truncation_note);
    }
    if (out.out_of_range_samples > 0)
        notes << QStringLiteral("%1 个样本偏移越过文件跨度，已归入末端桶").arg(out.out_of_range_samples);
    out.coverage_note = notes.join(QStringLiteral("；"));
    return out;
}

double MapLayoutProjection::Project(double nx) const {
    if (segments.empty())
        return Clamp01(nx);
    if (nx <= segments.front().raw0)
        return Clamp01(segments.front().layout0);
    for (size_t i = 0; i < segments.size(); ++i) {
        const Segment& s = segments[i];
        if (nx <= s.raw1) {
            const double t = s.raw1 > s.raw0 ? (nx - s.raw0) / (s.raw1 - s.raw0) : 0.0;
            return Clamp01(s.layout0 + t * (s.layout1 - s.layout0));
        }
        // 顶层 box 之间有空隙 / 畸形偏移：在两段布局端点之间线性过渡，不留跳变。
        if (i + 1 < segments.size()) {
            const Segment& next = segments[i + 1];
            if (nx < next.raw0 && next.raw0 > s.raw1) {
                const double t = (nx - s.raw1) / (next.raw0 - s.raw1);
                return Clamp01(s.layout1 + t * (next.layout0 - s.layout1));
            }
        }
    }
    return Clamp01(segments.back().layout1);
}

MapLayoutProjection BuildLayoutProjection(const std::vector<MapNode>& nodes) {
    MapLayoutProjection out;
    double min_offset = 1e300;
    double max_end = 0.0;
    for (const MapNode& node : nodes) {
        if (node.depth != 0)
            continue;
        min_offset = std::min(min_offset, static_cast<double>(node.offset));
        max_end = std::max(max_end, static_cast<double>(node.offset) + static_cast<double>(node.size));
    }
    const double span = max_end - min_offset;
    if (!(span > 0.0))
        return out;
    for (const MapNode& node : nodes) {
        if (node.depth != 0 || node.size == 0)
            continue;
        MapLayoutProjection::Segment s;
        s.raw0 = Clamp01((static_cast<double>(node.offset) - min_offset) / span);
        s.raw1 = Clamp01((static_cast<double>(node.offset) + static_cast<double>(node.size) - min_offset) / span);
        s.layout0 = Clamp01(node.x0);
        s.layout1 = Clamp01(node.x1);
        if (s.raw1 > s.raw0 && s.layout1 > s.layout0)
            out.segments.push_back(s);
    }
    std::sort(out.segments.begin(), out.segments.end(),
              [](const MapLayoutProjection::Segment& a, const MapLayoutProjection::Segment& b) {
                  return a.raw0 < b.raw0 || (a.raw0 == b.raw0 && a.layout0 < b.layout0);
              });
    return out;
}

std::vector<MapVisual> BuildStreamByteVisuals(const StreamByteRows& rows, QRect band_area, const MapViewport& view,
                                              int row_height) {
    std::vector<MapVisual> out;
    if (!rows.available || rows.bins <= 0 || band_area.width() < 2 || row_height < 4)
        return out;
    const int capacity = std::max(1, band_area.height() / std::max(1, row_height));
    const int shown = std::min<int>(kStreamMaxRows, std::min<int>(capacity, static_cast<int>(rows.rows.size())));
    if (shown <= 0)
        return out;

    // 所有行共用一个峰值：否则小轨被单独归一化后看起来和主轨一样满，等于谎报密度。
    quint64 peak = 0;
    for (const StreamByteRow& row : rows.rows) {
        for (const StreamByteBin& bin : row.bins) {
            if (bin.count > 0)
                peak = std::max(peak, bin.bytes > 0 ? bin.bytes : bin.count);
        }
    }
    for (int r = 0; r < shown; ++r) {
        const StreamByteRow& row = rows.rows[static_cast<size_t>(r)];
        // ❗ 截断轨右侧是「没有展开」而不是「没有数据」。不显式标出来，包更密的
        // 音频轨（同样 20000 个样本只覆盖半份文件）就会被读成只覆盖前半段。
        if (row.truncated && row.unexpanded_samples > 0 && row.expanded_x1 < 1.0) {
            const double sx0 = view.ToScreen(row.expanded_x1) * band_area.width();
            const double sx1 = view.ToScreen(1.0) * band_area.width();
            if (sx1 >= 0 && sx0 <= band_area.width()) {
                const int x0 = band_area.left() + std::max(0, static_cast<int>(std::lround(sx0)));
                const int x1 = band_area.left() + std::min(band_area.width(), static_cast<int>(std::lround(sx1)));
                if (x1 > x0) {
                    MapVisual unexpanded;
                    unexpanded.stream_row = r;
                    unexpanded.decorative = true; // 与扫描窗口叠加同口径：只画，不抢命中
                    unexpanded.hatched = true;
                    unexpanded.rect = QRect(x0, band_area.top() + r * row_height, x1 - x0, row_height - 1);
                    unexpanded.fill = QColor(QStringLiteral("#30363D"));
                    unexpanded.border = QColor(QStringLiteral("#484F58"));
                    unexpanded.label = QStringLiteral("未展开 %1 样本").arg(row.unexpanded_samples);
                    out.push_back(std::move(unexpanded));
                }
            }
        }
        const QColor color = StreamColor(row.type);
        const int y = band_area.top() + r * row_height;
        for (int b = 0; b < rows.bins; ++b) {
            if (b >= static_cast<int>(row.bins.size()))
                break;
            const StreamByteBin& bin = row.bins[static_cast<size_t>(b)];
            // 空桶不建矩形：一条贯穿全宽的底色正是「流占满整行」这个误读的来源。
            if (bin.count == 0)
                continue;
            // 桶已经在布局坐标上分好（见 BuildStreamByteRows），这里只做视窗变换。
            const double sx0 = view.ToScreen(static_cast<double>(b) / rows.bins) * band_area.width();
            const double sx1 = view.ToScreen(static_cast<double>(b + 1) / rows.bins) * band_area.width();
            if (sx1 < 0 || sx0 > band_area.width())
                continue; // 可视裁剪：缩放后屏幕外的桶不建矩形
            const int x0 = band_area.left() + std::max(0, static_cast<int>(std::lround(sx0)));
            const int x1 = band_area.left() + std::min(band_area.width(), static_cast<int>(std::lround(sx1)));
            if (x1 <= x0)
                continue;
            const quint64 weight = bin.bytes > 0 ? bin.bytes : bin.count;
            MapVisual visual;
            visual.stream_row = r;
            visual.stream_bin = b;
            visual.rect = QRect(x0, y, x1 - x0, row_height - 1);
            visual.fill = color;
            visual.border = QColor(QStringLiteral("#0D1117"));
            visual.bar_height = peak > 0 ? std::max(2, static_cast<int>((row_height - 1) * weight / peak)) : 0;
            out.push_back(std::move(visual));
        }
    }
    return out;
}

QString DescribeStreamByteBin(const StreamByteRows& rows, int row_index, int bin_index) {
    if (!rows.available || rows.bins <= 0 || row_index < 0 || row_index >= static_cast<int>(rows.rows.size()) ||
        bin_index < 0 || bin_index >= rows.bins)
        return {};
    const StreamByteRow& row = rows.rows[static_cast<size_t>(row_index)];
    if (bin_index >= static_cast<int>(row.bins.size()))
        return {};
    const StreamByteBin& bin = row.bins[static_cast<size_t>(bin_index)];
    // 桶是布局坐标上的分段，不能由「桶宽 × file_span」反推字节区间 ——
    // 均衡布局下那是假的。这里只报桶内样本的实测最小偏移与最大末端。
    return QStringLiteral("<div style='color:%1'><b style='font-size:13px'>样本字节区间</b><br>"
                          "<span style='color:%2'>%3</span><br><br>"
                          "<table cellspacing='2'>"
                          "<tr><td style='color:%2'>实测字节范围</td><td>%4 – %5</td></tr>"
                          "<tr><td style='color:%2'>样本数</td><td>%6</td></tr>"
                          "<tr><td style='color:%2'>字节</td><td>%7 (%8 B)</td></tr>"
                          "<tr><td style='color:%2'>关键帧</td><td>%9</td></tr>"
                          "</table><div style='color:%2;margin-top:8px'>口径：%10</div>"
                          "<div style='color:%2'>%11</div></div>")
        .arg(QString::fromLatin1(theme::color::kTextPrimary), QString::fromLatin1(theme::color::kTextSecondary),
             HtmlEscape(row.label), FormatBytes(bin.min_offset), FormatBytes(bin.max_end))
        .arg(bin.count)
        .arg(FormatBytes(bin.bytes))
        .arg(bin.bytes)
        .arg(bin.keyframes)
        .arg(HtmlEscape(row.truncated ? QStringLiteral("样本表已截断，抽样结果")
                                      : QStringLiteral("stco/co64 + stsz 实测偏移")),
             HtmlEscape(rows.coverage_note));
}

// ---------------------------------------------------------------------------
// 完整性：五个维度分开落，绝不合成一个"完成"
// ---------------------------------------------------------------------------

MapCoverage BuildCoverage(const model::ContainerStructureResult& result, bool nodes_truncated, bool depth_truncated) {
    MapCoverage coverage;
    coverage.analyzed = result.valid;
    if (!result.valid) {
        coverage.error_reason = result.error_message.empty() ? QStringLiteral("无法分析该格式")
                                                             : QString::fromStdString(result.error_message);
        coverage.facts << QStringLiteral("分析未成功");
        coverage.facts << QStringLiteral("错误：%1").arg(coverage.error_reason);
        return coverage;
    }
    coverage.facts << QStringLiteral("结构解析成功");

    using CF = model::ContainerFormat;
    if (result.format == CF::HLS || result.format == CF::DASH)
        coverage.facts << QStringLiteral("清单节点无 size / offset，宽度按等分表示");

    const model::ContainerDiagnosticsInfo& diagnostics = result.diagnostics;
    QStringList window_sources;
    if (diagnostics.avi.analyzed && !diagnostics.avi.scan_complete)
        window_sources << QStringLiteral("AVI");
    if (diagnostics.flv.analyzed && !diagnostics.flv.scan_complete)
        window_sources << QStringLiteral("FLV");
    if (diagnostics.asf.analyzed && !diagnostics.asf.scan_complete)
        window_sources << QStringLiteral("ASF");
    if (diagnostics.ogg.analyzed && !diagnostics.ogg.scan_complete)
        window_sources << QStringLiteral("OGG");
    if (diagnostics.mkv.analyzed && (!diagnostics.mkv.scan_complete || diagnostics.mkv.truncated))
        window_sources << QStringLiteral("MKV");
    if (diagnostics.mxf.analyzed && !diagnostics.mxf.scan_complete)
        window_sources << QStringLiteral("MXF");
    if (diagnostics.mpeg_ps.analyzed && !diagnostics.mpeg_ps.scan_complete)
        window_sources << QStringLiteral("MPEG-PS");
    if (result.ebml_detail.valid && result.ebml_detail.scan_truncated) {
        const model::EbmlAnalysisResult& ebml = result.ebml_detail;
        if (ebml.file_bytes > 0 && ebml.scanned_bytes > 0)
            window_sources << QStringLiteral("EBML 结构树（前 %1 / 共 %2）")
                                  .arg(FormatBytes(static_cast<quint64>(ebml.scanned_bytes)),
                                       FormatBytes(static_cast<quint64>(ebml.file_bytes)));
        else
            window_sources << QStringLiteral("EBML 结构树");
    }
    if (!window_sources.isEmpty()) {
        coverage.window_limited = true;
        coverage.facts << QStringLiteral("结构扫描仅覆盖窗口（%1），片尾元素可能不在树里")
                              .arg(window_sources.join(QStringLiteral(" / ")));
    }

    int truncated_tracks = 0;
    for (const model::Mp4TrackSampleTable& track : result.mp4_samples.tracks) {
        if (track.samples_truncated)
            ++truncated_tracks;
    }
    if (truncated_tracks > 0) {
        coverage.sampled = true;
        coverage.facts << QStringLiteral("样本表已截断（%1 轨），包带为抽样结果").arg(truncated_tracks);
    }
    if (result.streaming_package.valid) {
        if (!result.streaming_package.SegmentProbeComplete()) {
            coverage.sampled = true;
            coverage.facts << QStringLiteral("分片抽查 %1 / %2")
                                  .arg(result.streaming_package.ProbedSegments())
                                  .arg(result.streaming_package.TotalSegments());
        }
        if (result.streaming_package.truncated) {
            coverage.sampled = true;
            coverage.facts << QStringLiteral("清单解析已达上限被截断");
        }
    }
    if (result.ebml_detail.valid && result.ebml_detail.block_nodes_truncated)
        coverage.facts << QStringLiteral("EBML 块节点按上限未挂进树（总计数仍精确）");

    if (nodes_truncated) {
        coverage.truncated = true;
        coverage.facts << QStringLiteral("图上节点已按绘制预算裁剪");
    }
    if (depth_truncated) {
        coverage.truncated = true;
        coverage.facts << QStringLiteral("结构深度超过 %1 层已裁剪").arg(kMaxTreeDepth);
    }
    // 切片 4：格式画像里的口径事实（精确扫描窗口 / 全文件体检与结构树不同口径 /
    // TS 采样汇总 / 裸流回退）也要落进完整性条，不能只画在概览卡片上。
    const FormatProfile profile = BuildFormatProfile(result);
    for (const QString& note : FormatCoverageFacts(profile)) {
        if (!coverage.facts.contains(note))
            coverage.facts << note;
    }
    if (profile.window_limited)
        coverage.window_limited = true;
    coverage.facts << QStringLiteral("视图口径：结构骨架（载荷 / 逐包不展开）");
    // 只断言"本视图声称的结构骨架"完整，不覆盖载荷与逐包结论。
    coverage.conclusion_complete = coverage.analyzed && !coverage.window_limited && !coverage.sampled &&
                                   !coverage.truncated && coverage.error_reason.isEmpty();
    return coverage;
}

QByteArray BuildStructureJson(const model::ContainerStructureResult& result, const MapCoverage& coverage) {
    // 与 CSV / SVG 同一反向守护：没有事实就不产文件。
    if (!result.valid)
        return {};
    const std::vector<MapNode> nodes = CollectNodes(result, kMaxTreeDepth, 200000);
    if (nodes.empty())
        return {};

    QJsonObject root;
    root[QStringLiteral("schema")] = QStringLiteral("videoeye.structure-map/1");
    QJsonObject source;
    source[QStringLiteral("path")] = QString::fromStdString(result.file_path);
    source[QStringLiteral("format")] = QString::fromStdString(result.format_name);
    source[QStringLiteral("valid")] = result.valid;
    root[QStringLiteral("source")] = source;

    QJsonObject cov;
    cov[QStringLiteral("analyzed")] = coverage.analyzed;
    cov[QStringLiteral("structure_complete")] = coverage.conclusion_complete;
    cov[QStringLiteral("window_limited")] = coverage.window_limited;
    cov[QStringLiteral("sampled")] = coverage.sampled;
    cov[QStringLiteral("truncated")] = coverage.truncated;
    cov[QStringLiteral("error_reason")] = coverage.error_reason;
    cov[QStringLiteral("facts")] = QJsonArray::fromStringList(coverage.facts);
    root[QStringLiteral("coverage")] = cov;
    // 逐格式事实与完整性同源：导出里能看到「这棵树是哪个口径、窗口到哪」。
    root[QStringLiteral("format_profile")] = FormatProfileJson(BuildFormatProfile(result));
    root[QStringLiteral("file_span")] = JsonUInt(FileSpan(result));
    root[QStringLiteral("element_count")] = static_cast<int>(nodes.size());

    QJsonArray streams;
    for (const model::ContainerStreamInfo& stream : result.streams) {
        QJsonObject item;
        item[QStringLiteral("index")] = stream.index;
        item[QStringLiteral("type")] = QString::fromStdString(stream.type);
        item[QStringLiteral("codec")] = QString::fromStdString(stream.codec);
        item[QStringLiteral("details")] = QString::fromStdString(stream.details);
        streams.append(item);
    }
    root[QStringLiteral("streams")] = streams;

    // 样本表按每轨上限只展开前 N 个：导出必须和图上一样说清「右侧是未展开」，
    // 否则下游只看到流信息，会把截断后的密度当成全文件。
    QJsonObject sample_table;
    sample_table[QStringLiteral("source")] = QStringLiteral("stco/co64 + stsz");
    sample_table[QStringLiteral("valid")] = result.mp4_samples.valid;
    QJsonArray sample_tracks;
    for (const model::Mp4TrackSampleTable& track : result.mp4_samples.tracks) {
        QJsonObject item;
        item[QStringLiteral("track_id")] = static_cast<int>(track.track_id);
        item[QStringLiteral("type")] = QString::fromStdString(track.type);
        item[QStringLiteral("declared_samples")] = JsonUInt(track.stsz_sample_count);
        item[QStringLiteral("expanded_samples")] = JsonUInt(track.samples.size());
        item[QStringLiteral("unexpanded_samples")] = JsonUInt(
            track.stsz_sample_count > track.samples.size() ? track.stsz_sample_count - track.samples.size() : 0);
        item[QStringLiteral("truncated")] = track.samples_truncated;
        sample_tracks.append(item);
    }
    sample_table[QStringLiteral("tracks")] = sample_tracks;
    root[QStringLiteral("sample_table")] = sample_table;

    QJsonObject metadata;
    for (const auto& entry : result.metadata)
        metadata[QString::fromStdString(entry.first)] = QString::fromStdString(entry.second);
    root[QStringLiteral("metadata")] = metadata;

    QJsonArray elements;
    for (const MapNode& node : nodes) {
        QJsonObject item;
        item[QStringLiteral("name")] = node.name;
        item[QStringLiteral("type")] = node.type;
        item[QStringLiteral("offset")] = JsonUInt(node.offset);
        item[QStringLiteral("size")] = JsonUInt(node.size);
        item[QStringLiteral("depth")] = node.depth;
        item[QStringLiteral("value")] = node.value;
        item[QStringLiteral("parent")] = node.parent;
        item[QStringLiteral("x0")] = node.x0;
        item[QStringLiteral("x1")] = node.x1;
        elements.append(item);
    }
    root[QStringLiteral("elements")] = elements;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

} // namespace structure_map_detail
} // namespace ui
} // namespace videoeye
