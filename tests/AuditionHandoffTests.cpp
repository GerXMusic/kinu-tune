// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

// The editor's audition hand-offs in the plug-in (MainComponent without an
// audio device): the dragged segment's feedback, the piano key tone, the
// region preview and the transport commands take the one host output from
// each other. The host callbacks are recorded instead of sounding.

#include "../Source/Audio/EditorController.h"
#include "../Source/UI/MainComponent.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "TestAssert.h"
#include <onnxruntime_cxx_api.h>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> calls;

int count(const std::string &call)
{
    int n = 0;
    for (const auto &c : calls)
        n += c == call ? 1 : 0;
    return n;
}

void pump(int ms)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + ms;
    while (juce::Time::getMillisecondCounterHiRes() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
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

void setUpProject(Project &project)
{
    constexpr int frames = 800;
    auto &audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;
    audio.waveform.setSize(1, frames * HOP_SIZE);
    for (int i = 0; i < audio.waveform.getNumSamples(); ++i)
        audio.waveform.setSample(
            0, i, 0.4f * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi *
                                                     261.6 * i / SAMPLE_RATE)));
    audio.originalWaveform.makeCopyOf(audio.waveform);
    audio.f0.assign(frames, 261.6f);
    audio.basePitch.assign(frames, 60.0f);
    audio.deltaPitch.assign(frames, 0.0f);
    audio.voicedMask.assign(frames, true);
    audio.melSpectrogram.assign(frames, std::vector<float>(1, 0.0f));
    project.addNote(makeNote(100, 140, 60.0f));
    project.addNote(makeNote(200, 290, 64.0f));
}
} // namespace

// Friend of MainComponent (see there).
struct MainComponentTestAccess
{
    MainComponent &mc;
    using Source = AuditionOutput::Source;

    Source owner() const { return mc.auditionOutput.owner(); }
    bool dragActive() const { return mc.noteDragAuditionActive; }
    bool dragSuppressed() const { return mc.noteDragAuditionSuppressed; }
    bool keyActive() const { return mc.pianoKeyAuditionActive; }
    bool regionActive() const { return mc.previewRegionActive; }

    // Drags `note` by `shift` semitones and waits until its feedback sounds.
    void drag(Note &note, float shift)
    {
        note.setPitchOffset(shift);
        mc.auditionDraggedNote(note);
        mc.noteDragAuditionLastInputMs -= 1000; // skip the input debounce
        mc.dispatchPendingDragAudition();
        pump(250);
    }
    void mouseUp() { mc.finishDraggedNoteAudition(); }
    void key(int midi) { mc.auditionPianoKey(midi); }
    void keyUp() { mc.finishPianoKeyAudition(); }
    void region(int start, int end) { mc.previewNoteRegion(start, end); }
    void play() { mc.play(); }
    void pause() { mc.pause(); }
    void stop() { mc.stop(); }
    void auditionOff() { mc.cancelDraggedNoteAudition(); }

    static void wire(MainComponent &mc, EditorController &controller,
                     PitchUndoManager &history)
    {
        mc.editorController = &controller;
        mc.undoManager = &history;
        mc.pianoRoll.setProject(controller.getProject());
        mc.pianoRoll.setUndoManager(&history);
        mc.dragAuditionEnabled = true;
    }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    // The detectors the editor owns need the ONNX Runtime API; the plug-in
    // loads it from its bundle, a test from the library it links.
    Ort::InitApi(OrtGetApiBase()->GetApi(ORT_API_VERSION));
    using Source = AuditionOutput::Source;

    auto run = [](const char *name, auto scenario)
    {
        // The editor goes first on teardown, while what it points at lives.
        EditorController controller(false);
        PitchUndoManager history(100);
        MainComponent mc(false); // plug-in: no audio device
        setUpProject(*controller.getProject());
        MainComponentTestAccess::wire(mc, controller, history);
        mc.setOnRequestBackendPreview([](const Project &, int start, int end)
                                      { calls.push_back("preview " + std::to_string(start) +
                                                        "-" + std::to_string(end)); });
        mc.setOnStopBackendPreview([] { calls.push_back("preview stop"); });
        mc.setOnRequestDragAudition([](const juce::AudioBuffer<float> &, double, bool whole)
                                    { calls.push_back(whole ? "loop whole" : "loop seamless"); });
        mc.setOnStopDragAudition([] { calls.push_back("loop stop"); });
        calls.clear();
        MainComponentTestAccess access{mc};
        auto &notes = controller.getProject()->getNotes();
        std::cout << name << "\n";
        scenario(access, notes);
    };

