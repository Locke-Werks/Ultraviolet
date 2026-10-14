#include "MainWindow.h"

#include "AudioPlayer.h"
#include "ProcessWorker.h"
#include "SpectrumPlot.h"

#include "uvcore/AudioFile.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <functional>

namespace uv::app {

namespace {

constexpr auto kSupportedFilter =
    "Audio files (*.wav *.flac *.aiff *.aif);;All files (*)";

bool isSupportedAudio(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("wav") || suffix == QLatin1String("flac") ||
           suffix == QLatin1String("aiff") || suffix == QLatin1String("aif");
}

bool sameSettings(const uv::PipelineSettings& a, const uv::PipelineSettings& b) {
    return a.targetRate == b.targetRate && a.outputFormat == b.outputFormat &&
           a.mode == b.mode && a.hfStrength == b.hfStrength && a.ultrasonic == b.ultrasonic;
}

QString kHz(double hz) {
    return QString::number(hz / 1000.0, 'f', hz >= 100000.0 ? 0 : 1) + QStringLiteral(" kHz");
}

QString duration(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Forwards core progress to the UI thread. Posting only on whole-percent
// changes keeps a long file from flooding the event loop.
class UiProgress : public uv::ProgressSink {
public:
    UiProgress(std::function<void(int)> post, const std::atomic<bool>* cancel)
        : post_(std::move(post)), cancel_(cancel) {}
    void onProgress(double f) override {
        const int pct = std::clamp(static_cast<int>(f * 100.0 + 0.5), 0, 100);
        if (pct != last_) {
            last_ = pct;
            post_(pct);
        }
    }
    bool cancelled() const override {
        return cancel_ && cancel_->load(std::memory_order_relaxed);
    }

private:
    std::function<void(int)> post_;
    const std::atomic<bool>* cancel_;
    int last_ = -1;
};

// Source and processed curves are overlaid, so they must share the same
// frequency resolution: a fixed FFT size at 4x the rate gives 4x wider bins,
// which smears low-frequency peaks and lifts the noise floor by about 6 dB.
// Scale the FFT with the rate to hold bin width near 5.9 Hz.
uv::SpectrumCurve displayCurve(const uv::AudioBuffer& b) {
    int n = 8192;
    while (n < (1 << 20) && n * 48000.0 < 8192.0 * b.sampleRate) n *= 2;
    return uv::computeSpectrumCurve(b, n);
}

struct LoadResult {
    std::shared_ptr<const uv::AudioBuffer> buffer;
    uv::SpectrumInfo info;
    uv::SpectrumCurve curve;
    QString error;
};

struct ApplyResult {
    std::shared_ptr<const uv::AudioBuffer> buffer; // null if cancelled
    uv::SpectrumCurve curve;
};

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Ultraviolet"));
    setWindowIcon(QIcon(QStringLiteral(":/ultraviolet.png")));
    resize(1280, 800);
    setAcceptDrops(true);

    player_ = new AudioPlayer(this);
    connect(player_, &AudioPlayer::stopped, this, &MainWindow::onPlayerStopped);
    connect(player_, &AudioPlayer::started, this, &MainWindow::refreshControls);
    connect(player_, &AudioPlayer::error, this, [this](const QString& msg) {
        hintLabel_->setText(QStringLiteral("Playback: ") + msg);
        refreshControls();
    });

    buildUi();
    refreshView();
    refreshControls();
}

MainWindow::~MainWindow() {
    // Workers post back to this object, so none may outlive it.
    cancelFlag_.store(true, std::memory_order_relaxed);
    QThreadPool::globalInstance()->clear();
    QThreadPool::globalInstance()->waitForDone();
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);

    // Left: queue on top, spectrum and transport below, split so the plot can
    // take most of the height.
    auto* split = new QSplitter(Qt::Vertical, central);

    auto* queuePane = new QWidget(split);
    auto* queueLayout = new QVBoxLayout(queuePane);
    queueLayout->setContentsMargins(0, 0, 0, 0);

