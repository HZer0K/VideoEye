// 主题渲染探针（开发期工具，不进仓库构建）：
// 应用 AppTheme 深色主题后构造一批代表性控件，offscreen grab() 成 PNG，
// 用来肉眼验证表头字号/对比度、复选框勾选符号、键盘焦点轮廓等改动。
// 标签一律用 ASCII —— offscreen 平台没有中文字体回退，中文会渲染成方块。
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/theme/AppTheme.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    videoeye::ui::theme::applyDarkTheme();

    QWidget root;
    QVBoxLayout* rl = new QVBoxLayout(&root);

    // 表头 + 表格
    QTableWidget* table = new QTableWidget(3, 3, &root);
    table->setHorizontalHeaderLabels({"File", "Status", "Score"});
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            table->setItem(r, c, new QTableWidgetItem(QString("cell %1-%2").arg(r).arg(c)));
    rl->addWidget(table);

    // 复选框（未选/选中/禁用选中）
    QHBoxLayout* cb_row = new QHBoxLayout();
    QCheckBox* cb_off = new QCheckBox("unchecked", &root);
    QCheckBox* cb_on = new QCheckBox("checked", &root);
    cb_on->setChecked(true);
    QCheckBox* cb_dis = new QCheckBox("checked+disabled", &root);
    cb_dis->setChecked(true);
    cb_dis->setEnabled(false);
    cb_row->addWidget(cb_off);
    cb_row->addWidget(cb_on);
    cb_row->addWidget(cb_dis);
    cb_row->addStretch();
    rl->addLayout(cb_row);

    // 输入类控件（测焦点轮廓）
    QHBoxLayout* edit_row = new QHBoxLayout();
    QPushButton* btn = new QPushButton("Button", &root);
    QComboBox* combo = new QComboBox(&root);
    combo->addItems({"option1", "option2"});
    QLineEdit* edit = new QLineEdit(&root);
    edit->setText("lineedit");
    QSlider* slider = new QSlider(Qt::Horizontal, &root);
    slider->setValue(40);
    edit_row->addWidget(btn);
    edit_row->addWidget(combo);
    edit_row->addWidget(edit);
    edit_row->addWidget(slider);
    edit_row->addStretch();
    rl->addLayout(edit_row);

    // 分组框（标题说明文字）
    QGroupBox* gb = new QGroupBox("GroupBox title (hint text)", &root);
    QHBoxLayout* gbl = new QHBoxLayout(gb);
    gbl->addWidget(new QLabel("label inside group"));
    rl->addWidget(gb);

    // 树（含表头）
    QTreeWidget* tree = new QTreeWidget(&root);
    tree->setHeaderLabels({"Tree header 1", "Tree header 2"});
    QTreeWidgetItem* item = new QTreeWidgetItem({"entry", "value"});
    tree->addTopLevelItem(item);
    rl->addWidget(tree);

    root.resize(760, 560);
    root.show();
    QApplication::setActiveWindow(&root);

    QTimer::singleShot(30, [&]() {
        root.grab().save("probe_all.png");
        // 焦点态：按钮拿焦点后抓自己（验证键盘焦点轮廓是否可见）
        btn->setFocus();
        QApplication::processEvents();
        btn->grab().save("probe_button_focus.png");
        // 复选框放大抓取（验证勾选符号）
        cb_on->setFixedSize(160, 40);
        cb_on->grab().save("probe_checkbox_checked.png");
        cb_off->setFixedSize(160, 40);
        cb_off->grab().save("probe_checkbox_off.png");
        cb_dis->setFixedSize(200, 40);
        cb_dis->grab().save("probe_checkbox_disabled.png");
        // 表头特写
        table->horizontalHeader()->setFixedHeight(28);
        table->grab().save("probe_table.png");
        // 输入框焦点
        edit->setFocus();
        QApplication::processEvents();
        edit->grab().save("probe_lineedit_focus.png");
        combo->grab().save("probe_combobox.png");
        app.quit();
    });
    return app.exec();
}
