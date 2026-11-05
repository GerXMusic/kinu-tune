// D-180: Pitchcore is not used for shifts beyond the naturalness cut.
#include "../Source/Audio/Synthesis/EngineBlocks.h"
#include "../Source/Audio/Synthesis/LargeShiftEnginePolicy.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace pitchnet;

int main()
{
    // Up to the cut (and its negative mirror) the request is unchanged.
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, 0.0f, true) ==
          SynthesisEngineType::Pitchcore);
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, 100.0f, true) ==
          SynthesisEngineType::Pitchcore);
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, 150.0f, true) ==
          SynthesisEngineType::Pitchcore);
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, -150.0f, true) ==
          SynthesisEngineType::Pitchcore);

    // Beyond the cut the original vocoder takes over, both directions.
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, 400.0f, true) ==
          SynthesisEngineType::Vocoder);
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, -500.0f, true) ==
          SynthesisEngineType::Vocoder);
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, 150.01f, true) ==
          SynthesisEngineType::Vocoder);

    // Other engines are never rewritten by this rule.
    CHECK(engineForPitchShift(SynthesisEngineType::Psola, 500.0f, true) ==
          SynthesisEngineType::Psola);
    CHECK(engineForPitchShift(SynthesisEngineType::Vocoder, 500.0f, true) ==
          SynthesisEngineType::Vocoder);

    // Without the vocoder model the request is left as-is: a missing model
    // must not turn a large shift into silence.
    CHECK(engineForPitchShift(SynthesisEngineType::Pitchcore, 500.0f, false) ==
          SynthesisEngineType::Pitchcore);

    // Integration: a large-shift note with a real silence before it starts a
    // vocoder block, while the untouched note keeps Pitchcore.
    {
        constexpr int sampleRate = 48000;
        constexpr int hopSize = 240;
        juce::AudioBuffer<float> source(1, sampleRate);
        source.clear();
        auto *samples = source.getWritePointer(0);
        for (int i = 0; i < sampleRate / 10; ++i)
            samples[i] = 0.2f * std::sin(2.0f * 3.14159265f * 220.0f * i /
                                         sampleRate);
        for (int i = sampleRate / 2; i < sampleRate * 6 / 10; ++i)
            samples[i] = 0.2f * std::sin(2.0f * 3.14159265f * 330.0f * i /
                                         sampleRate);

        std::vector<Note> notes;
        Note untouched(0, 20, 60.0f);
        Note shifted(60, 80, 64.0f); // +400 cents from its original 60.
        shifted.setOriginalMidiNote(60.0f);
        notes.push_back(untouched);
        notes.push_back(shifted);

        const auto plan = engineBlocks::buildPlan(
            notes, 0, 100, SynthesisEngineType::Pitchcore, source, sampleRate,
            hopSize, true);
        CHECK(plan.size() >= 2);
        CHECK(plan.front().engine == SynthesisEngineType::Pitchcore);
        CHECK(plan.back().engine == SynthesisEngineType::Vocoder);
    }

    std::cout << "Large-shift engine policy tests passed\n";
    return 0;
}
