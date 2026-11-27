// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
//
// The checks of D-239 shared by the tests that measure a rendered block:
// CommitSeamTests (the commit unit is the engine block) and BatchRenderTests
// (N5: a batch asks for one render per item and none of them stays mute).
//
// They are the measuring instruments, not a criterion: each test picks its own
// limit and its own window.

#pragma once

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>

namespace commitSeamChecks
{
inline float midiHz(float midi)
{
    return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

// Worst absolute difference of the two buffers inside [firstSample, lastSample).
inline float maxAbsDiff(const juce::AudioBuffer<float> &a,
                        const juce::AudioBuffer<float> &b, int firstSample,
                        int lastSample)
{
    float worst = 0.0f;
    const int n = std::min({a.getNumSamples(), b.getNumSamples(), lastSample});
    for (int i = std::max(0, firstSample); i < n; ++i)
        worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
    return worst;
}

// Worst absolute difference outside [firstSample, lastSample): what the render
// must leave bit for bit as it was.
inline float maxAbsDiffOutside(const juce::AudioBuffer<float> &a,
                               const juce::AudioBuffer<float> &b,
                               int firstSample, int lastSample)
{
    float worst = 0.0f;
    const int n = std::min(a.getNumSamples(), b.getNumSamples());
    for (int i = 0; i < firstSample; ++i)
        worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
    for (int i = std::max(firstSample, lastSample); i < n; ++i)
        worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
    return worst;
}

// Peak absolute sample of one window: the "is it silent" measure.
inline float peakAbs(const juce::AudioBuffer<float> &buffer, int firstSample,
                     int lastSample)
{
    float peak = 0.0f;
    const int n = std::min(buffer.getNumSamples(), lastSample);
    for (int i = std::max(0, firstSample); i < n; ++i)
        peak = std::max(peak, std::abs(buffer.getSample(0, i)));
    return peak;
}

// One incremental pass, run on the message thread like the editor does.
inline bool renderAndWait(Project &project, SynthesisEngineType engine,
                          const char *label)
{
    IncrementalSynthesizer synthesizer;
    synthesizer.setProject(&project);
    synthesizer.setSynthesisEngine(engine);

    std::atomic<bool> finished{false};
    std::atomic<bool> success{false};
    synthesizer.synthesizeRegion(
        nullptr, [&](bool ok)
        {
            success.store(ok);
            finished.store(true);
        });

    const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
    while (!finished.load() &&
           juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    if (!finished.load())
    {
        std::cout << label << ": render did not finish in 30 s\n";
        return false;
    }
    return success.load();
}
} // namespace commitSeamChecks
