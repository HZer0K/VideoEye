#include "ui/analysis_panel/FileStructureMapPage.h"

#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStringList>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>

#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/analysis_panel/StructureMapFormat.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {

namespace smd = structure_map_detail;

namespace {

constexpr int kMinRowHeight = 22;
constexpr int kTreemapMaxDepth = 6;

QString HtmlEscape(const QString& text) {
    return text.toHtmlEscaped();
}

} // namespace

// ---------------------------------------------------------------------------
// 画布：纯自绘。三个视图都消费 smd::MapVisual，绘制 / 命中 / SVG 导出共用几何。
// ---------------------------------------------------------------------------
class StructureMapCanvas : public QWidget {
public:
    explicit StructureMapCanvas(QWidget* parent) : QWidget(parent) {
        setAutoFillBackground(false);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(340);
    }

    void SetData(const std::vector<smd::MapNode>& nodes, const std::vector<model::ContainerStreamInfo>& streams,
                 const QString& format_name, const QString& summary, const std::map<std::string, std::string>& metadata,
                 bool truncated, quint64 file_span, bool enabled, smd::MapLayoutMode mode, smd::MapViewKind kind,
                 const smd::PacketBandData& bands, int rows, int total_layers, const smd::FormatProfile& profile,
                 const smd::StreamByteRows& stream_rows) {
        nodes_ = nodes;
        profile_ = profile;
        streams_ = streams;
        format_name_ = format_name;
        summary_ = summary;
        metadata_ = metadata;
        truncated_ = truncated;
        file_span_ = file_span;
        enabled_ = enabled;
        mode_ = mode;
        view_kind_ = kind;
        bands_ = bands;
        stream_rows_ = stream_rows;
        SetRows(rows, total_layers);
        if (selected_ >= static_cast<int>(nodes_.size()))
            selected_ = -1;
        InvalidateVisuals();
        update();
    }

    void SetRows(int rows, int total_layers) {
        rows_ = std::max(1, rows);
        total_layers_ = std::max(rows_, total_layers);
        first_row_ = std::max(0, std::min(first_row_, total_layers_ - rows_));
        InvalidateVisuals();
        update();
    }

    void Reset() {
        nodes_.clear();
        streams_.clear();
        metadata_.clear();
        bands_ = smd::PacketBandData{};
        stream_rows_ = smd::StreamByteRows{};
        profile_ = smd::FormatProfile{};
        truncated_ = false;
        file_span_ = 0;
        selected_ = -1;
        hovered_ = -1;
        selected_bin_track_ = -1;
        selected_bin_index_ = -1;
        selected_fact_ = -1;
        selected_stream_row_ = -1;
        selected_stream_bin_ = -1;
        hits_.clear();
        view_ = smd::MapViewport{};
        first_row_ = 0;
        InvalidateVisuals();
        update();
    }

    smd::MapViewport view() const {
        return view_;
    }
    int first_row() const {
        return first_row_;
    }
    int rows() const {
        return rows_;
    }
    smd::MapViewKind view_kind() const {
        return view_kind_;
    }
    int VisualCount() const {
        return static_cast<int>(Visuals().size());
    }
    const std::vector<smd::MapVisual>& Visuals() const {
        if (visuals_dirty_) {
            visuals_cache_ = BuildVisuals();
            visuals_dirty_ = false;
        }
        return visuals_cache_;
    }

    smd::MapGeometry Geometry() const {
        smd::MapGeometry g;
        g.left = smd::kDepthLabelWidth;
        g.top = smd::kInfoHeight;
        g.width = std::max(1, width() - smd::kDepthLabelWidth - smd::kPlotRight);
        const int stream_h = streams_.empty() || view_kind_ != smd::MapViewKind::Icicle ? 0 : smd::kStreamHeight;
        const int available = height() - smd::kInfoHeight - smd::kAxisHeight - stream_h - 4;
        g.rows = rows_;
        g.row_height = std::max(kMinRowHeight, std::min(34, available / std::max(1, rows_)));
        return g;
    }

    QRect PlotRect() const {
        const smd::MapGeometry g = Geometry();
        // -2 而不是 -8：底部两行「流字节分布」也要落在这个裁剪区里，
        // 否则最下面一行会被裁掉，看起来像只有一条流有数据。
        const int h = std::max(8, height() - smd::kInfoHeight - smd::kAxisHeight - 2);
        return QRect(g.left, g.top, g.width, h);
    }

    // 流带顶边。绘制与命中必须读同一份几何，别在两处各算一遍。
    int BandTop(const smd::MapGeometry& geom) const {
        const int natural = geom.top + geom.rows * geom.row_height + 4;
        return std::min(natural, std::max(geom.top, height() - smd::kAxisHeight - smd::kStreamHeight));
    }

    void SetView(smd::MapViewport view) {
        view_ = smd::ClampViewport(view);
        InvalidateVisuals();
        update();
    }
    void SetFirstRow(int row) {
        const int clamped = std::max(0, std::min(row, total_layers_ - rows_));
        if (clamped == first_row_)
            return;
        first_row_ = clamped;
        InvalidateVisuals();
        update();
    }
    void SetSelection(int index) {
        selected_ = index;
        update();
    }
    void SetHits(const QVector<int>& hits) {
        hits_ = hits;
        update();
    }
    void SetViewKind(smd::MapViewKind kind) {
        if (view_kind_ == kind)
            return;
        view_kind_ = kind;
        selected_bin_track_ = -1;
        selected_bin_index_ = -1;
        hovered_ = -1;
        selected_fact_ = -1;
        InvalidateVisuals();
        update();
    }
    void SetBinSelection(int track, int bin) {
        selected_bin_track_ = track;
        selected_bin_index_ = bin;
        if (track >= 0)
            selected_ = -1;
        update();
    }
    void SetFactSelection(int index) {
        selected_fact_ = index;
        update();
    }
    void SetStreamSelection(int row, int bin) {
        selected_stream_row_ = row;
        selected_stream_bin_ = bin;
        update();
    }

