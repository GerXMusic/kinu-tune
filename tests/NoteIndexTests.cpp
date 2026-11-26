// N1 (notas del documento, D-340): the index enumerates every note of the
// track's items from one place and points at the real Note objects. This test
// reproduces the measured case (5 items, 2 sources, 276 notes, 5 of them in no
// window) and pins the invariants the rest of the series depends on: no copies,
// nothing invented for an item without a project, an item switch loses nothing,
// and the shapes track mode draws come from the same enumeration.
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/Project.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
constexpr int kSampleRate = 44100;
constexpr int kHopSize = 512;

bool near(double a, double b, double tolerance = 1.0e-5)
{
    return std::abs(a - b) <= tolerance;
}

int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * kSampleRate / static_cast<double>(kHopSize)));
}

Note makeNote(double startSeconds, double endSeconds, float midi)
{
    return Note(frameFor(startSeconds), frameFor(endSeconds), midi);
}

void setLength(Project& project, double seconds)
{
    project.getAudioData().f0.assign(
        static_cast<size_t>(frameFor(seconds)), 0.0f);
}

// `count` notes spread inside [start, end), each 90% of its slot long.
void addNotes(Project& project, double start, double end, int count,
              float midi)
{
    const double span = end - start - 0.2;
    const double step = span / static_cast<double>(count);
    for (int i = 0; i < count; ++i)
    {
        const double noteStart = start + 0.1 + step * i;
        project.getNotes().push_back(
            makeNote(noteStart, noteStart + step * 0.9, midi));
    }
}

DocumentNoteRegionSpec makeSpec(const juce::String& key,
                                const juce::String& modification,
                                const juce::String& source, double hostStart,
                                double hostEnd, double sourceStart,
                                double sourceEnd, Project* project)
{
    DocumentNoteRegionSpec spec;
    spec.key = key;
    spec.name = key;
    spec.modificationKey = modification;
    spec.sourceKey = source;
    spec.hostStartSeconds = hostStart;
    spec.hostEndSeconds = hostEnd;
    spec.sourceStartSeconds = sourceStart;
    spec.sourceEndSeconds = sourceEnd;
    spec.project = project;
    return spec;
}

// The real track: items 0, 2, 3 and 4 of Miu_stems_v2.wav (one modification,
// one Project, contiguous cuts) and item 1 of Miu_stems_v2-001.wav.
struct RealCase
{
    Project stems;
    Project stems001;
    std::vector<DocumentNoteRegionSpec> specs;

    RealCase()
    {
        setLength(stems, 116.5);
        addNotes(stems, 10.737, 35.157, 76, 60.0f);
        addNotes(stems, 42.749, 68.930, 67, 62.0f);
        addNotes(stems, 70.198, 115.787, 98, 64.0f);
        // The five notes measured between item 0 and item 2, in no window.
        for (int i = 0; i < 5; ++i)
            stems.getNotes().push_back(
                makeNote(7.9 + 0.5 * i, 8.2 + 0.5 * i, 55.0f + i));

        setLength(stems001, 3.8);
        addNotes(stems001, 0.0, 3.66, 30, 61.0f);

        specs.push_back(makeSpec("it0", "modA", "Miu_stems_v2.wav", 5.818,
                                 12.924, 0.0, 7.106, &stems));
        specs.push_back(makeSpec("it1", "modB", "Miu_stems_v2-001.wav", 12.914,
                                 16.574, 0.0, 3.660, &stems001));
        specs.push_back(makeSpec("it2", "modA", "Miu_stems_v2.wav", 16.555,
                                 40.975, 10.737, 35.157, &stems));
        specs.push_back(makeSpec("it3", "modA", "Miu_stems_v2.wav", 48.567,
                                 74.748, 42.749, 68.930, &stems));
        specs.push_back(makeSpec("it4", "modA", "Miu_stems_v2.wav", 76.016,
                                 121.605, 70.198, 115.787, &stems));
    }
};

int windowCount(const DocumentNoteIndex& index, const juce::String& key)
{
    for (const auto& region : index.regions)
        if (region.key == key)
            return region.notesInWindow;
    return -1;
}

const DocumentNoteSourceSummary* summaryFor(const DocumentNoteIndex& index,
                                            const juce::String& source)
{
    for (const auto& summary : index.sources)
        if (summary.sourceKey == source)
            return &summary;
    return nullptr;
}

