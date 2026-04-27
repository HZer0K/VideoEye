#include <QApplication>
#include <QTimer>

#include "ui/main_window/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    app.setApplicationName("VideoEye");
    app.setApplicationVersion("2.0.0");
    app.setOrganizationName("VideoEye Team");

    videoeye::ui::MainWindow window;
    window.show();

    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        const QString source = QString::fromLocal8Bit(argv[1]);
        QTimer::singleShot(0, [&window, source]() { window.OpenMedia(source, true); });
    }

    return app.exec();
}

