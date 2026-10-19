// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/PianoRoll/PitchToolHandles.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

// VariAudio-style operation: smart-control layout, pitch snap modes and the
// keyboard (docs/design.md 5.1-5.3).
namespace
{
using Kind = PitchEditor::DragSnapKind;
using Type = PitchToolHandles::HandleType;

void prepareAudio(Project& project)
{
    project.getAudioData().sampleRate = SAMPLE_RATE;
    project.getAudioData().f0.assign(600, 261.6f);
    project.getAudioData().basePitch.assign(600, 60.0f);
    project.getAudioData().deltaPitch.assign(600, 0.0f);
}

Note makeNote(int start, int end, float midi)
{
    Note note(start, end, midi);
    note.setSrcStartFrame(start);
    note.setSrcEndFrame(end);
    note.setDeltaPitch(std::vector<float>(static_cast<size_t>(end - start), 0.0f));
    return note;
}

const PitchToolHandles::Handle* findHandle(const PitchToolHandles& handles, Type type)
{
    for (const auto& handle : handles.getHandles())
        if (handle.type == type)
            return &handle;
    return nullptr;
}

bool near(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // --- Smart controls sit on the segment rectangle ---------------------
    {
        CoordinateMapper mapper;
        mapper.setPixelsPerSecond(200.0f);
        mapper.setPixelsPerSemitone(24.0f);
        Note note = makeNote(100, 200, 64.0f);
        const float left = mapper.timeToX(mapper.framesToSeconds(100));
        const float right = mapper.timeToX(mapper.framesToSeconds(200));
        const float top = mapper.midiToY(64.0f);
        const float bottom = top + 24.0f;

        PitchToolHandles handles;
        handles.updateHandles({&note}, mapper);
        // Default set without neighbours: straighten and correct pitch. No
        // split line: a click on the segment only selects it.
        CHECK(handles.getHandles().size() == 2);
        const auto* straighten = findHandle(handles, Type::Vibrato);
        const auto* correct = findHandle(handles, Type::CorrectPitch);
        CHECK(straighten && correct);
        CHECK(near(straighten->bounds.getCentreX(), (left + right) * 0.5f));
        CHECK(straighten->bounds.getBottom() <= top);
        CHECK(near(correct->bounds.getCentreX(), (left + right) * 0.5f));
        CHECK(correct->bounds.getY() >= bottom);
        CHECK(!findHandle(handles, Type::TiltLeft));

        // Glue appears only where a contiguous neighbour exists.
        handles.setOptions({false, true, false});
        handles.updateHandles({&note}, mapper);
        CHECK(findHandle(handles, Type::GlueLeft) != nullptr);
        CHECK(findHandle(handles, Type::GlueRight) == nullptr);
        CHECK(findHandle(handles, Type::GlueLeft)->bounds.getRight() <= left);

        // "Show all" adds tilt, formant and volume.
        handles.setOptions({true, true, true});
        handles.updateHandles({&note}, mapper);
        CHECK(handles.getHandles().size() == 8);
        CHECK(findHandle(handles, Type::TiltLeft)->bounds.getBottom() <= top);
        CHECK(findHandle(handles, Type::Formant)->bounds.getBottom() <= bottom);
        CHECK(findHandle(handles, Type::Amplitude)->bounds.getRight() <= right);
        CHECK(handles.hitTest((left + right) * 0.5f, top - 7.5f) >= 0);

        // The segment body never belongs to the control layout (regression:
        // track gestures and Shift-click were swallowed on hovered segments).
        CHECK(!handles.containsLayoutPoint((left + right) * 0.5f, top + 12.0f));
        CHECK(handles.containsLayoutPoint((left + right) * 0.5f, top - 7.5f));

        // Like VariAudio, controls appear only once the segment is zoomed in.
        mapper.setPixelsPerSemitone(12.0f);
        handles.updateHandles({&note}, mapper);
        CHECK(handles.isEmpty());
        mapper.setPixelsPerSemitone(24.0f);
        Note narrow = makeNote(100, 102, 64.0f); // ~4 px wide
        handles.updateHandles({&narrow}, mapper);
        CHECK(handles.isEmpty());
        Note medium = makeNote(100, 112, 64.0f); // ~28 px: no corner controls
        handles.updateHandles({&medium}, mapper);
        CHECK(!handles.isEmpty() && findHandle(handles, Type::TiltLeft) == nullptr);
    }

    // --- Pitch snap modes ------------------------------------------------
    {
        Project project;
        prepareAudio(project);
        project.setProjectKey(0, ScaleMode::Major);
        PitchEditor editor;
        editor.setProject(&project);

        // Anchor 60.3 (30 cents sharp) dragged up by 1.4 semitones.
        CHECK(near(editor.resolveDragOffset(1.4f, 60.3f, Kind::Off), 1.4f));
        CHECK(near(editor.resolveDragOffset(1.4f, 60.3f, Kind::Relative), 1.0f));
        CHECK(near(editor.resolveDragOffset(1.4f, 60.3f, Kind::Absolute), 1.7f)); // lands on 62
        editor.setDragSnapMode(DragSnapMode::Scale);
        // 60.3 + 0.9 = 61.2; the nearest C-major tone is D (62), 1.7 above.
        CHECK(near(editor.resolveDragOffset(0.9f, 60.3f, Kind::Absolute), 1.7f));
    }

    // --- Modifier keys during a drag --------------------------------------
    {
        PianoRollComponent roll;
        roll.setSnapToSemitoneDrag(true);
        // Absolute is the default Pitch Snap Mode, as in Cubase.
        CHECK(roll.getAbsoluteDragSnap());
        CHECK(roll.resolveDragSnapKind({}) == Kind::Absolute);
        roll.setAbsoluteDragSnap(false);
        CHECK(roll.resolveDragSnapKind({}) == Kind::Relative);
        roll.setAbsoluteDragSnap(true);
        CHECK(roll.resolveDragSnapKind({}) == Kind::Absolute);
        CHECK(roll.resolveDragSnapKind(juce::ModifierKeys::shiftModifier) == Kind::Off);
        CHECK(roll.resolveDragSnapKind(juce::ModifierKeys::commandModifier) == Kind::Absolute);
        CHECK(roll.resolveDragSnapKind(juce::ModifierKeys::altModifier) == Kind::Relative);
        roll.setSnapToSemitoneDrag(false);
        CHECK(roll.resolveDragSnapKind({}) == Kind::Off);
    }

    // --- Keyboard: semitones, cents, segment navigation --------------------
    {
        Project project;
        prepareAudio(project);
        project.addNote(makeNote(0, 100, 60.0f));
        project.addNote(makeNote(100, 200, 62.0f));
        project.addNote(makeNote(250, 300, 64.0f));
        project.getNotes()[0].setSelected(true);

        PianoRollComponent roll;
        PitchUndoManager history;
        roll.setSize(800, 400);
        roll.setProject(&project);
        roll.setUndoManager(&history);

        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(near(project.getNotes()[0].getMidiNote(), 61.0f));
        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::downKey,
                                             juce::ModifierKeys::shiftModifier, 0)));
        CHECK(near(project.getNotes()[0].getMidiNote(), 60.99f));
        history.undo();
        history.undo();
        CHECK(near(project.getNotes()[0].getMidiNote(), 60.0f));

        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        CHECK(!project.getNotes()[0].isSelected() && project.getNotes()[1].isSelected());
        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        CHECK(project.getNotes()[2].isSelected());
        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey))); // stays at the end
        CHECK(project.getNotes()[2].isSelected());
        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
        CHECK(project.getNotes()[1].isSelected() && !project.getNotes()[2].isSelected());

        // Contiguous neighbours are what the glue controls join.
        CHECK(roll.findContiguousNeighbour(project.getNotes()[0], true) == &project.getNotes()[1]);
        CHECK(roll.findContiguousNeighbour(project.getNotes()[1], true) == nullptr); // gap
        CHECK(roll.findContiguousNeighbour(project.getNotes()[1], false) == &project.getNotes()[0]);

        // ...but never across two analysed chunks (regression).
        project.getAudioData().segmentChunkRanges = {{0, 100}, {100, 300}};
        CHECK(roll.findContiguousNeighbour(project.getNotes()[0], true) == nullptr);
        project.getAudioData().segmentChunkRanges.clear();
    }

    // --- Absolute snap: arrows land on the next semitone --------------------
    {
        Project project;
        prepareAudio(project);
        project.addNote(makeNote(0, 100, 60.3f));
        project.getNotes()[0].setSelected(true);
        PianoRollComponent roll;
        PitchUndoManager history;
        roll.setSize(800, 400);
        roll.setProject(&project);
        roll.setUndoManager(&history);
        roll.setSnapToSemitoneDrag(true);
        roll.setAbsoluteDragSnap(true);
        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(near(project.getNotes()[0].getMidiNote(), 61.0f));
        roll.setAbsoluteDragSnap(false); // Relative keeps the deviation
        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(near(project.getNotes()[0].getMidiNote(), 62.0f));
        project.getNotes()[0].setMidiNote(62.3f);
        CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(near(project.getNotes()[0].getMidiNote(), 63.3f));
    }

    // --- Dragging empty canvas pans the view with the pointer --------------
    {
        Project project;
        prepareAudio(project);
        auto& audio = project.getAudioData();
        audio.f0.assign(2000, 261.6f);
        audio.basePitch.assign(2000, 60.0f);
        audio.deltaPitch.assign(2000, 0.0f);
        audio.waveform.setSize(1, 2000 * HOP_SIZE);
        audio.waveform.clear();
        // Loud audio only between frames 100 and 200; silence elsewhere.
        for (int i = 100 * HOP_SIZE; i < 200 * HOP_SIZE; ++i)
            audio.waveform.setSample(0, i, 0.6f * std::sin(0.05f * static_cast<float>(i)));
        project.addNote(makeNote(1500, 1600, 60.0f));
        project.getNotes()[0].setSelected(true);

        PianoRollComponent roll;
        PitchUndoManager history;
        roll.setSize(900, 420);
        roll.setProject(&project);
        roll.setUndoManager(&history);
        roll.resized();
        roll.setScrollX(0.0);
        roll.setScrollY(1000.0);

        const float canvasCentreY = static_cast<float>(PianoRollComponent::headerHeight) +
                                    roll.getVisibleContentHeight() * 0.5f;
        const auto viewX = [&roll](int frame)
        {
            return static_cast<float>(PianoRollComponent::pianoKeysWidth) +
                   static_cast<float>(framesToSeconds(frame)) * roll.getPixelsPerSecond() -
                   static_cast<float>(roll.getScrollX());
        };
        CHECK(roll.isEmptyCanvasPoint({viewX(400), canvasCentreY}));  // silence
        CHECK(!roll.isEmptyCanvasPoint({viewX(150), canvasCentreY})); // waveform

        auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto event = [&](juce::Point<float> at, juce::Point<float> downAt, bool dragged)
        {
            return juce::MouseEvent(source, at, juce::ModifierKeys::leftButtonModifier,
                                    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &roll, &roll,
                                    juce::Time::getCurrentTime(), downAt,
                                    juce::Time::getCurrentTime(), 1, dragged);
        };

        // Empty canvas: the content follows the pointer, and the click clears
        // the selection.
        const juce::Point<float> start(viewX(400), canvasCentreY);
        const double scrollXBefore = roll.getScrollX();
        const double scrollYBefore = roll.getScrollY();
        roll.mouseDown(event(start, start, false));
        CHECK(!project.getNotes()[0].isSelected());
        roll.mouseDrag(event(start + juce::Point<float>(-60.0f, -40.0f), start, true));
        roll.mouseUp(event(start + juce::Point<float>(-60.0f, -40.0f), start, true));
        CHECK(near(static_cast<float>(roll.getScrollX() - scrollXBefore), 60.0f, 0.5f));
        CHECK(near(static_cast<float>(roll.getScrollY() - scrollYBefore), 40.0f, 0.5f));

        // From the waveform the drag selects instead; the view stays put.
        const juce::Point<float> onWave(viewX(150), canvasCentreY);
        const double scrollXMid = roll.getScrollX();
        roll.mouseDown(event(onWave, onWave, false));
        roll.mouseDrag(event(onWave + juce::Point<float>(-60.0f, 0.0f), onWave, true));
        roll.mouseUp(event(onWave + juce::Point<float>(-60.0f, 0.0f), onWave, true));
        CHECK(near(static_cast<float>(roll.getScrollX()), static_cast<float>(scrollXMid)));
    }

    std::cout << "VariAudio control tests passed\n";
    return 0;
}