    run("mouse up ends the feedback", [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.drag(n[0], 0.3f);
            CHECK(count("preview 100-140") == 1 && count("loop whole") == 1);
            CHECK(a.owner() == Source::DraggedSegment);
            a.mouseUp();
            CHECK(count("loop stop") == 1);
            CHECK(a.owner() == Source::None && !a.dragActive());
        });

    run("play silences the drag until mouse up", [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.drag(n[0], 0.3f);
            a.play();
            CHECK(count("loop stop") == 1);
            CHECK(a.owner() == Source::None && a.dragSuppressed());
            a.drag(n[0], 1.0f); // the drag goes on: stays silent
            CHECK(count("loop whole") == 1);
            a.mouseUp();
            CHECK(!a.dragSuppressed() && count("loop stop") == 1);
        });

    run("pause and stop silence the drag", [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.drag(n[0], 0.3f);
            a.pause();
            CHECK(count("loop stop") == 1 && !a.dragActive());
            a.mouseUp();
            a.drag(n[1], 0.3f);
            a.stop();
            CHECK(count("loop stop") == 2 && !a.dragActive());
        });

    run("a piano key replaces the drag", [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.drag(n[0], 0.3f);
            a.key(69);
            CHECK(count("loop stop") == 0); // replaced, not stopped
            CHECK(count("preview 0-1") == 1 && count("loop seamless") == 1);
            CHECK(a.owner() == Source::PianoKey && !a.dragActive() && a.keyActive());
            a.keyUp();
            CHECK(count("loop stop") == 1 && a.owner() == Source::None && !a.keyActive());
        });

    run("a transport command silences the piano key", [](MainComponentTestAccess &a, std::vector<Note> &)
        {
            a.key(69);
            a.pause();
            CHECK(count("loop stop") == 1 && !a.keyActive() && a.owner() == Source::None);
        });

    run("a region preview stops the drag's loop", [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.drag(n[0], 0.3f);
            a.region(200, 290);
            CHECK(count("loop stop") == 1 && !a.dragActive());
            CHECK(count("preview 200-290") == 1 && a.regionActive());
            CHECK(a.owner() == Source::RegionPreview);
            a.stop();
            CHECK(count("preview stop") == 1 && !a.regionActive() && a.owner() == Source::None);
        });

    run("a drag replaces the region preview", [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.region(200, 290);
            a.drag(n[0], 0.3f);
            CHECK(count("preview stop") == 1 && !a.regionActive());
            CHECK(count("preview 100-140") == 1 && count("loop whole") == 1);
            CHECK(a.owner() == Source::DraggedSegment);
            a.mouseUp();
        });

    run("a region preview stops a held piano key", [](MainComponentTestAccess &a, std::vector<Note> &)
        {
            a.key(69);
            a.region(200, 290);
            CHECK(calls.size() >= 4 && calls[2] == "loop stop" && calls[3] == "preview 200-290");
            CHECK(!a.keyActive() && a.owner() == Source::RegionPreview);
            a.keyUp(); // nothing left to stop
            CHECK(count("loop stop") == 1);
        });

    run("a piano key ends a region preview", [](MainComponentTestAccess &a, std::vector<Note> &)
        {
            a.region(200, 290);
            a.key(69);
            CHECK(calls.size() >= 3 && calls[1] == "preview stop" && calls[2] == "preview 0-1");
            CHECK(!a.regionActive() && a.keyActive());
            a.keyUp();
        });

    run("play stops a held piano key", [](MainComponentTestAccess &a, std::vector<Note> &)
        {
            a.key(69);
            a.play();
            CHECK(count("loop stop") == 1 && !a.keyActive());
        });

    run("a drag replaces a piano key; its key up does not stop the drag",
        [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.key(69);
            a.drag(n[0], 0.3f);
            CHECK(!a.keyActive() && a.dragActive() && count("loop stop") == 0);
            a.keyUp();
            CHECK(count("loop stop") == 0 && a.dragActive());
            a.mouseUp();
            CHECK(count("loop stop") == 1);
        });

    run("switching the audition off mid-drag stops it", [](MainComponentTestAccess &a, std::vector<Note> &n)
        {
            a.drag(n[0], 0.3f);
            a.auditionOff();
            CHECK(count("loop stop") == 1 && !a.dragActive() && a.owner() == Source::None);
        });

    std::cout << "AuditionHandoffTests passed\n";
    return 0;
}