    std::function<void(int)> on_select;          // node index（或 -1 取消选中）
    std::function<void(int, int)> on_bin_select; // track / bin
    std::function<void(int)> on_fact_select;     // 格式概览里的事实卡片
    std::function<void(int, int)> on_stream_bin; // 流字节分布的行 / 桶
    std::function<void(int)> on_focus;           // 聚焦节点区间
    std::function<void(int)> on_jump;            // 跳到结构树页

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(theme::color::kBg));
        painter.setRenderHint(QPainter::Antialiasing, false);
        QFont small = font();
        small.setPointSizeF(8.5);
        painter.setFont(small);

        if (!enabled_) {
            DrawCenteredHint(painter, tr("本页分析已关闭 —— 勾选右上角「启用分析」即可显示结构图谱"));
            return;
        }
        if (view_kind_ == smd::MapViewKind::PacketBands && !bands_.available) {
            DrawCenteredHint(painter, tr("%1\n%2").arg(bands_.source, bands_.coverage_note));
            return;
        }
        if (view_kind_ == smd::MapViewKind::FormatOverview && profile_.facts.empty()) {
            DrawCenteredHint(painter, tr("本格式没有可展示的容器事实卡片"));
            return;
        }
        if (view_kind_ != smd::MapViewKind::PacketBands && view_kind_ != smd::MapViewKind::FormatOverview &&
            nodes_.empty()) {
            DrawCenteredHint(painter, tr("打开媒体文件后将自动生成结构图谱"));
            return;
        }

        const smd::MapGeometry geom = Geometry();
        DrawInfoBar(painter, geom);
        if (view_kind_ == smd::MapViewKind::Icicle)
            DrawDepthLabels(painter, geom);
        else if (view_kind_ == smd::MapViewKind::PacketBands)
            DrawBandLabels(painter);

        const std::vector<smd::MapVisual>& visuals = Visuals();
        painter.save();
        painter.setClipRect(PlotRect());
        for (size_t i = 0; i < visuals.size(); ++i) {
            const smd::MapVisual& visual = visuals[i];
            const QRect box = visual.rect;
            QRect fill_rect = box;
            if (visual.bar_height >= 0) {
                const int h = std::max(1, std::min(box.height(), visual.bar_height));
                fill_rect = QRect(box.left(), box.bottom() + 1 - h, box.width(), h);
            }
            if (visual.hatched)
                painter.fillRect(fill_rect, QBrush(visual.fill, Qt::BDiagPattern));
            else
                painter.fillRect(fill_rect, visual.fill);
            painter.setPen(visual.border);
            painter.drawRect(fill_rect.adjusted(0, 0, -1, -1));
            const bool hit = visual.node_index >= 0 && hits_.contains(visual.node_index);
            if (hit) {
                painter.fillRect(fill_rect, QColor(210, 153, 34, 46));
                painter.setPen(QPen(QColor(theme::color::kWarning), 1));
                painter.drawRect(fill_rect.adjusted(0, 0, -1, -1));
            }
            const bool hovered = static_cast<int>(i) == hovered_;
            const bool selected = (visual.node_index >= 0 && visual.node_index == selected_) ||
                                  (visual.track_index >= 0 && visual.track_index == selected_bin_track_ &&
                                   visual.bin_index == selected_bin_index_) ||
                                  (visual.fact_index >= 0 && visual.fact_index == selected_fact_) ||
                                  (visual.stream_row >= 0 && visual.stream_row == selected_stream_row_ &&
                                   visual.stream_bin == selected_stream_bin_);
            if (hovered || selected) {
                painter.setPen(QPen(QColor(hovered ? theme::color::kBorderLight : theme::color::kAccent), 2));
                painter.drawRect(fill_rect.adjusted(1, 1, -2, -2));
            }
            if (visual.label.isEmpty() || fill_rect.width() <= 30 || fill_rect.height() < 12)
                continue;
            const QString label =
                fill_rect.width() > 130 && visual.node_index >= 0
                    ? QStringLiteral("%1  %2").arg(visual.label, smd::FormatBytes(nodes_[visual.node_index].size))
                    : visual.label;
            painter.setPen(QColor(theme::color::kTextPrimary));
            painter.drawText(fill_rect.adjusted(4, 0, -4, 0), Qt::AlignTop | Qt::AlignLeft,
                             painter.fontMetrics().elidedText(label, Qt::ElideRight, fill_rect.width() - 8));
        }
        painter.restore();

        if (view_kind_ == smd::MapViewKind::Icicle)
            DrawStreamRowLabels(painter, geom, BandTop(geom));
        DrawAxis(painter, geom);
        DrawHoverCard(painter);
    }

    void resizeEvent(QResizeEvent* event) override {
        InvalidateVisuals();
        QWidget::resizeEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
        const int delta = event->angleDelta().y();
        if (delta == 0) {
            event->ignore();
            return;
        }
        const smd::MapGeometry geom = Geometry();
        if ((event->modifiers() & Qt::ShiftModifier) && view_kind_ == smd::MapViewKind::Icicle) {
            SetFirstRow(first_row_ + (delta > 0 ? -1 : 1));
        } else if (view_kind_ == smd::MapViewKind::Icicle) {
            const double px =
                std::max(0.0, std::min(1.0, static_cast<double>(event->position().x() - geom.left) / geom.width));
            const double anchor = view_.FromScreen(px);
            SetView(smd::ZoomAt(view_, anchor, std::exp(delta / 120.0 * 0.35)));
        } else {
            event->ignore(); // treemap / 包带没有字节视窗，别把滚轮吞了
            return;
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        pressed_ = true;
        dragging_ = false;
        press_pos_ = event->position().toPoint();
        last_pos_ = press_pos_;
        drag_y_accum_ = 0.0;
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const QPoint pos = event->position().toPoint();
        if (pressed_) {
            const QPoint moved = pos - press_pos_;
            if (!dragging_ && (std::abs(moved.x()) > 4 || std::abs(moved.y()) > 4))
                dragging_ = true;
            if (dragging_) {
                const smd::MapGeometry geom = Geometry();
                const double dx = static_cast<double>(pos.x() - last_pos_.x());
                if (view_kind_ == smd::MapViewKind::Icicle) {
                    smd::MapViewport next = view_;
                    next.origin -= dx / (geom.width * next.scale);
                    SetView(next);
                }
                drag_y_accum_ += static_cast<double>(pos.y() - last_pos_.y());
                while (drag_y_accum_ >= geom.row_height) {
                    SetFirstRow(first_row_ + 1);
                    drag_y_accum_ -= geom.row_height;
                }
                while (drag_y_accum_ <= -geom.row_height) {
                    SetFirstRow(first_row_ - 1);
                    drag_y_accum_ += geom.row_height;
                }
            }
            last_pos_ = pos;
            event->accept();
            return;
        }
        const int hit = HitAt(pos);
        if (hit != hovered_) {
            hovered_ = hit;
            update();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (pressed_ && !dragging_ && event->button() == Qt::LeftButton)
            DispatchSelection(event->position().toPoint());
        pressed_ = false;
        dragging_ = false;
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseDoubleClickEvent(event);
            return;
        }
        const int vi = HitAt(event->position().toPoint());
        if (vi < 0) {
            if (on_focus)
                on_focus(-1);
        } else if (event->modifiers() & Qt::ShiftModifier) {
            if (on_focus)
                on_focus(Visuals()[vi].node_index);
        } else {
            const smd::MapVisual& visual = Visuals()[vi];
            if (visual.node_index >= 0) {
                if (on_jump)
                    on_jump(visual.node_index);
            } else {
                DispatchSelection(event->position().toPoint());
            }
        }
        event->accept();
    }

    void leaveEvent(QEvent* event) override {
        if (hovered_ != -1) {
            hovered_ = -1;
            update();
        }
        QWidget::leaveEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
        case Qt::Key_Escape:
            view_ = smd::MapViewport{};
            SetFirstRow(0);
            if (on_select)
                on_select(-1);
            update();
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            SetView(smd::ZoomAt(view_, view_.FromScreen(0.5), 1.4));
            break;
        case Qt::Key_Minus:
            SetView(smd::ZoomAt(view_, view_.FromScreen(0.5), 1.0 / 1.4));
            break;
        case Qt::Key_Left: {
            smd::MapViewport next = view_;
            next.origin -= 0.1 / next.scale;
            SetView(next);
            break;
        }
        case Qt::Key_Right: {
            smd::MapViewport next = view_;
            next.origin += 0.1 / next.scale;
            SetView(next);
            break;
        }
        case Qt::Key_Up:
            SetFirstRow(first_row_ - 1);
            break;
        case Qt::Key_Down:
            SetFirstRow(first_row_ + 1);
            break;
        default:
            QWidget::keyPressEvent(event);
            return;
        }
        event->accept();
    }

