#include "ui/analysis_panel/MacroblockView.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"

#include <QCheckBox>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace videoeye {
namespace ui {

namespace {
constexpr int kMaxMotionVectorRows = 500;       // 保证主线程不被大表填充拖慢
constexpr int kMaxVizVectors = 1200;            // 预览最多绘制多少条箭头（超出按顺序等距抽样）
constexpr std::size_t kMaxMotionSamples = 6000; // 约 10Hz × 10 分钟
constexpr int kMaxVizWidth = 480;
constexpr int kMaxVizHeight = 320;

// 曲线绘图区：左侧留给纵轴刻度，右侧留一点余量；点击换算与绘制共用这套边距。
constexpr int kChartLeftAxis = 34;
constexpr int kChartRightMargin = 8;
constexpr int kChartTopMargin = 8;
constexpr int kChartBottomAxis = 18;

// 曲线历史的重置阈值：时间回退说明 Seek 回去了；向前跳超过这个跨度才视为跳转。
// 不能用「跳变 > 1.5s」—— 长时间静止镜头 / 全 I 帧片段会连续几秒没有可记录帧，
// 那不是 Seek，清空会白白丢掉前面的曲线。
constexpr double kMotionHistoryGapResetSeconds = 10.0;
// 场景切换标记的上限：长片可能有上千个切镜，超出后丢最早的。
constexpr std::size_t kMaxSceneChangeMarks = 512;

// 帧类型短名 (AVPictureType: 1=I, 2=P, 3=B)
QString FrameTypeShortName(int frame_type) {
    switch (frame_type) {
    case 1:
        return QStringLiteral("I");
    case 2:
        return QStringLiteral("P");
    case 3:
        return QStringLiteral("B");
    default:
        return QStringLiteral("?");
    }
}
} // namespace

MacroblockView::MacroblockView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void MacroblockView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 页头：标题 + 启用分析 + 统一「导出」菜单（与原工具栏里的导出按钮同一能力，
    // 只是收进标准位置）
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("运动矢量与块分析"), row);
        title->setObjectName(QStringLiteral("PageTitle"));
        rl->addWidget(title);
        rl->addStretch();

        toggle_ = new QCheckBox(tr("启用分析"), row);
        toggle_->setToolTip(tr("播放时实时采集运动矢量与块统计，需要额外解码残差，默认关闭，按需开启。"));
        rl->addWidget(toggle_);

        rl->addWidget(MakeExportMenuButton(row, {{tr("运动矢量与块统计 CSV…"), [this]() { OnExportCsv(); },
                                                  tr("导出当前帧的运动矢量、块大小分布与幅度分布")},
                                                 {tr("运动强度时序 CSV…"), [this]() { OnExportMotionHistoryCsv(); },
                                                  tr("导出曲线历史（时间 / 平均与最大幅度 / 帧内块 / 总块数）")}}));
        layout->addWidget(row);
    }

    // 工具栏
    QHBoxLayout* toolbar = new QHBoxLayout();
    summary_label_ = new QLabel(this);
    toolbar->addWidget(summary_label_, 1);
    layout->addLayout(toolbar);

    // 运动矢量表格 + 可视化预览 (水平分割)
    QSplitter* h_split = new QSplitter(Qt::Horizontal, this);

    // 左: 运动矢量表格
    QGroupBox* mv_group = new QGroupBox(tr("运动矢量列表"), this);
    QVBoxLayout* mv_layout = new QVBoxLayout(mv_group);
    mv_table_ = new QTableWidget(0, 9, mv_group);
    mv_table_->setHorizontalHeaderLabels(
        {"序号", "中心X", "中心Y", "块大小", "MVx(px)", "MVy(px)", "幅度(px)", "角度(°)", "参考"});
    mv_table_->verticalHeader()->setVisible(false);
    mv_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mv_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mv_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    mv_table_->horizontalHeader()->setStretchLastSection(true);
    mv_table_->setColumnWidth(0, 50);
    mv_table_->setColumnWidth(1, 60);
    mv_table_->setColumnWidth(2, 60);
    mv_table_->setColumnWidth(3, 70);
    mv_table_->setColumnWidth(4, 80);
    mv_table_->setColumnWidth(5, 80);
    mv_table_->setColumnWidth(6, 80);
    mv_table_->setColumnWidth(7, 70);
    mv_table_->setMinimumWidth(450);
    mv_table_->setMinimumHeight(200);
    mv_layout->addWidget(mv_table_);
    h_split->addWidget(mv_group);

    // 右: 运动矢量可视化预览
    QGroupBox* viz_group = new QGroupBox(tr("运动矢量可视化"), this);
    QVBoxLayout* viz_layout = new QVBoxLayout(viz_group);
    viz_label_ = new QLabel(viz_group);
    viz_label_->setMinimumSize(320, 240);
    viz_label_->setAlignment(Qt::AlignCenter);
    viz_label_->setStyleSheet("background-color: #0D1117; border: 1px solid #30363D;");
    viz_label_->setWordWrap(true);
    viz_label_->setToolTip(tr("点击左侧「运动矢量列表」的某一行，可在预览中高亮对应的块与箭头"));
    viz_layout->addWidget(viz_label_);

    // 运动强度时间曲线: 放在预览下方，点击可定位
    chart_label_ = new QLabel(tr("运动强度曲线（播放后生成）"), viz_group);
    chart_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    chart_label_->setStyleSheet("background-color: #0D1117; border: 1px solid #30363D; color: #8B949E;");
    chart_label_->setMinimumHeight(110);
    chart_label_->setToolTip(tr("横轴为时间：黄线=平均幅度，蓝线=最大幅度；点击曲线可跳转到对应时间"));
    chart_label_->installEventFilter(this);
    viz_layout->addWidget(chart_label_);

    h_split->addWidget(viz_group);

    // 表格选中行 -> 预览高亮。只重绘预览，不重新填表/统计。
    connect(mv_table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const int row = mv_table_->currentRow();
        if (row == highlighted_row_)
            return;
        highlighted_row_ = row;
        RedrawVisualization();
    });

    h_split->setStretchFactor(0, 3);
    h_split->setStretchFactor(1, 2);
    layout->addWidget(h_split, 1);

    // 块大小分布 + 幅度分布 (水平排列)
    QHBoxLayout* dist_layout = new QHBoxLayout();

    QGroupBox* blocksize_group = new QGroupBox(tr("块大小分布"), this);
    QVBoxLayout* bs_layout = new QVBoxLayout(blocksize_group);
    blocksize_table_ = new QTableWidget(0, 3, blocksize_group);
    blocksize_table_->setHorizontalHeaderLabels({"块大小", "数量", "占比"});
    blocksize_table_->verticalHeader()->setVisible(false);
    blocksize_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    blocksize_table_->horizontalHeader()->setStretchLastSection(true);
    blocksize_table_->setMinimumHeight(160);
    bs_layout->addWidget(blocksize_table_);
    dist_layout->addWidget(blocksize_group);

    QGroupBox* mag_group = new QGroupBox(tr("运动幅度分布"), this);
    QVBoxLayout* mg_layout = new QVBoxLayout(mag_group);
    mag_table_ = new QTableWidget(0, 3, mag_group);
    mag_table_->setHorizontalHeaderLabels({"幅度范围(px)", "数量", "占比"});
    mag_table_->verticalHeader()->setVisible(false);
    mag_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mag_table_->horizontalHeader()->setStretchLastSection(true);
    mag_table_->setMinimumHeight(160);
    mg_layout->addWidget(mag_table_);
    dist_layout->addWidget(mag_group);

    layout->addLayout(dist_layout);

    // 初态文案统一走 Reset()，避免这里和换文件清空各写一份、改一处漏一处。
    Reset();
}

