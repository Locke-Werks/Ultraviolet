#include "WaveformView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace uv::app {

namespace {

const QColor kBg(16, 20, 24);
const QColor kGrid(255, 255, 255, 22);
const QColor kLsb(255, 179, 71, 40);
const QColor kSource(255, 179, 71);
const QColor kProcessed(79, 209, 255);
const QColor kText(200, 205, 210);
constexpr int kBlocks[] = {64, 1024, 16384};
constexpr double kMinSpan = 0.0002; // 0.2 ms: about 10 samples at 48 kHz

QString timeLabel(double s) {
    if (s < 1.0) return QString::number(s * 1000.0, 'f', 3) + QStringLiteral(" ms");
    return QString::number(s, 'f', 6) + QStringLiteral(" s");
}

} // namespace

WaveformView::WaveformView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setToolTip(QStringLiteral(
        "Wheel: zoom time. Ctrl+wheel: zoom amplitude. Drag: pan. Double-click: reset. "
        "C: channel. F: follow playhead."));
}

QSize WaveformView::minimumSizeHint() const { return {300, 140}; }

WaveformView::Track WaveformView::makeTrack(std::shared_ptr<const uv::AudioBuffer> buf) {
    Track t;
    t.buf = std::move(buf);
    if (!t.buf) return t;
    for (const auto& ch : t.buf->channels) {
        std::vector<Peaks> levels;
        for (int block : kBlocks) {
            Peaks pk;
            pk.block = block;
            const std::size_t n = (ch.size() + block - 1) / block;
            pk.lo.resize(n);
            pk.hi.resize(n);
            for (std::size_t b = 0; b < n; ++b) {
                const std::size_t a = b * block, e = std::min(ch.size(), a + block);
                double lo = ch[a], hi = ch[a];
                for (std::size_t i = a + 1; i < e; ++i) {
                    lo = std::min(lo, ch[i]);
                    hi = std::max(hi, ch[i]);
                }
                pk.lo[b] = static_cast<float>(lo);
                pk.hi[b] = static_cast<float>(hi);
            }
            levels.push_back(std::move(pk));
        }
        t.levels.push_back(std::move(levels));
    }
    return t;
}

void WaveformView::setBuffers(std::shared_ptr<const uv::AudioBuffer> source,
                              std::shared_ptr<const uv::AudioBuffer> processed, int sourceBitDepth) {
    const bool sameSource = source_.buf == source;
    if (!sameSource) source_ = makeTrack(std::move(source));
    if (processed_.buf != processed) processed_ = makeTrack(std::move(processed));
    bitDepth_ = sourceBitDepth;
    if (!sameSource) resetView();
    clampView();
    update();
}

void WaveformView::setPlayhead(std::optional<double> seconds) {
    playhead_ = seconds;
    // Centre on the playhead every tick, so the waveform scrolls under it
    // instead of jumping a page at a time.
    if (playhead_ && follow_) {
        t0_ = *playhead_ - 0.5 * span_;
        clampView();
    }
    update();
}

void WaveformView::showRange(double t0, double t1) {
    if (t1 < t0) std::swap(t0, t1);
    span_ = std::max(kMinSpan, t1 - t0);
    t0_ = t0;
    clampView();
    update();
}

double WaveformView::duration() const {
    double d = 0.0;
    for (const Track* t : {&source_, &processed_})
        if (t->buf && t->buf->sampleRate > 0) d = std::max(d, t->buf->frames() / t->buf->sampleRate);
    return d;
}

void WaveformView::resetView() {
    // Open on a short window a third of the way in: past any fade-in, and
    // short enough that the source's individual samples are visible.
    const double d = duration();
    span_ = std::min(0.004, std::max(d, kMinSpan));
    t0_ = d / 3.0;
    ampZoom_ = 1.0;
    ampCentre_ = 0.0;
}

void WaveformView::clampView() {
    const double d = duration();
    span_ = std::clamp(span_, kMinSpan, std::max(kMinSpan, d));
    t0_ = std::clamp(t0_, 0.0, std::max(0.0, d - span_));
    ampZoom_ = std::clamp(ampZoom_, 1.0, 1e6);
    const double half = 1.0 / ampZoom_;
    ampCentre_ = std::clamp(ampCentre_, -1.0 + half, 1.0 - half);
}

QRectF WaveformView::plotRect() const { return QRectF(rect()).adjusted(48, 20, -8, -18); }

double WaveformView::xToTime(double x, const QRectF& r) const { return t0_ + (x - r.left()) / r.width() * span_; }
double WaveformView::timeToX(double t, const QRectF& r) const { return r.left() + (t - t0_) / span_ * r.width(); }
double WaveformView::ampToY(double a, const QRectF& r) const {
    return r.center().y() - (a - ampCentre_) * ampZoom_ * 0.5 * r.height();
}

