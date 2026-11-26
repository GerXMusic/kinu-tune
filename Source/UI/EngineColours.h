#pragma once

#include "../Audio/SynthesisEngineType.h"
#include "../JuceHeader.h"

/**
 * D-309: one colour per engine, used by the notes' block border and by the
 * dots in the engine drawer. Only the engine drawer shows them, so a note's
 * usual look does not change while the drawer is closed.
 *
 * The palette is the one agreed for 0.2.0:
 *   Clasico (PSOLA) #4FA8FF, Vocoder #FF9F43, Pitchcore #FFD84D.
 * `underlineFor` is the dark thin stroke drawn under the colour so a bright
 * border still reads on the steel-blue note body.
 */
namespace engineColours {

inline juce::Colour forEngine(SynthesisEngineType engine)
{
    switch (engine)
    {
    case SynthesisEngineType::Vocoder:   return juce::Colour(0xFFFF9F43u);
    case SynthesisEngineType::Pitchcore: return juce::Colour(0xFFFFD84Du);
    case SynthesisEngineType::Psola:
    default:                             return juce::Colour(0xFF4FA8FFu);
    }
}

inline juce::Colour underlineFor(SynthesisEngineType engine)
{
    return forEngine(engine).darker(0.62f);
}

/// Neutral colour of a sweep that reaches more than one visual block.
inline juce::Colour neutralSweep()
{
    return juce::Colours::white.withAlpha(0.88f);
}

} // namespace engineColours
