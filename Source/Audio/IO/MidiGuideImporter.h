// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../Models/MidiGuide.h"
#include "../../JuceHeader.h"

#include <vector>

/**
 * Reads DJ/DAW MIDI guides into GuideSource (D-311).
 *
 * Two inputs:
 *  - a .mid file: every track merged, timestamps converted to quarter notes
 *    with the file's own PPQ. The file's tempo is deliberately ignored: the
 *    guide is a *pitch over the song*, so a beat-to-beat reference must follow
 *    the host tempo map, not the tempo the MIDI file was exported with.
 *    SMPTE-timestamped files are rejected (they would need a map we do not
 *    have at import time).
 *  - live JSON snapshots written by the REAPER script / the companion plug-in
 *    (schema 1, see the roadmap's Appendix A).
 *
 * Everything is validated (schema, ranges, limits) before it reaches the
 * project: a malformed file produces an error string, never a half guide.
 * Message thread only; the caller decides when to refresh.
 */
class MidiGuideImporter
{
public:
    static constexpr int kSchemaVersion = 1;
    static constexpr int kMaxNotes = 50000;
    static constexpr juce::int64 kMaxJsonBytes = 8 * 1024 * 1024;

    struct Result
    {
        bool ok = false;
        juce::String error;
        GuideSource source;
        int droppedNotes = 0;
    };

    // A .mid/.midi file. sourceId = content hash, so re-importing the same
    // file replaces the same guide instead of piling up copies.
    static Result importMidiFile(const juce::File &file);

    // Already-parsed MIDI data (tests, or a host-provided file).
    static Result fromMidiFile(const juce::MidiFile &midi,
                               const juce::String &label,
                               const juce::String &sourceId);

    // One JSON snapshot (schema 1). `error` explains why it was rejected.
    static bool fromJson(const juce::var &json, GuideSource &destination,
                         juce::String &error);

    static bool importJsonFile(const juce::File &file, GuideSource &destination,
                               juce::String &error);

    // Scans <config>/guides/live for guide_*.json and returns every valid
    // snapshot. Bad files are skipped and counted in `rejectedFiles`.
    static std::vector<GuideSource> readLiveGuides(const juce::File &liveDirectory,
                                                   int *rejectedFiles = nullptr);
};
