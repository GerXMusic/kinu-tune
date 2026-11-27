// N3 (notas del documento, D-343): the batch edit. One operation over the
// selection of the whole track applies to every selected note in its own item,
// in (region, note) order, and gives exactly what applying it note by note
// gives. This test reproduces the measured case (5 items, 2 sources: items
// 2/3/4 of one take and item 1 of the other) and pins the invariants of the
// phase: pitch and tilt by NotePitch byte for byte, mute/delete over several
// items, quantize with one host grid per item, a refused batch that writes
// nothing at all, and the unselected neighbours left intact.
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteBatch.h"
#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteSelection.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/NotePitch.h"
#include "../Source/Utils/TimingQuantize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    project.getAudioData().f0.assign(static_cast<size_t>(frameFor(seconds)), 0.0f);
}

// `count` notes spread inside [start, end), each 90% of its slot long.
void addNotes(Project& project, double start, double end, int count, float midi)
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

// The measured track: items 0, 2, 3 and 4 of Miu_stems_v2.wav (one
// modification, one Project, contiguous cuts) and item 1 of
// Miu_stems_v2-001.wav (the other source).
struct RealCase
{
    Project stems;
    Project stems001;
    std::vector<DocumentNoteRegionSpec> specs;

    RealCase()
    {
        setLength(stems, 116.5);
        addNotes(stems, 10.737, 35.157, 76, 60.0f);   // item 2
        addNotes(stems, 42.749, 68.930, 67, 62.0f);   // item 3
        addNotes(stems, 70.198, 115.787, 98, 64.0f);  // item 4
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

std::vector<std::pair<Project*, Note*>> pairsOfRegion(const DocumentNoteIndex& index,
                                                      const juce::String& key)
{
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& region : index.regions)
        if (region.key == key)
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
    return pairs;
}

// Selects the named items of a track and returns the batch targets.
std::vector<documentNoteBatch::Target> selectItems(
    const DocumentNoteIndexPtr& index, DocumentNoteSelection& selection,
    const std::vector<juce::String>& keys)
{
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& key : keys)
        for (const auto& pair : pairsOfRegion(*index, key))
            pairs.push_back(pair);
    if (!selection.setFromPairs(pairs, *index))
        return {};
    return documentNoteBatch::targetsOf(selection, *index);
}

bool sameBits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

struct PitchSnapshot
{
    float midi = 0.0f;
    float source = 0.0f;
};

std::vector<PitchSnapshot> pitchesOf(const Project& project)
{
    std::vector<PitchSnapshot> out;
    for (const auto& note : project.getNotes())
        out.push_back({note.getMidiNote(), note.getLastNonMacroMidiNote()});
    return out;
}

// Byte for byte the same pitch and reference, in the same note order.
void checkPitchEqual(const Project& a, const Project& b)
{
    CHECK(a.getNotes().size() == b.getNotes().size());
    for (size_t i = 0; i < a.getNotes().size(); ++i)
    {
        CHECK(sameBits(a.getNotes()[i].getMidiNote(), b.getNotes()[i].getMidiNote()));
        CHECK(sameBits(a.getNotes()[i].getLastNonMacroMidiNote(),
                       b.getNotes()[i].getLastNonMacroMidiNote()));
    }
}

void checkTiltEqual(const Project& a, const Project& b)
{
    CHECK(a.getNotes().size() == b.getNotes().size());
    for (size_t i = 0; i < a.getNotes().size(); ++i)
    {
        CHECK(sameBits(a.getNotes()[i].getTilt(), b.getNotes()[i].getTilt()));
        CHECK(sameBits(a.getNotes()[i].getTiltFineLeft(),
                       b.getNotes()[i].getTiltFineLeft()));
        CHECK(sameBits(a.getNotes()[i].getTiltFineRight(),
                       b.getNotes()[i].getTiltFineRight()));
    }
}

int selectedCount(const Project& project)
{
    int count = 0;
    for (const auto& note : project.getNotes())
        if (note.isSelected() && !note.isRest())
            ++count;
    return count;
}

