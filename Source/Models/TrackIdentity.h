// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../JuceHeader.h"

#include <vector>

/**
 * Stable, persistent identity of one ARA track for the document's guide store
 * (R5d D-358, reworked in R11b D-365 and in R11d D-367).
 *
 * The key used to be the ADDRESS of the ARA::PlugIn::RegionSequence, then the
 * address of the ARA::PlugIn::Document plus the track name, and then the
 * fingerprint of the track's audio sources plus the name. Each of the first two
 * survived nothing (R5c, R11). The third survived closing and reopening the
 * project (G-E3 PASS in R11c) but NOT a change in the track's item list: moving
 * the four items that were not the target to another track took the key from
 * `src:7221217c41993ac6|name:v2 - stem` to `src:782bf3636937c799|name:v2 - stem`
 * and the store went to `guide_count=0` / `ghost_count=0` (G-B2 FAIL). Any
 * added, removed or moved audio on the track did the same, which contradicts
 * SCOPE=A: the guide belongs to the whole track, all its items and sources.
 *
 * The track key is therefore the track NAME alone - the datum the host restores
 * with the project and shows to the user:
 *
 *     name:<track name>
 *
 * The item list, the audio sources and the position in the document play no
 * part. The document fingerprint (MidiGuidePersistence's projectKey, still
 * derived from the document's source names) is recorded inside the sidecar as
 * "which view wrote it last"; since R11f (D-370) it is NOT part of the file
 * name: one path per track, so a change of view reads the file it already has.
 *
 * Fallbacks and risks (D-367, D-370):
 *  - a track with no name falls back to `ordinal:<n>`, its position in
 *    ARADocument::getRegionSequences(). Documented, not preferred: reordering
 *    nameless tracks renames the key.
 *  - renaming the track changes the key: the old sidecar is not deleted, it
 *    simply stops being found (same as D-365).
 *  - two tracks with the same name are one key AND one file (D-370). Inside
 *    one document SCOPE=A gives one track per plug-in instance, the same
 *    accepted limitation as D-358/D-365; in two DIFFERENT documents with a
 *    homonymous track both instances read and write that one file, so the last
 *    writer wins and the other document adopts the same guide. Accepted and
 *    documented: the alternative - hashing the document view into the path -
 *    is the R12 bug (the guide jumped to an empty file on the second change).
 */

// Deterministic fingerprint of the document's audio sources from their stable
// identities (the source names ARA exposes). Not part of the track key; it is
// the document half of the sidecar name. Order- and duplicate-independent; an
// empty input gives an empty string.
juce::String stableSourceFingerprint(std::vector<juce::String> sourceIdentities);

// Key of one track: its name, or the ordinal when the host sends none. Never an
// address, never the sources, never the number of items.
juce::String trackKeyForTrack(const juce::String &trackName, int trackOrdinal);