    table_ = new QTableWidget(0, ColCount, queuePane);
    table_->setHorizontalHeaderLabels({QStringLiteral("File"), QStringLiteral("Status"),
                                       QStringLiteral("Progress")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ColProgress, QHeaderView::Fixed);
    table_->setColumnWidth(ColProgress, 160);
    table_->verticalHeader()->setVisible(false);
    queueLayout->addWidget(table_, 1);

    auto* queueButtons = new QHBoxLayout();
    addButton_ = new QPushButton(QStringLiteral("Add files..."), queuePane);
    removeButton_ = new QPushButton(QStringLiteral("Remove selected"), queuePane);
    queueButtons->addWidget(addButton_);
    queueButtons->addWidget(removeButton_);
    queueButtons->addStretch(1);
    queueLayout->addLayout(queueButtons);

    auto* viewPane = new QWidget(split);
    auto* viewLayout = new QVBoxLayout(viewPane);
    viewLayout->setContentsMargins(0, 0, 0, 0);

    plot_ = new SpectrumPlot(viewPane);
    viewLayout->addWidget(plot_, 1);

    infoLabel_ = new QLabel(viewPane);
    infoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    viewLayout->addWidget(infoLabel_);

    auto* transport = new QHBoxLayout();
    playOriginalButton_ = new QPushButton(QStringLiteral("Play original"), viewPane);
    playProcessedButton_ = new QPushButton(QStringLiteral("Play processed"), viewPane);
    stopButton_ = new QPushButton(QStringLiteral("Stop"), viewPane);
    hintLabel_ = new QLabel(viewPane);
    hintLabel_->setStyleSheet(QStringLiteral("color: #ffb347;"));
    transport->addWidget(playOriginalButton_);
    transport->addWidget(playProcessedButton_);
    transport->addWidget(stopButton_);
    transport->addSpacing(12);
    transport->addWidget(hintLabel_, 1);
    viewLayout->addLayout(transport);

    split->addWidget(queuePane);
    split->addWidget(viewPane);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    root->addWidget(split, 1);

    // Right: settings and actions.
    auto* settings = new QGroupBox(QStringLiteral("Settings"), central);
    auto* form = new QVBoxLayout(settings);

    form->addWidget(new QLabel(QStringLiteral("Target sample rate"), settings));
    rateCombo_ = new QComboBox(settings);
    rateCombo_->addItem(QStringLiteral("88.2 kHz"), 88200);
    rateCombo_->addItem(QStringLiteral("96 kHz"), 96000);
    rateCombo_->addItem(QStringLiteral("176.4 kHz"), 176400);
    rateCombo_->addItem(QStringLiteral("192 kHz"), 192000);
    rateCombo_->setCurrentIndex(3);
    form->addWidget(rateCombo_);

    form->addWidget(new QLabel(QStringLiteral("Output format"), settings));
    formatCombo_ = new QComboBox(settings);
    formatCombo_->addItem(QStringLiteral("32-bit float"), static_cast<int>(uv::OutputFormat::Float32));
    formatCombo_->addItem(QStringLiteral("24-bit PCM"), static_cast<int>(uv::OutputFormat::Pcm24));
    form->addWidget(formatCombo_);

    form->addWidget(new QLabel(QStringLiteral("Mode"), settings));
    modeCombo_ = new QComboBox(settings);
    modeCombo_->addItem(QStringLiteral("Mastering"), static_cast<int>(uv::PipelineSettings::Mode::Mastering));
    modeCombo_->addItem(QStringLiteral("Rescue"), static_cast<int>(uv::PipelineSettings::Mode::Rescue));
    modeCombo_->addItem(QStringLiteral("Auto"), static_cast<int>(uv::PipelineSettings::Mode::Auto));
    modeCombo_->setCurrentIndex(2);
    form->addWidget(modeCombo_);

    auto* hfRow = new QHBoxLayout();
    hfRow->addWidget(new QLabel(QStringLiteral("HF strength"), settings));
    hfValueLabel_ = new QLabel(QStringLiteral("50"), settings);
    hfValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hfRow->addStretch(1);
    hfRow->addWidget(hfValueLabel_);
    form->addLayout(hfRow);

    hfSlider_ = new QSlider(Qt::Horizontal, settings);
    hfSlider_->setRange(0, 100);
    hfSlider_->setValue(50);
    form->addWidget(hfSlider_);

    ultrasonicCheck_ = new QCheckBox(QStringLiteral("Generate ultrasonic content"), settings);
    form->addWidget(ultrasonicCheck_);

    form->addSpacing(8);
    form->addWidget(new QLabel(QStringLiteral("Output folder"), settings));
    auto* outRow = new QHBoxLayout();
    outputFolderEdit_ = new QLineEdit(settings);
    outputFolderEdit_->setPlaceholderText(QStringLiteral("Next to each input (_uv suffix)"));
    outputFolderEdit_->setReadOnly(true);
    auto* chooseOut = new QPushButton(QStringLiteral("Choose..."), settings);
    outRow->addWidget(outputFolderEdit_, 1);
    outRow->addWidget(chooseOut);
    form->addLayout(outRow);

    form->addStretch(1);

    applyButton_ = new QPushButton(QStringLiteral("Apply"), settings);
    applyButton_->setToolTip(QStringLiteral("Process the selected file in memory and show the result. Nothing is written."));
    saveButton_ = new QPushButton(QStringLiteral("Save..."), settings);
    saveButton_->setToolTip(QStringLiteral("Export the selected file with the current settings."));
    saveAllButton_ = new QPushButton(QStringLiteral("Save all"), settings);
    saveAllButton_->setToolTip(QStringLiteral("Export every queued file with the current settings to the output folder."));
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), settings);
    form->addWidget(applyButton_);
    form->addWidget(saveButton_);
    form->addWidget(saveAllButton_);
    form->addWidget(cancelButton_);

