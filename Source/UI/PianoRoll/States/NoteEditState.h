// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../../Models/Note.h"
#include "../../../Utils/NotePitch.h"

#include <vector>

// The saved state of one note's edits and the predicates that ask whether a
// note has any. Moved out of PianoRollComponent.cpp without changing what is
// captured, what "restablecer" restores, or which notes count as edited.
namespace pianoRollEditState
{

struct NoteEditState
{
  float midiNote;
  float pitchOffset;
  // D-303: el centro de render tambien es parte del estado guardado; si
  // "restablecer" lo dejara fuera, la base quedaria desplazada.
  float renderCenterOffset;
  float volumeDb;
  float formantShift;
  float tilt;
  float tiltFineLeft;
  float tiltFineRight;
  float vibrato;
  float pitchDrift;
  int smoothLeftFrames;
  int smoothRightFrames;
  float deltaScale;
  float deltaOffset;
  std::vector<float> bakedDeltaPitch;
  std::vector<float> deltaPitch;
  float correctionSource; // Correct Pitch reference (the sung pitch)
  // D-268: la transicion al siguiente (D-225) tambien es una edicion guardada:
  // si no se limpia, la nota nunca es neutral y "restablecer" no devuelve la
  // fuente (medido en REAPER: 75 ms persistidos dejaban la nota fuera de
  // isNeutralForOriginalWaveform()).
  int transitionOutMs;
  // D-258/T6: restablecer limpia tambien el motor elegido y el mute. Sin
  // esto, "restablecer original" dejaba la nota re-sintetizada con su motor
  // o silenciada, y no devolvia la fuente exacta.
  NoteEngine engine;
  bool muted;
  NoteEngine blockEngineOverride;

  static NoteEditState capture(const Note& note)
  {
    return {note.getMidiNote(), note.getPitchOffset(),
            note.getRenderCenterOffset(), note.getVolumeDb(),
            note.getFormantShift(),
            note.getTilt(), note.getTiltFineLeft(), note.getTiltFineRight(),
            note.getVibrato(), note.getPitchDrift(),
            note.getSmoothLeftFrames(), note.getSmoothRightFrames(),
            note.getDeltaScale(), note.getDeltaOffset(),
            note.getBakedDeltaPitch(), note.getDeltaPitch(),
            note.getLastNonMacroMidiNote(), note.getTransitionOutMs(),
            note.getEngine(), note.isMuted(),
            note.getBlockEngineOverride()};
  }

  static NoteEditState defaultsFor(const Note& note)
  {
    return {note.getOriginalMidiNote(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 1.0f, 0, 0, 1.0f, 0.0f, {},
            note.getOriginalDeltaPitch(), note.getOriginalMidiNote(), 0,
            NoteEngine::Inherit, false, NoteEngine::Inherit};
  }

  void applyTo(Note& note) const
  {
    NotePitch::restore(note, midiNote, correctionSource);
    note.setPitchOffset(pitchOffset);
    note.setRenderCenterOffset(renderCenterOffset);
    note.setVolumeDb(volumeDb);
    note.setFormantShift(formantShift);
    note.setTilt(tilt);
    note.setTiltFineLeft(tiltFineLeft);
    note.setTiltFineRight(tiltFineRight);
    note.setVibrato(vibrato);
    note.setPitchDrift(pitchDrift);
    note.setSmoothLeftFrames(smoothLeftFrames);
    note.setSmoothRightFrames(smoothRightFrames);
    note.setDeltaScale(deltaScale);
    note.setDeltaOffset(deltaOffset);
    note.setBakedDeltaPitch(bakedDeltaPitch);
    note.setDeltaPitch(deltaPitch);
    note.setTransitionOutMs(transitionOutMs);
    note.setEngine(engine);
    note.setMuted(muted);
    note.setBlockEngineOverride(blockEngineOverride);
    note.markDirty();
    note.markSynthDirty();
  }
};

// Note::deltaPitch is only the active contour when the note has no
// analysed originalDeltaPitch (legacy projects). Otherwise it is a scratch
// copy that stays empty after a fresh scan and is filled from the global
// curve when a drag starts, so comparing it to the original would flag
// every untouched note as pitch-edited. Real contour edits live in
// bakedDeltaPitch, which is compared separately.
inline bool hasUnrenderedDeltaEdit(const Note& note, const NoteEditState& current,
                                   const NoteEditState& defaults)
{
  if (note.hasOriginalDeltaPitch())
    return false;
  return current.deltaPitch != defaults.deltaPitch;
}

inline bool hasPitchEdits(const Note& note)
{
  const auto current = NoteEditState::capture(note);
  const auto defaults = NoteEditState::defaultsFor(note);
  return current.midiNote != defaults.midiNote ||
         current.pitchOffset != defaults.pitchOffset ||
         current.renderCenterOffset != defaults.renderCenterOffset ||
         current.transitionOutMs != defaults.transitionOutMs ||
         current.tilt != defaults.tilt ||
         current.tiltFineLeft != defaults.tiltFineLeft ||
         current.tiltFineRight != defaults.tiltFineRight ||
         current.vibrato != defaults.vibrato ||
         current.pitchDrift != defaults.pitchDrift ||
         current.smoothLeftFrames != defaults.smoothLeftFrames ||
         current.smoothRightFrames != defaults.smoothRightFrames ||
         current.deltaScale != defaults.deltaScale ||
         current.deltaOffset != defaults.deltaOffset ||
         current.bakedDeltaPitch != defaults.bakedDeltaPitch ||
         hasUnrenderedDeltaEdit(note, current, defaults);
}

inline bool hasFormantEdits(const Note& note) { return note.getFormantShift() != 0.0f; }
inline bool hasAmplitudeEdits(const Note& note) { return note.getVolumeDb() != 0.0f; }
inline bool hasTimingEdits(const Note& note)
{
  return note.getStartFrame() != note.getSrcStartFrame() ||
         note.getEndFrame() != note.getSrcEndFrame();
}

} // namespace pianoRollEditState
