// HfSynthesizer tests. Owned by the HfSynthesizer task.
// Targets: strength<=0 is a null (no change); restoreInBand adds energy only in
// [cutoff, restoreTop]; exciter THD bounded; no aliasing below cutoff.
#include <catch2/catch_test_macros.hpp>

TEST_CASE("HfSynthesizer placeholder", "[hfsynth]") {
    SUCCEED("replace with synthesis/null tests");
}
