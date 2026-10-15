find_package(SndFile CONFIG REQUIRED)
find_package(FFTW3 CONFIG REQUIRED)

add_library(uvcore STATIC
    src/AudioFile.cpp
    src/Resampler.cpp
    src/SpectralAnalyzer.cpp
    src/Dither.cpp
    src/AlgorithmicHfSynthesizer.cpp
    src/Pipeline.cpp
    src/Audition.cpp
    src/Selection.cpp
)

target_include_directories(uvcore PUBLIC include)

target_link_libraries(uvcore
    PUBLIC
        SndFile::sndfile
    PRIVATE
        FFTW3::fftw3
        r8brain
)
