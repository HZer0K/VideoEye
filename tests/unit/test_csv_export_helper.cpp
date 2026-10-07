// CSV 导出统一样板的写入端（ui/analysis_panel/AnalysisPageSupport.h 的 WriteCsvStream）。
//
// 为什么值得单测: 这一段以前在 20 来处各抄一份，BOM 有的写有的没写（漏的那一半
// 用 Excel 打开中文列直接乱码，只有真导出一次才看得见），编码设置更是每处都能忘。
// 现在收成一个纯函数，这里不建 QApplication、不弹任何对话框，只钉三件事：
//   1) UTF-8 BOM + 正文逐字落盘（QIODevice::Text 在 Windows 会把 \n 写成 \r\n，
//      读回时归一化再比）；
//   2) 覆盖既有文件内容而不是追加（二次导出同名文件是常态）；
//   3) 打不开时返回 false 且不留半截文件 —— 调用方据此弹"无法写入文件"，
//      静默返回 true 会让用户以为导出成功。
//
// 用例数会进 scripts/summarize_tests.py 的统计（README 表格与 CI 的 --expect）。

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace {

using videoeye::ui::WriteCsvStream;

QString ReadAllText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

TEST(CsvExportHelperTest, WritesUtf8BomHeaderAndRows) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("out.csv");

    ASSERT_TRUE(WriteCsvStream(path, [](QTextStream& stream) {
        stream << "index,score\n";
        stream << 1 << "," << QString::number(2.5, 'f', 2) << "\n";
    }));

    QByteArray raw;
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        raw = f.readAll();
    }
    ASSERT_FALSE(raw.isEmpty());
    // BOM 必须按**原始字节**检查：QString::fromUtf8 默认会把开头的 3 字节 BOM
    // 消费掉（QStringConverter 的初始 BOM 语义），读成 QString 反而看不见它。
    EXPECT_EQ(raw.left(3), QByteArray("\xEF\xBB\xBF", 3))
        << "文件头必须是 UTF-8 BOM，否则 Excel 打开中文列乱码";

    QString body = QString::fromUtf8(raw);
    // fromUtf8 若没消费 BOM（不同 Qt 版本语义），正文可能还带 U+FEFF，先摘掉再比
    if (body.startsWith(QChar(0xFEFF))) body.remove(0, 1);
    body.replace("\r\n", "\n");  // Text 模式在 Windows 写 CRLF，按逻辑行比较
    EXPECT_EQ(body, QStringLiteral("index,score\n1,2.50\n"));
}

TEST(CsvExportHelperTest, OverwritesExistingFileInsteadOfAppending) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("out.csv");

    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("stale content that must disappear");
    }
    ASSERT_TRUE(WriteCsvStream(path, [](QTextStream& stream) { stream << "a,b\n"; }));

    const QString text = ReadAllText(path);
    EXPECT_FALSE(text.contains(QStringLiteral("stale")))
        << "二次导出复用了旧文件，应在打开时就截断";
    EXPECT_TRUE(text.contains(QStringLiteral("a,b")));
}

TEST(CsvExportHelperTest, ReturnsFalseWhenTargetCannotBeOpened) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // 目标目录不存在 -> QFile 打不开。助手必须返回 false（调用方弹"无法写入文件"），
    // 而不是静默成功。
    const QString path = dir.filePath("no_such_subdir/out.csv");
    EXPECT_FALSE(WriteCsvStream(path, [](QTextStream& stream) { stream << "a\n"; }));
    EXPECT_FALSE(QFile::exists(path));
}

} // namespace