#include "ui/analysis_panel/VisualDefectPage.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

#include <algorithm>
#include <cmath>

namespace videoeye {
namespace ui {

VisualDefectPage::VisualDefectPage(bool feature_checked, const model::VisualDefectOptions& options,
                                   QWidget* parent)
    : QWidget(parent), feature_checked_(feature_checked), options_(options) {
    SetupUi();
}

void VisualDefectPage::SetupUi() {
    // 页面本体就是 QWidget（外部 QStackedWidget 的一页），不再多包一层
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 启用开关
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(
            tr("画面质量（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 过曝欠曝 / 色偏 / 隔行梳齿 / 黑边）"), row);
        title->setWordWrap(true);
        rl->addWidget(title);
        rl->addStretch();
        QCheckBox* toggle = new QCheckBox(tr("启用分析"), row);
        toggle->setObjectName(QStringLiteral("VisualDefectEnable"));
        toggle->setChecked(feature_checked_);
        toggle->setToolTip(tr("播放时对视频帧做画面体检，需要额外的降采样与边缘统计，默认关闭，按需开启。"));
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_checked_ = checked;
            emit FeatureToggled(checked);
        });
        rl->addWidget(toggle);
        layout->addWidget(row);
    }

    // 选项: 采样档位 / 模糊阈值 / 冻结阈值 / 是否采集缩略图
    {
        QHBoxLayout* ol = new QHBoxLayout();
        ol->setContentsMargins(0, 0, 0, 0);

        ol->addWidget(new QLabel(tr("采样档位:"), this));
        preset_combo_ = new QComboBox(this);
        // objectName: UI 单测靠 findChild() 精确定位（页面里 QComboBox / QDoubleSpinBox
        // 都不止一个），不给名字就只能靠创建顺序猜，猜中了也是假绿。
        preset_combo_->setObjectName(QStringLiteral("VisualDefectPreset"));
        preset_combo_->addItem(
            tr("快速 (1 帧/秒)"), static_cast<int>(model::VisualSamplingPreset::Fast));
        preset_combo_->addItem(
            tr("标准 (2 帧/秒)"), static_cast<int>(model::VisualSamplingPreset::Standard));
        preset_combo_->addItem(
            tr("精细 (5 帧/秒)"), static_cast<int>(model::VisualSamplingPreset::Fine));
        preset_combo_->addItem(
            tr("离线全帧 (逐帧，不丢帧)"), static_cast<int>(model::VisualSamplingPreset::OfflineFull));
        // 跟随传入的 options.preset。旧代码写死 setCurrentIndex(1)，把调用方显式
        // 传进来的档位悄悄留在「标准」——界面显示与实际送进分析器的档位分家
        // （docs/ARCHITECTURE.md §5.4）。用 findData 按 userData 定位而不是硬编码
        // 索引，将来枚举与下拉项顺序调整也不会错位；未知档位退回默认项「标准」。
        const int preset_index = preset_combo_->findData(static_cast<int>(options_.preset));
        preset_combo_->setCurrentIndex(preset_index >= 0 ? preset_index : 1);
        preset_combo_->setToolTip(
            tr("播放时默认抽样分析；分析队列有上限，压力大会丢分析帧但绝不拖慢播放。\n"
               "「离线全帧」逐帧同步分析，适合不追求播放流畅度的全检场景。"));
        connect(preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &VisualDefectPage::OnOptionChanged);
        ol->addWidget(preset_combo_);

        ol->addWidget(new QLabel(tr("模糊阈值:"), this));
        blur_spin_ = new QDoubleSpinBox(this);
        blur_spin_->setObjectName(QStringLiteral("VisualDefectBlurThreshold"));
        blur_spin_->setRange(0.05, 50.0);
        blur_spin_->setDecimals(2);
        blur_spin_->setSingleStep(0.25);
        blur_spin_->setValue(options_.blur_threshold);
        blur_spin_->setToolTip(tr("锐度指数（拉普拉斯方差/1000）低于该值判模糊。调大更灵敏。"));
        connect(blur_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &VisualDefectPage::OnOptionChanged);
        ol->addWidget(blur_spin_);

        ol->addWidget(new QLabel(tr("冻结阈值:"), this));
        freeze_spin_ = new QDoubleSpinBox(this);
        freeze_spin_->setObjectName(QStringLiteral("VisualDefectFreezeThreshold"));
        freeze_spin_->setRange(0.001, 0.2);
        freeze_spin_->setDecimals(3);
        freeze_spin_->setSingleStep(0.001);
        freeze_spin_->setValue(options_.freeze_diff);
        freeze_spin_->setToolTip(tr("相邻采样帧的平均像素差低于该值判冻结（0.01 约等于 2.5 个灰阶）。"));
        connect(freeze_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &VisualDefectPage::OnOptionChanged);
        ol->addWidget(freeze_spin_);

        rgb_check_ = new QCheckBox(tr("采集缩略图（色偏 / 证据）"), this);
        rgb_check_->setObjectName(QStringLiteral("VisualDefectCaptureRgb"));
        rgb_check_->setChecked(options_.capture_rgb);
        rgb_check_->setToolTip(tr("多一次 RGB 降采样，用于色偏判定与缺陷证据图导出。"));
        connect(rgb_check_, &QCheckBox::toggled, this, &VisualDefectPage::OnOptionChanged);
        ol->addWidget(rgb_check_);

        ol->addStretch();
        layout->addLayout(ol);
    }

    summary_label_ = new QLabel(
        tr("开启分析后播放视频，将在此列出人眼可见的画面问题（黑场、冻结、马赛克、模糊、闪烁、曝光、色偏、梳齿、黑边）。"
           "点击列表行可跳转播放器。"), this);
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    // 曲线 1: 亮度 + 黑场比例（双 Y 轴）
    luma_chart_ = new MetricChartWidget(this);
    luma_chart_->SetTitle(tr("亮度与黑场比例"));
    luma_series_ = luma_chart_->AddLineSeries(tr("亮度均值"), QColor("#fdd835"));
    black_series_ = luma_chart_->AddLineSeries(tr("黑像素比例"), QColor("#e53935"));
    black_series_->SetValueAxisIndex(1);
    luma_chart_->SetAxisY2Visible(true);
    luma_chart_->AttachAxis(black_series_, luma_chart_->AxisY2());
    luma_chart_->AxisX()->SetTitleText(tr("时间 (秒)"));
    luma_chart_->AxisY()->SetTitleText(tr("亮度"));
    luma_chart_->AxisY()->SetRange(0, 255);
    luma_chart_->AxisY2()->SetTitleText(tr("黑像素比例"));
    luma_chart_->AxisY2()->SetRange(0, 1);
    luma_chart_->setMinimumHeight(170);
    layout->addWidget(luma_chart_);

    // 曲线 2: 锐度 + 帧差
    sharp_chart_ = new MetricChartWidget(this);
    sharp_chart_->SetTitle(tr("锐度与帧间差异"));
    blur_series_ = sharp_chart_->AddLineSeries(tr("锐度指数"), QColor("#42a5f5"));
    diff_series_ = sharp_chart_->AddLineSeries(tr("帧间差异"), QColor("#8e24aa"));
    sharp_chart_->AxisX()->SetTitleText(tr("时间 (秒)"));
    sharp_chart_->AxisY()->SetTitleText(tr("指标值"));
    sharp_chart_->setMinimumHeight(170);
    layout->addWidget(sharp_chart_);

    // 缺陷列表 + 证据预览
    {
        QHBoxLayout* dl = new QHBoxLayout();
        table_ = new QTableWidget(0, 7, this);
        table_->setHorizontalHeaderLabels(
            {tr("类型"), tr("严重度"), tr("开始"), tr("结束"), tr("时长"), tr("触发值"), tr("说明")});
        table_->verticalHeader()->setVisible(false);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->setMinimumHeight(220);
        connect(table_, &QTableWidget::cellClicked, this, &VisualDefectPage::OnCellClicked);
        connect(table_, &QTableWidget::itemSelectionChanged, this, &VisualDefectPage::OnSelectionChanged);
        dl->addWidget(table_, 1);

        QVBoxLayout* el = new QVBoxLayout();
        el->addWidget(new QLabel(tr("证据缩略图"), this));
        evidence_label_ = new QLabel(this);
        evidence_label_->setFixedSize(192, 120);
        evidence_label_->setAlignment(Qt::AlignCenter);
        evidence_label_->setStyleSheet(QStringLiteral("background:#1e1e1e; color:#9e9e9e;"));
        evidence_label_->setText(tr("选中缺陷后显示"));
        el->addWidget(evidence_label_);
        el->addStretch();
        dl->addLayout(el);
        layout->addLayout(dl, 1);
    }

    // 导出
    {
        QHBoxLayout* bl = new QHBoxLayout();
        bl->addStretch();
        QPushButton* export_csv = new QPushButton(tr("导出缺陷 CSV"), this);
        connect(export_csv, &QPushButton::clicked, this, &VisualDefectPage::OnExportCsv);
        bl->addWidget(export_csv);
        QPushButton* export_png = new QPushButton(tr("导出证据缩略图"), this);
        connect(export_png, &QPushButton::clicked, this, &VisualDefectPage::OnExportEvidence);
        bl->addWidget(export_png);
        layout->addLayout(bl);
    }

    // 建好之后由 AnalysisPanel::AddPageWithScroll 收进外部 stack
}

