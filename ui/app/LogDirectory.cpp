#include "ui/app/LogDirectory.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace videoeye {
namespace ui {

namespace {

// 用户级数据目录。三平台的落点：
//   Windows : C:/Users/<u>/AppData/Local/<org>/<app>
//   Linux   : ~/.local/share/<org>/<app>
//   macOS   : ~/Library/Application Support/<org>/<app>
// AppLocalDataLocation 在 Windows 上落在 AppData\Local（本就不随用户配置漫游），
// Linux 上等价 GenericDataLocation —— 日志不该进 AppConfigLocation（那是配置，会被备份同步）。
QString UserLogDirectory() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        return QString();
    return base + "/logs";
}

bool EnsureWritableDirectory(const QString& dir, QString* reason) {
    QDir d(dir);
    if (!d.exists() && !d.mkpath(".")) {
        if (reason)
            *reason = QStringLiteral("目录创建失败: %1").arg(dir);
        return false;
    }
    if (!QFileInfo(dir).isWritable()) {
        if (reason)
            *reason = QStringLiteral("目录不可写: %1").arg(dir);
        return false;
    }
    return true;
}

}  // namespace

LogFileLocation ResolveDebugLogPath(const QString& app_dir, const QString& app_name) {
    LogFileLocation out;

    // 让 QStandardPaths 拿到正确的组织名/应用名：这一步必须在 QApplication 构造之后，
    // main.cpp 里调用点已经满足；这里再兜一次底，避免有人把它挪到更早的位置。
    if (QCoreApplication::organizationName().isEmpty())
        QCoreApplication::setOrganizationName(app_name);
    if (QCoreApplication::applicationName().isEmpty())
        QCoreApplication::setApplicationName(app_name);

    const QString user_dir = UserLogDirectory();
    if (!user_dir.isEmpty() && EnsureWritableDirectory(user_dir, &out.reason)) {
        out.path = user_dir + "/videoeye_debug.log";
        return out;
    }
    if (out.reason.isEmpty())
        out.reason = QStringLiteral("系统未提供用户级数据目录");

    // 回退：exe 同目录。开发机与绿色版就是这条路径，写不了也只能认了 ——
    // 总比因为拿不到日志就拒绝启动好。
    out.fell_back_to_app_dir = true;
    out.path = app_dir + "/videoeye_debug.log";
    return out;
}

void RotateLogFileIfOversized(const QString& path, qint64 max_bytes, int backup_count) {
    if (path.isEmpty() || max_bytes <= 0)
        return;
    QFileInfo info(path);
    if (!info.exists() || info.size() < max_bytes)
        return;

    // 从最老的备份开始删，再依次往后推：.1 -> .2, 当前 -> .1
    const int keep = backup_count < 0 ? 0 : backup_count;
    const QString oldest = QStringLiteral("%1.%2").arg(path).arg(keep);
    if (keep > 0 && QFile::exists(oldest))
        QFile::remove(oldest);
    for (int i = keep - 1; i >= 1; --i) {
        const QString from = QStringLiteral("%1.%2").arg(path).arg(i);
        if (!QFile::exists(from))
            continue;
        QFile::rename(from, QStringLiteral("%1.%2").arg(path).arg(i + 1));
    }
    QFile::rename(path, QStringLiteral("%1.1").arg(path));
}

}  // namespace ui
}  // namespace videoeye
