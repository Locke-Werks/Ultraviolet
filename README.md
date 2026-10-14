<div align="center">

<img src="assets/ultraviolet.ico" width="96" alt="Ultraviolet">

# Ultraviolet

**An audio upscaler: transparent 192 kHz / 32-bit conversion, with reconstruction of rolled-off high-frequency content.**

[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%2011-d6262a?style=flat-square)](#requirements)

</div>

---

Ultraviolet upscales audio files to high sample rates and bit depths. It does two
separable jobs and is honest about the difference:

1. **Transparent conversion.** A reference-grade arbitrary-ratio resampler and
   noise-shaped requantization take a source (e.g. 48 kHz / 16-bit) to 192 kHz /
   32-bit without altering the original passband. Provably lossless in-band.
2. **High-frequency reconstruction.** For sources whose top end was rolled off
   (lossy codecs, older recordings), Ultraviolet synthesizes plausible content in
   the 10-20 kHz region, and optionally adds ultrasonic character. This is
   resynthesis, not recovery: nothing restores information a brickwall filter
   removed from a true full-band capture.

Mode is user-selectable (Mastering / Rescue / Auto), with Auto deciding per source
from a measured high-frequency rolloff.

## Requirements

Windows 11 x64.

## Building

Toolchain: Visual Studio 2022, CMake 3.24+, Qt 6, and vcpkg (manifest mode) for
libsndfile, FFTW3, and Catch2.

```
cmake --preset default
cmake --build --preset default --config Release
ctest --preset default
```

Adjust the Qt path in `CMakePresets.json` (`CMAKE_PREFIX_PATH`) to match your
install.

## Layout

- `src/core` - `uvcore`, the Qt-free DSP library (file IO, resampling, analysis,
  HF synthesis, dither, pipeline).
- `src/app` - `uvapp`, the Qt 6 desktop application.
- `tests` - Catch2 tests against `uvcore`.
- `third_party` - vendored r8brain-free-src.

## License

GPLv3. See [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).
