# Third-party components

Ultraviolet is licensed under GPLv3. The following dependencies are used and
their licenses are compatible with GPLv3.

| Component          | Purpose                          | License        |
| ------------------ | -------------------------------- | -------------- |
| Qt 6               | Desktop GUI framework            | LGPLv3 / GPLv3 |
| libsndfile         | Audio file decode/encode         | LGPL-2.1       |
| FFTW3              | FFT for spectral analysis        | GPL-2.0+       |
| r8brain-free-src   | High-quality arbitrary resampler | MIT            |
| Catch2             | Unit test framework              | BSL-1.0        |

r8brain-free-src is vendored under `third_party/r8brain-free-src/`. The other
components are resolved at build time (Qt from the system Qt install; the rest
via the vcpkg manifest in `vcpkg.json`).
