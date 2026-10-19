// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/TransformParams.h"
#include <algorithm>
#include <limits>
#include <vector>

// One inspector slider gesture (Correct Pitch, Flatten Curve, Pitch Drift,
// Formant) applied to a set of segments. Like PitchToolAction, but it also
// restores each note's "last non-macro" pitch, which Correct Pitch measures
// from; applyToNote() would otherwise overwrite it with the corrected pitch.
class InspectorEditAction : public UndoableAction
{
public:
    InspectorEditAction(Project *project, std::vector<Note *> notes,
                        std::vector<TransformParams> oldParams,
                        std::vector<TransformParams> newParams,
                        std::vector<float> oldSourceMidi,
                        std::vector<float> newSourceMidi, juce::String name)
        : project(project), notes(std::move(notes)),
          oldParams(std::move(oldParams)), newParams(std::move(newParams)),
          oldSourceMidi(std::move(oldSourceMidi)),
          newSourceMidi(std::move(newSourceMidi)), name(std::move(name))
    {
    }

    void undo() override { apply(oldParams, oldSourceMidi); }
    void redo() override { apply(newParams, newSourceMidi); }
    juce::String getName() const override { return name; }

private:
    void apply(const std::vector<TransformParams> &params,
               const std::vector<float> &sourceMidi)
    {
        int minFrame = std::numeric_limits<int>::max();
        int maxFrame = std::numeric_limits<int>::min();
        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto *note = notes[i];
            if (!note || i >= params.size())
                continue;
            params[i].applyToNote(*note);
            if (i < sourceMidi.size())
                note->setLastNonMacroMidiNote(sourceMidi[i]);
            note->markDirty();
            note->markSynthDirty();
            minFrame = std::min(minFrame, note->getStartFrame());
            maxFrame = std::max(maxFrame, note->getEndFrame());
        }
        if (project && maxFrame >= minFrame)
        {
            PitchCurveProcessor::rebuildBaseFromNotes(*project);
            project->markNoteEditDirtyRange(minFrame, maxFrame);
        }
    }

    Project *project;
    std::vector<Note *> notes;
    std::vector<TransformParams> oldParams;
    std::vector<TransformParams> newParams;
    std::vector<float> oldSourceMidi;
    std::vector<float> newSourceMidi;
    juce::String name;
};