// A batch over several items gives exactly what editing one note at a time
// gives, and it moves the selection only.
void testPitchBatchEqualsNoteByNote()
{
    RealCase batch, single;
    const auto keys = std::vector<juce::String>{"it1", "it2", "it3", "it4"};

    const auto batchIndex = buildDocumentNoteIndex("track", batch.specs);
    DocumentNoteSelection batchSelection;
    const auto targets =
        selectItems(batchIndex, batchSelection, keys);
    CHECK(targets.size() == 30 + 76 + 67 + 98);

    const auto singleIndex = buildDocumentNoteIndex("track", single.specs);
    DocumentNoteSelection singleSelection;
    const auto singleTargets =
        selectItems(singleIndex, singleSelection, keys);
    CHECK(singleTargets.size() == targets.size());

    // Deterministic order: region index first, then the note's index in its
    // own project. Item 1 is region 1 and comes before items 2, 3 and 4.
    CHECK(targets.front().project == &batch.stems001);
    CHECK(targets.front().regionKey == "it1");
    CHECK(targets[30].project == &batch.stems);
    CHECK(targets[30].regionKey == "it2");
    for (size_t i = 1; i < targets.size(); ++i)
        if (targets[i].project == targets[i - 1].project)
            CHECK(targets[i].note > targets[i - 1].note); // index order

    const auto stemsBefore = pitchesOf(batch.stems);
    const auto stems001Before = pitchesOf(batch.stems001);

    const auto result = documentNoteBatch::shiftPitch(
        targets, [](const Project&, const Note&) { return std::optional<float>(1.0f); });
    CHECK(result.applied());
    CHECK(result.notes == static_cast<int>(targets.size()));
    CHECK(result.projects.size() == 2);
    CHECK(result.projects[0] == &batch.stems001);
    CHECK(result.projects[1] == &batch.stems);

    // One note at a time, in the reverse order: the same values bit for bit.
    for (size_t i = singleTargets.size(); i-- > 0;)
    {
        const auto one = documentNoteBatch::shiftPitch(
            {singleTargets[i]},
            [](const Project&, const Note&) { return std::optional<float>(1.0f); });
        CHECK(one.applied());
        CHECK(one.notes == 1);
    }
    checkPitchEqual(batch.stems, single.stems);
    checkPitchEqual(batch.stems001, single.stems001);

    // Absolute: a semitone up from 60 and 61, and the reference carried by the
    // edit (NotePitch), never copied from the neighbour.
    CHECK(batch.stems001.getNotes().front().getMidiNote() == 62.0f);
    CHECK(batch.stems.getNotes()[76].getMidiNote() == 63.0f);
    CHECK(selectedCount(batch.stems) == 241);
    CHECK(selectedCount(batch.stems001) == 30);

    // The five notes no window shows are the neighbours here: untouched.
    for (size_t i = batch.stems.getNotes().size() - 5;
         i < batch.stems.getNotes().size(); ++i)
    {
        CHECK(sameBits(batch.stems.getNotes()[i].getMidiNote(),
                       stemsBefore[i].midi));
        CHECK(sameBits(batch.stems.getNotes()[i].getLastNonMacroMidiNote(),
                       stemsBefore[i].source));
    }
    CHECK(!stems001Before.empty());
}

// The reference of a corrected note moves with it: 60.0 sung at 60.3, up a
// semitone, is 61.0 with its reference at 61.3 (NotePitch::carriedCorrection).
void testPitchBatchCarriesTheReference()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;
    const auto targets =
        selectItems(index, selection, {"it1"});
    CHECK(targets.size() == 30);

    auto& note = *targets.front().note;
    NotePitch::restore(note, 60.0f, 60.3f);

    const auto result = documentNoteBatch::shiftPitch(
        {targets.front()},
        [](const Project&, const Note&) { return std::optional<float>(1.0f); });
    CHECK(result.applied());
    CHECK(note.getMidiNote() == 61.0f);
    CHECK(note.getLastNonMacroMidiNote() == 61.3f);
}

