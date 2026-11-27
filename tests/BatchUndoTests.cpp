// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
//
// N4 (D-344): un gesto en lote es UN paso de undo del documento.
//
// Measures, in the real editor path (PianoRollComponent + PitchUndoManager):
//   - a batch over three items and two Projects registers exactly one action;
//   - one undo puts every item back to the exact previous values (pitch, the
//     reference and the three tilt layers, timing and flags) and the exact
//     previous curves; redo restores the exact values the gesture left;
//   - a quantize also moves a shared boundary of an unselected neighbour, and
//     the same undo restores it (the whole item is in the record);
//   - the non-modal message says what was applied, in how many items, and that
//     it takes one step (because it does);
//   - a rejected batch writes nothing and registers nothing.

#include "TestAssert.h"

#include "../Source/Models/DocumentNoteBatch.h"
#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteSelection.h"
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/DocumentBatchAction.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/Localization.h"
#include "../Source/Utils/NotePitch.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "../Source/Utils/TransformParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
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
    project.getAudioData().sampleRate = kSampleRate;
    const int frames = frameFor(seconds);
    // The editor's frame count comes from the mel grid, and the curve rebuilds
    // compose into f0: both have to be sized for a curve to be built and
    // compared bit by bit after an undo.
    project.getAudioData().melSpectrogram.assign(
        static_cast<size_t>(frames), std::vector<float>(1, 0.0f));
    project.getAudioData().f0.assign(static_cast<size_t>(frames), 0.0f);
}

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

// Every value a batch gesture can change, note by note.
struct NoteValues
{
    TransformParams params;
    bool muted = false;
    bool deleted = false;
    bool rest = false;
    bool selected = false;
    int startFrame = 0;
    int endFrame = 0;

    static NoteValues of(const Note& note)
    {
        NoteValues values;
        values.params = TransformParams::fromNote(note);
        values.muted = note.isMuted();
        values.deleted = note.isDeleted();
        values.rest = note.isRest();
        values.selected = note.isSelected();
        values.startFrame = note.getStartFrame();
        values.endFrame = note.getEndFrame();
        return values;
    }
};

std::vector<NoteValues> valuesOf(const Project& project)
{
    std::vector<NoteValues> values;
    for (const auto& note : project.getNotes())
        values.push_back(NoteValues::of(note));
    return values;
}

std::vector<float> curveOf(const Project& project)
{
    std::vector<float> curve;
    const auto& audio = project.getAudioData();
    curve.insert(curve.end(), audio.basePitch.begin(), audio.basePitch.end());
    curve.insert(curve.end(), audio.deltaPitch.begin(), audio.deltaPitch.end());
    curve.insert(curve.end(), audio.f0.begin(), audio.f0.end());
    return curve;
}

bool sameCurve(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return false;
    return a.empty() ||
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

void checkValuesEqual(const std::vector<NoteValues>& expected,
                      const Project& project, const char* what)
{
    const auto actual = valuesOf(project);
    if (actual.size() != expected.size())
    {
        std::printf("%s: el numero de notas cambio\n", what);
        CHECK(actual.size() == expected.size());
        return;
    }
    for (size_t i = 0; i < expected.size(); ++i)
    {
        const auto& want = expected[i];
        const auto& have = actual[i];
        const bool same = want.params == have.params &&
                          want.muted == have.muted &&
                          want.deleted == have.deleted &&
                          want.rest == have.rest &&
                          want.selected == have.selected &&
                          want.startFrame == have.startFrame &&
                          want.endFrame == have.endFrame;
        if (!same)
        {
            std::printf("%s: la nota %d no es la misma (midi %.6f/%.6f, "
                        "tilt %.6f/%.6f)\n",
                        what, static_cast<int>(i), want.params.midiNote,
                        have.params.midiNote, want.params.tilt, have.params.tilt);
            CHECK(same);
            return;
        }
    }
}

// The measured track of N0/N3: items 0, 2, 3 and 4 of Miu_stems_v2.wav (one
// modification, one Project, contiguous cuts) and item 1 of the other source.
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

        // The same starting point the editor has after analysing: a curve built
        // from the notes, so an undo can be compared against it bit by bit.
        PitchCurveProcessor::rebuildBaseFromNotes(stems);
        PitchCurveProcessor::rebuildBaseFromNotes(stems001);
    }
};

