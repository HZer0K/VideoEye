#include "ui/analysis_panel/SceneChangePage.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include "infrastructure/logging/ScopedTimer.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/theme/AppTheme.h"

#include <algorithm>
#include <cmath>

namespace videoeye {
namespace ui {

namespace {

// 表格只保留最近这么多个切换点: QTableWidget 每行三个 item，长片快切素材下
// 无限增长会持续拖慢布局与滚动。records_ 不受限，CSV 导出始终是全集。
constexpr int kMaxTableRows = 2000;

// 切换点需要亚秒精度：截断到秒会让同一秒内的两个切点看起来完全一样。
QString FormatCutTimestamp(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0)
        seconds = 0.0;
    const int total_ms = static_cast<int>(seconds * 1000.0 + 0.5);
    return theme::font::formatTime(total_ms) + QStringLiteral(".") +
           QString("%1").arg(total_ms % 1000, 3, 10, QChar('0'));
}

// 柱状图上限：画布就这么宽，再多柱子只会糊成一片，悬停命中也会变慢。
constexpr int kMaxChartBars = 600;

// 切换点是离散事件，横轴必须用时间戳，否则"从这个切点到播放到哪儿"没有对应关系。
// 点数超过上限时按时间分桶、每桶保留最强的一次：等间隔抽取会把强切点抹掉，
// 而 MinMax 抽稀会画出屏幕上并不存在的低强度柱。
std::vector<QPointF> BuildChartPoints(const std::vector<model::SceneChangeResult>& records) {
    std::vector<QPointF> points;
    if (records.empty())
        return points;
    points.reserve(static_cast<std::size_t>(std::min<std::size_t>(records.size(), kMaxChartBars)));

    // 时间戳全部相同/不可用时退回按下标分桶，至少还能看出强度分布。
    const double span = records.back().timestamp - records.front().timestamp;
    const bool use_time = std::isfinite(span) && span > 1e-9;
    const auto x_of = [&](std::size_t i) { return use_time ? records[i].timestamp : static_cast<double>(i); };

    if (records.size() <= static_cast<std::size_t>(kMaxChartBars)) {
        for (std::size_t i = 0; i < records.size(); ++i)
            points.emplace_back(x_of(i), records[i].score);
        return points;
    }

    const double x_begin = x_of(0);
    const double bucket_w = (x_of(records.size() - 1) - x_begin) / kMaxChartBars;
    std::vector<double> best_score(static_cast<std::size_t>(kMaxChartBars), -1.0);
    std::vector<double> best_x(static_cast<std::size_t>(kMaxChartBars), 0.0);
    for (std::size_t i = 0; i < records.size(); ++i) {
        int b = bucket_w > 0.0 ? static_cast<int>((x_of(i) - x_begin) / bucket_w) : 0;
        b = std::clamp(b, 0, kMaxChartBars - 1);
        const auto idx = static_cast<std::size_t>(b);
        if (records[i].score > best_score[idx]) {
            best_score[idx] = records[i].score;
            best_x[idx] = x_of(i);
        }
    }
    for (std::size_t b = 0; b < best_score.size(); ++b) {
        if (best_score[b] >= 0.0)
            points.emplace_back(best_x[b], best_score[b]);
    }
    return points;
}

} // namespace

SceneChangePage::SceneChangePage(bool feature_checked, QWidget* parent)
    : QWidget(parent), feature_checked_(feature_checked) {
    SetupUi();
}

void SceneChangePage::SetupUi() {
    // 页面本体就是 QWidget（外部 QStackedWidget 的一页），不再多包一层
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 启用开关
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("场景切换检测（镜头边界）"), row);
        title->setObjectName(QStringLiteral("PageTitle"));
        rl->addWidget(title);
        rl->addStretch();
        QCheckBox* toggle = new QCheckBox(tr("启用分析"), row);
        toggle->setChecked(feature_checked_);
        toggle->setToolTip(tr("播放时实时采集，需要额外的解码与逐帧比较，默认关闭，按需开启。"));
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_checked_ = checked;
            emit FeatureToggled(checked);
        });
        rl->addWidget(toggle);
        // 页头唯一的「导出」入口（原来是表格下方单独一个按钮）
        rl->addWidget(MakeExportMenuButton(
            row, {{tr("切换点 CSV…"), [this]() { OnExportCsv(); }, tr("导出检测到的镜头切换点与切换强度")}}));
        layout->addWidget(row);
    }

    summary_label_ = new QLabel(tr("开启分析后播放视频，将在此列出检测到的镜头切换点（时间戳 + 切换强度）。"
                                   "仅统计播放经过的区间，拖动进度条跳过的部分不会补算。"));
    summary_label_->setObjectName(QStringLiteral("HintLabel"));
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    // 切换强度柱状图
    chart_ = new MetricChartWidget(this);
    chart_->SetTitle(tr("切换强度（按时间分布）"));
    chart_->SetLegendVisible(false);
    series_ = chart_->AddBarSeries(tr("强度"), QColor("#8e24aa"));
    axis_x_ = chart_->AxisX();
    axis_y_ = chart_->AxisY();
    axis_x_->SetLabelFormat("%.0f");
    axis_x_->SetTitleText(tr("时间（秒）"));
    axis_y_->SetTitleText(tr("强度"));
    axis_y_->SetRange(0, 1);
    chart_->setMinimumHeight(180);
    chart_->setToolTip(tr("滚轮缩放时间轴，左键拖动平移，双击复位；单击任意位置跳转到该时刻。"));
    connect(chart_, &MetricChartWidget::PlotClicked, this, &SceneChangePage::SeekRequested);
    layout->addWidget(chart_);

    // 切换点列表
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("帧序号"), tr("时间戳"), tr("切换强度")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setMinimumHeight(200);
    table_->setToolTip(tr("点击任意行可定位到该切换点"));
    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        const std::size_t index = RecordIndexForRow(row);
        if (index < records_.size())
            emit SeekRequested(records_[index].timestamp);
    });
    layout->addWidget(table_);

    // 建好之后由 AnalysisPanel::AddPageWithScroll 收进外部 stack
}

