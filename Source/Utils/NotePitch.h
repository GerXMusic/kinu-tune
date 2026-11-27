// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../Models/Note.h"
#include "../Models/Project.h"
#include "ScaleUtils.h"

#include <cmath>
#include <optional>

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

// N3: the result of a relative pitch edit (drag, arrow keys, harmony, guide),
// worked out before anything is written so a batch can plan first and write
// only when the whole selection is valid.
struct PitchEdit
{
    float midi = 0.0f;
    float source = 0.0f;
};

// Relative move by `semitoneDelta` on the on-screen pitch (midi + visible
// offset), clamped to the editor range, with the reference carried as an edit
// keeps it. Null when the note is a rest, the delta is nil, or the clamp leaves
// the note exactly where it was. The drag, the arrow-key nudge and the
// document-level batch all come through here, so they cannot disagree.
inline std::optional<PitchEdit>
editTarget(const Project &project, const Note &note, float semitoneDelta,
           float minMidi, float maxMidi)
{
    if (note.isRest() || std::abs(semitoneDelta) < 1.0e-6f)
        return std::nullopt;

    const float oldMidi = note.getMidiNote();
    const float offset = note.getPitchOffset();
    const float movedAdjustedMidi =
        juce::jlimit(minMidi, maxMidi, note.getAdjustedMidiNote() + semitoneDelta);
    const float movedMidi = movedAdjustedMidi - offset;

    if (std::abs(movedMidi - oldMidi) <= 1.0e-6f)
        return std::nullopt;

    return PitchEdit{movedMidi,
                     sourceAfterEdit(project, note, note.getLastNonMacroMidiNote(),
                                     oldMidi, movedMidi)};
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

// D-312: the MIDI guide sets the note's centre (its base pitch) to the guide
// note and nothing else. Vibrato lives in deltaPitch and the manual visible
// offset in pitchOffset: both stay exactly as they were, which is what makes a
// guide apply audible only as a pitch move. Returns the new reference, for the
// undo record.
inline float applyGuideTarget(Note &note, const Project &project,
                              float guideMidi)
{
    const float from = note.getMidiNote();
    const float sourceBefore = note.getLastNonMacroMidiNote();
    return applyEdit(note, project, sourceBefore, from, guideMidi);
}
} // namespace NotePitch
