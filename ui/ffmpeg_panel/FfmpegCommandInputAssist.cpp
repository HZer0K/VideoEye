#include "ui/ffmpeg_panel/FfmpegCommandInputAssist.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScreen>
#include <QStandardItemModel>
#include <QStyledItemDelegate>

#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {
namespace {

/// 提示行是**固定两行**的，长文案必须截断，否则 QLabel 一换行就把命令区顶高。
/// 详情本来就该看右侧字典 / 解释页签，这里只要"一眼知道在说什么"。
QString Shorten(const QString& text, int max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.left(max_chars) + QStringLiteral("…");
}

/// 候选行是**两行**：上面是能写进命令的片段，下面是它干什么。
/// 单行的话 `-crf` 与 `-crf ` 之类看不出区别，而这一栏的价值就在说明。
class CandidateDelegate : public QStyledItemDelegate {
public:
    explicit CandidateDelegate(QObject* parent) : QStyledItemDelegate(parent) {
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(320, 40);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        const QRect rect = option.rect;
        const bool selected = (option.state & QStyle::State_Selected) != 0;

        painter->fillRect(
            rect, QColor(selected ? QLatin1String(theme::color::kBgSurface) : QLatin1String(theme::color::kBgCard)));
        if (selected) {
            // 左侧一条强调色竖条：深色底上光靠背景色差分不清选没选中
            painter->fillRect(QRect(rect.left(), rect.top(), 2, rect.height()),
                              QColor(QLatin1String(theme::color::kAccent)));
        }

        const QRect main_rect(rect.left() + 10, rect.top() + 2, rect.width() - 20, 20);
        painter->setFont(theme::font::monoFont(9, true));
        painter->setPen(QColor(QLatin1String(selected ? theme::color::kTextPrimary : theme::color::kAccent)));
        painter->drawText(main_rect, Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());

        const QString note = index.data(Qt::UserRole + 1).toString();
        if (!note.isEmpty()) {
            const QRect note_rect(rect.left() + 10, rect.top() + 21, rect.width() - 20, 16);
            painter->setFont(theme::font::uiFont(8));
            painter->setPen(QColor(QLatin1String(theme::color::kTextSecondary)));
            const QString elided = painter->fontMetrics().elidedText(note, Qt::ElideRight, note_rect.width());
            painter->drawText(note_rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
        }
        painter->restore();
    }
};

} // namespace

FfmpegCommandInputAssist::FfmpegCommandInputAssist(QLineEdit* edit, QWidget* parent) : QObject(parent), edit_(edit) {
    SetupUi();
    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::textChanged, this, &FfmpegCommandInputAssist::Refresh);
    connect(edit_, &QLineEdit::cursorPositionChanged, this, &FfmpegCommandInputAssist::Refresh);
    Refresh();
}

FfmpegCommandInputAssist::~FfmpegCommandInputAssist() = default;

void FfmpegCommandInputAssist::SetupUi() {
    hint_label_ = new QLabel(edit_->parentWidget());
    hint_label_->setObjectName(QStringLiteral("CommandHintLabel"));
    hint_label_->setWordWrap(true);
    hint_label_->setTextFormat(Qt::RichText);
    hint_label_->setTextInteractionFlags(Qt::TextSelectableByMouse); // 说明文字可以选中复制
    // 至少两行的高度：文案在 HintHtml 里已按字符数截断，通常正好两行；
    // 窗口很窄时允许长到三行 —— 宁可命令区略微变高，也不能把"下一步"裁掉。
    hint_label_->setMinimumHeight(36);
    hint_label_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::color::kTextSecondary)));

    model_ = new QStandardItemModel(this);
    completer_ = new QCompleter(this);
    completer_->setModel(model_);
    // 只借 popup 的定位与鼠标点击，**不**调 setCompleter：那样 QLineEdit 会按 Qt 的
    // 空白切词去替换文本，引号里的路径会被切坏。替换一律走 ApplyCompletion。
    completer_->setWidget(edit_);
    completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer_->setMaxVisibleItems(8);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);

    QAbstractItemView* popup = completer_->popup();
    popup->setItemDelegate(new CandidateDelegate(popup));
    // ⚠️ 这里**不能**给 popup 设焦点策略。QCompleter 把输入框设成了 popup 的焦点代理
    // （focusProxy），而 QWidget::setFocusPolicy 是沿焦点代理往下写的 —— 给 popup 设
    // Qt::NoFocus 实际会落到 QLineEdit 自己身上，输入框从此点不进去、一个字也打不上。
    // 想「焦点留在输入框」什么都不用做：QCompleter 的代理机制本来就负责把按键转回输入框。
    popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 候选开着时按键落在列表窗口上，只给输入框装过滤器收不到 Tab/回车。
    popup_ = popup;
    popup_->installEventFilter(this);
    popup->setStyleSheet(QStringLiteral("QListView { background: %1; border: 1px solid %2; outline: none; }"
                                        "QListView::item { border: none; }")
                             .arg(QLatin1String(theme::color::kBgCard), QLatin1String(theme::color::kBorder)));

    connect(completer_, QOverload<const QModelIndex&>::of(&QCompleter::activated), this,
            [this](const QModelIndex& index) { AcceptCandidate(index.row()); });
}

