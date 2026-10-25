#pragma once

// 「规则与阈值」子页：QC 规则表的展示与就地编辑。
//
// 从 DiagnosticsPage 拆出的组件。它不持有规则集：规则集归 AnalysisFacade 所有，
// 通过 RulesProvider 注入进来（表格的勾选 / 阈值改写直接落到这份规则集上）。
// 每次有效改动只发 RulesChanged，由页面决定是否用当前扫描结果重算报告；
// 「恢复默认规则」也只发 ResetRequested —— 默认规则集与重算是页面的职责。

#include <QString>
#include <QWidget>

#include <QTableWidget>

#include <functional>
#include <vector>

#include "core/domain/model/QcRule.h"

namespace videoeye {
namespace ui {

class QcRuleTableView : public QWidget {
    Q_OBJECT

public:
    using RulesProvider = std::function<std::vector<model::QcRule>&()>;

    explicit QcRuleTableView(QWidget* parent = nullptr);

    // 注入规则集来源并重建表格。provider 的生命周期由页面保证。
    void SetRulesProvider(RulesProvider provider);

    // 按 provider 的当前内容重建表格（恢复默认规则后由页面调用）
    void Reload();

signals:
    // 用户改动已写入规则集（一次点击一条）。页面据此决定是否重算报告。
    void RulesChanged();
    // 用户点了「恢复默认规则」：页面负责替换规则集并再次 Reload()
    void ResetRequested();

private:
    void SetupUi();
    void OnItemChanged(QTableWidgetItem* item);

    RulesProvider provider_;
    QTableWidget* table_ = nullptr;
    // 重建表格时屏蔽 itemChanged，避免把"填表"当成"用户改规则"
    bool updating_ = false;
};

} // namespace ui
} // namespace videoeye
