// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/VariAudioInspector.h"
#include "../Source/Undo/PitchUndoManager.h"
#include <cmath>
#include <iostream>

// Renders the piano roll and the inspector offscreen to PNG files, so the
// look can be checked without an audio device or a host.
// Usage: RenderSnapshots <output-dir>
namespace
{
Note makeNote(int start, int end, float midi)
{
    Note note(start, end, midi);
    note.setOriginalMidiNote(midi);
    std::vector<float> vibrato(static_cast<size_t>(end - start));
    for (size_t i = 0; i < vibrato.size(); ++i)
        vibrato[i] = 0.25f * std::sin(0.35f * static_cast<float>(i));
    note.setDeltaPitch(vibrato);
    note.setOriginalDeltaPitch(vibrato);
    return note;
}

bool save(juce::Component& component, const juce::File& file)
{
    auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    return out.openedOk() && png.writeImageToStream(image, out);
}
} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir(argc > 1 ? juce::String(argv[1])
                                     : juce::File::getCurrentWorkingDirectory().getFullPathName());

    Project project;
    auto& audio = project.getAudioData();
    audio.sampleRate = SAMPLE_RATE;
    const int frames = 600;
    audio.f0.assign(frames, 261.6f);
    audio.basePitch.assign(frames, 60.0f);
    audio.deltaPitch.assign(frames, 0.0f);
    audio.waveform.setSize(1, frames * HOP_SIZE);
    for (int i = 0; i < audio.waveform.getNumSamples(); ++i)
        audio.waveform.setSample(0, i, 0.4f * std::sin(0.03f * static_cast<float>(i)));

    const float melody[] = {60, 62, 64, 65, 67};
    for (int i = 0; i < 5; ++i)
        project.addNote(makeNote(20 + i * 100, 100 + i * 100, melody[i]));
    project.getNotes()[2].setSelected(true);
    // F4 moved up a whole tone: its original position shows as a grey ghost.
    project.getNotes()[3].setMidiNote(67.0f);
    project.setProjectKey(0, ScaleMode::Major);

    PianoRollComponent roll;
    PitchUndoManager history;
    roll.setSize(900, 420);
    roll.setProject(&project);
    roll.setUndoManager(&history);
    roll.resized();
    roll.fitPitchRangeToView(58.0f, 69.0f);

    // Hover the selected E4 segment so its smart controls (all of them) and
    // the pitch label show, as they would under the mouse.
    roll.setShowAllSmartControls(true);
    {
        const float worldX = static_cast<float>(framesToSeconds(260)) * roll.getPixelsPerSecond();
        const float worldY = (MAX_MIDI_NOTE - 64.0f) * roll.getPixelsPerSemitone() +
                             roll.getPixelsPerSemitone() * 0.5f;
        const juce::Point<float> position(
            worldX + PianoRollComponent::pianoKeysWidth - static_cast<float>(roll.getScrollX()),
            worldY + PianoRollComponent::headerHeight - static_cast<float>(roll.getScrollY()));
        auto source = juce::Desktop::getInstance().getMainMouseSource();
        const juce::MouseEvent move(source, position, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    &roll, &roll, juce::Time::getCurrentTime(), position,
                                    juce::Time::getCurrentTime(), 0, false);
        roll.mouseMove(move);
    }

    VariAudioInspector inspector;
    inspector.getCurrentProject = [&project] { return &project; };
    inspector.getParamValue = [&roll](VariAudioInspector::Param param)
    { return roll.getInspectorValue(static_cast<PianoRollComponent::InspectorParam>(param)); };
    inspector.getTargetCount = [&roll]
    {
        const auto targets = roll.getInspectorTargets();
        return std::pair<int, bool>{static_cast<int>(targets.size()),
                                    !targets.empty() && targets.front()->isSelected()};
    };
    inspector.getHarmonyPanel().refreshProject();
    inspector.refresh();
    inspector.setSize(VariAudioInspector::preferredWidth, 760);

    bool ok = save(roll, outDir.getChildFile("pianoroll.png")) &&
              save(inspector, outDir.getChildFile("inspector.png"));

    // Default editor height: the column scrolls.
    inspector.setSize(VariAudioInspector::preferredWidth, 560);
    ok = ok && save(inspector, outDir.getChildFile("inspector_short.png"));

    // Track view: the menus edit the project key, no "Use Project Key" switch.
    inspector.isProjectKeyScope = [] { return true; };
    inspector.refresh();
    inspector.setSize(VariAudioInspector::preferredWidth, 760);
    ok = ok && save(inspector, outDir.getChildFile("inspector_track.png"));

    // Japanese with the longest scale name, at the scrolling width.
    Localization::getInstance().setLanguage("ja");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100); // watchers are async
    project.setProjectKey(9, ScaleMode::MinorPentatonic);
    inspector.isProjectKeyScope = [] { return false; };
    inspector.refresh();
    inspector.setSize(VariAudioInspector::preferredWidth, 560);
    ok = ok && save(inspector, outDir.getChildFile("inspector_ja.png"));
    std::cout << (ok ? "Snapshots written to " : "Snapshot failed: ")
              << outDir.getFullPathName() << "\n";
    return ok ? 0 : 1;
}