bool FfmpegCommandInputAssist::PopupVisible() const {
    return popup_shown_;
}

void FfmpegCommandInputAssist::Refresh() {
    if (suppress_refresh_ || edit_ == nullptr) {
        return;
    }
    hint_ = ffmpeg::BuildInputHint(edit_->text(), edit_->cursorPosition());
    if (hint_label_ != nullptr) {
        hint_label_->setText(HintHtml());
    }
    UpdatePopup(false);
}

void FfmpegCommandInputAssist::UpdatePopup(bool force) {
    if (model_ == nullptr || completer_ == nullptr) {
        return;
    }
    model_->clear();
    if (hint_.candidates.empty()) {
        HidePopup();
        return;
    }
    // 自动弹的条件是"已经在打一个词"。光标停在空白处（准备起新词）时不主动弹 ——
    // 每敲一个空格就冒一个列表会挡住命令本身；想看候选按 Tab。
    if (!force && hint_.prefix.isEmpty()) {
        HidePopup();
        return;
    }

    for (const ffmpeg::FfmpegInputCandidate& candidate : hint_.candidates) {
        auto* item = new QStandardItem(candidate.label);
        item->setData(candidate.note, Qt::UserRole + 1);
        item->setEditable(false);
        model_->appendRow(item);
    }

    // 候选已经按项目规则（引号内的空白不切）过滤好了，别再让 QCompleter 按它自己的
    // 规则筛一遍 —— 那一步在引号路径上会筛出空列表。
    completer_->setCompletionPrefix(QString());
    completer_->complete();
    QAbstractItemView* popup = completer_->popup();
    popup->setCurrentIndex(model_->index(0, 0));
    const int width = qMax(360, edit_->width());
    popup->setMinimumWidth(width);
    popup->resize(width, popup->height());

    // 候选默认贴在输入框正下方，正好盖住提示行 —— 而"这一步该写什么"恰恰是打字时最该看的。
    // 挪到提示行下面；下面放不下就不动，留给 QCompleter 自己挑方向（它会往上翻）。
    if (hint_label_ != nullptr && hint_label_->isVisible()) {
        const QPoint below = hint_label_->mapToGlobal(QPoint(0, hint_label_->height() + 2));
        const QScreen* screen = edit_->screen();
        const QRect available = screen != nullptr ? screen->availableGeometry() : QRect();
        const bool fits = available.isEmpty() || below.y() + popup->height() <= available.bottom();
        if (fits) {
            popup->move(below);
        }
    }
    popup_shown_ = true;
}

void FfmpegCommandInputAssist::MoveSelection(int delta) {
    if (!PopupVisible() || model_ == nullptr) {
        return;
    }
    const int count = model_->rowCount();
    if (count == 0) {
        return;
    }
    const int row = completer_->popup()->currentIndex().row();
    int next = 0;
    if (row < 0) {
        next = delta > 0 ? 0 : count - 1;
    } else {
        next = (row + delta + count) % count;
    }
    completer_->popup()->setCurrentIndex(model_->index(next, 0));
}

void FfmpegCommandInputAssist::AcceptCandidate(int row) {
    if (row < 0) {
        row = 0;
    }
    if (row >= static_cast<int>(hint_.candidates.size())) {
        return;
    }
    const ffmpeg::FfmpegInputCandidate& chosen = hint_.candidates.at(static_cast<size_t>(row));
    // 值候选接在选项后面成一个新词（`-c` → `-c copy`）；选项/路径候选则原地替换半截词。
    const int begin = chosen.starts_new_word ? hint_.word_end : hint_.word_begin;
    const int end = hint_.word_end;
    const ffmpeg::FfmpegCompletionEdit applied = ffmpeg::ApplyCompletion(edit_->text(), begin, end, chosen.text);

    suppress_refresh_ = true;
    edit_->setText(applied.text);
    edit_->setCursorPosition(applied.cursor);
    suppress_refresh_ = false;

    HidePopup();
    Refresh(); // setText 那一趟被 suppress 挡住了，这里补一次
}

void FfmpegCommandInputAssist::HidePopup() {
    popup_shown_ = false;
    if (completer_ != nullptr && completer_->popup() != nullptr) {
        completer_->popup()->hide();
    }
}

