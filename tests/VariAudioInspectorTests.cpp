// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/Audio/ResampledLoopAudition.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/VariAudioInspector.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/PitchMacroScope.h"
#include "../Source/Utils/HarmonyUtils.h"
#include "../Source/Utils/ScaleUtils.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

// Inspector sliders, double-click reset, Option zoom, drag-audition buffer
// and the selection-aware pitch macro (docs/design.md 5.6). The editor scale
// menus were removed from the inspector by D-234; their block went with them.
namespace
{
using Param = PianoRollComponent::InspectorParam;

void prepareAudio(Project& project, int frames = 600)
{
    auto& audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;
    audio.f0.assign(static_cast<size_t>(frames), 261.6f);
    audio.basePitch.assign(static_cast<size_t>(frames), 60.0f);
    audio.deltaPitch.assign(static_cast<size_t>(frames), 0.0f);
}

Note makeNote(int start, int end, float midi)
{
    Note note(start, end, midi);
    note.setSrcStartFrame(start);
    note.setSrcEndFrame(end);
    note.setOriginalMidiNote(midi);
    note.setDeltaPitch(std::vector<float>(static_cast<size_t>(end - start), 0.0f));
    return note;
}

bool near(float a, float b, float tolerance = 1.0e-3f)
{
    return std::abs(a - b) <= tolerance;
}

// The inspector's menus are private; find them the way a user sees them.
template <typename T>
std::vector<T*> findChildren(juce::Component& root)
{
    std::vector<T*> found;
    std::function<void(juce::Component&)> walk = [&](juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (auto* typed = dynamic_cast<T*>(child))
                found.push_back(typed);
            walk(*child);
        }
    };
    walk(root);
    return found;
}