    settings->setFixedWidth(300);
    root->addWidget(settings);
    setCentralWidget(central);

    connect(addButton_, &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(removeButton_, &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
    connect(chooseOut, &QPushButton::clicked, this, &MainWindow::onChooseOutputFolder);
    connect(applyButton_, &QPushButton::clicked, this, &MainWindow::onApply);
    connect(saveButton_, &QPushButton::clicked, this, &MainWindow::onSave);
    connect(saveAllButton_, &QPushButton::clicked, this, &MainWindow::onSaveAll);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(playOriginalButton_, &QPushButton::clicked, this, &MainWindow::onPlayOriginal);
    connect(playProcessedButton_, &QPushButton::clicked, this, &MainWindow::onPlayProcessed);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStopPlayback);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);

    connect(hfSlider_, &QSlider::valueChanged, this, [this](int v) {
        hfValueLabel_->setText(QString::number(v));
    });
    connect(rateCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onSettingsChanged);
    connect(formatCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onSettingsChanged);
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onSettingsChanged);
    connect(hfSlider_, &QSlider::valueChanged, this, &MainWindow::onSettingsChanged);
    connect(ultrasonicCheck_, &QCheckBox::toggled, this, &MainWindow::onSettingsChanged);
}

// ---- Queue -----------------------------------------------------------------

void MainWindow::openFiles(const QStringList& paths) {
    for (const QString& p : paths) {
        if (isSupportedAudio(p) && QFileInfo(p).isFile()) addFile(p);
    }
}

void MainWindow::onAddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add audio files"), QString(), QString::fromLatin1(kSupportedFilter));
    for (const QString& f : files) addFile(f);
}

void MainWindow::addFile(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (const Entry& e : entries_) {
        if (e.path == absolute) return; // no duplicates
    }

    Entry e;
    e.id = nextId_++;
    e.path = absolute;
    entries_.push_back(std::move(e));

    const int row = table_->rowCount();
    table_->insertRow(row);
    auto* nameItem = new QTableWidgetItem(QFileInfo(absolute).fileName());
    nameItem->setToolTip(absolute);
    table_->setItem(row, ColName, nameItem);
    table_->setItem(row, ColStatus, new QTableWidgetItem(QStringLiteral("Not loaded")));
    auto* bar = new QProgressBar(table_);
    bar->setRange(0, 100);
    bar->setValue(0);
    table_->setCellWidget(row, ColProgress, bar);

    // Selecting the first file shows something immediately.
    if (table_->selectionModel()->selectedRows().isEmpty()) table_->selectRow(row);
    refreshControls();
}

void MainWindow::onRemoveSelected() {
    if (busy_) return;
    QList<int> rows;
    for (const QModelIndex& idx : table_->selectionModel()->selectedRows()) rows.append(idx.row());
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    if (!rows.isEmpty()) player_->stop();
    for (int row : rows) {
        table_->removeRow(row);
        entries_.erase(entries_.begin() + row);
    }
    refreshView();
    refreshControls();
}

void MainWindow::onChooseOutputFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose output folder"), outputFolderEdit_->text());
    if (!dir.isEmpty()) outputFolderEdit_->setText(dir);
}

