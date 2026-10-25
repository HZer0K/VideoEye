#include "ui/analysis_panel/AudioVerdictView.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {
namespace {

// 参与「音频 QC → 规则结果」展示的规则 id（与 QcModels.cpp 的 audio.* 保持一致）
const char* const kAudioQcRuleIds[] = {
    "audio.loudness.target_high",
    "audio.loudness.target_low",
    "audio.loudness.range",
    "audio.true_peak",
    "audio.clipping",
    "audio.silence.longest",
    "audio.silence.ratio",
    "audio.dc_offset",
    "audio.phase_correlation",
    "audio.metadata.layout",
    "audio.metadata.duration_mismatch",
    // 覆盖完整性：不解码成功的部分不进统计，必须让用户看见"这条结论只覆盖多少时长"
    "audio.qc.incomplete",
};

QString AudioVerdictText(const model::QcReport& report, const QString& rule_id, QString* detail_out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id.toStdString())
            continue;
        if (detail_out)
            *detail_out = QString::fromStdString(issue.detail);
        switch (issue.severity) {
        case model::IssueSeverity::Critical:
        case model::IssueSeverity::Error:
            return QStringLiteral("失败");
        case model::IssueSeverity::Warning:
            return QStringLiteral("警告");
        default:
            return QStringLiteral("提示");
        }
    }
    return QStringLiteral("通过");
}

} // namespace

AudioVerdictView::AudioVerdictView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void AudioVerdictView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    QLabel* hint = new QLabel(tr("判定阈值取自「诊断与报告 → 规则与阈值」，改完会立即重算；"
                                 "这些音频问题也会一并进入诊断报告的评分。"),
                              this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    verdict_table_ = new QTableWidget(0, 4, this);
    verdict_table_->setHorizontalHeaderLabels({tr("规则"), tr("判定"), tr("实测值"), tr("说明")});
    verdict_table_->verticalHeader()->setVisible(false);
    verdict_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    verdict_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    verdict_table_->horizontalHeader()->setStretchLastSection(true);
    verdict_table_->setMinimumHeight(220);
    layout->addWidget(verdict_table_);
}

void AudioVerdictView::SetQcReport(const model::QcReport& report) {
    if (!verdict_table_)
        return;
    verdict_table_->setRowCount(0);

    const size_t count = sizeof(kAudioQcRuleIds) / sizeof(kAudioQcRuleIds[0]);
    verdict_table_->setRowCount(static_cast<int>(count));
    for (size_t i = 0; i < count; ++i) {
        const QString id = QString::fromUtf8(kAudioQcRuleIds[i]);
        QString name = id;
        QString threshold_text;
        for (const auto& rule : report.rules) {
            if (rule.id == id.toStdString()) {
                name = QString::fromStdString(rule.name);
                threshold_text = QString::fromStdString(
                    rule.unit.empty()
                        ? QString::number(rule.threshold, 'f', 2).toStdString()
                        : (QString::number(rule.threshold, 'f', 2) + QString::fromStdString(rule.unit)).toStdString());
                break;
            }
        }
        QString detail;
        const QString verdict = AudioVerdictText(report, id, &detail);
        SetTableItemText(verdict_table_, static_cast<int>(i), 0, name);
        SetTableItemText(verdict_table_, static_cast<int>(i), 1, verdict);
        SetTableItemText(verdict_table_, static_cast<int>(i), 2, threshold_text);
        SetTableItemText(verdict_table_, static_cast<int>(i), 3, detail);
        QTableWidgetItem* item = verdict_table_->item(static_cast<int>(i), 1);
        if (item != nullptr) {
            item->setForeground(verdict == tr("失败")   ? QColor("#e53935")
                                : verdict == tr("警告") ? QColor("#fb8c00")
                                : verdict == tr("提示") ? QColor("#1e88e5")
                                                        : QColor("#43a047"));
        }
    }
}

int AudioVerdictView::RuleCount() {
    return static_cast<int>(sizeof(kAudioQcRuleIds) / sizeof(kAudioQcRuleIds[0]));
}

} // namespace ui
} // namespace videoeye