private:
    void InvalidateVisuals() {
        visuals_dirty_ = true;
    }

    std::vector<smd::MapVisual> BuildVisuals() const {
        switch (view_kind_) {
        case smd::MapViewKind::Treemap:
            return smd::BuildTreemapVisuals(nodes_, PlotRect(), smd::kMaxMapNodes);
        case smd::MapViewKind::PacketBands:
            return smd::BuildPacketVisuals(bands_, PlotRect());
        case smd::MapViewKind::FormatOverview:
            return smd::BuildFormatVisuals(profile_, PlotRect());
        case smd::MapViewKind::Icicle:
        default: {
            std::vector<smd::MapVisual> visuals = smd::BuildIcicleVisuals(nodes_, Geometry(), view_, first_row_);
            // 扫描窗口叠加放最前面：先画、且 decorative 不参与命中，
            // 节点矩形与 SVG 导出仍共用同一份 visuals。
            const std::vector<smd::MapVisual> overlay = smd::BuildWindowOverlay(profile_, Geometry(), view_);
            visuals.insert(visuals.begin(), overlay.begin(), overlay.end());
            // 底部流带也进同一份 visuals：绘制 / 命中 / 详情 / SVG 导出共用矩形。
            const smd::MapGeometry geom = Geometry();
            const QRect band(geom.left, BandTop(geom), geom.width, smd::kStreamHeight);
            const std::vector<smd::MapVisual> stream_visuals =
                smd::BuildStreamByteVisuals(stream_rows_, band, view_, smd::kStreamRowHeight);
            visuals.insert(visuals.end(), stream_visuals.begin(), stream_visuals.end());
            return visuals;
        }
        }
    }

    int HitAt(QPoint pos) const {
        return smd::VisualAt(Visuals(), pos);
    }

    void DispatchSelection(QPoint pos) {
        const int vi = HitAt(pos);
        if (vi < 0) {
            if (on_select)
                on_select(-1);
            if (on_bin_select)
                on_bin_select(-1, -1);
            if (on_stream_bin)
                on_stream_bin(-1, -1);
            return;
        }
        const smd::MapVisual& visual = Visuals()[vi];
        if (visual.node_index >= 0) {
            if (on_select)
                on_select(visual.node_index);
            if (on_bin_select)
                on_bin_select(-1, -1);
        } else if (visual.track_index >= 0) {
            if (on_select)
                on_select(-1);
            if (on_bin_select)
                on_bin_select(visual.track_index, visual.bin_index);
        } else if (visual.fact_index >= 0) {
            if (on_select)
                on_select(-1);
            if (on_bin_select)
                on_bin_select(-1, -1);
            if (on_fact_select)
                on_fact_select(visual.fact_index);
        } else if (visual.stream_row >= 0) {
            if (on_select)
                on_select(-1);
            if (on_bin_select)
                on_bin_select(-1, -1);
            if (on_stream_bin)
                on_stream_bin(visual.stream_row, visual.stream_bin);
        }
    }

    void DrawCenteredHint(QPainter& painter, const QString& text) {
        painter.setPen(QColor(theme::color::kTextSecondary));
        painter.drawText(rect(), Qt::AlignCenter, text);
    }

    void DrawInfoBar(QPainter& painter, const smd::MapGeometry& geom) {
        QString line1;
        QString line2;
        switch (view_kind_) {
        case smd::MapViewKind::Treemap:
            line1 = tr("%1 · %2 · %3 个节点 · 字节树图（压缩字节权重）")
                        .arg(format_name_, file_span_ > 0 ? smd::FormatBytes(file_span_) : tr("大小未知"))
                        .arg(nodes_.size());
            line2 = tr("面积按压缩权重且小盒子保底（真实占比见节点详情）· 最深 %1 层 · 与结构图共用节点身份")
                        .arg(kTreemapMaxDepth);
            break;
        case smd::MapViewKind::PacketBands:
            if (bands_.axis == smd::MapBandAxis::ByteOffset) {
                line1 = tr("%1 · 字节分布带 · %2 · 跨度 %3")
                            .arg(format_name_, bands_.source, smd::FormatBytes(static_cast<quint64>(bands_.axis_max)));
                line2 = tr("%1 条轨 · %2 个字节桶 · %3 条块摘要 · %4")
                            .arg(bands_.tracks.size())
                            .arg(bands_.BinCount())
                            .arg(bands_.TotalCount())
                            .arg(bands_.sampled ? tr("抽样结果") : tr("已声明摘要全集"));
            } else {
                line1 = tr("%1 · 时间轴包带 · %2 · 时长 %3 s")
                            .arg(format_name_, bands_.source)
                            .arg(bands_.duration_seconds, 0, 'f', 2);
                line2 = tr("%1 条轨 · %2 个时间桶 · %3 条记录 · %4")
                            .arg(bands_.tracks.size())
                            .arg(bands_.BinCount())
                            .arg(bands_.TotalCount())
                            .arg(bands_.sampled ? tr("抽样结果") : tr("已声明样本全集"));
            }
            break;
        case smd::MapViewKind::FormatOverview:
            line1 = tr("%1 · 格式概览 · %2 项实测事实").arg(profile_.family_label).arg(profile_.facts.size());
            line2 = profile_.structure_note;
            if (!profile_.window_label.isEmpty())
                line2 += QStringLiteral(" · ") + profile_.window_label;
            break;
        case smd::MapViewKind::Icicle:
        default: {
            const QString mode = mode_ == smd::MapLayoutMode::Balanced ? tr("均衡布局") : tr("字节占比");
            const int shown_layers = std::min(rows_, total_layers_ - first_row_);
            line1 = tr("%1 · %2 · %3 个节点 · %4 · 视窗 %5%")
                        .arg(format_name_, file_span_ > 0 ? smd::FormatBytes(file_span_) : tr("大小未知"))
                        .arg(nodes_.size())
                        .arg(mode)
                        .arg(100.0 / view_.scale, 0, 'f', 1);
            line2 = tr("可见第 %1–%2 层 / 共 %3 层 · %4 条流 · %5 项元数据 · %6")
                        .arg(first_row_ + 1)
                        .arg(first_row_ + shown_layers)
                        .arg(total_layers_)
                        .arg(streams_.size())
                        .arg(metadata_.size())
                        .arg(!stream_rows_.available ? tr("底部不绘制流区间")
                             : stream_rows_.any_truncated ? tr("底部 = 样本分布（样本表截断，右侧为未展开区）")
                                                          : tr("底部 = 样本按当前布局的分布"));
            if (!summary_.isEmpty())
                line2 += QStringLiteral(" · ") + summary_;
            if (profile_.has_precise_window && !profile_.window_label.isEmpty())
                line2 += QStringLiteral(" · ") + profile_.window_label;
            break;
        }
        }
        if (truncated_)
            line1 += tr(" · 已裁剪");
        painter.setPen(QColor(theme::color::kTextPrimary));
        painter.drawText(geom.left, 14, painter.fontMetrics().elidedText(line1, Qt::ElideRight, geom.width));

        // 图例固定在右上角；第二行文字截到它左边，两行文字与色块不能抢同一块像素。
        static const struct LegendItem {
            const char* color;
            const char* label;
        } kLegend[] = {{"#12263A", "元数据 / 关键帧"}, {"#21262D", "载荷"}, {"#3A2A12", "分片"}, {"#5A1E1E", "异常"}};
        const int legend_x = std::max(geom.left, width() - smd::kPlotRight - 320);
        painter.setPen(QColor(theme::color::kTextSecondary));
        const int line2_width = std::max(40, legend_x - geom.left - 12);
        painter.drawText(geom.left, 29, painter.fontMetrics().elidedText(line2, Qt::ElideRight, line2_width));
        int x = legend_x;
        for (const LegendItem& item : kLegend) {
            painter.fillRect(x, 23, 8, 8, QColor(QString::fromLatin1(item.color)));
            painter.setPen(QColor(theme::color::kTextMuted));
            painter.drawText(x + 11, 30, QString::fromUtf8(item.label));
            x += 11 + painter.fontMetrics().horizontalAdvance(QString::fromUtf8(item.label)) + 12;
        }
    }

    void DrawDepthLabels(QPainter& painter, const smd::MapGeometry& geom) {
        painter.setPen(QColor(theme::color::kTextMuted));
        for (int row = 0; row < geom.rows; ++row) {
            const int layer = first_row_ + row;
            if (layer >= total_layers_)
                break;
            painter.drawText(4, geom.top + row * geom.row_height + geom.row_height - 7,
                             QStringLiteral("L%1").arg(layer));
        }
        if (total_layers_ > rows_ + first_row_)
            painter.drawText(4, height() - smd::kAxisHeight - 4, tr("↓ 更多"));
    }

    void DrawBandLabels(QPainter& painter) {
        if (bands_.tracks.empty())
            return;
        const QRect plot = PlotRect();
        const size_t shown = std::min<size_t>(bands_.tracks.size(), 8);
        const int row_h = std::max(20, plot.height() / static_cast<int>(shown));
        painter.setPen(QColor(theme::color::kTextSecondary));
        for (size_t i = 0; i < shown; ++i) {
            const QString label = painter.fontMetrics().elidedText(bands_.tracks[i].label, Qt::ElideRight, 34);
            painter.drawText(2, plot.top() + static_cast<int>(i) * row_h + 12, label);
        }
    }

    // ❗ 旧版把 N 条流按 geom.width / N 等分横铺，画在与上方结构盒共用的字节轴上 ——
    // 读起来就是「video 占前半文件、audio 占后半文件」。流是逻辑轨道，根本没有
    // offset/size；真实位置在 Mp4Sample::offset 里（交错存放于 mdat）。
    // 现在密度条由 BuildStreamByteVisuals 统一生成 MapVisual，这里只画行标签与口径。
    void DrawStreamRowLabels(QPainter& painter, const smd::MapGeometry& geom, int band_top) {
        if (!stream_rows_.available) {
            painter.setPen(QColor(theme::color::kTextMuted));
            const QString note = tr("%1 条流是逻辑轨道，不带字节区间 → 不绘制：%2")
                                     .arg(static_cast<int>(streams_.size()))
                                     .arg(stream_rows_.coverage_note);
            painter.drawText(geom.left, band_top + 13,
                             painter.fontMetrics().elidedText(note, Qt::ElideRight, geom.width));
            return;
        }
        const int shown = std::min<int>(smd::kStreamMaxRows, static_cast<int>(stream_rows_.rows.size()));
        for (int r = 0; r < shown; ++r) {
            const smd::StreamByteRow& row = stream_rows_.rows[static_cast<size_t>(r)];
            const int y = band_top + r * smd::kStreamRowHeight;
            painter.fillRect(2, y + 2, 4, smd::kStreamRowHeight - 5, smd::StreamColor(row.type));
            painter.setPen(QColor(theme::color::kTextSecondary));
            painter.drawText(9, y + smd::kStreamRowHeight - 3, QStringLiteral("#%1").arg(row.stream_index));
        }
        painter.setPen(QColor(theme::color::kTextMuted));
        if (stream_rows_.rows.size() > static_cast<size_t>(shown))
            painter.drawText(geom.left + 4, band_top + smd::kStreamHeight - 3,
                             tr("另有 %1 条流未在字节轴展开").arg(static_cast<int>(stream_rows_.rows.size() - shown)));
        // 口径必须写清：横轴是字节位置不是时间；柱高按全轨共用峰值归一化（小轨天然矮）；
        // 斜纹区是未展开的样本，不是「这条流没有数据」。
        const QString note = stream_rows_.truncation_note.isEmpty()
                                 ? tr("横轴=字节位置(布局投影)，非时间轴 · 柱高=桶字节(全轨共用峰值)")
                                 : tr("斜纹=未展开(%1) · 横轴为字节位置，非时间轴").arg(stream_rows_.truncation_note);
        const QString elided = painter.fontMetrics().elidedText(note, Qt::ElideRight, geom.width - 8);
        painter.drawText(geom.left + geom.width - painter.fontMetrics().horizontalAdvance(elided) - 4,
                         band_top + smd::kStreamHeight - 3, elided);
    }

    void DrawAxis(QPainter& painter, const smd::MapGeometry& geom) {
        painter.setPen(QColor(theme::color::kTextMuted));
        const int axis_y = height() - 8;
        const bool byte_axis =
            view_kind_ == smd::MapViewKind::PacketBands && bands_.axis == smd::MapBandAxis::ByteOffset;
        const bool overview = view_kind_ == smd::MapViewKind::FormatOverview;
        for (int tick = 0; tick <= 4; ++tick) {
            if (overview)
                break; // 事实卡片没有横轴，画刻度只会让人误以为它是一张图
            const int x = geom.left + geom.width * tick / 4;
            QString label;
            if (byte_axis) {
                label = smd::FormatBytes(static_cast<quint64>(bands_.axis_max * tick / 4.0));
            } else if (view_kind_ == smd::MapViewKind::PacketBands) {
                label = tr("%1 s").arg(bands_.duration_seconds * tick / 4.0, 0, 'f', 1);
            } else {
                const double file_x = view_.FromScreen(tick / 4.0);
                label = file_span_ > 0 ? smd::FormatBytes(static_cast<quint64>(std::max(0.0, file_x * file_span_)))
                                       : QStringLiteral("%1%").arg(tick * 25);
            }
            painter.drawText(x + 2, axis_y, label);
        }
        const QString hint = overview ? tr("点击事实卡片查看口径 · 所有字段均来自解析结果")
                             : byte_axis ? tr("横轴 = 文件字节偏移（不是时间）· 点击字节桶查看详情")
                             : view_kind_ == smd::MapViewKind::PacketBands
                                 ? tr("点击时间桶查看详情 · 双击结构图节点跳树页")
                                 : tr("横轴 = 文件跨度（均衡布局已压缩）· 滚轮缩放 · 拖拽平移 · 双击跳树页");
        painter.drawText(width() - smd::kPlotRight - painter.fontMetrics().horizontalAdvance(hint), axis_y, hint);
    }

    void DrawHoverCard(QPainter& painter) {
        if (hovered_ < 0 || hovered_ >= static_cast<int>(Visuals().size()))
            return;
        const smd::MapVisual& visual = Visuals()[hovered_];
        QStringList lines;
        if (visual.node_index >= 0 && visual.node_index < static_cast<int>(nodes_.size())) {
            const smd::MapNode& node = nodes_[visual.node_index];
            const double pct = file_span_ > 0 ? 100.0 * node.size / file_span_ : 0.0;
            lines << node.name << tr("偏移 0x%1 · 大小 %2").arg(node.offset, 0, 16).arg(smd::FormatBytes(node.size))
                  << tr("L%1 · 占文件 %2%").arg(node.depth).arg(pct, 0, 'f', 2);
        } else if (visual.track_index >= 0 && visual.track_index < static_cast<int>(bands_.tracks.size())) {
            const smd::PacketBandTrack& track = bands_.tracks[visual.track_index];
            if (visual.bin_index >= 0 && visual.bin_index < static_cast<int>(track.bins.size())) {
                const smd::PacketBandBin& bin = track.bins[visual.bin_index];
                if (bands_.axis == smd::MapBandAxis::ByteOffset)
                    lines << tr("%1 – %2").arg(smd::FormatBytes(static_cast<quint64>(bin.t0)),
                                               smd::FormatBytes(static_cast<quint64>(bin.t1)));
                else
                    lines << tr("%1 – %2 s").arg(bin.t0, 0, 'f', 2).arg(bin.t1, 0, 'f', 2);
                lines << tr("%1 条 · %2").arg(bin.count).arg(smd::FormatBytes(bin.bytes))
                      << tr("关键帧 %1").arg(bin.keyframes);
            }
        } else if (visual.fact_index >= 0 && visual.fact_index < static_cast<int>(profile_.facts.size())) {
            const smd::FormatFact& fact = profile_.facts[static_cast<size_t>(visual.fact_index)];
            lines << fact.label << fact.value;
            if (!fact.note.isEmpty())
                lines << fact.note;
        } else if (visual.stream_row >= 0 && visual.stream_row < static_cast<int>(stream_rows_.rows.size())) {
            const smd::StreamByteRow& row = stream_rows_.rows[static_cast<size_t>(visual.stream_row)];
            lines << row.label << stream_rows_.source;
            if (row.unexpanded_samples > 0)
                lines << tr("声明 %1 个样本 / 已展开 %2 个，右侧 %3 个未展开")
                             .arg(row.declared_samples)
                             .arg(row.expanded_samples)
                             .arg(row.unexpanded_samples);
            if (visual.stream_bin >= 0 && visual.stream_bin < static_cast<int>(row.bins.size())) {
                const smd::StreamByteBin& bin = row.bins[static_cast<size_t>(visual.stream_bin)];
                lines << tr("%1 个样本 · %2 · 关键帧 %3")
                             .arg(bin.count)
                             .arg(smd::FormatBytes(bin.bytes))
                             .arg(bin.keyframes);
            }
        }
        if (lines.isEmpty())
            return;
        int text_w = 0;
        for (const QString& line : lines)
            text_w = std::max(text_w, painter.fontMetrics().horizontalAdvance(line));
        const int w = std::min(width() - 16, text_w + 16);
        const int h = 12 + lines.size() * 14;
        const QRect anchor = visual.rect;
        const int x = std::max(4, std::min(width() - w - 4, anchor.x() + 4));
        const int y = std::max(4, std::min(height() - h - 4, anchor.bottom() + 6));
        painter.fillRect(QRect(x, y, w, h), QColor(1, 6, 14, 238));
        painter.setPen(QColor(theme::color::kBorderLight));
        painter.drawRect(QRect(x, y, w, h).adjusted(0, 0, -1, -1));
        for (int i = 0; i < lines.size(); ++i) {
            painter.setPen(QColor(i == 0 ? theme::color::kTextPrimary : theme::color::kTextSecondary));
            painter.drawText(x + 8, y + 16 + i * 14,
                             painter.fontMetrics().elidedText(lines[i], Qt::ElideRight, w - 16));
        }
    }

    std::vector<smd::MapNode> nodes_;
    std::vector<model::ContainerStreamInfo> streams_;
    std::map<std::string, std::string> metadata_;
    smd::PacketBandData bands_;
    smd::StreamByteRows stream_rows_;
    smd::FormatProfile profile_;
    QString format_name_;
    QString summary_;
    bool truncated_ = false;
    bool enabled_ = true;
    quint64 file_span_ = 0;
    smd::MapLayoutMode mode_ = smd::MapLayoutMode::Balanced;
    smd::MapViewKind view_kind_ = smd::MapViewKind::Icicle;
    smd::MapViewport view_;
    int rows_ = 1;
    int total_layers_ = 1;
    int first_row_ = 0;
    int selected_ = -1;
    int selected_bin_track_ = -1;
    int selected_bin_index_ = -1;
    int selected_fact_ = -1;
    int selected_stream_row_ = -1;
    int selected_stream_bin_ = -1;
    int hovered_ = -1;
    QVector<int> hits_;

    mutable std::vector<smd::MapVisual> visuals_cache_;
    mutable bool visuals_dirty_ = true;
    bool pressed_ = false;
    bool dragging_ = false;
    QPoint press_pos_;
    QPoint last_pos_;
    double drag_y_accum_ = 0.0;
};

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------
FileStructureMapPage::FileStructureMapPage(bool feature_checked, QWidget* parent) : QWidget(parent) {
    SetupUi();
    feature_toggle_->setChecked(feature_checked);
    Refresh();
}

