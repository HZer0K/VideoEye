#include "ui/analysis_panel/MacroblockView.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"

#include <QCheckBox>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace videoeye {
namespace ui {

namespace {
constexpr int kMaxMotionVectorRows = 500;   // 保证主线程不被大表填充拖慢
constexpr int kMaxVizWidth = 480;
constexpr int kMaxVizHeight = 320;
}  // namespace

MacroblockView::MacroblockView(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
}

void MacroblockView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 工具栏
    QHBoxLayout* toolbar = new QHBoxLayout();
    summary_label_ = new QLabel(
        tr("帧: -- | 块总数: 0 | 前向: 0 | 后向: 0 | 帧内: 0 | 平均MV: 0.0 | 最大MV: 0.0"),
        this);
    toolbar->addWidget(summary_label_, 1);

    export_csv_button_ = new QPushButton(tr("导出 CSV"), this);
    toolbar->addWidget(export_csv_button_);

    toggle_ = new QCheckBox(tr("启用分析"), this);
    toolbar->addWidget(toggle_);
    layout->addLayout(toolbar);

    // 运动矢量表格 + 可视化预览 (水平分割)
    QSplitter* h_split = new QSplitter(Qt::Horizontal, this);

    // 左: 运动矢量表格
    QGroupBox* mv_group = new QGroupBox(tr("运动矢量列表"), this);
    QVBoxLayout* mv_layout = new QVBoxLayout(mv_group);
    mv_table_ = new QTableWidget(0, 9, mv_group);
    mv_table_->setHorizontalHeaderLabels(
        {"序号", "块X", "块Y", "块大小", "MVx(px)", "MVy(px)", "幅度(px)", "角度(°)", "参考"});
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
    viz_label_->setText(tr("等待视频播放...\n\n启用分析后将在播放时\n显示运动矢量可视化"));
    viz_layout->addWidget(viz_label_);
    h_split->addWidget(viz_group);

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

    connect(export_csv_button_, &QPushButton::clicked, this, &MacroblockView::OnExportCsv);
}

void MacroblockView::SetFeatureHooks(std::function<bool(int)> is_enabled,
                                     std::function<void(int, bool)> set_enabled) {
    is_enabled_ = std::move(is_enabled);
    set_enabled_ = std::move(set_enabled);

    SyncToggleFromHooks();
    if (toggle_) {
        connect(toggle_, &QCheckBox::toggled, this, [this](bool checked) {
            if (set_enabled_) set_enabled_(0, checked);
        });
    }
}

void MacroblockView::SyncToggleFromHooks() {
    if (!toggle_) return;
    // 没有钩子时按控件当前值保留（组件可脱离面板单独构造）
    const bool enabled = is_enabled_ ? is_enabled_(0) : toggle_->isChecked();
    if (enabled == toggle_->isChecked()) return;
    const QSignalBlocker blocker(toggle_);
    toggle_->setChecked(enabled);
}

void MacroblockView::SetAnalysis(const model::MacroblockFrameAnalysis& analysis) {
    analysis_ = analysis;
    dirty_ = true;
}