MainWindow::Entry* MainWindow::entryById(int id) {
    for (Entry& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

int MainWindow::rowOf(int id) const {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].id == id) return i;
    }
    return -1;
}

MainWindow::Entry* MainWindow::selectedEntry() {
    const auto rows = table_->selectionModel()->selectedRows();
    if (rows.size() != 1) return nullptr; // preview works on exactly one file
    const int row = rows.first().row();
    return row >= 0 && row < static_cast<int>(entries_.size()) ? &entries_[row] : nullptr;
}

uv::PipelineSettings MainWindow::currentSettings() const {
    uv::PipelineSettings s;
    s.targetRate = static_cast<double>(rateCombo_->currentData().toInt());
    s.outputFormat = static_cast<uv::OutputFormat>(formatCombo_->currentData().toInt());
    s.mode = static_cast<uv::PipelineSettings::Mode>(modeCombo_->currentData().toInt());
    s.hfStrength = hfSlider_->value() / 100.0;
    s.ultrasonic = ultrasonicCheck_->isChecked();
    return s;
}

bool MainWindow::previewValid(const Entry& e) const {
    return e.processedCurve.has_value() && sameSettings(e.appliedWith, currentSettings());
}

QString MainWindow::outputPathFor(const QString& inputPath) const {
    const QFileInfo info(inputPath);
    const QString folder = outputFolderEdit_->text().isEmpty() ? info.absolutePath()
                                                               : outputFolderEdit_->text();
    return QDir(folder).filePath(info.completeBaseName() + QStringLiteral("_uv.wav"));
}

void MainWindow::setRowStatus(int id, const QString& text, const QString& tooltip) {
    const int row = rowOf(id);
    if (row < 0) return;
    if (auto* item = table_->item(row, ColStatus)) {
        item->setText(text);
        item->setToolTip(tooltip);
    }
}

void MainWindow::setRowProgress(int id, int percent) {
    const int row = rowOf(id);
    if (row < 0) return;
    if (auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(row, ColProgress))) {
        bar->setValue(percent);
    }
}

// ---- Selection and loading -------------------------------------------------

void MainWindow::onSelectionChanged() {
    player_->stop();
    Entry* e = selectedEntry();
    releaseBuffersExcept(e ? e->id : 0);
    if (e && !e->source && !e->loading) {
        // A file previewed earlier gets its preview back without a second click.
        e->applyAfterLoad = previewValid(*e);
        startLoad(*e);
    }
    refreshView();
    refreshControls();
}

void MainWindow::releaseBuffersExcept(int keepId) {
    for (Entry& e : entries_) {
        if (e.id == keepId || e.id == applyingId_) continue;
        e.source.reset();
        e.processed.reset();
    }
}

void MainWindow::startLoad(Entry& e) {
    e.loading = true;
    const int id = e.id;
    const int token = ++e.loadToken;
    setRowStatus(id, QStringLiteral("Loading"));

    const std::string path = e.path.toStdString();
    auto* watcher = new QFutureWatcher<LoadResult>(this);
    connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher, id, token] {
        watcher->deleteLater();
        LoadResult r = watcher->result();
        Entry* e = entryById(id);
        if (!e || e->loadToken != token) return;
        e->loading = false;
        if (!r.buffer) {
            setRowStatus(id, QStringLiteral("Error"), r.error);
            hintLabel_->setText(QStringLiteral("Could not load: ") + r.error);
            refreshView();
            refreshControls();
            return;
        }
        e->info = r.info;
        e->sourceCurve = std::move(r.curve);
        e->sourceBitDepth = r.buffer->sourceBitDepth;
        e->channels = r.buffer->channelCount();
        e->seconds = r.buffer->sampleRate > 0 ? r.buffer->frames() / r.buffer->sampleRate : 0.0;

        // Keep the buffer only if the user is still looking at this file.
        const Entry* sel = selectedEntry();
        if (sel && sel->id == id) e->source = std::move(r.buffer);

        setRowStatus(id, previewValid(*e) ? QStringLiteral("Previewed") : QStringLiteral("Loaded"));
        if (e->applyAfterLoad && e->source && !busy_) {
            e->applyAfterLoad = false;
            startApply(*e);
        }
        refreshView();
        refreshControls();
    });

    watcher->setFuture(QtConcurrent::run([path] {
        LoadResult r;
        std::string err;
        auto buf = uv::loadAudio(path, &err);
        if (!buf) {
            r.error = QString::fromStdString(err.empty() ? std::string("unreadable file") : err);
            return r;
        }
        r.info = uv::analyzeSpectrum(*buf);
        r.curve = displayCurve(*buf);
        r.buffer = std::make_shared<const uv::AudioBuffer>(std::move(*buf));
        return r;
    }));
}