// Tilt is three fields written through the note's own setters; a batch writes
// the same three values as one note at a time and nothing else moves.
void testTiltBatchEqualsNoteByNote()
{
    RealCase batch, single;
    const auto keys = std::vector<juce::String>{"it1", "it3"};
    const auto batchIndex = buildDocumentNoteIndex("track", batch.specs);
    DocumentNoteSelection batchSelection;
    const auto targets = selectItems(batchIndex, batchSelection, keys);
    CHECK(targets.size() == 30 + 67);

    const auto singleIndex = buildDocumentNoteIndex("track", single.specs);
    DocumentNoteSelection singleSelection;
    const auto singleTargets =
        selectItems(singleIndex, singleSelection, keys);

    // Item 2 is in the same project as item 3 and is not selected: its notes
    // keep their tilt.
    const float neighbourTilt = batch.stems.getNotes().front().getTilt();

    const auto general = documentNoteBatch::setTilt(
        targets, documentNoteBatch::TiltLayer::General, 1.0f);
    CHECK(general.applied());
    CHECK(general.notes == 97);
    const auto left = documentNoteBatch::setTilt(
        targets, documentNoteBatch::TiltLayer::FineLeft, 0.5f);
    CHECK(left.applied());
    const auto right = documentNoteBatch::setTilt(
        targets, documentNoteBatch::TiltLayer::FineRight, -0.25f);
    CHECK(right.applied());

    for (const auto& target : singleTargets)
    {
        documentNoteBatch::setTilt({target},
                                   documentNoteBatch::TiltLayer::General, 1.0f);
        documentNoteBatch::setTilt({target},
                                   documentNoteBatch::TiltLayer::FineLeft, 0.5f);
        documentNoteBatch::setTilt({target},
                                   documentNoteBatch::TiltLayer::FineRight, -0.25f);
    }
    checkTiltEqual(batch.stems, single.stems);
    checkTiltEqual(batch.stems001, single.stems001);

    CHECK(batch.stems001.getNotes().front().getTilt() == 1.0f);
    CHECK(batch.stems001.getNotes().front().getTiltFineLeft() == 0.5f);
    CHECK(batch.stems001.getNotes().front().getTiltFineRight() == -0.25f);
    CHECK(batch.stems.getNotes().front().getTilt() == neighbourTilt);

    // The same value a second time is a no-op: no note is touched.
    const auto again = documentNoteBatch::setTilt(
        targets, documentNoteBatch::TiltLayer::General, 1.0f);
    CHECK(again.status == documentNoteBatch::Result::Status::Nothing);
    CHECK(again.notes == 0);
}

// Mute and delete in batch over two takes; a second pass changes nothing.
void testMuteAndDeleteInBatch()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;
    const auto targets = selectItems(index, selection, {"it1", "it3"});
    CHECK(targets.size() == 30 + 67);

    const auto muted = documentNoteBatch::setMuted(targets, true);
    CHECK(muted.applied());
    CHECK(muted.notes == 97);
    CHECK(muted.projects.size() == 2);
    for (const auto& target : targets)
        CHECK(target.note->isMuted());
    // Item 2 and item 4 share the project with item 3 and are not selected.
    CHECK(!real.stems.getNotes().front().isMuted());

    const auto again = documentNoteBatch::setMuted(targets, true);
    CHECK(again.status == documentNoteBatch::Result::Status::Nothing);

    const auto restored = documentNoteBatch::setMuted(targets, false);
    CHECK(restored.applied());
    CHECK(restored.notes == 97);

    // Delete: tombstone plus rest, and the flags of the set drop with them.
    const auto deleted = documentNoteBatch::deleteNotes(targets);
    CHECK(deleted.applied());
    CHECK(deleted.notes == 97);
    for (const auto& target : targets)
    {
        CHECK(target.note->isDeleted());
        CHECK(target.note->isRest());
        CHECK(!target.note->isSelected());
    }
    // The neighbours of item 2 are neither deleted nor turned into rests.
    for (size_t i = 0; i < 76; ++i)
    {
        CHECK(!real.stems.getNotes()[i].isDeleted());
        CHECK(!real.stems.getNotes()[i].isRest());
    }
    const auto deletedAgain = documentNoteBatch::deleteNotes(targets);
    CHECK(deletedAgain.status == documentNoteBatch::Result::Status::Nothing);
}

// Quantize: one host grid for the track, each item in its own phase.
struct QuantizeCase
{
    Project first;
    Project second;
    std::vector<DocumentNoteRegionSpec> specs;

    QuantizeCase()
    {
        setLength(first, 4.0);
        first.getNotes().push_back(makeNote(1.0, 1.5, 60.0f));
        first.getNotes().push_back(makeNote(1.6, 2.1, 60.0f));
        setLength(second, 4.0);
        second.getNotes().push_back(makeNote(0.2, 0.7, 62.0f));
        second.getNotes().push_back(makeNote(0.8, 1.3, 62.0f));

        // The offsets are not grid multiples: only using the host phase
        // (source + offset) lands the boundaries on the host grid.
        specs.push_back(makeSpec("a", "modA", "a.wav", 11.1, 14.1, 1.0, 4.0,
                                 &first));
        specs.push_back(makeSpec("b", "modB", "b.wav", 20.2, 23.2, 0.0, 3.0,
                                 &second));
    }
};