void WaveformView::drawEnvelope(QPainter& p, const QRectF& r, const Track& t, const QColor& c) const {
    const auto& ch = t.buf->channels[channel_];
    const double sr = t.buf->sampleRate;
    const double spp = span_ * sr / r.width();
    const Peaks* lvl = nullptr;
    for (const Peaks& pk : t.levels[channel_])
        if (pk.block <= spp) lvl = &pk;
    p.setPen(QPen(c, 1.0));
    for (int px = 0; px < static_cast<int>(r.width()); ++px) {
        const double ta = t0_ + px * span_ / r.width(), tb = ta + span_ / r.width();
        const long long a = std::max(0LL, static_cast<long long>(std::floor(ta * sr)));
        const long long b = std::min(static_cast<long long>(ch.size()), static_cast<long long>(std::ceil(tb * sr)));
        if (b <= a) continue;
        double lo = 1e9, hi = -1e9;
        if (lvl) {
            const long long ba = a / lvl->block, bb = std::min<long long>(lvl->lo.size(), (b + lvl->block - 1) / lvl->block);
            for (long long k = ba; k < bb; ++k) {
                lo = std::min<double>(lo, lvl->lo[k]);
                hi = std::max<double>(hi, lvl->hi[k]);
            }
        } else {
            for (long long i = a; i < b; ++i) {
                lo = std::min(lo, ch[i]);
                hi = std::max(hi, ch[i]);
            }
        }
        const double x = r.left() + px + 0.5;
        p.drawLine(QPointF(x, ampToY(hi, r)), QPointF(x, ampToY(lo, r) + 0.5));
    }
}

void WaveformView::drawSamples(QPainter& p, const QRectF& r, const Track& t, const QColor& c, bool steps) const {
    const auto& ch = t.buf->channels[channel_];
    const double sr = t.buf->sampleRate;
    const long long a = std::max(0LL, static_cast<long long>(std::floor(t0_ * sr)) - 1);
    const long long b = std::min(static_cast<long long>(ch.size()), static_cast<long long>(std::ceil((t0_ + span_) * sr)) + 2);
    if (b <= a) return;
    QPainterPath path;
    for (long long i = a; i < b; ++i) {
        const double x = timeToX(i / sr, r), y = ampToY(ch[i], r);
        if (i == a) path.moveTo(x, y);
        else if (steps) {
            path.lineTo(x, path.currentPosition().y());
            path.lineTo(x, y);
        } else {
            path.lineTo(x, y);
        }
        if (steps && i + 1 == b) path.lineTo(timeToX((i + 1) / sr, r), y);
    }
    p.setPen(QPen(c, steps ? 1.25 : 1.75));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Dots mark the actual sample instants once there is room for them.
    const double pxPerSample = r.width() / (span_ * sr);
    if (pxPerSample >= 8.0) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        const double rad = steps ? 2.5 : 1.75;
        for (long long i = a; i < b; ++i) p.drawEllipse(QPointF(timeToX(i / sr, r), ampToY(ch[i], r)), rad, rad);
    }
}