void FileStructureMapPage::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 4);
    layout->setSpacing(2);

    // 页头只有一行：[标题] … [启用分析] [导出]。所有画布参数都放第二行工具条，
    // 免得页头出现第二个分析入口，也免得挤窄标题。
    QWidget* title_row = new QWidget(this);
    QHBoxLayout* header = new QHBoxLayout(title_row);
    header->setContentsMargins(0, 0, 0, 0);
    title_label_ = new QLabel(tr("文件结构图谱"), title_row);
    title_label_->setObjectName(QStringLiteral("PageTitle"));
    title_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    header->addWidget(title_label_);
    header->addStretch();

    feature_toggle_ = new QCheckBox(tr("启用分析"), title_row);
    feature_toggle_->setToolTip(
        tr("控制本页是否绘制结构图谱。容器结构在打开文件时已自动解析，关闭开关不会取消分析或丢弃结果。"));
    connect(feature_toggle_, &QCheckBox::toggled, this, [this](bool) { Refresh(); });
    header->addWidget(feature_toggle_);
    header->addWidget(
        MakeExportMenuButton(title_row, {{tr("结构节点 CSV…"), [this]() { OnExportCsv(); },
                                          tr("按结构树顺序导出全部节点的名称 / 类型 / 偏移 / 大小 / 层级")},
                                         {tr("结构数据 JSON…"), [this]() { OnExportJson(); },
                                          tr("导出结构树、流、元数据与完整性标记（分五维，不合成一个完成态）")},
                                         {tr("当前视图 SVG…"), [this]() { OnExportSvg(); },
                                          tr("把当前视图（三种视图均支持）导出为矢量图，导出即所见")}}));
    layout->addWidget(title_row);

    status_label_ = new QLabel(tr("打开媒体文件后将自动生成结构图谱"), this);
    status_label_->setObjectName(QStringLiteral("SectionLabel"));
    status_label_->setWordWrap(true);
    status_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(status_label_);

    // 完整性条：分析成功 / 结论完整 / 扫描窗口 / 抽样 / 裁剪 / 错误原因逐条分色展示。
    // 绝不显示"完成"两个字 —— 半份结果配上"完成"就是误导。
    coverage_label_ = new QLabel(this);
    coverage_label_->setObjectName(QStringLiteral("StructureMapCoverage"));
    coverage_label_->setWordWrap(true);
    coverage_label_->setTextFormat(Qt::RichText);
    coverage_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(coverage_label_);

    // 第二行工具条：这里不能出现第二个「启用分析」/「开始分析」，也不能出现第二个
    // QCheckBox（页头测试按控件形态数入口）。所以全部用下拉框 / 输入框 / 工具按钮。
    QWidget* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("StructureMapToolbar"));
    QHBoxLayout* tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(2, 0, 2, 0);
    tools->setSpacing(4);

    view_combo_ = new QComboBox(toolbar);
    view_combo_->setObjectName(QStringLiteral("StructureMapViewKind"));
    view_combo_->addItem(tr("结构图（层级带）"));
    view_combo_->addItem(tr("字节树图（面积）"));
    view_combo_->addItem(tr("时间轴包带"));
    view_combo_->addItem(tr("格式概览（逐格式事实）"));
    view_combo_->setToolTip(tr("同一份结构结果换四种口径看：层级深度 / 面积 / 包带 / 逐格式事实；"
                               "不适用的视图会置灰，理由见「格式概览」"));
    connect(view_combo_, &QComboBox::currentIndexChanged, this,
            [this](int index) { SetViewKind(static_cast<smd::MapViewKind>(index)); });
    tools->addWidget(view_combo_);

    layout_combo_ = new QComboBox(toolbar);
    layout_combo_->setObjectName(QStringLiteral("StructureMapLayoutMode"));
    layout_combo_->addItem(tr("均衡布局"));
    layout_combo_->addItem(tr("字节占比"));
    layout_combo_->setToolTip(tr("均衡布局压缩字节占比让元数据盒子可见；字节占比按真实偏移与大小还原"));
    layout_combo_->setCurrentIndex(static_cast<int>(mode_));
    connect(layout_combo_, &QComboBox::currentIndexChanged, this,
            [this](int index) { SetLayoutMode(static_cast<smd::MapLayoutMode>(index)); });
    tools->addWidget(layout_combo_);

    depth_combo_ = new QComboBox(toolbar);
    depth_combo_->setObjectName(QStringLiteral("StructureMapDepth"));
    depth_combo_->addItem(tr("全部层级"));
    connect(depth_combo_, &QComboBox::currentIndexChanged, this,
            [this](int index) { SetVisibleLayers(index <= 0 ? 0 : index); });
    tools->addWidget(depth_combo_);

    search_edit_ = new QLineEdit(toolbar);
    search_edit_->setObjectName(QStringLiteral("StructureMapSearch"));
    search_edit_->setPlaceholderText(tr("搜索节点名称 / 类型 / 值"));
    search_edit_->setClearButtonEnabled(true);
    search_edit_->setMinimumWidth(150);
    connect(search_edit_, &QLineEdit::textChanged, this, [this](const QString& text) { SetSearchQuery(text); });
    tools->addWidget(search_edit_);

    hit_label_ = new QLabel(tr("输入关键字搜索节点"), toolbar);
    hit_label_->setObjectName(QStringLiteral("StructureMapHitCount"));
    hit_label_->setMinimumWidth(90);
    tools->addWidget(hit_label_);

    auto add_tool = [&](const QString& text, const QString& tip, const std::function<void()>& action) {
        QToolButton* button = new QToolButton(toolbar);
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this, action);
        tools->addWidget(button);
        return button;
    };
    add_tool(QStringLiteral("◀"), tr("上一个命中"), [this]() {
        if (hits_.isEmpty())
            return;
        hit_cursor_ = (hit_cursor_ < 0 ? 0 : (hit_cursor_ - 1 + hits_.size()) % hits_.size());
        SelectNode(hits_[hit_cursor_]);
        FocusNode(hits_[hit_cursor_]);
        UpdateHitLabel();
    });
    add_tool(QStringLiteral("▶"), tr("下一个命中"), [this]() {
        if (hits_.isEmpty())
            return;
        hit_cursor_ = (hit_cursor_ + 1) % hits_.size();
        SelectNode(hits_[hit_cursor_]);
        FocusNode(hits_[hit_cursor_]);
        UpdateHitLabel();
    });
    add_tool(QStringLiteral("＋"), tr("放大当前字节区间"),
             [this]() { canvas_->SetView(smd::ZoomAt(canvas_->view(), canvas_->view().FromScreen(0.5), 1.4)); });
    add_tool(QStringLiteral("－"), tr("缩小当前字节区间"),
             [this]() { canvas_->SetView(smd::ZoomAt(canvas_->view(), canvas_->view().FromScreen(0.5), 1.0 / 1.4)); });
    add_tool(tr("聚焦选中"), tr("把选中节点的字节区间铺满画布（Shift+双击同效）"), [this]() { FocusNode(selected_); });
    add_tool(tr("定位结构树"), tr("切到「文件结构」页并选中同名节点（双击节点同效）"),
             [this]() { RequestJumpToTree(); });
    add_tool(tr("适应窗口"), tr("复位缩放、平移与垂直滚动"), [this]() { ResetView(); });
    tools->addStretch();
    layout->addWidget(toolbar);

    QWidget* content = new QWidget(this);
    QHBoxLayout* content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(4);
    canvas_ = new StructureMapCanvas(content);
    canvas_->setObjectName(QStringLiteral("StructureMapCanvas"));
    content_layout->addWidget(canvas_, 1);
    canvas_->on_select = [this](int index) { SelectNode(index); };
    canvas_->on_bin_select = [this](int track, int bin) {
        selected_bin_track_ = track;
        selected_bin_index_ = bin;
        canvas_->SetBinSelection(track, bin);
        UpdateDetail();
    };
    canvas_->on_fact_select = [this](int index) {
        selected_fact_ = index;
        canvas_->SetFactSelection(index);
        UpdateDetail();
    };
    canvas_->on_stream_bin = [this](int row, int bin) { SelectStreamBin(row, bin); };
    canvas_->on_focus = [this](int index) {
        if (index >= 0)
            FocusNode(index);
        else
            ResetView();
    };
    canvas_->on_jump = [this](int index) {
        SelectNode(index);
        RequestJumpToTree();
    };

    detail_label_ = new QLabel(content);
    detail_label_->setObjectName(QStringLiteral("StructureMapDetail"));
    detail_label_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    detail_label_->setWordWrap(true);
    detail_label_->setTextFormat(Qt::RichText);
    detail_label_->setFrameShape(QFrame::StyledPanel);
    detail_label_->setFixedWidth(250);
    detail_label_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    content_layout->addWidget(detail_label_);
    layout->addWidget(content, 1);

    QLabel* boundary = new QLabel(
        tr("底部色带是样本真实偏移按当前布局投影后的位置：音视频样本交错存放于 mdat（在 ftyp / moov 之后），"
           "流本身不是字节区间，两份流也不各占半份文件。"
           "双击图中节点跳「文件结构」树页；Shift+双击聚焦该区间。包带来自已持久化的样本表 / 分片清单 / EBML 块摘要，"
           "逐包数据在 demux 时已释放；EBML 块只能按字节偏移分桶，不是时间轴。覆盖度以完整性条与「格式概览」为准。"),
        this);
    boundary->setObjectName(QStringLiteral("StructureMapBoundaryNote"));
    boundary->setWordWrap(true);
    boundary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(boundary);
    UpdateDetail();
    UpdateCoverageUi();
}

