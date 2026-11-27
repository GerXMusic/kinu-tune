// N10 (document-scope UI, D-350): one copy of the notes, one drawing path.
//
// Track mode used to keep, per inactive item, a cut-down copy of the item's
// Project (its notes included), and the hit-test resolved against that copy:
// a second universe of notes. This test pins the replacement - the preview
// borrows the live Project, the index, the drawing and the hit-test all read
// the same Note objects - and measures it:
//
//   * the preview's Project IS the index's Project and every indexed Note lives
//     inside it;
//   * a pitch change made in an inactive Project shows in the next paint
//     without republishing the previews (the layer is not a stale copy);
//   * the hit-test follows the live note (Alt-click, the notes-only probe) and
//     the note shapes the thumbnail/drawing enumerate;
//   * a pinned clip is drawn and is never clickable.
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteSelection.h"
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/RegionPreview.h"
#include "../Source/Utils/NotePitch.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
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

void addNote(Project& project, double startSeconds, double endSeconds, float midi)
{
    project.addNote(Note(frameFor(startSeconds), frameFor(endSeconds), midi));
}

juce::MouseEvent event(PianoRollComponent& roll, juce::Point<float> point,
                       int modifiers)
{
    return {juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys(modifiers), 1.0f, 0, 0, 0, 0, &roll, &roll,
            juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(),
            1, true};
}

// The canvas point of a live note of a preview: the same arithmetic the
// drawing uses (note frames + the item's host offset, through the canvas).
juce::Point<float> notePoint(PianoRollComponent& roll, const Note& note,
                             const MainViewRegionPreview& region)
{
    const double middleSeconds =
        0.5 * (framesToSeconds(note.getVisualStartFrame()) +
               framesToSeconds(note.getVisualEndFrame()));
    const double hostSeconds = middleSeconds + region.projectHostOffsetSeconds;
    const float x = PianoRollComponent::pianoKeysWidth +
                    static_cast<float>(hostSeconds * roll.getPixelsPerSecond()) -
                    static_cast<float>(roll.getScrollX());
    const float y = PianoRollComponent::headerHeight +
                    (MAX_MIDI_NOTE - note.getAdjustedMidiNote() + 0.5f) *
                        roll.getPixelsPerSemitone() -
                    static_cast<float>(roll.getScrollY());
    return {x, y};
}

