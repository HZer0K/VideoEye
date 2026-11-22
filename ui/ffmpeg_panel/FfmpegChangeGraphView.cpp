#include "ui/ffmpeg_panel/FfmpegChangeGraphView.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPolygon>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>

#include "core/ffmpeg/FfmpegMediaProbe.h"

namespace videoeye {
namespace ui {
namespace change_graph_view {
namespace {

// CJK 按两个字符宽计：等宽估算比 QFontMetrics 更适合做纯函数（无字体依赖），
// 也够用 —— 它只决定"截断到几个字"，不决定排版精度。
int TextUnits(const QString& text) {
    int units = 0;
    for (const QChar c : text) {
        units += (c.unicode() < 0x2E80) ? 1 : 2;
    }
    return units;
}

int TextWidthPx(const QString& text, int font_px) {
    // 11px 字号下，ASCII 约 6px/字符、CJK 约 12px/字符。
    return static_cast<int>(std::lround(TextUnits(text) * font_px * 0.55));
}

// 状态标签（"已变"/"未变"…）用 10px 字号画在徽标右侧、内缩 8px。
// 算宽时不给它留位置，长文本就会顶到它身上 —— 第一版渲染出来就是
// "时长 1:23.4 → 1:23.4" 和 "未变" 叠成一团。
int StateLabelWidth(const QString& text) {
    return text.isEmpty() ? 0 : TextWidthPx(text, 10);
}

struct Cols {
    int x0 = 0;
    int x1 = 0;
    int x2 = 0;
    int x3 = 0;
    int right = 0;
};

Cols ComputeCols(int width) {
    Cols cols;
    cols.x0 = kOuterPad;
    const int right = std::max(width - kOuterPad, kOuterPad + 40);
    const int avail = std::max(right - cols.x0 - kColumnGap * 3, 4);
    const int w0 = static_cast<int>(avail * 0.26);
    const int w1 = static_cast<int>(avail * 0.20);
    const int w2 = static_cast<int>(avail * 0.20);
    cols.x1 = cols.x0 + w0 + kColumnGap;
    cols.x2 = cols.x1 + w1 + kColumnGap;
    cols.x3 = cols.x2 + w2 + kColumnGap;
    cols.right = right;
    return cols;
}

int BadgeLines(const ffmpeg::ChangeGraph& graph, int row) {
    if (row < 0 || row >= static_cast<int>(graph.rows.size())) {
        return 0;
    }
    const int count = static_cast<int>(graph.rows.at(static_cast<size_t>(row)).changes.size());
    return std::min(count, kMaxBadgesPerRow);
}

int RowHeight(const ffmpeg::ChangeGraph& graph, int row) {
    const int badges = BadgeLines(graph, row);
    if (badges == 0) {
        return kNodeHeight;
    }
    return kNodeHeight + kBadgeGap + badges * kBadgeHeight + (badges - 1) * kBadgeGap;
}

QString KindPrefix(ffmpeg::MediaStreamKind kind) {
    return ffmpeg::MediaStreamKindName(kind);
}

QString StateBadgeColor(ffmpeg::ChangeState state) {
    return ffmpeg::ChangeStateColor(state);
}

} // namespace

ViewPalette DarkPalette() {
    ViewPalette palette;
    palette.background = QColor(QStringLiteral("#17181b"));
    palette.node_fill = QColor(QStringLiteral("#232529"));
    palette.process_fill = QColor(QStringLiteral("#20262b"));
    palette.border = QColor(QStringLiteral("#3a3d44"));
    palette.title = QColor(QStringLiteral("#e6e8ec"));
    palette.subtitle = QColor(QStringLiteral("#8b919a"));
    palette.header = QColor(QStringLiteral("#6f757e"));
    palette.grid = QColor(QStringLiteral("#2a2d32"));
    return palette;
}

QString ElideToWidth(const QString& text, int pixels) {
    if (pixels <= 0) {
        return QString();
    }
    if (TextWidthPx(text, 11) <= pixels) {
        return text;
    }
    const int max_units = static_cast<int>(pixels / (11 * 0.55));
    if (max_units <= 1) {
        return QStringLiteral("…");
    }
    int units = 0;
    QString out;
    for (const QChar c : text) {
        const int unit = (c.unicode() < 0x2E80) ? 1 : 2;
        if (units + unit > max_units - 1) {
            break;
        }
        out.append(c);
        units += unit;
    }
    return out + QStringLiteral("…");
}

int BadgeMinWidth(const QString& text, const QString& state) {
    // 左内缩 8 + 正文 + （间隙 6 + 状态词）+ 右内缩 8
    return 8 + TextWidthPx(text, 11) + (state.isEmpty() ? 0 : 6 + StateLabelWidth(state)) + 8;
}

int VisibleBadgeCount(const ffmpeg::ChangeGraph& graph, int row) {
    return BadgeLines(graph, row);
}

int PreferredHeight(const ffmpeg::ChangeGraph& graph, int width) {
    if (!graph.ok) {
        return kHeaderHeight + kOuterPad * 2;
    }
    int height = kOuterPad + kHeaderHeight;
    for (int i = 0; i < static_cast<int>(graph.rows.size()); ++i) {
        height += RowHeight(graph, i) + kRowGap;
    }
    if (!graph.container_changes.empty()) {
        height += kContainerStripHeight + kRowGap;
    }
    (void)width;
    return height + kOuterPad;
}

std::vector<Visual> BuildVisuals(const ffmpeg::ChangeGraph& graph, int width, const ViewPalette& palette) {
    std::vector<Visual> visuals;
    if (!graph.ok) {
        return visuals;
    }
    const Cols cols = ComputeCols(width);
    const int node_width_input = cols.x1 - kColumnGap - cols.x0;
    const int node_width_filter = cols.x2 - kColumnGap - cols.x1;
    const int node_width_encoder = cols.x3 - kColumnGap - cols.x2;
    const int node_width_output = cols.right - cols.x3;

    // ---- 表头 ----
    struct HeadItem {
        int x;
        int w;
        const char* text;
    };
    const HeadItem heads[] = {
        {cols.x0, node_width_input, "输入流（实测）"},
        {cols.x1, node_width_filter, "滤镜（声明）"},
        {cols.x2, node_width_encoder, "编码（声明）"},
        {cols.x3, node_width_output, "输出流（实测）"},
    };
    for (const HeadItem& head : heads) {
        Visual visual;
        visual.kind = VisualKind::Header;
        visual.rect = QRect(head.x, kOuterPad, head.w, kHeaderHeight - 6);
        visual.title = QString::fromUtf8(head.text);
        visual.title_color = palette.header;
        visuals.push_back(visual);
    }

    int y = kOuterPad + kHeaderHeight;
    for (int row_i = 0; row_i < static_cast<int>(graph.rows.size()); ++row_i) {
        const ffmpeg::GraphRow& row = graph.rows.at(static_cast<size_t>(row_i));
        const int height = RowHeight(graph, row_i);

        // 输入节点
        Visual input;
        input.row = row_i;
        if (row.input_index >= 0) {
            input.kind = VisualKind::InputNode;
            input.title = QStringLiteral("%1 %2").arg(row.input_label, KindPrefix(row.kind));
            input.subtitle = ElideToWidth(row.input_summary, node_width_input - 12);
            input.fill = palette.node_fill;
            input.border = palette.border;
        } else {
            input.kind = VisualKind::Placeholder;
            input.title = QStringLiteral("（无对应输入）");
            input.subtitle = QStringLiteral("滤镜 / lavfi 新生成");
            input.border = palette.border;
        }
        input.title_color = palette.title;
        input.subtitle_color = palette.subtitle;
        input.rect = QRect(cols.x0, y, node_width_input, kNodeHeight);
        visuals.push_back(input);

        // 滤镜节点
        Visual filter;
        filter.row = row_i;
        if (!row.filter_labels.isEmpty()) {
            filter.kind = VisualKind::FilterNode;
            filter.title = ElideToWidth(row.filter_labels.join(QStringLiteral(" → ")), node_width_filter - 12);
            filter.subtitle = QStringLiteral("滤镜链（%1 个）").arg(row.filter_labels.size());
            filter.fill = palette.process_fill;
        } else {
            filter.kind = VisualKind::Placeholder;
            filter.title = QStringLiteral("直通");
            filter.subtitle = QStringLiteral("不过滤镜");
        }
        filter.border = palette.border;
        filter.title_color = row.filter_labels.isEmpty() ? palette.subtitle : palette.title;
        filter.subtitle_color = palette.subtitle;
        filter.rect = QRect(cols.x1, y, node_width_filter, kNodeHeight);
        visuals.push_back(filter);

        // 编码节点
        Visual encoder;
        encoder.row = row_i;
        switch (row.encoder_kind) {
        case ffmpeg::ProcessKind::Drop:
            encoder.kind = VisualKind::Placeholder;
            encoder.title = row.encoder_label;
            encoder.subtitle = QStringLiteral("不写入这一路");
            encoder.title_color = QColor(StateBadgeColor(ffmpeg::ChangeState::Removed));
            break;
        case ffmpeg::ProcessKind::Copy:
            encoder.kind = VisualKind::EncoderNode;
            encoder.title = row.encoder_label;
            encoder.subtitle = ElideToWidth(row.encoder_detail, node_width_encoder - 12);
            encoder.fill = palette.process_fill;
            encoder.title_color = palette.title;
            break;
        default:
            encoder.kind = VisualKind::EncoderNode;
            encoder.title = ElideToWidth(row.encoder_label, node_width_encoder - 12);
            encoder.subtitle = ElideToWidth(row.encoder_detail, node_width_encoder - 12);
            encoder.fill = palette.process_fill;
            encoder.title_color = palette.title;
            break;
        }
        encoder.border = palette.border;
        encoder.subtitle_color = palette.subtitle;
        encoder.rect = QRect(cols.x2, y, node_width_encoder, kNodeHeight);
        visuals.push_back(encoder);

        // 输出节点
        Visual output;
        output.row = row_i;
        if (row.output_index >= 0) {
            output.kind = VisualKind::OutputNode;
            output.title = QStringLiteral("%1 %2").arg(row.output_label, KindPrefix(row.kind));
            output.subtitle = ElideToWidth(row.output_summary, node_width_output - 12);
            output.fill = palette.node_fill;
            output.border = QColor(StateBadgeColor(row.state));
        } else {
            output.kind = VisualKind::Placeholder;
            output.title = graph.output_measured ? QStringLiteral("输出里没有这一路") : QStringLiteral("未探测");
            output.subtitle =
                graph.output_measured ? QStringLiteral("按声明被丢弃 / 未映射") : QStringLiteral("跑完之后才有实测");
            output.border = QColor(StateBadgeColor(ffmpeg::ChangeState::Unknown));
        }
        output.title_color = palette.title;
        output.subtitle_color = palette.subtitle;
        output.rect = QRect(cols.x3, y, node_width_output, kNodeHeight);
        visuals.push_back(output);

        // 变化标签（挂在输出节点下方）
        const int badge_lines = BadgeLines(graph, row_i);
        if (badge_lines > 0) {
            const bool folded = static_cast<int>(row.changes.size()) > kMaxBadgesPerRow;
            int badge_y = y + kNodeHeight + kBadgeGap;
            for (int b = 0; b < badge_lines; ++b) {
                Visual badge;
                badge.row = row_i;
                badge.kind = VisualKind::ChangeBadge;
                badge.rect = QRect(cols.x3, badge_y, node_width_output, kBadgeHeight);
                if (folded && b == badge_lines - 1) {
                    const int rest = static_cast<int>(row.changes.size()) - (badge_lines - 1);
                    badge.change_index = -1;
                    badge.title = QStringLiteral("还有 %1 项（点开看详情）").arg(rest);
                    badge.title_color = palette.subtitle;
                    badge.border = palette.border;
                } else {
                    const ffmpeg::FieldChange& change = row.changes.at(static_cast<size_t>(b));
                    badge.change_index = b;
                    // 标题只占"右侧留给状态词"之外的宽度：两段文字抢同一块地方就会叠字
                    const QString state = ffmpeg::ChangeStateName(change.state);
                    const int limit =
                        std::max(node_width_output - 8 - 8 - StateLabelWidth(state) - (state.isEmpty() ? 0 : 6), 24);
                    badge.title = ElideToWidth(QStringLiteral("%1 %2 → %3")
                                                   .arg(change.label,
                                                        change.before.isEmpty() ? QStringLiteral("—") : change.before,
                                                        change.after.isEmpty() ? QStringLiteral("—") : change.after),
                                               limit);
                    badge.badge = ffmpeg::ChangeStateName(change.state);
                    badge.title_color = QColor(StateBadgeColor(change.state));
                    badge.border = QColor(StateBadgeColor(change.state));
                }
                badge_y += kBadgeHeight + kBadgeGap;
                visuals.push_back(badge);
            }
        }

        // 连接线
        const auto add_edge = [&](int from_x, int to_x, int mid_y) {
            Visual edge;
            edge.kind = VisualKind::Edge;
            edge.row = row_i;
            edge.edge = QLine(from_x, mid_y, to_x, mid_y);
            edge.border = palette.grid;
            visuals.push_back(edge);
        };
        add_edge(input.rect.right(), filter.rect.left() - 1, y + kNodeHeight / 2);
        add_edge(filter.rect.right(), encoder.rect.left() - 1, y + kNodeHeight / 2);
        add_edge(encoder.rect.right(), output.rect.left() - 1, y + kNodeHeight / 2);

        y += height + kRowGap;
    }

    // ---- 底部容器条 ----
    if (!graph.container_changes.empty()) {
        int x = cols.x0;
        const int max_right = cols.right;
        int drawn = 0;
        for (size_t i = 0; i < graph.container_changes.size(); ++i) {
            const ffmpeg::FieldChange& change = graph.container_changes.at(i);
            const QString text = QStringLiteral("%1 %2 → %3")
                                     .arg(change.label, change.before.isEmpty() ? QStringLiteral("—") : change.before,
                                          change.after.isEmpty() ? QStringLiteral("—") : change.after);
            const QString state = ffmpeg::ChangeStateName(change.state);
            const int state_width = StateLabelWidth(state);
            // 封顶 260；封顶后由 ElideToWidth 截正文，而不是让两段文字压在一起。
            const int width = std::min(BadgeMinWidth(text, state), 260);
            if (x + width > max_right) {
                Visual more;
                more.kind = VisualKind::ContainerBadge;
                more.change_index = -1;
                more.rect = QRect(x, y, std::min(max_right - x, 120), kBadgeHeight);
                more.title = QStringLiteral("还有 %1 项").arg(graph.container_changes.size() - drawn);
                more.title_color = palette.subtitle;
                more.border = palette.border;
                visuals.push_back(more);
                break;
            }
            Visual badge;
            badge.kind = VisualKind::ContainerBadge;
            badge.change_index = static_cast<int>(i);
            badge.rect = QRect(x, y, width, kBadgeHeight);
            badge.title = ElideToWidth(text, std::max(width - 8 - 8 - state_width - (state.isEmpty() ? 0 : 6), 24));
            badge.badge = state;
            badge.title_color = QColor(StateBadgeColor(change.state));
            badge.border = QColor(StateBadgeColor(change.state));
            visuals.push_back(badge);
            x += width + kBadgeGap;
            ++drawn;
        }
    }
    return visuals;
}

int VisualAt(const std::vector<Visual>& visuals, QPoint pos) {
    for (int i = static_cast<int>(visuals.size()) - 1; i >= 0; --i) {
        const Visual& visual = visuals.at(static_cast<size_t>(i));
        if (visual.kind == VisualKind::Edge || visual.kind == VisualKind::Header) {
            continue; // 点线 / 点表头不该跳详情
        }
        if (visual.rect.contains(pos)) {
            return i;
        }
    }
    return -1;
}

QString DescribeVisual(const ffmpeg::ChangeGraph& graph, const Visual& visual) {
    switch (visual.kind) {
    case VisualKind::InputNode: {
        if (visual.row < 0 || visual.row >= static_cast<int>(graph.rows.size())) {
            return QString();
        }
        const ffmpeg::GraphRow& row = graph.rows.at(static_cast<size_t>(visual.row));
        return QStringLiteral("输入 %1（%2）：%3")
            .arg(row.input_label, KindPrefix(row.kind),
                 row.input_summary.isEmpty() ? QStringLiteral("未读到参数") : row.input_summary);
    }
    case VisualKind::FilterNode: {
        const ffmpeg::GraphRow& row = graph.rows.at(static_cast<size_t>(visual.row));
        return QStringLiteral("滤镜链（按命令顺序）：%1 —— 滤镜会真正改变画面，实际效果只能看实测结果")
            .arg(row.filter_labels.join(QStringLiteral(" → ")));
    }
    case VisualKind::EncoderNode: {
        const ffmpeg::GraphRow& row = graph.rows.at(static_cast<size_t>(visual.row));
        return QStringLiteral("编码：%1 — %2").arg(row.encoder_label, row.encoder_detail);
    }
    case VisualKind::OutputNode: {
        const ffmpeg::GraphRow& row = graph.rows.at(static_cast<size_t>(visual.row));
        QString text = QStringLiteral("输出 %1（%2）：%3")
                           .arg(row.output_label, KindPrefix(row.kind),
                                row.output_summary.isEmpty() ? QStringLiteral("未读到参数") : row.output_summary);
        if (!row.note.isEmpty()) {
            text += QStringLiteral(" — ") + row.note;
        }
        return text;
    }
    case VisualKind::ChangeBadge: {
        const ffmpeg::GraphRow& row = graph.rows.at(static_cast<size_t>(visual.row));
        if (visual.change_index < 0 || visual.change_index >= static_cast<int>(row.changes.size())) {
            return QStringLiteral("这一行共 %1 项变化，已折叠显示").arg(row.changes.size());
        }
        return ffmpeg::DescribeChange(row.changes.at(static_cast<size_t>(visual.change_index)));
    }
    case VisualKind::ContainerBadge: {
        if (visual.change_index < 0 || visual.change_index >= static_cast<int>(graph.container_changes.size())) {
            return QStringLiteral("容器级共 %1 项变化，已折叠显示").arg(graph.container_changes.size());
        }
        return ffmpeg::DescribeChange(graph.container_changes.at(static_cast<size_t>(visual.change_index)));
    }
    case VisualKind::Placeholder:
        return visual.title + (visual.subtitle.isEmpty() ? QString() : QStringLiteral(" — ") + visual.subtitle);
    case VisualKind::Header:
    case VisualKind::Edge:
        break;
    }
    return QString();
}

QByteArray BuildSvg(const std::vector<Visual>& visuals, QSize size, const QString& title, const ViewPalette& palette) {
    QByteArray bytes;
    if (visuals.empty() || size.isEmpty()) {
        return bytes; // 空图不产文件："导出成功但一片空白"比失败更糟
    }
    QXmlStreamWriter xml(&bytes);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("svg"));
    xml.writeAttribute(QStringLiteral("xmlns"), QStringLiteral("http://www.w3.org/2000/svg"));
    xml.writeAttribute(QStringLiteral("width"), QString::number(size.width()));
    xml.writeAttribute(QStringLiteral("height"), QString::number(size.height()));
    xml.writeAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 %1 %2").arg(size.width()).arg(size.height()));
    xml.writeStartElement(QStringLiteral("title"));
    xml.writeCharacters(title);
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("defs"));
    xml.writeStartElement(QStringLiteral("marker"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("arrow"));
    xml.writeAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 10 10"));
    xml.writeAttribute(QStringLiteral("refX"), QStringLiteral("8"));
    xml.writeAttribute(QStringLiteral("refY"), QStringLiteral("5"));
    xml.writeAttribute(QStringLiteral("markerWidth"), QStringLiteral("6"));
    xml.writeAttribute(QStringLiteral("markerHeight"), QStringLiteral("6"));
    xml.writeAttribute(QStringLiteral("orient"), QStringLiteral("auto-start-reverse"));
    xml.writeStartElement(QStringLiteral("path"));
    xml.writeAttribute(QStringLiteral("d"), QStringLiteral("M2 1L8 5L2 9"));
    xml.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
    xml.writeAttribute(QStringLiteral("stroke"), QStringLiteral("context-stroke"));
    xml.writeAttribute(QStringLiteral("stroke-width"), QStringLiteral("1.5"));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();

    xml.writeStartElement(QStringLiteral("rect"));
    xml.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    xml.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    xml.writeAttribute(QStringLiteral("width"), QString::number(size.width()));
    xml.writeAttribute(QStringLiteral("height"), QString::number(size.height()));
    xml.writeAttribute(QStringLiteral("fill"), palette.background.name());
    xml.writeEndElement();

    for (const Visual& visual : visuals) {
        if (visual.kind == VisualKind::Edge) {
            xml.writeStartElement(QStringLiteral("line"));
            xml.writeAttribute(QStringLiteral("x1"), QString::number(visual.edge.x1()));
            xml.writeAttribute(QStringLiteral("y1"), QString::number(visual.edge.y1()));
            xml.writeAttribute(QStringLiteral("x2"), QString::number(visual.edge.x2()));
            xml.writeAttribute(QStringLiteral("y2"), QString::number(visual.edge.y2()));
            xml.writeAttribute(QStringLiteral("stroke"), visual.border.name());
            xml.writeAttribute(QStringLiteral("stroke-width"), QStringLiteral("1.2"));
            xml.writeAttribute(QStringLiteral("marker-end"), QStringLiteral("url(#arrow)"));
            xml.writeEndElement();
            continue;
        }
        if (visual.kind == VisualKind::Header) {
            xml.writeStartElement(QStringLiteral("text"));
            xml.writeAttribute(QStringLiteral("x"), QString::number(visual.rect.left()));
            xml.writeAttribute(QStringLiteral("y"), QString::number(visual.rect.bottom()));
            xml.writeAttribute(QStringLiteral("font-family"), QStringLiteral("sans-serif"));
            xml.writeAttribute(QStringLiteral("font-size"), QStringLiteral("11"));
            xml.writeAttribute(QStringLiteral("fill"), visual.title_color.name());
            xml.writeCharacters(visual.title);
            xml.writeEndElement();
            continue;
        }
        if (!visual.fill.isValid()) {
            // 占位 / 标签：只描边不填充
            xml.writeStartElement(QStringLiteral("rect"));
            xml.writeAttribute(QStringLiteral("x"), QString::number(visual.rect.left()));
            xml.writeAttribute(QStringLiteral("y"), QString::number(visual.rect.top()));
            xml.writeAttribute(QStringLiteral("width"), QString::number(visual.rect.width()));
            xml.writeAttribute(QStringLiteral("height"), QString::number(visual.rect.height()));
            xml.writeAttribute(QStringLiteral("rx"),
                               visual.kind == VisualKind::ChangeBadge || visual.kind == VisualKind::ContainerBadge
                                   ? QStringLiteral("9")
                                   : QStringLiteral("6"));
            xml.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
            xml.writeAttribute(QStringLiteral("stroke"), visual.border.name());
            xml.writeAttribute(QStringLiteral("stroke-width"), QStringLiteral("0.6"));
            if (visual.kind == VisualKind::Placeholder) {
                xml.writeAttribute(QStringLiteral("stroke-dasharray"), QStringLiteral("3 3"));
            }
            xml.writeEndElement();
        } else {
            xml.writeStartElement(QStringLiteral("rect"));
            xml.writeAttribute(QStringLiteral("x"), QString::number(visual.rect.left()));
            xml.writeAttribute(QStringLiteral("y"), QString::number(visual.rect.top()));
            xml.writeAttribute(QStringLiteral("width"), QString::number(visual.rect.width()));
            xml.writeAttribute(QStringLiteral("height"), QString::number(visual.rect.height()));
            xml.writeAttribute(QStringLiteral("rx"), QStringLiteral("6"));
            xml.writeAttribute(QStringLiteral("fill"), visual.fill.name());
            xml.writeAttribute(QStringLiteral("stroke"), visual.border.name());
            xml.writeAttribute(QStringLiteral("stroke-width"), QStringLiteral("0.6"));
            xml.writeEndElement();
        }

        const bool compact = visual.kind == VisualKind::ChangeBadge || visual.kind == VisualKind::ContainerBadge;
        const int title_y = compact ? visual.rect.center().y() + 4 : visual.rect.top() + 20;
        xml.writeStartElement(QStringLiteral("text"));
        xml.writeAttribute(QStringLiteral("x"), QString::number(visual.rect.left() + 8));
        xml.writeAttribute(QStringLiteral("y"), QString::number(title_y));
        xml.writeAttribute(QStringLiteral("font-family"), QStringLiteral("sans-serif"));
        xml.writeAttribute(QStringLiteral("font-size"), compact ? QStringLiteral("11") : QStringLiteral("12"));
        xml.writeAttribute(QStringLiteral("fill"), visual.title_color.name());
        xml.writeCharacters(visual.title);
        xml.writeEndElement();

        if (!compact && !visual.subtitle.isEmpty()) {
            xml.writeStartElement(QStringLiteral("text"));
            xml.writeAttribute(QStringLiteral("x"), QString::number(visual.rect.left() + 8));
            xml.writeAttribute(QStringLiteral("y"), QString::number(visual.rect.top() + 38));
            xml.writeAttribute(QStringLiteral("font-family"), QStringLiteral("sans-serif"));
            xml.writeAttribute(QStringLiteral("font-size"), QStringLiteral("11"));
            xml.writeAttribute(QStringLiteral("fill"), visual.subtitle_color.name());
            xml.writeCharacters(visual.subtitle);
            xml.writeEndElement();
        }
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    return bytes;
}

} // namespace change_graph_view

// ---------------------------------------------------------------------------
// 控件
// ---------------------------------------------------------------------------

using namespace change_graph_view;

FfmpegChangeGraphCanvas::FfmpegChangeGraphCanvas(QWidget* parent) : QWidget(parent) {
    palette_ = DarkPalette();
    setMinimumHeight(180);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void FfmpegChangeGraphCanvas::SetGraph(const ffmpeg::ChangeGraph& graph, const change_graph_view::ViewPalette& palette,
                                       const QString& empty_hint) {
    graph_ = graph;
    palette_ = palette;
    empty_hint_ = empty_hint;
    has_data_ = graph_.ok;
    selected_ = -1;
    // 高度只跟行数有关（不随宽度折行），在这里算一次就够 ——
    // 外层是 QScrollArea：行多了就滚动，不裁掉最后一路流。
    setMinimumHeight(has_data_ ? PreferredHeight(graph_, std::max(width(), 640)) : 180);
    Rebuild();
}

void FfmpegChangeGraphCanvas::Clear() {
    graph_ = ffmpeg::ChangeGraph();
    visuals_.clear();
    empty_hint_.clear();
    selected_ = -1;
    has_data_ = false;
    setMinimumHeight(180);
    update();
}

void FfmpegChangeGraphCanvas::Rebuild() {
    visuals_ = BuildVisuals(graph_, width(), palette_);
    update();
}

QByteArray FfmpegChangeGraphCanvas::ExportSvg(const QString& title) const {
    return BuildSvg(visuals_, size(), title, palette_);
}

void FfmpegChangeGraphCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (has_data_) {
        visuals_ = BuildVisuals(graph_, width(), palette_);
    }
}

void FfmpegChangeGraphCanvas::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette_.background);