void MacroblockView::FlushPending() {
    if (!dirty_) return;
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
    QString frame_type_str;
    switch (ma.frame_type) {
        case 1: frame_type_str = "I"; break;
        case 2: frame_type_str = "P"; break;
        case 3: frame_type_str = "B"; break;
        default: frame_type_str = "?"; break;
    }
    summary_label_->setText(
        tr("帧 #%1 [%2] | 时间: %3s | %4总数: %5 | 前向: %6 | 后向: %7 | 帧内: %8 | 平均MV: %9 | 最大MV: %10")
            .arg(ma.frame_index)
            .arg(frame_type_str)
            .arg(ma.timestamp, 0, 'f', 3)
            .arg(block_term)
            .arg(s.total_blocks)
            .arg(s.forward_count)
            .arg(s.backward_count)
            .arg(s.intra_count)
            .arg(s.avg_motion_magnitude, 0, 'f', 2)
            .arg(s.max_motion_magnitude, 0, 'f', 2));

    // 填充运动矢量表格 (限制最多 kMaxMotionVectorRows 行以保证流畅)
    mv_table_->setUpdatesEnabled(false);
    const int mv_count = static_cast<int>(ma.motion_vectors.size());
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
    auto fill_dist_table = [](QTableWidget* table, const QStringList& labels,
                              const QList<int>& counts, int total) {
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
        bs_labels = {"64x64", "32x32", "32x16", "16x32", "32x8", "8x32",
                     "16x16", "16x8", "8x16", "8x8", "8x4", "4x8", "4x4", tr("其他")};
        bs_counts = {s.count_64x64, s.count_32x32, s.count_32x16, s.count_16x32,
                     s.count_32x8, s.count_8x32, s.count_16x16, s.count_16x8,
                     s.count_8x16, s.count_8x8, s.count_8x4, s.count_4x8,
                     s.count_4x4, s.count_other};
    } else {
        // H.264 等传统编码: 宏块尺寸
        bs_labels = {"16x16", "16x8", "8x16", "8x8", "8x4", "4x8", "4x4", tr("其他")};
        bs_counts = {s.count_16x16, s.count_16x8, s.count_8x16, s.count_8x8,
                     s.count_8x4, s.count_4x8, s.count_4x4, s.count_other};
    }
    int bs_total = 0;
    for (int c : bs_counts) bs_total += c;
    fill_dist_table(blocksize_table_, bs_labels, bs_counts, bs_total);

    int mg_total = s.mag_0_2 + s.mag_2_4 + s.mag_4_8 + s.mag_8_16 + s.mag_16_plus;
    fill_dist_table(mag_table_,
                    {"0-2", "2-4", "4-8", "8-16", "16+"},
                    {s.mag_0_2, s.mag_2_4, s.mag_4_8, s.mag_8_16, s.mag_16_plus},
                    mg_total);

    // 绘制运动矢量可视化图
    if (ma.frame_width > 0 && ma.frame_height > 0 && !ma.motion_vectors.empty()) {
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
        viz_img.fill(QColor(13, 17, 23));  // 深色背景

        QPainter painter(&viz_img);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // 绘制运动矢量箭头
        for (const auto& mv : ma.motion_vectors) {
            double mv_scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
            double px_mx = mv.motion_x / mv_scale;
            double px_my = mv.motion_y / mv_scale;

            double cx = (mv.block_x + mv.block_w / 2.0) * scale_x;
            double cy = (mv.block_y + mv.block_h / 2.0) * scale_y;
            double ex = cx + px_mx * scale_x;
            double ey = cy + px_my * scale_y;

            // 颜色: 前向=红, 后向=蓝
            QColor color = (mv.source < 0) ? QColor(255, 107, 107)
                                           : QColor(88, 166, 255);
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
                QPointF p1(ex - arrow_len * std::cos(angle - 0.5),
                           ey - arrow_len * std::sin(angle - 0.5));
                QPointF p2(ex - arrow_len * std::cos(angle + 0.5),
                           ey - arrow_len * std::sin(angle + 0.5));
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
    } else if (ma.has_motion_vectors == false && ma.frame_width > 0) {
        // I 帧: 无运动矢量
        viz_label_->setText(
            tr("I 帧 (无运动矢量)\n帧内块数: %1").arg(s.intra_count));
    }
}

void MacroblockView::OnExportCsv() {
    const auto& ma = analysis_;
    if (ma.motion_vectors.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有宏块分析数据可导出"));
        return;
    }

    ExportCsvStream(this, tr("导出宏块分析 CSV"),
        QStringLiteral("macroblock_frame_%1.csv").arg(ma.frame_index),
        [this, &ma](QTextStream& stream) {
            stream << "序号,块X,块Y,块宽,块高,MVx(raw),MVy(raw),精度分母,MVx(px),MVy(px),幅度(px),角度(度),参考方向\n";

            for (int i = 0; i < static_cast<int>(ma.motion_vectors.size()); ++i) {
                const auto& mv = ma.motion_vectors[i];
                double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
                double px_mx = mv.motion_x / scale;
                double px_my = mv.motion_y / scale;
                QString ref = (mv.source < 0) ? "forward" : (mv.source > 0) ? "backward" : "none";

                stream << i << ","
                       << mv.block_x << ","
                       << mv.block_y << ","
                       << static_cast<int>(mv.block_w) << ","
                       << static_cast<int>(mv.block_h) << ","
                       << mv.motion_x << ","
                       << mv.motion_y << ","
                       << mv.motion_scale << ","
                       << QString::number(px_mx, 'f', 4) << ","
                       << QString::number(px_my, 'f', 4) << ","
                       << QString::number(mv.motion_magnitude, 'f', 4) << ","
                       << QString::number(mv.motion_angle, 'f', 2) << ","
                       << ref << "\n";
            }
        },
        static_cast<int>(ma.motion_vectors.size()));
}

}  // namespace ui
}  // namespace videoeye
