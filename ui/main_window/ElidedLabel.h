#pragma once

// 单行省略标签：文本按控件实际宽度做省略，而不是按文本自然宽度撑开布局。
//
// 为什么需要它：应用栏里的媒体路径是任意长度的绝对路径。QLabel 在非自动换行时
// 的 minimumSizeHint 就是整段文本的宽度，而布局会把 minimumSizeHint 当硬下限 ——
// 于是"打开一个深层目录里的文件"就能把应用栏撑宽，右侧的「打开文件 / 打开 URL」
// 被挤出屏幕 —— 小屏、高缩放（逻辑宽度变小）、或用户把侧栏拉宽时都会撞上。
//
// 做法：控件本身报一个很小的宽度下限（可被压缩），拿到真实宽度后再用
// QFontMetrics::elidedText 把完整文本省略成"首…尾"。完整文本始终留在 tooltip 里。
// 不复写 paintEvent —— 交给 QLabel 自己画，主题 QSS 的 color 才会照常生效。

#include <QLabel>
#include <QString>

class QResizeEvent;

namespace videoeye {
namespace ui {

class ElidedLabel : public QLabel {
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget* parent = nullptr);

    // 设置完整文本（tooltip 同步为同一份完整文本）
    void SetFullText(const QString& text);
    // 完整文本（不是屏幕上那份省略后的）
    const QString& FullText() const {
        return full_text_;
    }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void UpdateElidedText();

    QString full_text_;
};

} // namespace ui
} // namespace videoeye
