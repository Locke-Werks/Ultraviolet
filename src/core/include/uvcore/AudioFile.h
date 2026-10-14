#pragma once

#include "uvcore/Types.h"

#include <optional>
#include <string>

namespace uv {

enum class OutputFormat {
    Float32, // 32-bit IEEE float
    Pcm24,   // 24-bit PCM (requires dither/noise-shaping upstream)
    Float64, // 64-bit IEEE float: the pipeline's internal precision, written as is
};

// Decode an audio file (WAV/FLAC/AIFF/...) into a planar double buffer.
// On failure returns nullopt and, if err != nullptr, sets *err.
std::optional<AudioBuffer> loadAudio(const std::string& path, std::string* err = nullptr);

// Encode a planar buffer to disk in the requested format.
// Returns false on failure and, if err != nullptr, sets *err.
bool saveAudio(const std::string& path, const AudioBuffer& buffer,
               OutputFormat format, std::string* err = nullptr);

} // namespace uv