// The counts of the measured case, per item and in total.
void testRealCaseCounts()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);

    CHECK(index != nullptr);
    CHECK(index->regionCount() == 5);
    CHECK(windowCount(*index, "it0") == 0); // first note of the project is 7.848
    CHECK(windowCount(*index, "it1") == 30);
    CHECK(windowCount(*index, "it2") == 76);
    CHECK(windowCount(*index, "it3") == 67);
    CHECK(windowCount(*index, "it4") == 98);
    CHECK(index->totalNotes == 0 + 30 + 76 + 67 + 98);
    CHECK(index->projectNotes == 246 + 30);
    CHECK(static_cast<int>(index->outsideNotes.size()) == 5);

    const auto* a = summaryFor(*index, "Miu_stems_v2.wav");
    const auto* b = summaryFor(*index, "Miu_stems_v2-001.wav");
    CHECK(a != nullptr && b != nullptr);
    CHECK(a->regions == 4);
    CHECK(a->projectNotes == 246);
    CHECK(a->visibleNotes == 241);
    CHECK(a->outsideNotes == 5);
    CHECK(b->regions == 1);
    CHECK(b->projectNotes == 30);
    CHECK(b->visibleNotes == 30);
    CHECK(b->outsideNotes == 0);
    CHECK(a->analyzed && b->analyzed);

    // Offsets: each region's source window maps onto the timeline by a constant.
    for (const auto& region : index->regions)
        CHECK(near(region.hostStartSeconds - region.sourceStartSeconds,
                   region.offsetSeconds));
}

// Every reference points inside its project's own note vector: one owner, no
// copy, and the frames are the note's own.
void testReferencesAreTheRealNotes()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);

    int checked = 0;
    for (const auto& ref : index->notes)
    {
        CHECK(ref.project != nullptr);
        CHECK(ref.note != nullptr);
        CHECK(ref.regionIndex >= 0 && ref.regionIndex < index->regionCount());
        if (ref.note == nullptr || ref.project == nullptr)
            continue;
        const auto& notes = ref.project->getNotes();
        CHECK(!notes.empty());
        CHECK(ref.note >= notes.data());
        CHECK(ref.note < notes.data() + notes.size());
        CHECK(ref.note->getStartFrame() == ref.firstFrame);
        CHECK(ref.note->getEndFrame() == ref.lastFrame);
        ++checked;
    }
    CHECK(checked == index->totalNotes);

    for (const auto& ref : index->outsideNotes)
    {
        CHECK(ref.regionIndex == -1);
        CHECK(ref.note != nullptr && ref.project != nullptr);
        if (ref.note != nullptr && ref.project != nullptr)
        {
            const auto& notes = ref.project->getNotes();
            CHECK(ref.note >= notes.data() && ref.note < notes.data() + notes.size());
        }
    }
}

// An item the host has not analysed is published as not analysed: no notes, and
// no placeholders invented to fill the count.
void testUnanalysedItemIsNotInvented()
{
    Project project;
    setLength(project, 4.0);
    addNotes(project, 0.0, 3.0, 3, 60.0f);

    std::vector<DocumentNoteRegionSpec> specs;
    specs.push_back(makeSpec("analysed", "mod", "take.wav", 0.0, 3.0, 0.0, 3.0,
                             &project));
    specs.push_back(makeSpec("pending", "mod2", "take2.wav", 5.0, 8.0, 0.0, 3.0,
                             nullptr));

    const auto index = buildDocumentNoteIndex("track", specs);
    CHECK(index->regionCount() == 2);
    CHECK(index->regions[1].key == "pending");
    CHECK(!index->regions[1].analyzed);
    CHECK(index->regions[1].project == nullptr);
    CHECK(index->regions[1].notesInWindow == 0);
    CHECK(index->regions[1].projectNotes == 0);
    CHECK(index->regions[1].savedNotes == -1);
    CHECK(index->regions[1].notes.empty());
    CHECK(index->totalNotes == 3); // only the analysed item's notes

    // And the same alone: an unanalysed track has zero notes, not a guess.
    const auto empty = buildDocumentNoteIndex(
        "track", {makeSpec("pending", "mod2", "take2.wav", 5.0, 8.0, 0.0, 3.0,
                           nullptr)});
    CHECK(empty->totalNotes == 0);
    CHECK(empty->projectNotes == 0);
    CHECK(empty->outsideNotes.empty());
}

// A note shown by two sibling windows of one modification counts once in the
// track total, but each item still answers for its own window.
void testSiblingWindowsDeduplicate()
{
    Project project;
    setLength(project, 20.0);
    project.getNotes().push_back(makeNote(6.0, 7.0, 60.0f));
    project.getNotes().push_back(makeNote(2.0, 3.0, 61.0f));

    std::vector<DocumentNoteRegionSpec> specs;
    specs.push_back(makeSpec("left", "mod", "take.wav", 0.0, 10.0, 0.0, 10.0,
                             &project));
    specs.push_back(makeSpec("right", "mod", "take.wav", 5.0, 15.0, 5.0, 15.0,
                             &project));

    const auto index = buildDocumentNoteIndex("track", specs);
    CHECK(index->regions[0].notesInWindow == 2); // 2-3 s and 6-7 s
    CHECK(index->regions[1].notesInWindow == 1); // 6-7 s only
    CHECK(index->totalNotes == 2);
    CHECK(index->projectNotes == 2);
    CHECK(index->outsideNotes.empty());
}

