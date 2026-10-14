#include "uvcore/AudioFile.h"

// STUB: implemented by the AudioFile task (libsndfile).

namespace uv {

std::optional<AudioBuffer> loadAudio(const std::string& /*path*/, std::string* err) {
    if (err) *err = "loadAudio not implemented";
    return std::nullopt;
}

bool saveAudio(const std::string& /*path*/, const AudioBuffer& /*buffer*/,
               OutputFormat /*format*/, std::string* err) {
    if (err) *err = "saveAudio not implemented";
    return false;
}

} // namespace uv
