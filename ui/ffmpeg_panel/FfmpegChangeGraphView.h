#pragma once

// 「转换图谱」画布：把 ChangeGraph 摆成 输入流 → 滤镜 → 编码 → 输出流 四列，
// 每个输出节点下面挂一串"变了什么"的小标签。
//
// 为什么自己画（而不是用 QTableWidget / QTreeWidget）：
//   这一页要表达的是**流向**（一路输入经过哪些工序变成哪一路输出），表格能表达
//   "字段对比"，表达不了"谁变成了谁"。四列加连接线是最小可用的形状。
//
// 与文件结构图谱同一套做法：几何、命中、SVG **全部**由纯函数产出，
// 控件只负责 paintEvent 与鼠标。自绘最容易出的不是崩溃，而是"画的位置和点中的
// 位置不是一回事"、"导出的图和屏幕上的不一致"—— 做成纯函数才能被单测直接钉住。
//
// 颜色口径：节点用调色板（跟随主题），状态色用 ChangeStateColor（已变=琥珀、
// 未变=灰、新增=蓝、移除=红、未知=更暗的灰）。**不要**给"已变"单独上红/绿 ——
// 这不是涨跌，红绿会被读成好坏。

#include <QByteArray>
#include <QColor>
#include <QLine>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <vector>

#include "core/ffmpeg/FfmpegChangeGraph.h"

class QComboBox;
class QLabel;
class QPaintEvent;
class QPushButton;
class QResizeEvent;

namespace videoeye {
namespace ui {

// ---------------------------------------------------------------------------
// 纯几何层
// ---------------------------------------------------------------------------
namespace change_graph_view {

enum class VisualKind {
    InputNode,
    FilterNode,
    EncoderNode,
    OutputNode,
    ChangeBadge,    // 某个字段的变化（行内）
    ContainerBadge, // 容器级变化（底部）
    Header,
    Edge,
    Placeholder, // "未探测" / "直通" 这类虚线占位
};

struct Visual {
    VisualKind kind = VisualKind::Placeholder;
    int row = -1;          // 所属行（Header / ContainerBadge / Edge 为 -1）
    int change_index = -1; // ChangeBadge: row.changes 的下标；ContainerBadge: container_changes 的下标
    QRect rect;
    QLine edge;
    QString title;
    QString subtitle;
    QString badge; // 状态标签文字（"已变"/"未变"/…）
    QColor fill;
    QColor border;
    QColor title_color;
    QColor subtitle_color;

    bool is_node() const {
        return kind == VisualKind::InputNode || kind == VisualKind::FilterNode || kind == VisualKind::EncoderNode ||
               kind == VisualKind::OutputNode;
    }
};

/// 主题相关的颜色。默认值就是暗色主题；控件构造时按自己的调色板覆盖。
struct ViewPalette {
    QColor background;
    QColor node_fill;
    QColor process_fill;
    QColor border;
    QColor title;
    QColor subtitle;
    QColor header;
    QColor grid;
};

ViewPalette DarkPalette();

// 版式常量：页面与单测共用同一份，别在两边各抄一遍。
inline constexpr int kHeaderHeight = 26;
inline constexpr int kNodeHeight = 54;
inline constexpr int kBadgeHeight = 19;
inline constexpr int kBadgeGap = 3;
inline constexpr int kRowGap = 12;
inline constexpr int kOuterPad = 10;
inline constexpr int kColumnGap = 22;
inline constexpr int kMaxBadgesPerRow = 3; // 超过就折叠成"+N 项"，不无限撑高行
inline constexpr int kContainerStripHeight = 26;

/// 四列的相对边界（0~1）。输入 / 滤镜 / 编码 / 输出。
struct ColumnBounds {
    double input_end = 0.24;
    double filter_end = 0.44;
    double encoder_end = 0.64;
    double output_end = 1.0;
};
ColumnBounds DefaultColumns();

/// 某一行最多画几条变化标签（超过就折叠）。
int VisibleBadgeCount(const ffmpeg::ChangeGraph& graph, int row);

/// 整张图在给定宽度下需要多高（含表头与底部容器条）。
int PreferredHeight(const ffmpeg::ChangeGraph& graph, int width);

/// 按宽度生成全部视觉元素。graph.ok=false 时返回空。
std::vector<Visual> BuildVisuals(const ffmpeg::ChangeGraph& graph, int width, const ViewPalette& palette);

/// 命中最靠上的那个元素（后画的在上）。命中不到返回 -1。
/// 只命中带信息的元素：Edge / Header 不参与命中（点线不该跳详情）。
int VisualAt(const std::vector<Visual>& visuals, QPoint pos);

/// 某条视觉元素的完整说明（详情区用）。没有可说的返回空串。
QString DescribeVisual(const ffmpeg::ChangeGraph& graph, const Visual& visual);

/// 导出 SVG：与屏幕同一份 Visual 列表，附带图例。
QByteArray BuildSvg(const std::vector<Visual>& visuals, QSize size, const QString& title, const ViewPalette& palette);

/// 按像素宽度截断文本（CJK 按 2 个字符宽计）。纯函数，便于断言"导出的文字和屏幕上一样"。
QString ElideToWidth(const QString& text, int pixels);

/// 一条变化标签放得下「正文 + 状态词」所需的最小宽度（含两侧内缩）。
/// 布局和单测共用：宽度不够时截正文，绝不能让状态词压在正文上。
int BadgeMinWidth(const QString& text, const QString& state);

} // namespace change_graph_view

// ---------------------------------------------------------------------------
// 控件
// ---------------------------------------------------------------------------
/// 画布本体：只做两件事 —— 把 Visual 列表画出来、把点击落回某个 Visual。
///
/// 为什么单独一个控件，而不是让外面的 View 自己 paintEvent 铺满自己：
/// 顶部的「输出：xx」、摘要行、"导出 SVG"按钮、底部详情都是**真控件**（要能选中
/// 复制、要能放按钮），它们和画布在同一个竖向流里排队。外层控件若自己铺满整个
/// 矩形再画表头，列标题就会被那行文字压住 —— 第一版正是这样，一渲染就看见
/// 「输入流（实测）」被「输出：out.mp4」盖住、摘要行压在第一个节点上。
class FfmpegChangeGraphCanvas : public QWidget {
    Q_OBJECT

public:
    explicit FfmpegChangeGraphCanvas(QWidget* parent = nullptr);