struct Fixture
{
    Project project;
    PianoRollComponent roll;
    PitchUndoManager history;
    Fixture()
    {
        prepareAudio(project);
        project.addNote(makeNote(0, 100, 60.3f));   // 30 cents sharp
        project.addNote(makeNote(100, 200, 61.8f)); // 20 cents flat of D
        project.addNote(makeNote(250, 300, 64.0f));
        roll.setSize(900, 420);
        roll.setProject(&project);
        roll.setUndoManager(&history);
        roll.resized();
    }
    Note& note(size_t i) { return project.getNotes()[i]; }
};
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // --- Targets: the selection, otherwise every segment -------------------
    {
        Fixture f;
        CHECK(f.roll.getInspectorTargets().size() == 3);
        f.note(1).setSelected(true);
        CHECK(f.roll.getInspectorTargets().size() == 1);
        CHECK(f.roll.getInspectorTargets().front() == &f.note(1));
    }

    // --- Correct Pitch: proportional pull toward the semitone, one undo ----
    {
        Fixture f;
        f.note(0).setSelected(true);
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f));
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(50.0f);
        CHECK(near(f.note(0).getMidiNote(), 60.15f));
        f.roll.setInspectorEditValue(100.0f);
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        f.roll.endInspectorEdit();
        CHECK(near(f.note(1).getMidiNote(), 61.8f)); // not selected: untouched
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        // A later gesture measures from the sung pitch, not the corrected one.
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(0.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
        CHECK(!f.history.canUndo());
    }

    // --- Flatten, drift, formant: one value for all targets, undoable ------
    {
        Fixture f;
        f.roll.beginInspectorEdit(Param::FlattenCurve);
        f.roll.setInspectorEditValue(75.0f);
        f.roll.endInspectorEdit();
        for (size_t i = 0; i < 3; ++i)
            CHECK(near(f.note(i).getVibrato(), 0.25f));
        CHECK(near(*f.roll.getInspectorValue(Param::FlattenCurve), 75.0f));

        f.roll.beginInspectorEdit(Param::PitchDrift);
        f.roll.setInspectorEditValue(40.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(2).getPitchDrift(), 0.6f));

        f.roll.beginInspectorEdit(Param::Formant);
        f.roll.setInspectorEditValue(-2.5f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(1).getFormantShift(), -2.5f));

        f.history.undo();
        f.history.undo();
        f.history.undo();
        for (size_t i = 0; i < 3; ++i)
        {
            CHECK(near(f.note(i).getVibrato(), 1.0f));
            CHECK(near(f.note(i).getPitchDrift(), 1.0f));
            CHECK(near(f.note(i).getFormantShift(), 0.0f));
        }
    }

    // --- Volume goes through the gain-only action --------------------------
    {
        Fixture f;
        f.note(2).setSelected(true);
        f.roll.beginInspectorEdit(Param::Volume);
        f.roll.setInspectorEditValue(-6.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(2).getVolumeDb(), -6.0f));
        CHECK(near(f.note(0).getVolumeDb(), 0.0f));
        f.history.undo();
        CHECK(near(f.note(2).getVolumeDb(), 0.0f));
    }

    // --- Tilt moved from the note overlay to the inspector ----------------
    {
        Fixture f;
        f.note(0).setSelected(true);
        const float beforeMidi = f.note(0).getMidiNote();
        CHECK(near(*f.roll.getInspectorValue(Param::TiltLeft), 0.0f));
        f.roll.beginInspectorEdit(Param::TiltLeft);
        f.roll.setInspectorEditValue(1.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(0).getTiltLeft(), 1.0f));
        CHECK(near(*f.roll.getInspectorValue(Param::TiltLeft), 1.0f));
        CHECK(f.history.getUndoName() == "Tilt Left");
        f.history.undo();
        CHECK(near(f.note(0).getTiltLeft(), 0.0f));
        CHECK(near(f.note(0).getMidiNote(), beforeMidi));

        f.roll.beginInspectorEdit(Param::TiltRight);
        f.roll.setInspectorEditValue(-1.5f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(0).getTiltRight(), -1.5f));
        CHECK(near(*f.roll.getInspectorValue(Param::TiltRight), -1.5f));
        CHECK(f.history.getUndoName() == "Tilt Right");
        f.history.undo();
        CHECK(near(f.note(0).getTiltRight(), 0.0f));
        CHECK(near(f.note(0).getMidiNote(), beforeMidi));
    }

    // --- Glue moved from the note overlay to the inspector ----------------
    {
        Fixture f;
        CHECK(!f.roll.canGlueTarget(true)); // no selection: never "all notes"
        f.note(0).setSelected(true);
        CHECK(!f.roll.canGlueTarget(false)); // no previous note
        CHECK(f.roll.canGlueTarget(true));   // note 0 touches note 1
        const size_t before = f.project.getNotes().size();
        CHECK(f.roll.glueTargetNote(true));
        CHECK(f.project.getNotes().size() == before - 1);
        CHECK(f.history.canUndo());
        f.history.undo();
        CHECK(f.project.getNotes().size() == before);
    }

    // --- Transition: one undo step, valid action name ----------------------
    // Regression: the undo-name table had five entries, so releasing this
    // slider indexed past it and built the action name from garbage (crash).
    // D-223: it is the only transition control (Blend was folded into the
    // engine share). D-225: the value belongs to the union with the NEXT note,
    // so note 0 (which touches note 1) is the one selected.
    {
        Fixture f;
        f.note(0).setSelected(true); // 0-100, legato into note 1

        f.roll.beginInspectorEdit(Param::Transition);
        f.roll.setInspectorEditValue(120.0f);
        f.roll.endInspectorEdit();
        CHECK(f.note(0).getTransitionOutMs() == 120);
        CHECK(f.note(1).getTransitionOutMs() == 0); // never the neighbour
        CHECK(f.history.canUndo());
        CHECK(f.history.getUndoName() == "Transition");
        f.history.undo();
        CHECK(f.note(0).getTransitionOutMs() == 0);
        CHECK(!f.history.canUndo());
    }

    // --- D-225: Transition requires a selection -----------------------------
    // Without a selection the other sliders act on every segment; Transition
    // must not, or one gesture would rewrite the whole project's unions.
    {
        Fixture f;
        CHECK(!f.roll.getInspectorValue(Param::Transition).has_value());
        f.roll.beginInspectorEdit(Param::Transition);
        f.roll.setInspectorEditValue(200.0f);
        f.roll.endInspectorEdit();
        for (size_t i = 0; i < 3; ++i)
            CHECK(f.note(i).getTransitionOutMs() == 0);
        CHECK(!f.history.canUndo());
    }

    // --- D-225: split moves the transition to the tail ----------------------
    // The union with the next note belongs to the second part; the new inner
    // union (first part -> second part) starts at 0.
    {
        Fixture f;
        f.note(0).setTransitionOutMs(180);
        NoteSplitter splitter;
        splitter.setProject(&f.project);
        CHECK(splitter.splitNoteAtFrame(&f.note(0), 40));
        CHECK(f.note(0).getEndFrame() == 40);
        CHECK(f.note(0).getTransitionOutMs() == 0);
        CHECK(f.note(1).getStartFrame() == 40);
        CHECK(f.note(1).getEndFrame() == 100);
        CHECK(f.note(1).getTransitionOutMs() == 180);
    }

    // --- A gesture that changes nothing leaves no undo step -----------------
    {
        Fixture f;
        f.roll.beginInspectorEdit(Param::Formant);
        f.roll.setInspectorEditValue(0.0f);
        f.roll.endInspectorEdit();
        CHECK(!f.history.canUndo());
    }

    auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto event = [&source](juce::Component& c, juce::Point<float> at,
                                 juce::ModifierKeys mods, int clicks)
    {
        return juce::MouseEvent(source, at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                juce::Time::getCurrentTime(), at,
                                juce::Time::getCurrentTime(), clicks, false);
    };

    // --- Double-click a segment: back to the analysed state ----------------
    {
        Fixture f;
        f.note(1).setMidiNote(63.0f);
        f.note(1).setVibrato(0.2f);
        f.note(1).setFormantShift(3.0f);
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        const float x = PianoRollComponent::pianoKeysWidth +
                        static_cast<float>(framesToSeconds(150)) * f.roll.getPixelsPerSecond() -
                        static_cast<float>(f.roll.getScrollX());
        const float y = PianoRollComponent::headerHeight +
                        (MAX_MIDI_NOTE - 63.0f) * f.roll.getPixelsPerSemitone() +
                        f.roll.getPixelsPerSemitone() * 0.5f -
                        static_cast<float>(f.roll.getScrollY());
        // Restablecer: Option/Alt + doble clic (el doble clic simple corta el segmento).
        f.roll.mouseDoubleClick(event(f.roll, {x, y},
                                   juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                                      juce::ModifierKeys::leftButtonModifier), 2));
        CHECK(near(f.note(1).getMidiNote(), 61.8f));
        CHECK(near(f.note(1).getVibrato(), 1.0f));
        CHECK(near(f.note(1).getFormantShift(), 0.0f));
        CHECK(f.history.canUndo());
        f.history.undo();
        CHECK(near(f.note(1).getMidiNote(), 63.0f));
    }

    // --- Option/Alt: vertical zoom only (wheel and drag) -------------------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(50.0f, 70.0f);
        const float ppsBefore = f.roll.getPixelsPerSecond();
        const float semitoneBefore = f.roll.getPixelsPerSemitone();
        const juce::Point<float> at(400.0f, 200.0f);
        juce::MouseWheelDetails wheel{};
        wheel.deltaY = 0.5f;
        f.roll.mouseWheelMove(event(f.roll, at, juce::ModifierKeys::altModifier, 0), wheel);
        CHECK(f.roll.getPixelsPerSemitone() > semitoneBefore);
        CHECK(near(f.roll.getPixelsPerSecond(), ppsBefore));

        const float semitoneMid = f.roll.getPixelsPerSemitone();
        const auto altLeft = juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                                juce::ModifierKeys::leftButtonModifier);
        // Start the drag on empty canvas (no segment there).
        const juce::Point<float> start(860.0f, 120.0f);
        f.roll.mouseDown(event(f.roll, start, altLeft, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(40.0f, -60.0f), altLeft, 1));
        f.roll.mouseUp(event(f.roll, start + juce::Point<float>(40.0f, -60.0f), altLeft, 1));
        CHECK(f.roll.getPixelsPerSemitone() > semitoneMid);
        CHECK(near(f.roll.getPixelsPerSecond(), ppsBefore));
    }

    // --- Drag feedback: the whole segment, then a short silent gap ---------
    {
        std::vector<float> sourceAudio(22050);
        for (size_t i = 0; i < sourceAudio.size(); ++i)
            sourceAudio[i] = 0.5f * std::sin(0.06f * static_cast<float>(i));
        juce::AudioBuffer<float> received;
        bool done = false;
        ResampledLoopAudition audition([&](juce::AudioBuffer<float> buffer)
                                       { received = std::move(buffer); done = true; });
        audition.request(sourceAudio, 12.0f, 72.0f, true); // an octave up: half length
        for (int i = 0; i < 100 && !done; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        CHECK(done);
        const int padding = static_cast<int>(ResampledLoopAudition::loopGapSeconds * 44100.0);
        CHECK(received.getNumSamples() == 11025 + padding);
        float tailPeak = 0.0f;
        for (int i = 11025; i < received.getNumSamples(); ++i)
            tailPeak = std::max(tailPeak, std::abs(received.getSample(0, i)));
        CHECK(tailPeak == 0.0f);
        CHECK(received.getMagnitude(0, 1000, 9000) > 0.1f); // sound in the body
    }

    // --- Pitch macro scope honours a selection in track mode ---------------
    {
        Project a, b;
        prepareAudio(a);
        prepareAudio(b);
        a.addNote(makeNote(0, 100, 60.0f));
        b.addNote(makeNote(0, 100, 62.0f));
        b.addNote(makeNote(100, 200, 64.0f));
        b.getNotes()[1].setSelected(true);
        const std::vector<juce::Range<int>> window{{0, 600}};
        bool onlySelected = hasSelectedPitchMacroNotes(a, true, window) ||
                            hasSelectedPitchMacroNotes(b, true, window);
        CHECK(onlySelected);
        CHECK(collectPitchMacroTargetNotes(a, true, window, onlySelected).empty());
        const auto bTargets = collectPitchMacroTargetNotes(b, true, window, onlySelected);
        CHECK(bTargets.size() == 1 && bTargets.front() == &b.getNotes()[1]);
        CHECK(collectPitchMacroTargetNotes(b, true, window, false).size() == 2);
    }

    // --- Correct Pitch survives harmony, arrow keys and snapped drags ----
    // (user report 2026-10-19: after a harmony shift Correct Pitch showed 0%
    // and the slider did nothing, because the move overwrote the sung pitch
    // the correction is measured from with the already-corrected pitch.)
    {
        Fixture f;
        f.project.setProjectKey(0, ScaleMode::Major); // C major
        f.note(0).setSelected(true);                  // sung 30 cents sharp of C
        const auto correctTo = [&f](float percent)
        {
            f.roll.beginInspectorEdit(Param::CorrectPitch);
            f.roll.setInspectorEditValue(percent);
            f.roll.endInspectorEdit();
        };
        correctTo(100.0f);
        CHECK(near(f.note(0).getMidiNote(), 60.0f));

        // Third up: C -> E. Still 100% corrected, and the slider still works.
        CHECK(f.roll.applyHarmonyInterval(HarmonyUtils::Interval::Third, true) ==
              HarmonyUtils::Result::Applied);
        CHECK(near(f.note(0).getMidiNote(), 64.0f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        correctTo(0.0f);
        CHECK(near(f.note(0).getMidiNote(), 64.3f)); // the sung 30 cents, at E
        correctTo(100.0f);
        CHECK(near(f.note(0).getMidiNote(), 64.0f));

        // Undo walks the reference back too.
        f.history.undo(); // correct 100
        f.history.undo(); // correct 0
        f.history.undo(); // harmony
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        f.history.redo(); // harmony again
        CHECK(near(f.note(0).getMidiNote(), 64.0f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 64.3f));

        // Arrow key (Absolute: next semitone) keeps the correction too.
        CHECK(f.roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(near(f.note(0).getMidiNote(), 65.0f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        // Shift+arrow (cents) toward the sung side eats into the correction.
        CHECK(f.roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey,
                                               juce::ModifierKeys::shiftModifier, 0)));
        CHECK(near(f.note(0).getMidiNote(), 65.01f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 96.67f, 0.05f));
    }

    // --- Pitch drags: snapped keeps the sung pitch, free places it ---------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto drag = [&](float fromMidi, float semitones, juce::ModifierKeys mods)
        {
            const juce::Point<float> start(xAt(50), yAt(fromMidi));
            const auto end = start + juce::Point<float>(0.0f, -semitones * f.roll.getPixelsPerSemitone());
            const auto held = juce::ModifierKeys(mods.getRawFlags() | left.getRawFlags());
            f.roll.mouseDown(event(f.roll, start, held, 1));
            f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -6.0f), held, 1));
            f.roll.mouseDrag(event(f.roll, end, held, 1));
            f.roll.mouseUp(event(f.roll, end, held, 1));
        };

        // Absolute drag up two semitones: lands on D, counts as fully corrected.
        drag(60.3f, 2.0f, {});
        CHECK(near(f.note(0).getMidiNote(), 62.0f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 62.3f));
        f.note(0).setSelected(true);
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        f.note(0).setSelected(false);
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));

        // Free drag (Shift = snap off): the placed pitch is the new reference.
        drag(60.3f, 1.37f, juce::ModifierKeys::shiftModifier);
        const float placed = f.note(0).getMidiNote();
        CHECK(std::abs(placed - 61.67f) < 0.15f);
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), placed));
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
    }

    // --- Out and back leaves an unedited segment unedited ------------------
    // (review 2026-10-19: cents nudges past the sung pitch and back used to
    // leave the reference 5 cents off, so an unedited note read as 14%.)
    {
        Fixture f;
        f.note(0).setSelected(true); // sung and analysed at 60.3
        const auto key = [&f](int code, bool shift)
        {
            return f.roll.keyPressed(juce::KeyPress(
                code, shift ? juce::ModifierKeys::shiftModifier : juce::ModifierKeys(), 0));
        };
        const auto unedited = [&f]
        {
            return near(f.note(0).getMidiNote(), 60.3f) &&
                   near(f.note(0).getLastNonMacroMidiNote(), 60.3f) &&
                   near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f);
        };
        for (int i = 0; i < 5; ++i) CHECK(key(juce::KeyPress::upKey, true));
        CHECK(near(f.note(0).getMidiNote(), 60.35f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f));
        for (int i = 0; i < 5; ++i) CHECK(key(juce::KeyPress::downKey, true));
        CHECK(unedited());

        // Absolute Up (lands on C#, 100%), then 70 cents back down.
        CHECK(key(juce::KeyPress::upKey, false));
        CHECK(near(f.note(0).getMidiNote(), 61.0f));
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 100.0f));
        for (int i = 0; i < 70; ++i) CHECK(key(juce::KeyPress::downKey, true));
        CHECK(unedited());

        // Down past the semitone and back up.
        for (int i = 0; i < 40; ++i) CHECK(key(juce::KeyPress::downKey, true));
        for (int i = 0; i < 40; ++i) CHECK(key(juce::KeyPress::upKey, true));
        CHECK(unedited());
    }

    // --- A pitch beyond 100% shows 0% and the slider starts there ---------
    {
        Fixture f;
        f.note(0).setSelected(true);
        // Overcorrected: sung 60.2, now 59.9 (past C). Not a 0-100% correction.
        f.note(0).setMidiNote(59.9f);
        f.note(0).setLastNonMacroMidiNote(60.2f);
        CHECK(near(*f.roll.getInspectorValue(Param::CorrectPitch), 0.0f));
        // Touching the slider without moving it changes nothing, no undo step.
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.endInspectorEdit();
        CHECK(!f.history.canUndo());
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.2f));
        // A small move stays small (no jump) and is one undo step.
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(10.0f);
        f.roll.endInspectorEdit();
        CHECK(std::abs(f.note(0).getMidiNote() - 59.9f) < 0.02f);
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 59.9f));
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.2f));
    }

    // --- Multi-note drags and splits keep every share within 0-100% -------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto held = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto shareOf = [&f](const Note& note)
        {
            return ScaleUtils::correctionShare(f.project, note.getLastNonMacroMidiNote(),
                                               note.getMidiNote(), 440);
        };
        // Grab note 0 (60.3) with note 1 (61.8) also selected; Absolute lands
        // the grabbed one on D and moves the other by the same amount.
        f.note(0).setSelected(true);
        f.note(1).setSelected(true);
        const juce::Point<float> start(xAt(50), yAt(60.3f));
        const auto end = start + juce::Point<float>(0.0f, -2.0f * f.roll.getPixelsPerSemitone());
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -6.0f), held, 1));
        f.roll.mouseDrag(event(f.roll, end, held, 1));
        f.roll.mouseUp(event(f.roll, end, held, 1));
        CHECK(near(f.note(0).getMidiNote(), 62.0f));
        CHECK(shareOf(f.note(0)).has_value() && near(*shareOf(f.note(0)), 1.0f));
        CHECK(near(f.note(1).getMidiNote(), 63.5f));
        CHECK(shareOf(f.note(1)).has_value()); // within 0-100%, no jump
        f.history.undo();
        CHECK(near(f.note(0).getLastNonMacroMidiNote(), 60.3f));
        CHECK(near(f.note(1).getLastNonMacroMidiNote(), 61.8f));

        // Correct to 100%, then a double-click splits: both halves stay
        // corrected (splitting is pitch-neutral).
        f.note(1).setSelected(false);
        f.roll.beginInspectorEdit(Param::CorrectPitch);
        f.roll.setInspectorEditValue(100.0f);
        f.roll.endInspectorEdit();
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        const size_t before = f.project.getNotes().size();
        const auto left = juce::ModifierKeys::leftButtonModifier;
        // Primer clic (selecciona) y segundo clic del doble clic (corta).
        // El corte es el doble clic en la franja SUPERIOR de la nota (Melodyne); en
        // el cuerpo el doble clic afina. Aqui se apunta a la franja superior.
        const auto yTop = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.15f -
                   static_cast<float>(f.roll.getScrollY());
        };
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop(60.0f)}, left, 1));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop(60.0f)}, left, 1));
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop(60.0f)}, left, 2));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop(60.0f)}, left, 2));
        CHECK(f.project.getNotes().size() == before + 1);
        for (size_t i = 0; i < 2; ++i)
        {
            const auto share = shareOf(f.note(i));
            CHECK(share.has_value() && *share > 0.9f);
        }
    }

    // --- A right click on the cycle ruler does not hijack the next drag ----
    {
        Fixture f;
        f.roll.fitPitchRangeToView(56.0f, 68.0f);
        const auto loopBefore = f.project.getLoopRange();
        const juce::Point<float> ruler(400.0f, (PianoRollComponent::timelineHeight +
                                                PianoRollComponent::headerHeight) * 0.5f);
        const auto right = juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier);
        f.roll.mouseDown(event(f.roll, ruler, right, 1));
        f.roll.mouseUp(event(f.roll, ruler, right, 1));
        const auto held = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        const float x = PianoRollComponent::pianoKeysWidth +
                        static_cast<float>(framesToSeconds(50)) * f.roll.getPixelsPerSecond() -
                        static_cast<float>(f.roll.getScrollX());
        const float y = PianoRollComponent::headerHeight +
                        (MAX_MIDI_NOTE - 60.3f) * f.roll.getPixelsPerSemitone() +
                        f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        const juce::Point<float> start(x, y);
        const auto end = start + juce::Point<float>(0.0f, -2.0f * f.roll.getPixelsPerSemitone());
        f.roll.mouseDown(event(f.roll, start, held, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -6.0f), held, 1));
        f.roll.mouseDrag(event(f.roll, end, held, 1));
        f.roll.mouseUp(event(f.roll, end, held, 1));
        CHECK(near(f.note(0).getMidiNote(), 62.0f)); // the drag moved the note
        const auto loopAfter = f.project.getLoopRange();
        CHECK(loopAfter.enabled == loopBefore.enabled &&
              near(static_cast<float>(loopAfter.startSeconds), static_cast<float>(loopBefore.startSeconds)) &&
              near(static_cast<float>(loopAfter.endSeconds), static_cast<float>(loopBefore.endSeconds)));
    }

    // --- Defaults: Absolute snap, all smart controls, scale shown ---------
    {
        Project project;
        prepareAudio(project);
        CHECK(project.getSnapToSemitones());
        CHECK(project.getScaleMode() != ScaleMode::Chromatic &&
              project.getScaleMode() != ScaleMode::None);
        PianoRollComponent roll;
        CHECK(roll.getShowAllSmartControls());
        CHECK(roll.getAbsoluteDragSnap());

        VariAudioInspector inspector;
        inspector.getCurrentProject = [&project] { return &project; };
        inspector.refresh();
        juce::ComboBox* smartBox = nullptr;
        juce::ComboBox* snapBox = nullptr;
        for (auto* box : findChildren<juce::ComboBox>(inspector))
        {
            if (box->getNumItems() == 2)
                smartBox = box;
            else if (box->getNumItems() == 3)
                snapBox = box;
        }
        CHECK(smartBox != nullptr && snapBox != nullptr);
        CHECK(smartBox->getSelectedId() == 2); // all smart controls
        CHECK(snapBox->getSelectedId() == 1);  // Absolute
    }

    // --- Option-double-click on empty space: zoom to show everything ------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        int requests = 0;
        f.roll.onZoomFullRequested = [&requests] { ++requests; };
        const auto altLeft = juce::ModifierKeys(juce::ModifierKeys::altModifier |
                                                juce::ModifierKeys::leftButtonModifier);
        const auto yFor = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f - static_cast<float>(f.roll.getScrollY());
        };
        const auto xFor = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        // No segment at frame 400 (the last one ends at 300).
        f.roll.mouseDoubleClick(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 2));
        CHECK(requests == 1);
        // On a segment it is not a zoom.
        f.roll.mouseDoubleClick(event(f.roll, {xFor(50), yFor(60.3f)}, altLeft, 2));
        CHECK(requests == 1);
        // Without Option it is not a zoom either.
        f.roll.mouseDoubleClick(event(f.roll, {xFor(400), yFor(60.0f)},
                                      juce::ModifierKeys::leftButtonModifier, 2));
        CHECK(requests == 1);

        // Un doble clic cuyo primer clic cayo en la franja superior de un segmento
        // (y lo corto) no es un zoom, aunque al final el puntero quede en vacio.
        const size_t notesBefore = f.project.getNotes().size();
        const auto left = juce::ModifierKeys::leftButtonModifier;
        const float yTop = yFor(60.3f) - f.roll.getPixelsPerSemitone() * 0.35f;
        f.roll.mouseDown(event(f.roll, {xFor(50), yTop}, left, 1));
        f.roll.mouseUp(event(f.roll, {xFor(50), yTop}, left, 1));
        f.roll.mouseDown(event(f.roll, {xFor(50), yTop}, left, 2));
        f.roll.mouseUp(event(f.roll, {xFor(50), yTop}, left, 2));
        CHECK(f.project.getNotes().size() == notesBefore + 1); // doble clic: corte
        f.roll.mouseDoubleClick(event(f.roll, {xFor(50), yFor(64.0f)}, altLeft, 2));
        CHECK(requests == 1);
        // The next double-click that starts on empty space zooms again.
        f.roll.mouseDown(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 1));
        f.roll.mouseUp(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 1));
        f.roll.mouseDoubleClick(event(f.roll, {xFor(400), yFor(60.0f)}, altLeft, 2));
        CHECK(requests == 2);
    }

    // --- Doble clic en el cuerpo afina; en la franja superior corta (D-281) ----
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f -
                   static_cast<float>(f.roll.getScrollY());
        };
        const auto left = juce::ModifierKeys::leftButtonModifier;
        CHECK(near(f.note(0).getMidiNote(), 60.3f));

        // Cuerpo: afina al 100 % hacia el semitono mas cercano, y es undoable.
        f.roll.mouseDoubleClick(event(f.roll, {xAt(50), yAt(60.3f)}, left, 2));
        CHECK(near(f.note(0).getMidiNote(), 60.0f));
        CHECK(f.history.canUndo());
        f.history.undo();
        CHECK(near(f.note(0).getMidiNote(), 60.3f));

        // Franja superior: corta en el puntero, no afina. El orden es el real de
        // JUCE: down(1) -> up -> down(2) -> doubleClick -> up.
        const size_t before = f.project.getNotes().size();
        const float yTop = yAt(60.3f) - f.roll.getPixelsPerSemitone() * 0.35f;
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop}, left, 1));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop}, left, 1));
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop}, left, 2));
        f.roll.mouseDoubleClick(event(f.roll, {xAt(50), yTop}, left, 2));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop}, left, 2));
        // D-285/2: el corte se queda; el doubleClick de este mismo gesto no lo une.
        CHECK(f.project.getNotes().size() == before + 1);
        CHECK(near(f.note(0).getMidiNote(), 60.3f)); // el corte no afina
    }

    // --- D-285 (2.1/2.2): recuadro en vacio y unir en el corte --------------
    {
        Fixture f;
        f.roll.fitPitchRangeToView(58.0f, 66.0f);
        const auto left = juce::ModifierKeys::leftButtonModifier;
        const auto xAt = [&f](int frame)
        {
            return PianoRollComponent::pianoKeysWidth +
                   static_cast<float>(framesToSeconds(frame)) * f.roll.getPixelsPerSecond() -
                   static_cast<float>(f.roll.getScrollX());
        };
        const auto yAt = [&f](float midi)
        {
            return PianoRollComponent::headerHeight +
                   (MAX_MIDI_NOTE - midi) * f.roll.getPixelsPerSemitone() +
                   f.roll.getPixelsPerSemitone() * 0.5f -
                   static_cast<float>(f.roll.getScrollY());
        };

        // 2.1: arrastrar con el clic izquierdo en vacio dibuja el recuadro y
        // selecciona lo que toca; no panea. El hueco 200-250 esta vacio.
        const float scrollBefore = f.roll.getScrollX();
        const juce::Point<float> start(xAt(205), yAt(56.5f));
        const juce::Point<float> end(xAt(320), yAt(65.0f));
        f.roll.mouseDown(event(f.roll, start, left, 1));
        f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(15.0f, 10.0f), left, 1));
        f.roll.mouseDrag(event(f.roll, end, left, 1));
        f.roll.mouseUp(event(f.roll, end, left, 1));
        CHECK(near(f.roll.getScrollX(), scrollBefore)); // no es la mano
        CHECK(f.note(2).isSelected());                  // la cubre el recuadro
        CHECK(!f.note(0).isSelected());
        CHECK(!f.note(1).isSelected());

        // 2.2: cortar y volver a unir con doble clic en la frontera, en un paso.
        // Se usan los dos ordenes reales: el gesto de corte completo (con su
        // doubleClick) y luego el gesto de union.
        const float yTop = yAt(60.3f) - f.roll.getPixelsPerSemitone() * 0.35f;
        const size_t notesBefore = f.project.getNotes().size();
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop}, left, 1));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop}, left, 1));
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop}, left, 2));
        f.roll.mouseDoubleClick(event(f.roll, {xAt(50), yTop}, left, 2));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop}, left, 2));
        CHECK(f.project.getNotes().size() == notesBefore + 1);
        // El primer clic del siguiente doble clic tampoco deja otro corte.
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop}, left, 1));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop}, left, 1));
        CHECK(f.project.getNotes().size() == notesBefore + 1);
        // Doble clic sobre el corte: une (y no afina).
        f.roll.mouseDown(event(f.roll, {xAt(50), yTop}, left, 2));
        f.roll.mouseDoubleClick(event(f.roll, {xAt(50), yTop}, left, 2));
        f.roll.mouseUp(event(f.roll, {xAt(50), yTop}, left, 2));
        CHECK(f.project.getNotes().size() == notesBefore);
        CHECK(near(f.note(0).getMidiNote(), 60.3f));
        // Un solo paso de undo vuelve al corte.
        CHECK(f.history.canUndo());
        f.history.undo();
        CHECK(f.project.getNotes().size() == notesBefore + 1);
    }

    // --- D-299: el comando Eliminar deja lapidas, no toca el vector ---------
    {
        Fixture f;
        f.note(0).setSelected(true);
        f.note(1).setSelected(true);
        const size_t notesBefore = f.project.getNotes().size();
        CHECK(f.roll.deleteSelection());
        CHECK(f.project.getNotes().size() == notesBefore);
        CHECK(f.note(0).isDeleted());
        CHECK(f.note(0).isRest());
        CHECK(!f.note(0).isSelected());
        CHECK(f.note(1).isDeleted());
        CHECK(!f.note(2).isDeleted());
        f.project.selectAllNotes(/*includeRests=*/true);
        CHECK(!f.note(0).isSelected());
        CHECK(!f.note(1).isSelected());
        CHECK(f.note(2).isSelected());
        CHECK(f.history.canUndo());
        CHECK(f.history.getUndoName() == "Delete Notes");
        CHECK(f.history.undo());
        CHECK(!f.note(0).isDeleted());
        CHECK(!f.note(0).isRest());
        CHECK(f.note(0).isSelected());
        CHECK(!f.note(1).isDeleted());
        CHECK(f.note(1).isSelected());
        CHECK(f.history.canRedo());
        CHECK(f.history.redo());
        CHECK(f.note(0).isDeleted());
        CHECK(!f.note(0).isSelected());
    }

    std::cout << "VariAudio inspector tests passed\n";
    return 0;
}