// The frames the phase convention says: the boundary is moved to the nearest
// host grid point (M4's quantizeFrame, on source frames shifted by the offset).
int expectedGridFrame(int sourceFrame, double offsetSeconds, double gridSeconds)
{
    const double framesPerSecond =
        static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
    const double gridFrames = gridSeconds * framesPerSecond;
    const int phaseFrames = static_cast<int>(
        std::llround(offsetSeconds * framesPerSecond));
    return timingQuantize::quantizeFrame(sourceFrame - phaseFrames, gridFrames,
                                         100) +
           phaseFrames;
}

void testQuantizeBatchUsesTheHostGrid()
{
    QuantizeCase batch, perItem;
    const auto batchIndex = buildDocumentNoteIndex("track", batch.specs);
    DocumentNoteSelection batchSelection;
    const auto targets = selectItems(batchIndex, batchSelection, {"a", "b"});
    CHECK(targets.size() == 4);

    const auto result = documentNoteBatch::quantize(targets, 0.5, 100);
    CHECK(result.applied());
    CHECK(result.projects.size() == 2);

    // Every boundary lands on the host grid of its own item (offset 10.1 for
    // the first item, 20.2 for the second), and the notes keep being adjacent
    // (no gap, no overlap).
    const Note& firstNote = batch.first.getNotes()[0];
    CHECK(firstNote.getStartFrame() ==
          expectedGridFrame(frameFor(1.0), 10.1, 0.5));
    CHECK(firstNote.getEndFrame() ==
          expectedGridFrame(frameFor(1.5), 10.1, 0.5));
    const Note& secondNote = batch.second.getNotes()[0];
    CHECK(secondNote.getStartFrame() ==
          expectedGridFrame(frameFor(0.2), 20.2, 0.5));
    CHECK(firstNote.getEndFrame() ==
          batch.first.getNotes()[1].getStartFrame());
    CHECK(secondNote.getEndFrame() ==
          batch.second.getNotes()[1].getStartFrame());
    // The host phase is used, not the source zero: without it the first
    // boundary of `second` would stay at 0.0 s of its source.
    CHECK(secondNote.getStartFrame() != frameFor(0.0));
    CHECK(firstNote.getStartFrame() != frameFor(1.0));

    // Region by region gives the same frames, exactly.
    const auto perIndex = buildDocumentNoteIndex("track", perItem.specs);
    DocumentNoteSelection perSelection;
    const auto perTargets = selectItems(perIndex, perSelection, {"a", "b"});
    std::vector<documentNoteBatch::Target> onlyFirst, onlySecond;
    for (const auto& target : perTargets)
        (target.project == &perItem.first ? onlyFirst : onlySecond)
            .push_back(target);
    CHECK(documentNoteBatch::quantize(onlyFirst, 0.5, 100).applied());
    CHECK(documentNoteBatch::quantize(onlySecond, 0.5, 100).applied());

    CHECK(perItem.first.getNotes().size() == batch.first.getNotes().size());
    for (size_t i = 0; i < batch.first.getNotes().size(); ++i)
    {
        CHECK(batch.first.getNotes()[i].getStartFrame() ==
              perItem.first.getNotes()[i].getStartFrame());
        CHECK(batch.first.getNotes()[i].getEndFrame() ==
              perItem.first.getNotes()[i].getEndFrame());
        CHECK(batch.second.getNotes()[i].getStartFrame() ==
              perItem.second.getNotes()[i].getStartFrame());
        CHECK(batch.second.getNotes()[i].getEndFrame() ==
              perItem.second.getNotes()[i].getEndFrame());
    }
}

