#pragma once

#include "uvcore/SpectralAnalyzer.h"

#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

namespace uv::app {

// Magnitude spectrum display. Log-frequency X axis with a labelled grid of
// reference frequencies, dB Y axis. Shows the source curve and, once applied,
// the processed curve overlaid so the added high-frequency content is visible.
class SpectrumPlot : public QWidget {
    Q_OBJECT
public:
    struct Marker {
        double freqHz;
        QString label;
    };

    explicit SpectrumPlot(QWidget* parent = nullptr);

    void setSource(const uv::SpectrumCurve& curve);
    void clearSource();
    void setProcessed(const uv::SpectrumCurve& curve);
    void clearProcessed();

    // Extra labelled vertical lines, e.g. detected cutoff or source Nyquist.
    void setMarkers(std::vector<Marker> markers);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF plotRect() const;
    double maxFreq() const;
    double freqToX(double hz, const QRectF& r) const;
    double xToFreq(double x, const QRectF& r) const;

    std::optional<QPointF> hover_;
    std::optional<uv::SpectrumCurve> source_;
    std::optional<uv::SpectrumCurve> processed_;
    std::vector<Marker> markers_;
};

} // namespace uv::app