void VisualDefectPage::ApplyOptionsFromUi() {
    options_.blur_threshold = blur_spin_ ? blur_spin_->value() : options_.blur_threshold;
    options_.freeze_diff = freeze_spin_ ? freeze_spin_->value() : options_.freeze_diff;
    options_.capture_rgb = rgb_check_ ? rgb_check_->isChecked() : options_.capture_rgb;
    if (preset_combo_) {
        const int preset = preset_combo_->currentData().toInt();
        options_.preset = static_cast<model::VisualSamplingPreset>(preset);
        // 档位自带采样率与分析宽度，改档位时清掉可能的显式覆盖值
        options_.sample_fps = 0.0;
        options_.analysis_width = 0;
    }
}

void VisualDefectPage::ApplyStats(int analyzed_frames, int dropped_frames,
                                  const model::ActivePictureArea& effective_area) {
    analyzed_frames_ = analyzed_frames;
    dropped_frames_ = dropped_frames;
    effective_area_ = effective_area;
    dirty_ = true;
}

void VisualDefectPage::OnOptionChanged() {
    if (updating_options_) return;
    ApplyOptionsFromUi();
    emit OptionsChanged(options_);
}

void VisualDefectPage::AppendFrame(const model::FrameQualityMetric& metric) {
    samples_.push_back(metric);
    // 长播放时限制内存：窗口滑出最早的一批（保留最新 2 万个采样点）
    if (samples_.size() > 20000) {
        samples_.erase(samples_.begin(), samples_.begin() + 4000);
    }
    dirty_ = true;
}

