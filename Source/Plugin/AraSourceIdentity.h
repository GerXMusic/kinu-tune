// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../JuceHeader.h"

#include <vector>

/**
 * Stable identity of the ARA model objects that survives closing and reopening
 * the project (R11b, D-365).
 *
 * The guide store used to key its sidecars on `getPersistentID()`, then on the
 * ARADocument address. Measured in R11 with the build 6EF011EA: the persistent
 * IDs of the same two media produced three different document fingerprints
 * (`7fe16568...`, `b22f0ae4...`, `2616d898...`) across sessions, and the
 * document address changed on every reopen. Neither is a file identity.
 *
 * ARA does not expose a file path on ARAAudioSource; the stable datum the host
 * restores with the project is the source NAME (`ARAAudioSourceProperties::
 * name`, normally the media file name). The DOCUMENT key is the fingerprint of
 * those names (R11b, D-365). The TRACK key is the track name alone since R11d
 * (D-367), because the track's source list changes with its items. When a host
 * sends no source name, the geometry (sample rate, channels, samples) is the
 * fallback - still data, still not an address - and a source with neither
 * contributes nothing.
 */
namespace araIdentity
{
inline juce::String sourceIdentity(const juce::ARAAudioSource &source)
{
    const auto name = juce::String(source.getName()).trim();
    if (name.isNotEmpty())
        return "name:" + name;

    juce::String geometry("geometry:");
    geometry << juce::String(source.getSampleRate(), 6) << 'x'
             << source.getChannelCount() << 'x'
             << juce::String(source.getSampleCount());
    return geometry;
}

// Sources of one document (all of them; the document key uses this set).
inline std::vector<juce::String>
documentSourceIdentities(const juce::ARADocument &document)
{
    std::vector<juce::String> identities;
    for (auto *source : document.getAudioSources<juce::ARAAudioSource>())
        if (source != nullptr)
        {
            const auto identity = sourceIdentity(*source);
            if (identity.isNotEmpty())
                identities.push_back(identity);
        }
    return identities;
}

// Sources of one track, read through the playback regions of its sequence.
// Since R11d (D-367) the track key does NOT use this: it is the list of the
// track's live items, and moving, adding or removing one item renamed the
// sidecar (R11c G-B2). Kept for diagnostics; do not feed it to a key again.
inline std::vector<juce::String>
sequenceSourceIdentities(const juce::ARARegionSequence &sequence)
{
    std::vector<juce::String> identities;
    for (auto *region : sequence.getPlaybackRegions<juce::ARAPlaybackRegion>())
    {
        if (region == nullptr)
            continue;
        const auto *modification = region->getAudioModification();
        if (modification == nullptr)
            continue;
        const auto *source = modification->getAudioSource();
        if (source == nullptr)
            continue;
        const auto identity = sourceIdentity(*source);
        if (identity.isNotEmpty())
            identities.push_back(identity);
    }
    return identities;
}
} // namespace araIdentity
