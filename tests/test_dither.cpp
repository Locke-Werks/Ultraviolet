// BitDepth/Dither tests. Owned by the BitDepthProcessor task.
// Targets: float path is a no-op (bit-exact); 24-bit path quantizes to grid,
// noise floor near expected, no DC offset introduced.
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Dither placeholder", "[dither]") {
    SUCCEED("replace with quantization/noise-floor tests");
}
