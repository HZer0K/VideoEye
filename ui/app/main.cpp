#include <QApplication>
#include <QMetaType>
#include <QTimer>

#include "infrastructure/logging/Logger.h"
#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"
#include "ui/main_window/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 调试日志: 写入 exe 同目录, 便于 Windows GUI 下排查卡死/崩溃 (默认仅写控制台不可见)
    {
        std::string log_path = app.applicationDirPath().toStdString() + "/videoeye_debug.log";
        videoeye::Logger::GetInstance().SetLogFile(log_path);
        videoeye::Logger::GetInstance().SetLevel(videoeye::LogLevel::Debug);
    }

    app.setApplicationName("VideoEye");
    app.setApplicationVersion("2.0.0");
    app.setOrganizationName("VideoEye Team");

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
