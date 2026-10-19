// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../Models/Note.h"
#include "../Models/Project.h"
#include "ScaleUtils.h"

// The only place that writes a note's pitch (Note::midiNote) and its Correct
// Pitch reference (Note::lastNonMacroMidiNote, "the sung pitch"). Every pitch
// write in the editor is one of the operations below; the
// SourceGuard_pitch-writes test fails when code outside Note.h and this file
// calls the raw setters, so a new feature cannot forget the reference rule.
// (The check reads one line at a time: calls through a member pointer or split
// across lines are not caught, so do not write them.)
namespace NotePitch
{
// The reference after an edit other than Correct Pitch (drag, arrow keys,
// harmony, snap, tilt and the other smart controls) moved `note` from
// fromMidi to toMidi. sourceBefore is the reference when the edit began.
inline float sourceAfterEdit(const Project &project, const Note &note,
                             float sourceBefore, float fromMidi, float toMidi)
{
    return ScaleUtils::carriedCorrectionSource(project, sourceBefore,
                                               note.getOriginalMidiNote(),
                                               fromMidi, toMidi,
                                               project.getPitchReferenceHz());
}

// Applies such an edit: the new pitch and the carried reference. Returns the
// new reference, for the undo record.
inline float applyEdit(Note &note, const Project &project, float sourceBefore,
                       float fromMidi, float toMidi)
{
    const float source = sourceAfterEdit(project, note, sourceBefore, fromMidi, toMidi);
    note.setMidiNote(toMidi);
    note.setLastNonMacroMidiNote(source);
    return source;
}

// Puts back a recorded pitch and reference exactly: undo, redo, cancelling a
// gesture, loading a project, and the halves of a split or merge.
inline void restore(Note &note, float midi, float correctionSource)
{
    note.setMidiNote(midi);
    note.setLastNonMacroMidiNote(correctionSource);
}

// Correct Pitch itself and the Pitch Center macro: the pitch moves towards
// the target, the reference stays.
inline void applyCorrection(Note &note, float midi)
{
    note.setMidiNoteFromPitchCorrection(midi);
}

// Correct Pitch starts over from `source` (a pitch that is not a 0-100%
// correction of its old reference), or a no-op touch puts the old one back.
inline void setCorrectionSource(Note &note, float source)
{
    note.setLastNonMacroMidiNote(source);
}

// A new sung pitch (pitch drawing): the reference follows, so Correct Pitch
// reads 0% afterwards.
inline void setSungPitch(Note &note, float midi)
{
    note.setMidiNote(midi);
}
} // namespace NotePitch
