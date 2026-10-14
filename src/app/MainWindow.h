#pragma once

#include <QMainWindow>

namespace uv::app {

// Minimal starting window. The GUI task builds the real batch queue,
// settings panel, worker threads, and spectrogram A/B on top of this.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
};

} // namespace uv::app
