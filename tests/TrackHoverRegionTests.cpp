// N8 (document-scope UI, D-348): the pointer no longer opens notebooks.
//
// Passing the mouse over a note of another item used to call
// prepareTrackPointer(), which asked the owner to activate that region. The
// project and the inspector changed without a click, so the user read "29
// segments" or "262" depending on where the pointer happened to be. This test
// pins the new rule: hover changes nothing (project, document target count,
// item count, active region); a click still activates the region, and the
// inspector keeps talking about the whole document.
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteSelection.h"
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"

#include <iostream>
#include <memory>
#include <vector>

namespace
{
constexpr double kRegionSeconds = 1.0;

int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * SAMPLE_RATE / static_cast<double>(HOP_SIZE)));
}

void setLength(Project& project, double seconds)
{
    const auto frames = static_cast<size_t>(frameFor(seconds));
    project.getAudioData().sampleRate = SAMPLE_RATE;
    project.getAudioData().f0.assign(frames, 220.0f);
    project.getAudioData().basePitch.assign(frames, 60.0f);
    project.getAudioData().deltaPitch.assign(frames, 0.0f);
}

void addNote(Project& project, int start, int end, float midi)
{
    project.addNote(Note(start, end, midi));
}

juce::MouseEvent event(PianoRollComponent& roll, juce::Point<float> point,
                       int modifiers = 0)
{
    return {juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys(modifiers), 1.0f, 0, 0, 0, 0, &roll, &roll,
            juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(),
            1, true};
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    Project item0, item1;
    setLength(item0, kRegionSeconds);
    setLength(item1, kRegionSeconds);
    addNote(item0, 3, 20, 88.0f);
    addNote(item0, 25, 40, 90.0f);
    addNote(item1, 3, 20, 86.0f);

    std::vector<DocumentNoteRegionSpec> specs;
    auto spec = [](const juce::String& key, const juce::String& source,
                   double hostStart, double hostEnd, Project* project)
    {
        DocumentNoteRegionSpec s;
        s.key = key;
        s.name = key;
        s.modificationKey = key;
        s.sourceKey = source;
        s.hostStartSeconds = hostStart;
        s.hostEndSeconds = hostEnd;
        s.sourceStartSeconds = 0.0;
        s.sourceEndSeconds = 0.5;
        s.project = project;
        return s;
    };
    specs.push_back(spec("it0", "a.wav", 0.0, 0.5, &item0));
    specs.push_back(spec("it1", "b.wav", 1.0, 1.5, &item1));
    const auto index = buildDocumentNoteIndex("track", specs);
    auto selection = std::make_shared<DocumentNoteSelection>();

    CHECK(index != nullptr);
    CHECK(index->totalNotes == 3);
    CHECK(index->regionCount() == 2);

    PianoRollComponent roll;
    roll.setSize(900, 500);
    roll.setProject(&item0);
    roll.setTrackViewMode(true);
    roll.setDocumentNoteContext(index, selection);
    roll.setScrollX(0);
    roll.setScrollY(0);

    auto previews = std::make_shared<std::vector<MainViewRegionPreview>>();
    MainViewRegionPreview first;
    first.key = "it0";
    first.active = true;
    first.startSeconds = 0.0;
    first.endSeconds = 0.5;
    first.project = &item0;
    first.sourceStartSeconds = 0.0;
    first.sourceEndSeconds = 0.5;
    first.projectHostOffsetSeconds = 0.0;
    first.hasContent = true;
    previews->push_back(first);
    MainViewRegionPreview second;
    second.key = "it1";
    second.active = false;
    second.startSeconds = 1.0;
    second.endSeconds = 1.5;
    // N10 (D-350): the preview borrows the live Project; no window copy.
    second.project = &item1;
    second.sourceStartSeconds = 0.0;
    second.sourceEndSeconds = 0.5;
    second.projectHostOffsetSeconds = 1.0;
    second.hasContent = true;
    previews->push_back(second);
    roll.setRegionPreviews(previews);

    juce::String routed;
    roll.onInactiveRegionClicked = [&](const juce::String& key)
    {
        routed = key;
        roll.setProject(key == "it0" ? &item0 : &item1);
        roll.setTimelineDisplayOffset(key == "it0" ? 0.0 : 1.0);
        (*previews)[0].active = key == "it0";
        (*previews)[1].active = key == "it1";
        roll.setRegionPreviews(previews);
    };

    // The document objective: two items of this take, three visible notes.
    CHECK(roll.getInspectorTargetCount() == 3);
    CHECK(roll.getInspectorTargetItemCount() == 2);
    CHECK(!roll.inspectorTargetsAreSelection());
    const auto targets = roll.getInspectorTargets();
    CHECK(targets.size() == 3);
    CHECK(targets[0] == &item0.getNotes()[0]);
    CHECK(targets[1] == &item0.getNotes()[1]);
    CHECK(targets[2] == &item1.getNotes()[0]);

    const float pps = roll.getPixelsPerSecond();
    const float h = roll.getPixelsPerSemitone();
    const float header = PianoRollComponent::headerHeight;
    const float x = PianoRollComponent::pianoKeysWidth +
                    static_cast<float>((1.0 + framesToSeconds(11.5f)) * pps) -
                    static_cast<float>(roll.getScrollX());
    const float y = header + 3.5f * h - static_cast<float>(roll.getScrollY());

    // Hover over the note of the second item: nothing changes.
    roll.mouseMove(event(roll, {x, y}));
    CHECK(routed.isEmpty());
    CHECK(roll.getProject() == &item0);
    CHECK(roll.getInspectorTargetCount() == 3);
    CHECK(roll.getInspectorTargetItemCount() == 2);
    CHECK(!(*previews)[1].active);

    // A click on that note does activate its region.
    const auto left = juce::ModifierKeys::leftButtonModifier;
    roll.mouseDown(event(roll, {x, y}, left));
    roll.mouseUp(event(roll, {x, y}, left));
    CHECK(routed == "it1");
    CHECK(roll.getProject() == &item1);
    CHECK((*previews)[1].active);
    // And the inspector speaks about the document: the click selected its note
    // (N9, D-349: a click changes the selection through its owner, so nothing
    // writes a flag the set does not know), and the objective is that selection
    // of the document, not the notes of the newly active item.
    CHECK(roll.getInspectorTargetCount() == 1);
    CHECK(roll.getInspectorTargetItemCount() == 1);
    CHECK(roll.inspectorTargetsAreSelection());
    CHECK(roll.getInspectorTargets()[0] == &item1.getNotes()[0]);

    std::cout << "Track hover region tests passed\n";
    return 0;
}
