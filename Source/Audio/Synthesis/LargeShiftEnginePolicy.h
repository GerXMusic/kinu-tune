#pragma once

#include "../SynthesisEngineType.h"

#include <cmath>

namespace pitchnet
{
// D-180: the user's product rule for large pitch moves. Pitchcore's classic
// TD-PSOLA loses naturalness (spectrum and formants) beyond roughly 100-150
// cents; the plug-in's original vocoder is the better engine there. D-172
// measured Pitchcore at +148.49 cents, so the cut stays above that run:
// 150 cents, and only the sign-independent magnitude matters.
inline constexpr float kPitchcoreMaxShiftCents = 150.0f;

// Requests below the cut keep the requested engine. A Pitchcore request above
// it becomes Vocoder when the vocoder is loaded; without the model the request
// is left untouched so a missing vocoder cannot silence the note.
inline SynthesisEngineType engineForPitchShift(
    SynthesisEngineType requested, float shiftCents, bool vocoderAvailable)
{
#if defined(PITCHNET_D180_LEGACY)
    // D-180 measurement only: pre-fix engine choice ignored the shift.
    (void)shiftCents;
    (void)vocoderAvailable;
    return requested;
#else
    if (requested == SynthesisEngineType::Pitchcore && vocoderAvailable &&
        std::abs(shiftCents) > kPitchcoreMaxShiftCents)
        return SynthesisEngineType::Vocoder;
    return requested;
#endif
}
} // namespace pitchnet
