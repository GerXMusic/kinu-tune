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

    // The seed text is kept byte for byte from R11b on purpose: the document
    // key is stored inside every sidecar and names its file, so changing it
    // would orphan the files already on disk for nothing.
    juce::String seed("tunex-ara-track-sources-v2");
    for (const auto &identity : sourceIdentities)
        seed << '\n' << identity;
    return fnv1aHex(seed);
}

juce::String trackKeyForTrack(const juce::String &trackName, int trackOrdinal)
{
    // R11d (D-367): the NAME is the whole key. R11b also mixed in the
    // fingerprint of the track's sources, which is the list of its live items:
    // moving, adding or removing one item renamed the sidecar (G-B2 FAIL). A
    // track's name is what the host restores with the project and what the user
    // sees; the item list is not an identity.
    const auto name = trackName.trim();
    if (name.isNotEmpty())
        return "name:" + name;

    // Last resort: the position in the document's sequence list. Documented,
    // not preferred (TrackIdentity.h).
    return "ordinal:" + juce::String(trackOrdinal);
}