void MacroblockView::SetFeatureHooks(std::function<bool(int)> is_enabled, std::function<void(int, bool)> set_enabled) {
    is_enabled_ = std::move(is_enabled);
    set_enabled_ = std::move(set_enabled);

    SyncToggleFromHooks();
    if (toggle_) {
        connect(toggle_, &QCheckBox::toggled, this, [this](bool checked) {
            if (set_enabled_)
                set_enabled_(0, checked);
        });
    }
}

void MacroblockView::SyncToggleFromHooks() {
    if (!toggle_)
        return;
    // 没有钩子时按控件当前值保留（组件可脱离面板单独构造）
    const bool enabled = is_enabled_ ? is_enabled_(0) : toggle_->isChecked();
    if (enabled == toggle_->isChecked())
        return;
    const QSignalBlocker blocker(toggle_);
    toggle_->setChecked(enabled);
}

void MacroblockView::SetAnalysis(const model::MacroblockFrameAnalysis& analysis) {
    analysis_ = analysis;
    dirty_ = true;
}

void MacroblockView::Reset() {
    analysis_ = model::MacroblockFrameAnalysis{};
    dirty_ = false;
    highlighted_row_ = -1;
    motion_history_.clear();
    scene_change_timestamps_.clear();
    last_sample_timestamp_ = -1.0;
    chart_t0_ = 0.0;
    chart_t1_ = 1.0;
    chart_pixel_width_ = 0;
    chart_pixel_height_ = 0;

    if (summary_label_) {
        summary_label_->setToolTip(QString());
        summary_label_->setText(
            tr("分析帧: -- | 块总数: -- | 去重块: -- | 前向: -- | 后向: -- | 帧内: -- | 平均MV: -- | 最大MV: --"));
    }
    if (mv_table_)
        mv_table_->setRowCount(0);
    if (blocksize_table_)
        blocksize_table_->setRowCount(0);
    if (mag_table_)
        mag_table_->setRowCount(0);
    if (viz_label_) {
        // clear() 连原来的 pixmap 一起清掉；只 setText 的话，旧帧的箭头图会留在
        // 文字下面继续显示（QLabel 的 pixmap 与 text 是两套内容）。
        viz_label_->clear();
        viz_label_->setText(tr("等待视频播放...\n\n启用分析后将在播放时\n显示运动矢量可视化"));
    }
    if (chart_label_) {
        chart_label_->clear();
        chart_label_->setText(tr("运动强度曲线（播放后生成）"));
    }
}

