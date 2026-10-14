#pragma once

#include "uvcore/Pipeline.h"

#include <QObject>
#include <QRunnable>
#include <QString>

#include <atomic>
#include <string>

namespace uv::app {

// One queued file. Runs on a QThreadPool worker thread, forwarding progress and
// completion back to the UI thread through queued signal/slot connections. The
// object is also the ProgressSink the core pipeline writes to: onProgress emits,
// cancelled() reads a flag the UI thread owns. Never touches widgets directly.
class ProcessWorker : public QObject, public QRunnable, public uv::ProgressSink {
    Q_OBJECT
public:
    ProcessWorker(int row, QString inputPath, QString outputPath,
                  uv::PipelineSettings settings, const std::atomic<bool>* cancelFlag);

    void run() override;

    // ProgressSink. Called from the worker thread by the core pipeline.
    void onProgress(double fraction01) override;
    bool cancelled() const override;

signals:
    void progress(int row, int percent);
    void finished(int row, bool ok, QString errorText);

private:
    int row_;
    QString inputPath_;
    QString outputPath_;
    uv::PipelineSettings settings_;
    const std::atomic<bool>* cancelFlag_;
};

} // namespace uv::app
