#include "ui/ffmpeg_panel/FfmpegCommandEditorDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVBoxLayout>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegCommandText.h"
#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {
namespace {

QColor ColorOf(const char* value) {
    return QColor(QString::fromLatin1(value));
}

/// 命令行的轻量着色：只做"看得出层次"，不做语义校验（那是解释区的活）。
///
/// 着色规则是按**词**来的，不是按行正则：引号里的空格会被当成路径内部空格，
/// 用正则切词会把 `-i "my file.mp4"` 切成三段，然后给中间那段乱上色。
class CommandHighlighter : public QSyntaxHighlighter {
public:
    explicit CommandHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
        option_format_.setForeground(ColorOf(theme::color::kAccent));
        unknown_format_.setForeground(ColorOf(theme::color::kWarning));
        path_format_.setForeground(ColorOf(theme::color::kSuccess));
    }

protected:
    void highlightBlock(const QString& text) override {
        const std::vector<ffmpeg::FfmpegCommandWord> words = ffmpeg::SplitCommandWords(text);
        for (const ffmpeg::FfmpegCommandWord& word : words) {
            const QString piece = text.mid(word.begin, word.end - word.begin);
            const QTextCharFormat* format = FormatFor(piece);
            if (format != nullptr) {
                setFormat(word.begin, piece.size(), *format);
            }
        }
    }

private:
    const QTextCharFormat* FormatFor(const QString& piece) const {
        if (piece.startsWith(QLatin1Char('"')) || piece.startsWith(QLatin1Char('\''))) {
            return &path_format_; // 带引号 = 路径；含空格时不引号也写不成一个词
        }
        if (piece.contains(QStringLiteral("://")) || piece.startsWith(QStringLiteral("pipe:"))) {
            return &path_format_;
        }
        if (piece.size() > 1 && piece.startsWith(QLatin1Char('-'))) {
            QString name = piece;
            const int eq = name.indexOf(QLatin1Char('='));
            if (eq > 0) {
                name.truncate(eq); // -map=0:v 这种写法，查的是等号前面
            }
            // 字典认得的选项才上强调色：不认得的标成警告色，用户一眼看出哪里可疑
            return ffmpeg::FfmpegCommandCatalog::Find(name) != nullptr ? &option_format_ : &unknown_format_;
        }
        return nullptr; // 普通参数（程序名、选项的值、裸路径）保持默认色
    }

    QTextCharFormat option_format_;
    QTextCharFormat unknown_format_;
    QTextCharFormat path_format_;
};

} // namespace

FfmpegCommandEditorDialog::FfmpegCommandEditorDialog(const QString& command, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("展开编辑命令"));
    resize(760, 420);
    SetupUi(command);
}

void FfmpegCommandEditorDialog::SetupUi(const QString& command) {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    QLabel* hint = new QLabel(tr("换行只用于排版：运行时会按行 trim、用单个空格拼成一行。"
                                 "ffmpeg 命令行没有行继续符，换行本身不是参数。"),
                              this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::color::kTextSecondary)));
    layout->addWidget(hint);

    editor_ = new QPlainTextEdit(this);
    editor_->setFont(theme::font::monoFont(10));
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setTabChangesFocus(true); // Tab 在命令里没意义，别让它吃掉焦点
    // 一进来就按「选项 + 值」折好行：一条长命令摊开看，比横着滚有用
    editor_->setPlainText(ffmpeg::WrapCommandIntoLines(command).join(QLatin1Char('\n')));
    new CommandHighlighter(editor_->document());
    layout->addWidget(editor_, 1);

    status_label_ = new QLabel(this);
    status_label_->setWordWrap(true);
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_label_);

    QDialogButtonBox* buttons = new QDialogButtonBox(this);
    // ActionRole 而不是 AcceptRole：点「运行」只走 clicked，「确定」才走 accepted
    run_button_ = buttons->addButton(tr("运行"), QDialogButtonBox::ActionRole);
    run_button_->setObjectName(QStringLiteral("PrimaryButton"));
    run_button_->setToolTip(tr("Ctrl+Enter"));
    buttons->addButton(tr("确定"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(run_button_, &QPushButton::clicked, this, &FfmpegCommandEditorDialog::OnAcceptAndRun);
    connect(editor_, &QPlainTextEdit::textChanged, this, &FfmpegCommandEditorDialog::RefreshStatus);

    // Ctrl+Enter / Ctrl+Return 都接：小键盘上的回车与主键盘上的回车是两个键码
    for (const char* sequence : {"Ctrl+Return", "Ctrl+Enter"}) {
        QShortcut* shortcut = new QShortcut(QKeySequence(QString::fromLatin1(sequence)), this);
        connect(shortcut, &QShortcut::activated, this, &FfmpegCommandEditorDialog::OnAcceptAndRun);
    }

    RefreshStatus();
    editor_->setFocus();
}

QString FfmpegCommandEditorDialog::Command() const {
    return ffmpeg::FlattenCommandLines(editor_->toPlainText());
}

void FfmpegCommandEditorDialog::RefreshStatus() {
    if (status_label_ == nullptr) {
        return;
    }
    const QString command = Command();
    const auto parsed = ffmpeg::ParseCommandLine(command);
    QString text;
    QString color = QLatin1String(theme::color::kTextSecondary);

    if (parsed.status == ffmpeg::CommandParseStatus::Empty) {
        text = tr("还没有命令。");
    } else if (parsed.status != ffmpeg::CommandParseStatus::Ok) {
        // message 是多行的（第一行是结论，后面是建议），这里只要第一行
        text = parsed.message.section(QLatin1Char('\n'), 0, 0);
        color = QLatin1String(theme::color::kDanger);
    } else {
        const auto structure = ffmpeg::AnalyzeCommandStructure(parsed.arguments);
        if (!structure.complete) {
            text = tr("解析通过，但有些参数的归属看不出来（共 %1 个参数）—— 建议先回单行框里确认。")
                       .arg(parsed.arguments.size());
            color = QLatin1String(theme::color::kWarning);
        } else if (!structure.has_input) {
            text = tr("没有输入：命令里缺 -i。共 %1 个参数。").arg(parsed.arguments.size());
            color = QLatin1String(theme::color::kWarning);
        } else if (structure.output_count == 0) {
            text = tr("%1 个输入 → 没有输出：ffmpeg 会拒绝执行。共 %2 个参数。")
                       .arg(structure.input_count)
                       .arg(parsed.arguments.size());
            color = QLatin1String(theme::color::kWarning);
        } else {
            text = tr("结构完整：%1 个输入 → %2 个输出，共 %3 个参数。")
                       .arg(structure.input_count)
                       .arg(structure.output_count)
                       .arg(parsed.arguments.size());
            color = QLatin1String(theme::color::kSuccess);
        }
    }
    status_label_->setText(text);
    status_label_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    // 命令跑不起来时也别把「运行」按死：预检、覆盖确认这些判断在页面那边还会再过一遍，
    // 这里只是提示，不替用户做决定。
}

void FfmpegCommandEditorDialog::OnAcceptAndRun() {
    run_requested_ = true;
    accept();
}

} // namespace ui
} // namespace videoeye