void MacroblockView::FlushPending() {
    if (!dirty_)
        return;
    dirty_ = false;
    RefreshUi();
}

void MacroblockView::RefreshUi() {
    const auto& ma = analysis_;
    const auto& s = ma.stats;

    // 术语自适应: HEVC→CTU, 其余→宏块
    const bool is_hevc = CodecType::IsHevc(ma.codec_id);
    const QString block_term = is_hevc ? tr("CTU") : tr("宏块");

    // 更新统计标签
    const QString frame_type_str = FrameTypeShortName(ma.frame_type);
    const QString estimate_note = s.block_counts_estimated ? tr("（估算）") : QString();
    // 总块数只有两条路径会填: 带运动矢量时 ComputeStats 铺一次网格, 或 I 帧走估算分支。
    // 两者都没走时它仍是 0 —— 那不是"0 个块", 是"没算", 所以必须报不可用。
    const QString total_blocks = ma.has_motion_vectors || s.intra_count_available
                                     ? QString::number(s.total_blocks) + estimate_note
                                     : tr("不可用");
    const QString intra_blocks =
        s.intra_count_available ? QString::number(s.intra_count) + estimate_note : tr("不可用");
    // 去重块数只在有运动矢量时才有意义: 没铺过网格/没遍历记录时它停在 0, 报 0 会误导。
    const QString distinct_blocks = ma.has_motion_vectors ? QString::number(s.distinct_block_count) : tr("不可用");

    const int mv_count = static_cast<int>(ma.motion_vectors.size());
    // 表格只画前 500 条，必须明说，否则用户会把"表里 500 条"当成这一帧的全部运动矢量。
    const QString truncate_note = mv_count > kMaxMotionVectorRows
                                      ? tr(" | 列表仅显示前 %1/%2 条").arg(kMaxMotionVectorRows).arg(mv_count)
                                      : QString();

    summary_label_->setToolTip(tr("块数按固定网格估算；运动矢量条数不能推导真实帧内块数。"
                                  "B 帧的同一块会同时产生前向与后向两条记录，块大小分布、幅度分布与"
                                  "平均/最大幅度都按去重后的块统计（同一块取较大幅度）；前向/后向是参考"
                                  "记录数，两者之和可能大于去重块数。分析帧序号与帧表共用同一套"
                                  "逐媒体帧号（Seek 跳过的帧不计数，帧表未开启也不影响编号）。"));
    summary_label_->setText(
        tr("分析帧 #%1 [%2] | 时间: %3s | %4总数: %5 | 去重块: %6 | 前向: %7 | 后向: %8 | 帧内: %9 | "
           "平均MV: %10 | 最大MV: %11%12")
            .arg(ma.frame_index)
            .arg(frame_type_str)
            .arg(ma.timestamp, 0, 'f', 3)
            .arg(block_term)
            .arg(total_blocks)
            .arg(distinct_blocks)
            .arg(s.forward_count)
            .arg(s.backward_count)
            .arg(intra_blocks)
            .arg(s.avg_motion_magnitude, 0, 'f', 2)
            .arg(s.max_motion_magnitude, 0, 'f', 2)
            .arg(truncate_note));

    // 填充运动矢量表格 (限制最多 kMaxMotionVectorRows 行以保证流畅)
    // 新一帧的同一行已经换了矢量，旧选中没有意义；屏蔽选中变化信号，避免填充过程触发重绘。
    const QSignalBlocker table_blocker(mv_table_);
    highlighted_row_ = -1;
    mv_table_->clearSelection();
    mv_table_->setUpdatesEnabled(false);
    const int display_count = qMin(mv_count, kMaxMotionVectorRows);
    mv_table_->setRowCount(display_count);

    for (int i = 0; i < display_count; ++i) {
        const auto& mv = ma.motion_vectors[i];
        double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
        double px_mx = mv.motion_x / scale;
        double px_my = mv.motion_y / scale;
        QString ref_str = (mv.source < 0) ? tr("前向") : (mv.source > 0) ? tr("后向") : tr("—");
        QString size_str = QString("%1x%2").arg(mv.block_w).arg(mv.block_h);

        SetTableItemText(mv_table_, i, 0, QString::number(i));
        SetTableItemText(mv_table_, i, 1, QString::number(mv.block_x));
        SetTableItemText(mv_table_, i, 2, QString::number(mv.block_y));
        SetTableItemText(mv_table_, i, 3, size_str);
        SetTableItemText(mv_table_, i, 4, QString::number(px_mx, 'f', 2));
        SetTableItemText(mv_table_, i, 5, QString::number(px_my, 'f', 2));
        SetTableItemText(mv_table_, i, 6, QString::number(mv.motion_magnitude, 'f', 2));
        SetTableItemText(mv_table_, i, 7, QString::number(mv.motion_angle, 'f', 1));
        SetTableItemText(mv_table_, i, 8, ref_str);
    }
    mv_table_->setUpdatesEnabled(true);

    // 填充块大小分布表
    auto fill_dist_table = [](QTableWidget* table, const QStringList& labels, const QList<int>& counts, int total) {
        table->setUpdatesEnabled(false);
        table->setRowCount(labels.size());
        for (int i = 0; i < labels.size(); ++i) {
            SetTableItemText(table, i, 0, labels[i]);
            SetTableItemText(table, i, 1, QString::number(counts[i]));
            double ratio = (total > 0) ? (100.0 * counts[i] / total) : 0.0;
            SetTableItemText(table, i, 2, QString::number(ratio, 'f', 1) + "%");
        }
        table->setUpdatesEnabled(true);
    };

    // 块大小分布表 — 根据 codec 动态显示尺寸行
    QStringList bs_labels;
    QList<int> bs_counts;
    if (is_hevc) {
        // HEVC CTU/CU 大尺寸分区 + 共有小尺寸
        bs_labels = {"64x64", "32x32", "32x16", "16x32", "32x8", "8x32", "16x16",
                     "16x8",  "8x16",  "8x8",   "8x4",   "4x8",  "4x4",  tr("其他")};
        bs_counts = {s.count_64x64, s.count_32x32, s.count_32x16, s.count_16x32, s.count_32x8,
                     s.count_8x32,  s.count_16x16, s.count_16x8,  s.count_8x16,  s.count_8x8,
                     s.count_8x4,   s.count_4x8,   s.count_4x4,   s.count_other};
    } else {
        // H.264 等传统编码: 宏块尺寸
        bs_labels = {"16x16", "16x8", "8x16", "8x8", "8x4", "4x8", "4x4", tr("其他")};
        bs_counts = {s.count_16x16, s.count_16x8, s.count_8x16, s.count_8x8,
                     s.count_8x4,   s.count_4x8,  s.count_4x4,  s.count_other};
    }
    int bs_total = 0;
    for (int c : bs_counts)
        bs_total += c;
    fill_dist_table(blocksize_table_, bs_labels, bs_counts, bs_total);

    int mg_total = s.mag_0_2 + s.mag_2_4 + s.mag_4_8 + s.mag_8_16 + s.mag_16_plus;
    fill_dist_table(mag_table_, {"0-2", "2-4", "4-8", "8-16", "16+"},
                    {s.mag_0_2, s.mag_2_4, s.mag_4_8, s.mag_8_16, s.mag_16_plus}, mg_total);

    // 绘制运动矢量可视化图 + 运动强度曲线
    AppendMotionHistory(ma);
    RedrawVisualization();
    RedrawMotionChart();
}