// Runs one batch the way PianoRollComponent::applyDocumentBatch does (capture
// the items, run, record only what changed) without the editor.
struct BatchRun
{
    documentNoteBatch::Result result;
    bool recorded = false;
    int recordedNotes = 0;
};

BatchRun runBatch(
    PitchUndoManager& history,
    const std::vector<documentNoteBatch::Target>& targets,
    const std::function<documentNoteBatch::Result(
        const std::vector<documentNoteBatch::Target>&)>& operation,
    const juce::String& name, std::vector<Project*>* notified = nullptr)
{
    const auto before =
        documentNoteBatch::captureStates(documentNoteBatch::projectsOf(targets));
    BatchRun run;
    run.result = operation(targets);
    if (!run.result.applied())
        return run;
    auto changes = documentNoteBatch::changedSince(before);
    if (changes.empty())
        return run;
    run.recorded = true;
    run.recordedNotes = static_cast<int>(changes.size());
    auto notify = [notified](const std::vector<Project*>& projects)
    {
        if (notified != nullptr)
            *notified = projects;
    };
    history.addAction(std::make_unique<documentNoteBatch::DocumentBatchAction>(
        std::move(changes), notify, name));
    return run;
}

// Three items of the measured track, two Projects: items 1, 2 and 3.
void testThreeItemsOneStep()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;
    const auto targets = selectItems(index, selection, {"it1", "it2", "it3"});
    CHECK(targets.size() == 30 + 76 + 67);

    const auto stemsBefore = valuesOf(real.stems);
    const auto stems001Before = valuesOf(real.stems001);
    const auto stemsCurve = curveOf(real.stems);

    PitchUndoManager history;
    std::vector<Project*> notified;
    const auto pitch =
        runBatch(history, targets,
                 [](const std::vector<documentNoteBatch::Target>& items)
                 {
                     return documentNoteBatch::shiftPitch(
                         items, [](const Project&, const Note&)
                         { return std::optional<float>(1.0f); });
                 },
                 "Nudge Pitch", &notified);
    CHECK(pitch.result.applied());
    CHECK(pitch.result.notes == targets.size());
    CHECK(pitch.result.projects.size() == 2); // three items, two takes
    CHECK(pitch.recorded);
    CHECK(pitch.recordedNotes == static_cast<int>(targets.size()));
    CHECK(history.getUndoName() == "Nudge Pitch");
    CHECK(history.canUndo() && !history.canRedo());

    const auto stemsAfterPitch = valuesOf(real.stems);
    const auto stems001AfterPitch = valuesOf(real.stems001);
    CHECK(stemsAfterPitch.front().params.midiNote == 61.0f);
    CHECK(stems001AfterPitch.front().params.midiNote == 62.0f);

    // A second gesture on the same three items: the three tilt layers.
    const auto tilt =
        runBatch(history, targets,
                 [](const std::vector<documentNoteBatch::Target>& items)
                 {
                     auto result = documentNoteBatch::setTilt(
                         items, documentNoteBatch::TiltLayer::General, 1.0f);
                     result = documentNoteBatch::setTilt(
                         items, documentNoteBatch::TiltLayer::FineLeft, 0.5f);
                     documentNoteBatch::setTilt(
                         items, documentNoteBatch::TiltLayer::FineRight, -0.25f);
                     return result;
                 },
                 "Tilt", &notified);
    CHECK(tilt.result.applied());
    CHECK(history.canUndo());

    // Undo #2 (tilt only): all three items go back to the pitched state in one
    // step; the pitch of the first gesture is still there.
    CHECK(history.undo());
    checkValuesEqual(stemsAfterPitch, real.stems, "undo del tilt, stems");
    checkValuesEqual(stems001AfterPitch, real.stems001,
                     "undo del tilt, stems001");

    // Undo #1: every item is exactly where it started, curves included.
    CHECK(history.undo());
    checkValuesEqual(stemsBefore, real.stems, "undo del lote, stems");
    checkValuesEqual(stems001Before, real.stems001, "undo del lote, stems001");
    CHECK(sameCurve(stemsCurve, curveOf(real.stems)));
    CHECK(!history.canUndo());

    // Redo #1 and #2 restore exactly what each gesture left.
    CHECK(history.redo());
    checkValuesEqual(stemsAfterPitch, real.stems, "redo del lote, stems");
    checkValuesEqual(stems001AfterPitch, real.stems001, "redo del lote, stems001");
    CHECK(history.redo());
    CHECK(real.stems.getNotes().front().getTilt() == 1.0f);
    CHECK(real.stems.getNotes().front().getTiltFineLeft() == 0.5f);
    CHECK(real.stems.getNotes().front().getTiltFineRight() == -0.25f);
    CHECK(!history.canRedo());
}