// A refused batch writes nothing at all, not even the items whose own plan was
// valid.
void testRejectedBatchLeavesEverything()
{
    // (a) Unsafe grid: the 4-frame note of `first` collapses onto one grid
    // point, although `second` would be fine.
    QuantizeCase unsafe;
    unsafe.first.getNotes().clear();
    unsafe.first.getNotes().push_back(makeNote(1.0, 1.04, 60.0f));
    const auto unsafeIndex = buildDocumentNoteIndex("track", unsafe.specs);
    DocumentNoteSelection unsafeSelection;
    const auto unsafeTargets = selectItems(unsafeIndex, unsafeSelection, {"a", "b"});
    CHECK(unsafeTargets.size() == 3);

    std::vector<int> framesBefore;
    for (const auto& note : unsafe.first.getNotes())
        framesBefore.push_back(note.getStartFrame());
    for (const auto& note : unsafe.second.getNotes())
        framesBefore.push_back(note.getStartFrame());

    const auto rejected = documentNoteBatch::quantize(unsafeTargets, 0.5, 100);
    CHECK(rejected.rejected());
    CHECK(rejected.reason == "unsafe-grid");
    CHECK(rejected.notes == 0);
    CHECK(rejected.projects.empty());
    size_t cursor = 0;
    for (const auto& note : unsafe.first.getNotes())
        CHECK(note.getStartFrame() == framesBefore[cursor++]);
    for (const auto& note : unsafe.second.getNotes())
        CHECK(note.getStartFrame() == framesBefore[cursor++]);

    // (b) Two sibling windows of one item with different offsets (a stretched
    // clip): there is no single grid for the item, so the batch is refused.
    Project stretched;
    setLength(stretched, 4.0);
    stretched.getNotes().push_back(makeNote(0.2, 0.8, 60.0f));
    stretched.getNotes().push_back(makeNote(1.2, 1.8, 60.0f));
    std::vector<DocumentNoteRegionSpec> specs;
    specs.push_back(makeSpec("s1", "modA", "s.wav", 10.2, 10.9, 0.2, 0.9,
                             &stretched));
    specs.push_back(makeSpec("s2", "modA", "s.wav", 21.4, 22.0, 1.2, 1.8,
                             &stretched));
    const auto ambiguousIndex = buildDocumentNoteIndex("track", specs);
    DocumentNoteSelection ambiguousSelection;
    std::vector<std::pair<Project*, Note*>> ambiguousPairs;
    for (const auto& region : ambiguousIndex->regions)
        for (const auto& ref : region.notes)
            if (ref.note != nullptr)
                ambiguousPairs.push_back({ref.project, ref.note});
    CHECK(ambiguousSelection.setFromPairs(ambiguousPairs, *ambiguousIndex));
    const auto ambiguousTargets =
        documentNoteBatch::targetsOf(ambiguousSelection, *ambiguousIndex);
    CHECK(ambiguousTargets.size() == 2);

    const auto ambiguous = documentNoteBatch::quantize(ambiguousTargets, 0.5, 100);
    CHECK(ambiguous.rejected());
    CHECK(ambiguous.reason == "ambiguous-grid");
    CHECK(stretched.getNotes()[0].getStartFrame() == frameFor(0.2));
    CHECK(stretched.getNotes()[1].getStartFrame() == frameFor(1.2));

    // A pitch batch never leaves a half-edited selection either: a note at the
    // top of the range stays, the others still move.
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;
    const auto targets = selectItems(index, selection, {"it1"});
    NotePitch::restore(*targets.front().note, 89.0f, 89.0f); // MAX_MIDI_NOTE
    const auto moved = documentNoteBatch::shiftPitch(
        targets, [](const Project&, const Note&) { return std::optional<float>(1.0f); });
    CHECK(moved.applied());
    CHECK(moved.notes == 29);
    CHECK(targets.front().note->getMidiNote() == 89.0f);
}

// The two-window case needs its own selection helper: the pairs come from the
// published index, not from a name list.

// An empty selection is nothing, not an edit.
void testEmptySelectionIsNothing()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;
    const auto targets = documentNoteBatch::targetsOf(selection, *index);
    CHECK(targets.empty());
    CHECK(documentNoteBatch::shiftPitch(
              targets, [](const Project&, const Note&)
              { return std::optional<float>(1.0f); })
              .status == documentNoteBatch::Result::Status::Nothing);
    CHECK(documentNoteBatch::setMuted(targets, true).status ==
          documentNoteBatch::Result::Status::Nothing);
    CHECK(documentNoteBatch::deleteNotes(targets).status ==
          documentNoteBatch::Result::Status::Nothing);
    CHECK(documentNoteBatch::quantize(targets, 0.5, 100).status ==
          documentNoteBatch::Result::Status::Nothing);
}
} // namespace

int main()
{
    testPitchBatchEqualsNoteByNote();
    testPitchBatchCarriesTheReference();
    testTiltBatchEqualsNoteByNote();
    testMuteAndDeleteInBatch();
    testQuantizeBatchUsesTheHostGrid();
    testRejectedBatchLeavesEverything();
    testEmptySelectionIsNothing();

    std::printf("BatchEditTests: ok\n");
    return 0;
}
