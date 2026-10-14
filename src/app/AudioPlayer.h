#pragma once

#include "uvcore/Types.h"

#include <QObject>

#include <memory>

class QAudioSink;
class QBuffer;

namespace uv::app {

// Plays an in-memory AudioBuffer on the default output device. Buffers whose
// rate the device cannot take (e.g. 192 kHz) are resampled to a supported rate
// for playback only, so original and processed always audition at one rate.
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

    // Stops anything playing, then plays buffer from the start.
    void play(const uv::AudioBuffer& buffer);
    void stop();
    bool isPlaying() const;

    // Seconds of audio handed to the device since play(). Leads what is heard
    // by the device buffer (tens of ms), which is fine for a display playhead.
    double positionSeconds() const;

signals:
    void started();
    void stopped();
    void error(const QString& message);

private:
    QAudioSink* sink_ = nullptr;           // parented to this
    QBuffer* device_ = nullptr;            // parented to this
    std::unique_ptr<QByteArray> pcm_;      // backing store for device_
    bool playing_ = false;
    // Bumped on every play/stop so a queued end-of-playback from an earlier
    // sink cannot stop a newer one.
    quint64 generation_ = 0;

    void onSinkStateChanged(int state);
    void finishPlayback(quint64 generation);
};

} // namespace uv::app