juce::Image paintToImage(PianoRollComponent& roll)
{
    juce::Image image(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
    juce::Graphics g(image);
    roll.paint(g);
    return image;
}

// Pixels that differ between two paints. The canvas area only: the piano keys
// and ruler carry no note.
int differingPixels(const juce::Image& a, const juce::Image& b)
{
    int different = 0;
    const int left = PianoRollComponent::pianoKeysWidth;
    const int top = PianoRollComponent::headerHeight;
    for (int y = top; y < a.getHeight(); y += 2)
        for (int x = left; x < a.getWidth(); x += 2)
            if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                ++different;
    return different;
}

struct Scene
{
    Project itemA; // the item the canvas edits
    Project itemB; // a second, inactive item
    Project itemC; // a clip of another track, pinned
    std::shared_ptr<std::vector<MainViewRegionPreview>> previews;

    Scene()
    {
        setLength(itemA, 0.5);
        setLength(itemB, 0.5);
        setLength(itemC, 0.5);
        addNote(itemA, 0.02, 0.20, 74.0f);
        addNote(itemB, 0.03, 0.22, 72.0f);
        addNote(itemC, 0.05, 0.24, 68.0f);
    }
};

MainViewRegionPreview makePreview(const juce::String& key, Project* project,
                                  double hostStart, double hostEnd, bool active,
                                  bool pinned = false)
{
    MainViewRegionPreview preview;
    preview.key = key;
    preview.name = key;
    preview.project = project;
    preview.startSeconds = hostStart;
    preview.endSeconds = hostEnd;
    preview.sourceStartSeconds = 0.0;
    preview.sourceEndSeconds = hostEnd - hostStart;
    preview.projectHostOffsetSeconds = hostStart;
    preview.active = active;
    preview.pinned = pinned;
    preview.hasContent = project != nullptr;
    preview.outlineColour = juce::Colours::red;
    return preview;
}

DocumentNoteIndexPtr buildIndex(Scene& scene)
{
    std::vector<DocumentNoteRegionSpec> specs;
    const auto add = [&specs](const juce::String& key, Project* project,
                              double hostStart, double hostEnd) {
        DocumentNoteRegionSpec spec;
        spec.key = key;
        spec.name = key;
        spec.modificationKey = key;
        spec.sourceKey = key + ".wav";
        spec.hostStartSeconds = hostStart;
        spec.hostEndSeconds = hostEnd;
        spec.sourceStartSeconds = 0.0;
        spec.sourceEndSeconds = hostEnd - hostStart;
        spec.project = project;
        specs.push_back(spec);
    };
    add("itA", &scene.itemA, 0.0, 0.5);
    add("itB", &scene.itemB, 1.0, 1.5);
    return buildDocumentNoteIndex("track", specs);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    Scene scene;
    const auto index = buildIndex(scene);
    auto selection = std::make_shared<DocumentNoteSelection>();

    CHECK(index != nullptr);
    CHECK(index->regionCount() == 2);
    CHECK(index->totalNotes == 2);

    PianoRollComponent roll;
    roll.setSize(900, 520);
    roll.setProject(&scene.itemA);
    roll.setTrackViewMode(true);
    roll.setDocumentNoteContext(index, selection);
    roll.setScrollX(0);
    roll.setScrollY(0);
    roll.setTimelineDisplayOffset(0.0);
    // A zoom where every note of the scene is on screen; the point arithmetic
    // below reads the same midiToY the drawing and the hit-test use.
    roll.fitPitchRangeToView(58.0f, 78.0f);

    scene.previews =
        std::make_shared<std::vector<MainViewRegionPreview>>();
    scene.previews->push_back(makePreview("itA", &scene.itemA, 0.0, 0.5, true));
    scene.previews->push_back(makePreview("itB", &scene.itemB, 1.0, 1.5, false));
    scene.previews->push_back(
        makePreview("pinC", &scene.itemC, 2.0, 2.5, false, true));
    roll.setRegionPreviews(scene.previews);

    juce::String routed;
    roll.onInactiveRegionClicked = [&](const juce::String& key) {
        routed = key;
    };

    // 1. One copy: the preview borrows the index's own Project, and every
    //    indexed Note lives inside that Project's note vector.
    for (const auto& region : index->regions)
    {
        const auto* preview = [&]() -> const MainViewRegionPreview* {
            for (const auto& item : *scene.previews)
                if (item.key == region.key)
                    return &item;
            return nullptr;
        }();
        CHECK(preview != nullptr);
        if (preview == nullptr || region.project == nullptr)
            continue;
        CHECK(preview->project == region.project);
        for (const auto& ref : region.notes)
        {
            CHECK(ref.project == preview->project);
            CHECK(ref.note != nullptr);
            if (ref.note == nullptr)
                continue;
            const auto& notes = preview->project->getNotes();
            CHECK(ref.note >= notes.data());
            CHECK(ref.note < notes.data() + notes.size());
        }
    }

    // 2. The drawing: a pitch change in an inactive Project shows up in the
    //    next paint without republishing the previews.
    const auto beforeEdit = paintToImage(roll);
    Note* inactiveNote = nullptr;
    for (const auto& region : index->regions)
        if (region.key == "itB")
            inactiveNote = region.notes.front().note;
    CHECK(inactiveNote != nullptr);
    if (inactiveNote != nullptr)
    {
        const auto previousMidi = inactiveNote->getMidiNote();
        NotePitch::applyCorrection(*inactiveNote, previousMidi - 3.0f);
        const auto afterEdit = paintToImage(roll);
        CHECK(differingPixels(beforeEdit, afterEdit) > 0);
        CHECK(scene.previews->size() == 3); // nothing was republished
        NotePitch::applyCorrection(*inactiveNote, previousMidi);
    }

    // 3. The hit-test walks the live notes: Alt-click on the note routes to its
    //    item, and the same point misses once the note moved away.
    {
        const MainViewRegionPreview* previewB = nullptr;
        for (const auto& item : *scene.previews)
            if (item.key == "itB")
                previewB = &item;
        CHECK(previewB != nullptr);
        CHECK(inactiveNote != nullptr);
        if (previewB != nullptr && inactiveNote != nullptr)
        {
            const auto notePosition = notePoint(roll, *inactiveNote, *previewB);
            const int altClick = juce::ModifierKeys::leftButtonModifier |
                                 juce::ModifierKeys::altModifier;
            roll.mouseDown(event(roll, notePosition, altClick));
            roll.mouseUp(event(roll, notePosition, altClick));
            CHECK(routed == "itB");

            // Move the note two semitones down: the old point must not hit it
            // any more, the new one must. (The note-only probe is what the Alt
            // click uses, so empty space does not route on its own.)
            routed.clear();
            const auto midi = inactiveNote->getMidiNote();
            NotePitch::applyCorrection(*inactiveNote, midi - 2.0f);
            roll.mouseDown(event(roll, notePosition, altClick));
            roll.mouseUp(event(roll, notePosition, altClick));
            CHECK(routed.isEmpty());
            const auto movedPosition = notePoint(roll, *inactiveNote, *previewB);
            roll.mouseDown(event(roll, movedPosition, altClick));
            roll.mouseUp(event(roll, movedPosition, altClick));
            CHECK(routed == "itB");
            NotePitch::applyCorrection(*inactiveNote, midi);
            routed.clear();
        }
    }

    // 4. Hit-test and the drawing enumerate the same shapes: every note the
    //    index reports can be hit where the drawing places it. (The active
    //    item is the bound region, so there the click must route nowhere.)
    for (const auto& region : index->regions)
    {
        const MainViewRegionPreview* preview = nullptr;
        for (const auto& item : *scene.previews)
            if (item.key == region.key)
                preview = &item;
        if (preview == nullptr)
            continue;
        const auto shapes = documentNoteShapes(region);
        CHECK(static_cast<int>(shapes.size()) == region.notesInWindow);
        for (const auto& shape : shapes)
        {
            routed.clear();
            const double middleSeconds = 0.5 * (shape.startSeconds + shape.endSeconds);
            // The canvas display offset is 0 here, so host seconds are canvas
            // seconds; the shape already carries the item's host offset.
            const float x = PianoRollComponent::pianoKeysWidth +
                            static_cast<float>(middleSeconds *
                                               roll.getPixelsPerSecond()) -
                            static_cast<float>(roll.getScrollX());
            const float y = PianoRollComponent::headerHeight +
                            (MAX_MIDI_NOTE - shape.midi + 0.5f) *
                                roll.getPixelsPerSemitone() -
                            static_cast<float>(roll.getScrollY());
            const int altClick = juce::ModifierKeys::leftButtonModifier |
                                 juce::ModifierKeys::altModifier;
            roll.mouseDown(event(roll, {x, y}, altClick));
            roll.mouseUp(event(roll, {x, y}, altClick));
            if (preview->active)
                CHECK(routed.isEmpty());
            else
                CHECK(routed == region.key);
        }
    }

    // 5. Pinned: drawn, never clickable.
    {
        const MainViewRegionPreview* pinned = nullptr;
        for (const auto& item : *scene.previews)
            if (item.pinned)
                pinned = &item;
        CHECK(pinned != nullptr);
        const Note& pinnedNote = scene.itemC.getNotes().front();
        const auto pinnedPosition = notePoint(roll, pinnedNote, *pinned);
        routed.clear();
        roll.mouseDown(event(roll, pinnedPosition,
                             juce::ModifierKeys::leftButtonModifier));
        roll.mouseUp(event(roll, pinnedPosition,
                           juce::ModifierKeys::leftButtonModifier));
        CHECK(routed.isEmpty());

        const auto withPinned = paintToImage(roll);
        auto withoutPinned =
            std::make_shared<std::vector<MainViewRegionPreview>>();
        for (const auto& item : *scene.previews)
            if (!item.pinned)
                withoutPinned->push_back(item);
        roll.setRegionPreviews(withoutPinned);
        const auto noPinned = paintToImage(roll);
        CHECK(differingPixels(withPinned, noPinned) > 0);
        roll.setRegionPreviews(scene.previews);
    }

    std::cout << "Region preview single-source tests passed\n";
    return 0;
}
