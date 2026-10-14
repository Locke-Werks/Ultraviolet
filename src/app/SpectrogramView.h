#pragma once

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

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
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