void MacroblockView::RedrawVisualization() {
    const auto& ma = analysis_;
    if (ma.frame_width <= 0 || ma.frame_height <= 0)
        return;

    const auto& s = ma.stats;
    const bool is_hevc = CodecType::IsHevc(ma.codec_id);
    const QString block_term = is_hevc ? tr("CTU") : tr("宏块");
    const QString frame_type_str = FrameTypeShortName(ma.frame_type);
    const QString estimate_note = s.block_counts_estimated ? tr("（估算）") : QString();
    const QString intra_blocks =
        s.intra_count_available ? QString::number(s.intra_count) + estimate_note : tr("不可用");

    if (!ma.motion_vectors.empty()) {
        double aspect = static_cast<double>(ma.frame_width) / ma.frame_height;
        int viz_w = kMaxVizWidth;
        int viz_h = static_cast<int>(viz_w / aspect);
        if (viz_h > kMaxVizHeight) {
            viz_h = kMaxVizHeight;
            viz_w = static_cast<int>(viz_h * aspect);
        }
        double scale_x = static_cast<double>(viz_w) / ma.frame_width;
        double scale_y = static_cast<double>(viz_h) / ma.frame_height;

        QImage viz_img(viz_w, viz_h, QImage::Format_RGB32);
        viz_img.fill(QColor(13, 17, 23)); // 深色背景

        QPainter painter(&viz_img);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // 绘制运动矢量箭头。
        // 大分辨率一帧可能有几千个分区，10Hz 全量重绘会拖慢主线程；按记录顺序等距抽样，
        // 空间分布基本保持，表格选中的那条仍在最后单独高亮。
        const auto& all_mvs = ma.motion_vectors;
        const int stride = all_mvs.size() > static_cast<std::size_t>(kMaxVizVectors)
                               ? static_cast<int>((all_mvs.size() + kMaxVizVectors - 1) / kMaxVizVectors)
                               : 1;
        int drawn_count = 0;
        for (std::size_t i = 0; i < all_mvs.size(); i += static_cast<std::size_t>(stride)) {
            const auto& mv = all_mvs[i];
            ++drawn_count;
            double mv_scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
            double px_mx = mv.motion_x / mv_scale;
            double px_my = mv.motion_y / mv_scale;

            // block_x/block_y 是块中心（FFmpeg AVMotionVector 的 dst 语义），
            // 不要再加半个块宽高，否则箭头整体偏移半个宏块。
            double cx = mv.block_x * scale_x;
            double cy = mv.block_y * scale_y;
            double ex = cx + px_mx * scale_x;
            double ey = cy + px_my * scale_y;

            // 颜色: 前向=红, 后向=蓝
            QColor color = (mv.source < 0) ? QColor(255, 107, 107) : QColor(88, 166, 255);
            // 幅度越大越亮
            double brightness = qMin(1.0, mv.motion_magnitude / 16.0);
            color.setAlphaF(0.3 + 0.7 * brightness);

            QPen pen(color, 1.2);
            painter.setPen(pen);
            painter.drawLine(QPointF(cx, cy), QPointF(ex, ey));

            // 箭头头部
            if (mv.motion_magnitude > 0.5) {
                double angle = std::atan2(ey - cy, ex - cx);
                double arrow_len = 3.0;
                QPointF p1(ex - arrow_len * std::cos(angle - 0.5), ey - arrow_len * std::sin(angle - 0.5));
                QPointF p2(ex - arrow_len * std::cos(angle + 0.5), ey - arrow_len * std::sin(angle + 0.5));
                painter.drawLine(QPointF(ex, ey), p1);
                painter.drawLine(QPointF(ex, ey), p2);
            }
        }

        if (stride > 1) {
            painter.setPen(QColor(200, 200, 200));
            painter.drawText(10, 16, tr("抽样显示 %1/%2").arg(drawn_count).arg(all_mvs.size()));
        }

        // 高亮表格里选中的运动矢量：虚线框标出块范围，实线复描箭头，画在最后保证在最上层。
        if (highlighted_row_ >= 0 && highlighted_row_ < static_cast<int>(ma.motion_vectors.size()) &&
            highlighted_row_ < kMaxMotionVectorRows) {
            const auto& mv = ma.motion_vectors[highlighted_row_];
            double mv_scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
            double cx = mv.block_x * scale_x;
            double cy = mv.block_y * scale_y;
            double ex = cx + (mv.motion_x / mv_scale) * scale_x;
            double ey = cy + (mv.motion_y / mv_scale) * scale_y;

            const QColor highlight(255, 214, 10);
            painter.setPen(QPen(highlight, 1.2, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(cx - mv.block_w * scale_x / 2.0, cy - mv.block_h * scale_y / 2.0,
                                    mv.block_w * scale_x, mv.block_h * scale_y));

            painter.setPen(QPen(highlight, 2.4));
            painter.drawLine(QPointF(cx, cy), QPointF(ex, ey));
            if (mv.motion_magnitude > 0.5) {
                double angle = std::atan2(ey - cy, ex - cx);
                double arrow_len = 4.5;
                QPointF p1(ex - arrow_len * std::cos(angle - 0.5), ey - arrow_len * std::sin(angle - 0.5));
                QPointF p2(ex - arrow_len * std::cos(angle + 0.5), ey - arrow_len * std::sin(angle + 0.5));
                painter.drawLine(QPointF(ex, ey), p1);
                painter.drawLine(QPointF(ex, ey), p2);
            }
        }

        // 绘制图例
        painter.setPen(QPen(QColor(255, 107, 107), 2));
        painter.drawLine(10, viz_h - 20, 30, viz_h - 20);
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(35, viz_h - 16, tr("前向"));
        painter.setPen(QPen(QColor(88, 166, 255), 2));
        painter.drawLine(80, viz_h - 20, 100, viz_h - 20);
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(105, viz_h - 16, tr("后向"));

        painter.end();
        viz_label_->setPixmap(QPixmap::fromImage(viz_img));
        return;
    }

    if (!ma.has_motion_vectors) {
        const QString codec_name = QString::fromLatin1(CodecType::DisplayName(ma.codec_id));
        if (!CodecType::SupportsMotionVectorExport(ma.codec_id)) {
            // 解码器能力不足 ≠ 分析失败：HEVC / VP9 / AV1 的 FFmpeg 解码器不导出
            // AV_FRAME_DATA_MOTION_VECTORS，直说清楚，别让用户以为开了开关却没生效。
            const QString codec_label = codec_name.isEmpty() ? tr("当前编码") : codec_name;
            viz_label_->setText(tr("%1 帧\n%2 解码器不导出运动矢量\n帧内%3数: %4")
                                    .arg(frame_type_str, codec_label, block_term, intra_blocks));
        } else {
            viz_label_->setText(
                tr("%1 帧\n未提供运动矢量数据\n帧内%2数: %3").arg(frame_type_str, block_term, intra_blocks));
        }
    }
}

void MacroblockView::AppendMotionHistory(const model::MacroblockFrameAnalysis& ma) {
    // I 帧没有运动矢量，avg/max 恒为 0，画进曲线会被误读成"运动突然消失"，跳过。
    if (!ma.has_motion_vectors)
        return;

    MotionSample sample;
    sample.timestamp = ma.timestamp;
    sample.avg_magnitude = ma.stats.avg_motion_magnitude;
    sample.max_magnitude = ma.stats.max_motion_magnitude;
    sample.intra_count = ma.stats.intra_count;
    sample.total_blocks = ma.stats.total_blocks;

    // 时间回退 = Seek 回去了，旧曲线横轴与当前位置无关，清空重来；
    // 向前跳只有超过阈值才算跳转（静止镜头 / 全 I 帧的空档不该清空历史）。
    if (last_sample_timestamp_ >= 0.0) {
        const double delta = sample.timestamp - last_sample_timestamp_;
        if (delta < -1e-3 || delta > kMotionHistoryGapResetSeconds)
            motion_history_.clear();
    }
    if (!motion_history_.empty() && sample.timestamp <= motion_history_.back().timestamp + 1e-6) {
        // 暂停补采 / 同一帧重复投递: 替换最后一点，不重复追加。
        motion_history_.back() = sample;
        last_sample_timestamp_ = sample.timestamp;
        return;
    }

    motion_history_.push_back(sample);
    last_sample_timestamp_ = sample.timestamp;

    // 滚动窗口：超过上限时一次丢一批，避免每个采样点都搬整段 vector。
    if (motion_history_.size() > kMaxMotionSamples) {
        motion_history_.erase(motion_history_.begin(),
                              motion_history_.begin() + static_cast<std::ptrdiff_t>(kMaxMotionSamples / 6));
    }
}

void MacroblockView::AppendSceneChangeTimestamp(double seconds) {
    if (seconds < 0.0)
        return;
    // 保持升序：正常播放按时间到达，Seek 回头时可能乱序；同位置的重复投递只留一条。
    auto it = std::lower_bound(scene_change_timestamps_.begin(), scene_change_timestamps_.end(), seconds);
    if (it != scene_change_timestamps_.end() && std::abs(*it - seconds) < 1e-3)
        return;
    scene_change_timestamps_.insert(it, seconds);
    if (scene_change_timestamps_.size() > kMaxSceneChangeMarks)
        scene_change_timestamps_.erase(scene_change_timestamps_.begin());
    // 曲线可能正显示着这个位置：立即重画，不必等下一帧分析数据。
    RedrawMotionChart();
}

void MacroblockView::RedrawMotionChart() {
    if (!chart_label_)
        return;
    if (motion_history_.size() < 2) {
        chart_label_->clear();
        chart_label_->setText(tr("运动强度曲线（播放后生成）"));
        chart_pixel_width_ = 0;
        chart_pixel_height_ = 0;
        return;
    }

    // 按标签实际可用宽度绘制：固定 360px 在宽窗口里只占一小段，右侧空白区却仍参与
    // 点击换算，点在那里会被误判成「跳到结尾」。布局尚未完成时退回 360。
    const QRect contents = chart_label_->contentsRect();
    const int chart_width = contents.width() >= 200 ? contents.width() : 360;
    const int chart_height = std::max(90, contents.height());
    const int left = kChartLeftAxis;
    const int right = chart_width - kChartRightMargin;
    if (right - left < 40) // 极窄时放弃绘制，避免除零 / 负宽度
        return;
    const int top = kChartTopMargin;
    const int bottom = chart_height - kChartBottomAxis;

    double t0 = motion_history_.front().timestamp;
    double t1 = motion_history_.back().timestamp;
    if (t1 - t0 < 1.0)
        t1 = t0 + 1.0;

    double max_value = 1.0;
    for (const auto& sample : motion_history_) {
        max_value = std::max(max_value, sample.max_magnitude);
    }

    QImage img(chart_width, chart_height, QImage::Format_RGB32);
    img.fill(QColor(13, 17, 23));
    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double width = right - left;
    const double height = bottom - top;

    // 网格与纵轴刻度
    for (int i = 0; i <= 2; ++i) {
        const double y = top + height * i / 2.0;
        painter.setPen(QPen(QColor(48, 54, 61), 1, Qt::DotLine));
        painter.drawLine(QPointF(left, y), QPointF(right, y));
        painter.setPen(QColor(139, 148, 158));
        painter.drawText(2, static_cast<int>(y) + 4, QString::number(max_value * (1.0 - i / 2.0), 'f', 1));
    }

    auto to_point = [&](double ts, double value) {
        const double x = left + (ts - t0) / (t1 - t0) * width;
        const double y = bottom - (value / max_value) * height;
        return QPointF(x, y);
    };

    // 场景切换标记：竖线画在曲线之前（不遮挡折线），并记录两个标记之间是否断线。
    // 切镜两侧的幅度没有可比性，连线会把"换镜头"画成一次持续运动。
    auto has_cut_between = [&](double from, double to) {
        auto it = std::upper_bound(scene_change_timestamps_.begin(), scene_change_timestamps_.end(), from);
        return it != scene_change_timestamps_.end() && *it <= to;
    };
    for (double cut : scene_change_timestamps_) {
        if (cut < t0 || cut > t1)
            continue;
        // +0.5 让 1px 竖线落在像素中心，避免抗锯齿把整条线摊成两列半透明。
        const double x = std::floor(to_point(cut, 0.0).x()) + 0.5;
        painter.setPen(QPen(QColor(240, 160, 60), 1, Qt::DashLine));
        painter.drawLine(QPointF(x, top), QPointF(x, bottom));
    }

    // 最大幅度（蓝）在下、平均幅度（黄）在上，便于看出瞬时峰值与整体强度
    painter.setPen(QPen(QColor(88, 166, 255), 1.2));
    for (std::size_t i = 1; i < motion_history_.size(); ++i) {
        if (has_cut_between(motion_history_[i - 1].timestamp, motion_history_[i].timestamp))
            continue;
        painter.drawLine(to_point(motion_history_[i - 1].timestamp, motion_history_[i - 1].max_magnitude),
                         to_point(motion_history_[i].timestamp, motion_history_[i].max_magnitude));
    }
    painter.setPen(QPen(QColor(240, 200, 80), 1.6));
    for (std::size_t i = 1; i < motion_history_.size(); ++i) {
        if (has_cut_between(motion_history_[i - 1].timestamp, motion_history_[i].timestamp))
            continue;
        painter.drawLine(to_point(motion_history_[i - 1].timestamp, motion_history_[i - 1].avg_magnitude),
                         to_point(motion_history_[i].timestamp, motion_history_[i].avg_magnitude));
    }

    // 最新采样点：竖虚线 + 两个实心点，标出「当前播放到哪」
    const MotionSample& latest = motion_history_.back();
    const double latest_x = to_point(latest.timestamp, 0.0).x();
    painter.setPen(QPen(QColor(139, 148, 158), 1, Qt::DashLine));
    painter.drawLine(QPointF(latest_x, top), QPointF(latest_x, bottom));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(88, 166, 255));
    painter.drawEllipse(to_point(latest.timestamp, latest.max_magnitude), 2.5, 2.5);
    painter.setBrush(QColor(240, 200, 80));
    painter.drawEllipse(to_point(latest.timestamp, latest.avg_magnitude), 2.5, 2.5);

    painter.setPen(QColor(139, 148, 158));
    painter.drawText(left, chart_height - 4, QString::number(t0, 'f', 1) + "s");
    painter.drawText(right - 30, chart_height - 4, QString::number(t1, 'f', 1) + "s");

    painter.end();
    chart_label_->setPixmap(QPixmap::fromImage(img));
    chart_t0_ = t0;
    chart_t1_ = t1;
    chart_pixel_width_ = chart_width;
    chart_pixel_height_ = chart_height;
}

