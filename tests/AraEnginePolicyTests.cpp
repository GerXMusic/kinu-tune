// D-179: the shared modification engine wins over a stale per-instance Project
// copy, so every window shows the same engine for a track.
#include "../Source/Plugin/AraEnginePolicy.h"
#include "TestAssert.h"

#include <iostream>

using pitchnet::chooseSharedEngine;

namespace
{
void testModificationEngineWins()
{
    const auto chosen = chooseSharedEngine(SynthesisEngineType::Psola,
                                           SynthesisEngineType::Pitchcore);
    CHECK(chosen.has_value());
    CHECK(*chosen == SynthesisEngineType::Psola);
}

void testProjectEngineFallsBackWhenModificationHasNone()
{
    const auto chosen =
        chooseSharedEngine(std::nullopt, SynthesisEngineType::Pitchcore);
    CHECK(chosen.has_value());
    CHECK(*chosen == SynthesisEngineType::Pitchcore);
}

void testModificationOnly()
{
    const auto chosen = chooseSharedEngine(SynthesisEngineType::Pitchcore,
                                           std::nullopt);
    CHECK(chosen.has_value());
    CHECK(*chosen == SynthesisEngineType::Pitchcore);
}

void testLegacyWithoutAnyEngine()
{
    CHECK(!chooseSharedEngine(std::nullopt, std::nullopt).has_value());
}
} // namespace

int main()
{
    testModificationEngineWins();
    testProjectEngineFallsBackWhenModificationHasNone();
    testModificationOnly();
    testLegacyWithoutAnyEngine();

    std::cout << "ARA engine policy tests passed\n";
    return 0;
}
