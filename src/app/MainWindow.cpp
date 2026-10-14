#include "MainWindow.h"

#include <QLabel>

namespace uv::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Ultraviolet"));
    resize(900, 600);

    auto* placeholder = new QLabel(
        QStringLiteral("Ultraviolet - audio upscaler"), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);
}

} // namespace uv::app
