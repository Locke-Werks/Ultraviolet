#pragma once

#include "uvcore/Types.h"

#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

namespace uv::app {

// Sample-level comparison of the source and the processed buffer on one time
// axis. The source is drawn as its literal sample-and-hold staircase at its own
// rate, with its quantization grid when zoomed in far enough; the processed
// buffer is drawn as a line at the target rate over it, so the difference
// between the steps and the reconstruction is visible directly.
//
// Wheel zooms time around the cursor, Ctrl+wheel zooms amplitude, drag pans,
// double-click resets, C cycles the channel, F toggles following the playhead.
class WaveformView : public QWidget {
    Q_OBJECT
public:
    explicit WaveformView(QWidget* parent = nullptr);

    // Either may be null. sourceBitDepth draws the LSB grid; 0 hides it.
    void setBuffers(std::shared_ptr<const uv::AudioBuffer> source,
                    std::shared_ptr<const uv::AudioBuffer> processed, int sourceBitDepth);
    void setPlayhead(std::optional<double> seconds);
    // Zoom to a time span, e.g. a selection drawn on the spectrogram.
    void showRange(double t0, double t1);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // Min/max per block, so a zoomed-out paint reads thousands of values
    // instead of tens of millions of samples.
    struct Peaks {
        int block = 0;
        std::vector<float> lo, hi;
    };
    struct Track {
        std::shared_ptr<const uv::AudioBuffer> buf;
        std::vector<std::vector<Peaks>> levels; // [channel][level]
    };
    static Track makeTrack(std::shared_ptr<const uv::AudioBuffer> buf);

    QRectF plotRect() const;
    double duration() const;
    double xToTime(double x, const QRectF& r) const;
    double timeToX(double t, const QRectF& r) const;
    double ampToY(double a, const QRectF& r) const;
    void clampView();
    void resetView();
    void drawEnvelope(QPainter& p, const QRectF& r, const Track& t, const QColor& c) const;
    void drawSamples(QPainter& p, const QRectF& r, const Track& t, const QColor& c, bool steps) const;

    Track source_, processed_;
    int bitDepth_ = 0;
    int channel_ = 0;
    double t0_ = 0.0;        // left edge, seconds
    double span_ = 0.01;     // visible seconds
    double ampZoom_ = 1.0;   // 1 = full scale fills the height
    double ampCentre_ = 0.0;
    bool follow_ = true;
    std::optional<double> playhead_;
    std::optional<QPointF> hover_;
    std::optional<QPointF> dragFrom_;
    double dragT0_ = 0.0, dragAmp_ = 0.0;
};

} // namespace uv::app
