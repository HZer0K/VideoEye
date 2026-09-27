#include "core/ffmpeg/FfmpegToolLocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

namespace videoeye {
namespace ffmpegtool {
namespace {

QString BundleName() {
    return QString::fromLatin1(VIDEOEYE_FFMPEG_TOOL_NAME);
}

QString BundledPath() {
    if (!VIDEOEYE_FFMPEG_TOOL_BUNDLED) {
        return QString();
    }
    const QString name = BundleName();
    if (name.isEmpty()) {
        return QString();
    }
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

QString ConfiguredAtBuildTime() {
    return QString::fromLatin1(VIDEOEYE_FFMPEG_TOOL_PATH);
}

QStringList CandidateNames() {
#ifdef Q_OS_WIN
    return {QStringLiteral("ffmpeg.exe"), QStringLiteral("ffmpeg")};
#else
    return {QStringLiteral("ffmpeg")};
#endif
}

/// 把 %LOCALAPPDATA% 之类的环境变量展开。Linux/macOS 上的 ~ 也一起处理。
QString ExpandPath(const QString& raw) {
    QString out = raw;
    const auto env = QProcessEnvironment::systemEnvironment();
    static const QRegularExpression win_var(QStringLiteral(R"(%([A-Za-z0-9_()]+)%)"));
    QRegularExpressionMatch m = win_var.match(out);
    while (m.hasMatch()) {
        const QString name = m.captured(1);
        const QString value = env.value(name);
        out.replace(m.capturedStart(), m.capturedLength(), value);
        m = win_var.match(out);
    }
    if (out.startsWith(QLatin1Char('~'))) {
        out = QDir::homePath() + out.mid(1);
    }
    return out;
}

}  // namespace

bool FfmpegToolLocator::IsExecutable(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
#ifdef Q_OS_WIN
    // Windows 没有 POSIX 可执行位：QFileInfo::permission(Exe*) 只是从 CRT 的
    // 文件属性反推出来的近似值，解压出来的 .exe 完全可能被判成"不可执行"。
    // 这里只要它是个可执行类型的文件就放行 —— 能不能真跑起来，交给 QProcess 报错。
    const QString suffix = info.suffix().toLower();
    if (suffix == QLatin1String("exe") || suffix == QLatin1String("bat") ||
        suffix == QLatin1String("cmd") || suffix == QLatin1String("com")) {
        return true;
    }
    return info.permission(QFile::ExeUser) || info.permission(QFile::ExeOwner);
#else
    return info.permission(QFile::ExeUser) || info.permission(QFile::ExeOwner) ||
           info.permission(QFile::ExeGroup) || info.permission(QFile::ExeOther);
#endif
}

QString FfmpegToolLocator::FindInDirectory(const QString& dir) {
    if (dir.isEmpty()) {
        return QString();
    }
    const QDir base(ExpandPath(dir));
    if (!base.exists()) {
        return QString();
    }
    for (const QString& name : CandidateNames()) {
        const QString candidate = base.filePath(name);
        if (IsExecutable(candidate)) {
            return QDir::toNativeSeparators(candidate);
        }
    }
    return QString();
}

QStringList FfmpegToolLocator::CommonSearchDirs() {
    QStringList dirs;
#ifdef Q_OS_WIN
    // Windows 没有统一安装位置: 包管理器、手工解压、绿色版各有各的习惯。
    // 只列"真的有人这么装"的目录，别把整个 Program Files 扫一遍。
    dirs << QStringLiteral("%ProgramFiles%\\ffmpeg\\bin")
         << QStringLiteral("%ProgramFiles%\\ffmpeg")
         << QStringLiteral("%ProgramFiles(x86)%\\ffmpeg\\bin")
         << QStringLiteral("%LOCALAPPDATA%\\Programs\\ffmpeg\\bin")
         << QStringLiteral("%LOCALAPPDATA%\\Programs\\ffmpeg")
         << QStringLiteral("%ProgramData%\\chocolatey\\bin")           // choco install ffmpeg
         << QStringLiteral("%USERPROFILE%\\scoop\\apps\\ffmpeg\\current\\bin")  // scoop
         << QStringLiteral("%ProgramFiles%\\Gyan\\ffmpeg\\bin")        // winget Gyan.FFmpeg
         << QStringLiteral("C:\\ffmpeg\\bin")
         << QStringLiteral("C:\\Program Files\\ffmpeg\\bin");
#elif defined(Q_OS_MACOS)
    dirs << QStringLiteral("/opt/homebrew/bin")      // Apple Silicon 的 Homebrew
         << QStringLiteral("/usr/local/bin")         // Intel 的 Homebrew / 手工安装
         << QStringLiteral("/opt/local/bin")         // MacPorts
         << QStringLiteral("/opt/homebrew/sbin")
         << QStringLiteral("/usr/local/opt/ffmpeg/bin")
         << QStringLiteral("/Applications");
#else
    dirs << QStringLiteral("/usr/bin")
         << QStringLiteral("/usr/local/bin")
         << QStringLiteral("/snap/bin")
         << QStringLiteral("/var/lib/flatpak/exports/bin")
         << QStringLiteral("~/.local/bin")
         << QStringLiteral("/opt/ffmpeg/bin")
         << QStringLiteral("/usr/lib/ffmpeg");
#endif
    return dirs;
}

FfmpegToolInfo FfmpegToolLocator::Resolve(const QString& configured_path) {
    FfmpegToolInfo info;
    info.bundled = VIDEOEYE_FFMPEG_TOOL_BUNDLED != 0;
    info.bundled_name = BundleName();

    // 1) 用户设置: 即使文件不存在也要返回它 —— 界面要能显示"你填的路径不存在"
    if (!configured_path.trimmed().isEmpty()) {
        info.path = configured_path.trimmed();
        info.exists = IsExecutable(info.path);
        info.origin = QStringLiteral("用户设置");
        return info;
    }

    // 2) 随包分发
    const QString bundled = BundledPath();
    if (!bundled.isEmpty() && IsExecutable(bundled)) {
        info.path = bundled;
        info.exists = true;
        info.origin = QStringLiteral("随包分发");
        return info;
    }

    // 3) 构建期探测到的绝对路径
    const QString buildtime = ConfiguredAtBuildTime();
    if (!buildtime.isEmpty() && IsExecutable(buildtime)) {
        info.path = buildtime;
        info.exists = true;
        info.origin = QStringLiteral("构建期探测");
        return info;
    }

    // 4) PATH
    for (const QString& name : CandidateNames()) {
        const QString from_path = QStandardPaths::findExecutable(name);
        if (!from_path.isEmpty()) {
            info.path = from_path;
            info.exists = true;
            info.origin = QStringLiteral("PATH");
            return info;
        }
    }

    // 5) 常见安装位置（装了但没进 PATH 的情况，Windows 上很常见）
    for (const QString& dir : CommonSearchDirs()) {
        const QString found = FindInDirectory(dir);
        if (!found.isEmpty()) {
            info.path = found;
            info.exists = true;
            info.origin = QStringLiteral("常见安装位置");
            return info;
        }
    }

    // 都没找到: 至少把"随包那份本来应该在哪"告诉用户，比空着好排查
    info.path = bundled.isEmpty() ? QString() : bundled;
    info.exists = false;
    info.origin = QStringLiteral("未找到");
    return info;
}

QString FfmpegToolLocator::PlatformName() {
#ifdef Q_OS_WIN
    return QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macOS");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Linux");
#else
    return QStringLiteral("其他");
#endif
}

FfmpegInstallGuide FfmpegToolLocator::InstallGuide() {
    FfmpegInstallGuide guide;
    guide.platform = PlatformName();

#ifdef Q_OS_WIN
    guide.headline =
        QStringLiteral("Windows 不自带 ffmpeg。装一次即可，之后本页会自动找到它。");
    guide.package_commands = {
        QStringLiteral("winget install --id Gyan.FFmpeg -e"),
        QStringLiteral("scoop install ffmpeg"),
        QStringLiteral("choco install ffmpeg"),
    };
    guide.download_url = QStringLiteral("https://www.gyan.dev/ffmpeg/builds/#release-builds");
    guide.manual_steps = {
        QStringLiteral("下载 ffmpeg-release-essentials.zip（Windows 静态构建，约 80MB）"),
        QStringLiteral("解压到例如 C:\\ffmpeg，得到 C:\\ffmpeg\\bin\\ffmpeg.exe"),
        QStringLiteral("（可选）把 C:\\ffmpeg\\bin 加进系统 PATH 后重开本程序"),
        QStringLiteral("不想改 PATH 也行：点本页的「浏览…」直接选中 ffmpeg.exe，路径会被记住"),
    };
    guide.note = QStringLiteral(
        "VideoEye 的 Windows 安装包若随包分发了 ffmpeg.exe，会自动优先用它，无需另外安装。");
#elif defined(Q_OS_MACOS)
    guide.headline =
        QStringLiteral("macOS 不自带 ffmpeg。用 Homebrew 装最省事，装完本页会自动在两个常见目录里找到。");
    guide.package_commands = {
        QStringLiteral("brew install ffmpeg"),
        QStringLiteral("sudo port install ffmpeg"),  // MacPorts 备选
    };
    guide.download_url = QStringLiteral("https://ffmpeg.org/download.html#build-mac");
    guide.manual_steps = {
        QStringLiteral("Apple Silicon 装完在 /opt/homebrew/bin/ffmpeg，Intel 在 /usr/local/bin/ffmpeg"),
        QStringLiteral("若用自定义前缀或静态构建，点本页「浏览…」指定绝对路径"),
        QStringLiteral("macOS 会拦截未签名的下载程序：首次运行如被 Gatekeeper 拦下，"
                       "在「系统设置 → 隐私与安全性」里点「仍要打开」"),
    };
    guide.note = QStringLiteral(
        "Homebrew 装的 ffmpeg 依赖一堆 dylib，别直接把它拷进别的目录使用。");
#else
    guide.headline =
        QStringLiteral("Linux 上 ffmpeg 由发行版提供，一条命令就能装好。");
    guide.package_commands = {
        QStringLiteral("sudo apt install -y ffmpeg"),        // Debian / Ubuntu
        QStringLiteral("sudo dnf install -y ffmpeg-free"),   // Fedora
        QStringLiteral("sudo pacman -S ffmpeg"),             // Arch
        QStringLiteral("sudo zypper install -y ffmpeg"),     // openSUSE
    };
    guide.download_url = QStringLiteral("https://ffmpeg.org/download.html#build-linux");
    guide.manual_steps = {
        QStringLiteral("发行版仓库里的版本通常偏旧，但和系统的 .so 完全匹配，优先用它"),
        QStringLiteral("需要新版本可下载静态构建（johnvansickle.com），解压后在本页「浏览…」指定路径"),
        QStringLiteral("用 AppImage / Flatpak / Snap 安装的话路径不在标准位置，必须手动指定"),
    };
    guide.note = QStringLiteral(
        "Linux 版刻意不随包分发 ffmpeg：复制进安装树会和系统的 libav* 版本打架。");
#endif

    return guide;
}

QString FfmpegToolLocator::ParseVersionLine(const QString& version_output) {
    if (version_output.isEmpty()) {
        return QString();
    }
    // 形如: ffmpeg version 8.1.2-full_build-www.gyan.dev Copyright (c) 2000-2025 the FFmpeg developers
    static const QRegularExpression re(QStringLiteral(R"(ffmpeg\s+version\s+(\S+))"));
    const auto match = re.match(version_output);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    // 退一步: 只要第一行里还有东西就原样返回（不同发行版/自编译版本格式差别很大）
    const QString first_line = version_output.section(QLatin1Char('\n'), 0, 0).trimmed();
    return first_line.isEmpty() ? QString() : first_line;
}

}  // namespace ffmpegtool
}  // namespace videoeye
