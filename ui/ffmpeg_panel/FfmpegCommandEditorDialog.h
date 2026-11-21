#pragma once

// 「展开编辑」对话框：长命令放进多行编辑器里看，带最基本的着色与结构提示。
//
// 为什么单独开一个对话框而不是把命令框换成多行: 单行 QLineEdit 撑着回车运行、
// ↑↓ 翻历史、光标插入片段一整套行为，换掉它等于把这些全部重写一遍。对话框的
// 定位是"读改"，接受之后仍然交回单行框，执行路径一个字都不用改。
//
// 契约（由 core/ffmpeg/FfmpegCommandText 保证）:
//   Command() == FlattenCommandLines(编辑器内容)，即拿进来的命令进去再出来不变
//   （空白规范化到单个空格）。换行只是排版，ffmpeg 命令行没有行继续符。

#include <QDialog>
#include <QString>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace videoeye {
namespace ui {

class FfmpegCommandEditorDialog : public QDialog {
    Q_OBJECT

public:
    explicit FfmpegCommandEditorDialog(const QString& command, QWidget* parent = nullptr);

    /// 压成单行后的命令（给回单行输入框 / 直接执行）
    QString Command() const;
    /// 用户是按「运行」(Ctrl+Enter) 关的窗，而不是「确定」
    bool RunRequested() const {
        return run_requested_;
    }

private:
    void SetupUi(const QString& command);
    /// 结构提示：解析是否通过、输入输出各几个
    void RefreshStatus();
    void OnAcceptAndRun();

    QPlainTextEdit* editor_ = nullptr;
    QLabel* status_label_ = nullptr;
    QPushButton* run_button_ = nullptr;
    bool run_requested_ = false;
};

} // namespace ui
} // namespace videoeye