void VisualDefectPage::AppendDefect(const model::VisualDefect& defect) {
    records_.push_back(defect);
    dirty_ = true;
}

void VisualDefectPage::ResetAll() {
    samples_.clear();
    records_.clear();
    effective_area_ = model::ActivePictureArea();
    dropped_frames_ = 0;
    analyzed_frames_ = 0;
    dirty_ = false;
    if (luma_chart_) luma_chart_->ClearSeriesData();
    if (sharp_chart_) sharp_chart_->ClearSeriesData();
    if (table_) table_->setRowCount(0);
    UpdateSummary();
}

void VisualDefectPage::FlushPending() {
    if (!dirty_) return;
    dirty_ = false;
    UpdateSummary();
    UpdateCharts();
    RebuildTable();
}

void VisualDefectPage::UpdateSummary() {
    if (!summary_label_) return;

    if (samples_.empty() && records_.empty()) {
        summary_label_->setText(
            tr("暂无画面质量数据。开启分析后播放视频即可开始采集。"));
        return;
    }

    int warning_or_above = 0;
    for (const auto& d : records_) {
        if (d.severity != model::VisualDefectSeverity::Info) ++warning_or_above;
    }

    QString area_text = tr("未检出");
    if (effective_area_.valid) {
        area_text = QStringLiteral("%1x%2 (偏移 %3,%4)")
                        .arg(effective_area_.width)
                        .arg(effective_area_.height)
                        .arg(effective_area_.x)
                        .arg(effective_area_.y);
    }

    summary_label_->setText(
        tr("已分析 %1 帧，缺陷 %2 条（其中警告及以上 %3 条）；有效画面区域: %4%5")
            .arg(analyzed_frames_)
            .arg(records_.size())
            .arg(warning_or_above)
            .arg(area_text)
            .arg(dropped_frames_ > 0
                     ? tr("；播放压力大，已丢弃 %1 个分析帧").arg(dropped_frames_)
                     : QString()));
}

void VisualDefectPage::UpdateCharts() {
    if (!luma_chart_ || !sharp_chart_) return;

    const size_t total = samples_.size();
    const size_t max_points = 600;
    const size_t stride = (total > max_points) ? (total + max_points - 1) / max_points : 1;

    {
        SeriesBatch luma(luma_series_);
        SeriesBatch black(black_series_);
        luma.Reserve(static_cast<int>(total / stride + 1));
        black.Reserve(static_cast<int>(total / stride + 1));
        for (size_t i = 0; i < total; i += stride) {
            const auto& s = samples_[i];
            luma.Add(s.timestamp_seconds, s.luma_mean);
            black.Add(s.timestamp_seconds, s.black_ratio);
        }
    }
    {
        SeriesBatch blur(blur_series_);
        SeriesBatch diff(diff_series_);
        blur.Reserve(static_cast<int>(total / stride + 1));
        diff.Reserve(static_cast<int>(total / stride + 1));
        for (size_t i = 0; i < total; i += stride) {
            const auto& s = samples_[i];
            // 锐度可能远大于 1（清晰画面），放进同一张图会压平帧差曲线，这里做对数压缩
            const double blur_display = std::log10(1.0 + std::max(0.0, s.blur_score));
            blur.Add(s.timestamp_seconds, blur_display);
            diff.Add(s.timestamp_seconds, s.frame_diff);
        }
    }
}

