// ============================================================
// 日志落点（ui/app/LogDirectory.{h,cpp}）的行为测试
//
// 为什么值得测:
//   日志目录从"exe 同目录"迁到用户级数据目录，是为了修"装到 Program Files 后
//   启动异常、却拿不到日志"这个场景 —— 恰恰是最需要日志的时候拿不到。
//   这条路径跑在 main() 里、QApplication 构造之后，没有任何 UI 测试覆盖；
//   回退逻辑一旦写错，只有真实装机才会发现。
//
// 这里只测两件确定性的事:
//   1. 解析出来的路径必须落在用户级目录里，且父目录确实被创建出来了
//      （"不写 exe 同目录"是这次改动的核心诉求，必须钉住）
//   2. 超过上限时会滚动出 .1，且当前文件被移走（不是截断、不是覆盖）；
//      没超限则一个字节都不动 —— 滚动逻辑写反的话每次启动都会丢日志。
//
// 用 gtest + QtCore（与项目其余测试一致，不引入 QtTest 依赖）。
// ============================================================

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include "ui/app/LogDirectory.h"

namespace {

// QStandardPaths 需要组织名/应用名才知道往哪儿写。LogDirectory 内部也会兜底设一次，
// 这里提前设好是为了让"期望的落点"可独立算出来。
void SetAppIdentity() {
    QCoreApplication::setOrganizationName("VideoEye Team");
    QCoreApplication::setApplicationName("VideoEye");
}

bool WriteFile(const QString& path, int bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    const qint64 written = f.write(QByteArray(bytes, 'x'));
    f.close();
    return written == bytes;
}

QString ParentDir(const QString& path) {
    const int slash = path.lastIndexOf('/');
    return slash > 0 ? path.left(slash) : QString();
}

} // namespace

// 正常路径: 解析结果必须落在用户级数据目录，而不是传进去的 app_dir
TEST(LogDirectoryTest, ResolvesToUserDirectoryNotAppDir) {
    SetAppIdentity();

    QTemporaryDir app_dir;
    ASSERT_TRUE(app_dir.isValid());

    const videoeye::ui::LogFileLocation loc = videoeye::ui::ResolveDebugLogPath(app_dir.path());

    EXPECT_FALSE(loc.path.isEmpty());
    EXPECT_FALSE(loc.fell_back_to_app_dir) << "不该回退到程序目录: " << loc.reason.toStdString();

    const QString user_base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    ASSERT_FALSE(user_base.isEmpty());
    EXPECT_TRUE(loc.path.startsWith(user_base))
        << "日志路径 " << loc.path.toStdString() << " 不在用户目录 " << user_base.toStdString() << " 下";

    // 目录必须已经存在 —— 否则 Logger::SetLogFile 会失败
    EXPECT_TRUE(QFileInfo::exists(ParentDir(loc.path))) << "日志目录没被创建: " << ParentDir(loc.path).toStdString();
}

// 超过上限时滚动: 当前 -> .1，第二次滚动把 .1 推成 .2
TEST(LogDirectoryTest, RotatesWhenOversized) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/videoeye_debug.log";

    ASSERT_TRUE(WriteFile(path, 4096));
    ASSERT_EQ(4096, QFileInfo(path).size());

    videoeye::ui::RotateLogFileIfOversized(path, 1024, 2);

    EXPECT_FALSE(QFileInfo::exists(path)) << "超限后当前文件应被移走";
    EXPECT_TRUE(QFileInfo::exists(path + ".1")) << "应滚动出 .1";

    ASSERT_TRUE(WriteFile(path, 4096));
    videoeye::ui::RotateLogFileIfOversized(path, 1024, 2);
    EXPECT_TRUE(QFileInfo::exists(path + ".2")) << "第二次滚动应把 .1 推成 .2";
}

// 没超限就不动
TEST(LogDirectoryTest, LeavesSmallFileAlone) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/videoeye_debug.log";
    ASSERT_TRUE(WriteFile(path, 100));

    videoeye::ui::RotateLogFileIfOversized(path, 1024, 2);

    EXPECT_TRUE(QFileInfo::exists(path)) << "未超限不该被动";
    EXPECT_FALSE(QFileInfo::exists(path + ".1")) << "未超限不该产生备份";
}

// 空路径 / 非正上限必须直接返回，不能去动一个叫 "" 的文件
TEST(LogDirectoryTest, IgnoresDegenerateInputs) {
    videoeye::ui::RotateLogFileIfOversized(QString(), 1024, 2);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/videoeye_debug.log";
    ASSERT_TRUE(WriteFile(path, 4096));
    videoeye::ui::RotateLogFileIfOversized(path, 0, 2);
    EXPECT_TRUE(QFileInfo::exists(path)) << "max_bytes<=0 时不该滚动";
}
