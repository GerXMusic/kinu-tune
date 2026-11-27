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
 * Identity (R11b D-365, R11d D-367): ARA exposes no host project path (no .rpp
 * name, no project folder), and it exposes no persistent file path either:
 * measured in R11, the audio source persistent IDs changed on every reopen and
 * produced three different document fingerprints for the same media. What
 * survives is the source NAME the host restores with the project. The document
 * key is the fingerprint of those names and the track key is the track NAME
 * alone (Models/TrackIdentity.h); no address, pointer or ordinal of the session
 * is part of either, and neither is the track's item list: adding, removing or
 * moving items must not rename the sidecar (SCOPE=A, G-B2 in R11c). Two
 * projects with different media never share a file; two tracks of one project
 * never share a file.
 *
 * Legacy migration: a sidecar written before R11b carries a track key that
 * starts with "doc:" (the ARADocument address) and one written before R11d
 * starts with "src:" (the fingerprint of the track's live items). When the
 * current key has no file, load() adopts those files for the same track (same
 * name after "name:", or the same ordinal), merges them by guide id, rewrites
 * them under the current key and renames each superseded file to "<file>.bak".
 * Nothing is deleted; the bytes stay for the user and only one live file per
 * track remains.
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
// Order-independent; empty input gives an empty key.
juce::String projectKeyFromSources(std::vector<juce::String> sourceIds);

// One file per (document fingerprint, track key).
juce::File fileFor(const juce::File &directory, const juce::String &projectKey,
                   const juce::String &trackKey);
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
    juce::File fileFor(const juce::String &projectKey,
                       const juce::String &trackKey) const;

    // Atomic replace. Returns false (and fills error when given) if the file
    // could not be written; the previous copy is left untouched. On success
    // the superseded ("doc:" / "src:...|name:") sidecars of the same track are
    // retired to .bak, so a track never ends up with two live keys (D-365).
    bool save(const juce::String &projectKey, const juce::String &trackKey,
              const std::vector<GuideSource> &guides,
              juce::String *error = nullptr);

    // Reads the file for the key. When that file does not exist yet, adopts
    // the sidecars of the same track written in a superseded form ("doc:" and
    // "src:...|name:"), materialises them under the key and returns them
    // (status loaded), so a project opened after a key change - or after the
    // R11d fix - recovers the guide instead of starting empty. A canonical
    // file of another document view is only adopted when it is the single
    // candidate (D-367).
    MidiGuideLoadResult load(const juce::String &projectKey,
                             const juce::String &trackKey) const;

private:
    bool writeGuides(const juce::String &projectKey,
                     const juce::String &trackKey,
                     const std::vector<GuideSource> &guides,
                     juce::String *error) const;
    void retireSupersededFiles(const juce::String &trackKey,
                               const juce::File &keep) const;

    juce::File directory;
};
