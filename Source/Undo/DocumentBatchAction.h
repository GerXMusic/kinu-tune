// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include "UndoableAction.h"
#include "../Models/DocumentNoteBatch.h"
#include "../Models/DocumentNoteSelection.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/TransformParams.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

/**
 * N4 (D-344): one document-level undo step for a batch gesture.
 *
 * A batch edit (N3) writes the selected notes in the Project that owns each of
 * them. This action records, per note, the exact values it had before and after
 * the gesture, so one Ctrl+Z puts every item back and one Ctrl+Y redoes it.
 *
 * It is still one source of truth: what is stored per note is a record of
 * VALUES (like TrackDragAction keeps midi/reference), keyed by the live Note*.
 * The note itself is never copied, no note list is duplicated and no second set
 * of edits is created. A note that is no longer in its project (the item was
 * reloaded) is skipped instead of being written through a stale pointer.
 *
 * The pitch and its Correct Pitch reference only reach the note through
 * TransformParams::applyToNote(), which goes through NotePitch::restore: the
 * SourceGuard_pitch-writes rule holds for the undo path too.
 */
namespace documentNoteBatch
{
// The values of one note at one point of the gesture.
struct NoteState
{
    Note *note = nullptr;
    TransformParams params; // pitch + reference + the three tilt layers + the
                            // rest of the smart controls (D-308)
    bool muted = false;
    bool deleted = false;
    bool rest = false;
    bool selected = false;
    int startFrame = 0;
    int endFrame = 0;

    static NoteState capture(Note &source)
    {
        NoteState state;
        state.note = &source;
        state.params = TransformParams::fromNote(source);
        state.muted = source.isMuted();
        state.deleted = source.isDeleted();
        state.rest = source.isRest();
        state.selected = source.isSelected();
        state.startFrame = source.getStartFrame();
        state.endFrame = source.getEndFrame();
        return state;
    }

    static bool noteStillLive(const Project *project, const Note *note)
    {
        if (project == nullptr || note == nullptr)
            return false;
        const auto &notes = project->getNotes();
        return std::any_of(notes.begin(), notes.end(),
                           [note](const Note &candidate)
                           { return &candidate == note; });
    }

    void applyTo(Note &target) const
    {
        // Pitch and reference only through NotePitch (inside TransformParams).
        params.applyToNote(target);
        target.setMuted(muted);
        target.setDeleted(deleted);
        target.setRest(rest);
        // N9 (D-349): the selection flag is not written here; the caller routes
        // it through DocumentNoteSelection::setNoteSelected.
        target.setStartFrame(startFrame);
        target.setEndFrame(endFrame);
    }

    bool sameValuesAs(const NoteState &other) const
    {
        return params == other.params && muted == other.muted &&
               deleted == other.deleted && rest == other.rest &&
               selected == other.selected && startFrame == other.startFrame &&
               endFrame == other.endFrame;
    }
};

// One note of the record, with the item it belongs to.
struct TrackedState
{
    Project *project = nullptr;
    NoteState state;
};

// One note that the gesture actually changed.
struct StateChange
{
    Project *project = nullptr;
    NoteState before;
    NoteState after;
};

// The items of a batch, deduplicated, in the order the targets arrive (which
// targetsOf() already sorted by region and note).
inline std::vector<Project *> projectsOf(const std::vector<Target> &targets)
{
    std::vector<Project *> projects;
    for (const auto &target : targets)
        if (target.project != nullptr &&
            std::find(projects.begin(), projects.end(), target.project) ==
                projects.end())
            projects.push_back(target.project);
    return projects;
}

/**
 * Every live note of the given items, item by item. The whole item is recorded,
 * not only the selection: a quantize moves shared boundaries and therefore can
 * change an unselected neighbour, and undo has to put that back too.
 */
inline std::vector<TrackedState> captureStates(
    const std::vector<Project *> &projects)
{
    std::vector<TrackedState> states;
    for (auto *project : projects)
    {
        if (project == nullptr)
            continue;
        for (auto &note : project->getNotes())
            states.push_back({project, NoteState::capture(note)});
    }
    return states;
}

// The notes whose values are not `before` anymore, with their current values.
inline std::vector<StateChange> changedSince(
    const std::vector<TrackedState> &before)
{
    std::vector<StateChange> changes;
    for (const auto &tracked : before)
    {
        if (!NoteState::noteStillLive(tracked.project, tracked.state.note))
            continue;
        auto after = NoteState::capture(*tracked.state.note);
        if (tracked.state.sameValuesAs(after))
            continue;
        changes.push_back({tracked.project, tracked.state, after});
    }
    return changes;
}

// The item of every change, deduplicated, in the record order.
inline std::vector<Project *> projectsOfChanges(
    const std::vector<StateChange> &changes)
{
    std::vector<Project *> projects;
    for (const auto &change : changes)
        if (change.project != nullptr &&
            std::find(projects.begin(), projects.end(), change.project) ==
                projects.end())
            projects.push_back(change.project);
    return projects;
}

/**
 * The single undo step of a batch gesture. `notify` runs after undo and after
 * redo with the items it touched, so the editor can persist them and ask for
 * their render (D-247: whoever dirties the audio requests the render).
 */
class DocumentBatchAction final : public UndoableAction
{
public:
    using Notify = std::function<void(const std::vector<Project *> &)>;

