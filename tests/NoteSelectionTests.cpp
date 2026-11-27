// N2 (notas del documento, D-340): the selection of the whole track. One set
// of (region, note) pairs deduplicated by (modification, Note*), written by the
// marquee, shift-click and "select all" of track mode and published by
// notes_debug. This test reproduces the measured case (5 items, 2 sources) and
// pins the invariants of the phase: a marquee over three items, a selection
// with both takes, an additive toggle, a track-wide select-all that leaves the
// invisible notes out and does not select a note twice, a selection that
// survives an item switch, and the pruning when a project goes away.
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteSelection.h"
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

// Every pair the window of one item shows: what a marquee over that item can
// reach (collectPitchMacroNotes returns exactly the notes of the window).
std::vector<std::pair<Project*, Note*>>
pairsOfRegion(const DocumentNoteIndex& index, const juce::String& key)
{
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& region : index.regions)
        if (region.key == key)
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
    return pairs;
}

int selectedCount(Project& project)
{
    int count = 0;
    for (const auto& note : project.getNotes())
        if (note.isSelected() && !note.isRest())
            ++count;
    return count;
}

int countForRegion(const DocumentNoteSelection& selection,
                   const juce::String& key)
{
    return selection.countForRegion(key);
}

// One rectangle over items 2, 3 and 4 selects their notes once each.
void testMarqueeAcrossThreeItems()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;

    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto* key : {"it2", "it3", "it4"})
        for (const auto& pair : pairsOfRegion(*index, key))
            pairs.push_back(pair);

    CHECK(selection.setFromPairs(pairs, *index));
    CHECK(selection.size() == 76 + 67 + 98);
    CHECK(countForRegion(selection, "it2") == 76);
    CHECK(countForRegion(selection, "it3") == 67);
    CHECK(countForRegion(selection, "it4") == 98);
    CHECK(countForRegion(selection, "it0") == 0);
    CHECK(countForRegion(selection, "it1") == 0);
    // The per-region counts are a partition of the set: no note counted twice.
    int sum = 0;
    for (const auto& region : index->regions)
        sum += countForRegion(selection, region.key);
    CHECK(sum == selection.size());

    selection.applyToNotes();
    // All 241 visible notes of the take are selected; the five notes between
    // the windows are not (they are not in any window's selection).
    CHECK(selectedCount(real.stems) == 241);
    CHECK(selectedCount(real.stems001) == 0);
    for (const auto& ref : index->outsideNotes)
        CHECK(ref.note != nullptr && !ref.note->isSelected());
}

// A marquee that also covers the other take selects across sources.
void testSelectionSpansTwoSources()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;

    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*index, "it1");
    for (const auto& pair : pairsOfRegion(*index, "it4"))
        pairs.push_back(pair);

    CHECK(selection.setFromPairs(pairs, *index));
    CHECK(selection.size() == 30 + 98);
    CHECK(selection.countForSource("Miu_stems_v2-001.wav") == 30);
    CHECK(selection.countForSource("Miu_stems_v2.wav") == 98);
    CHECK(selection.countForModification("modA") == 98);
    CHECK(selection.countForModification("modB") == 30);

    selection.applyToNotes();
    CHECK(selectedCount(real.stems) == 98);
    CHECK(selectedCount(real.stems001) == 30);
}

// Shift-click is additive and toggles; a note no window shows is not
// selectable and adds nothing.
void testAdditiveToggle()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;

    Note* fromB = &real.stems001.getNotes().front();
    // The first note of the stems project is at 10.8 s, inside item 2; the five
    // notes in no window were appended after them (7.9 s).
    Note* fromA = nullptr;
    for (auto& note : real.stems.getNotes())
        if (note.getStartFrame() < frameFor(10.0))
        {
            fromA = &note;
            break;
        }
    CHECK(fromA != nullptr);
    Note* visibleA = index->regions[4].notes.front().note;
    CHECK(visibleA != nullptr);

    CHECK(selection.toggle(&real.stems001, fromB, *index));
    CHECK(selection.size() == 1);
    CHECK(selection.contains(&real.stems001, fromB));
    CHECK(selection.countForRegion("it1") == 1);

    // A note of the other take adds to the same set.
    CHECK(selection.toggle(&real.stems, visibleA, *index));
    CHECK(selection.size() == 2);
    CHECK(selection.countForSource("Miu_stems_v2.wav") == 1);
    CHECK(selection.countForSource("Miu_stems_v2-001.wav") == 1);
    selection.applyToNotes();
    CHECK(fromB->isSelected());
    CHECK(visibleA->isSelected());

    // Toggling it again removes only that note.
    CHECK(selection.toggle(&real.stems, visibleA, *index));
    CHECK(selection.size() == 1);
    CHECK(!selection.contains(&real.stems, visibleA));
    selection.applyToNotes();
    CHECK(!visibleA->isSelected());
    CHECK(fromB->isSelected());

    // The five notes in no window cannot enter the set.
    CHECK(!selection.toggle(&real.stems, fromA, *index));
    CHECK(selection.size() == 1);
}