    if (visuals_.empty()) {
        painter.setPen(palette_.subtitle);
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         empty_hint_);
        return;
    }

    for (int i = 0; i < static_cast<int>(visuals_.size()); ++i) {
        const Visual& visual = visuals_.at(static_cast<size_t>(i));
        if (visual.kind == VisualKind::Edge) {
            painter.setPen(QPen(visual.border, 1.2));
            painter.drawLine(visual.edge);
            // 箭头：主绘制的箭头交给末端小三角，避免依赖 marker
            const QPoint tip = visual.edge.p2();
            QPolygon arrow;
            arrow << QPoint(tip.x() - 4, tip.y() - 3) << tip << QPoint(tip.x() - 4, tip.y() + 3);
            painter.setBrush(visual.border);
            painter.drawPolygon(arrow);
            continue;
        }
        if (visual.kind == VisualKind::Header) {
            painter.setPen(visual.title_color);
            QFont font = this->font();
            font.setPixelSize(11);
            painter.setFont(font);
            painter.drawText(visual.rect, Qt::AlignLeft | Qt::AlignVCenter, visual.title);
            continue;
        }

        const bool compact = visual.kind == VisualKind::ChangeBadge || visual.kind == VisualKind::ContainerBadge;
        const int radius = compact ? 9 : 6;
        painter.setPen(QPen(visual.border, selected_ == i ? 1.6 : 0.8));
        if (visual.fill.isValid()) {
            painter.setBrush(visual.fill);
        } else {
            painter.setBrush(Qt::NoBrush);
        }
        if (visual.kind == VisualKind::Placeholder) {
            painter.setPen(QPen(visual.border, 1.0, Qt::DashLine));
        }
        painter.drawRoundedRect(visual.rect, radius, radius);

        QFont font = this->font();
        font.setPixelSize(compact ? 11 : 12);
        painter.setFont(font);
        painter.setPen(visual.title_color);
        if (compact) {
            painter.drawText(visual.rect.adjusted(8, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter, visual.title);
        } else {
            painter.drawText(visual.rect.adjusted(8, 5, -8, -26), Qt::AlignLeft | Qt::AlignTop, visual.title);
            font.setPixelSize(11);
            painter.setFont(font);
            painter.setPen(visual.subtitle_color);
            painter.drawText(visual.rect.adjusted(8, 24, -8, -5), Qt::AlignLeft | Qt::AlignTop, visual.subtitle);
        }
        if (!visual.badge.isEmpty()) {
            font.setPixelSize(10);
            painter.setFont(font);
            painter.setPen(visual.title_color);
            painter.drawText(visual.rect.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, visual.badge);
        }
    }
}

