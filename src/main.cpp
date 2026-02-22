#include <QApplication>
#include <QSurfaceFormat>

#include "app/MainWindow.h"

int main(int argc, char* argv[]) {
    QSurfaceFormat format;
    format.setVersion(4, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    trace::MainWindow window;
    window.show();
    return app.exec();
}
