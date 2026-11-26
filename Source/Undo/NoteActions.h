#pragma once

#include <cmath>

#include "UndoableAction.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include "../Utils/NotePitch.h"
#include <vector>
#include <functional>

/**
 * Generic action for changing a single float property on a Note.
 * Uses a member function pointer to call the appropriate setter.
 */
class NoteFloatPropertyAction : public UndoableAction
{
public:
    using Setter = void (Note::*)(float);

    NoteFloatPropertyAction(Note *note, float oldVal, float newVal,
                            Setter setter, juce::String actionName,
                            std::function<void(Note *)> onNoteChanged = nullptr)
        : note(note), oldVal(oldVal), newVal(newVal),
          setter(setter), actionName(std::move(actionName)),
          onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (!note)
            return;
        (note->*setter)(oldVal);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }
    void redo() override
    {
        if (!note)
            return;
        (note->*setter)(newVal);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }
    juce::String getName() const override { return actionName; }

private:
    Note *note;
    float oldVal;
    float newVal;
    Setter setter;
    juce::String actionName;
    std::function<void(Note *)> onNoteChanged;
};

/**
 * Generic action for changing a single float property on multiple Notes.
 * Uses a member function pointer to call the appropriate setter.
 */
class MultiNoteFloatPropertyAction : public UndoableAction
{
public:
    using Setter = void (Note::*)(float);

