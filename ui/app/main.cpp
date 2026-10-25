#include <QApplication>
#include <QMetaType>
#include <QTimer>

#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"
#include "infrastructure/logging/Logger.h"
#include "ui/app/LogDirectory.h"
#include "ui/main_window/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    app.setApplicationName("VideoEye");
    app.setApplicationVersion("2.0.0");
    app.setOrganizationName("VideoEye Team");

    // 调试日志: 写用户级数据目录（Windows AppData\Local / Linux ~/.local/share /
    // macOS ~/Library/Application Support）。装到 Program Files 这类只读安装目录时
    // 也写得进去 —— 以前固定写 exe 同目录，恰好在"装好了但启动异常"这个最需要日志
    // 的场景下拿不到日志。真写不了才回退 exe 同目录，并把这个事实打进日志。
    {
        const videoeye::ui::LogFileLocation log_location = videoeye::ui::ResolveDebugLogPath(app.applicationDirPath());
        videoeye::ui::RotateLogFileIfOversized(log_location.path);

        videoeye::Logger::GetInstance().SetLevel(videoeye::LogLevel::Debug);
        if (!videoeye::Logger::GetInstance().SetLogFile(log_location.path.toStdString())) {
            // Logger 默认同时输出到控制台，这条警告不会跟着一起丢
            videoeye::Logger::GetInstance().Warning("日志文件无法打开，本次运行只有控制台输出: " +
                                                    log_location.path.toStdString());
        }
        if (log_location.fell_back_to_app_dir) {
            videoeye::Logger::GetInstance().Warning("用户级日志目录不可用，已回退到程序目录: " +
                                                    log_location.reason.toStdString());
        }
        videoeye::Logger::GetInstance().Info("日志文件: " + log_location.path.toStdString());
    }

    qRegisterMetaType<videoeye::model::AudioVisualizationFrame>("videoeye::model::AudioVisualizationFrame");
    qRegisterMetaType<videoeye::model::PacketInfo>("videoeye::model::PacketInfo");
    qRegisterMetaType<videoeye::model::AnalysisEvent>("videoeye::model::AnalysisEvent");
    qRegisterMetaType<videoeye::model::SyncSample>("videoeye::model::SyncSample");
    qRegisterMetaType<videoeye::model::TimelineEvent>("videoeye::model::TimelineEvent");

    videoeye::ui::MainWindow window;
    window.show();

    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        const QString source = QString::fromLocal8Bit(argv[1]);
        QTimer::singleShot(0, [&window, source]() { window.OpenMedia(source, true); });
    }

    return app.exec();
}
