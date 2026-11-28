#include "ui/analysis_panel/ReferenceQualityPage.h"

#include <QAbstractItemView>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include "core/analysis/quality/ReferenceQualityRunner.h"
#include "core/reporting/ReferenceQualityExporter.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/main_window/media_file_filter.h"

namespace videoeye {
namespace ui {
namespace {

// 指标值的展示口径（与导出层一致）:
//   NaN = 没有这个值（没对上 / 比较失败）→ 长破折号
//   +inf = 两帧逐像素一致 → ∞（无损）。写"∞"而不是空，是因为"完全一样"是个
//          真结论，不该看起来像"没算"。
QString FormatPsnr(double value) {
    if (std::isnan(value))
        return QStringLiteral("—");
    if (std::isinf(value))
        return value > 0 ? QStringLiteral("∞（无损）") : QStringLiteral("-∞");
    return QString::number(value, 'f', 2);
}

QString FormatSsim(double value) {
    if (std::isnan(value))
        return QStringLiteral("—");
    if (std::isinf(value))
        return value > 0 ? QStringLiteral("∞") : QStringLiteral("-∞");
    return QString::number(value, 'f', 4);
}

} // namespace

ReferenceQualityPage::ReferenceQualityPage(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

ReferenceQualityPage::~ReferenceQualityPage() {
    // 先把运行身份作废，迟到的消息就都落空；再取消 + join —— 只置取消不 join 会让
    // 线程活到页面析构之后，它下一次访问成员就是悬垂。
    ++run_generation_;
    cancel_.store(true, std::memory_order_relaxed);
    if (worker_.joinable())
        worker_.join();
}

void ReferenceQualityPage::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // ---- 页头：标题 + 开始/取消 + 唯一的「导出」入口 ----
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("参考帧质量对比"), row);
        title->setObjectName(QStringLiteral("PageTitle"));
        rl->addWidget(title);
        rl->addStretch();

        start_button_ = new QPushButton(tr("开始分析"), row);
        start_button_->setToolTip(tr("用参考视频作基准，按采样帧率逐帧对齐待测视频，比较 PSNR / SSIM。"
                                     "与其它页的全文件扫描互不影响，也不会占用它们的扫描任务。"));
        connect(start_button_, &QPushButton::clicked, this, &ReferenceQualityPage::StartComparison);
        rl->addWidget(start_button_);

        cancel_button_ = new QPushButton(tr("取消"), row);
        cancel_button_->setEnabled(false);
        connect(cancel_button_, &QPushButton::clicked, this, &ReferenceQualityPage::CancelComparison);
        rl->addWidget(cancel_button_);

        rl->addWidget(MakeExportMenuButton(row, {{tr("对比明细 CSV…"), [this]() { OnExportCsv(); },
                                                  tr("导出逐帧 PSNR / SSIM 明细与汇总（UTF-8 CSV）")},
                                                 {tr("对比结果 JSON…"), [this]() { OnExportJson(); },
                                                  tr("导出同一份结果的 JSON（参数 / 汇总 / 逐帧指标）")}}));
        layout->addWidget(row);
    }

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(progress_bar_);

    // ---- 两个输入 ----
    //
    // 文本框只读：路径只能经「选择…」写入。手输路径在多平台（中文 / 网络盘 / 相对路径）
    // 上几乎总会出现"看起来对、打不开"的情况，而失败信息在解码层，用户很难对上。
    auto make_path_row = [this](const QString& caption, const QString& placeholder, const QString& tooltip,
                                QLineEdit** edit, QPushButton** pick, void (ReferenceQualityPage::*handler)()) {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* label = new QLabel(caption, row);
        label->setMinimumWidth(72);
        rl->addWidget(label);
        *edit = new QLineEdit(row);
        (*edit)->setReadOnly(true);
        (*edit)->setPlaceholderText(placeholder);
        (*edit)->setToolTip(tooltip);
        rl->addWidget(*edit, 1);
        *pick = new QPushButton(tr("选择…"), row);
        connect(*pick, &QPushButton::clicked, this, handler);
        rl->addWidget(*pick);
        return row;
    };