void FileStructureMapPage::SetResult(const model::ContainerStructureResult& result) {
    result_ = result;
    title_label_->setText(result_.valid ? QString::fromStdString(result_.format_name) + tr(" 结构图谱")
                                        : tr("文件结构图谱"));
    selected_ = -1;
    selected_bin_track_ = -1;
    selected_bin_index_ = -1;
    selected_fact_ = -1;
    selected_stream_row_ = -1;
    selected_stream_bin_ = -1;
    hit_cursor_ = -1;
    Refresh();
}

void FileStructureMapPage::SetError(const QString& message) {
    result_ = model::ContainerStructureResult{};
    title_label_->setText(tr("文件结构图谱"));
    Refresh();
    status_label_->setText(tr("结构分析失败：%1").arg(message.isEmpty() ? tr("无法分析该格式") : message));
}

void FileStructureMapPage::Clear() {
    result_ = model::ContainerStructureResult{};
    title_label_->setText(tr("文件结构图谱"));
    SetSearchQuery(QString());
    selected_ = -1;
    selected_bin_track_ = -1;
    selected_bin_index_ = -1;
    selected_fact_ = -1;
    selected_stream_row_ = -1;
    selected_stream_bin_ = -1;
    hit_cursor_ = -1;
    Refresh();
    status_label_->setText(tr("打开媒体文件后将自动生成结构图谱"));
}

