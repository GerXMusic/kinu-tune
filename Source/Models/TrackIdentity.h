// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../JuceHeader.h"

#include <vector>

/**
 * Stable, persistent identity of one ARA track for the document's guide store
 * (R5d D-358, reworked in R11b D-365).
 *
 * The key used to be the ADDRESS of the ARA::PlugIn::RegionSequence, and then
 * the address of the ARA::PlugIn::Document plus the track name. The sequence
 * address survived nothing (R5c), and the document address survives neither
 * closing nor reopening the project: measured in R11 with the build 6EF011EA,
 * `doc:75e02210` became `doc:75dd3060` (no save) and `doc:75c709f0` (with
 * save), so the sidecar was written under a key that never came back: 6
 * orphan files, `guide_count=0`.
 *
 * What survives closing and reopening is what ARA hands over as DATA, never as
 * an object identity:
 *  - the audio sources of the track, named by the host (the media the project
 *    restores). The identity is the source NAME ARA exposes, not its
 *    persistent ID: measured in the same session, `projectKeyFromSources()`
 *    built from the persistent IDs produced three different fingerprints
 *    (`7fe16568...`, `b22f0ae4...`, `2616d898...`) for the same media. The
 *    persistent ID is a reconnection token, not a file identity.
 *  - the track name, which the host restores with the project.
 *
 * The key never contains an address, a pointer, or an ordinal when a name is
 * available: `src:<fingerprint>|name:<track name>`. The ordinal is the last
 * resort for hosts that send neither a name nor sources, and it is the
 * position in ARADocument::getRegionSequences() - documented, not preferred.
 *
 * Deliberate risks (D-365):
 *  - moving the project folder or renaming the media changes the fingerprint,
 *    so the sidecar is no longer found (the guide is kept on disk; nothing is
 *    deleted).
 *  - renaming the track changes the key: same consequence, documented.
 *  - two tracks of one document with the same name and the same sources are
 *    not distinguished. SCOPE=A gives one track per plug-in instance, so this
 *    is the same accepted limitation as D-358.
 */

// Deterministic fingerprint of the track's audio sources from their stable
// identities (the source names ARA exposes). Order- and duplicate-independent;
// an empty input gives an empty string.
juce::String stableSourceFingerprint(std::vector<juce::String> sourceIdentities);

// Key of one track: sources first, then the name. Never an address.
juce::String trackKeyFromSources(std::vector<juce::String> sourceIdentities,
                                 const juce::String &trackName,
                                 int trackOrdinal);
