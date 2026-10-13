// 批量表格排序/显示格式探针（开发期工具，offscreen 运行）。
//
// 验证三件纯 UI 交互测不到的事：
//   1. 数值列（DisplayRole 存 double/int）排序是否按数值而不是字典序
//      —— 否则评分 "10.0" 会排在 "9.5" 前面；
//   2. DoubleFormatDelegate 的显示格式（评分 1 位 / 耗时取整）；
//   3. setSortingEnabled(false) 期间插入行不重排，打开时才按当前排指示排一次；
//      排序后 item 的 UserRole 绑定跟着行走，隐藏行排序后仍隐藏。
//
// 输出一律走 stderr（qInfo 在本环境的输出会被吞，原因未深究，与产品代码无关）。

#include <QApplication>
#include <QHeaderView>
#include <QLocale>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVariant>

#include <cstdio>

namespace {

// 与 ReportingPanel.cpp 里同款同参的委托（复制一份，探针自证行为）
class DoubleFormatDelegate : public QStyledItemDelegate {
public:
    DoubleFormatDelegate(int decimals, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), decimals_(decimals) {}

    QString displayText(const QVariant& value, const QLocale& locale) const override {
        if (value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Float) {
            return QString::number(value.toDouble(), 'f', decimals_);
        }
        return QStyledItemDelegate::displayText(value, locale);
    }

private:
    int decimals_;
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QTableWidget table(0, 4);
    table.setHorizontalHeaderLabels({"file", "score", "elapsed_ms", "critical"});
    table.setSortingEnabled(false);  // 模拟扫描中
    table.setItemDelegateForColumn(1, new DoubleFormatDelegate(1, &table));
    table.setItemDelegateForColumn(2, new DoubleFormatDelegate(0, &table));

    struct Row {
        const char* file;
        double score;
        double elapsed;
        int critical;
    };
    // 刻意选会让字典序出错的值: 9.5 / 10.0 / 100.0 / 85.3
    const Row rows[] = {
        {"a.mp4", 9.5, 9.44, 0},
        {"b.mp4", 100.0, 1200.56, 3},
        {"c.mp4", 10.0, 99.99, 1},
        {"d.mp4", 85.3, 10000.0, 0},
        {"e.mp4", 85.25, 5.0, 2},
    };
    for (const Row& r : rows) {
        const int row = table.rowCount();
        table.insertRow(row);
        table.setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(r.file)));
        auto* score = new QTableWidgetItem;
        score->setData(Qt::DisplayRole, QVariant::fromValue(r.score));
        table.setItem(row, 1, score);
        auto* elapsed = new QTableWidgetItem;
        elapsed->setData(Qt::DisplayRole, QVariant::fromValue(r.elapsed));
        table.setItem(row, 2, elapsed);
        auto* critical = new QTableWidgetItem;
        critical->setData(Qt::DisplayRole, QVariant::fromValue(r.critical));
        table.setItem(row, 3, critical);
    }

    auto order_of = [](QTableWidget& t) {
        QStringList names;
        for (int i = 0; i < t.rowCount(); ++i) {
            if (t.isRowHidden(i)) names << "(" + t.item(i, 0)->text() + ")";
            else names << t.item(i, 0)->text();
        }
        return names.join(",");
    };

    fprintf(stderr, "[before-sort]        %s\n", qPrintable(order_of(table)));

    // ---- 1) 评分列降序（扫描结束 -> 打开排序） ----
    table.setSortingEnabled(true);
    fprintf(stderr, "[after-enable]       %s   (看 Qt 是否自动按第 0 列排一次)\n",
            qPrintable(order_of(table)));
    table.sortItems(1, Qt::DescendingOrder);
    fprintf(stderr, "[score-desc]         %s   (期望 b,d,e,c,a = 100/85.3/85.25/10/9.5)\n",
            qPrintable(order_of(table)));

    // ---- 2) 耗时列升序 ----
    table.sortItems(2, Qt::AscendingOrder);
    fprintf(stderr, "[elapsed-asc]        %s   (期望 e,a,c,b,d = 5/9.44/99.99/1200.56/10000)\n",
            qPrintable(order_of(table)));

    // ---- 3) 计数列降序（int 数值） ----
    table.sortItems(3, Qt::DescendingOrder);
    fprintf(stderr, "[critical-desc]      %s   (期望 b,e,c,a/d)\n", qPrintable(order_of(table)));

    // ---- 4) 委托显示格式 ----
    // itemDelegateForColumn 返回 QAbstractItemDelegate*，displayText 是
    // QStyledItemDelegate 的成员，这里 static_cast 下来调
    auto* score_delegate = static_cast<QStyledItemDelegate*>(table.itemDelegateForColumn(1));
    auto* elapsed_delegate = static_cast<QStyledItemDelegate*>(table.itemDelegateForColumn(2));
    fprintf(stderr, "[display] score 85.25 -> %s, 100 -> %s (期望 85.3 / 100.0)\n",
            qPrintable(score_delegate->displayText(QVariant(85.25), QLocale::c())),
            qPrintable(score_delegate->displayText(QVariant(100.0), QLocale::c())));
    fprintf(stderr, "[display] elapsed 1200.56 -> %s, 9.44 -> %s (期望 1201 / 9)\n",
            qPrintable(elapsed_delegate->displayText(QVariant(1200.56), QLocale::c())),
            qPrintable(elapsed_delegate->displayText(QVariant(9.44), QLocale::c())));

    // ---- 5) 排序后 item 的 UserRole 绑定是否跟着行走 ----
    for (int i = 0; i < table.rowCount(); ++i) {
        table.item(i, 0)->setData(Qt::UserRole, i * 10);
    }
    table.sortItems(1, Qt::AscendingOrder);
    QStringList bound;
    for (int i = 0; i < table.rowCount(); ++i) {
        bound << QString("%1:%2").arg(table.item(i, 0)->text()).arg(table.item(i, 0)->data(Qt::UserRole).toInt());
    }
    fprintf(stderr, "[userrole-after-sort] %s   (期望 a:30,c:20,e:10,d:40,b:0)\n",
            qPrintable(bound.join(",")));

    // ---- 6) 隐藏行在排序后保持隐藏且 UserRole 仍对 ----
    table.setRowHidden(0, true);
    table.sortItems(1, Qt::DescendingOrder);
    fprintf(stderr, "[hidden-after-sort] row0-hidden=%d order=%s   (期望 1 且首行 b 被括起来)\n",
            table.isRowHidden(0) ? 1 : 0, qPrintable(order_of(table)));

    return 0;
}
