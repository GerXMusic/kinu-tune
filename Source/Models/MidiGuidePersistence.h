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
 * Identity: ARA exposes no host project path (no .rpp name, no project folder),
 * so the closest stable stand-in is the set of audio sources of the ARA
 * document - their persistent IDs are the media paths the host restores with
 * the project. midiGuideDisk::projectKeyFromSources() hashes that set, and the
 * track key the store already uses (document + track name + ordinal, D-358)
 * completes the key. Two projects with different media never share a file; two
 * tracks of one project never share a file.
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

// Deterministic fingerprint of one ARA document from its audio sources'
// persistent IDs. Order-independent; empty input gives an empty key.
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
    // could not be written; the previous copy is left untouched.
    bool save(const juce::String &projectKey, const juce::String &trackKey,
              const std::vector<GuideSource> &guides,
              juce::String *error = nullptr);

    MidiGuideLoadResult load(const juce::String &projectKey,
                             const juce::String &trackKey) const;

private:
    juce::File directory;
};
