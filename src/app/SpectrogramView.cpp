#include "SpectrogramView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace uv::app {

namespace {

constexpr double kMinHz = 20.0;
constexpr double kDbFloor = -130.0;
constexpr double kDbCeil = 0.0;

const QColor kBg(0x0d, 0x0b, 0x14);
const QColor kGridMajor(255, 255, 255, 30);
const QColor kGridFocus(178, 107, 255, 110);
const QColor kAxisText(0xa8, 0xa4, 0xb8);
const QColor kFocusText(0xd6, 0xb4, 0xff);
const QColor kTitleText(0xe0, 0xdc, 0xec);
const QColor kMarkerColor(0xff, 0xb3, 0x47);
const QRgb kEmpty = qRgb(0x08, 0x07, 0x0c);
const QRgb kEmptyHatch = qRgb(0x11, 0x0e, 0x18);

QString freqLabel(double hz) {
    if (hz >= 1000.0) {
        const double k = hz / 1000.0;
        if (std::abs(k - std::round(k)) < 1e-6) return QString::number(int(std::round(k))) + "k";
        return QString::number(k, 'f', 1) + "k";
    }
    return QString::number(int(std::round(hz)));
}

QString timeLabel(double s, bool tenths) {
    if (s < 0) s = 0;
    const int m = int(s / 60.0);
    const double rest = s - m * 60.0;
    if (tenths) return QString("%1:%2").arg(m).arg(rest, 4, 'f', 1, QChar('0'));
    return QString("%1:%2").arg(m).arg(int(rest), 2, 10, QChar('0'));
}

// Built once: interpolating sRGB stops is good enough here because the stops
// themselves were picked to rise monotonically in lightness.
const std::array<QRgb, 256>& palette() {
    static const std::array<QRgb, 256> lut = [] {
        struct Stop { double t; int r, g, b; };
        static const Stop stops[] = {
            {0.00, 0x00, 0x00, 0x00},
            {0.18, 0x1a, 0x10, 0x4a},
            {0.40, 0x7c, 0x3a, 0xed},
            {0.62, 0xd9, 0x46, 0xa8},
            {0.82, 0xff, 0xb3, 0x47},
            {1.00, 0xff, 0xf8, 0xec},
        };
        std::array<QRgb, 256> out{};
        for (int i = 0; i < 256; ++i) {
            const double t = i / 255.0;
            size_t k = 0;
            while (k + 2 < std::size(stops) && t > stops[k + 1].t) ++k;
            const Stop& a = stops[k];
            const Stop& b = stops[k + 1];
            const double u = std::clamp((t - a.t) / (b.t - a.t), 0.0, 1.0);
            out[size_t(i)] = qRgb(int(a.r + (b.r - a.r) * u), int(a.g + (b.g - a.g) * u),
                                  int(a.b + (b.b - a.b) * u));
        }
        return out;
    }();
    return lut;
}

QRgb colorFor(float db) {
    const double t = (double(db) - kDbFloor) / (kDbCeil - kDbFloor);
    return palette()[size_t(std::clamp(int(t * 255.0 + 0.5), 0, 255))];
}

bool valid(const uv::Spectrogram& s) {
    return s.rows > 0 && s.cols > 0 && s.maxHz > s.minHz && s.minHz > 0 &&
           s.db.size() >= size_t(s.rows) * size_t(s.cols);
}

} // namespace

SpectrogramView::SpectrogramView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Click focus so Esc reaches the view that was just used to select.
    setFocusPolicy(Qt::ClickFocus);
}

void SpectrogramView::setSelectionMode(SelectionMode mode) {
    selMode_ = mode;
    dragStart_.reset();
    dragging_ = false;
    update();
}

void SpectrogramView::setSelection(const std::optional<uv::Selection>& sel) {
    selection_ = sel;
    update();
}

void SpectrogramView::clearSelection() {
    const bool had = selection_.has_value();
    selection_.reset();
    update();
    if (had) emit selectionChanged(std::nullopt);
}

double SpectrogramView::durationSeconds() const {
    return (data_ && valid(*data_)) ? data_->durationSeconds : 0.0;
}

double SpectrogramView::xToTime(double x, const QRectF& r) const {
    return std::clamp((x - r.left()) / r.width(), 0.0, 1.0) * durationSeconds();
}

double SpectrogramView::timeToX(double t, const QRectF& r) const {
    const double dur = durationSeconds();
    return dur > 0 ? r.left() + t / dur * r.width() : r.left();
}

