#pragma once

#include "uvcore/AudioFile.h"
#include "uvcore/Types.h"

#include <string>

namespace uv {

struct PipelineSettings {
    double targetRate = 192000.0;
    OutputFormat outputFormat = OutputFormat::Float32;

    enum class Mode {
        Mastering, // transparent only; no in-band synthesis
        Rescue,    // synthesize/reconstruct rolled-off HF content
        Auto,      // decide per-source from SpectrumInfo.rolledOff
    } mode = Mode::Auto;

    double hfStrength = 0.5; // used in Rescue/Auto-rescue
    bool ultrasonic = false; // generate content above original Nyquist
};

// Progress + cancellation sink implemented by callers (e.g. the GUI worker).
class ProgressSink {
public:
    virtual ~ProgressSink() = default;
    virtual void onProgress(double /*fraction01*/) {}
    virtual bool cancelled() const { return false; }
};

// In-memory processing: every stage except file IO. Used for preview, so
// nothing touches disk until the user saves. Returns an empty buffer if
// cancelled.
AudioBuffer processBuffer(const AudioBuffer& input, const PipelineSettings& settings,
                          ProgressSink* progress = nullptr);

// Run the full offline pipeline on one file (load, processBuffer, save).
// Returns false on failure.
bool runPipeline(const std::string& inputPath, const std::string& outputPath,
                 const PipelineSettings& settings, ProgressSink* progress = nullptr,
                 std::string* err = nullptr);

} // namespace uv