    QPushButton* reference_pick = nullptr;
    QPushButton* distorted_pick = nullptr;
    layout->addWidget(make_path_row(tr("参考视频"), tr("尚未选择（基准视频）"),
                                    tr("参考视频是基准：例如原始母版、或上一版成片。"), &reference_edit_,
                                    &reference_pick, &ReferenceQualityPage::PickReference));
    layout->addWidget(make_path_row(tr("待测视频"), tr("尚未选择（要检查的那一份）"),
                                    tr("待测视频会被按采样栅格对齐到参考视频上。"), &distorted_edit_, &distorted_pick,
                                    &ReferenceQualityPage::PickDistorted));

    // ---- 参数 ----
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        auto add_field = [row, rl](const QString& caption, QWidget* field) {
            rl->addWidget(new QLabel(caption, row));
            rl->addWidget(field);
        };

        fps_spin_ = new QDoubleSpinBox(row);
        fps_spin_->setRange(0.1, 30.0);
        fps_spin_->setDecimals(1);
        fps_spin_->setSingleStep(0.5);
        fps_spin_->setValue(2.0);
        fps_spin_->setSuffix(tr(" fps"));
        fps_spin_->setToolTip(tr("参考视频每秒取几帧来比。越高越细，但耗时与结果行数成正比；"
                                 "高于源帧率时退化为逐帧。"));
        add_field(tr("采样帧率"), fps_spin_);

        offset_spin_ = new QDoubleSpinBox(row);
        offset_spin_->setRange(-60.0, 60.0);
        offset_spin_->setDecimals(3);
        offset_spin_->setSingleStep(0.1);
        offset_spin_->setValue(0.0);
        offset_spin_->setSuffix(tr(" s"));
        offset_spin_->setToolTip(tr("待测视频相对参考视频的整体时间偏移：>0 表示待测内容出现得更晚"
                                    "（例如片头多了几帧黑场）。"));
        add_field(tr("时间偏移"), offset_spin_);

        tolerance_spin_ = new QDoubleSpinBox(row);
        tolerance_spin_->setRange(0.0, 1.0);
        tolerance_spin_->setDecimals(3);
        tolerance_spin_->setSingleStep(0.001);
        tolerance_spin_->setValue(0.042);
        tolerance_spin_->setSuffix(tr(" s"));
        tolerance_spin_->setToolTip(tr("单帧对齐容差：容差内找不到待测帧就记「跳过」，绝不拿别处画面凑数。"
                                       "默认 0.042s 约等于 24fps 一帧。"));
        add_field(tr("对齐容差"), tolerance_spin_);

        sample_width_spin_ = new QSpinBox(row);
        sample_width_spin_->setRange(32, 1920);
        sample_width_spin_->setSingleStep(32);
        sample_width_spin_->setValue(320);
        sample_width_spin_->setToolTip(tr("整帧缩放到这个宽度后再算指标（两侧相同）。"
                                          "改小更快，改大更接近原始清晰度差异。"));
        add_field(tr("采样宽度"), sample_width_spin_);

        max_frames_spin_ = new QSpinBox(row);
        max_frames_spin_->setRange(0, 100000);
        max_frames_spin_->setSingleStep(100);
        max_frames_spin_->setValue(0);
        max_frames_spin_->setSpecialValueText(tr("不限"));
        max_frames_spin_->setToolTip(tr("最多比较多少帧，0 = 不限。长素材先用一个上限试跑很划算。"));
        add_field(tr("最大帧数"), max_frames_spin_);