uv::Selection SpectrogramView::selectionFromDrag(QPointF a, QPointF b) const {
    const QRectF r = plotRect();
    uv::Selection s;
    if (selMode_ != SelectionMode::Frequency) {
        s.hasTime = true;
        s.t0 = xToTime(std::min(a.x(), b.x()), r);
        s.t1 = xToTime(std::max(a.x(), b.x()), r);
    }
    if (selMode_ != SelectionMode::Time) {
        s.hasFreq = true;
        // Higher y is lower frequency; clamp to the drawn axis range.
        const double yTop = std::clamp(std::min(a.y(), b.y()), r.top(), r.bottom());
        const double yBot = std::clamp(std::max(a.y(), b.y()), r.top(), r.bottom());
        s.f0 = yToFreq(yBot, r);
        s.f1 = std::min(yToFreq(yTop, r), displayMaxHz_);
    }
    return s;
}

void SpectrogramView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && plotRect().contains(event->position()) && durationSeconds() > 0) {
        dragStart_ = event->position();
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void SpectrogramView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragStart_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasDrag = dragging_;
    dragStart_.reset();
    dragging_ = false;
    if (wasDrag) emit selectionChanged(selection_);
    else clearSelection(); // a click without a drag deselects, as in RX
    event->accept();
}

void SpectrogramView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        dragStart_.reset();
        dragging_ = false;
        clearSelection();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SpectrogramView::setTitle(const QString& title) { title_ = title; update(); }

void SpectrogramView::setData(const uv::Spectrogram& spectrogram) {
    data_ = spectrogram;
    rebuildImage();
    update();
}

void SpectrogramView::clear() {
    data_.reset();
    image_ = QImage();
    update();
}

void SpectrogramView::setDisplayMaxHz(double hz) {
    displayMaxHz_ = hz > kMinHz * 2 ? hz : 24000.0;
    rebuildImage();
    update();
}

void SpectrogramView::setCutoffMarker(std::optional<double> hz) { cutoffHz_ = hz; update(); }
void SpectrogramView::setPlayhead(std::optional<double> seconds) { playhead_ = seconds; update(); }

QSize SpectrogramView::minimumSizeHint() const { return {300, 180}; }

QRectF SpectrogramView::plotRect() const {
    return QRectF(rect()).adjusted(40, 22, -10, -22);
}

double SpectrogramView::freqToY(double hz, const QRectF& r) const {
    if (linear_) return r.bottom() - std::clamp(hz / displayMaxHz_, 0.0, 1.0) * r.height();
    const double lo = std::log10(kMinHz), hi = std::log10(displayMaxHz_);
    return r.bottom() - (std::log10(std::max(hz, 1.0)) - lo) / (hi - lo) * r.height();
}

double SpectrogramView::yToFreq(double y, const QRectF& r) const {
    if (linear_) return std::max(0.0, (r.bottom() - y) / r.height() * displayMaxHz_);
    const double lo = std::log10(kMinHz), hi = std::log10(displayMaxHz_);
    return std::pow(10.0, lo + (r.bottom() - y) / r.height() * (hi - lo));
}

void SpectrogramView::setLinearFrequency(bool linear) {
    if (linear_ == linear) return;
    linear_ = linear;
    rebuildImage();
    update();
}

void SpectrogramView::resizeEvent(QResizeEvent*) { rebuildImage(); }

void SpectrogramView::rebuildImage() {
    image_ = QImage();
    if (!data_ || !valid(*data_)) return;
    const QRectF r = plotRect();
    const qreal dpr = devicePixelRatioF();
    const int w = int(std::round(r.width() * dpr));
    const int h = int(std::round(r.height() * dpr));
    if (w < 2 || h < 2) return;

    const uv::Spectrogram& s = *data_;
    QImage img(w, h, QImage::Format_RGB32);

    // Column spans: when the image is narrower than the data, each pixel takes
    // the max of the columns it covers so a short transient is not skipped.
    std::vector<int> colLo(static_cast<size_t>(w)), colHi(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x) {
        int a = int(std::floor(double(x) * s.cols / w));
        int b = int(std::floor(double(x + 1) * s.cols / w));
        a = std::clamp(a, 0, s.cols - 1);
        b = std::clamp(std::max(b, a + 1), a + 1, s.cols);
        colLo[size_t(x)] = a;
        colHi[size_t(x)] = b;
    }

    const double logLo = std::log10(s.minHz), logHi = std::log10(s.maxHz);
    for (int y = 0; y < h; ++y) {
        auto* out = reinterpret_cast<QRgb*>(img.scanLine(y));
        // A linear axis starts at 0 Hz, below the lowest data row; show the
        // lowest row there rather than marking it as missing.
        const double f = std::max(yToFreq(r.top() + (y + 0.5) / dpr, r), s.minHz);
        if (f > s.maxHz) {
            // Diagonal hatch marks "no data here", distinct from quiet black.
            for (int x = 0; x < w; ++x)
                out[x] = (((x + y) / int(std::max<qreal>(1, 3 * dpr))) % 4 == 0) ? kEmptyHatch : kEmpty;
            continue;
        }
        const double pos = (std::log10(f) - logLo) / (logHi - logLo) * (s.rows - 1);
        const int r0 = std::clamp(int(std::floor(pos)), 0, s.rows - 1);
        const int r1 = std::min(r0 + 1, s.rows - 1);
        const float u = float(std::clamp(pos - r0, 0.0, 1.0));
        const float* row0 = s.db.data() + size_t(r0) * size_t(s.cols);
        const float* row1 = s.db.data() + size_t(r1) * size_t(s.cols);
        for (int x = 0; x < w; ++x) {
            float best = -1e9f;
            for (int c = colLo[size_t(x)]; c < colHi[size_t(x)]; ++c)
                best = std::max(best, row0[c] + (row1[c] - row0[c]) * u);
            out[x] = colorFor(best);
        }
    }
    img.setDevicePixelRatio(dpr);
    image_ = std::move(img);
}

