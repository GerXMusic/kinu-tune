// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/Utils/HarmonyUtils.h"
#include "TestAssert.h"
#include <iostream>

using HarmonyUtils::Interval;
using HarmonyUtils::intervalShiftSemitones;

namespace
{
int shift(float midi, Interval interval, bool up, ScaleMode mode, int root,
          int referenceHz = 440)
{
    const auto result = intervalShiftSemitones(midi, interval, up, mode, root, referenceHz);
    CHECK(result.has_value());
    return *result;
}
}

int main()
{
    constexpr int C = 0;
    constexpr int A = 9;

    // C major: thirds switch between major and minor automatically.
    CHECK(shift(60, Interval::Third, true, ScaleMode::Major, C) == 4);  // C -> E
    CHECK(shift(62, Interval::Third, true, ScaleMode::Major, C) == 3);  // D -> F
    CHECK(shift(64, Interval::Third, true, ScaleMode::Major, C) == 3);  // E -> G
    CHECK(shift(71, Interval::Third, true, ScaleMode::Major, C) == 3);  // B -> D
    CHECK(shift(60, Interval::Third, false, ScaleMode::Major, C) == -3); // C -> A
    CHECK(shift(64, Interval::Third, false, ScaleMode::Major, C) == -4); // E -> C

    // Fourths, fifths and sixths stay inside the scale (B -> F is a tritone).
    CHECK(shift(60, Interval::Fourth, true, ScaleMode::Major, C) == 5);  // C -> F
    CHECK(shift(65, Interval::Fourth, true, ScaleMode::Major, C) == 6);  // F -> B
    CHECK(shift(60, Interval::Fifth, true, ScaleMode::Major, C) == 7);   // C -> G
    CHECK(shift(71, Interval::Fifth, true, ScaleMode::Major, C) == 6);   // B -> F
    CHECK(shift(67, Interval::Fifth, false, ScaleMode::Major, C) == -7); // G -> C
    CHECK(shift(60, Interval::Sixth, true, ScaleMode::Major, C) == 9);   // C -> A
    CHECK(shift(64, Interval::Sixth, true, ScaleMode::Major, C) == 8);   // E -> C
    CHECK(shift(60, Interval::Second, true, ScaleMode::Major, C) == 2);  // C -> D
    CHECK(shift(64, Interval::Second, true, ScaleMode::Major, C) == 1);  // E -> F

    // Octaves are fixed and work in any scale, even chromatic.
    CHECK(shift(61, Interval::Octave, true, ScaleMode::Chromatic, C) == 12);
    CHECK(shift(61, Interval::Octave, false, ScaleMode::MajorPentatonic, C) == -12);

    // A natural minor.
    CHECK(shift(57, Interval::Third, true, ScaleMode::Minor, A) == 3);   // A -> C
    CHECK(shift(60, Interval::Third, true, ScaleMode::Minor, A) == 4);   // C -> E
    CHECK(shift(64, Interval::Third, true, ScaleMode::Minor, A) == 3);   // E -> G

    // The same tones as a mode: D dorian has C major's notes.
    CHECK(shift(62, Interval::Third, true, ScaleMode::Dorian, 2) == 3);  // D -> F

    // Off-pitch base notes round to the nearest semitone before the lookup.
    CHECK(shift(60.3f, Interval::Third, true, ScaleMode::Major, C) == 4);
    CHECK(shift(61.7f, Interval::Third, true, ScaleMode::Major, C) == 3); // ~D -> F

    // Chromatic passing tone: C# anchors to C (ties go down), moves with it to
    // E, and keeps its +1 offset -> F.
    CHECK(shift(61, Interval::Third, true, ScaleMode::Major, C) == 4);

    // A raised pitch reference shifts the semitone grid, not the result.
    CHECK(shift(60.0785f, Interval::Third, true, ScaleMode::Major, C, 442) == 4);

    // Scales without seven tones cannot express degrees.
    CHECK(!intervalShiftSemitones(60, Interval::Third, true, ScaleMode::MajorPentatonic, C));
    CHECK(!intervalShiftSemitones(60, Interval::Third, true, ScaleMode::Blues, C));
    CHECK(!intervalShiftSemitones(60, Interval::Third, true, ScaleMode::Chromatic, C));
    CHECK(!intervalShiftSemitones(60, Interval::Third, true, ScaleMode::Major, -1));

    // Every in-scale note, shifted up a third and back down, returns home.
    for (int midi = 48; midi <= 84; ++midi) {
        if (!ScaleUtils::isPitchClassInScale(ScaleMode::HarmonicMinor, midi, A))
            continue;
        const int up = shift(static_cast<float>(midi), Interval::Third, true,
                             ScaleMode::HarmonicMinor, A);
        const int down = shift(static_cast<float>(midi + up), Interval::Third, false,
                               ScaleMode::HarmonicMinor, A);
        CHECK(up + down == 0);
        CHECK(ScaleUtils::isPitchClassInScale(ScaleMode::HarmonicMinor, midi + up, A));
    }

    std::cout << "Harmony interval tests passed\n";
    return 0;
}