// A quantize moves shared boundaries of unselected neighbours: the same single
// undo has to put them back.
void testQuantizeUndoRestoresNeighbour()
{
    Project project;
    setLength(project, 6.0);
    project.getNotes().push_back(makeNote(1.0, 1.5, 60.0f));
    project.getNotes().push_back(makeNote(1.5, 2.0, 61.0f));
    project.getNotes().push_back(makeNote(2.0, 2.5, 62.0f)); // unselected neighbour
    project.getNotes().push_back(makeNote(3.0, 3.5, 63.0f));
    std::vector<DocumentNoteRegionSpec> specs;
    // The host offset is not a grid multiple: only the host phase lands the
    // boundaries on the grid.
    specs.push_back(makeSpec("item", "modA", "a.wav", 11.1, 16.0, 1.0, 6.0,
                             &project));
    const auto index = buildDocumentNoteIndex("track", specs);
    DocumentNoteSelection selection;
    // Only the first two notes are selected; the index window shows all four.
    const auto regionPairs = pairsOfRegion(*index, "item");
    CHECK(regionPairs.size() == 4);
    CHECK(selection.setFromPairs({regionPairs[0], regionPairs[1]}, *index));
    const auto targets = documentNoteBatch::targetsOf(selection, *index);
    CHECK(targets.size() == 2);

    const auto before = valuesOf(project);
    PitchUndoManager history;
    const auto run =
        runBatch(history, targets,
                 [](const std::vector<documentNoteBatch::Target>& items)
                 { return documentNoteBatch::quantize(items, 0.5, 100); },
                 "Quantize Time");
    CHECK(run.result.applied());
    CHECK(run.recorded);
    // Two selected notes and the unselected neighbour whose shared boundary
    // moved: the record is the whole item, not only the selection.
    CHECK(run.recordedNotes == 3);

    // The unselected neighbour moved with the shared boundary.
    const auto after = valuesOf(project);
    CHECK(after[2].startFrame != before[2].startFrame);
    // ... and one undo puts all four notes back, timing included.
    CHECK(history.undo());
    checkValuesEqual(before, project, "undo del cuantizar");
    CHECK(history.redo());
    checkValuesEqual(after, project, "redo del cuantizar");
}