void FfmpegChangeGraphCanvas::mousePressEvent(QMouseEvent* event) {
    if (!has_data_) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int hit = VisualAt(visuals_, event->position().toPoint());
    selected_ = hit;
    if (hit >= 0) {
        emit VisualActivated(DescribeVisual(graph_, visuals_.at(static_cast<size_t>(hit))));
    } else {
        emit VisualActivated(QString());
    }
    update();
}

FfmpegChangeGraphView::FfmpegChangeGraphView(QWidget* parent) : QWidget(parent) {
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    header_bar_ = new QWidget(this);
    QHBoxLayout* header = new QHBoxLayout(header_bar_);
    header->setContentsMargins(8, 4, 8, 0);
    header->setSpacing(8);
    target_label_ = new QLabel(header_bar_);
    target_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    output_combo_ = new QComboBox(header_bar_);
    output_combo_->setVisible(false);
    export_button_ = new QPushButton(tr("导出 SVG…"), header_bar_);
    header->addWidget(target_label_, 1);
    header->addWidget(output_combo_);
    header->addWidget(export_button_);
    root->addWidget(header_bar_);

    summary_label_ = new QLabel(this);
    summary_label_->setWordWrap(true);
    summary_label_->setContentsMargins(8, 0, 8, 0);
    root->addWidget(summary_label_);

    canvas_ = new FfmpegChangeGraphCanvas(this);
    root->addWidget(canvas_, 1);
    connect(canvas_, &FfmpegChangeGraphCanvas::VisualActivated, this,
            [this](const QString& description) { detail_label_->setText(description); });

    detail_label_ = new QLabel(this);
    detail_label_->setWordWrap(true);
    detail_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_label_->setContentsMargins(8, 0, 8, 6);
    root->addWidget(detail_label_);

    setMouseTracking(true);

    connect(output_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { OnOutputChanged(); });
    connect(export_button_, &QPushButton::clicked, this, [this]() {
        if (canvas_->VisualCount() == 0) {
            emit StatusMessage(tr("当前没有可导出的图谱"));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("导出转换图谱 SVG"),
                                                          QStringLiteral("change-graph.svg"), tr("SVG 图片 (*.svg)"));
        if (path.isEmpty()) {
            return;
        }
        const QByteArray bytes = canvas_->ExportSvg(tr("FFmpeg 转换图谱"));
        if (bytes.isEmpty()) {
            emit StatusMessage(tr("导出失败：当前没有可画的元素"));
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            emit StatusMessage(tr("导出失败：写不进 %1").arg(path));
            return;
        }
        file.write(bytes);
        file.close();
        emit StatusMessage(tr("已导出 %1").arg(path));
    });
    Clear();
}