int FileStructureMapPage::NodeCount() const {
    return static_cast<int>(nodes_.size());
}

bool FileStructureMapPage::HasData() const {
    return result_.valid && !nodes_.empty();
}

bool FileStructureMapPage::DrawingEnabled() const {
    return feature_toggle_ != nullptr && feature_toggle_->isChecked();
}

QString FileStructureMapPage::StatusText() const {
    return status_label_->text();
}

QString FileStructureMapPage::CoverageText() const {
    return coverage_label_->text();
}

smd::MapCoverage FileStructureMapPage::Coverage() const {
    return coverage_;
}

smd::PacketBandData FileStructureMapPage::Bands() const {
    return bands_;
}

smd::FormatProfile FileStructureMapPage::Profile() const {
    return profile_;
}

int FileStructureMapPage::SelectedFact() const {
    return selected_fact_;
}

bool FileStructureMapPage::ViewSupported(smd::MapViewKind kind) const {
    return smd::ViewSupported(profile_, kind);
}

QString FileStructureMapPage::ViewUnsupportedNote() const {
    return smd::ViewUnsupportedReason(profile_, smd::MapViewKind::Treemap);
}

int FileStructureMapPage::VisualCount() const {
    return canvas_->VisualCount();
}

smd::StreamByteRows FileStructureMapPage::StreamRows() const {
    return stream_rows_;
}

double FileStructureMapPage::ViewScale() const {
    return canvas_->view().scale;
}

smd::MapLayoutMode FileStructureMapPage::LayoutMode() const {
    return mode_;
}

smd::MapViewKind FileStructureMapPage::ViewKind() const {
    return view_kind_;
}

int FileStructureMapPage::SelectedNode() const {
    return selected_;
}

int FileStructureMapPage::HitCount() const {
    return hits_.size();
}

QString FileStructureMapPage::DetailText() const {
    return detail_label_->text();
}

QString FileStructureMapPage::SearchQuery() const {
    return search_edit_->text();
}

void FileStructureMapPage::RebuildNodes() {
    // 格式画像先算：它决定哪些视图 / 布局口径对本格式成立，也决定窗口叠加画在哪。
    profile_ = smd::BuildFormatProfile(result_);
    tree_depth_ = smd::TreeMaxDepth(result_);
    file_span_ = smd::FileSpan(result_);
    depth_truncated_ = tree_depth_ > smd::kMaxTreeDepth;

    const int plot_width = std::max(1, canvas_->width() - smd::kDepthLabelWidth - smd::kPlotRight);
    const smd::MapLod lod = smd::ComputeLod(plot_width, canvas_->height(), tree_depth_);
    int rows = lod.rows;
    int total_layers = std::min(tree_depth_ + 1, smd::kMaxTreeDepth + 1);
    if (visible_layers_ > 0) {
        rows = std::min(visible_layers_, rows);
        total_layers = std::min(visible_layers_, total_layers);
    }
    rows = std::max(1, rows);
    total_layers = std::max(rows, total_layers);

    nodes_ = smd::CollectNodes(result_, smd::kMaxTreeDepth, lod.max_nodes, mode_);
    // 多要一个节点来判断"是否真的还有没画出来的"，别把刚好等于预算误报成裁剪。
    nodes_truncated_ = smd::CollectNodes(result_, smd::kMaxTreeDepth, lod.max_nodes + 1, mode_).size() >
                       static_cast<size_t>(lod.max_nodes);
    // 换了布局 / 层级后节点集合可能变了，旧命中索引不能留着指到别的盒子上。
    hits_ = smd::MatchNodes(nodes_, SearchQuery());
    hit_cursor_ = -1;
    RebuildDepthCombo();
    UpdateHitLabel();
    UpdateCoverageUi();

    // 包带桶数跟画布宽度走：窄窗口少画几个桶，但每桶仍覆盖同样长的时间窗。
    const int bins = std::max(16, std::min(160, plot_width / 6));
    bands_ = smd::BuildPacketBands(result_, bins);
    // 分桶用与上方结构盒相同的布局投影；没有样本偏移时一行都不画。
    // 切「字节占比 / 均衡布局」时 nodes_ 会重建，这里必须跟着重算，否则色带会停在旧口径。
    const smd::MapLayoutProjection projection = smd::BuildLayoutProjection(nodes_);
    stream_rows_ = smd::BuildStreamByteRows(result_, bins, projection);
    coverage_ = smd::BuildCoverage(result_, nodes_truncated_, depth_truncated_);
    ApplyViewAvailability();

    canvas_->SetData(
        nodes_, result_.streams, result_.valid ? QString::fromStdString(result_.format_name) : tr("文件结构图谱"),
        QString::fromStdString(result_.summary), result_.metadata, nodes_truncated_ || depth_truncated_, file_span_,
        DrawingEnabled(), mode_, view_kind_, bands_, rows, total_layers, profile_, stream_rows_);
    canvas_->SetSelection(selected_);
    canvas_->SetBinSelection(selected_bin_track_, selected_bin_index_);
    canvas_->SetFactSelection(selected_fact_);
    canvas_->SetStreamSelection(selected_stream_row_, selected_stream_bin_);
    canvas_->SetHits(hits_);
}

