// 命令框的实时解释 / 补全组件：提示行跟不跟光标、Tab 能不能列出候选。
//
// 这里**不**重测位置判断（那是 core 的 FfmpegCommandInputHintTests）。这一层只回答
// 一件 UI 才会错的事：提示行说的，是不是光标处那个词；以及 Tab 之后下拉里有没有东西。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>

#include "ui/ffmpeg_panel/FfmpegCommandInputAssist.h"

using videoeye::ui::FfmpegCommandInputAssist;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_ffmpeg_command_input_assist";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

} // namespace

TEST(FfmpegCommandInputAssistTests, EmptyCommandShowsGuidance) {
    EnsureApp();

    QWidget host;
    auto* edit = new QLineEdit(&host);
    FfmpegCommandInputAssist assist(edit, &host);

    auto* label = qobject_cast<QLabel*>(assist.HintWidget());
    ASSERT_TRUE(label != nullptr);
    EXPECT_TRUE(label->text().contains(QStringLiteral("在这里写命令"))) << label->text().toStdString();
}

TEST(FfmpegCommandInputAssistTests, HintDescribesTheWordUnderTheCursor) {
    EnsureApp();

    QWidget host;
    auto* edit = new QLineEdit(&host);
    FfmpegCommandInputAssist assist(edit, &host);
    auto* label = qobject_cast<QLabel*>(assist.HintWidget());
    ASSERT_TRUE(label != nullptr);

    // 刚输完 -crf 加空格：提示行应该解释 -crf，并说下一步是填值
    edit->setText(QStringLiteral("ffmpeg -i in.mp4 -crf "));
    edit->setCursorPosition(edit->text().size());
    EXPECT_TRUE(label->text().contains(QStringLiteral("CRF"))) << label->text().toStdString();
    EXPECT_TRUE(label->text().contains(QStringLiteral("下一步"))) << label->text().toStdString();

    // 光标移到输出文件名上：同一句话要换成"输出文件"
    edit->setText(QStringLiteral("ffmpeg -i in.mp4 out.mp4"));
    edit->setCursorPosition(edit->text().size());
    EXPECT_TRUE(label->text().contains(QStringLiteral("输出文件"))) << label->text().toStdString();
}

TEST(FfmpegCommandInputAssistTests, HintListsThePiecesThatCanBeTypedNext) {
    EnsureApp();

    QWidget host;
    auto* edit = new QLineEdit(&host);
    FfmpegCommandInputAssist assist(edit, &host);
    auto* label = qobject_cast<QLabel*>(assist.HintWidget());
    ASSERT_TRUE(label != nullptr);

    // 半截选项名：提示行要把"接下去能写什么"直接列出来（不依赖下拉是否弹开 ——
    // 下拉的显隐由 QCompleter 决定，offscreen 平台上不可靠）。
    // 必须带上输入：-crf 是**输出侧**选项，没有 -i 之前列出来反而是误导。
    const QString command = QStringLiteral("ffmpeg -i in.mp4 -cr");
    edit->setText(command);
    edit->setCursorPosition(command.size()); // 词尾：前缀是完整的 "-cr"
    EXPECT_TRUE(label->text().contains(QStringLiteral("-crf"))) << label->text().toStdString();
}

TEST(FfmpegCommandInputAssistTests, ConstructingTheAssistKeepsTheEditClickable) {
    EnsureApp();

    QWidget host;
    auto* edit = new QLineEdit(&host);
    FfmpegCommandInputAssist assist(edit, &host);

    // 回归：曾给补全下拉设过 Qt::NoFocus，想"把焦点留在输入框"。但 QCompleter 把输入框
    // 设成了下拉的焦点代理，而 QWidget::setFocusPolicy 是沿焦点代理往下写的 —— 这一行
    // 实际把 QLineEdit 自己设成了 NoFocus：点不进去、敲字毫无反应，界面上只看到占位
    // 文字一动不动，看起来像"整个命令框坏了"。
    EXPECT_TRUE((edit->focusPolicy() & Qt::ClickFocus) != 0) << "命令框焦点策略=" << int(edit->focusPolicy());
    EXPECT_TRUE(edit->isEnabled());
    EXPECT_FALSE(edit->isReadOnly());
}

TEST(FfmpegCommandInputAssistTests, TabOpensCandidatesThenAcceptsThem) {
    EnsureApp();

    QWidget host;
    auto* edit = new QLineEdit(&host);
    FfmpegCommandInputAssist assist(edit, &host);
    // 必须带上输入：-crf 是**输出侧**选项，没有 -i 之前列出来反而是误导。
    const QString command = QStringLiteral("ffmpeg -i in.mp4 -cr");
    edit->setText(command);
    edit->setCursorPosition(command.size());
    EXPECT_TRUE(assist.PopupVisible()); // 正在打一个词：自动弹

    // Esc 关掉之后，Tab 是"我要看候选"的主动请求。这里曾经拿"弹窗是否已可见"
    // 当判据 —— 窗口映射有一拍延迟，判早了就把 Tab 放行，结果是焦点跑掉、命令一个字
    // 也没补上，用户看到的是"补全没反应"。
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(edit, &esc);
    EXPECT_FALSE(assist.PopupVisible());

    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(edit, &tab);
    EXPECT_TRUE(assist.PopupVisible());
    EXPECT_EQ(command, edit->text()); // 这一下只是列出来，不动文本

    // 再按一次 Tab：接受第一项（-crf），并补上分隔空格
    QApplication::sendEvent(edit, &tab);
    EXPECT_EQ(QStringLiteral("ffmpeg -i in.mp4 -crf "), edit->text().toStdString());
}