void FfmpegChangeGraphView::SetGraphs(std::vector<ffmpeg::ChangeGraph> graphs) {
    graphs_ = std::move(graphs);
    current_ = 0;
    stale_hint_.clear();

    output_combo_->blockSignals(true);
    output_combo_->clear();
    for (const ffmpeg::ChangeGraph& graph : graphs_) {
        output_combo_->addItem(graph.target.isEmpty() ? tr("输出") : graph.target);
    }
    output_combo_->setCurrentIndex(0);
    output_combo_->setVisible(graphs_.size() > 1);
    output_combo_->blockSignals(false);

    Rebuild();
}

void FfmpegChangeGraphView::SetStaleHint(const QString& text) {
    if (stale_hint_ == text) {
        return;
    }
    stale_hint_ = text;
    UpdateSummary();
}

void FfmpegChangeGraphView::Clear() {
    graphs_.clear();
    stale_hint_.clear();
    current_ = 0;
    setMinimumHeight(0); // 交还给布局：最小高度由"表头 + 摘要 + 画布 + 详情"自己算
    output_combo_->clear();
    output_combo_->setVisible(false);
    target_label_->clear();
    summary_label_->clear();
    detail_label_->clear();
    canvas_->Clear();
}

void FfmpegChangeGraphView::SetCurrentIndex(int index) {
    if (index < 0 || index >= static_cast<int>(graphs_.size()) || index == current_) {
        return;
    }
    current_ = index;
    output_combo_->setCurrentIndex(index);
    Rebuild();
}