    MultiNoteFloatPropertyAction(const std::vector<Note *> &notes,
                                 const std::vector<float> &oldVals,
                                 const std::vector<float> &newVals,
                                 Setter setter, juce::String actionName,
                                 std::function<void()> onChanged = nullptr)
        : notes(notes), oldVals(oldVals), newVals(newVals),
          setter(setter), actionName(std::move(actionName)),
          onChanged(onChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldVals.size(); ++i)
        {
            if (notes[i])
            {
                (notes[i]->*setter)(oldVals[i]);
                notes[i]->markDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size() && i < newVals.size(); ++i)
        {
            if (notes[i])
            {
                (notes[i]->*setter)(newVals[i]);
                notes[i]->markDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override { return actionName; }

private:
    std::vector<Note *> notes;
    std::vector<float> oldVals;
    std::vector<float> newVals;
    Setter setter;
    juce::String actionName;
    std::function<void()> onChanged;
};

/**
 * Action for resetting one tilt layer on multiple notes.
 * Used for double-click on the TiltLeft/TiltRight handles: each corner resets
 * its own fine layer (D-308). The general tilt and the other fine stay put.
 */
class TiltResetAction : public UndoableAction
{
public:
    enum class TiltSide
    {
        Left,
        Right
    };

    // oldSources: Correct Pitch references to restore, one per note (see
    // NotePitch). D-306: tilt no longer moves the body, so undo and redo both
    // put back the same midiNote / reference; only the fine layer differs.
    TiltResetAction(const std::vector<Note *> &notes,
                    TiltSide side,
                    const std::vector<float> &oldTilts,
                    const std::vector<float> &oldFineLeft,
                    const std::vector<float> &oldFineRight,
                    const std::vector<float> &oldMidiNotes,
                    std::function<void()> onChanged,
                    std::vector<float> oldSources)
        : notes(notes), side(side), oldTilts(oldTilts),
          oldFineLeft(oldFineLeft), oldFineRight(oldFineRight),
          oldMidiNotes(oldMidiNotes), onChanged(onChanged),
          oldSources(std::move(oldSources)) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldTilts.size(); ++i)
        {
            if (notes[i])
            {
                // D-308: restore all three layers so undo is exact even if the
                // note was edited elsewhere in between.
                notes[i]->setTilt(oldTilts[i]);
                if (i < oldFineLeft.size())
                    notes[i]->setTiltFineLeft(oldFineLeft[i]);
                if (i < oldFineRight.size())
                    notes[i]->setTiltFineRight(oldFineRight[i]);

                if (i < oldMidiNotes.size() && i < oldSources.size())
                    NotePitch::restore(*notes[i], oldMidiNotes[i], oldSources[i]);

                notes[i]->markDirty();
                notes[i]->markSynthDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (notes[i])
            {
                if (side == TiltSide::Left)
                    notes[i]->setTiltFineLeft(0.0f);
                else
                    notes[i]->setTiltFineRight(0.0f);

                // D-306: the body and the Correct Pitch reference stay where
                // they were; the general tilt and the other fine are untouched.
                if (i < oldMidiNotes.size() && i < oldSources.size())
                    NotePitch::restore(*notes[i], oldMidiNotes[i], oldSources[i]);

                notes[i]->markDirty();
                notes[i]->markSynthDirty();
            }
        }
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override
    {
        return side == TiltSide::Left ? "Reset Tilt Left" : "Reset Tilt Right";
    }

private:
    std::vector<Note *> notes;
    TiltSide side;
    std::vector<float> oldTilts;
    std::vector<float> oldFineLeft;
    std::vector<float> oldFineRight;
    std::vector<float> oldMidiNotes;
    std::function<void()> onChanged;
    std::vector<float> oldSources;
};

/**
 * Action for changing the synthesis engine of a set of notes (M2 paso 2,
 * D-125). The rule that decides which notes may actually change - a usable
 * silence before them - is applied before the action is built, so this only
 * stores what the renderer will honour.
 */
class NoteEngineAction : public UndoableAction
{
public:
    struct Change
    {
        Note *note = nullptr;
        NoteEngine before = NoteEngine::Inherit;
        NoteEngine after = NoteEngine::Inherit;
    };

    NoteEngineAction(std::vector<Change> changes,
                     std::function<void()> onChanged = nullptr)
        : changes(std::move(changes)), onChanged(std::move(onChanged)) {}

    void undo() override { apply(false); }
    void redo() override { apply(true); }

    juce::String getName() const override { return "Change Note Engine"; }

private:
    void apply(bool useAfter)
    {
        for (auto &change : changes)
        {
            if (change.note == nullptr)
                continue;
            change.note->setEngine(useAfter ? change.after : change.before);
            change.note->markDirty();
            change.note->markSynthDirty();
        }
        if (onChanged)
            onChanged();
    }

    std::vector<Change> changes;
    std::function<void()> onChanged;
};

/**
 * Action for snapping a note to the nearest semitone (double-click).
 * Combines midiNote and pitchOffset into a rounded integer MIDI value.
 */
class NoteSnapToSemitoneAction : public UndoableAction
{
public:
    // oldSource/newSource: Correct Pitch reference before and after (see
    // NotePitch).
    NoteSnapToSemitoneAction(Note *note,
                             float oldMidi, float oldOffset,
                             float newMidi,
                             std::function<void(Note *)> onNoteChanged,
                             float oldSource, float newSource)
        : note(note), oldMidi(oldMidi), oldOffset(oldOffset),
          newMidi(newMidi), onNoteChanged(onNoteChanged),
          oldSource(oldSource), newSource(newSource) {}

    void undo() override
    {
        if (note)
        {
            NotePitch::restore(*note, oldMidi, oldSource);
            note->setPitchOffset(oldOffset);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    void redo() override
    {
        if (note)
        {
            NotePitch::restore(*note, newMidi, newSource);
            note->setPitchOffset(0.0f);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    juce::String getName() const override { return "Snap to Semitone"; }

private:
    Note *note;
    float oldMidi;
    float oldOffset;
    float newMidi;
    std::function<void(Note *)> onNoteChanged;
    float oldSource;
    float newSource;
};

/**
 * Action for snapping multiple notes to the nearest semitone.
 */
class MultiNoteSnapToSemitoneAction : public UndoableAction
{
public:
    MultiNoteSnapToSemitoneAction(const std::vector<Note *> &notes,
                                  std::vector<float> oldMidis,
                                  std::vector<float> oldOffsets,
                                  std::vector<float> newMidis,
                                  std::function<void(const std::vector<Note *> &)> onNotesChanged,
                                  std::vector<float> oldSources,
                                  std::vector<float> newSources)
        : notes(notes),
          oldMidis(std::move(oldMidis)),
          oldOffsets(std::move(oldOffsets)),
          newMidis(std::move(newMidis)),
          onNotesChanged(onNotesChanged),
          oldSources(std::move(oldSources)),
          newSources(std::move(newSources)) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldSources.size(); ++i)
        {
            auto *note = notes[i];
            if (!note)
                continue;
            NotePitch::restore(*note, oldMidis[i], oldSources[i]);
            note->setPitchOffset(oldOffsets[i]);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size() && i < newSources.size(); ++i)
        {
            auto *note = notes[i];
            if (!note)
                continue;
            NotePitch::restore(*note, newMidis[i], newSources[i]);
            note->setPitchOffset(0.0f);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Snap Notes to Semitone"; }

private:
    std::vector<Note *> notes;
    std::vector<float> oldMidis;
    std::vector<float> oldOffsets;
    std::vector<float> newMidis;
    std::function<void(const std::vector<Note *> &)> onNotesChanged;
    std::vector<float> oldSources;
    std::vector<float> newSources;
};

/**
 * Action for splitting a note into two.
 */
class NoteSplitAction : public UndoableAction
{
public:
    NoteSplitAction(Project *proj, const Note &original, const Note &firstPart, const Note &secondPart,
                    std::function<void()> onChanged = nullptr)
        : project(proj), originalNote(original), firstNote(firstPart), secondNote(secondPart),
          onChanged(onChanged) {}

    void undo() override
    {
        if (!project)
            return;
        project->removeNoteByStartFrame(secondNote.getStartFrame());
        for (auto &note : project->getNotes())
        {
            if (note.getStartFrame() == firstNote.getStartFrame())
            {
                note = originalNote;
                break;
            }
        }
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        if (!project)
            return;
        for (auto &note : project->getNotes())
        {
            if (note.getStartFrame() == originalNote.getStartFrame())
            {
                note = firstNote;
                break;
            }
        }
        project->addNote(secondNote);
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override { return "Split Note"; }
    bool requiresAudioResynthesis() const override { return false; }

private:
    Project *project;
    Note originalNote;
    Note firstNote;
    Note secondNote;
    std::function<void()> onChanged;
};

/**
 * Action for merging two adjacent note segments.
 */
class NoteMergeAction : public UndoableAction
{
public:
    NoteMergeAction(Project *proj, const Note &firstPart, const Note &secondPart,
                    const Note &merged, std::function<void()> onChanged = nullptr)
        : project(proj), firstNote(firstPart), secondNote(secondPart),
          mergedNote(merged), onChanged(onChanged) {}

    void undo() override
    {
        if (!project)
            return;
        for (auto &note : project->getNotes())
        {
            if (note.getStartFrame() == mergedNote.getStartFrame())
            {
                note = firstNote;
                break;
            }
        }
        project->addNote(secondNote);
        if (onChanged)
            onChanged();
    }

    void redo() override
    {
        if (!project)
            return;
        for (auto &note : project->getNotes())
        {
            if (note.getStartFrame() == firstNote.getStartFrame())
            {
                note = mergedNote;
                break;
            }
        }
        project->removeNoteByStartFrame(secondNote.getStartFrame());
        if (onChanged)
            onChanged();
    }

    juce::String getName() const override { return "Merge Notes"; }
    bool requiresAudioResynthesis() const override { return false; }

private:
    Project *project;
    Note firstNote;
    Note secondNote;
    Note mergedNote;
    std::function<void()> onChanged;
};
