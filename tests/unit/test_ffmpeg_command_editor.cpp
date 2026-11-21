// 「展开编辑」对话框：折行显示、压行回填、结构提示、点「运行」的那条分支。
//
// 值得单独测的理由：这个对话框只做"读改"，看起来谁都不会写错。但它站在命令
// 输入与执行之间 —— 一旦压行环节把命令改了一个字符（引号没了、空格多了），
// 用户不会收到任何提示，只会看到一个"跑得不对"的结果。所以这里的主线是
// **进去什么、出来必须还是什么**。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>

#include "ui/ffmpeg_panel/FfmpegCommandEditorDialog.h"

using videoeye::ui::FfmpegCommandEditorDialog;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_ffmpeg_command_editor";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QLabel* LabelContaining(FfmpegCommandEditorDialog& dialog, const QString& needle) {
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text().contains(needle)) {
            return label;
        }
    }
    return nullptr;
}

} // namespace

TEST(FfmpegCommandEditorDialogTests, OpensWithOneOptionPerLineAndRoundTripsTheCommand) {
    EnsureApp();

    const QString command = QStringLiteral("ffmpeg -i \"my file.mp4\" -c:v libx264 -crf 23 \"out file.mp4\"");
    FfmpegCommandEditorDialog dialog(command);

    QPlainTextEdit* editor = dialog.findChild<QPlainTextEdit*>();
    ASSERT_TRUE(editor != nullptr);
    // 一进来就折好行：程序名 / 输入 / 每个选项与它的值 / 输出
    EXPECT_EQ(5, editor->blockCount()) << editor->toPlainText().toStdString();
    EXPECT_TRUE(editor->toPlainText().startsWith(QStringLiteral("ffmpeg\n-i \"my file.mp4\"")))
        << editor->toPlainText().toStdString();

    // 进去什么、出来还是什么（这是最要紧的一条）
    EXPECT_EQ(command, dialog.Command());
    EXPECT_FALSE(dialog.RunRequested());
}

TEST(FfmpegCommandEditorDialogTests, EditsAreFlattenedBackIntoOneLine) {
    EnsureApp();

    FfmpegCommandEditorDialog dialog(QStringLiteral("ffmpeg -i in.mp4 out.mp4"));
    QPlainTextEdit* editor = dialog.findChild<QPlainTextEdit*>();
    ASSERT_TRUE(editor != nullptr);

    // 用户自己排版：空行与缩进都不该带进命令
    editor->setPlainText(QStringLiteral("ffmpeg\n\n  -i in.mp4  \n-c copy\nout.mp4\n"));
    EXPECT_EQ(QStringLiteral("ffmpeg -i in.mp4 -c copy out.mp4"), dialog.Command());
}

TEST(FfmpegCommandEditorDialogTests, StatusLineReportsStructureAndParseErrors) {
    EnsureApp();

    {
        FfmpegCommandEditorDialog dialog(QStringLiteral("ffmpeg -i in.mp4 -c copy out.mp4"));
        EXPECT_TRUE(LabelContaining(dialog, QStringLiteral("结构完整")) != nullptr);
    }
    {
        // 管道是硬错误：对话框里就要说，不能等点了运行才弹窗
        FfmpegCommandEditorDialog dialog(QStringLiteral("ffmpeg -i in.mp4 | out.mp4"));
        EXPECT_TRUE(LabelContaining(dialog, QStringLiteral("shell 操作符")) != nullptr);
    }
    {
        // 有输入没输出：ffmpeg 会直接拒绝
        FfmpegCommandEditorDialog dialog(QStringLiteral("ffmpeg -i in.mp4"));
        EXPECT_TRUE(LabelContaining(dialog, QStringLiteral("没有输出")) != nullptr);
    }
}

TEST(FfmpegCommandEditorDialogTests, RunButtonMarksRunRequested) {
    EnsureApp();

    FfmpegCommandEditorDialog dialog(QStringLiteral("ffmpeg -i in.mp4 out.mp4"));
    QPushButton* run = nullptr;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("运行")) {
            run = button;
        }
    }
    ASSERT_TRUE(run != nullptr);

    run->click();
    EXPECT_TRUE(dialog.RunRequested());
    EXPECT_EQ(QDialog::Accepted, dialog.result());
}

TEST(FfmpegCommandEditorDialogTests, OkButtonLeavesRunRequestedFalse) {
    EnsureApp();

    FfmpegCommandEditorDialog dialog(QStringLiteral("ffmpeg -i in.mp4 out.mp4"));
    QPushButton* ok = nullptr;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("确定")) {
            ok = button;
        }
    }
    ASSERT_TRUE(ok != nullptr);

    ok->click();
    EXPECT_FALSE(dialog.RunRequested()); // 「确定」只改命令，不替用户按下运行
    EXPECT_EQ(QDialog::Accepted, dialog.result());
}
