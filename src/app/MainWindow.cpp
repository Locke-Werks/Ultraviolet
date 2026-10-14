#include "MainWindow.h"

#include "ProcessWorker.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QModelIndex>
#include <QUrl>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
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
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Ultraviolet"));
    setWindowIcon(QIcon(QStringLiteral(":/ultraviolet.png")));
    resize(1000, 640);
    setAcceptDrops(true);
    buildUi();
    setControlsForRunning(false);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);

    // Left: queue table plus its action buttons.
    auto* left = new QVBoxLayout();

    table_ = new QTableWidget(0, ColCount, central);
    table_->setHorizontalHeaderLabels({QStringLiteral("File"), QStringLiteral("Status"),
                                       QStringLiteral("Progress")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ColProgress, QHeaderView::Fixed);
    table_->setColumnWidth(ColProgress, 180);
    table_->verticalHeader()->setVisible(false);
    left->addWidget(table_, 1);

    auto* queueButtons = new QHBoxLayout();
    addButton_ = new QPushButton(QStringLiteral("Add files..."), central);
    removeButton_ = new QPushButton(QStringLiteral("Remove selected"), central);
    queueButtons->addWidget(addButton_);
    queueButtons->addWidget(removeButton_);
    queueButtons->addStretch(1);
    left->addLayout(queueButtons);

    root->addLayout(left, 1);

    // Right: settings panel.
    auto* settings = new QGroupBox(QStringLiteral("Settings"), central);
    auto* form = new QVBoxLayout(settings);

    form->addWidget(new QLabel(QStringLiteral("Target sample rate"), settings));
    rateCombo_ = new QComboBox(settings);
    rateCombo_->addItem(QStringLiteral("88.2 kHz"), 88200);
    rateCombo_->addItem(QStringLiteral("96 kHz"), 96000);
    rateCombo_->addItem(QStringLiteral("176.4 kHz"), 176400);
    rateCombo_->addItem(QStringLiteral("192 kHz"), 192000);
    rateCombo_->setCurrentIndex(3); // 192000 default
    form->addWidget(rateCombo_);

    form->addWidget(new QLabel(QStringLiteral("Output format"), settings));
    formatCombo_ = new QComboBox(settings);
    formatCombo_->addItem(QStringLiteral("32-bit float"),
                          static_cast<int>(uv::OutputFormat::Float32));
    formatCombo_->addItem(QStringLiteral("24-bit PCM"),
                          static_cast<int>(uv::OutputFormat::Pcm24));
    form->addWidget(formatCombo_);

    form->addWidget(new QLabel(QStringLiteral("Mode"), settings));
    modeCombo_ = new QComboBox(settings);
    modeCombo_->addItem(QStringLiteral("Mastering"),
                        static_cast<int>(uv::PipelineSettings::Mode::Mastering));
    modeCombo_->addItem(QStringLiteral("Rescue"),
                        static_cast<int>(uv::PipelineSettings::Mode::Rescue));
    modeCombo_->addItem(QStringLiteral("Auto"),
                        static_cast<int>(uv::PipelineSettings::Mode::Auto));
    modeCombo_->setCurrentIndex(2); // Auto default
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
    connect(hfSlider_, &QSlider::valueChanged, this, [this](int v) {
        hfValueLabel_->setText(QString::number(v));
    });

    ultrasonicCheck_ = new QCheckBox(QStringLiteral("Generate ultrasonic content"), settings);
    ultrasonicCheck_->setChecked(false);
    form->addWidget(ultrasonicCheck_);

    form->addSpacing(8);
    form->addWidget(new QLabel(QStringLiteral("Output folder"), settings));
    auto* outRow = new QHBoxLayout();
    outputFolderEdit_ = new QLineEdit(settings);
    outputFolderEdit_->setPlaceholderText(
        QStringLiteral("Next to each input (_uv suffix)"));
    outputFolderEdit_->setReadOnly(true);
    auto* chooseOut = new QPushButton(QStringLiteral("Choose..."), settings);
    outRow->addWidget(outputFolderEdit_, 1);
    outRow->addWidget(chooseOut);
    form->addLayout(outRow);

    form->addStretch(1);

    processButton_ = new QPushButton(QStringLiteral("Process"), settings);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), settings);
    form->addWidget(processButton_);
    form->addWidget(cancelButton_);

    settings->setFixedWidth(320);
    root->addWidget(settings);

    setCentralWidget(central);

    connect(addButton_, &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(removeButton_, &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
    connect(chooseOut, &QPushButton::clicked, this, &MainWindow::onChooseOutputFolder);
    connect(processButton_, &QPushButton::clicked, this, &MainWindow::onProcess);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::onCancel);
}

void MainWindow::onAddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add audio files"), QString(),
        QString::fromLatin1(kSupportedFilter));
    for (const QString& f : files) {
        addFile(f);
    }
}

void MainWindow::addFile(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (inputPaths_.contains(absolute)) {
        return; // no duplicates in the queue
    }
    inputPaths_.append(absolute);

    const int row = table_->rowCount();
    table_->insertRow(row);

    auto* nameItem = new QTableWidgetItem(QFileInfo(absolute).fileName());
    nameItem->setToolTip(absolute);
    table_->setItem(row, ColName, nameItem);

    table_->setItem(row, ColStatus, new QTableWidgetItem(QStringLiteral("Queued")));

    auto* bar = new QProgressBar(table_);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(true);
    table_->setCellWidget(row, ColProgress, bar);
}

