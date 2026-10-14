#pragma once

#include "uvcore/Pipeline.h"
#include "uvcore/SpectralAnalyzer.h"

#include <QMainWindow>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

class QTableWidget;
class QComboBox;
class QSlider;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace uv::app {

class AudioPlayer;
class SpectrumPlot;

// Preview-first upscaler window. Selecting a file loads it and shows its
// spectrum; Apply processes it in memory and overlays the result; the user
// auditions original against processed; only Save / Save All write to disk.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Queue files given on the command line or by "Open with".
    void openFiles(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onAddFiles();
    void onRemoveSelected();
    void onChooseOutputFolder();
    void onSelectionChanged();
    void onSettingsChanged();
    void onApply();
    void onSave();
    void onSaveAll();
    void onCancel();
    void onPlayOriginal();
    void onPlayProcessed();
    void onStopPlayback();
    void onPlayerStopped();
    void onBatchProgress(int row, int percent);
    void onBatchFinished(int row, bool ok, QString errorText);

private:
    enum Column { ColName = 0, ColStatus, ColProgress, ColCount };

    struct Entry {
        int id = 0;
        QString path;

        // Full buffers are held only for the selected entry: at 192 kHz in
        // double precision one track can be around a gigabyte.
        std::shared_ptr<const uv::AudioBuffer> source;
        std::shared_ptr<const uv::AudioBuffer> processed;

        // Small, so kept for every entry once computed.
        std::optional<uv::SpectrumInfo> info;
        std::optional<uv::SpectrumCurve> sourceCurve;
        std::optional<uv::SpectrumCurve> processedCurve;
        uv::PipelineSettings appliedWith;

        // Monotonic tokens: a finished job whose token is stale is discarded.
        int loadToken = 0;
        int applyToken = 0;
        bool loading = false;
        bool applyAfterLoad = false;

        // Source facts for the info readout.
        int sourceBitDepth = 0;
        int channels = 0;
        double seconds = 0.0;
    };

    void buildUi();
    void addFile(const QString& path);
    uv::PipelineSettings currentSettings() const;
    QString outputPathFor(const QString& inputPath) const;

    Entry* entryById(int id);
    int rowOf(int id) const;
    Entry* selectedEntry();
    bool previewValid(const Entry& e) const;

    void startLoad(Entry& e);
    void startApply(Entry& e);
    void releaseBuffersExcept(int keepId);

    void setRowStatus(int id, const QString& text, const QString& tooltip = QString());
    void setRowProgress(int id, int percent);
    void refreshView();
    void refreshControls();
    void setBusy(bool busy);

    std::vector<Entry> entries_; // parallel to table rows
    int nextId_ = 1;

    QTableWidget* table_ = nullptr;
    SpectrumPlot* plot_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QComboBox* rateCombo_ = nullptr;
    QComboBox* formatCombo_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QSlider* hfSlider_ = nullptr;
    QLabel* hfValueLabel_ = nullptr;
    QCheckBox* ultrasonicCheck_ = nullptr;
    QLineEdit* outputFolderEdit_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveAllButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* playOriginalButton_ = nullptr;
    QPushButton* playProcessedButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;

    AudioPlayer* player_ = nullptr;

    // Read by worker threads through ProgressSink::cancelled().
    std::atomic<bool> cancelFlag_{false};
    bool busy_ = false;     // an Apply, Save, or Save All is running
    int applyingId_ = 0;    // entry being applied, 0 if none
    int batchPending_ = 0;  // Save All workers not yet finished
};

} // namespace uv::app