// ---- Apply (in-memory preview) ---------------------------------------------

void MainWindow::onApply() {
    Entry* e = selectedEntry();
    if (!e || busy_) return;
    if (!e->source) {
        // Still loading: apply as soon as it arrives.
        e->applyAfterLoad = true;
        if (!e->loading) startLoad(*e);
        return;
    }
    startApply(*e);
}

void MainWindow::startApply(Entry& e) {
    player_->stop();
    const int id = e.id;
    const int token = ++e.applyToken;
    const uv::PipelineSettings settings = currentSettings();
    const auto source = e.source;

    cancelFlag_.store(false, std::memory_order_relaxed);
    applyingId_ = id;
    setBusy(true);
    setRowStatus(id, QStringLiteral("Applying"));
    setRowProgress(id, 0);
    hintLabel_->clear();

    auto* watcher = new QFutureWatcher<ApplyResult>(this);
    connect(watcher, &QFutureWatcher<ApplyResult>::finished, this,
            [this, watcher, id, token, settings] {
        watcher->deleteLater();
        ApplyResult r = watcher->result();
        applyingId_ = 0;
        setBusy(false);

        Entry* e = entryById(id);
        if (!e || e->applyToken != token) {
            refreshControls();
            return;
        }
        if (!r.buffer) {
            setRowStatus(id, QStringLiteral("Cancelled"));
            setRowProgress(id, 0);
        } else {
            e->appliedWith = settings;
            e->processedCurve = std::move(r.curve);
            const Entry* sel = selectedEntry();
            if (sel && sel->id == id) e->processed = std::move(r.buffer);
            setRowStatus(id, QStringLiteral("Previewed"));
            setRowProgress(id, 100);
        }
        refreshView();
        refreshControls();
    });

    // Progress is posted back by queued invocation; the window outlives every
    // job because its destructor drains the pool.
    auto post = [this, id](int pct) {
        QMetaObject::invokeMethod(this, [this, id, pct] { setRowProgress(id, pct); },
                                  Qt::QueuedConnection);
    };
    const std::atomic<bool>* cancel = &cancelFlag_;

    watcher->setFuture(QtConcurrent::run([source, settings, post, cancel] {
        ApplyResult r;
        UiProgress sink(post, cancel);
        uv::AudioBuffer out = uv::processBuffer(*source, settings, &sink);
        if (out.empty()) return r;
        r.curve = displayCurve(out);
        r.buffer = std::make_shared<const uv::AudioBuffer>(std::move(out));
        return r;
    }));
}

void MainWindow::onSettingsChanged() {
    // A preview only counts while it matches the settings that would be saved.
    Entry* e = selectedEntry();
    if (e && e->processed && !previewValid(*e) && player_->isPlaying()) player_->stop();
    for (const Entry& x : entries_) {
        if (x.processedCurve) {
            setRowStatus(x.id, previewValid(x) ? QStringLiteral("Previewed") : QStringLiteral("Loaded"));
        }
    }
    refreshView();
    refreshControls();
}

// ---- Save ------------------------------------------------------------------

