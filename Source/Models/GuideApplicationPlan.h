// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "MidiGuide.h"
#include "Project.h"

#include <vector>

/**
 * Track-wide application plan for a MIDI guide (R8 of the guide-integration
 * fix, D-361).
 *
 * SCOPE=A is fixed: the guide belongs to the track, so a guide slot is resolved
 * against EVERY item of the track, whatever source file it shows, before
 * anything is written. R1's active-item filter is not repeated here; the caller
 * passes the regions of the whole track.
 *
 * Axes, kept explicit (same rule as MidiGuideRegionScope and DocumentNoteIndex):
 *   guide slot : timeline (host) seconds, quarters -> HostTempoMap
 *   audio note : project (source) seconds; host = source + region offset
 *
 * Pure: the projects are read, never written. Applying is R9.
 */

/** One item of the track as the plan needs it. */
struct GuideApplicationRegion
{
    juce::String modificationKey; // edit layer (each modification is one undo target)
    juce::String sourceKey;       // audio source (file) the item shows
    double hostStartSeconds = 0.0;
    double hostEndSeconds = 0.0;
    double sourceStartSeconds = 0.0;
    double sourceEndSeconds = 0.0;
    double offsetSeconds = 0.0; // host - source
    const Project* project = nullptr; // nullptr = item not analysed
};

/** Analysis grid the notes' frames are expressed in. */
struct MidiGuideSampling
{
    int sampleRate = 44100;
    int hopSize = 512;
};

struct GuidePlannedEdit
{
    int guideIndex = -1;   // slot that won the dedupe (diagnostics)
    int noteIndex = -1;    // index into the modification's Project::getNotes()
    double currentMidi = 0.0; // Note::getMidiNote() at plan time (what changes)
    double targetPitch = 0.0; // value R9 writes through NotePitch
    double score = 0.0;    // matcher score
};

/** All edits of one modification, so R9 can apply and undo them as one unit. */
struct GuideModificationPlan
{
    juce::String modificationKey;
    const Project* project = nullptr; // not owned; the modification's Project
    double hostStartSeconds = 0.0;    // first slot the modification takes
    double hostEndSeconds = 0.0;      // last slot it takes
    int guideNotes = 0;               // slots that resolved to it
    std::vector<GuidePlannedEdit> edits; // sorted by noteIndex
};

enum class GuideSkipReason
{
    NoItem = 0,        // a gap between items: no region covers the slot
    NotAnalyzed = 1,   // the slot falls on items, none has a Project
    DifferentSource = 2, // items of two or more sources overlap the slot and
                         // none of them carries a note it can take
    NoMatch = 3        // an analysed item of one source owns the slot, but no
                       // note of it fits (pitch/time); measured: 1 of 55
};

const char* guideSkipReasonName(GuideSkipReason reason);

struct GuideSkippedSlot
{
    int guideIndex = -1;
    GuideSkipReason reason = GuideSkipReason::NoItem;
    juce::String sourceKey; // first overlapping region, for the message
};

struct GuideApplicationPlan
{
    // Only modifications with at least one edit. A modification whose items
    // the guide never reaches (measured: item 4, 0 of 111) is absent, not an
    // empty entry, so "nothing to apply" cannot be confused with "applied 0".
    std::vector<GuideModificationPlan> modifications; // sorted by key
    std::vector<GuideSkippedSlot> skipped;            // sorted by guide index

    int guideNotes = 0;  // usable slots (non-zero duration)
    int appliedSlots = 0; // slots that produced at least one edit
    int editedNotes = 0; // deduplicated (modification, note) pairs
    int changedNotes = 0; // edits where the centre really moves

    int skippedCount(GuideSkipReason reason) const;
    bool appliesTo(const juce::String& modificationKey) const;
};

/**
 * Resolves every guide slot against the track's regions.
 *
 * Per slot: regions with strict host overlap -> host to source with THAT
 * region's offset -> notes of its Project overlapping the source window ->
 * MidiGuideMatcher (same one-to-one rule as the single-item path) -> one
 * proposed edit per modification and note, deduplicated across sibling windows
 * of the same modification (best score, then lower guide index).
 *
 * Not applicable to the plan (kept in R1/MidiGuideRegionScope): the active
 * item window. No active-region parameter is accepted on purpose; if a caller
 * needs it, it filters the regions it passes in.
 */
GuideApplicationPlan
planGuideApplication(const GuideSource& guide, const HostTempoMap& tempo,
                     const std::vector<GuideApplicationRegion>& regions,
                     const MidiGuideSampling& sampling);
