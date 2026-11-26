// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../Audio/Engine/HostTempoMap.h"
#include "../JuceHeader.h"
#include <memory>

#if JucePlugin_Enable_ARA

/**
 * Reads the host tempo map published through ARA and returns an immutable
 * snapshot for the UI (D-310, plan/tempo-map-ara-grilla.md §4.2).
 *
 * One reader, one source of truth: the grid, the snap and (later) the MIDI
 * guide all consume the published HostTempoMap; nothing else calls the ARA
 * content readers.
 *
 * Threading: the ARA content reader takes a lock inside the host, so this must
 * only be called from the message thread (editor timer / region switch), never
 * from processBlock or a paint.
 */
class ARATempoMapReader
{
public:
    // What the probe found. Logged on the first build that reads the map
    // (G1) so REAPER's answer can be compared with the ruler.
    struct Probe
    {
        int musicalContexts = 0;
        int tempoEntries = 0;
        int barSignatures = 0;
        double firstTimeSeconds = 0.0;
        double lastTimeSeconds = 0.0;
        double firstQuarter = 0.0;
        double lastQuarter = 0.0;
        // ARA content grade as an integer (kARAContentGradeInitial..Approved).
        int contentGrade = -1;
        // "no-context" | "no-reader" | "constant" | "real-map" | "invalid"
        juce::String grade;

        juce::String toString() const;
    };

    // The context to read for a region: its region sequence's context, else the
    // document's only context, else nullptr.
    static juce::ARAMusicalContext* chooseContext(juce::ARAPlaybackRegion* region);

    // Builds the map from a musical context. Returns nullptr when there is no
    // usable map (the caller keeps its constant-BPM fallback).
    static std::shared_ptr<const HostTempoMap>
    readFromContext(juce::ARAMusicalContext* context, Probe* probe = nullptr);

    // Convenience: context for `region`, then readFromContext().
    static std::shared_ptr<const HostTempoMap>
    readForRegion(juce::ARAPlaybackRegion* region, Probe* probe = nullptr);
};

#else

class ARATempoMapReader
{
public:
    struct Probe
    {
        int musicalContexts = 0;
        int tempoEntries = 0;
        int barSignatures = 0;
        double firstTimeSeconds = 0.0;
        double lastTimeSeconds = 0.0;
        double firstQuarter = 0.0;
        double lastQuarter = 0.0;
        juce::String grade;

        juce::String toString() const { return "no-ara"; }
    };

    static std::shared_ptr<const HostTempoMap>
    readForRegion(void*, Probe* = nullptr)
    {
        return nullptr;
    }
};

#endif
