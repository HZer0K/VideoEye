#pragma once

// 命令输入框的"边打边提示"：光标停在哪，就在输入框下面解释那个词，
// 并按前缀给出下一个词的可选补全。
//
// 为什么不直接用 QLineEdit::setCompleter:
//   QCompleter 自己替换文本时按 Qt 的切词规则（空白分隔）算"当前词"，而本项目的命令
//   规则是**成对引号里的空白不切**（`-i "my file.mp4"` 是一个词）。走 setCompleter 的话，
//   光标在引号里时补全会把后半截路径当成另一个词替换掉。这里只借 QCompleter 的 popup
//   定位与鼠标点击，文本替换一律走 core 的 ApplyCompletion。
//
// 分工:
//   * 位置判断 / 候选 / 文案 全在 core/ffmpeg/FfmpegCommandInputHint（无 Qt Widget 依赖，可单测）；
//   * 本类只做排版、popup 与键盘（↑↓ 选择、Tab/Enter 接受、Esc 关闭）。

#include <QObject>
#include <QString>

#include "core/ffmpeg/FfmpegCommandInputHint.h"

class QAbstractItemView;
class QCompleter;
class QLabel;
class QLineEdit;
class QStandardItemModel;
class QWidget;

namespace videoeye {
namespace ui {

class FfmpegCommandInputAssist : public QObject {
    Q_OBJECT

public:
    explicit FfmpegCommandInputAssist(QLineEdit* edit, QWidget* parent = nullptr);
    ~FfmpegCommandInputAssist() override;

    /// 提示行（页面把它放进布局，紧贴命令输入框下方）。
    /// 返回具体类型而不是 QWidget*：这里只有前向声明，转基类会要求完整定义。
    QLabel* HintWidget() const {
        return hint_label_;
    }

    /// 候选下拉是否开着。页面据此让出 ↑/↓ —— 否则会一边选候选一边翻历史。
    /// 判据是组件自己记的状态，不是窗口系统的 isVisible()：后者在 show() 之后有一拍
    /// 延迟（真实平台）或干脆不置位（offscreen），拿它当判据会让 Tab/回车时灵时不灵。
    bool PopupVisible() const;

    /// 重算提示与候选（文本或光标变了都要重算）
    void Refresh();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void SetupUi();
    void UpdatePopup(bool force);
    void MoveSelection(int delta);
    void AcceptCandidate(int row);
    void HidePopup();
    QString HintHtml() const;

    QLineEdit* edit_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QCompleter* completer_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    /// 候选列表（QCompleter 自己的 popup）。焦点会落在它上面，所以要单独装过滤器。
    QAbstractItemView* popup_ = nullptr;
    ffmpeg::FfmpegInputHint hint_;
    /// 候选下拉现在是不是开着（自己的状态，不看窗口系统）
    bool popup_shown_ = false;
    /// 接受候选时 setText 会触发 textChanged；那一趟不该再弹一次（会立刻又冒出来）
    bool suppress_refresh_ = false;
};

} // namespace ui
} // namespace videoeye