void SceneChangePage::AppendResult(const model::SceneChangeResult& result) {
    records_.push_back(result);
    table_dirty_ = true; // 真正刷新等面板的批量定时器，避免每个切换点重绘一次
}

void SceneChangePage::FlushPending() {
    if (!table_dirty_)
        return;
    table_dirty_ = false;
    const size_t total = records_.size();
    for (size_t i = table_synced_count_; i < total; ++i) {
        AppendRow(records_[i]);
    }
    table_synced_count_ = total;
    UpdateChart();
    UpdateSummary();
}

void SceneChangePage::AppendRow(const model::SceneChangeResult& result) {
    if (table_->rowCount() >= kMaxTableRows)
        table_->removeRow(0);
    const int row = table_->rowCount();
    table_->insertRow(row);
    SetTableItemText(table_, row, 0, QString::number(result.frame_index));
    SetTableItemText(table_, row, 1, FormatCutTimestamp(result.timestamp));
    SetTableItemText(table_, row, 2, QString::number(result.score, 'f', 3));
}

std::size_t SceneChangePage::RecordIndexForRow(int row) const {
    const int rows = table_ ? table_->rowCount() : 0;
    if (row < 0 || rows <= 0)
        return records_.size();
    // 行上限生效后，表里是 records_ 的末尾一段，行号不再从 0 对应。
    const std::size_t first =
        table_synced_count_ > static_cast<std::size_t>(rows) ? table_synced_count_ - static_cast<std::size_t>(rows) : 0;
    const std::size_t index = first + static_cast<std::size_t>(row);
    return index < records_.size() ? index : records_.size();
}

void SceneChangePage::UpdateChart() {
    if (!series_)
        return;
    const std::vector<QPointF> points = BuildChartPoints(records_);
    // 逐点 Append 会每根柱子投一次重绘，攒完一次性提交。
    SeriesBatch batch(series_);
    batch.Reserve(static_cast<int>(points.size()));
    for (const QPointF& p : points)
        batch.Add(p.x(), p.y());
    // 横轴量程交给数据自动定：这里不给 SetRange，否则换文件后旧范围会残留。
}

void SceneChangePage::UpdateSummary() {
    const int n = static_cast<int>(records_.size());
    if (n == 0) {
        summary_label_->setText(tr("开启分析后播放视频，将在此列出检测到的镜头切换点（时间戳 + 切换强度）。"));
        return;
    }
    double sum = 0.0;
    double max_s = 0.0;
    for (const auto& r : records_) {
        sum += r.score;
        if (r.score > max_s)
            max_s = r.score;
    }
    QString text = tr("共检测到 %1 个切换点 | 平均强度 %2 | 最大强度 %3")
                       .arg(n)
                       .arg(QString::number(sum / n, 'f', 3))
                       .arg(QString::number(max_s, 'f', 3));
    // 只统计已解码区间：拖过的部分没有逐帧比较，不能让人误以为这是全片结论。
    text += tr(" | 仅统计已解码区间，跳过的部分未覆盖");
    if (n > kMaxTableRows)
        text += tr(" | 表格仅显示最近 %1 条，导出为全集").arg(kMaxTableRows);
    summary_label_->setText(text);
}

void SceneChangePage::OnExportCsv() {
    if (records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的切换点数据。"));
        return;
    }
    ExportCsvStream(
        this, tr("导出切换点 CSV"), source_path_.section('/', -1) + "_scenecut.csv",
        [this](QTextStream& stream) {
            stream << "frame_index,timestamp_seconds,score\n";
            for (const auto& r : records_) {
                stream << r.frame_index << "," << QString::number(r.timestamp, 'f', 3) << ","
                       << QString::number(r.score, 'f', 4) << "\n";
            }
        },
        static_cast<int>(records_.size()));
}

void SceneChangePage::Reset() {
    records_.clear();
    table_dirty_ = false;
    table_synced_count_ = 0;
    if (table_)
        table_->setRowCount(0);
    // 换文件时清掉上一份数据的局部缩放，避免旧视窗残留在新曲线上。
    if (chart_)
        chart_->ResetView();
    UpdateChart();
    UpdateSummary();
}

} // namespace ui
} // namespace videoeye