void VisualDefectPage::RebuildTable() {
    if (!table_) return;

    const int rows = static_cast<int>(records_.size());
    TableBatch batch(table_);
    batch.SetRowCount(rows);
    for (int r = 0; r < rows; ++r) {
        const auto& d = records_[static_cast<size_t>(r)];
        batch.SetText(r, 0, QString::fromUtf8(model::ToString(d.type)));
        batch.SetText(r, 1, QString::fromUtf8(model::ToString(d.severity)));
        batch.SetText(r, 2, QString::number(d.start_seconds, 'f', 2));
        batch.SetText(r, 3, QString::number(d.end_seconds, 'f', 2));
        batch.SetText(r, 4, QString::number(d.DurationSeconds(), 'f', 2));
        batch.SetText(r, 5, QString::number(d.score, 'f', 4));
        batch.SetText(r, 6, QString::fromUtf8(d.description.c_str()));
    }
}

QImage VisualDefectPage::EvidenceImage(const model::VisualDefect& defect) {
    if (!defect.evidence.valid()) return QImage();
    QImage img(defect.evidence.rgb.data(), defect.evidence.width, defect.evidence.height,
               defect.evidence.width * 3, QImage::Format_RGB888);
    return img.copy();   // 深拷贝: 源数据属于缺陷记录，随时可能被清掉
}

void VisualDefectPage::UpdateEvidencePreview(int defect_index) {
    if (!evidence_label_) return;
    if (defect_index < 0 || defect_index >= static_cast<int>(records_.size())) {
        evidence_label_->setPixmap(QPixmap());
        evidence_label_->setText(tr("选中缺陷后显示"));
        return;
    }
    const auto& d = records_[static_cast<size_t>(defect_index)];
    const QImage img = EvidenceImage(d);
    if (img.isNull()) {
        evidence_label_->setPixmap(QPixmap());
        evidence_label_->setText(tr("该缺陷无证据图\n（未采集缩略图）"));
        return;
    }
    evidence_label_->setText(QString());
    evidence_label_->setPixmap(
        QPixmap::fromImage(img.scaled(evidence_label_->size(),
                                      Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void VisualDefectPage::OnCellClicked(int row, int) {
    if (row < 0 || row >= static_cast<int>(records_.size())) return;
    emit SeekRequested(records_[static_cast<size_t>(row)].start_seconds);
}

void VisualDefectPage::OnSelectionChanged() {
    if (!table_) return;
    const auto selected = table_->selectionModel()->selectedRows();
    UpdateEvidencePreview(selected.isEmpty() ? -1 : selected.first().row());
}

void VisualDefectPage::OnExportCsv() {
    if (records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有画面质量缺陷数据可导出"));
        return;
    }
    ExportCsvStream(this, tr("导出画面质量缺陷 CSV"), QStringLiteral("visual_defects.csv"),
        [this](QTextStream& stream) {
            stream << "序号,类型,类型代码,严重度,开始(秒),结束(秒),时长(秒),触发值,阈值,说明\n";
            for (const auto& d : records_) {
                stream << d.id << ","
                       << QString::fromUtf8(model::ToString(d.type)) << ","
                       << QString::fromUtf8(model::DefectTypeCode(d.type)) << ","
                       << QString::fromUtf8(model::ToString(d.severity)) << ","
                       << QString::number(d.start_seconds, 'f', 3) << ","
                       << QString::number(d.end_seconds, 'f', 3) << ","
                       << QString::number(d.DurationSeconds(), 'f', 3) << ","
                       << QString::number(d.score, 'f', 6) << ","
                       << QString::number(d.threshold, 'f', 6) << ","
                       << QString::fromUtf8(d.description.c_str()) << "\n";
            }
        },
        static_cast<int>(records_.size()));
}

void VisualDefectPage::OnExportEvidence() {
    int available = 0;
    for (const auto& d : records_) {
        if (d.evidence.valid()) ++available;
    }
    if (available == 0) {
        QMessageBox::information(this, tr("提示"),
            tr("没有可导出的证据图。请勾选「采集缩略图（色偏 / 证据）」后重新分析。"));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择证据图导出目录"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) return;

    int written = 0;
    for (const auto& d : records_) {
        const QImage img = EvidenceImage(d);
        if (img.isNull()) continue;
        const QString name = QStringLiteral("%1_%2_%3s.png")
                                 .arg(d.id, 3, 10, QLatin1Char('0'))
                                 .arg(QString::fromUtf8(model::DefectTypeCode(d.type)))
                                 .arg(QString::number(d.start_seconds, 'f', 2));
        if (img.save(dir + QStringLiteral("/") + name, "PNG")) ++written;
    }
    QMessageBox::information(this, tr("成功"),
        tr("已导出 %1 / %2 张证据图到:\n%3").arg(written).arg(available).arg(dir));
}

} // namespace ui
} // namespace videoeye
