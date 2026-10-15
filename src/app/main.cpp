#include "MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Ultraviolet"));
    QApplication::setOrganizationName(QStringLiteral("Locke Werks"));
    // The .rc icon only gives Explorer the file icon; the title bar, taskbar
    // and Alt-Tab take theirs from Qt, which uses its default without this.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/ultraviolet.ico")));

    uv::app::MainWindow window;
    window.show();
    window.openFiles(QApplication::arguments().mid(1));
    return QApplication::exec();
}
