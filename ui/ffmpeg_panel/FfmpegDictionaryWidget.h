#pragma once

// 「FFmpeg 命令工作台」页右侧的「指令字典」区：搜索框 + 分类下拉 + 词条列表 +
// 详情 + 「插入到命令」。
//
// 从 FfmpegPanel 抽出的独立组件 —— 原 FfmpegPanel 把程序路径 / 命令编辑 / 输出 /
// 字典四块全塞在一个类里，膨胀到 1000+ 行。字典是自成一体的浏览控件（搜 → 列 →
// 看详情 → 插入），除了最后一步要写页面的命令输入框，其余完全不碰页面状态。
//
// 对外接缝：
//   - 插入出去：InsertRequested(snippet) —— 目标输入框（命令编辑框）属于页面，
//     组件不持有指针，只把拼好的片段（滤镜已自动补 -vf/-af）发出去，由页面插入。
//     刻意不用"直接写 command_edit_"，否则组件就被绑死在页面的布局上。
//   - 数据进来：无 —— 词条来自 FfmpegCommandCatalog（静态表），组件自己搜。
//
// ffmpeg 不可用不影响本区：搜索与解释不依赖可执行程序，这也是它适合先拆出来的原因。

#include <QWidget>

#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>

#include <vector>

#include "core/ffmpeg/FfmpegCommandCatalog.h"

namespace videoeye {
namespace ui {

class FfmpegDictionaryWidget : public QWidget {
    Q_OBJECT

public:
    explicit FfmpegDictionaryWidget(QWidget* parent = nullptr);

signals:
    // 用户点了「插入到命令」：snippet 已按词条类型拼好（滤镜自带 -vf / -af 前缀），
    // 末尾带一个空格，由页面插到命令输入框末尾。
    void InsertRequested(const QString& snippet);

private slots:
    void OnSearchTextChanged();
    void OnCategoryChanged();
    void OnDictionaryCurrentChanged();
    void OnInsertEntry();

private:
    void SetupUi();
    void RefreshDictionary();
    void ShowEntryDetail(const ffmpeg::FfmpegCatalogEntry* entry);

    QLineEdit* search_edit_ = nullptr;
    QComboBox* category_combo_ = nullptr;
    QListWidget* dict_list_ = nullptr;
    QPushButton* dict_insert_button_ = nullptr;
    QTextBrowser* dict_detail_ = nullptr;

    // 当前列表里每个行对应的词条（与 dict_list_ 行序一一对应）
    std::vector<const ffmpeg::FfmpegCatalogEntry*> dict_results_;
};

}  // namespace ui
}  // namespace videoeye
