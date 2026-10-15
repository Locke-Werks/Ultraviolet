#pragma once

#include "uvcore/Types.h"

namespace uv {

// A region of a time-frequency view. Time in seconds, frequency in Hz. A time
// selection is a full-height column, a frequency selection a full-width band,
// and both together a box.
struct Selection {
    double t0 = 0, t1 = 0;
    double f0 = 0, f1 = 0;
    bool hasTime = false, hasFreq = false;
};

// Audio inside the selection, for auditioning it. With hasTime the result
// covers only [t0, t1] (with short fades so the cut does not click); without
// it the whole buffer. With hasFreq the content is band-limited to [f0, f1]
// by an STFT mask whose edges roll off just outside the band, so content
// inside the band passes at unity gain.
AudioBuffer extractSelection(const AudioBuffer& in, const Selection& s);

} // namespace uv
