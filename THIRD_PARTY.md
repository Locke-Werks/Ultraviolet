# Third-party components

| Component | License | Use |
|---|---|---|
| [Qt 6](https://www.qt.io/) (Widgets, Multimedia, Concurrent) | LGPLv3 | GUI and audio output, linked dynamically |
| [libsndfile](https://github.com/libsndfile/libsndfile) | LGPL-2.1 | WAV, RF64, FLAC and AIFF reading and writing |
| [FFTW 3](https://www.fftw.org/) | GPL-2.0-or-later | Every FFT: analysis, extension, audition, selection, air shelf |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | MIT | Sample rate conversion, vendored in `third_party/` |
| [Catch2](https://github.com/catchorg/Catch2) | BSL-1.0 | Tests only, not shipped |

libsndfile pulls in libFLAC, libogg, libvorbis, libopus, mpg123 and LAME through
vcpkg, each under its own BSD, LGPL or similar license.
