#pragma once

#include "../Models/Note.h"
#include "NotePitch.h"

/**
 * Stores pitch tool transformation parameters for a single note.
 * Used by UndoManager to capture and restore transformation state non-destructively.
 */
struct TransformParams
{
    float tilt = 0.0f;
    float tiltFineLeft = 0.0f;
    float tiltFineRight = 0.0f;
    float formantShift = 0.0f;
    float volumeDb = 0.0f;
    float vibrato = 1.0f;
    float pitchDrift = 1.0f;
    int smoothLeftFrames = 0;
    int smoothRightFrames = 0;
    int transitionOutMs = 0;
    float midiNote = 0.0f;
    float deltaScale = 1.0f;
    float deltaOffset = 0.0f;
    // Correct Pitch reference (the sung pitch); restored with midiNote.
    float correctionSource = 0.0f;

    TransformParams() = default;

    /** Capture all transformation params from a note. */
    static TransformParams fromNote(const Note& note)
    {
        TransformParams p;
        p.tilt = note.getTilt();
        p.tiltFineLeft = note.getTiltFineLeft();
        p.tiltFineRight = note.getTiltFineRight();
        p.formantShift = note.getFormantShift();
        p.volumeDb = note.getVolumeDb();
        p.vibrato = note.getVibrato();
        p.pitchDrift = note.getPitchDrift();
        p.smoothLeftFrames = note.getSmoothLeftFrames();
        p.smoothRightFrames = note.getSmoothRightFrames();
        p.transitionOutMs = note.getTransitionOutMs();
        p.midiNote = note.getMidiNote();
        p.deltaScale = note.getDeltaScale();
        p.deltaOffset = note.getDeltaOffset();
        p.correctionSource = note.getLastNonMacroMidiNote();
        return p;
    }

    /** Apply all transformation params back to a note. */
    void applyToNote(Note& note) const
    {
        NotePitch::restore(note, midiNote, correctionSource);
        note.setTilt(tilt);
        note.setTiltFineLeft(tiltFineLeft);
        note.setTiltFineRight(tiltFineRight);
        note.setVibrato(vibrato);
        note.setPitchDrift(pitchDrift);
        note.setFormantShift(formantShift);
        note.setVolumeDb(volumeDb);
        note.setSmoothLeftFrames(smoothLeftFrames);
        note.setSmoothRightFrames(smoothRightFrames);
        note.setTransitionOutMs(transitionOutMs);
        note.setDeltaScale(deltaScale);
        note.setDeltaOffset(deltaOffset);
    }

    bool operator==(const TransformParams& other) const
    {
        return tilt == other.tilt &&
               tiltFineLeft == other.tiltFineLeft &&
               tiltFineRight == other.tiltFineRight &&
               formantShift == other.formantShift &&
               volumeDb == other.volumeDb &&
               vibrato == other.vibrato &&
               pitchDrift == other.pitchDrift &&
               smoothLeftFrames == other.smoothLeftFrames &&
               smoothRightFrames == other.smoothRightFrames &&
               transitionOutMs == other.transitionOutMs &&
               midiNote == other.midiNote &&
               deltaScale == other.deltaScale &&
               deltaOffset == other.deltaOffset &&
               correctionSource == other.correctionSource;
    }

    bool operator!=(const TransformParams& other) const
    {
        return !(*this == other);
    }

    bool isIdentity() const
    {
        return tilt == 0.0f &&
               tiltFineLeft == 0.0f &&
               tiltFineRight == 0.0f &&
               formantShift == 0.0f &&
               volumeDb == 0.0f &&
               vibrato == 1.0f &&
               pitchDrift == 1.0f &&
               smoothLeftFrames == 0 &&
               smoothRightFrames == 0 &&
               transitionOutMs == 0 &&
               deltaScale == 1.0f &&
               deltaOffset == 0.0f;
    }
};
