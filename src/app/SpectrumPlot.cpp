#include "SpectrumPlot.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <climits>
#include <cmath>

namespace uv::app {

namespace {

constexpr double kMinHz = 20.0;
constexpr double kDefaultMaxHz = 24000.0;
constexpr double kDbMin = -140.0;
constexpr double kDbMax = 6.0;

const QColor kBg(0x0d, 0x0b, 0x14);
const QColor kPlotBg(0x12, 0x0f, 0x1c);
const QColor kGridMajor(255, 255, 255, 34);
const QColor kGridMinor(255, 255, 255, 12);
const QColor kGridFocus(178, 107, 255, 90);
const QColor kAxisText(0xa8, 0xa4, 0xb8);
const QColor kSourceColor(0x8a, 0x90, 0xa6);
const QColor kProcessedColor(0xb2, 0x6b, 0xff);
const QColor kMarkerColor(0xff, 0xb3, 0x47);

QString freqLabel(double hz) {
    if (hz >= 1000.0) {
        const double k = hz / 1000.0;
        if (std::abs(k - std::round(k)) < 1e-6) return QString::number(int(std::round(k))) + "k";
        return QString::number(k, 'f', 1) + "k";
    }
    return QString::number(int(std::round(hz)));
}

double clampDb(double db) { return std::clamp(db, kDbMin, kDbMax); }

double dbToY(double db, const QRectF& r) {
    return r.top() + (kDbMax - clampDb(db)) / (kDbMax - kDbMin) * r.height();
}

// Nearest bin by binary search; freqHz is ascending.
std::optional<float> valueAt(const uv::SpectrumCurve& c, double hz) {
    if (c.freqHz.empty() || hz > c.sampleRate * 0.5) return std::nullopt;
    auto it = std::lower_bound(c.freqHz.begin(), c.freqHz.end(), float(hz));
    size_t i = size_t(it - c.freqHz.begin());
    if (i >= c.freqHz.size()) i = c.freqHz.size() - 1;
    if (i > 0 && std::abs(c.freqHz[i - 1] - hz) < std::abs(c.freqHz[i] - hz)) --i;
    return c.magDb[i];
}

} // namespace

SpectrumPlot::SpectrumPlot(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SpectrumPlot::setSource(const uv::SpectrumCurve& curve) { source_ = curve; update(); }
void SpectrumPlot::clearSource() { source_.reset(); update(); }
void SpectrumPlot::setProcessed(const uv::SpectrumCurve& curve) { processed_ = curve; update(); }
void SpectrumPlot::clearProcessed() { processed_.reset(); update(); }
void SpectrumPlot::setMarkers(std::vector<Marker> markers) { markers_ = std::move(markers); update(); }

QSize SpectrumPlot::minimumSizeHint() const { return {520, 260}; }

QRectF SpectrumPlot::plotRect() const {
    return QRectF(rect()).adjusted(48, 14, -14, -28);
}

double SpectrumPlot::maxFreq() const {
    double top = 0.0;
    if (source_) top = std::max(top, source_->sampleRate * 0.5);
    if (processed_) top = std::max(top, processed_->sampleRate * 0.5);
    return top > kMinHz * 2 ? top : kDefaultMaxHz;
}

double SpectrumPlot::freqToX(double hz, const QRectF& r) const {
    const double lo = std::log10(kMinHz), hi = std::log10(maxFreq());
    return r.left() + (std::log10(std::max(hz, 1.0)) - lo) / (hi - lo) * r.width();
}

double SpectrumPlot::xToFreq(double x, const QRectF& r) const {
    const double lo = std::log10(kMinHz), hi = std::log10(maxFreq());
    return std::pow(10.0, lo + (x - r.left()) / r.width() * (hi - lo));
}

void SpectrumPlot::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (plotRect().contains(pos)) hover_ = pos;
    else hover_.reset();
    update();
}

void SpectrumPlot::leaveEvent(QEvent*) {
    hover_.reset();
    update();
}

void SpectrumPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), kBg);

    const QRectF r = plotRect();
    if (r.width() < 10 || r.height() < 10) return;
    p.fillRect(r, kPlotBg);

    const double fMax = maxFreq();
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
    p.setFont(small);
    const QFontMetricsF fm(small);

    // Region of interest wash: the band the product reconstructs.
    {
        const double x0 = freqToX(10000.0, r);
        const double x1 = freqToX(std::min(20000.0, fMax), r);
        if (x1 > x0) p.fillRect(QRectF(x0, r.top(), x1 - x0, r.height()), QColor(178, 107, 255, 16));
    }

    // Minor log grid.
    p.setPen(QPen(kGridMinor, 1.0));
    for (double decade = 10.0; decade <= fMax; decade *= 10.0) {
        for (int m = 1; m <= 9; ++m) {
            const double f = decade * m;
            if (f < kMinHz || f > fMax) continue;
            const double x = freqToX(f, r);
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
    }

    // Major marked frequencies.
    static const double kMajor[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000};
    for (double f : kMajor) {
        if (f < kMinHz || f > fMax * 1.0001) continue;
        const double x = freqToX(f, r);
        const bool focus = (f == 10000.0 || f == 20000.0);
        p.setPen(QPen(focus ? kGridFocus : kGridMajor, focus ? 1.3 : 1.0));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        const QString t = freqLabel(f);
        const double w = fm.horizontalAdvance(t);
        p.setPen(focus ? QColor(0xd6, 0xb4, 0xff) : kAxisText);
        p.drawText(QPointF(std::clamp(x - w / 2, r.left() - 4, r.right() - w + 4), r.bottom() + fm.ascent() + 5), t);
    }

    // dB grid.
    for (int db = 0; db >= int(kDbMin); db -= 20) {
        const double y = dbToY(db, r);
        p.setPen(QPen(db == 0 ? QColor(255, 255, 255, 55) : kGridMajor, 1.0));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        const QString t = QString::number(db);
        p.setPen(kAxisText);
        p.drawText(QPointF(r.left() - 6 - fm.horizontalAdvance(t), y + fm.ascent() / 2 - 1), t);
    }

    p.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    if (!source_ && !processed_) {
        p.setPen(QColor(0x8a, 0x86, 0x9a));
        QFont big = font();
        big.setPointSizeF(big.pointSizeF() + 2);
        p.setFont(big);
        p.drawText(r, Qt::AlignCenter, tr("Select a file to see its spectrum"));
        return;
    }

    // Peak-hold per pixel column so a single narrow bin or the HF edge cannot
    // disappear between samples when thousands of bins share a few hundred pixels.
    auto buildPath = [&](const uv::SpectrumCurve& c) {
        QPainterPath path;
        const size_t n = std::min(c.freqHz.size(), c.magDb.size());
        bool started = false;
        int col = INT_MIN;
        double colMax = 0.0, colX = 0.0;
        auto pushPt = [&](double x, double db) {
            const QPointF pt(x, dbToY(db, r));
            if (!started) { path.moveTo(pt); started = true; }
            else path.lineTo(pt);
        };
        for (size_t i = 0; i < n; ++i) {
            const double f = c.freqHz[i];
            if (f < kMinHz || f > fMax) continue;
            const double x = freqToX(f, r);
            const int px = int(std::floor(x));
            const double db = c.magDb[i];
            if (px != col) {
                if (col != INT_MIN) pushPt(colX, colMax);
                col = px;
                colMax = db;
                colX = x;
            } else {
                colMax = std::max(colMax, db);
            }
        }
        if (col != INT_MIN) pushPt(colX, colMax);
        return path;
    };

    p.save();
    p.setClipRect(r);
    if (source_) {
        p.setPen(QPen(kSourceColor, 1.2));
        p.drawPath(buildPath(*source_));
    }
    if (processed_) {
        QPainterPath line = buildPath(*processed_);
        if (!line.isEmpty()) {
            QPainterPath fill = line;
            fill.lineTo(line.currentPosition().x(), r.bottom());
            fill.lineTo(line.elementAt(0).x, r.bottom());
            fill.closeSubpath();
            QLinearGradient g(0, r.top(), 0, r.bottom());
            g.setColorAt(0.0, QColor(178, 107, 255, 70));
            g.setColorAt(1.0, QColor(178, 107, 255, 0));
            p.fillPath(fill, g);
        }
        p.setPen(QPen(kProcessedColor, 1.6));
        p.drawPath(line);
    }
    p.restore();

    for (const auto& m : markers_) {
        if (m.freqHz < kMinHz || m.freqHz > fMax) continue;
        const double x = freqToX(m.freqHz, r);
        p.setPen(QPen(kMarkerColor, 1.0, Qt::DashLine));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }

    QRectF legendBox;
    {
        struct Item { QString name; QColor color; };
        std::vector<Item> items;
        if (source_) items.push_back({tr("Source"), kSourceColor});
        if (processed_) items.push_back({tr("Processed"), kProcessedColor});
        double w = 0;
        for (auto& it : items) w = std::max(w, fm.horizontalAdvance(it.name));
        const double boxW = w + 34, boxH = items.size() * (fm.height() + 4) + 8;
        QRectF box(r.right() - boxW - 8, r.top() + 8, boxW, boxH);
        p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
        p.setBrush(QColor(13, 11, 20, 210));
        p.drawRoundedRect(box, 4, 4);
        double y = box.top() + 6;
        for (auto& it : items) {
            const double cy = y + fm.height() / 2;
            p.setPen(QPen(it.color, 2.0));
            p.drawLine(QPointF(box.left() + 8, cy), QPointF(box.left() + 22, cy));
            p.setPen(QColor(0xe0, 0xdc, 0xec));
            p.drawText(QPointF(box.left() + 28, y + fm.ascent()), it.name);
            y += fm.height() + 4;
        }
        legendBox = box.adjusted(-4, -4, 4, 4);
    }

    // Marker labels go after the legend and step down until clear of it and of
    // each other, since cutoff and Nyquist markers usually sit close together
    // in the same top-right corner the legend occupies.
    {
        std::vector<QRectF> taken{legendBox};
        const double lineH = fm.height() + 3;
        for (const auto& m : markers_) {
            if (m.freqHz < kMinHz || m.freqHz > fMax) continue;
            const double x = freqToX(m.freqHz, r);
            const double w = fm.horizontalAdvance(m.label);
            double tx = x + 4;
            if (tx + w > r.right() - 2) tx = x - 4 - w;
            QRectF lr(tx - 2, r.top() + 4, w + 4, fm.height() + 2);
            for (int guard = 0; guard < 40; ++guard) {
                bool hit = false;
                for (const auto& t : taken) hit = hit || t.intersects(lr);
                if (!hit) break;
                lr.translate(0, lineH);
            }
            taken.push_back(lr);
            p.fillRect(lr, QColor(13, 11, 20, 210));
            p.setPen(kMarkerColor);
            p.drawText(QPointF(lr.left() + 2, lr.top() + 1 + fm.ascent()), m.label);
        }
    }

    // Hover readout.
    if (hover_) {
        const QPointF h = *hover_;
        p.setPen(QPen(QColor(255, 255, 255, 70), 1.0));
        p.drawLine(QPointF(h.x(), r.top()), QPointF(h.x(), r.bottom()));
        p.drawLine(QPointF(r.left(), h.y()), QPointF(r.right(), h.y()));

        const double f = xToFreq(h.x(), r);
        QStringList lines;
        std::vector<QColor> colors;
        lines << (f >= 1000 ? QString::number(f / 1000.0, 'f', 2) + " kHz" : QString::number(f, 'f', 0) + " Hz");
        colors.push_back(QColor(0xe0, 0xdc, 0xec));
        auto add = [&](const std::optional<uv::SpectrumCurve>& c, const QString& name, QColor col) {
            if (!c) return;
            auto v = valueAt(*c, f);
            lines << name + ": " + (v ? QString::number(*v, 'f', 1) + " dB" : QStringLiteral("n/a"));
            colors.push_back(col);
        };
        add(source_, tr("Source"), kSourceColor);
        add(processed_, tr("Processed"), kProcessedColor);

        double w = 0;
        for (auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const double bw = w + 12, bh = lines.size() * fm.height() + 8;
        double bx = h.x() + 12, by = h.y() + 12;
        if (bx + bw > r.right()) bx = h.x() - 12 - bw;
        if (by + bh > r.bottom()) by = h.y() - 12 - bh;
        p.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
        p.setBrush(QColor(13, 11, 20, 230));
        p.drawRoundedRect(QRectF(bx, by, bw, bh), 3, 3);
        for (int i = 0; i < lines.size(); ++i) {
            p.setPen(colors[size_t(i)]);
            p.drawText(QPointF(bx + 6, by + 4 + fm.ascent() + i * fm.height()), lines[i]);
        }
    }
}

} // namespace uv::app
