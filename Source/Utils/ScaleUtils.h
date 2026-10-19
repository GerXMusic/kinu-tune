#pragma once

#include "../Models/Project.h"
#include "Constants.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>

namespace ScaleUtils
{
inline constexpr std::uint16_t intervalBit(int semitone)
{
    return static_cast<std::uint16_t>(1u << semitone);
}

inline constexpr std::uint16_t getIntervalMaskForMode(ScaleMode mode)
{
    switch (mode) {
    case ScaleMode::Major:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::Minor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Dorian:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(10);
    case ScaleMode::Phrygian:
        return intervalBit(0) | intervalBit(1) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Lydian:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(6) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::Mixolydian:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(10);
    case ScaleMode::Locrian:
        return intervalBit(0) | intervalBit(1) | intervalBit(3) |
               intervalBit(5) | intervalBit(6) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Blues:
        return intervalBit(0) | intervalBit(3) | intervalBit(5) |
               intervalBit(6) | intervalBit(7) | intervalBit(10);
    case ScaleMode::HarmonicMinor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(11);
    case ScaleMode::MajorPentatonic:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(7) | intervalBit(9);
    case ScaleMode::MelodicMinor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::MinorPentatonic:
        return intervalBit(0) | intervalBit(3) | intervalBit(5) |
               intervalBit(7) | intervalBit(10);
    case ScaleMode::PhrygianDominant:
        return intervalBit(0) | intervalBit(1) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::WholeTone:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(6) | intervalBit(8) | intervalBit(10);
    case ScaleMode::None:
        return 0;
    case ScaleMode::Chromatic:
        return 0x0FFFu;
    }
    return 0;
}

inline bool isPitchClassInScale(ScaleMode mode, int pitchClass, int rootNote)
{
    if (mode == ScaleMode::None || rootNote < 0)
        return false;
    if (mode == ScaleMode::Chromatic)
        return true;

    const int normalizedPitch = (pitchClass % 12 + 12) % 12;
    const int normalizedRoot = (rootNote % 12 + 12) % 12;
    const int relativeSemitone = (normalizedPitch - normalizedRoot + 12) % 12;
    const auto intervalMask = getIntervalMaskForMode(mode);
    return (intervalMask & intervalBit(relativeSemitone)) != 0;
}

inline bool detectMajorOrRelativeMinor(const Project& project,
                                       int& detectedRootNote,
                                       ScaleMode& detectedMode)
{
    std::array<int, 12> pitchClassCounts {};
    int noteCount = 0;

    for (const auto& note : project.getNotes()) {
        if (note.isRest())
            continue;

        const int roundedMidi = static_cast<int>(std::round(note.getMidiNote()));
        const int pitchClass = (roundedMidi % 12 + 12) % 12;
        ++pitchClassCounts[static_cast<size_t>(pitchClass)];
        ++noteCount;
    }

    if (noteCount == 0)
        return false;

    int bestMajorRoot = 0;
    int bestFitCount = -1;
    for (int candidateRoot = 0; candidateRoot < 12; ++candidateRoot) {
        int fitCount = 0;
        for (int pitchClass = 0; pitchClass < 12; ++pitchClass) {
            if (isPitchClassInScale(ScaleMode::Major, pitchClass,
                                    candidateRoot))
                fitCount += pitchClassCounts[static_cast<size_t>(pitchClass)];
        }

        if (fitCount > bestFitCount) {
            bestFitCount = fitCount;
            bestMajorRoot = candidateRoot;
        }
    }

    const int relativeMinorRoot = (bestMajorRoot + 9) % 12;
    const int majorRootCount =
        pitchClassCounts[static_cast<size_t>(bestMajorRoot)];
    const int minorRootCount =
        pitchClassCounts[static_cast<size_t>(relativeMinorRoot)];

    if (minorRootCount > majorRootCount) {
        detectedRootNote = relativeMinorRoot;
        detectedMode = ScaleMode::Minor;
    } else {
        detectedRootNote = bestMajorRoot;
        detectedMode = ScaleMode::Major;
    }

    return true;
}

inline void detectAndApplyScale(Project& project)
{
    int rootNote = 0;
    ScaleMode mode = ScaleMode::Major;
    if (!detectMajorOrRelativeMinor(project, rootNote, mode))
        return;

    project.setDetectedKey(rootNote, mode);

    // Detection is a suggestion, not an explicit region override. Seed the
    // default once for standalone projects; the plugin adopts its first result.
    if (!project.getMacroParameters()->projectKeyInitialized)
        project.setProjectKey(rootNote, mode);
}

inline float getReferenceOffsetSemitones(int referenceHz)
{
    const int normalized = juce::jlimit(420, 450, referenceHz);
    return 12.0f * std::log2(static_cast<float>(normalized) / FREQ_A4);
}

inline float snapMidiToSemitone(float midi, int referenceHz = static_cast<int>(FREQ_A4))
{
    const float offset = getReferenceOffsetSemitones(referenceHz);
    return std::round(midi - offset) + offset;
}

inline float snapMidiToScale(float midi,
                             ScaleMode mode,
                             int rootNote,
                             int referenceHz = static_cast<int>(FREQ_A4))
{
    const float offset = getReferenceOffsetSemitones(referenceHz);
    const float normalizedMidi = midi - offset;
    const int roundedMidi = static_cast<int>(std::round(normalizedMidi));
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic || rootNote < 0)
        return snapMidiToSemitone(midi, referenceHz);

    constexpr int kSearchRadius = 24;
    constexpr float kTieEpsilon = 1.0e-4f;

    int bestMidi = roundedMidi;
    float bestDistance = std::numeric_limits<float>::max();
    int bestRoundedDistance = std::numeric_limits<int>::max();

