// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../JuceHeader.h"
#include "MidiGuide.h"

#include <string>
#include <vector>

/**
 * Where the document's MIDI guides live on disk (R7 of the guide-integration
 * fix, D-360).
 *
 * The ARA archive is NOT the carrier. Measured in D-346: `saved_notes = -1` in
 * all five regions and `copyProjectArchive` empty, so the host is not reliably
 * writing (or not handing back) the plug-in's per-modification JSON. Putting the
 * guide there would make it disappear exactly when the user reopens the
 * project - the one thing this phase exists to fix. The guide therefore goes to
 * a SIDECAR file next to the user's application data, and the host archive
 * keeps carrying the old `midiGuides` copy for compatibility (one version).
 *
 * Identity (R11b D-365, R11d D-367, R11f D-370): ARA exposes no host project
 * path (no .rpp name, no project folder), and it exposes no persistent file
 * path either: measured in R11, the audio source persistent IDs changed on
 * every reopen and produced three different document fingerprints for the same
 * media. What survives is the source NAME the host restores with the project.
 * The track key is the track NAME alone (Models/TrackIdentity.h); no address,
 * pointer or ordinal of the session is part of it, and neither is the track's
 * item list: adding, removing or moving items must not rename the sidecar
 * (SCOPE=A, G-B2 in R11c).
 *
 * ONE PATH PER TRACK (R11f, D-370): the FILE NAME is the track key and nothing
 * else. It used to hash the document fingerprint (the set of source names of
 * the document) together with the track key, so every view of the document -
 * an item added, a source restored - hashed to a DIFFERENT path: the guide
 * "jumped" from file to file and the second view change of R12 landed on an
 * empty one (1/111 -> 0/0 measured on the B2 build E2C56ECF). The document
 * fingerprint is still recorded INSIDE the file (which view wrote it last,
 * useful for a support report) but it is neither part of the path nor
 * validated on read: a guide belongs to a track, not to a view of a document.
 * Two documents whose track has the same name share the file; that is the
 * accepted risk of D-370 (two homonymous tracks in two different documents).
 *
 * Legacy migration: a sidecar written before R11b carries a track key that
 * starts with "doc:" (the ARADocument address) and one written before R11d
 * starts with "src:" (the fingerprint of the track's live items). When the
 * path of the current key has no file, load() adopts those files for the same
 * track (same name after "name:", or the same ordinal), merges them by guide
 * id, rewrites them under the current key and renames each superseded file to
 * "<file>.bak". Nothing is deleted; the bytes stay for the user and only one
 * live file per track remains. The canonical files R11d/R11e left at their
 * old (per-view) paths are adopted the same way, because a track's file moved
 * in R11f: that is the consolidation, and it is what turns N per-view files
 * into one.
 *
 * Write: temporary sibling + atomic replace (juce::TemporaryFile ->
 * replaceFileIn), so a reader never sees half a file and a crash leaves the
 * previous copy intact (at worst an orphan `*.tmp`, ignored by load()).
 *
 * Read: validated whole-file. A file that does not parse, whose format is
 * newer, or whose keys are not the ones asked for is REJECTED (not "empty"),
 * the caller is told why and falls back to the legacy Project copy.
 */
namespace midiGuideDisk
{
inline constexpr int kFormatVersion = 1;
inline constexpr const char* kFileExtension = ".json";

// %APPDATA%/Keinoaza/TuneX/guides/projects (PlatformPaths already names the
// product folder; the live import uses guides/live next to it).
juce::File defaultDirectory();

// Deterministic fingerprint of one ARA document from its audio sources' stable
// identities (the names ARA exposes), never from their persistent IDs.
// Order-independent; empty input gives an empty key. It is recorded INSIDE the
// sidecar as "which view wrote it last" (D-370); it never names a file.
juce::String projectKeyFromSources(std::vector<juce::String> sourceIds);

// One file per TRACK, and only the track key names it (D-370): the same track
// gets the same path in every view of the document, so a view change reads the
// file it already has instead of creating a second one.
juce::File fileFor(const juce::File &directory, const juce::String &trackKey);
} // namespace midiGuideDisk

struct MidiGuideLoadResult
{
    enum class Status
    {
        none,     // no file: the caller migrates from Project::midiGuides
        loaded,   // usable, guides may be empty (authoritative)
        rejected  // present but unusable: message says why
    };

    Status status = Status::none;
    std::vector<GuideSource> guides;
    juce::String message;

    bool found() const noexcept { return status != Status::none; }
    bool usable() const noexcept { return status == Status::loaded; }
};

class MidiGuidePersistence
{
public:
    // Empty directory = midiGuideDisk::defaultDirectory(). Nothing touches the
    // disk until save()/load(), so constructing one has no side effects.
    explicit MidiGuidePersistence(juce::File directory = {});

    juce::File getDirectory() const;
    juce::File fileFor(const juce::String &trackKey) const;

    // Atomic replace of the track's single file. `documentViewKey` is recorded
    // in the file but never used for the path (D-370): a save from another
    // view of the document is the same file. Returns false (and fills error
    // when given) if the file could not be written; the previous copy is left
    // untouched. On success every OTHER live file of the same track (the
    // "doc:" / "src:...|name:" forms and the per-view paths R11d/R11e wrote) is
    // retired to .bak, so a track never ends up with two live files.
    bool save(const juce::String &documentViewKey, const juce::String &trackKey,
              const std::vector<GuideSource> &guides,
              juce::String *error = nullptr);

    // Reads the track's file. The document view is informational: a file whose
    // recorded view differs is this track's file (D-370) and is accepted. When
    // the path has no file yet, adopts every file of the same track written
    // before (superseded key forms and canonical files at the old per-view
    // paths), merges them by guide id, materialises them under the canonical
    // path and renames each adopted file to .bak. This is the one-time
    // migration of older builds and the consolidation of the per-view files.
    MidiGuideLoadResult load(const juce::String &documentViewKey,
                             const juce::String &trackKey) const;

private:
    bool writeGuides(const juce::String &documentViewKey,
                     const juce::String &trackKey,
                     const std::vector<GuideSource> &guides,
                     juce::String *error) const;
    void retireOtherFilesOfTrack(const juce::String &trackKey,
                                 const juce::File &keep) const;

    juce::File directory;
};
