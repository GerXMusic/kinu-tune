#include "Note.h"
#include "../Utils/Constants.h"
#include <cmath>

Note::Note(int startFrame, int endFrame, float midiNote)
    : srcStartFrame(startFrame), srcEndFrame(endFrame),
      startFrame(startFrame), endFrame(endFrame), midiNote(midiNote),
      lastNonMacroMidiNote(midiNote), originalMidiNote(midiNote),
      renderedStartFrame(startFrame), renderedEndFrame(endFrame)
{
}

std::vector<float> Note::getAdjustedF0() const
{
    if (f0Values.empty() || pitchOffset == 0.0f)
        return f0Values;

    // Convert semitone offset to frequency ratio
    float ratio = std::pow(2.0f, pitchOffset / 12.0f);

    std::vector<float> adjusted;
    adjusted.reserve(f0Values.size());

    for (float f0 : f0Values)
    {
        if (f0 > 0.0f)
            adjusted.push_back(f0 * ratio);
        else
            adjusted.push_back(0.0f);
    }

    return adjusted;
}

std::vector<float> Note::computeF0FromDelta() const
{
    int numFrames = endFrame - startFrame;
    std::vector<float> result(numFrames, 0.0f);
    const auto& activeDelta = getActiveDeltaPitch();

    for (int i = 0; i < numFrames; ++i)
    {
        // Get delta pitch for this frame (or 0 if not available)
        float delta = (i < static_cast<int>(activeDelta.size())) ? activeDelta[i] : 0.0f;

        // Actual MIDI = render center + delta. D-303: el motor usa el centro de
        // render, no el visible, para que un corte no mueva el audio.
        float actualMidi = getRenderAdjustedMidiNote() + delta;

        // Convert to Hz
        result[i] = midiToFreq(actualMidi);
    }

    return result;
}

bool Note::containsFrame(int frame) const
{
    return frame >= startFrame && frame < endFrame;
}

bool Note::isNeutralForOriginalWaveform() const
{
    constexpr float kPitchEpsilon = 0.001f;
    constexpr float kGainEpsilon = 0.001f;

    return ! muted &&
           startFrame == srcStartFrame && endFrame == srcEndFrame &&
           // D-303: la neutralidad se mide contra el centro de render; el
           // pitchOffset se comprueba aparte justo debajo.
           std::abs(getRenderMidiNote() - originalMidiNote) <= kPitchEpsilon &&
           !hasBakedDeltaPitch() &&
           std::abs(pitchOffset) <= kPitchEpsilon &&
           std::abs(formantShift) <= kPitchEpsilon &&
           std::abs(volumeDb) <= kGainEpsilon &&
           std::abs(tiltLeft) <= kPitchEpsilon &&
           std::abs(tiltRight) <= kPitchEpsilon &&
           std::abs(vibrato - 1.0f) <= kPitchEpsilon &&
           std::abs(pitchDrift - 1.0f) <= kPitchEpsilon &&
           smoothLeftFrames == 0 &&
           smoothRightFrames == 0 &&
           transitionOutMs == 0 &&
           std::abs(deltaScale - 1.0f) <= kPitchEpsilon &&
           std::abs(deltaOffset) <= kPitchEpsilon;
}