// The values the record cannot see are not touched, and a note that left its
// project is skipped instead of being written through a stale pointer.
void testStaleNoteIsSkipped()
{
    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    DocumentNoteSelection selection;
    const auto targets = selectItems(index, selection, {"it1", "it2"});

    PitchUndoManager history;
    runBatch(history, targets,
             [](const std::vector<documentNoteBatch::Target>& items)
             {
                 return documentNoteBatch::shiftPitch(
                     items, [](const Project&, const Note&)
                     { return std::optional<float>(1.0f); });
             },
             "Nudge Pitch");
    CHECK(history.canUndo());

    // The item is reloaded: its note list is gone.
    real.stems001.getNotes().clear();
    CHECK(history.undo()); // must not crash
    CHECK(real.stems001.getNotes().empty());
    // The items that are still there go back exactly.
    CHECK(real.stems.getNotes().front().getMidiNote() == 60.0f);
}

// A batch the planner refuses is not applied and leaves no undo step.
void testRejectedBatchRegistersNothing()
{
    // (a) unsafe grid: a 4-frame note collapses onto one grid point.
    Project first;
    setLength(first, 4.0);
    first.getNotes().push_back(makeNote(1.0, 1.5, 60.0f));
    first.getNotes().push_back(
        Note(frameFor(1.5), frameFor(1.5) + 4, 61.0f));
    std::vector<DocumentNoteRegionSpec> specs;
    specs.push_back(makeSpec("a", "modA", "a.wav", 11.1, 14.1, 1.0, 4.0,
                             &first));
    const auto index = buildDocumentNoteIndex("track", specs);
    DocumentNoteSelection selection;
    const auto targets = selectItems(index, selection, {"a"});
    const auto before = valuesOf(first);
    PitchUndoManager history;
    const auto run =
        runBatch(history, targets,
                 [](const std::vector<documentNoteBatch::Target>& items)
                 { return documentNoteBatch::quantize(items, 0.5, 100); },
                 "Quantize Time");
    CHECK(run.result.rejected());
    CHECK(!run.recorded);
    CHECK(!history.canUndo());
    checkValuesEqual(before, first, "lote rechazado");
}

// Three items, three Projects, through the real editor: one action, exact undo
// and redo, and the notice says so.
struct UiCase
{
    Project item0;
    Project item1;
    Project item2;
    std::vector<DocumentNoteRegionSpec> specs;
    DocumentNoteIndexPtr index;
    std::shared_ptr<DocumentNoteSelection> selection;
    PianoRollComponent roll;
    PitchUndoManager history;
    std::vector<juce::String> notices;

    UiCase()
    {
        setLength(item0, 4.0);
        setLength(item1, 4.0);
        setLength(item2, 4.0);
        addNotes(item0, 0.0, 4.0, 6, 60.0f);
        addNotes(item1, 0.0, 4.0, 6, 62.0f);
        addNotes(item2, 0.0, 4.0, 6, 64.0f);
        specs.push_back(makeSpec("it0", "modA", "a.wav", 0.0, 4.0, 0.0, 4.0,
                                 &item0));
        specs.push_back(makeSpec("it1", "modB", "b.wav", 4.0, 8.0, 0.0, 4.0,
                                 &item1));
        specs.push_back(makeSpec("it2", "modC", "c.wav", 8.0, 12.0, 0.0, 4.0,
                                 &item2));
        index = buildDocumentNoteIndex("track", specs);
        selection = std::make_shared<DocumentNoteSelection>();
        std::vector<std::pair<Project*, Note*>> pairs;
        for (const auto& region : index->regions)
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
        selection->setFromPairs(pairs, *index);

        roll.setSize(800, 400);
        roll.setProject(&item0); // the active item is the first one
        roll.setUndoManager(&history);
        roll.setTrackViewMode(true);
        roll.setDocumentNoteContext(index, selection);
        roll.onBatchNotice = [this](const juce::String& message)
        { notices.push_back(message); };
    }
};