// Track "select all": every visible note of every item, once, and nothing
// invisible — even when the flags started from an item-mode select-all.
void testSelectAllInTrackMode()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;

    // The old behaviour: "select all" on the active item flagged the whole
    // project, the five notes in no window included.
    real.stems.selectAllNotes();
    CHECK(selectedCount(real.stems) == 246);

    CHECK(selection.selectAll(*index));
    CHECK(selection.size() == 0 + 30 + 76 + 67 + 98); // 271, not 276
    CHECK(selection.size() == index->totalNotes);
    int sum = 0;
    for (const auto& region : index->regions)
        sum += selection.countForRegion(region.key);
    CHECK(sum == selection.size());
    CHECK(selection.countForSource("Miu_stems_v2.wav") == 241);
    CHECK(selection.countForSource("Miu_stems_v2-001.wav") == 30);

    selection.applyToNotes();
    CHECK(selectedCount(real.stems) == 241);
    CHECK(selectedCount(real.stems001) == 30);
    for (const auto& ref : index->outsideNotes)
        CHECK(ref.note != nullptr && !ref.note->isSelected());
}

// The selection survives switching the item the canvas edits: the pointers are
// the real notes, so a republished index with another item order keeps them.
void testSelectionSurvivesItemSwitch()
{
    RealCase real;
    const auto before = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;

    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*before, "it2");
    for (const auto& pair : pairsOfRegion(*before, "it1"))
        pairs.push_back(pair);
    CHECK(selection.setFromPairs(pairs, *before));
    selection.applyToNotes();
    const int size = selection.size();
    CHECK(size == 76 + 30);

    // The host makes item 2 active: the plug-in republishes the same windows in
    // another order, with item 2 first.
    auto reordered = real.specs;
    std::rotate(reordered.begin(), reordered.begin() + 2, reordered.end());
    const auto after = buildDocumentNoteIndex("track", reordered);
    selection.refresh(after.get());

    CHECK(selection.size() == size);
    CHECK(selection.countForRegion("it2") == 76);
    CHECK(selection.countForRegion("it1") == 30);
    CHECK(selectedCount(real.stems) == 76);
    CHECK(selectedCount(real.stems001) == 30);

    std::vector<const Note*> left;
    std::vector<const Note*> right;
    for (const auto& entry : selection.entries())
        right.push_back(entry.note);
    for (const auto& ref : after->notes)
        if (selection.contains(ref.project, ref.note))
            left.push_back(ref.note);
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    CHECK(left == right); // same real notes, nothing lost or duplicated
}

// The same note shown by two sibling windows of one take counts once, and a
// track select-all does not select it twice.
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
    DocumentNoteSelection selection;

    // Both windows show the 6-7 s note; the marquee over both still adds one.
    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*index, "left");
    for (const auto& pair : pairsOfRegion(*index, "right"))
        pairs.push_back(pair);
    CHECK(selection.setFromPairs(pairs, *index));
    CHECK(selection.size() == 2);
    CHECK(selection.countForRegion("left") == 2); // the lowest window owns them
    CHECK(selection.countForRegion("right") == 0);

    selection.selectAll(*index);
    CHECK(selection.size() == 2); // not 3
    CHECK(selection.size() == index->totalNotes);

    selection.applyToNotes();
    CHECK(selectedCount(project) == 2);
}

// A project that goes away (item deleted, project reloaded) takes its selected
// notes with it instead of leaving stale pointers behind.
void testSelectionIsPrunedWhenProjectGoesAway()
{
    RealCase real;
    const auto before = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;

    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*before, "it1");
    for (const auto& pair : pairsOfRegion(*before, "it2"))
        pairs.push_back(pair);
    CHECK(selection.setFromPairs(pairs, *before));
    selection.applyToNotes();
    CHECK(selection.size() == 30 + 76);

    // Item 1 disappears from the track: its window is no longer in the index.
    std::vector<DocumentNoteRegionSpec> withoutB;
    for (const auto& spec : real.specs)
        if (spec.modificationKey != "modB")
            withoutB.push_back(spec);
    const auto after = buildDocumentNoteIndex("track", withoutB);
    selection.refresh(after.get());

    CHECK(selection.size() == 76);
    CHECK(selection.countForSource("Miu_stems_v2-001.wav") == 0);
    CHECK(selection.countForRegion("it2") == 76);
    for (const auto& entry : selection.entries())
        CHECK(entry.project == &real.stems);

    // A reloaded project is a different object: the old pointers are dropped,
    // and the new project's flags are what the set adopts.
    Project reloaded;
    setLength(reloaded, 116.5);
    addNotes(reloaded, 10.737, 35.157, 76, 60.0f);
    auto reloadedSpecs = withoutB;
    for (auto& spec : reloadedSpecs)
        if (spec.modificationKey == "modA")
            spec.project = &reloaded;
    const auto replaced = buildDocumentNoteIndex("track", reloadedSpecs);
    selection.refresh(replaced.get());
    CHECK(selection.size() == 0);
    CHECK(selection.liveProjects().size() == 1);
    CHECK(selection.liveProjects()[0] == &reloaded);

    // No document at all (clip mode) clears the set without touching flags.
    selection.refresh(nullptr);
    CHECK(selection.isEmpty());
}

} // namespace

int main()
{
    testMarqueeAcrossThreeItems();
    testSelectionSpansTwoSources();
    testAdditiveToggle();
    testSelectAllInTrackMode();
    testSelectionSurvivesItemSwitch();
    testSiblingWindowsDeduplicate();
    testSelectionIsPrunedWhenProjectGoesAway();

    std::printf("NoteSelectionTests: ok\n");
    return 0;
}