    /// 换图。empty_hint 为空态文案（没跑过 / 输入没探测到 / 命令结构不明）。
    void SetGraph(const ffmpeg::ChangeGraph& graph, const change_graph_view::ViewPalette& palette,
                  const QString& empty_hint);
    void Clear();

    bool HasGraph() const {
        return has_data_;
    }
    int VisualCount() const {
        return static_cast<int>(visuals_.size());
    }
    int SelectedVisual() const {
        return selected_;
    }
    const std::vector<change_graph_view::Visual>& Visuals() const {
        return visuals_;
    }
    QByteArray ExportSvg(const QString& title) const;

signals:
    /// 点中了某条元素：把它的完整说明抛给外层填详情区。点空白抛空串。
    void VisualActivated(const QString& description);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void Rebuild();

    ffmpeg::ChangeGraph graph_;
    change_graph_view::ViewPalette palette_;
    std::vector<change_graph_view::Visual> visuals_;
    QString empty_hint_;
    int selected_ = -1;
    bool has_data_ = false;
};

class FfmpegChangeGraphView : public QWidget {
    Q_OBJECT

public:
    explicit FfmpegChangeGraphView(QWidget* parent = nullptr);

    /// 换一组图谱。多输出命令会有多份，用顶部下拉切换。
    void SetGraphs(std::vector<ffmpeg::ChangeGraph> graphs);
    void Clear();

    bool HasGraph() const {
        return !graphs_.empty();
    }
    int GraphCount() const {
        return static_cast<int>(graphs_.size());
    }
    int CurrentIndex() const {
        return current_;
    }
    void SetCurrentIndex(int index);
    /// 命令被改动后挂一句提醒（空串 = 不提醒）。图谱不会因此失效 ——
    /// 它描述的是上一次运行，删掉反而让人看不到刚跑出来的结果。
    void SetStaleHint(const QString& text);
    /// 空态文案（区分"没跑过" / "输入没探测到" / "命令结构不明"）。
    QString EmptyHintText() const;
    QString SummaryText() const;
    QString DetailText() const;
    /// 只读状态快照，供单测断言（不依赖像素）。
    int VisualCount() const;
    int SelectedVisual() const;
    QByteArray ExportSvg() const;

signals:
    void StatusMessage(const QString& text);

private:
    void Rebuild();
    const ffmpeg::ChangeGraph& Current() const;
    change_graph_view::ViewPalette Palette() const;
    void UpdateSummary();
    void OnOutputChanged();

    std::vector<ffmpeg::ChangeGraph> graphs_;
    int current_ = 0;

    QWidget* header_bar_ = nullptr;
    QLabel* target_label_ = nullptr;
    QComboBox* output_combo_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QLabel* summary_label_ = nullptr;
    FfmpegChangeGraphCanvas* canvas_ = nullptr;
    QLabel* detail_label_ = nullptr;
    QString stale_hint_;
};

} // namespace ui
} // namespace videoeye
