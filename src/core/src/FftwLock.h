#pragma once

#include <mutex>

namespace uv::detail {

// FFTW planning and plan destruction are not thread-safe, and files are
// processed concurrently, so every fftw_plan_* and fftw_destroy_plan in uvcore
// goes through this one lock.
inline std::mutex& fftwPlanMutex() {
    static std::mutex m;
    return m;
}

} // namespace uv::detail
