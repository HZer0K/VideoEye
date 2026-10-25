#pragma once

// 「规则结果」子页：音频 QC 的固定规则清单 → 判定 / 阈值 / 说明。
//
// 从 AudioQcPage 拆出的展示型组件。判定阈值取自「诊断与报告 → 规则与阈值」的
// 报告快照（qc_report.rules），所以页面在规则改动后只需把新报告灌进来。

#include <QWidget>

#include <QTableWidget>

#include "core/domain/model/QcReport.h"

namespace videoeye {
namespace ui {

class AudioVerdictView : public QWidget {
    Q_OBJECT

public:
    explicit AudioVerdictView(QWidget* parent = nullptr);

    // 按报告里的 issues / rules 重建判定表（顺序见 RuleCount 对应的固定规则 id 列表）
    void SetQcReport(const model::QcReport& report);

    // 固定展示的音频规则条数（页面对外暴露 VerdictRuleCount 的口径）
    static int RuleCount();

private:
    void SetupUi();

    QTableWidget* verdict_table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