void FileStructureMapPage::RebuildDepthCombo() {
    if (!depth_combo_)
        return;
    const int layers = std::min(std::max(1, tree_depth_ + 1), 8);
    // 只更新项数与文案，不每次 SetResult 都毁掉重建 —— combo 在布局测试里也算可见控件。
    QSignalBlocker blocker(depth_combo_);
    while (depth_combo_->count() < layers + 1)
        depth_combo_->addItem(QString());
    while (depth_combo_->count() > layers + 1)
        depth_combo_->removeItem(depth_combo_->count() - 1);
    depth_combo_->setItemText(0, tr("全部层级"));
    for (int i = 1; i <= layers; ++i)
        depth_combo_->setItemText(i, tr("%1 层").arg(i));
    const int index = visible_layers_ > 0 ? std::min(visible_layers_, layers) : 0;
    depth_combo_->setCurrentIndex(std::min(index, depth_combo_->count() - 1));
}

void FileStructureMapPage::ApplyViewAvailability() {
    if (!view_combo_)
        return;
    // 不适用的视图直接置灰，而不是让用户在下拉里选到一张失真 / 空白的图。
    const bool treemap_ok = ViewSupported(smd::MapViewKind::Treemap);
    if (auto* model = qobject_cast<QStandardItemModel*>(view_combo_->model())) {
        if (QStandardItem* item = model->item(static_cast<int>(smd::MapViewKind::Treemap)))
            item->setEnabled(treemap_ok);
    }
    if (!treemap_ok && view_kind_ == smd::MapViewKind::Treemap)
        view_kind_ = smd::MapViewKind::Icicle;
    if (view_combo_->currentIndex() != static_cast<int>(view_kind_)) {
        QSignalBlocker blocker(view_combo_);
        view_combo_->setCurrentIndex(static_cast<int>(view_kind_));
    }

    // 没有可比较的 size 时「字节占比」也是假的（会退化成等宽，却挂着字节的名字）。
    if (layout_combo_) {
        const bool bytes_ok = profile_.has_byte_layout;
        if (auto* model = qobject_cast<QStandardItemModel*>(layout_combo_->model())) {
            if (QStandardItem* item = model->item(static_cast<int>(smd::MapLayoutMode::ByteProportional)))
                item->setEnabled(bytes_ok);
        }
        if (!bytes_ok && mode_ == smd::MapLayoutMode::ByteProportional) {
            mode_ = smd::MapLayoutMode::Balanced;
            QSignalBlocker blocker(layout_combo_);
            layout_combo_->setCurrentIndex(static_cast<int>(mode_));
        }
    }
}

void FileStructureMapPage::Refresh() {
    RebuildNodes();

    if (!result_.valid) {
        status_label_->setText(tr("结构分析失败：%1")
                                   .arg(result_.error_message.empty() ? tr("无法分析该格式")
                                                                      : QString::fromStdString(result_.error_message)));
        UpdateDetail();
        UpdateCoverageUi();
        return;
    }
    if (nodes_.empty()) {
        status_label_->setText(tr("结构树为空：本次解析没有产出可绘制的节点"));
        UpdateDetail();
        UpdateCoverageUi();
        return;
    }
    QString status = tr("%1 · %2 个顶层元素 · %3 条流 · 图上 %4 个节点")
                         .arg(QString::fromStdString(result_.format_name))
                         .arg(result_.element_tree.size())
                         .arg(result_.streams.size())
                         .arg(nodes_.size());
    if (nodes_truncated_)
        status += tr(" · 已按预算裁剪");
    if (depth_truncated_)
        status += tr(" · 深度已裁剪");
    status += tr(" · 结构骨架（载荷 / 逐包不展开）");
    status_label_->setText(status);
    UpdateDetail();
    UpdateCoverageUi();
}

void FileStructureMapPage::UpdateCoverageUi() {
    if (!coverage_label_)
        return;
    coverage_ = smd::BuildCoverage(result_, nodes_truncated_, depth_truncated_);
    if (coverage_.facts.isEmpty()) {
        coverage_label_->setText(QString());
        return;
    }
    QString html = QStringLiteral("<span style='color:%1'><b>完整性</b> </span>")
                       .arg(QString::fromLatin1(theme::color::kTextPrimary));
    for (int i = 0; i < coverage_.facts.size(); ++i) {
        const QString& fact = coverage_.facts[i];
        const bool bad = fact.contains(QStringLiteral("失败")) || fact.contains(QStringLiteral("错误"));
        const bool caution = fact.contains(QStringLiteral("裁剪")) || fact.contains(QStringLiteral("截断")) ||
                             fact.contains(QStringLiteral("窗口")) || fact.contains(QStringLiteral("抽查")) ||
                             fact.contains(QStringLiteral("无 size")) || fact.contains(QStringLiteral("抽样"));
        const QString color = bad       ? QString::fromLatin1(theme::color::kDanger)
                              : caution ? QString::fromLatin1(theme::color::kWarning)
                                        : QString::fromLatin1(theme::color::kTextSecondary);
        html += QStringLiteral("<span style='color:%1'>%2</span>").arg(color, HtmlEscape(fact));
        if (i + 1 < coverage_.facts.size())
            html +=
                QStringLiteral(" <span style='color:%1'>·</span> ").arg(QString::fromLatin1(theme::color::kTextMuted));
    }
    if (coverage_.conclusion_complete)
        html += QStringLiteral(" <span style='color:%1'>[结构骨架完整]</span>")
                    .arg(QString::fromLatin1(theme::color::kAccent));
    coverage_label_->setText(html);
}

void FileStructureMapPage::UpdateDetail() {
    if (!detail_label_)
        return;
    if (selected_fact_ >= 0 && view_kind_ == smd::MapViewKind::FormatOverview) {
        const QString text = smd::DescribeFormatFact(profile_, selected_fact_);
        if (!text.isEmpty()) {
            detail_label_->setText(text);
            return;
        }
    }
    if (selected_bin_track_ >= 0 && view_kind_ == smd::MapViewKind::PacketBands) {
        const QString text = smd::DescribeBandBin(bands_, selected_bin_track_, selected_bin_index_);
        if (!text.isEmpty()) {
            detail_label_->setText(text);
            return;
        }
    }
    if (selected_stream_row_ >= 0 && view_kind_ == smd::MapViewKind::Icicle) {
        const QString text = smd::DescribeStreamByteBin(stream_rows_, selected_stream_row_, selected_stream_bin_);
        if (!text.isEmpty()) {
            detail_label_->setText(text);
            return;
        }
    }
    if (selected_ >= 0 && selected_ < static_cast<int>(nodes_.size())) {
        const QString path = smd::BuildNodePath(nodes_, selected_);
        detail_label_->setText(tr("<div style='color:%1'><b>节点详情</b></div>"
                                  "<div style='color:%2;margin-bottom:6px'>路径：%3</div>")
                                   .arg(QString::fromLatin1(theme::color::kTextPrimary),
                                        QString::fromLatin1(theme::color::kTextSecondary), HtmlEscape(path)) +
                               smd::DescribeNode(nodes_[selected_], file_span_));
        return;
    }
    if (!result_.valid || nodes_.empty()) {
        detail_label_->setText(tr("<div style='color:%1'><b>节点详情</b></div><div style='color:%2'>"
                                  "尚无结构数据。打开文件后，单击图中任意节点可查看类型、偏移、大小与字节区间。</div>")
                                   .arg(QString::fromLatin1(theme::color::kTextPrimary),
                                        QString::fromLatin1(theme::color::kTextSecondary)));
        return;
    }
    QString meta_lines;
    int shown = 0;
    for (const auto& entry : result_.metadata) {
        if (shown++ >= 6)
            break;
        meta_lines +=
            tr("<div style='color:%1'>%2: %3</div>")
                .arg(QString::fromLatin1(theme::color::kTextSecondary), HtmlEscape(QString::fromStdString(entry.first)),
                     HtmlEscape(QString::fromStdString(entry.second)));
    }
    detail_label_->setText(tr("<div style='color:%1'><b>文件概览</b></div>"
                              "<div style='color:%2;margin-bottom:6px'>%3 · %4 个顶层元素 · %5 条流 · %6 层</div>"
                              "<div style='color:%2'>单击节点查看详情<br>双击跳结构树页<br>"
                              "Shift+双击聚焦区间<br>切到包带视图看时间轴密度</div>"
                              "<hr><div style='color:%1'><b>元数据</b></div>")
                               .arg(QString::fromLatin1(theme::color::kTextPrimary),
                                    QString::fromLatin1(theme::color::kTextSecondary),
                                    HtmlEscape(QString::fromStdString(result_.format_name)))
                               .arg(result_.element_tree.size())
                               .arg(result_.streams.size())
                               .arg(tree_depth_ + 1) +
                           meta_lines);
}