void WaveformView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kBg);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = plotRect();
    p.setClipRect(r.adjusted(-1, -1, 1, 1));

    if (!source_.buf && !processed_.buf) {
        p.setClipping(false);
        p.setPen(kText);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Select a file to see its samples"));
        return;
    }

    // Source quantization grid: one line per LSB, shown only when the steps
    // are far enough apart to read, which is where the added bit depth shows.
    double lsbPx = 0.0;
    if (bitDepth_ > 0 && bitDepth_ < 32) {
        const double lsb = 1.0 / std::ldexp(1.0, bitDepth_ - 1);
        lsbPx = lsb * ampZoom_ * 0.5 * r.height();
        if (lsbPx >= 6.0) {
            p.setPen(QPen(kLsb, 1.0));
            const double top = ampCentre_ + 1.0 / ampZoom_, bot = ampCentre_ - 1.0 / ampZoom_;
            for (double v = std::floor(bot / lsb) * lsb; v <= top; v += lsb) p.drawLine(QPointF(r.left(), ampToY(v, r)), QPointF(r.right(), ampToY(v, r)));
        }
    }
    p.setPen(QPen(kGrid, 1.0));
    p.drawLine(QPointF(r.left(), ampToY(0.0, r)), QPointF(r.right(), ampToY(0.0, r)));

    // Processed first so the source staircase stays readable on top.
    for (const auto& [track, colour, steps] : {std::tuple{&processed_, kProcessed, false}, std::tuple{&source_, kSource, true}}) {
        if (!track->buf || channel_ >= track->buf->channelCount()) continue;
        const double spp = span_ * track->buf->sampleRate / r.width();
        // Steps need two pixels per sample to read as steps; a line reads at one.
        if (spp <= (steps ? 0.5 : 1.0)) drawSamples(p, r, *track, colour, steps);
        else drawEnvelope(p, r, *track, colour);
    }

    if (playhead_ && *playhead_ >= t0_ && *playhead_ <= t0_ + span_) {
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.0));
        const double x = timeToX(*playhead_, r);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }

    p.setClipping(false);
    p.setPen(kText);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    p.setFont(f);
    const QString srcInfo = source_.buf ? QStringLiteral("source %1 kHz, %2-bit (steps)")
                                              .arg(source_.buf->sampleRate / 1000.0)
                                              .arg(bitDepth_)
                                        : QString();
    const QString procInfo = processed_.buf ? QStringLiteral("processed %1 kHz (line)").arg(processed_.buf->sampleRate / 1000.0)
                                            : QStringLiteral("processed: press Apply");
    p.setPen(kSource);
    p.drawText(QPointF(r.left(), 14), srcInfo);
    p.setPen(kProcessed);
    p.drawText(QPointF(r.left() + p.fontMetrics().horizontalAdvance(srcInfo) + 16, 14), procInfo);
    p.setPen(kText);
    QString right = QStringLiteral("ch %1  |  %2 wide  |  amp x%3%4")
                        .arg(channel_ + 1)
                        .arg(timeLabel(span_))
                        .arg(ampZoom_, 0, 'g', 4)
                        .arg(follow_ ? QStringLiteral("  |  follow") : QString());
    if (lsbPx >= 6.0) right += QStringLiteral("  |  grid = 1 LSB");
    p.drawText(QRectF(r.left(), 0, r.width(), 18), Qt::AlignRight | Qt::AlignVCenter, right);
    p.drawText(QRectF(0, r.top(), 44, 14), Qt::AlignRight, QString::number(ampCentre_ + 1.0 / ampZoom_, 'g', 3));
    p.drawText(QRectF(0, r.bottom() - 14, 44, 14), Qt::AlignRight, QString::number(ampCentre_ - 1.0 / ampZoom_, 'g', 3));
    p.drawText(QRectF(r.left(), r.bottom() + 2, 200, 16), Qt::AlignLeft, timeLabel(t0_));
    p.drawText(QRectF(r.right() - 200, r.bottom() + 2, 200, 16), Qt::AlignRight, timeLabel(t0_ + span_));

    if (hover_ && r.contains(*hover_)) {
        const double t = xToTime(hover_->x(), r);
        QString s = timeLabel(t);
        for (const auto& [track, name] : {std::pair{&source_, "src"}, std::pair{&processed_, "proc"}}) {
            if (!track->buf || channel_ >= track->buf->channelCount()) continue;
            const long long i = static_cast<long long>(std::floor(t * track->buf->sampleRate));
            if (i < 0 || i >= static_cast<long long>(track->buf->frames())) continue;
            s += QStringLiteral("   %1 #%2 = %3").arg(QLatin1String(name)).arg(i).arg(track->buf->channels[channel_][i], 0, 'f', 7);
        }
        p.drawText(QRectF(r.left(), r.bottom() + 2, r.width(), 16), Qt::AlignHCenter, s);
    }
}

void WaveformView::wheelEvent(QWheelEvent* event) {
    const QRectF r = plotRect();
    const double steps = event->angleDelta().y() / 120.0;
    const double factor = std::pow(1.25, steps);
    if (event->modifiers() & Qt::ControlModifier) {
        const double a = ampCentre_ + (r.center().y() - event->position().y()) / (ampZoom_ * 0.5 * r.height());
        ampZoom_ *= factor;
        clampView();
        ampCentre_ = a - (r.center().y() - event->position().y()) / (ampZoom_ * 0.5 * r.height());
    } else {
        const double t = xToTime(event->position().x(), r);
        span_ /= factor;
        clampView();
        t0_ = t - (event->position().x() - r.left()) / r.width() * span_;
    }
    clampView();
    update();
    event->accept();
}

void WaveformView::mousePressEvent(QMouseEvent* event) {
    dragFrom_ = event->position();
    dragT0_ = t0_;
    dragAmp_ = ampCentre_;
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    hover_ = event->position();
    if (dragFrom_) {
        const QRectF r = plotRect();
        const QPointF d = event->position() - *dragFrom_;
        // Dragging takes the view off the playhead; F turns following back on.
        if (std::abs(d.x()) > 3) follow_ = false;
        t0_ = dragT0_ - d.x() / r.width() * span_;
        ampCentre_ = dragAmp_ + d.y() / (ampZoom_ * 0.5 * r.height());
        clampView();
    }
    update();
}

void WaveformView::mouseReleaseEvent(QMouseEvent*) { dragFrom_.reset(); }

void WaveformView::mouseDoubleClickEvent(QMouseEvent*) {
    resetView();
    clampView();
    update();
}

void WaveformView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_C) {
        int n = 0;
        for (const Track* t : {&source_, &processed_})
            if (t->buf) n = std::max(n, t->buf->channelCount());
        if (n > 0) channel_ = (channel_ + 1) % n;
    } else if (event->key() == Qt::Key_F) {
        follow_ = !follow_;
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    update();
}

void WaveformView::leaveEvent(QEvent*) {
    hover_.reset();
    update();
}

} // namespace uv::app