void testEditorOneStepAndMessage()
{
    Localization::getInstance().setLanguage("en");
    UiCase ui;
    CHECK(ui.selection->size() == 18); // three items of six notes
    const auto before0 = valuesOf(ui.item0);
    const auto before1 = valuesOf(ui.item1);
    const auto before2 = valuesOf(ui.item2);

    // The command the menu and the keyboard share, over three items.
    CHECK(ui.roll.muteSelection());
    CHECK(ui.history.canUndo());
    CHECK(ui.history.getUndoName() == "Mute Notes");
    CHECK(ui.item0.getNotes().front().isMuted());
    CHECK(ui.item2.getNotes().back().isMuted());
    const auto afterMute0 = valuesOf(ui.item0);
    const auto afterMute1 = valuesOf(ui.item1);
    const auto afterMute2 = valuesOf(ui.item2);

    // One action, not one per item: a single undo puts every item back.
    CHECK(ui.history.undo());
    CHECK(!ui.history.canUndo());
    checkValuesEqual(before0, ui.item0, "editor undo, item 0");
    checkValuesEqual(before1, ui.item1, "editor undo, item 1");
    checkValuesEqual(before2, ui.item2, "editor undo, item 2");
    CHECK(ui.history.canRedo());
    CHECK(ui.history.redo());
    checkValuesEqual(afterMute0, ui.item0, "editor redo, item 0");
    checkValuesEqual(afterMute1, ui.item1, "editor redo, item 1");
    checkValuesEqual(afterMute2, ui.item2, "editor redo, item 2");

    // The message says what happened, in how many items, and one step.
    CHECK(ui.notices.size() == 1);
    if (!ui.notices.empty())
    {
        const auto& message = ui.notices.front();
        CHECK(message.contains("18"));
        CHECK(message.contains("3 items"));
        CHECK(message.contains("one step"));
    }

    // The tilt gesture of the inspector is the same single step, in Spanish.
    Localization::getInstance().setLanguage("es");
    const auto beforeTilt0 = valuesOf(ui.item0);
    const auto beforeTilt1 = valuesOf(ui.item1);
    const auto beforeTilt2 = valuesOf(ui.item2);
    ui.roll.beginInspectorEdit(PianoRollComponent::InspectorParam::Tilt);
    ui.roll.setInspectorEditValue(1.0f);
    ui.roll.endInspectorEdit();
    CHECK(ui.history.canUndo());
    CHECK(ui.item0.getNotes().front().getTilt() == 1.0f);
    CHECK(ui.item2.getNotes().back().getTilt() == 1.0f);
    const auto afterTilt0 = valuesOf(ui.item0);
    const auto afterTilt1 = valuesOf(ui.item1);
    const auto afterTilt2 = valuesOf(ui.item2);

    CHECK(ui.history.undo());
    checkValuesEqual(beforeTilt0, ui.item0, "undo del tilt del inspector, 0");
    checkValuesEqual(beforeTilt1, ui.item1, "undo del tilt del inspector, 1");
    checkValuesEqual(beforeTilt2, ui.item2, "undo del tilt del inspector, 2");
    CHECK(ui.history.redo());
    checkValuesEqual(afterTilt0, ui.item0, "redo del tilt del inspector, 0");
    checkValuesEqual(afterTilt1, ui.item1, "redo del tilt del inspector, 1");
    checkValuesEqual(afterTilt2, ui.item2, "redo del tilt del inspector, 2");

    CHECK(ui.notices.size() == 2);
    if (ui.notices.size() == 2)
    {
        const auto& message = ui.notices.back();
        CHECK(message.contains("18"));
        CHECK(message.contains(juce::String::fromUTF8("3 \xC3\xADtems")));
        CHECK(message.contains("un solo paso"));
    }
    Localization::getInstance().setLanguage("en");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    testThreeItemsOneStep();
    testQuantizeUndoRestoresNeighbour();
    testStaleNoteIsSkipped();
    testRejectedBatchRegistersNothing();
    testEditorOneStepAndMessage();

    std::printf("BatchUndoTests: ok\n");
    return 0;
}
