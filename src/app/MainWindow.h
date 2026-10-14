#pragma once

#include "uvcore/Pipeline.h"

#include <QMainWindow>

#include <atomic>

class QTableWidget;
class QComboBox;
class QSlider;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace uv::app {

// Batch upscaler window: a queue of input files, a settings panel, and a
// QThreadPool that runs uv::runPipeline per file while keeping the UI responsive.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onAddFiles();
    void onRemoveSelected();
    void onChooseOutputFolder();
    void onProcess();
    void onCancel();
    void onWorkerProgress(int row, int percent);
    void onWorkerFinished(int row, bool ok, QString errorText);

private:
    enum Column { ColName = 0, ColStatus, ColProgress, ColCount };

    void addFile(const QString& path);
    void buildUi();
    uv::PipelineSettings currentSettings() const;
    QString outputPathFor(const QString& inputPath) const;
    void setControlsForRunning(bool running);
    void maybeFinishBatch();

    // Queue model: input absolute paths, one per table row.
    QStringList inputPaths_;

    QTableWidget* table_ = nullptr;
    QComboBox* rateCombo_ = nullptr;
    QComboBox* formatCombo_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QSlider* hfSlider_ = nullptr;
    QLabel* hfValueLabel_ = nullptr;
    QCheckBox* ultrasonicCheck_ = nullptr;
    QLineEdit* outputFolderEdit_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* processButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;

    // Owned by the UI thread; read by worker threads through ProgressSink.
    std::atomic<bool> cancelFlag_{false};
    int pendingCount_ = 0; // workers dispatched but not yet finished
    bool running_ = false;
};

} // namespace uv::app