    DocumentBatchAction(std::vector<StateChange> changes, Notify notify,
                        juce::String name)
        : changes(std::move(changes)), notify(std::move(notify)),
          name(std::move(name))
    {
    }

    void undo() override { apply(false); }
    void redo() override { apply(true); }

    juce::String getName() const override { return name; }

    // The action spans several items at once, so it owns its context: the
    // decorator must not wrap it in the single-region ContextUndoAction.
    bool ownsProjectContext() const override { return true; }

    int noteCount() const { return static_cast<int>(changes.size()); }

    std::vector<Project *> projects() const { return projectsOfChanges(changes); }

private:
    void apply(bool redo)
    {
        std::vector<Project *> touched;
        for (size_t i = 0; i < changes.size();)
        {
            Project *project = changes[i].project;
            size_t end = i;
            while (end < changes.size() && changes[end].project == project)
                ++end;

            std::vector<Note *> notes;
            int minFrame = std::numeric_limits<int>::max();
            int maxFrame = std::numeric_limits<int>::min();
            for (size_t j = i; j < end; ++j)
            {
                auto &change = changes[j];
                if (!NoteState::noteStillLive(project, change.before.note))
                    continue;
                const auto &state = redo ? change.after : change.before;
                const auto &other = redo ? change.before : change.after;
                state.applyTo(*state.note);
                // N9 (D-349): the selection travels through its owner, so undo
                // and redo restore it there instead of writing the flag.
                DocumentNoteSelection::setNoteSelected(*project, *state.note,
                                                       state.selected);
                state.note->markDirty();
                state.note->markSynthDirty();
                notes.push_back(state.note);
                minFrame = std::min({minFrame, state.startFrame,
                                     other.startFrame});
                maxFrame = std::max({maxFrame, state.endFrame,
                                     other.endFrame});
            }
            i = end;

            if (notes.empty() || minFrame > maxFrame)
                continue;

            // The same rebuilds the batch operation does after it writes: the
            // base carries pitch and timing, the delta carries the tilt and the
            // other smart controls.
            PitchCurveProcessor::rebuildBaseFromNotes(*project);
            PitchCurveProcessor::rebuildDeltaForNotes(*project, notes);
            project->markNoteEditDirtyRange(minFrame, maxFrame);
            const int frameCount = project->getAudioData().getNumFrames();
            project->setF0DirtyRange(std::max(0, minFrame - 24),
                                     std::min(frameCount, maxFrame + 24));
            project->setModified(true);
            touched.push_back(project);
        }

        if (!touched.empty() && notify)
            notify(touched);
    }

    std::vector<StateChange> changes;
    Notify notify;
    juce::String name;
};
} // namespace documentNoteBatch
