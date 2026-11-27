// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "GuideApplicationPlan.h"
#include "../Undo/DocumentBatchAction.h"

#include <vector>

/**
 * Track-wide application of a MIDI guide (R9 of the guide-integration fix,
 * D-362).
 *
 * R8 resolved, without writing, which note of which modification takes each
 * guide slot (SCOPE=A, D-361). This is the write: every edit of the plan is
 * applied through NotePitch, item by item, in the plan's order (modification
 * key, then note index), so the same state always produces the same writes.
 *
 * Undo is ONE document step across every item, not one per item: the values of
 * every note of every touched Project are captured before the first write and
 * compared afterwards, and the caller registers the difference as a single
 * DocumentBatchAction (D-344, the same composed step the note batch uses).
 *
 * The plan is the only source of pairings: nothing is re-matched here.
 */
struct MidiGuideTrackApplyResult
{
    // What changed, per note, for the caller's DocumentBatchAction.
    std::vector<documentNoteBatch::StateChange> changes;
    // Items with at least one real change, in plan order (deduplicated).
    std::vector<Project *> projects;
    // Edit layers with at least one real change, in plan order (deduplicated).
    std::vector<juce::String> modificationKeys;

    int matchedNotes = 0;   // edits the plan proposed and a live note took
    int changedNotes = 0;   // notes whose centre really moved (= changes.size())
    int unchangedNotes = 0; // matched, but already at the guide pitch
    int missingNotes = 0;   // plan pointing at a note that is not there anymore
    int duplicateEdits = 0; // same (item, note) reached twice: applied once

    // Guide slots the plan could not place, by reason (R8, never silent).
    int skippedNoItem = 0;
    int skippedNotAnalyzed = 0;
    int skippedDifferentSource = 0;
    int skippedNoMatch = 0;

    // The single undo step's name, as it will appear in the history.
    juce::String undoName;
    // Set by the caller when an item has no path to its render: nothing was
    // written (the audio would be heard stale or mute, D-247).
    bool renderPathMissing = false;

    bool hasEdits() const { return matchedNotes > 0; }
};

/** Counters of the plan, with no write. */
MidiGuideTrackApplyResult
summarizeGuideApplicationPlan(const GuideApplicationPlan &plan,
                              const juce::String &undoName);

/**
 * Applies every edit of the plan to its modification's Project.
 *
 * Only the centre moves and only through NotePitch::applyGuideTarget (D-312);
 * vibrato, pitchOffset and the smart controls are not touched. Each touched
 * Project gets the same rebuild a single-item edit does (D-285): base from the
 * notes, dirty range and modified flag. Nothing is written when no note really
 * moves.
 */
MidiGuideTrackApplyResult
applyMidiGuideTrackPlan(const GuideApplicationPlan &plan,
                        const juce::String &undoName);
