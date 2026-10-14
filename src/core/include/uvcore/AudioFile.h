#pragma once

#include "uvcore/Types.h"

#include <optional>
#include <string>

namespace uv {

enum class OutputFormat {
    Float32, // 32-bit IEEE float, full precision retained
    Pcm24,   // 24-bit PCM (requires dither/noise-shaping upstream)
};

// Decode an audio file (WAV/FLAC/AIFF/...) into a planar double buffer.
// On failure returns nullopt and, if err != nullptr, sets *err.
std::optional<AudioBuffer> loadAudio(const std::string& path, std::string* err = nullptr);

// Encode a planar buffer to disk in the requested format.
// Returns false on failure and, if err != nullptr, sets *err.
bool saveAudio(const std::string& path, const AudioBuffer& buffer,
               OutputFormat format, std::string* err = nullptr);

} // namespace uv