        rl->addStretch();
        layout->addWidget(row);
    }

    summary_label_ = new QLabel(
        tr("选择参考视频与待测视频后点「开始分析」：参考视频是基准，待测视频按采样栅格逐帧对齐后比较 PSNR / SSIM。"),
        this);
    summary_label_->setObjectName(QStringLiteral("HintLabel"));
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    // ---- 结果：走势 + 逐帧明细 ----
    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setMinimumHeight(340);

    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        chart_ = new MetricChartWidget(page);
        chart_->setMinimumHeight(280);
        chart_->SetTitle(tr("逐帧 PSNR / SSIM"));
        psnr_series_ = chart_->AddLineSeries(tr("PSNR (dB)"), QColor("#42a5f5"));
        ssim_series_ = chart_->AddLineSeries(tr("SSIM"), QColor("#43a047"));
        chart_->SetAxisY2Visible(true);
        chart_->AttachAxis(ssim_series_, chart_->AxisY2());
        chart_->AxisX()->SetTitleText(tr("时间 (秒)"));
        chart_->AxisY()->SetTitleText(tr("PSNR (dB)"));
        chart_->AxisY2()->SetTitleText(tr("SSIM"));
        chart_->AxisY2()->SetRange(0.0, 1.0);
        pl->addWidget(chart_);
        sub_tabs_->addTab(page, tr("指标走势"));
    }

    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        frame_hint_ = new QLabel(tr("逐帧明细会在对比完成后出现在这里。"), page);
        frame_hint_->setObjectName(QStringLiteral("HintLabel"));
        frame_hint_->setWordWrap(true);
        pl->addWidget(frame_hint_);

        frame_table_ = new QTableWidget(0, 7, page);
        frame_table_->setHorizontalHeaderLabels(
            {tr("参考时间(s)"), tr("待测时间(s)"), tr("偏差(ms)"), tr("尺寸"), tr("PSNR(dB)"), tr("SSIM"), tr("状态")});
        frame_table_->verticalHeader()->setVisible(false);
        frame_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        frame_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        frame_table_->horizontalHeader()->setStretchLastSection(true);
        frame_table_->setMinimumHeight(260);
        pl->addWidget(frame_table_, 1);

        sub_tabs_->addTab(page, tr("逐帧明细"));
    }

    layout->addWidget(sub_tabs_, 1);
}

void ReferenceQualityPage::SetSourcePath(const QString& path) {
    source_path_ = path;
    // 待测视频默认跟当前打开的文件；参考视频保持用户上一次的选择（基准通常不变）。
    distorted_path_ = path;
    if (distorted_edit_)
        distorted_edit_->setText(path);
    ClearResult();
}

void ReferenceQualityPage::ClearResult() {
    // 先作废运行身份再取消：迟到的进度 / 终态消息会因 run_id 不匹配被丢弃。
    ++run_generation_;
    cancel_.store(true, std::memory_order_relaxed);
    if (worker_.joinable())
        worker_.join();
    cancel_.store(false, std::memory_order_relaxed);

    has_result_ = false;
    result_ = model::ReferenceQualityResult();
    SetBusy(false);
    if (progress_bar_) {
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(0);
        progress_bar_->setFormat(tr("未开始"));
    }
    RebuildFrameTable();
    UpdateChart();
    RefreshSummary();
}

void ReferenceQualityPage::SetResult(const model::ReferenceQualityResult& result) {
    result_ = result;
    has_result_ = true;
    RefreshSummary();
    RebuildFrameTable();
    UpdateChart();
}

void ReferenceQualityPage::SetBusy(bool busy) {
    if (start_button_)
        start_button_->setEnabled(!busy);
    if (cancel_button_)
        cancel_button_->setEnabled(busy);
}

ReferenceQualityRunOptions ReferenceQualityPage::CollectRunOptions() const {
    ReferenceQualityRunOptions options;
    options.target_fps = fps_spin_ ? fps_spin_->value() : 2.0;
    options.sample_width = sample_width_spin_ ? sample_width_spin_->value() : 320;
    options.compare.offset_seconds = offset_spin_ ? offset_spin_->value() : 0.0;
    options.compare.alignment_tolerance_seconds = tolerance_spin_ ? tolerance_spin_->value() : 0.042;
    options.compare.max_frames = max_frames_spin_ ? max_frames_spin_->value() : 0;
    return options;
}