const ffmpeg::ChangeGraph& FfmpegChangeGraphView::Current() const {
    static const ffmpeg::ChangeGraph empty;
    if (graphs_.empty()) {
        return empty;
    }
    const int index = std::clamp(current_, 0, static_cast<int>(graphs_.size()) - 1);
    return graphs_.at(static_cast<size_t>(index));
}

change_graph_view::ViewPalette FfmpegChangeGraphView::Palette() const {
    ViewPalette palette = DarkPalette();
    const QPalette& system = this->palette();
    palette.background = system.color(QPalette::Base);
    palette.node_fill = system.color(QPalette::AlternateBase);
    palette.process_fill = system.color(QPalette::Window);
    palette.border = system.color(QPalette::Mid);
    palette.title = system.color(QPalette::WindowText);
    palette.subtitle = system.color(QPalette::PlaceholderText);
    palette.header = system.color(QPalette::PlaceholderText);
    palette.grid = system.color(QPalette::Mid);
    return palette;
}

void FfmpegChangeGraphView::Rebuild() {
    const ffmpeg::ChangeGraph& graph = Current();
    target_label_->setText(graph.target.isEmpty() ? QString() : tr("输出：%1").arg(graph.target));
    UpdateSummary();
    detail_label_->clear();
    // 空态语义由 View 定（没跑过 / 没探测到 / 结构不明），画布只负责画。
    canvas_->SetGraph(graph, Palette(), EmptyHintText());
    // 最小高度不在这里写死：画布自带 PreferredHeight，布局会把表头 / 摘要 / 详情
    // 一起算进 View 的 minimumSizeHint，QScrollArea 才量得对。
    setMinimumHeight(0);
}