QString FfmpegCommandInputAssist::HintHtml() const {
    const QString secondary = QLatin1String(theme::color::kTextSecondary);
    const QString accent = QLatin1String(theme::color::kAccent);
    const QString warning = QLatin1String(theme::color::kWarning);

    if (edit_->text().trimmed().isEmpty()) {
        return QStringLiteral("<span style='color:%1'>在这里写命令。每输入一个参数，这行会说明它在干什么，"
                              "并提示下一步该写什么；按 Tab 可以列出可接的选项。</span>")
            .arg(secondary);
    }

    QStringList lines;
    if (!hint_.problem.isEmpty()) {
        lines << QStringLiteral("<span style='color:%1'>%2</span>").arg(warning, hint_.problem.toHtmlEscaped());
    }

    if (hint_.has_subject) {
        QString head = QStringLiteral("<b style='color:%1'>%2</b>").arg(accent, hint_.subject_title.toHtmlEscaped());
        if (!hint_.subject.isEmpty()) {
            head += QStringLiteral(" <code>%1</code>").arg(Shorten(hint_.subject, 24).toHtmlEscaped());
        }
        if (hint_.subject_is_prefix) {
            head += QStringLiteral(" <span style='color:%1'>（按前缀猜的，可能还没打完）</span>").arg(warning);
        }
        if (!hint_.subject_summary.isEmpty()) {
            // 有 problem 时那条已经占掉一行，摘要要让位（高度固定只有两行）
            head += QStringLiteral(" —— %1").arg(
                Shorten(hint_.subject_summary, hint_.problem.isEmpty() ? 40 : 24).toHtmlEscaped());
        }
        lines << head;
    }

    // 引号没闭合这类问题优先占位：这时"下一步写什么"多半也是错的，先别挤进来。
    if (hint_.problem.isEmpty() && !hint_.next_label.isEmpty()) {
        QString next = QStringLiteral("<span style='color:%1'>下一步：%2</span>")
                           .arg(secondary, Shorten(hint_.next_label, 52).toHtmlEscaped());
        if (!hint_.next_examples.isEmpty()) {
            QStringList pieces;
            for (int i = 0; i < hint_.next_examples.size() && i < 2; ++i) {
                pieces << QStringLiteral("<code>%1</code>").arg(hint_.next_examples.at(i).toHtmlEscaped());
            }
            next +=
                QStringLiteral(" <span style='color:%1'>可接：%2</span>").arg(secondary, pieces.join(QLatin1Char(' ')));
        }
        lines << next;
    }

    return lines.join(QStringLiteral("<br>"));
}

bool FfmpegCommandInputAssist::eventFilter(QObject* watched, QEvent* event) {
    // 两个对象共用这套键位：输入框，以及候选列表。
    // 候选一开，键盘焦点其实落在列表那个 popup 窗口上 —— 只给输入框装过滤器时，
    // Tab 会被 QAbstractItemView 当成"换到下一项"吃掉，回车也被它吃掉，
    // 于是"补全"看起来完全没反应，焦点还莫名其妙跑到了按钮上。
    const bool from_popup = (popup_ != nullptr && watched == popup_);
    if ((watched != edit_ && !from_popup) || event->type() != QEvent::KeyPress) {
        return QObject::eventFilter(watched, event);
    }

    auto* key_event = static_cast<QKeyEvent*>(event);
    const bool popup_open = PopupVisible();
    const int current_row = (popup_ != nullptr) ? popup_->currentIndex().row() : -1;

    switch (key_event->key()) {
    case Qt::Key_Down:
    case Qt::Key_Up:
        // 列表自己上下移动就够了，这里不抢（抢了反而要重写滚动与边界）。
        if (!from_popup && popup_open) {
            MoveSelection(key_event->key() == Qt::Key_Down ? 1 : -1);
            return true; // 吃掉：否则页面那边会把它当"翻历史"
        }
        break;

    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        if (popup_open) {
            AcceptCandidate(current_row);
            return true;
        }
        // 光标在空白处时不自动弹，Tab 是用户主动要一次候选。
        // 判据用"有没有候选"而不是"弹窗是不是已经可见"：窗口映射有一拍延迟，
        // 判早了就会把 Tab 放行，结果是焦点跑掉、命令一个字也没补上。
        if (!hint_.candidates.empty()) {
            UpdatePopup(true);
            return true;
        }
        return false; // 真没候选才让 Tab 去切换焦点

    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (popup_open) {
            // 候选开着时回车是"接受补全"，不是"运行命令" —— 想跑命令时先按 Esc
            // 或让列表自己消失。反过来（回车跑掉一条半截命令）代价大得多。
            AcceptCandidate(current_row);
            return true;
        }
        break;

    case Qt::Key_Escape:
        if (popup_open) {
            HidePopup();
            edit_->setFocus(); // 从列表焦点交还输入框，否则下一个字不知道往哪走
            return true;
        }
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}

} // namespace ui
} // namespace videoeye