void ReferenceQualityPage::StartComparison() {
    if (worker_.joinable())
        return;
    if (reference_path_.isEmpty() || distorted_path_.isEmpty()) {
        QMessageBox::information(this, tr("提示"),
                                 tr("请先选择参考视频与待测视频：参考视频是基准，待测视频是要检查的那一份。"));
        return;
    }

    const ReferenceQualityRunOptions run_options = CollectRunOptions();
    const std::string reference = reference_path_.toUtf8().toStdString();
    const std::string distorted = distorted_path_.toUtf8().toStdString();
    const quint64 run_id = ++run_generation_;

    // 上一份结果立刻作废：新一次运行的数字还没出来时，旧数字留在页面上就会被读成
    // "这次的结果"。进度条切到不确定态，用户看到的至少是"在跑"。
    has_result_ = false;
    result_ = model::ReferenceQualityResult();
    RebuildFrameTable();
    UpdateChart();
    RefreshSummary();

    cancel_.store(false, std::memory_order_relaxed);
    SetBusy(true);
    if (progress_bar_) {
        progress_bar_->setRange(0, 0);
        progress_bar_->setFormat(tr("正在解码与对齐…"));
    }

    worker_ = std::thread([this, run_id, reference, distorted, run_options]() {
        const model::ReferenceQualityResult result = ReferenceQualityRunner::Run(
            reference, distorted, run_options, &cancel_, [this, run_id](const ReferenceQualityRunProgress& progress) {
                const int compared = progress.compared_frames;
                const int skipped = progress.skipped_frames;
                const double seconds = progress.reference_seconds;
                QMetaObject::invokeMethod(
                    this,
                    [this, run_id, compared, skipped, seconds]() {
                        if (run_id != run_generation_ || !progress_bar_)
                            return;
                        progress_bar_->setFormat(tr("已比较 %1 帧 · 跳过 %2 · 参考时间 %3 s")
                                                     .arg(compared)
                                                     .arg(skipped)
                                                     .arg(QString::number(seconds, 'f', 1)));
                    },
                    Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(
            this, [this, run_id, result]() { OnRunFinished(run_id, result); }, Qt::QueuedConnection);
    });
}

void ReferenceQualityPage::CancelComparison() {
    cancel_.store(true, std::memory_order_relaxed);
    if (cancel_button_)
        cancel_button_->setEnabled(false);
    if (progress_bar_)
        progress_bar_->setFormat(tr("正在取消…"));
}

void ReferenceQualityPage::OnRunFinished(quint64 run_id, const model::ReferenceQualityResult& result) {
    // 先比身份再 join：换文件后用新的 run_id 起了新任务时，这条旧消息绝不能去 join
    // 新任务的线程（那会把 UI 卡到新一次运行结束）。
    if (run_id != run_generation_)
        return;
    if (worker_.joinable())
        worker_.join();

    SetBusy(false);
    if (progress_bar_) {
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(100);
        if (!result.valid && !result.cancelled) {
            progress_bar_->setValue(0);
            progress_bar_->setFormat(tr("对比失败"));
        } else {
            progress_bar_->setFormat(result.cancelled ? tr("已取消（保留已完成部分）")
                                                      : tr("完成：已比较 %1 帧").arg(result.summary.compared_frames));
        }
    }
    SetResult(result);
}

void ReferenceQualityPage::RefreshSummary() {
    if (!summary_label_)
        return;
    if (!has_result_) {
        summary_label_->setText(tr("选择参考视频与待测视频后点「开始分析」：参考视频是基准，"
                                   "待测视频按采样栅格逐帧对齐后比较 PSNR / SSIM。"));
        return;
    }

    const model::ReferenceQualitySummary& summary = result_.summary;
    if (!result_.valid || result_.frames.empty()) {
        QString text = result_.error_message.empty() ? tr("本次对比没有拿到可比对的帧。")
                                                     : QString::fromStdString(result_.error_message);
        if (result_.cancelled)
            text = tr("已取消：%1").arg(text);
        summary_label_->setText(text);
        return;
    }

    QStringList parts;
    parts << tr("已比较 %1 / %2 帧（覆盖率 %3%）")
                 .arg(summary.compared_frames)
                 .arg(summary.target_count)
                 .arg(QString::number(summary.coverage * 100.0, 'f', 1));
    parts << tr("平均 PSNR %1 dB").arg(FormatPsnr(summary.mean_psnr_db));
    parts << tr("平均 SSIM %1").arg(FormatSsim(summary.mean_ssim));
    if (summary.min_psnr_frame_index >= 0) {
        parts
            << tr("最低 PSNR %1 dB（第 %2 帧）").arg(FormatPsnr(summary.min_psnr_db)).arg(summary.min_psnr_frame_index);
    }
    if (summary.min_ssim_frame_index >= 0) {
        parts << tr("最低 SSIM %1（第 %2 帧）").arg(FormatSsim(summary.min_ssim)).arg(summary.min_ssim_frame_index);
    }
    parts << tr("跳过 %1 · 失败 %2 · 无损 %3")
                 .arg(summary.skipped_frames)
                 .arg(summary.failed_frames)
                 .arg(summary.lossless_frames);
    if (summary.max_alignment_delta_seconds > 0.0)
        parts << tr("最大对齐偏差 %1 ms").arg(QString::number(summary.max_alignment_delta_seconds * 1000.0, 'f', 1));
    if (summary.duration_mismatch)
        parts << tr("两侧时长差异较大");
    if (summary.truncated)
        parts << tr("已达最大帧数，结果被截断");

    QString text = parts.join(QStringLiteral(" · "));
    if (result_.cancelled)
        text = tr("已取消（以下为已完成部分）：") + text;
    summary_label_->setText(text);
}

void ReferenceQualityPage::RebuildFrameTable() {
    if (!frame_table_)
        return;

    // 表格只是给人看的，上限 2000 行；报告 / CSV 不受这个上限影响（导出完整 frames）。
    constexpr int kMaxRows = 2000;
    const int total = static_cast<int>(result_.frames.size());
    const int rows = std::min(total, kMaxRows);
    {
        TableBatch batch(frame_table_);
        batch.SetRowCount(rows);
        for (int i = 0; i < rows; ++i) {
            const model::ReferenceFrameMetric& frame = result_.frames[static_cast<std::size_t>(i)];
            batch.SetText(i, 0, QString::number(frame.reference_timestamp_seconds, 'f', 3));
            batch.SetText(i, 1, QString::number(frame.distorted_timestamp_seconds, 'f', 3));
            batch.SetText(i, 2, QString::number(frame.alignment_delta_seconds * 1000.0, 'f', 1));
            batch.SetText(i, 3, QStringLiteral("%1×%2").arg(frame.width).arg(frame.height));
            batch.SetText(i, 4, FormatPsnr(frame.psnr_db));
            batch.SetText(i, 5, FormatSsim(frame.ssim));
            batch.SetText(i, 6, frame.valid ? tr("已比较") : QString::fromStdString(frame.error_message));
        }
    }

    if (!frame_hint_)
        return;
    if (total == 0) {
        frame_hint_->setText(has_result_ ? tr("本次对比没有可比对的帧（容差内没对上，或两侧尺寸不一致）。")
                                         : tr("逐帧明细会在对比完成后出现在这里。"));
    } else if (total > rows) {
        frame_hint_->setText(
            tr("共 %1 帧，表格只显示前 %2 帧；完整明细请用「导出 → 对比明细 CSV」。").arg(total).arg(rows));
    } else {
        frame_hint_->setText(tr("共 %1 帧。").arg(total));
    }
}

void ReferenceQualityPage::UpdateChart() {
    if (!chart_ || !psnr_series_ || !ssim_series_)
        return;
    // 旧视窗不带到新数据上（缩放 / 平移状态只在一次结果内有效）。
    chart_->ResetView();
    SeriesBatch psnr(psnr_series_);
    SeriesBatch ssim(ssim_series_);
    psnr.Reserve(static_cast<int>(result_.frames.size()));
    for (const model::ReferenceFrameMetric& frame : result_.frames) {
        // 无损帧的 PSNR 是 +inf，画不出来也压坏量程 —— 只跳过这一点，
        // 「无损」这件事由汇总里的计数说明。
        if (std::isfinite(frame.psnr_db))
            psnr.Add(frame.reference_timestamp_seconds, frame.psnr_db);
        if (std::isfinite(frame.ssim))
            ssim.Add(frame.reference_timestamp_seconds, frame.ssim);
    }
}

void ReferenceQualityPage::PickReference() {
    const QString start_dir = QFileInfo(reference_path_.isEmpty() ? distorted_path_ : reference_path_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("选择参考视频"), start_dir, BuildMediaFileFilter());
    if (path.isEmpty())
        return;
    reference_path_ = path;
    if (reference_edit_)
        reference_edit_->setText(path);
    ClearResult();
}

void ReferenceQualityPage::PickDistorted() {
    const QString start_dir = QFileInfo(distorted_path_.isEmpty() ? source_path_ : distorted_path_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("选择待测视频"), start_dir, BuildMediaFileFilter());
    if (path.isEmpty())
        return;
    distorted_path_ = path;
    if (distorted_edit_)
        distorted_edit_->setText(path);
    ClearResult();
}

QString ReferenceQualityPage::DefaultExportBaseName() const {
    const QString path = distorted_path_.isEmpty() ? source_path_ : distorted_path_;
    const QString stem = QFileInfo(path).completeBaseName();
    return (stem.isEmpty() ? QStringLiteral("reference_quality") : stem) + QStringLiteral("_refcmp");
}

void ReferenceQualityPage::OnExportCsv() {
    if (!has_result_ || result_.frames.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的逐帧对比结果。"));
        return;
    }
    const QString path =
        QFileDialog::getSaveFileName(this, tr("导出参考帧对比 CSV"), DefaultExportBaseName() + QStringLiteral(".csv"),
                                     tr("CSV 文件 (*.csv);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    const reporting::ReferenceQualityExportInput input{reference_path_.toUtf8().toStdString(),
                                                       distorted_path_.toUtf8().toStdString()};
    if (!reporting::ReferenceQualityExporter::ExportCsv(path.toUtf8().toStdString(), result_, input)) {
        QMessageBox::warning(this, tr("错误"), tr("无法写入文件: %1").arg(path));
        return;
    }
    QMessageBox::information(this, tr("成功"), tr("已导出 %1 帧到:\n%2").arg(result_.frames.size()).arg(path));
}

void ReferenceQualityPage::OnExportJson() {
    if (!has_result_ || result_.frames.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的对比结果。"));
        return;
    }
    const QString path =
        QFileDialog::getSaveFileName(this, tr("导出参考帧对比 JSON"), DefaultExportBaseName() + QStringLiteral(".json"),
                                     tr("JSON 文件 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    const reporting::ReferenceQualityExportInput input{reference_path_.toUtf8().toStdString(),
                                                       distorted_path_.toUtf8().toStdString()};
    if (!reporting::ReferenceQualityExporter::ExportJson(path.toUtf8().toStdString(), result_, input)) {
        QMessageBox::warning(this, tr("错误"), tr("无法写入文件: %1").arg(path));
        return;
    }
    QMessageBox::information(this, tr("成功"), tr("已导出到:\n%1").arg(path));
}

} // namespace ui
} // namespace videoeye
