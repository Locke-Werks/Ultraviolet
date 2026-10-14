#include "AudioPlayer.h"

#include "uvcore/Resampler.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QByteArray>
#include <QMediaDevices>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace uv::app {

namespace {

QAudioFormat makeFormat(int rate, int channels, QAudioFormat::SampleFormat sf) {
    QAudioFormat f;
    f.setSampleRate(rate);
    f.setChannelCount(channels);
    f.setSampleFormat(sf);
    return f;
}

// Float first because it carries the processed signal without requantizing;
// Int16 is the fallback every shared-mode device accepts.
bool pickFormat(const QAudioDevice& dev, int rate, int channels, QAudioFormat& out) {
    for (auto sf : {QAudioFormat::Float, QAudioFormat::Int16}) {
        QAudioFormat f = makeFormat(rate, channels, sf);
        if (dev.isFormatSupported(f)) {
            out = f;
            return true;
        }
    }
    return false;
}

// Source sample for output channel c, mapping between channel layouts.
inline double sampleFor(const uv::AudioBuffer& b, int outChannels, int c, std::size_t i) {
    const int n = b.channelCount();
    if (n == outChannels) return b.channels[c][i];
    if (outChannels == 1) {
        double s = 0.0;
        for (int k = 0; k < n; ++k) s += b.channels[k][i];
        return s / n;
    }
    return b.channels[c % n][i];
}

QByteArray interleave(const uv::AudioBuffer& b, const QAudioFormat& f) {
    const int outCh = f.channelCount();
    const std::size_t frames = b.frames();
    const bool isFloat = f.sampleFormat() == QAudioFormat::Float;
    const std::size_t bytes = frames * outCh * (isFloat ? sizeof(float) : sizeof(std::int16_t));
    QByteArray pcm(static_cast<qsizetype>(bytes), Qt::Uninitialized);

    if (isFloat) {
        auto* dst = reinterpret_cast<float*>(pcm.data());
        for (std::size_t i = 0; i < frames; ++i)
            for (int c = 0; c < outCh; ++c)
                *dst++ = static_cast<float>(std::clamp(sampleFor(b, outCh, c, i), -1.0, 1.0));
    } else {
        auto* dst = reinterpret_cast<std::int16_t*>(pcm.data());
        for (std::size_t i = 0; i < frames; ++i)
            for (int c = 0; c < outCh; ++c) {
                const double s = std::clamp(sampleFor(b, outCh, c, i), -1.0, 1.0);
                *dst++ = static_cast<std::int16_t>(std::lround(s * 32767.0));
            }
    }
    return pcm;
}

} // namespace

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {}

AudioPlayer::~AudioPlayer() {
    // Silence the signal so a destroying owner is not called back.
    const QSignalBlocker block(this);
    stop();
}

void AudioPlayer::play(const uv::AudioBuffer& buffer) {
    stop();

    if (buffer.empty() || buffer.sampleRate <= 0.0) {
        emit error(QStringLiteral("Nothing to play: buffer is empty."));
        return;
    }

    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) {
        emit error(QStringLiteral("No audio output device available."));
        return;
    }

    const QAudioFormat preferred = dev.preferredFormat();
    const int srcRate = static_cast<int>(std::lround(buffer.sampleRate));
    const int srcCh = buffer.channelCount();
    int outCh = srcCh;

    QAudioFormat fmt;
    bool ok = pickFormat(dev, srcRate, outCh, fmt);
    if (!ok) {
        // Channel count may be the problem rather than the rate.
        const int prefCh = preferred.channelCount() >= 2 ? 2 : 1;
        if (prefCh != outCh && pickFormat(dev, srcRate, prefCh, fmt)) {
            outCh = prefCh;
            ok = true;
        }
    }
    int playRate = srcRate;
    if (!ok) {
        playRate = preferred.sampleRate();
        if (!pickFormat(dev, playRate, outCh, fmt)) {
            outCh = preferred.channelCount() >= 2 ? 2 : 1;
            if (!pickFormat(dev, playRate, outCh, fmt)) {
                emit error(QStringLiteral("Output device supports no usable format."));
                return;
            }
        }
    }

    if (playRate != srcRate) {
        // Copy via resample: the caller's buffer is never touched.
        const uv::AudioBuffer converted = uv::resample(buffer, static_cast<double>(playRate));
        pcm_ = std::make_unique<QByteArray>(interleave(converted, fmt));
    } else {
        pcm_ = std::make_unique<QByteArray>(interleave(buffer, fmt));
    }

    device_ = new QBuffer(pcm_.get(), this);
    if (!device_->open(QIODevice::ReadOnly)) {
        delete device_;
        device_ = nullptr;
        pcm_.reset();
        emit error(QStringLiteral("Could not open playback buffer."));
        return;
    }

    sink_ = new QAudioSink(dev, fmt, this);
    const quint64 gen = ++generation_;
    connect(sink_, &QAudioSink::stateChanged, this,
            [this, gen](QtAudio::State s) {
                if (gen == generation_) onSinkStateChanged(static_cast<int>(s));
            });

    sink_->start(device_);
    if (sink_->error() != QtAudio::NoError) {
        stop();
        emit error(QStringLiteral("Audio output failed to start."));
        return;
    }

    playing_ = true;
    emit started();
}

void AudioPlayer::onSinkStateChanged(int state) {
    if (!sink_) return;
    const auto s = static_cast<QtAudio::State>(state);
    const QtAudio::Error err = sink_->error();

    const bool drained = s == QtAudio::IdleState && device_ && device_->atEnd();
    const bool halted = s == QtAudio::StoppedState;
    if (!drained && !halted) return;

    // Qt 6.11 no longer reports underrun at end of data, so any error is real.
    if (err != QtAudio::NoError)
        emit error(err == QtAudio::OpenError ? QStringLiteral("Audio device could not be opened.")
                   : err == QtAudio::IOError ? QStringLiteral("Audio device I/O error.")
                                             : QStringLiteral("Audio output error."));

    // Stopping the sink from inside its own signal re-enters it; defer.
    const quint64 gen = generation_;
    QMetaObject::invokeMethod(this, [this, gen] { finishPlayback(gen); }, Qt::QueuedConnection);
}

void AudioPlayer::finishPlayback(quint64 generation) {
    if (generation == generation_) stop();
}

void AudioPlayer::stop() {
    ++generation_;
    const bool wasPlaying = playing_;
    playing_ = false;

    if (sink_) {
        sink_->disconnect(this);
        sink_->stop();
        sink_->deleteLater();
        sink_ = nullptr;
    }
    if (device_) {
        // Closed before pcm_ goes so the deferred delete never sees a dangling array.
        device_->close();
        device_->deleteLater();
        device_ = nullptr;
    }
    pcm_.reset();

    if (wasPlaying) emit stopped();
}

bool AudioPlayer::isPlaying() const { return playing_; }

double AudioPlayer::positionSeconds() const {
    return (playing_ && sink_) ? static_cast<double>(sink_->processedUSecs()) / 1e6 : 0.0;
}

} // namespace uv::app
