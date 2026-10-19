// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include "ScaleUtils.h"
#include <cmath>
#include <optional>

// Scale-aware ("diatonic") transposition used by the harmony buttons.
//
// A harmony interval is expressed in scale degrees, not semitones: a third up
// is two scale steps up, so in C major C->E is a major third while D->F is a
// minor third. Only the note's base pitch moves; the per-frame pitch curve
// (vibrato, slides, intonation nuance) is left untouched by the caller.
namespace HarmonyUtils
{
// Outcome of a harmony button press, so the UI can explain a no-op.
enum class Result
{
    Applied,
    NoNotes,
    ScaleUnsupported,
    PartlyOutOfRange, // some notes moved; the rest would leave the editor range
    OutOfRange        // every candidate would leave the editor range
};

// Harmony intervals offered in the UI. The value is the number of scale steps
// for diatonic intervals; Octave is handled as a fixed 12 semitones.
enum class Interval : int
{
    Second = 1,
    Third = 2,
    Fourth = 3,
    Fifth = 4,
    Sixth = 5,
    Octave = 7
};

inline int countScaleTones(ScaleMode mode)
{
    const auto mask = ScaleUtils::getIntervalMaskForMode(mode);
    int count = 0;
    for (int i = 0; i < 12; ++i)
        if ((mask & ScaleUtils::intervalBit(i)) != 0)
            ++count;
    return count;
}

// Degree-based harmony needs a seven-tone scale. Pentatonic, blues and
// whole-tone scales have no stable meaning for "a third", so the UI disables
// the degree buttons there (octave still works).
inline bool supportsDiatonicIntervals(ScaleMode mode, int rootNote)
{
    if (rootNote < 0 || mode == ScaleMode::None || mode == ScaleMode::Chromatic)
        return false;
    return countScaleTones(mode) == 7;
}

// Nearest scale tone to an integer MIDI note. Ties resolve downward, the same
// tie rule as ScaleUtils::snapMidiToScale. Unlike drag-snap, the caller rounds
// to the nearest semitone first, so a slightly sharp C (61.4) anchors to C.
inline int nearestScaleTone(int midi, ScaleMode mode, int rootNote)
{
    for (int distance = 0; distance <= 6; ++distance) {
        if (ScaleUtils::isPitchClassInScale(mode, midi - distance, rootNote))
            return midi - distance;
        if (ScaleUtils::isPitchClassInScale(mode, midi + distance, rootNote))
            return midi + distance;
    }
    return midi;
}

// Semitone shift that moves `adjustedMidi` by `steps` scale degrees
// (positive = up). Returns nullopt when the scale cannot express degrees.
//
// Out-of-scale (chromatic) notes are anchored to their nearest scale tone, moved
// with it, and keep their chromatic offset. The result can land on a scale tone
// (C# a third up in C major is F), so for chromatic notes two thirds are not
// always the same as one fifth; for in-scale notes they are.
inline std::optional<int> diatonicShiftSemitones(float adjustedMidi,
                                                  ScaleMode mode,
                                                  int rootNote,
                                                  int steps,
                                                  int referenceHz = static_cast<int>(FREQ_A4))
{
    if (!supportsDiatonicIntervals(mode, rootNote))
        return std::nullopt;
    if (steps == 0)
        return 0;

    const float offset = ScaleUtils::getReferenceOffsetSemitones(referenceHz);
    const int rounded = static_cast<int>(std::round(adjustedMidi - offset));
    const int anchor = nearestScaleTone(rounded, mode, rootNote);

    const int direction = steps > 0 ? 1 : -1;
    int target = anchor;
    for (int remaining = std::abs(steps); remaining > 0; --remaining) {
        do {
            target += direction;
        } while (!ScaleUtils::isPitchClassInScale(mode, target, rootNote));
    }
    return target - anchor;
}

// Semitone shift for a harmony button press. `up` selects the direction.
inline std::optional<int> intervalShiftSemitones(float adjustedMidi,
                                                  Interval interval,
                                                  bool up,
                                                  ScaleMode mode,
                                                  int rootNote,
                                                  int referenceHz = static_cast<int>(FREQ_A4))
{
    if (interval == Interval::Octave)
        return up ? 12 : -12;
    const int steps = static_cast<int>(interval) * (up ? 1 : -1);
    return diatonicShiftSemitones(adjustedMidi, mode, rootNote, steps, referenceHz);
}
}
