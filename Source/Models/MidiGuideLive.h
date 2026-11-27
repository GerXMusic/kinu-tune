// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../JuceHeader.h"
#include "MidiGuide.h"

#include <map>
#include <string>
#include <vector>

/**
 * Live guides on disk (R10 of the guide-integration fix, D-363).
 *
 * The ReaScript drops one JSON snapshot per track in
 * %APPDATA%/TuneX/guides/live; this module is the only reader of that folder
 * and answers three questions the sync needs:
 *
 *  - IDENTITY. Before R10 the id was `reaper:<track GUID>:<item position>:
 *    <take>`, so moving the item produced a NEW id and the re-run left the old
 *    guide alive next to the new one (two guides for one track). The identity
 *    is now `reaper:<track GUID>:<take>`: the position is gone, and the take
 *    key (the take name, which REAPER keeps across a move and a split) names
 *    the logical source inside the track. Old positional ids are not adopted:
 *    they are recognised (isLegacyReaperSourceId) and ignored, so a stale file
 *    cannot resurrect a duplicate even when the script was not re-run.
 *
 *  - CHANGE. The sync compares a CONTENT HASH (start, end, pitch, velocity of
 *    every note), never the note count: moving a note without changing how
 *    many there are is a real change and must replace the guide. The hash is
 *    also what the editor remembers as "last applied" (lastAppliedHash), so
 *    the panel can say whether the guide changed since it was applied.
 *
 *  - COST. Scanning is polled. mtime + size of each file is the fast path: a
 *    file that did not change is not parsed again, only its cached notes are
 *    reused. The cache is keyed by path and pruned when a file disappears.
 */
namespace midiGuideLive
{
// FNV-1a 64 over the bit pattern of every start, end, pitch and velocity, in
// store order. Stable for a given note list; two different note lists are not
// expected to collide (a 64-bit hash is enough for a poll decision).
juce::String contentHash(const GuideSource& guide);

// Canonical live id: no position, no timestamp. `takeKey` is the take name
// (or "midi" when it is empty); ':' is replaced so the three-field shape of the
// id survives any take name. ReaScript mirrors this format exactly.
juce::String stableSourceId(const juce::String& trackGuid,
                            const juce::String& takeKey);

// True for the pre-R10 shape `reaper:<GUID>:<position>:<take>`, where the
// third field is a number. Those files are ignored by the scanner.
bool isLegacyReaperSourceId(const juce::String& sourceId);

// What the panel shows for one guide: coverage in host seconds (the notes are
// in quarters, the tempo map is the host's) and whether the notes differ from
// the hash the user last applied. An empty lastAppliedHash means "never
// applied": no warning, by design.
struct Status
{
    int notes = 0;
    double hostStartSeconds = 0.0;
    double hostEndSeconds = 0.0;
    juce::String updatedAt;
    juce::String hash;
    bool changedSinceApply = false;
};

Status statusFor(const GuideSource& guide, const HostTempoMap& tempo,
                 const juce::String& lastAppliedHash);

// Polls one live folder. Owns the mtime/size cache, so create one per editor
// (or per document) and keep it between syncs.
class LiveDirectoryScanner
{
public:
    struct Result
    {
        // False when the folder does not exist (first run, another machine):
        // the caller must then NOT treat the list as authoritative, or an
        // empty scan would delete guides that were loaded from the sidecar.
        bool directoryExists = false;
        std::vector<GuideSource> guides;
        int rejected = 0; // file present but unreadable/invalid
        int ignored = 0;  // pre-R10 positional id
        int reRead = 0;   // files parsed this scan
        int reused = 0;   // files served from the mtime+size cache
    };

    Result scan(const juce::File& directory);
    void reset() { cache.clear(); }

private:
    struct Cached
    {
        juce::int64 modified = 0;
        juce::int64 size = -1;
        GuideSource source;
    };

    std::map<juce::String, Cached> cache;
};
} // namespace midiGuideLive