void SpectrogramView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (plotRect().contains(pos)) hover_ = pos;
    else hover_.reset();
    if (dragStart_ && (event->buttons() & Qt::LeftButton)) {
        // A few pixels of slop so a shaky click still counts as a click.
        if (!dragging_ && (pos - *dragStart_).manhattanLength() >= 4) dragging_ = true;
        if (dragging_) selection_ = selectionFromDrag(*dragStart_, pos);
    }
    update();
}

void SpectrogramView::leaveEvent(QEvent*) {
    hover_.reset();
    update();
}

void SpectrogramView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kBg);

    // A device pixel ratio change (moving between monitors) arrives without a
    // resize, so catch it here rather than drawing a blurry image.
    if (data_ && (image_.isNull() || !qFuzzyCompare(image_.devicePixelRatio(), devicePixelRatioF())))
        rebuildImage();

    const QRectF r = plotRect();
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
    const QFontMetricsF fm(small);

    {
        QFont tf = font();
        tf.setBold(true);
        p.setFont(tf);
        p.setPen(kTitleText);
        p.drawText(QPointF(r.left(), 4 + QFontMetricsF(tf).ascent()), title_);
    }
    p.setFont(small);
    if (r.width() < 10 || r.height() < 10) return;

    const bool hasData = data_ && valid(*data_);
    if (hasData && !image_.isNull()) p.drawImage(r.topLeft(), image_);
    else p.fillRect(r, QColor(0x12, 0x0f, 0x1c));

    p.setRenderHint(QPainter::Antialiasing);

    // Log axis: the decade reference lines. Linear axis: even steps, as in
    // most external spectrogram tools, so the two can be compared directly.
    std::vector<double> gridHz;
    if (linear_) {
        const double step = displayMaxHz_ <= 30000.0 ? 5000.0 : displayMaxHz_ <= 100000.0 ? 10000.0 : 20000.0;
        for (double f = step; f <= displayMaxHz_ * 1.0001; f += step) gridHz.push_back(f);
    } else {
        gridHz = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000};
    }
    for (double f : gridHz) {
        if (f < kMinHz || f > displayMaxHz_ * 1.0001) continue;
        const double y = freqToY(f, r);
        const bool focus = (f == 10000.0 || f == 20000.0);
        p.setPen(QPen(focus ? kGridFocus : kGridMajor, 1.0, focus ? Qt::SolidLine : Qt::DotLine));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        const QString t = freqLabel(f);
        p.setPen(focus ? kFocusText : kAxisText);
        const double ty = std::clamp(y + fm.ascent() / 2 - 1, r.top() + fm.ascent() - 2, r.bottom() + 2);
        p.drawText(QPointF(r.left() - 5 - fm.horizontalAdvance(t), ty), t);
    }

    const double dur = hasData ? data_->durationSeconds : 0.0;
    if (dur > 0) {
        static const double kSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 1800, 3600};
        const int maxLabels = std::clamp(int(r.width() / 55.0), 2, 10);
        double step = kSteps[std::size(kSteps) - 1];
        for (double s : kSteps) {
            if (dur / s <= maxLabels) { step = s; break; }
        }
        for (double t = 0; t <= dur + 1e-9; t += step) {
            const double x = r.left() + t / dur * r.width();
            p.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
            p.drawLine(QPointF(x, r.bottom()), QPointF(x, r.bottom() + 4));
            const QString lbl = timeLabel(t, false);
            const double w = fm.horizontalAdvance(lbl);
            p.setPen(kAxisText);
            p.drawText(QPointF(std::clamp(x - w / 2, r.left() - 2, r.right() - w + 2), r.bottom() + 6 + fm.ascent()), lbl);
        }
    }

    p.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    if (!hasData) {
        p.setPen(QColor(0x8a, 0x86, 0x9a));
        QFont big = font();
        big.setPointSizeF(big.pointSizeF() + 1);
        p.setFont(big);
        p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap,
                   title_.startsWith("Processed") ? tr("Apply to see the processed spectrogram") : tr("No data"));
        return;
    }

    if (cutoffHz_ && *cutoffHz_ >= kMinHz && *cutoffHz_ <= displayMaxHz_) {
        const double y = freqToY(*cutoffHz_, r);
        p.setPen(QPen(kMarkerColor, 1.2, Qt::DashLine));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        const QString lbl = tr("cutoff %1").arg(freqLabel(*cutoffHz_));
        const double w = fm.horizontalAdvance(lbl);
        QRectF lr(r.right() - w - 8, y - fm.height() - 2, w + 4, fm.height());
        if (lr.top() < r.top()) lr.moveTop(y + 2);
        p.fillRect(lr, QColor(13, 11, 20, 200));
        p.setPen(kMarkerColor);
        p.drawText(QPointF(lr.left() + 2, lr.top() + fm.ascent()), lbl);
    }

    if (selection_ && (selection_->hasTime || selection_->hasFreq)) {
        const uv::Selection& s = *selection_;
        double x0 = r.left(), x1 = r.right(), y0 = r.top(), y1 = r.bottom();
        if (s.hasTime && dur > 0) {
            x0 = std::clamp(timeToX(std::min(s.t0, s.t1), r), r.left(), r.right());
            x1 = std::clamp(timeToX(std::max(s.t0, s.t1), r), r.left(), r.right());
        }
        if (s.hasFreq) {
            y0 = std::clamp(freqToY(std::max(s.f0, s.f1), r), r.top(), r.bottom());
            y1 = std::clamp(freqToY(std::min(s.f0, s.f1), r), r.top(), r.bottom());
        }
        const QRectF sr(QPointF(x0, y0), QPointF(x1, y1));
        p.setPen(QPen(QColor(120, 200, 255, 220), 1.0));
        p.setBrush(QColor(120, 200, 255, 50));
        p.drawRect(sr);
        p.setBrush(Qt::NoBrush);
    }

    if (playhead_ && dur > 0 && *playhead_ >= 0 && *playhead_ <= dur) {
        const double x = r.left() + *playhead_ / dur * r.width();
        p.setPen(QPen(QColor(255, 255, 255, 205), 1.5));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }

    if (hover_) {
        const QPointF hp = *hover_;
        p.setPen(QPen(QColor(255, 255, 255, 70), 1.0));
        p.drawLine(QPointF(hp.x(), r.top()), QPointF(hp.x(), r.bottom()));
        p.drawLine(QPointF(r.left(), hp.y()), QPointF(r.right(), hp.y()));

        const uv::Spectrogram& s = *data_;
        const double t = std::clamp((hp.x() - r.left()) / r.width(), 0.0, 1.0) * dur;
        const double f = yToFreq(hp.y(), r);
        QString dbText;
        if (f > s.maxHz) {
            dbText = tr("above Nyquist");
        } else {
            const double pos = (std::log10(std::max(f, s.minHz)) - std::log10(s.minHz)) /
                               (std::log10(s.maxHz) - std::log10(s.minHz)) * (s.rows - 1);
            const int row = std::clamp(int(std::round(pos)), 0, s.rows - 1);
            const int col = std::clamp(int(t / dur * s.cols), 0, s.cols - 1);
            dbText = QString::number(s.db[size_t(row) * size_t(s.cols) + size_t(col)], 'f', 1) + " dB";
        }
        const QStringList lines{
            timeLabel(t, true),
            f >= 1000 ? QString::number(f / 1000.0, 'f', 2) + " kHz" : QString::number(f, 'f', 0) + " Hz",
            dbText};
        double w = 0;
        for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const double bw = w + 12, bh = lines.size() * fm.height() + 8;
        double bx = hp.x() + 12, by = hp.y() + 12;
        if (bx + bw > r.right()) bx = hp.x() - 12 - bw;
        if (by + bh > r.bottom()) by = hp.y() - 12 - bh;
        p.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
        p.setBrush(QColor(13, 11, 20, 230));
        p.drawRoundedRect(QRectF(bx, by, bw, bh), 3, 3);
        p.setPen(kTitleText);
        for (int i = 0; i < lines.size(); ++i)
            p.drawText(QPointF(bx + 6, by + 4 + fm.ascent() + i * fm.height()), lines[i]);
    }
}

} // namespace uv::app
