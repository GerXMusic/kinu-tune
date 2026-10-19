// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

// End-to-end checks for the harmony buttons through PianoRollComponent:
// target selection, one undo step, untouched pitch curves and range handling.
namespace
{
void prepareAudio(Project& project)
{
    project.getAudioData().sampleRate = SAMPLE_RATE;
    project.getAudioData().f0.assign(400, 440.0f);
    project.getAudioData().basePitch.assign(400, 60.0f);
    project.getAudioData().deltaPitch.assign(400, 0.0f);
}

Note makeNote(int start, int end, float midi, float offset = 0.0f, bool rest = false)
{
    Note note(start, end, midi);
    note.setPitchOffset(offset);
    note.setRest(rest);
    std::vector<float> vibrato(static_cast<size_t>(end - start));
    for (size_t i = 0; i < vibrato.size(); ++i)
        vibrato[i] = 0.3f * std::sin(0.5f * static_cast<float>(i));
    note.setDeltaPitch(vibrato);
    return note;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    using HarmonyUtils::Interval;
    using HarmonyUtils::Result;

    // No selection: every note moves a third up in C major, in one undo step,
    // with the vibrato curve untouched. Drag-snap is chromatic on purpose.
    {
        Project project;
        prepareAudio(project);
        project.addNote(makeNote(0, 40, 60));                 // C
        project.addNote(makeNote(50, 90, 62));                // D
        project.addNote(makeNote(100, 140, 61));              // C#
        project.addNote(makeNote(150, 190, 63.7f, 0.3f));     // E via offset
        project.addNote(makeNote(200, 240, 67, 0.0f, true));  // rest
        project.setProjectKey(0, ScaleMode::Major);
        project.setScaleMode(ScaleMode::Chromatic);

        std::vector<std::vector<float>> curves;
        std::vector<float> before;
        for (auto& note : project.getNotes())
        {
            curves.push_back(note.getDeltaPitch());
            before.push_back(note.getMidiNote());
        }

        PianoRollComponent roll;
        PitchUndoManager history;
        int historyChanges = 0;
        roll.setSize(800, 400);
        roll.setProject(&project);
        roll.setUndoManager(&history);
        history.onHistoryChanged = [&] { ++historyChanges; };

        CHECK(roll.applyHarmonyInterval(Interval::Third, true) == Result::Applied);
        auto& notes = project.getNotes();
        CHECK(notes[0].getMidiNote() == 64.0f);
        CHECK(notes[1].getMidiNote() == 65.0f);
        CHECK(notes[2].getMidiNote() == 65.0f);
        CHECK(std::abs(notes[3].getAdjustedMidiNote() - 67.0f) < 1.0e-4f);
        CHECK(std::abs(notes[3].getPitchOffset() - 0.3f) < 1.0e-6f);
        CHECK(notes[4].getMidiNote() == 67.0f);
        CHECK(historyChanges == 1);
        for (size_t i = 0; i < notes.size(); ++i)
            CHECK(notes[i].getDeltaPitch() == curves[i]);

        history.undo();
        for (size_t i = 0; i < notes.size(); ++i)
            CHECK(notes[i].getMidiNote() == before[i]);
        CHECK(!history.canUndo());
    }

    // With a selection only the selected notes move.
    {
        Project project;
        prepareAudio(project);
        project.addNote(makeNote(0, 40, 60));
        project.addNote(makeNote(50, 90, 62));
        project.setProjectKey(0, ScaleMode::Major);
        project.getNotes()[1].setSelected(true);

        PianoRollComponent roll;
        PitchUndoManager history;
        roll.setSize(800, 400);
        roll.setProject(&project);
        roll.setUndoManager(&history);

        CHECK(roll.applyHarmonyInterval(Interval::Sixth, false) == Result::Applied);
        CHECK(project.getNotes()[0].getMidiNote() == 60.0f); // untouched
        CHECK(project.getNotes()[1].getMidiNote() == 53.0f); // D -> F below
    }

    // Notes that would leave the editor range stay put instead of being
    // clamped onto an out-of-key pitch; the rest still move.
    {
        Project project;
        prepareAudio(project);
        project.addNote(makeNote(0, 40, 88));  // E6, a third up is A6 > F6
        project.addNote(makeNote(50, 90, 74)); // D5 -> F#5
        project.setProjectKey(2, ScaleMode::Major);

        PianoRollComponent roll;
        PitchUndoManager history;
        roll.setSize(800, 400);
        roll.setProject(&project);
        roll.setUndoManager(&history);

        CHECK(roll.applyHarmonyInterval(Interval::Third, true) == Result::PartlyOutOfRange);
        CHECK(project.getNotes()[0].getMidiNote() == 88.0f);
        CHECK(project.getNotes()[1].getMidiNote() == 78.0f);

        project.getNotes()[1].setSelected(false);
        project.getNotes()[0].setSelected(true);
        CHECK(roll.applyHarmonyInterval(Interval::Octave, true) == Result::OutOfRange);
        CHECK(project.getNotes()[0].getMidiNote() == 88.0f);
    }

    // Degrees need a seven-tone key; octaves always work.
    {
        Project project;
        prepareAudio(project);
        project.addNote(makeNote(0, 40, 60));
        project.setProjectKey(0, ScaleMode::MajorPentatonic);

        PianoRollComponent roll;
        PitchUndoManager history;
        roll.setSize(800, 400);
        roll.setProject(&project);
        roll.setUndoManager(&history);

        CHECK(roll.applyHarmonyInterval(Interval::Third, true) == Result::ScaleUnsupported);
        CHECK(roll.applyHarmonyInterval(Interval::Octave, false) == Result::Applied);
        CHECK(project.getNotes()[0].getMidiNote() == 48.0f);
    }

    // Nothing to move.
    {
        Project project;
        prepareAudio(project);
        project.setProjectKey(0, ScaleMode::Major);
        PianoRollComponent roll;
        roll.setSize(800, 400);
        roll.setProject(&project);
        CHECK(roll.applyHarmonyInterval(Interval::Third, true) == Result::NoNotes);
    }

    std::cout << "Harmony apply tests passed\n";
    return 0;
}
