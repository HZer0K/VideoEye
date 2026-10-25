#pragma once

// 「QC 模板」区：模板下拉框 + 说明 + 加载 / 另存为。
//
// 从 ReportingPanel 拆出的展示型组件。模板的加载（读 JSON、校验未知规则 id）与
// 另存为仍留在面板里 —— 那两步要弹文件对话框、要读写文件；这里只负责控件、
// 当前选项与三个请求信号。

#include <QString>
#include <QWidget>

#include <QComboBox>
#include <QLabel>

namespace videoeye {
namespace ui {

class QcProfileView : public QWidget {
    Q_OBJECT

public:
    explicit QcProfileView(QWidget* parent = nullptr);

    // 下拉框当前项存进 itemData 的模板 id（与 profile_ 对得上才算切换）
    QString CurrentProfileId() const;

    void SetDescription(const QString& text);
    // 扫描中只锁模板下拉框；「加载模板文件…」「另存为…」与原实现一样始终可用
    void SetBusy(bool busy);

signals:
    void ProfileChanged(const QString& profile_id);
    void LoadRequested();
    void SaveRequested();

private:
    void SetupUi();

    QComboBox* profile_combo_ = nullptr;
    QLabel* description_ = nullptr;
};

} // namespace ui
} // namespace videoeye
