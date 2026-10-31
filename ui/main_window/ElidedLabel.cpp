#include "ui/main_window/ElidedLabel.h"

#include <QFontMetrics>
#include <QResizeEvent>
#include <QSizePolicy>

namespace videoeye {
namespace ui {

namespace {
// 布局里给它留的"理想宽度"。这只是建议值：控件可以被压到 minimumSizeHint 那么窄，
// 再窄就只剩省略号 —— 那也比把右侧按钮挤出去强。
constexpr int kPreferredWidth = 220;
constexpr int kMinimumWidth = 80;
} // namespace

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {
    // Ignored：横向 sizeHint 不参与布局的"想要多大"，只按 stretch 分剩下的空间。
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextFormat(Qt::PlainText);
}

void ElidedLabel::SetFullText(const QString& text) {
    full_text_ = text;
    setToolTip(text);
    UpdateElidedText();
}

QSize ElidedLabel::sizeHint() const {
    return QSize(kPreferredWidth, QLabel::sizeHint().height());
}

QSize ElidedLabel::minimumSizeHint() const {
    // 下限必须很小：这一条就是"别让长路径撑宽整条应用栏"的机关。
    return QSize(kMinimumWidth, QLabel::minimumSizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    UpdateElidedText();
}

void ElidedLabel::UpdateElidedText() {
    if (full_text_.isEmpty()) {
        QLabel::setText(QString());
        return;
    }
    const QFontMetrics metrics(font());
    const int available = width() > 0 ? width() : kPreferredWidth;
    // ElideMiddle：路径的首尾（盘符 / 文件名）都留着，中间那段目录才是最不重要的
    QLabel::setText(metrics.elidedText(full_text_, Qt::ElideMiddle, available));
}

} // namespace ui
} // namespace videoeye
