// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "TrackIdentity.h"

juce::String documentTrackKey(const juce::String& documentKey,
                              const juce::String& trackName,
                              int trackOrdinal)
{
    // The document is the outer scope: two documents never share a key even if
    // their tracks are named the same.
    juce::String key = "doc:";
    key << documentKey;

    // A named track is identified by its name alone. Keeping the ordinal out
    // of a named key means the key survives a reorder as well as a recreation,
    // which is exactly what the guide store needs. The ordinal is the fallback
    // only when the host sends no name.
    const auto name = trackName.trim();
    if (name.isNotEmpty())
        return key + "|name:" + name;

    return key + "|ordinal:" + juce::String(trackOrdinal);
}
