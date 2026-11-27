// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuideTrackApply.h"

#include "../Utils/NotePitch.h"
#include "../Utils/PitchCurveProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
// One item the write touched, with the frame span its notes cover.
struct DirtyItem
{
    Project *project = nullptr;
    int dirtyStart = std::numeric_limits<int>::max();
    int dirtyEnd = std::numeric_limits<int>::min();
};

DirtyItem *findDirtyItem(std::vector<DirtyItem> &items, Project *project)
{
    for (auto &item : items)
        if (item.project == project)
            return &item;
    return nullptr;
}

} // namespace

MidiGuideTrackApplyResult
summarizeGuideApplicationPlan(const GuideApplicationPlan &plan,
                              const juce::String &undoName)
{
    MidiGuideTrackApplyResult result;
    result.undoName = undoName;
    result.skippedNoItem = plan.skippedCount(GuideSkipReason::NoItem);
    result.skippedNotAnalyzed = plan.skippedCount(GuideSkipReason::NotAnalyzed);
    result.skippedDifferentSource =
        plan.skippedCount(GuideSkipReason::DifferentSource);
    result.skippedNoMatch = plan.skippedCount(GuideSkipReason::NoMatch);
    return result;
}

MidiGuideTrackApplyResult
applyMidiGuideTrackPlan(const GuideApplicationPlan &plan,
                        const juce::String &undoName)
{
    auto result = summarizeGuideApplicationPlan(plan, undoName);

    // Items the plan reaches, in its own (deterministic) order, deduplicated:
    // two modifications can share one Project, and recording it twice would
    // undo it twice.
    std::vector<Project *> items;
    for (const auto &modification : plan.modifications)
    {
        auto *project = const_cast<Project *>(modification.project);
        if (project == nullptr)
            continue;
        if (std::find(items.begin(), items.end(), project) == items.end())
            items.push_back(project);
    }
    if (items.empty())
        return result;

    // N4 (D-344): the record exists before the first write, covering every note
    // of every touched item. Only what really differs is kept for the undo.
    const auto before = documentNoteBatch::captureStates(items);

    std::vector<DirtyItem> dirty;
    // (item slot, note index) already written: one note is written once, however
    // many guide slots or sibling windows point at it.
    std::vector<std::pair<int, int>> written;
    std::vector<juce::String> changedKeys;

    for (const auto &modification : plan.modifications)
    {
        auto *project = const_cast<Project *>(modification.project);
        if (project == nullptr)
            continue;

        const int itemSlot = [&items, project]() {
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i] == project)
                    return static_cast<int>(i);
            return -1;
        }();
        if (itemSlot < 0)
            continue;

        auto &notes = project->getNotes();
        bool keyChanged = false;
        for (const auto &edit : modification.edits)
        {
            if (edit.noteIndex < 0 ||
                edit.noteIndex >= static_cast<int>(notes.size()))
            {
                ++result.missingNotes;
                continue;
            }

            const bool repeated = std::any_of(
                written.begin(), written.end(),
                [itemSlot, &edit](const std::pair<int, int> &entry) {
                    return entry.first == itemSlot &&
                           entry.second == edit.noteIndex;
                });
            if (repeated)
            {
                ++result.duplicateEdits;
                continue;
            }
            written.push_back({itemSlot, edit.noteIndex});

            auto &note = notes[static_cast<size_t>(edit.noteIndex)];
            if (note.isRest() || note.isMuted())
            {
                ++result.missingNotes;
                continue;
            }

            ++result.matchedNotes;

            // D-312: only the centre; NotePitch keeps the Correct Pitch
            // reference consistent, exactly like a drag.
            if (std::abs(edit.targetPitch - note.getMidiNote()) <= 1.0e-4)
            {
                ++result.unchangedNotes;
                continue;
            }

            NotePitch::applyGuideTarget(note, *project,
                                        static_cast<float>(edit.targetPitch));
            note.markDirty();
            note.markSynthDirty();

            auto *item = findDirtyItem(dirty, project);
            if (item == nullptr)
            {
                dirty.push_back({project, note.getStartFrame(), note.getEndFrame()});
                item = &dirty.back();
            }
            item->dirtyStart = std::min(item->dirtyStart, note.getStartFrame());
            item->dirtyEnd = std::max(item->dirtyEnd, note.getEndFrame());

            keyChanged = true;
            ++result.changedNotes;
        }

        if (keyChanged)
            changedKeys.push_back(modification.modificationKey);
    }

    // The same rebuilds a single-item edit does after writing (D-285): the base
    // carries the new centres and the dirty range asks for their render.
    for (const auto &item : dirty)
    {
        PitchCurveProcessor::rebuildBaseFromNotes(*item.project);
        item.project->markNoteEditDirtyRange(item.dirtyStart, item.dirtyEnd);
        item.project->setModified(true);
    }

    if (result.changedNotes == 0)
        return result;

    result.changes = documentNoteBatch::changedSince(before);
    result.projects = documentNoteBatch::projectsOfChanges(result.changes);
    // Only the edit layers whose write produced a real change.
    for (const auto &key : changedKeys)
        if (std::find(result.modificationKeys.begin(),
                      result.modificationKeys.end(),
                      key) == result.modificationKeys.end())
            result.modificationKeys.push_back(key);
    result.changedNotes = static_cast<int>(result.changes.size());
    return result;
}