    const int searchStart = roundedMidi - kSearchRadius;
    const int searchEnd = roundedMidi + kSearchRadius;
    for (int candidate = searchStart; candidate <= searchEnd; ++candidate) {
        if (!isPitchClassInScale(mode, candidate, rootNote))
            continue;

        const float distance = std::abs(static_cast<float>(candidate) - normalizedMidi);
        const int roundedDistance = std::abs(candidate - roundedMidi);
        const bool isCloser = distance < bestDistance - kTieEpsilon;
        const bool isDistanceTie = std::abs(distance - bestDistance) <= kTieEpsilon;
        const bool betterRoundedDistance =
            roundedDistance < bestRoundedDistance;
        const bool lowerPitchTie =
            roundedDistance == bestRoundedDistance && candidate < bestMidi;

        if (isCloser || (isDistanceTie && (betterRoundedDistance || lowerPitchTie))) {
            bestDistance = distance;
            bestRoundedDistance = roundedDistance;
            bestMidi = candidate;
        }
    }

    return static_cast<float>(bestMidi) + offset;
}

// Correct Pitch moves a segment toward the nearest semitone, or the nearest
// scale tone when "snap to scale" is on (inspector slider and smart control).
inline float correctionTargetFor(const Project& project, float sourceMidi, int referenceHz)
{
    if (project.getDragSnapMode() == DragSnapMode::Scale)
    {
        const auto mode = project.getPreferredScaleMode();
        const int root = project.getScaleRootNote();
        if (mode != ScaleMode::None && mode != ScaleMode::Chromatic && root >= 0)
            return snapMidiToScale(sourceMidi, mode, root, referenceHz);
    }
    return snapMidiToSemitone(sourceMidi, referenceHz);
}

// The Correct Pitch reference (Note::lastNonMacroMidiNote, "the sung pitch")
// after an edit other than Correct Pitch moved a segment from fromMidi to
// toMidi (harmony, arrow keys, drags, tilt, snap to semitone). The rule
// depends only on where the segment ends up, not on the path, so moving a
// segment away and back leaves it as it was. In order:
//  1. A move from one target pitch to another keeps the current reference's
//     deviation and correction share (whole-semitone moves, harmony).
//  2. The analysed pitch moved by whole semitones is the sung pitch whenever
//     the new pitch is a 0-100% correction of it (so a segment back at its
//     analysed pitch is back at 0%, unedited).
//  3. Otherwise the current deviation is kept around the target nearest to
//     the new pitch, if that reads as a 0-100% correction.
//  4. Otherwise, still around the same target, the reference stays; the
//     share is then outside 0-100% and shows as 0% (Correct Pitch starts
//     over from the current pitch when used).
//  5. Otherwise the new pitch becomes the sung pitch (0%).
inline float carriedCorrectionSource(const Project& project, float source,
                                     float analysedMidi, float fromMidi, float toMidi,
                                     int referenceHz)
{
    constexpr float kTolerance = 1.0e-3f;
    constexpr float kNoDeviation = 1.0e-4f;
    const auto targetOf = [&](float pitch)
    { return correctionTargetFor(project, pitch, referenceHz); };
    const auto inRange = [](float share)
    { return share >= -kTolerance && share <= 1.0f + kTolerance; };
    // Is `pitch` a 0-100% correction of the sung pitch `sung`?
    const auto corrects = [&](float sung, float pitch)
    {
        const float sungTarget = targetOf(sung);
        const float sungDeviation = sung - sungTarget;
        if (std::abs(sungDeviation) < kNoDeviation)
            return std::abs(pitch - sungTarget) < kTolerance;
        return inRange(1.0f - (pitch - sungTarget) / sungDeviation);
    };

    const float target = targetOf(source);
    const float deviation = source - target;
    const bool hasDeviation = std::abs(deviation) >= kNoDeviation;

    if (hasDeviation)
    {
        const float share = 1.0f - (fromMidi - target) / deviation;
        if (inRange(share))
        {
            const float movedTarget = toMidi - deviation * (1.0f - share);
            if (std::abs(targetOf(movedTarget) - movedTarget) < kTolerance &&
                std::abs(targetOf(movedTarget + deviation) - movedTarget) < kTolerance)
                return movedTarget + deviation;
        }
    }

    const float steps = std::round(toMidi - analysedMidi);
    for (const float n : {steps, steps - 1.0f, steps + 1.0f})
        if (corrects(analysedMidi + n, toMidi))
            return analysedMidi + n;

    if (hasDeviation)
    {
        const float nearestTarget = targetOf(toMidi);
        if (corrects(nearestTarget + deviation, toMidi) &&
            std::abs(targetOf(nearestTarget + deviation) - nearestTarget) < kTolerance)
            return nearestTarget + deviation;
    }

    if (std::abs(targetOf(toMidi) - target) < kTolerance)
        return source;

    return toMidi;
}

// Share (0-1) of the sung deviation that a pitch removes, or nullopt when the
// pitch is not a 0-100% correction of that sung pitch.
inline std::optional<float> correctionShare(const Project& project, float source,
                                            float midi, int referenceHz)
{
    const float target = correctionTargetFor(project, source, referenceHz);
    const float deviation = source - target;
    if (std::abs(deviation) < 1.0e-4f)
        return std::abs(midi - target) < 1.0e-3f ? std::optional<float>(0.0f) : std::nullopt;
    const float share = 1.0f - (midi - target) / deviation;
    if (share < -1.0e-3f || share > 1.0f + 1.0e-3f)
        return std::nullopt;
    return juce::jlimit(0.0f, 1.0f, share);
}
}
