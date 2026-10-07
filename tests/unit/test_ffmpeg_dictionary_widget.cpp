// FfmpegDictionaryWidget（FFmpeg 命令工作台「指令字典」区）的接缝测试。
//
// 为什么值得单独测：它是自成一体的浏览控件（搜 -> 列 -> 看详情 -> 插入），从 FfmpegPanel
// 抽出。它把「搜索 / 分类」这两种过滤、以及「滤镜要自动补 -vf / -af」的插入规则都压在
// 组件内部 —— 规则写错不会崩，只会把 filter 当选项插进命令、或搜出空列表还留着旧详情，
// 用户要对着 ffmpeg 报错才看得出来。
//
// 具体盯五件事：
//   1. 构造：分类下拉 = "全部分类" + 全部类别；词条列表 = 全部词条；默认选中第一行且
//      插入按钮可用。
//   2. 搜索过滤：列表条数与 FfmpegCommandCatalog::Search 完全一致（组件不自己做过滤）。
//   3. 分类过滤：切分类后列表条数与按该分类的 Search 一致，且确实少于全量。
//   4. 插入语义：滤镜补 -vf/-af 前缀且末尾带空格；普通选项按 insert_text 原样发。
//   5. 无匹配：列表清空、插入按钮禁用、详情提示"没有匹配的词条"，点插入不发信号。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>

#include <vector>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "ui/ffmpeg_panel/FfmpegDictionaryWidget.h"

using videoeye::ffmpeg::FfmpegCatalogEntry;
using videoeye::ffmpeg::FfmpegCommandCatalog;
using videoeye::ffmpeg::FfmpegEntryKind;
using videoeye::ui::FfmpegDictionaryWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_ffmpeg_dictionary_widget";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QLineEdit* SearchEdit(FfmpegDictionaryWidget& widget) {
    return widget.findChild<QLineEdit*>();
}

QListWidget* DictList(FfmpegDictionaryWidget& widget) {
    return widget.findChild<QListWidget*>();
}

// 组件里只有一个可点击按钮（「插入到命令」）。
QPushButton* InsertButton(FfmpegDictionaryWidget& widget) {
    return widget.findChild<QPushButton*>();
}

// 列表里第一个以给定前缀开头的行号（找不到返回 -1）。
int RowWithPrefix(QListWidget* list, const QString& prefix) {
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->text().startsWith(prefix)) {
            return row;
        }
    }
    return -1;
}

