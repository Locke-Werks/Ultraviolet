#include "ProcessWorker.h"

namespace uv::app {

ProcessWorker::ProcessWorker(int row, QString inputPath, QString outputPath,
                             uv::PipelineSettings settings,
                             const std::atomic<bool>* cancelFlag)
    : row_(row),
      inputPath_(std::move(inputPath)),
      outputPath_(std::move(outputPath)),
      settings_(settings),
      cancelFlag_(cancelFlag) {
    // The pool owns and deletes the runnable after run() returns.
    setAutoDelete(true);
}

void ProcessWorker::run() {
    if (cancelled()) {
        emit finished(row_, false, QStringLiteral("Cancelled"));
        return;
    }

    emit progress(row_, 0);

    std::string err;
    const bool ok = uv::runPipeline(inputPath_.toStdString(), outputPath_.toStdString(),
                                    settings_, this, &err);

    if (cancelled()) {
        emit finished(row_, false, QStringLiteral("Cancelled"));
        return;
    }

    if (ok) {
        emit progress(row_, 100);
        emit finished(row_, true, QString());
    } else {
        emit finished(row_, false,
                      err.empty() ? QStringLiteral("Processing failed")
                                  : QString::fromStdString(err));
    }
}

void ProcessWorker::onProgress(double fraction01) {
    int pct = static_cast<int>(fraction01 * 100.0 + 0.5);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    emit progress(row_, pct);
}

bool ProcessWorker::cancelled() const {
    return cancelFlag_ != nullptr && cancelFlag_->load(std::memory_order_relaxed);
}

} // namespace uv::app
