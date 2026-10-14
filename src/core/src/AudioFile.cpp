#include "uvcore/AudioFile.h"

#include <sndfile.h>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace uv {

namespace {

// Map a libsndfile subtype to the decoded source bit depth. This is
// informational: it records what the on-disk samples were before promotion
// to double, so downstream dither/noise-shaping can target the real source.
int bitDepthFromFormat(int format) {
    switch (format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_S8:
        case SF_FORMAT_PCM_U8:
            return 8;
        case SF_FORMAT_PCM_16:
            return 16;
        case SF_FORMAT_PCM_24:
            return 24;
        case SF_FORMAT_PCM_32:
        case SF_FORMAT_FLOAT:
            return 32;
        case SF_FORMAT_DOUBLE:
            return 64;
        default:
            return 0;
    }
}

} // namespace

std::optional<AudioBuffer> loadAudio(const std::string& path, std::string* err) {
    SF_INFO info{};
    SNDFILE* snd = sf_open(path.c_str(), SFM_READ, &info);
    if (!snd) {
        if (err) *err = sf_strerror(nullptr);
        return std::nullopt;
    }

    const int channels = info.channels;
    const sf_count_t frameCount = info.frames;

    AudioBuffer buffer;
    buffer.sampleRate = static_cast<double>(info.samplerate);
    buffer.sourceBitDepth = bitDepthFromFormat(info.format);
    buffer.channels.assign(static_cast<std::size_t>(channels),
                           std::vector<Sample>(static_cast<std::size_t>(frameCount)));

    // libsndfile hands back interleaved frames; read in blocks and
    // de-interleave into the planar channel vectors.
    const sf_count_t kBlockFrames = 4096;
    std::vector<double> interleaved(static_cast<std::size_t>(kBlockFrames) * channels);

    sf_count_t frameOffset = 0;
    while (frameOffset < frameCount) {
        const sf_count_t got = sf_readf_double(snd, interleaved.data(), kBlockFrames);
        if (got <= 0) break;
        for (sf_count_t f = 0; f < got; ++f) {
            for (int c = 0; c < channels; ++c) {
                buffer.channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(frameOffset + f)] =
                    interleaved[static_cast<std::size_t>(f) * channels + c];
            }
        }
        frameOffset += got;
    }

    sf_close(snd);
    return buffer;
}

bool saveAudio(const std::string& path, const AudioBuffer& buffer,
               OutputFormat format, std::string* err) {
    const int channels = buffer.channelCount();
    const sf_count_t frameCount = static_cast<sf_count_t>(buffer.frames());

    // Ensure the destination directory exists before libsndfile tries to
    // create the file there.
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (err) *err = ec.message();
            return false;
        }
    }

    int subtype = 0;
    switch (format) {
        case OutputFormat::Float32:
            subtype = SF_FORMAT_FLOAT;
            break;
        case OutputFormat::Pcm24:
            subtype = SF_FORMAT_PCM_24;
            break;
        case OutputFormat::Float64:
            subtype = SF_FORMAT_DOUBLE;
            break;
    }

    // RF64 lifts WAV's 4 GB limit, which 384 kHz / 64-bit stereo passes in
    // about ten minutes. Auto-downgrade writes a plain WAV whenever the
    // result fits, so ordinary files stay maximally compatible.
    SF_INFO info{};
    info.samplerate = static_cast<int>(buffer.sampleRate);
    info.channels = channels;
    info.format = SF_FORMAT_RF64 | subtype;

    SNDFILE* snd = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!snd) {
        if (err) *err = sf_strerror(nullptr);
        return false;
    }
    sf_command(snd, SFC_RF64_AUTO_DOWNGRADE, nullptr, SF_TRUE);

    // Clamp samples that round above full scale rather than wrapping, so a
    // 24-bit write of a value at the extreme does not invert.
    sf_command(snd, SFC_SET_CLIPPING, nullptr, SF_TRUE);

    const sf_count_t kBlockFrames = 4096;
    std::vector<double> interleaved(static_cast<std::size_t>(kBlockFrames) * channels);

    bool ok = true;
    sf_count_t frameOffset = 0;
    while (frameOffset < frameCount) {
        const sf_count_t block = std::min<sf_count_t>(kBlockFrames, frameCount - frameOffset);
        for (sf_count_t f = 0; f < block; ++f) {
            for (int c = 0; c < channels; ++c) {
                interleaved[static_cast<std::size_t>(f) * channels + c] =
                    buffer.channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(frameOffset + f)];
            }
        }
        const sf_count_t wrote = sf_writef_double(snd, interleaved.data(), block);
        if (wrote != block) {
            if (err) *err = sf_strerror(snd);
            ok = false;
            break;
        }
        frameOffset += block;
    }

    sf_close(snd);
    return ok;
}

} // namespace uv
