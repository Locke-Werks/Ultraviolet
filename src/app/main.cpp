#include "MainWindow.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Ultraviolet"));
    QApplication::setOrganizationName(QStringLiteral("Locke Werks"));

    uv::app::MainWindow window;
    window.show();
    return QApplication::exec();
}