void FileStructureMapPage::UpdateHitLabel() {
    if (!hit_label_)
        return;
    const QString query = search_edit_ ? search_edit_->text().trimmed() : QString();
    if (query.isEmpty()) {
        hit_label_->setText(tr("输入关键字搜索节点"));
        return;
    }
    if (hits_.isEmpty()) {
        hit_label_->setText(tr("0 个命中"));
        return;
    }
    hit_label_->setText(tr("%1 个命中 · 第 %2 个").arg(hits_.size()).arg(hit_cursor_ < 0 ? 1 : hit_cursor_ + 1));
}

void FileStructureMapPage::SetSearchQuery(const QString& query) {
    if (search_edit_ && search_edit_->text() != query) {
        QSignalBlocker blocker(search_edit_);
        search_edit_->setText(query);
    }
    hits_ = smd::MatchNodes(nodes_, query);
    hit_cursor_ = -1;
    if (canvas_)
        canvas_->SetHits(hits_);
    UpdateHitLabel();
}

void FileStructureMapPage::SelectNode(int index) {
    if (index >= static_cast<int>(nodes_.size()))
        index = -1;
    selected_ = index;
    if (index >= 0) {
        selected_bin_track_ = -1;
        selected_bin_index_ = -1;
        selected_fact_ = -1;
        selected_stream_row_ = -1;
        selected_stream_bin_ = -1;
    }
    if (canvas_) {
        canvas_->SetSelection(index);
        canvas_->SetBinSelection(selected_bin_track_, selected_bin_index_);
        canvas_->SetFactSelection(selected_fact_);
        canvas_->SetStreamSelection(selected_stream_row_, selected_stream_bin_);
    }
    UpdateDetail();
}

void FileStructureMapPage::SelectStreamBin(int row, int bin) {
    if (!stream_rows_.available || row < 0 || row >= static_cast<int>(stream_rows_.rows.size()) || bin < 0 ||
        bin >= stream_rows_.bins) {
        row = -1;
        bin = -1;
    }
    selected_stream_row_ = row;
    selected_stream_bin_ = bin;
    if (row >= 0) {
        selected_ = -1;
        selected_bin_track_ = -1;
        selected_bin_index_ = -1;
        selected_fact_ = -1;
    }
    if (canvas_) {
        canvas_->SetSelection(selected_);
        canvas_->SetBinSelection(selected_bin_track_, selected_bin_index_);
        canvas_->SetFactSelection(selected_fact_);
        canvas_->SetStreamSelection(row, bin);
    }
    UpdateDetail();
}

void FileStructureMapPage::SelectFact(int index) {
    if (index >= static_cast<int>(profile_.facts.size()))
        index = -1;
    selected_fact_ = index;
    if (index >= 0) {
        selected_ = -1;
        selected_bin_track_ = -1;
        selected_bin_index_ = -1;
    }
    if (canvas_) {
        canvas_->SetSelection(selected_);
        canvas_->SetBinSelection(selected_bin_track_, selected_bin_index_);
        canvas_->SetFactSelection(index);
    }
    UpdateDetail();
}

void FileStructureMapPage::FocusNode(int index) {
    if (!canvas_ || index < 0 || index >= static_cast<int>(nodes_.size()))
        return;
    canvas_->SetView(smd::FitInterval(canvas_->view(), nodes_[index].x0, nodes_[index].x1));
}

void FileStructureMapPage::ResetView() {
    if (!canvas_)
        return;
    canvas_->SetView(smd::MapViewport{});
    canvas_->SetFirstRow(0);
}

void FileStructureMapPage::SetLayoutMode(smd::MapLayoutMode mode) {
    mode_ = mode;
    if (layout_combo_ && layout_combo_->currentIndex() != static_cast<int>(mode)) {
        QSignalBlocker blocker(layout_combo_);
        layout_combo_->setCurrentIndex(static_cast<int>(mode));
    }
    RebuildNodes();
}

void FileStructureMapPage::SetViewKind(smd::MapViewKind kind) {
    // 页面接口也可能被工具条以外的地方调用（测试 / 快捷键）：不适用的视图静默回退，
    // 不弹模态框，也不留下一个「用户以为切了、其实画的是空图」的状态。
    if (!ViewSupported(kind))
        kind = smd::MapViewKind::Icicle;
    view_kind_ = kind;
    if (view_combo_ && view_combo_->currentIndex() != static_cast<int>(kind)) {
        QSignalBlocker blocker(view_combo_);
        view_combo_->setCurrentIndex(static_cast<int>(kind));
    }
    selected_bin_track_ = -1;
    selected_bin_index_ = -1;
    selected_fact_ = -1;
    selected_stream_row_ = -1;
    selected_stream_bin_ = -1;
    // 「均衡 / 字节占比」「层级」「搜索」只对层级带视图有意义；其它两个视图置灰，
    // 免得用户调了半天发现图没变（这是自绘多视图最常见的假交互）。
    const bool icicle = kind == smd::MapViewKind::Icicle;
    if (layout_combo_)
        layout_combo_->setEnabled(icicle);
    if (depth_combo_)
        depth_combo_->setEnabled(icicle);
    if (search_edit_)
        search_edit_->setEnabled(icicle);
    if (canvas_) {
        canvas_->SetViewKind(kind);
        canvas_->SetBinSelection(-1, -1);
        canvas_->SetFactSelection(-1);
    }
    UpdateDetail();
    UpdateHitLabel();
}

void FileStructureMapPage::SetVisibleLayers(int layers) {
    visible_layers_ = std::max(0, layers);
    RebuildNodes();
}

void FileStructureMapPage::RequestJumpToTree() {
    if (selected_ < 0 || selected_ >= static_cast<int>(nodes_.size()))
        return;
    const smd::MapNode& node = nodes_[selected_];
    emit StructureNodeJumpRequested(smd::BuildNodePath(nodes_, selected_), node.offset, node.size);
}

void FileStructureMapPage::OnExportCsv() {
    const QByteArray csv = smd::BuildStructureCsv(result_);
    if (csv.isEmpty()) {
        QMessageBox::information(this, tr("导出"),
                                 tr("尚无有效的结构节点，未导出文件。\n请先打开媒体文件，或等待容器结构分析完成。"));
        return;
    }
    const int rows = csv.count('\n') - 1;
    ExportCsvStream(
        this, tr("导出结构节点 CSV"), QStringLiteral("structure_nodes.csv"),
        [csv](QTextStream& stream) { stream << QString::fromUtf8(csv); }, rows);
}

void FileStructureMapPage::OnExportJson() {
    const QByteArray json = smd::BuildStructureJson(result_, coverage_);
    if (json.isEmpty()) {
        QMessageBox::information(this, tr("导出"),
                                 tr("尚无有效的结构节点，未导出文件。\n空 JSON 分不清「没问题」和「没分析」。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出结构数据 JSON"), QStringLiteral("structure_map.json"), tr("JSON 文件 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("错误"), tr("无法写入文件: %1").arg(path));
        return;
    }
    file.write(json);
    file.close();
    QMessageBox::information(this, tr("成功"), tr("已导出结构数据与完整性标记到:\n%1").arg(path));
}

void FileStructureMapPage::OnExportSvg() {
    QByteArray svg;
    if (view_kind_ == smd::MapViewKind::Icicle) {
        svg = smd::BuildStructureSvg(nodes_, canvas_->Geometry(), canvas_->view(), canvas_->first_row(), mode_,
                                     nodes_truncated_);
    } else {
        svg = smd::BuildVisualsSvg(canvas_->Visuals(), canvas_->size(), view_kind_, nodes_truncated_);
    }
    if (svg.isEmpty()) {
        QMessageBox::information(this, tr("导出"), tr("当前视图没有可导出的节点，未生成文件。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("导出当前视图 SVG"), QStringLiteral("structure_map.svg"),
                                                      tr("SVG 矢量图 (*.svg);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("错误"), tr("无法写入文件: %1").arg(path));
        return;
    }
    file.write(svg);
    file.close();
    QMessageBox::information(this, tr("成功"), tr("已导出当前视图到:\n%1").arg(path));
}

} // namespace ui
} // namespace videoeye
