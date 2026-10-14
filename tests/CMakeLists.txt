find_package(Catch2 CONFIG REQUIRED)

add_executable(uvtests
    test_smoke.cpp
    test_audiofile.cpp
    test_resampler.cpp
    test_dither.cpp
    test_spectral.cpp
    test_hfsynth.cpp
    test_pipeline.cpp
    test_processing.cpp
    test_spectrogram.cpp
)

target_link_libraries(uvtests PRIVATE
    uvcore
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(uvtests)