// Switching the item the canvas edits (same Project for the siblings) must not
// lose, add or renumber anything.
void testItemSwitchLosesNothing()
{
    RealCase real;
    const auto before = buildDocumentNoteIndex("track", real.specs);
    auto reordered = real.specs;
    std::rotate(reordered.begin(), reordered.begin() + 2, reordered.end());
    const auto after = buildDocumentNoteIndex("track", reordered);

    CHECK(before->totalNotes == after->totalNotes);
    CHECK(before->projectNotes == after->projectNotes);
    CHECK(before->outsideNotes.size() == after->outsideNotes.size());
    CHECK(before->regions.size() == after->regions.size());

    std::vector<const Note*> left;
    std::vector<const Note*> right;
    for (const auto& ref : before->notes)
        left.push_back(ref.note);
    for (const auto& ref : after->notes)
        right.push_back(ref.note);
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    CHECK(left == right); // same real notes, nothing lost

    // Each window still answers for the same notes after the switch.
    for (const auto& region : before->regions)
    {
        CHECK(windowCount(*before, region.key) == windowCount(*after, region.key));
        CHECK(windowCount(*after, region.key) == region.notesInWindow);
    }
}

// The shapes track mode draws are the index's own notes, clipped to the item.
void testDrawingMatchesIndex()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);

    for (const auto& region : index->regions)
    {
        const auto shapes = documentNoteShapes(region);
        int visibleRefs = 0;
        for (const auto& ref : region.notes)
            if (ref.note != nullptr && !ref.note->isRest())
                ++visibleRefs;
        CHECK(static_cast<int>(shapes.size()) <= visibleRefs);
        for (const auto& shape : shapes)
        {
            CHECK(shape.startSeconds >= region.hostStartSeconds - 1.0e-6);
            CHECK(shape.endSeconds <= region.hostEndSeconds + 1.0e-6);
            CHECK(shape.endSeconds > shape.startSeconds);
        }
    }
    CHECK(static_cast<int>(documentNoteShapes(index->regions[4]).size()) == 98);
    CHECK(documentNoteShapes(index->regions[0]).empty());

    // One note, one item: the drawn rectangle is the note's own span on the
    // host timeline, not a rebased copy.
    Project project;
    setLength(project, 20.0);
    project.getNotes().push_back(makeNote(10.0, 11.0, 65.0f));
    const auto spec = makeSpec("one", "mod", "take.wav", 20.0, 22.0, 10.0, 12.0,
                               &project);
    const auto single = buildDocumentNoteIndex("track", {spec});
    CHECK(single->regions[0].notesInWindow == 1);
    const auto shapes = documentNoteShapes(single->regions[0]);
    CHECK(shapes.size() == 1);
    // One analysis frame of tolerance: notes live on the 512-sample grid.
    const double frame = static_cast<double>(kHopSize) / kSampleRate;
    CHECK(near(shapes[0].startSeconds, 20.0, frame));
    CHECK(near(shapes[0].endSeconds, 21.0, frame));
    CHECK(near(shapes[0].midi, 65.0f));
}

// Boundary rule of the window (the one the drawn window Project uses).
void testWindowBoundaries()
{
    const int frames = frameFor(20.0);
    const auto window = documentNoteWindowFrames(2.0, 4.0, frames);
    CHECK(window.valid);
    CHECK(window.firstFrame == frameFor(2.0));
    CHECK(window.lastFrame == frameFor(4.0) + 1); // one frame past the end

    CHECK(documentNoteBelongsToWindow(makeNote(2.5, 3.0, 60.0f), window));
    // Touching the window at its first frame is not overlapping it.
    const Note endingAtStart(frameFor(1.0), window.firstFrame, 60.0f);
    CHECK(!documentNoteBelongsToWindow(endingAtStart, window));
    // Starting exactly at the last frame is already outside.
    const Note startingAtEnd(window.lastFrame, window.lastFrame + 5, 60.0f);
    CHECK(!documentNoteBelongsToWindow(startingAtEnd, window));

    // Degenerate windows never include anything.
    CHECK(!documentNoteWindowFrames(5.0, 5.0, frames).valid);
    CHECK(!documentNoteWindowFrames(0.0, 1.0, 0).valid);
}

} // namespace

int main()
{
    testRealCaseCounts();
    testReferencesAreTheRealNotes();
    testUnanalysedItemIsNotInvented();
    testSiblingWindowsDeduplicate();
    testItemSwitchLosesNothing();
    testDrawingMatchesIndex();
    testWindowBoundaries();

    std::printf("NoteIndexTests: ok\n");
    return 0;
}
