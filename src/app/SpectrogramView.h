#pragma once

#include "uvcore/Selection.h"
#include "uvcore/SpectralAnalyzer.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <optional>

namespace uv::app {

// Time-frequency view of one buffer. Log-frequency Y axis with the same
// labelled reference lines as SpectrumPlot, time X axis, magnitude as colour.
// Two of these sit side by side (source, processed) sharing one frequency
// range so content added above the source cutoff lines up visually.
class SpectrogramView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrogramView(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setData(const uv::Spectrogram& data);
    void clear();

    // Upper edge of the Y axis. Set both views to the larger Nyquist so their
    // axes match; content above a buffer's own Nyquist is drawn as empty.
    void setDisplayMaxHz(double hz);

    // A horizontal dashed line, e.g. the detected source cutoff.
    void setCutoffMarker(std::optional<double> hz);

    // Linear frequency axis (0 Hz at the bottom) instead of log. Linear gives
    // the high octaves, where the extension lives, most of the height.
    void setLinearFrequency(bool linear);

    // Vertical playhead in seconds; nullopt hides it.
    void setPlayhead(std::optional<double> seconds);

    // RX style selection tool. Time selects a full-height column, Frequency a
    // full-width band, Box a rectangle.
    enum class SelectionMode { Time, Frequency, Box };
    void setSelectionMode(SelectionMode mode);
    SelectionMode selectionMode() const { return selMode_; }

    // Sets the selection without emitting selectionChanged, so two views can
    // mirror each other without a feedback loop.
    void setSelection(const std::optional<uv::Selection>& sel);
    const std::optional<uv::Selection>& selection() const { return selection_; }
    void clearSelection(); // emits selectionChanged(nullopt) if one existed

    QSize minimumSizeHint() const override;

signals:
    // Emitted on user edits only: drag release, single click, Esc.
    void selectionChanged(std::optional<uv::Selection> selection);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    double xToTime(double x, const QRectF& r) const;
    double timeToX(double t, const QRectF& r) const;
    double durationSeconds() const;
    uv::Selection selectionFromDrag(QPointF a, QPointF b) const;

    SelectionMode selMode_ = SelectionMode::Time;
    std::optional<uv::Selection> selection_;
    std::optional<QPointF> dragStart_;
    bool dragging_ = false;

    QRectF plotRect() const;
    double freqToY(double hz, const QRectF& r) const;
    double yToFreq(double y, const QRectF& r) const;
    void rebuildImage();

    std::optional<QPointF> hover_;
    QString title_;
    std::optional<uv::Spectrogram> data_;
    double displayMaxHz_ = 24000.0;
    bool linear_ = false;
    std::optional<double> cutoffHz_;
    std::optional<double> playhead_;
    QImage image_; // rendered colour image, rebuilt when data or size changes
};

} // namespace uv::app