void FfmpegChangeGraphView::UpdateSummary() {
    const ffmpeg::ChangeGraph& graph = Current();
    if (graphs_.empty()) {
        summary_label_->setText(QString());
        return;
    }
    QStringList lines;
    if (!stale_hint_.isEmpty()) {
        lines << stale_hint_;
    }
    lines << ffmpeg::DescribeChangeSummary(graph);
    if (!graph.pairing_note.isEmpty()) {
        lines << graph.pairing_note;
    }
    if (graph.has_complex_filter) {
        lines << tr("命令里有 -filter_complex：滤镜无法逐路归属，实际结果以实测为准");
    }
    if (!graph.note.isEmpty() && graph.ok) {
        lines << graph.note;
    }
    summary_label_->setText(lines.join(QStringLiteral(" · ")));
}

QString FfmpegChangeGraphView::EmptyHintText() const {
    if (graphs_.empty()) {
        return tr("跑一次命令后，这里会给出输入与输出的实际差异");
    }
    const ffmpeg::ChangeGraph& graph = Current();
    if (!graph.ok) {
        return graph.note.isEmpty() ? tr("没有可对比的数据") : graph.note;
    }
    return QString();
}

QString FfmpegChangeGraphView::SummaryText() const {
    return summary_label_->text();
}

QString FfmpegChangeGraphView::DetailText() const {
    return detail_label_->text();
}

int FfmpegChangeGraphView::VisualCount() const {
    return canvas_ == nullptr ? 0 : canvas_->VisualCount();
}

int FfmpegChangeGraphView::SelectedVisual() const {
    return canvas_ == nullptr ? -1 : canvas_->SelectedVisual();
}

QByteArray FfmpegChangeGraphView::ExportSvg() const {
    return canvas_ == nullptr ? QByteArray() : canvas_->ExportSvg(tr("FFmpeg 转换图谱"));
}

void FfmpegChangeGraphView::OnOutputChanged() {
    const int index = output_combo_->currentIndex();
    if (index == current_) {
        return;
    }
    current_ = index;
    Rebuild();
}

} // namespace ui
} // namespace videoeye
