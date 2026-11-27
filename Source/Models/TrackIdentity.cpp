// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "TrackIdentity.h"

#include <algorithm>
#include <utility>

namespace
{
// FNV-1a 64, the same hash the ARA diagnostics use. This copy only has to name
// a sidecar file and make two different source sets differ; the keys inside
// the file are what validate it.
juce::String fnv1aHex(const juce::String &text)
{
    juce::uint64 hash = 1469598103934665603ull;
    const auto utf8 = text.toUTF8();
    if (const char *p = utf8.getAddress())
        for (; *p != '\0'; ++p)
        {
            hash ^= static_cast<unsigned char>(*p);
            hash *= 1099511628211ull;
        }
    return juce::String::toHexString(static_cast<juce::int64>(hash))
        .toLowerCase();
}
} // namespace

juce::String stableSourceFingerprint(std::vector<juce::String> sourceIdentities)
{
    for (auto &identity : sourceIdentities)
        identity = identity.trim();

    sourceIdentities.erase(
        std::remove_if(sourceIdentities.begin(), sourceIdentities.end(),
                       [](const juce::String &id) { return id.isEmpty(); }),
        sourceIdentities.end());
    if (sourceIdentities.empty())
        return {};

    // The host may hand the sources in any order; the fingerprint must not
    // depend on it. Sorting also makes the duplicate removal below exact.
    std::sort(sourceIdentities.begin(), sourceIdentities.end());
    sourceIdentities.erase(
        std::unique(sourceIdentities.begin(), sourceIdentities.end()),
        sourceIdentities.end());

    juce::String seed("tunex-ara-track-sources-v2");
    for (const auto &identity : sourceIdentities)
        seed << '\n' << identity;
    return fnv1aHex(seed);
}

juce::String trackKeyFromSources(std::vector<juce::String> sourceIdentities,
                                 const juce::String &trackName,
                                 int trackOrdinal)
{
    // R11b (D-365): the fingerprint of the track's own media replaces the
    // document address. Only data that the host restores with the project is
    // allowed in here; Models/TrackIdentity.cpp is guarded against
    // reinterpret_cast / uintptr_t / getDocument for exactly that reason.
    const auto fingerprint = stableSourceFingerprint(std::move(sourceIdentities));

    juce::String key;
    key << "src:" << (fingerprint.isNotEmpty() ? fingerprint : juce::String("none"));

    // A named track is identified by its name alone, so the key survives a
    // reorder. The ordinal is the fallback only when the host sends no name.
    const auto name = trackName.trim();
    if (name.isNotEmpty())
        return key + "|name:" + name;

    return key + "|ordinal:" + juce::String(trackOrdinal);
}