void MainWindow::onSave() {
    Entry* e = selectedEntry();
    if (!e || busy_) return;

    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save processed file"), outputPathFor(e->path),
        QStringLiteral("WAV audio (*.wav)"));
    if (target.isEmpty()) return;

    player_->stop();
    const int id = e->id;
    const uv::PipelineSettings settings = currentSettings();
    const std::string inPath = e->path.toStdString();
    const std::string outPath = target.toStdString();
    // Reuse the auditioned buffer when it is exactly what would be produced.
    const auto ready = previewValid(*e) ? e->processed : nullptr;

    cancelFlag_.store(false, std::memory_order_relaxed);
    setBusy(true);
    setRowStatus(id, QStringLiteral("Saving"));

    auto post = [this, id](int pct) {
        QMetaObject::invokeMethod(this, [this, id, pct] { setRowProgress(id, pct); },
                                  Qt::QueuedConnection);
    };
    const std::atomic<bool>* cancel = &cancelFlag_;

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, id, target] {
        watcher->deleteLater();
        const QString err = watcher->result();
        setBusy(false);
        if (err.isEmpty()) {
            setRowStatus(id, QStringLiteral("Saved"), target);
            setRowProgress(id, 100);
            hintLabel_->setText(QStringLiteral("Saved ") + QDir::toNativeSeparators(target));
        } else {
            setRowStatus(id, QStringLiteral("Error"), err);
            hintLabel_->setText(QStringLiteral("Save failed: ") + err);
        }
        refreshControls();
    });

    watcher->setFuture(QtConcurrent::run([ready, inPath, outPath, settings, post, cancel]() -> QString {
        std::string err;
        bool ok = false;
        if (ready) {
            ok = uv::saveAudio(outPath, *ready, settings.outputFormat, &err);
        } else {
            UiProgress sink(post, cancel);
            ok = uv::runPipeline(inPath, outPath, settings, &sink, &err);
        }
        if (ok) return QString();
        return QString::fromStdString(err.empty() ? std::string("save failed") : err);
    }));
}

void MainWindow::onSaveAll() {
    if (busy_ || entries_.empty()) return;

    QStringList existing;
    for (const Entry& e : entries_) {
        const QString out = outputPathFor(e.path);
        if (QFileInfo::exists(out)) existing << QFileInfo(out).fileName();
    }
    if (!existing.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Overwrite files?"),
            QStringLiteral("%1 output file(s) already exist and will be replaced:\n\n%2")
                .arg(existing.size())
                .arg(existing.mid(0, 10).join(QLatin1Char('\n')) +
                     (existing.size() > 10 ? QStringLiteral("\n...") : QString())));
        if (answer != QMessageBox::Yes) return;
    }

    player_->stop();
    cancelFlag_.store(false, std::memory_order_relaxed);
    setBusy(true);
    batchPending_ = static_cast<int>(entries_.size());
    const uv::PipelineSettings settings = currentSettings();

    for (int row = 0; row < static_cast<int>(entries_.size()); ++row) {
        setRowStatus(entries_[row].id, QStringLiteral("Queued for save"));
        setRowProgress(entries_[row].id, 0);
        auto* worker = new ProcessWorker(row, entries_[row].path, outputPathFor(entries_[row].path),
                                         settings, &cancelFlag_);
        connect(worker, &ProcessWorker::progress, this, &MainWindow::onBatchProgress, Qt::QueuedConnection);
        connect(worker, &ProcessWorker::finished, this, &MainWindow::onBatchFinished, Qt::QueuedConnection);
        QThreadPool::globalInstance()->start(worker);
    }
}

void MainWindow::onBatchProgress(int row, int percent) {
    if (row < 0 || row >= static_cast<int>(entries_.size())) return;
    const int id = entries_[row].id;
    setRowStatus(id, QStringLiteral("Saving"));
    setRowProgress(id, percent);
}

void MainWindow::onBatchFinished(int row, bool ok, QString errorText) {
    if (row >= 0 && row < static_cast<int>(entries_.size())) {
        const int id = entries_[row].id;
        if (ok) {
            setRowStatus(id, QStringLiteral("Saved"), outputPathFor(entries_[row].path));
            setRowProgress(id, 100);
        } else {
            setRowStatus(id, errorText == QLatin1String("Cancelled") ? QStringLiteral("Cancelled")
                                                                      : QStringLiteral("Error"),
                         errorText);
        }
    }
    if (--batchPending_ <= 0) {
        batchPending_ = 0;
        setBusy(false);
        hintLabel_->setText(QStringLiteral("Save all finished."));
        refreshControls();
    }
}

void MainWindow::onCancel() {
    if (!busy_) return;
    cancelFlag_.store(true, std::memory_order_relaxed);
    // Workers not yet started never run; running ones observe the flag.
    QThreadPool::globalInstance()->clear();
    cancelButton_->setEnabled(false);
}

// ---- Playback --------------------------------------------------------------

void MainWindow::onPlayOriginal() {
    Entry* e = selectedEntry();
    if (e && e->source) {
        hintLabel_->setText(QStringLiteral("Playing original"));
        player_->play(*e->source);
    }
}

void MainWindow::onPlayProcessed() {
    Entry* e = selectedEntry();
    if (e && e->processed && previewValid(*e)) {
        hintLabel_->setText(QStringLiteral("Playing processed"));
        player_->play(*e->processed);
    }
}

