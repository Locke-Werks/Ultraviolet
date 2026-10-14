#include "MainWindow.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Ultraviolet"));
    QApplication::setOrganizationName(QStringLiteral("Locke Werks"));

    uv::app::MainWindow window;
    window.show();
    window.openFiles(QApplication::arguments().mid(1));
    return QApplication::exec();
}