const FfmpegCatalogEntry* FirstEntryOfKind(FfmpegEntryKind kind) {
    for (const auto& entry : FfmpegCommandCatalog::Entries()) {
        if (entry.kind == kind) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 构造：分类下拉 + 全部词条 + 默认选中可用
// ---------------------------------------------------------------------------

TEST(FfmpegDictionaryWidgetTests, ConstructorPopulatesCategoriesAndFullEntryList) {
    EnsureApp();

    FfmpegDictionaryWidget widget;
    QComboBox* combo = widget.findChild<QComboBox*>();
    QListWidget* list = DictList(widget);
    QPushButton* insert = InsertButton(widget);
    QTextBrowser* detail = widget.findChild<QTextBrowser*>();
    ASSERT_TRUE(combo != nullptr);
    ASSERT_TRUE(list != nullptr);
    ASSERT_TRUE(insert != nullptr);
    ASSERT_TRUE(detail != nullptr);

    ASSERT_EQ(1 + FfmpegCommandCatalog::CategoryNames().size(), combo->count());
    EXPECT_EQ(QStringLiteral("全部分类"), combo->itemText(0));

    EXPECT_EQ(static_cast<int>(FfmpegCommandCatalog::Entries().size()), list->count());
    EXPECT_EQ(0, list->currentRow());       // 默认选中第一条 -> 详情可见、可插入
    EXPECT_TRUE(insert->isEnabled());
    EXPECT_FALSE(detail->toPlainText().isEmpty());
}

// ---------------------------------------------------------------------------
// 2. 搜索过滤：列表条数与 Search 完全一致
// ---------------------------------------------------------------------------

TEST(FfmpegDictionaryWidgetTests, SearchTextFiltersListToCatalogHits) {
    EnsureApp();

    FfmpegDictionaryWidget widget;
    QLineEdit* edit = SearchEdit(widget);
    QListWidget* list = DictList(widget);
    ASSERT_TRUE(edit != nullptr);
    ASSERT_TRUE(list != nullptr);

    edit->setText(QStringLiteral("crf"));
    EXPECT_EQ(static_cast<int>(FfmpegCommandCatalog::Search(QStringLiteral("crf")).size()),
              list->count());
    EXPECT_GT(list->count(), 0);

    edit->setText(QStringLiteral("scale"));
    EXPECT_EQ(static_cast<int>(FfmpegCommandCatalog::Search(QStringLiteral("scale")).size()),
              list->count());
    EXPECT_GT(list->count(), 0);
}

// ---------------------------------------------------------------------------
// 3. 分类过滤：切分类后条数与该分类的 Search 一致，且少于全量
// ---------------------------------------------------------------------------

TEST(FfmpegDictionaryWidgetTests, CategorySelectionRestrictsList) {
    EnsureApp();

    FfmpegDictionaryWidget widget;
    QComboBox* combo = widget.findChild<QComboBox*>();
    QListWidget* list = DictList(widget);
    ASSERT_TRUE(combo != nullptr);
    ASSERT_TRUE(list != nullptr);

    const int last = combo->count() - 1;
    ASSERT_GT(last, 0);
    combo->setCurrentIndex(last);

    const QString category = combo->itemText(last);
    const int expected =
        static_cast<int>(FfmpegCommandCatalog::Search(QString(), category).size());
    EXPECT_GT(expected, 0);
    EXPECT_EQ(expected, list->count());
    EXPECT_LT(list->count(), static_cast<int>(FfmpegCommandCatalog::Entries().size()));
}

// ---------------------------------------------------------------------------
// 4a. 插入滤镜：自动补 -vf / -af，且末尾带空格
// ---------------------------------------------------------------------------

TEST(FfmpegDictionaryWidgetTests, InsertFilterEntryPrependsVfOrAfPrefix) {
    EnsureApp();

    const FfmpegCatalogEntry* filter = FirstEntryOfKind(FfmpegEntryKind::Filter);
    ASSERT_TRUE(filter != nullptr);

    FfmpegDictionaryWidget widget;
    QLineEdit* edit = SearchEdit(widget);
    QListWidget* list = DictList(widget);
    QPushButton* insert = InsertButton(widget);
    ASSERT_TRUE(edit != nullptr);
    ASSERT_TRUE(list != nullptr);
    ASSERT_TRUE(insert != nullptr);

    edit->setText(filter->name);
    const int row = RowWithPrefix(list, filter->name + QStringLiteral("（滤镜）"));
    ASSERT_GE(row, 0);
    list->setCurrentRow(row);

    std::vector<QString> snippets;
    QObject::connect(&widget, &FfmpegDictionaryWidget::InsertRequested,
                     [&snippets](const QString& snippet) { snippets.push_back(snippet); });
    insert->click();
    ASSERT_EQ(1u, snippets.size());

    const QString snippet = snippets[0];
    EXPECT_TRUE(snippet.startsWith(QStringLiteral("-vf ")) ||
                snippet.startsWith(QStringLiteral("-af ")));
    EXPECT_TRUE(snippet.contains(filter->name));
    EXPECT_TRUE(snippet.endsWith(QLatin1Char(' ')));
}

// ---------------------------------------------------------------------------
// 4b. 插入普通选项：不加滤镜前缀，按 insert_text 原样发
// ---------------------------------------------------------------------------

TEST(FfmpegDictionaryWidgetTests, InsertOptionEntryEmitsRawSnippet) {
    EnsureApp();

    const FfmpegCatalogEntry* option = FirstEntryOfKind(FfmpegEntryKind::Option);
    ASSERT_TRUE(option != nullptr);

    FfmpegDictionaryWidget widget;
    QLineEdit* edit = SearchEdit(widget);
    QListWidget* list = DictList(widget);
    QPushButton* insert = InsertButton(widget);
    ASSERT_TRUE(edit != nullptr);
    ASSERT_TRUE(list != nullptr);
    ASSERT_TRUE(insert != nullptr);

    edit->setText(option->name);
    const int row = RowWithPrefix(list, option->name);
    ASSERT_GE(row, 0);
    list->setCurrentRow(row);

    std::vector<QString> snippets;
    QObject::connect(&widget, &FfmpegDictionaryWidget::InsertRequested,
                     [&snippets](const QString& snippet) { snippets.push_back(snippet); });
    insert->click();
    ASSERT_EQ(1u, snippets.size());

    const QString snippet = snippets[0];
    EXPECT_TRUE(snippet.startsWith(option->name));
    EXPECT_FALSE(snippet.startsWith(QStringLiteral("-vf ")));
    EXPECT_FALSE(snippet.startsWith(QStringLiteral("-af ")));
    EXPECT_TRUE(snippet.endsWith(QLatin1Char(' ')));
}

// ---------------------------------------------------------------------------
// 5. 无匹配：清空列表 + 禁用插入 + 详情提示 + 点插入不发信号
// ---------------------------------------------------------------------------

TEST(FfmpegDictionaryWidgetTests, NoMatchDisablesInsertAndShowsEmptyDetail) {
    EnsureApp();

    FfmpegDictionaryWidget widget;
    QLineEdit* edit = SearchEdit(widget);
    QListWidget* list = DictList(widget);
    QPushButton* insert = InsertButton(widget);
    QTextBrowser* detail = widget.findChild<QTextBrowser*>();
    ASSERT_TRUE(edit != nullptr);
    ASSERT_TRUE(list != nullptr);
    ASSERT_TRUE(insert != nullptr);
    ASSERT_TRUE(detail != nullptr);

    edit->setText(QStringLiteral("zzz_no_such_entry_zzz"));
    EXPECT_EQ(0, list->count());
    EXPECT_FALSE(insert->isEnabled());
    EXPECT_TRUE(detail->toPlainText().contains(QStringLiteral("没有匹配的词条")));

    std::vector<QString> snippets;
    QObject::connect(&widget, &FfmpegDictionaryWidget::InsertRequested,
                     [&snippets](const QString& snippet) { snippets.push_back(snippet); });
    insert->click();
    EXPECT_TRUE(snippets.empty());  // 越界行号不能让插入路径发信号
}