void MainWindow::onStopPlayback() { player_->stop(); }

void MainWindow::onPlayerStopped() {
    if (hintLabel_->text().startsWith(QLatin1String("Playing"))) hintLabel_->clear();
    refreshControls();
}

// ---- View and control state ------------------------------------------------

void MainWindow::refreshView() {
    Entry* e = selectedEntry();
    if (!e) {
        plot_->clearSource();
        plot_->clearProcessed();
        plot_->setMarkers({});
        infoLabel_->setText(entries_.empty() ? QStringLiteral("Add or drop audio files to begin.")
                                             : QStringLiteral("Select one file to preview it."));
        return;
    }

    if (e->sourceCurve) plot_->setSource(*e->sourceCurve);
    else plot_->clearSource();

    if (previewValid(*e)) plot_->setProcessed(*e->processedCurve);
    else plot_->clearProcessed();

    std::vector<SpectrumPlot::Marker> markers;
    QString info;
    if (e->info && e->sourceCurve) {
        const double nyq = e->sourceCurve->sampleRate * 0.5;
        if (e->info->cutoffHz > 0 && e->info->cutoffHz < nyq * 0.99) {
            markers.push_back({e->info->cutoffHz, QStringLiteral("cutoff ") + kHz(e->info->cutoffHz)});
        }
        markers.push_back({nyq, QStringLiteral("source Nyquist")});

        info = QStringLiteral("%1  |  %2, %3-bit, %4 ch, %5  |  content to %6%7")
                   .arg(QFileInfo(e->path).fileName())
                   .arg(kHz(e->sourceCurve->sampleRate))
                   .arg(e->sourceBitDepth)
                   .arg(e->channels)
                   .arg(duration(e->seconds))
                   .arg(kHz(e->info->cutoffHz))
                   .arg(e->info->rolledOff ? QStringLiteral(" (rolled off: Auto will reconstruct)")
                                           : QStringLiteral(" (full band: Auto stays transparent)"));
        if (previewValid(*e)) {
            info += QStringLiteral("  |  processed: %1").arg(kHz(e->appliedWith.targetRate));
        }
    } else if (e->loading) {
        info = QStringLiteral("Loading %1...").arg(QFileInfo(e->path).fileName());
    }
    plot_->setMarkers(std::move(markers));
    infoLabel_->setText(info);

    if (e->processedCurve && !previewValid(*e) && !busy_) {
        hintLabel_->setText(QStringLiteral("Settings changed: press Apply to preview."));
    } else if (hintLabel_->text().startsWith(QLatin1String("Settings changed"))) {
        hintLabel_->clear();
    }
}

void MainWindow::refreshControls() {
    Entry* e = selectedEntry();
    const bool haveSource = e && e->source;
    const bool havePreview = e && e->processed && previewValid(*e);
    const bool playing = player_->isPlaying();

    addButton_->setEnabled(!busy_);
    removeButton_->setEnabled(!busy_ && !table_->selectionModel()->selectedRows().isEmpty());
    applyButton_->setEnabled(!busy_ && e != nullptr && !e->loading);
    saveButton_->setEnabled(!busy_ && e != nullptr);
    saveAllButton_->setEnabled(!busy_ && !entries_.empty());
    cancelButton_->setEnabled(busy_);

    playOriginalButton_->setEnabled(haveSource);
    playProcessedButton_->setEnabled(havePreview);
    stopButton_->setEnabled(playing);

    for (QWidget* w : {static_cast<QWidget*>(rateCombo_), static_cast<QWidget*>(formatCombo_),
                       static_cast<QWidget*>(modeCombo_), static_cast<QWidget*>(hfSlider_),
                       static_cast<QWidget*>(ultrasonicCheck_)}) {
        w->setEnabled(!busy_);
    }
}

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    refreshControls();
}

// ---- Drag and drop, close --------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (busy_ || !event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isSupportedAudio(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (busy_ || !event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isSupportedAudio(url.toLocalFile())) addFile(url.toLocalFile());
    }
    event->acceptProposedAction();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    player_->stop();
    cancelFlag_.store(true, std::memory_order_relaxed);
    QThreadPool::globalInstance()->clear();
    QThreadPool::globalInstance()->waitForDone();
    QMainWindow::closeEvent(event);
}

} // namespace uv::app