bool MacroblockView::eventFilter(QObject* watched, QEvent* event) {
    if (watched != chart_label_)
        return QWidget::eventFilter(watched, event);

    // 宽度变化后重画一次，让曲线跟着标签铺满；只在宽度真的变了时重绘，避免布局回环。
    if (event->type() == QEvent::Resize) {
        auto* resize = static_cast<QResizeEvent*>(event);
        if (resize->size().width() >= 200 && resize->size().width() != chart_pixel_width_)
            RedrawMotionChart();
        return false;
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pos = mouse->position().toPoint();
        // 只响应绘图区内的点击：图外（图例文字、留白）不该跳转。
        if (mouse->button() == Qt::LeftButton && chart_pixel_width_ > 0 && chart_t1_ > chart_t0_ && pos.x() >= 0 &&
            pos.x() <= chart_pixel_width_ && pos.y() >= 0 && pos.y() <= chart_pixel_height_) {
            // 与绘制共用同一套边距：左端 = t0，右端 = t1。
            const int left = kChartLeftAxis;
            const int right = chart_pixel_width_ - kChartRightMargin;
            const int x = std::clamp(pos.x(), left, right);
            const double ratio = static_cast<double>(x - left) / (right - left);
            emit SeekRequested(chart_t0_ + ratio * (chart_t1_ - chart_t0_));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MacroblockView::OnExportCsv() {
    const auto& ma = analysis_;
    if (ma.motion_vectors.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有宏块分析数据可导出"));
        return;
    }

    const auto& s = ma.stats;
    const bool is_hevc = CodecType::IsHevc(ma.codec_id);
    const QString block_term = is_hevc ? tr("CTU") : tr("宏块");

    ExportCsvStream(
        this, tr("导出宏块分析 CSV"), QStringLiteral("macroblock_analysis_frame_%1.csv").arg(ma.frame_index),
        [this, &ma, &s, is_hevc, block_term](QTextStream& stream) {
            stream << "# 运动矢量明细\n";
            stream
                << "序号,中心X,中心Y,块宽,块高,MVx(raw),MVy(raw),精度分母,MVx(px),MVy(px),幅度(px),角度(度),参考方向\n";

            for (int i = 0; i < static_cast<int>(ma.motion_vectors.size()); ++i) {
                const auto& mv = ma.motion_vectors[i];
                double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
                double px_mx = mv.motion_x / scale;
                double px_my = mv.motion_y / scale;
                QString ref = (mv.source < 0) ? "forward" : (mv.source > 0) ? "backward" : "none";

                stream << i << "," << mv.block_x << "," << mv.block_y << "," << static_cast<int>(mv.block_w) << ","
                       << static_cast<int>(mv.block_h) << "," << mv.motion_x << "," << mv.motion_y << ","
                       << mv.motion_scale << "," << QString::number(px_mx, 'f', 4) << ","
                       << QString::number(px_my, 'f', 4) << "," << QString::number(mv.motion_magnitude, 'f', 4) << ","
                       << QString::number(mv.motion_angle, 'f', 2) << "," << ref << "\n";
            }

            // 菜单提示承诺导出「块大小分布与幅度分布」，这里补上；否则 CSV 只有明细，
            // 想复算统计还得自己写脚本。
            stream << "\n# 帧统计\n";
            stream << "分析帧序号,时间戳(s),帧类型," << block_term
                   << "总数（估算）,去重块,前向,后向,帧内,平均幅度(px),最大幅度(px)\n";
            stream << ma.frame_index << "," << QString::number(ma.timestamp, 'f', 3) << "," << ma.frame_type << ","
                   << s.total_blocks << "," << s.distinct_block_count << "," << s.forward_count << ","
                   << s.backward_count << "," << s.intra_count << "," << QString::number(s.avg_motion_magnitude, 'f', 3)
                   << "," << QString::number(s.max_motion_magnitude, 'f', 3) << "\n";

            QStringList block_labels;
            QList<int> block_counts;
            if (is_hevc) {
                block_labels = {"64x64", "32x32", "32x16", "16x32", "32x8", "8x32", "16x16",
                                "16x8",  "8x16",  "8x8",   "8x4",   "4x8",  "4x4",  "other"};
                block_counts = {s.count_64x64, s.count_32x32, s.count_32x16, s.count_16x32, s.count_32x8,
                                s.count_8x32,  s.count_16x16, s.count_16x8,  s.count_8x16,  s.count_8x8,
                                s.count_8x4,   s.count_4x8,   s.count_4x4,   s.count_other};
            } else {
                block_labels = {"16x16", "16x8", "8x16", "8x8", "8x4", "4x8", "4x4", "other"};
                block_counts = {s.count_16x16, s.count_16x8, s.count_8x16, s.count_8x8,
                                s.count_8x4,   s.count_4x8,  s.count_4x4,  s.count_other};
            }
            int block_total = 0;
            for (int c : block_counts)
                block_total += c;
            stream << "\n# 块大小分布\n块大小,数量,占比(%)\n";
            for (int i = 0; i < block_labels.size(); ++i) {
                const double ratio = block_total > 0 ? 100.0 * block_counts[i] / block_total : 0.0;
                stream << block_labels[i] << "," << block_counts[i] << "," << QString::number(ratio, 'f', 2) << "\n";
            }

            const QList<int> mag_counts = {s.mag_0_2, s.mag_2_4, s.mag_4_8, s.mag_8_16, s.mag_16_plus};
            const QStringList mag_labels = {"0-2", "2-4", "4-8", "8-16", "16+"};
            int mag_total = 0;
            for (int c : mag_counts)
                mag_total += c;
            stream << "\n# 运动幅度分布(px)\n幅度范围,数量,占比(%)\n";
            for (int i = 0; i < mag_labels.size(); ++i) {
                const double ratio = mag_total > 0 ? 100.0 * mag_counts[i] / mag_total : 0.0;
                stream << mag_labels[i] << "," << mag_counts[i] << "," << QString::number(ratio, 'f', 2) << "\n";
            }
        },
        static_cast<int>(ma.motion_vectors.size()));
}

void MacroblockView::OnExportMotionHistoryCsv() {
    if (motion_history_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("还没有运动强度时序数据可导出"));
        return;
    }

    ExportCsvStream(
        this, tr("导出运动强度时序 CSV"), QStringLiteral("macroblock_motion_history.csv"),
        [this](QTextStream& stream) {
            stream << "时间戳(s),平均幅度(px),最大幅度(px),帧内块,总块数\n";
            for (const auto& sample : motion_history_) {
                stream << QString::number(sample.timestamp, 'f', 3) << ","
                       << QString::number(sample.avg_magnitude, 'f', 3) << ","
                       << QString::number(sample.max_magnitude, 'f', 3) << "," << sample.intra_count << ","
                       << sample.total_blocks << "\n";
            }
        },
        static_cast<int>(motion_history_.size()));
}

} // namespace ui
} // namespace videoeye