void MainWindow::onRemoveSelected() {
    if (running_) {
        return;
    }
    // Collect selected rows, remove high-to-low so indices stay valid.
    QList<int> rows;
    for (const QModelIndex& idx : table_->selectionModel()->selectedRows()) {
        rows.append(idx.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        table_->removeRow(row);
        inputPaths_.removeAt(row);
    }
}

void MainWindow::onChooseOutputFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose output folder"), outputFolderEdit_->text());
    if (!dir.isEmpty()) {
        outputFolderEdit_->setText(dir);
    }
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

QString MainWindow::outputPathFor(const QString& inputPath) const {
    const QFileInfo info(inputPath);
    const QString folder = outputFolderEdit_->text().isEmpty()
                               ? info.absolutePath()
                               : outputFolderEdit_->text();
    // Always write .wav; the "_uv" suffix keeps outputs beside same-named inputs.
    return QDir(folder).filePath(info.completeBaseName() + QStringLiteral("_uv.wav"));
}

void MainWindow::onProcess() {
    if (running_ || inputPaths_.isEmpty()) {
        return;
    }
    running_ = true;
    cancelFlag_.store(false, std::memory_order_relaxed);
    pendingCount_ = inputPaths_.size();
    setControlsForRunning(true);

    const uv::PipelineSettings settings = currentSettings();

    for (int row = 0; row < inputPaths_.size(); ++row) {
        if (auto* item = table_->item(row, ColStatus)) {
            item->setText(QStringLiteral("Queued"));
            item->setToolTip(QString());
        }
        if (auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(row, ColProgress))) {
            bar->setValue(0);
        }

        auto* worker = new ProcessWorker(row, inputPaths_[row], outputPathFor(inputPaths_[row]),
                                         settings, &cancelFlag_);
        // Queued connections marshal updates onto the UI thread.
        connect(worker, &ProcessWorker::progress, this, &MainWindow::onWorkerProgress,
                Qt::QueuedConnection);
        connect(worker, &ProcessWorker::finished, this, &MainWindow::onWorkerFinished,
                Qt::QueuedConnection);
        QThreadPool::globalInstance()->start(worker);
    }
}

void MainWindow::onCancel() {
    if (!running_) {
        return;
    }
    cancelFlag_.store(true, std::memory_order_relaxed);
    // Drop workers that have not started yet; in-flight ones observe the flag.
    QThreadPool::globalInstance()->clear();
    cancelButton_->setEnabled(false);
}

void MainWindow::onWorkerProgress(int row, int percent) {
    if (row < 0 || row >= table_->rowCount()) {
        return;
    }
    if (auto* item = table_->item(row, ColStatus)) {
        if (item->text() != QLatin1String("Done") && item->text() != QLatin1String("Error")) {
            item->setText(QStringLiteral("Processing"));
        }
    }
    if (auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(row, ColProgress))) {
        bar->setValue(percent);
    }
}

void MainWindow::onWorkerFinished(int row, bool ok, QString errorText) {
    if (row >= 0 && row < table_->rowCount()) {
        if (auto* item = table_->item(row, ColStatus)) {
            if (ok) {
                item->setText(QStringLiteral("Done"));
                item->setToolTip(QString());
            } else {
                item->setText(QStringLiteral("Error"));
                // Error text surfaces in the status cell tooltip.
                item->setToolTip(errorText);
            }
        }
        if (ok) {
            if (auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(row, ColProgress))) {
                bar->setValue(100);
            }
        }
    }

    if (pendingCount_ > 0) {
        --pendingCount_;
    }
    maybeFinishBatch();
}

void MainWindow::maybeFinishBatch() {
    if (pendingCount_ > 0) {
        return;
    }
    running_ = false;
    cancelFlag_.store(false, std::memory_order_relaxed);
    setControlsForRunning(false);
}

void MainWindow::setControlsForRunning(bool running) {
    addButton_->setEnabled(!running);
    removeButton_->setEnabled(!running);
    processButton_->setEnabled(!running);
    cancelButton_->setEnabled(running);
    rateCombo_->setEnabled(!running);
    formatCombo_->setEnabled(!running);
    modeCombo_->setEnabled(!running);
    hfSlider_->setEnabled(!running);
    ultrasonicCheck_->setEnabled(!running);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (running_ || !event->mimeData()->hasUrls()) {
        return;
    }
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isSupportedAudio(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (running_ || !event->mimeData()->hasUrls()) {
        return;
    }
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        if (isSupportedAudio(path)) {
            addFile(path);
        }
    }
    event->acceptProposedAction();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (running_) {
        // Stop in-flight work and wait so no worker outlives the window.
        cancelFlag_.store(true, std::memory_order_relaxed);
        QThreadPool::globalInstance()->clear();
        QThreadPool::globalInstance()->waitForDone();
    }
    QMainWindow::closeEvent(event);
}

} // namespace uv::app
