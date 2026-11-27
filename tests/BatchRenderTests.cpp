// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
//
// N5 (D-345): the render of a batch gesture, one request per affected ITEM.
//
// D-247: whoever marks the audio dirty has to request its render; a dirty item
// without a request is heard mute (or, since D-285, with the audio it had
// before the edit, which for an edit is just as wrong). A batch touches several
// items, so what is measured here is:
//   - three affected modifications -> exactly three requests (not one for the
//     whole document, not one per note and not two per item);
//   - one modification reached through two windows, or by two operations of the
//     same gesture, is requested once;
//   - an item outside the gesture is not requested and does not change;
//   - undoing and redoing the gesture asks again, once per item (the undo path
//     is a path that dirties, so it requests too);
//   - a batch whose items have no render path yet is refused before writing;
//   - and the real render of the requested items is not silent, only touches
//     the block it commits and leaves the unedited audio bit for bit as it was
//     (the checks of CommitSeam for the block unit, D-239).

#include "CommitSeamChecks.h"
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteBatch.h"
#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteSelection.h"
#include "../Source/Models/DocumentRenderBatch.h"
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/Localization.h"
#include "../Source/Utils/PitchCurveProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
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

std::vector<float> valuesOf(const Project& project)
{
    std::vector<float> values;
    for (const auto& note : project.getNotes())
        values.push_back(note.getMidiNote());
    return values;
}

// What the editor asked the backend to render, per item.
struct RequestLog
{
    std::map<Project*, int> counts;
    int total() const
    {
        int sum = 0;
        for (const auto& entry : counts)
            sum += entry.second;
        return sum;
    }
    int countFor(const Project* project) const
    {
        const auto found = counts.find(const_cast<Project*>(project));
        return found == counts.end() ? 0 : found->second;
    }
    void clear() { counts.clear(); }
};

// The fake backend target of one item: counts the request, does not render.
TrackPitchMacroTarget countingTarget(Project* project, RequestLog& log,
                                     bool valid = true)
{
    TrackPitchMacroTarget target;
    target.project = project;
    target.isValid = [valid] { return valid; };
    target.onChanged = [project, &log](bool render)
    {
        if (render)
            ++log.counts[project];
    };
    target.onCommit = [] {};
    return target;
}

// Three items, three Projects, plus a fourth item nobody selects. The active
// item is the first one, so its render goes through the editor's own path.
struct UiCase
{
    Project item0;
    Project item1;
    Project item2;
    Project untouched;
    std::vector<DocumentNoteRegionSpec> specs;
    DocumentNoteIndexPtr index;
    std::shared_ptr<DocumentNoteSelection> selection;
    PianoRollComponent roll;
    PitchUndoManager history;
    RequestLog log;
    std::vector<juce::String> notices;
    int activeRequests = 0;
    int visualUpdates = 0;

    UiCase()
    {
        setLength(item0, 4.0);
        setLength(item1, 4.0);
        setLength(item2, 4.0);
        setLength(untouched, 4.0);
        addNotes(item0, 0.0, 4.0, 6, 60.0f);
        addNotes(item1, 0.0, 4.0, 6, 62.0f);
        addNotes(item2, 0.0, 4.0, 6, 64.0f);
        addNotes(untouched, 0.0, 4.0, 6, 65.0f);
        specs.push_back(
            makeSpec("it0", "modA", "a.wav", 0.0, 4.0, 0.0, 4.0, &item0));
        specs.push_back(
            makeSpec("it1", "modB", "b.wav", 4.0, 8.0, 0.0, 4.0, &item1));
        specs.push_back(
            makeSpec("it2", "modC", "c.wav", 8.0, 12.0, 0.0, 4.0, &item2));
        specs.push_back(makeSpec("it3", "modD", "d.wav", 12.0, 16.0, 0.0, 4.0,
                                 &untouched));
        index = buildDocumentNoteIndex("track", specs);

        roll.setSize(800, 400);
        roll.setProject(&item0); // the active item is the first one
        roll.setUndoManager(&history);
        roll.setTrackViewMode(true);
        selectKeys({"it0", "it1", "it2"});
        roll.setDocumentNoteContext(index, selection);
        roll.onBatchNotice = [this](const juce::String& message)
        { notices.push_back(message); };
        roll.onPitchEditFinished = [this] { ++activeRequests; };
        roll.onPitchEdited = [this] { ++visualUpdates; };
        // Every item the index holds has a backend target, including the one
        // nobody selects: a request for it would be a render too many.
        roll.getTrackEditTargets = [this]
        {
            return std::vector<TrackPitchMacroTarget>{
                countingTarget(&item0, log), countingTarget(&item1, log),
                countingTarget(&item2, log), countingTarget(&untouched, log)};
        };
    }

    void selectAll()
    {
        selection = std::make_shared<DocumentNoteSelection>();
        std::vector<std::pair<Project*, Note*>> pairs;
        for (const auto& region : index->regions)
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
        selection->setFromPairs(pairs, *index);
    }

    // The gesture covers these windows only; the other items of the track are
    // there (and are drawn) but nobody selects them.
    void selectKeys(const std::vector<juce::String>& keys)
    {
        selection = std::make_shared<DocumentNoteSelection>();
        std::vector<std::pair<Project*, Note*>> pairs;
        for (const auto& region : index->regions)
        {
            if (std::find(keys.begin(), keys.end(), region.key) == keys.end())
                continue;
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
        }
        selection->setFromPairs(pairs, *index);
    }

    void resetCounts()
    {
        log.clear();
        activeRequests = 0;
        visualUpdates = 0;
    }
    int activeItem() const { return log.countFor(&item0) + activeRequests; }
    // Every path that requests a render of this item counts: the active item's
    // own (onPitchEditFinished) and the backend target of the others.
    int requestsFor(const Project* project) const
    {
        return log.countFor(project) +
               (project == &item0 ? activeRequests : 0);
    }
};

// --- the unit: one entry per item, one request per entry -------------------

void testGestureDeduplicatesByItem()
{
    Project a, b;
    documentRenderBatch::Gesture gesture;
    gesture.add(&a, true);
    gesture.add(&a, false); // a second window of the same take
    gesture.add(&a, true);  // or a second operation of the same gesture
    gesture.add(&b);
    gesture.add(nullptr);
    CHECK(gesture.size() == 2);
    CHECK(gesture.countFor(&a) == 1);
    CHECK(gesture.contains(&a) && gesture.contains(&b));
    CHECK(gesture.requests().front().project == &a);
    CHECK(gesture.requests().front().active);

    RequestLog log;
    const auto outcome = documentRenderBatch::flush(
        gesture.take(), [&log](const documentRenderBatch::Request& request)
        {
            ++log.counts[request.project];
            return true;
        });
    CHECK(outcome.complete());
    CHECK(outcome.count() == 2);
    CHECK(outcome.countFor(&a) == 1 && outcome.countFor(&b) == 1);
    CHECK(log.countFor(&a) == 1 && log.countFor(&b) == 1);
    CHECK(gesture.empty());

    // A requester that cannot reach an item reports it instead of dropping it.
    documentRenderBatch::Gesture second;
    second.add(&a);
    second.add(&b);
    const auto partial = documentRenderBatch::flush(
        second.take(),
        [&a](const documentRenderBatch::Request& request)
        { return request.project == &a; });
    CHECK(!partial.complete());
    CHECK(partial.countFor(&a) == 1);
    CHECK(partial.unreachable.size() == 1 && partial.unreachable.front() == &b);
}

// --- the editor: three modifications, three requests -----------------------

void testThreeModificationsThreeRequests()
{
    Localization::getInstance().setLanguage("en");
    UiCase ui;
    const auto beforeUntouched = valuesOf(ui.untouched);

    CHECK(ui.roll.muteSelection());
    CHECK(ui.log.total() + ui.activeRequests == 3);
    // One per item: never one for the document, never two per item.
    CHECK(ui.activeItem() == 1);
    CHECK(ui.log.countFor(&ui.item1) == 1);
    CHECK(ui.log.countFor(&ui.item2) == 1);
    CHECK(ui.activeRequests == 1);
    // The item outside the gesture: no request, no change.
    CHECK(ui.log.countFor(&ui.untouched) == 0);
    CHECK(valuesOf(ui.untouched) == beforeUntouched);
    CHECK(!ui.untouched.hasDirtyNotes() && !ui.untouched.hasF0DirtyRange());
    CHECK(ui.notices.size() == 1); // applied, and no render warning

    // Nothing is left dirty without its request (the mute of D-247).
    for (Project* project : {&ui.item0, &ui.item1, &ui.item2})
    {
        const bool dirty =
            project->hasDirtyNotes() || project->hasF0DirtyRange();
        CHECK(dirty);                        // the edit did mark it
        CHECK(ui.requestsFor(project) == 1); // and asked for its render
    }

    // Undo dirties every item again: it is another gesture, one request each.
    ui.resetCounts();
    CHECK(ui.history.undo());
    CHECK(ui.activeItem() == 1);
    CHECK(ui.log.countFor(&ui.item1) == 1);
    CHECK(ui.log.countFor(&ui.item2) == 1);
    CHECK(ui.log.countFor(&ui.untouched) == 0);

    // Redo, the same.
    ui.resetCounts();
    CHECK(ui.history.redo());
    CHECK(ui.activeItem() == 1);
    CHECK(ui.log.countFor(&ui.item1) == 1);
    CHECK(ui.log.countFor(&ui.item2) == 1);

    // A second operation of the same gesture (the three tilt layers are written
    // in one inspector gesture): one request per item, not one per operation.
    ui.resetCounts();
    const int noticesBefore = static_cast<int>(ui.notices.size());
    ui.roll.beginInspectorEdit(PianoRollComponent::InspectorParam::Tilt);
    ui.roll.setInspectorEditValue(0.5f);
    ui.roll.setInspectorEditValue(1.0f);
    ui.roll.endInspectorEdit();
    CHECK(ui.activeItem() == 1);
    CHECK(ui.log.countFor(&ui.item1) == 1);
    CHECK(ui.log.countFor(&ui.item2) == 1);
    CHECK(ui.log.countFor(&ui.untouched) == 0);
    CHECK(static_cast<int>(ui.notices.size()) == noticesBefore + 1);
}

// --- the editor: two windows of one take, one request ----------------------

void testTwoWindowsOneRequest()
{
    Project take;
    Project other;
    setLength(take, 6.0);
    setLength(other, 3.0);
    addNotes(take, 0.0, 6.0, 10, 60.0f);
    addNotes(other, 0.0, 3.0, 6, 62.0f);
    // Two contiguous cuts of the same modification and a second source.
    auto specs = std::vector<DocumentNoteRegionSpec>{
        makeSpec("cut0", "modA", "take.wav", 0.0, 3.0, 0.0, 3.0, &take),
        makeSpec("cut1", "modA", "take.wav", 3.0, 6.0, 3.0, 6.0, &take),
        makeSpec("other", "modB", "other.wav", 6.0, 9.0, 0.0, 3.0, &other)};
    const auto index = buildDocumentNoteIndex("track", specs);

    PianoRollComponent roll;
    roll.setSize(800, 400);
    roll.setProject(&take); // the active item is one half of the take
    roll.setTrackViewMode(true);
    auto selection = std::make_shared<DocumentNoteSelection>();
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& region : index->regions)
        for (const auto& ref : region.notes)
            if (ref.note != nullptr)
                pairs.push_back({ref.project, ref.note});
    CHECK(selection->setFromPairs(pairs, *index));
    roll.setDocumentNoteContext(index, selection);

    RequestLog log;
    int activeRequests = 0;
    std::vector<juce::String> notices;
    roll.onBatchNotice = [&notices](const juce::String& message)
    { notices.push_back(message); };
    roll.onPitchEditFinished = [&activeRequests] { ++activeRequests; };
    roll.getTrackEditTargets = [&]
    {
        return std::vector<TrackPitchMacroTarget>{
            countingTarget(&take, log), countingTarget(&other, log)};
    };

    PitchUndoManager history;
    roll.setUndoManager(&history);
    CHECK(roll.muteSelection());
    // Three windows, two items: two requests, and the take once even though two
    // of its cuts (and both operations of the toggle) reached it.
    CHECK(activeRequests == 1);
    CHECK(log.countFor(&other) == 1);
    CHECK(log.countFor(&take) == 0); // the active item goes through the editor
    CHECK(log.total() + activeRequests == 2);
    CHECK(notices.size() == 1);
}

// --- the D-247 pre-check: no path to a render, no edit ---------------------

void testBatchWithoutRenderPathIsRefused()
{
    UiCase ui;
    // The backend has no targets yet (the track is still loading): the batch
    // would be applied mute, so it must not be applied at all.
    ui.roll.getTrackEditTargets = [] {
        return std::vector<TrackPitchMacroTarget>{};
    };
    ui.resetCounts();
    const auto before0 = valuesOf(ui.item0);
    const auto before1 = valuesOf(ui.item1);
    CHECK(!ui.roll.muteSelection());
    CHECK(valuesOf(ui.item0) == before0);
    CHECK(valuesOf(ui.item1) == before1);
    CHECK(!ui.item0.hasDirtyNotes() && !ui.item1.hasDirtyNotes());
    CHECK(!ui.history.canUndo());
    CHECK(ui.log.total() == 0 && ui.activeRequests == 0);
    CHECK(ui.notices.size() == 1);
    if (!ui.notices.empty())
        CHECK(ui.notices.front().contains("render"));
}

// --- the real render: no silence, block unit, unedited audio intact --------

constexpr int kSeamSampleRate = 48000;
constexpr int kSeamHop = HOP_SIZE;
constexpr int kSeamTotalFrames = 500;
constexpr int kNote0Start = 50;
constexpr int kNote0End = 200;
constexpr int kNote1End = 350;

juce::AudioBuffer<float> makePhrase(float midi0, float midi1)
{
    juce::AudioBuffer<float> buffer(1, kSeamTotalFrames * kSeamHop);
    double phase = 0.0;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const int frame = i / kSeamHop;
        float value = 0.0f;
        if (frame >= kNote0Start && frame < kNote1End)
        {
            const float midi = frame < kNote0End ? midi0 : midi1;
            const double frequency =
                static_cast<double>(commitSeamChecks::midiHz(midi));
            phase += 2.0 * 3.14159265358979323846 * frequency / kSeamSampleRate;
            value = 0.30f * static_cast<float>(std::sin(phase)) +
                    0.12f * static_cast<float>(std::sin(2.0 * phase)) +
                    0.06f * static_cast<float>(std::sin(3.0 * phase));
        }
        else
        {
            phase = 0.0;
        }
        buffer.setSample(0, i, value);
    }
    const int fade = kSeamSampleRate / 100;
    const int first = kNote0Start * kSeamHop;
    const int last = kNote1End * kSeamHop;
    for (int i = 0; i < fade; ++i)
    {
        const float gain = static_cast<float>(i) / static_cast<float>(fade);
        buffer.setSample(0, first + i, buffer.getSample(0, first + i) * gain);
        buffer.setSample(0, last - 1 - i,
                         buffer.getSample(0, last - 1 - i) * gain);
    }
    return buffer;
}

Project makeSeamProject(float midi0, float midi1)
{
    Project project;
    auto& audio = project.getAudioData();
    audio.sampleRate = kSeamSampleRate;
    const auto phrase = makePhrase(midi0, midi1);
    audio.originalWaveform.makeCopyOf(phrase);
    audio.waveform.makeCopyOf(phrase);
    audio.melSpectrogram.assign(static_cast<size_t>(kSeamTotalFrames),
                                std::vector<float>(NUM_MELS, 1.0f));
    audio.denseF0.assign(static_cast<size_t>(kSeamTotalFrames), 0.0f);
    audio.voicedMask.assign(static_cast<size_t>(kSeamTotalFrames), false);
    for (int frame = kNote0Start; frame < kNote1End; ++frame)
    {
        audio.voicedMask[static_cast<size_t>(frame)] = true;
        audio.denseF0[static_cast<size_t>(frame)] = commitSeamChecks::midiHz(
            frame < kNote0End ? midi0 : midi1);
    }
    Note note0(kNote0Start, kNote0End, midi0);
    note0.setOriginalMidiNote(midi0);
    note0.setOriginalDeltaPitch(std::vector<float>(kNote0End - kNote0Start, 0.0f));
    Note note1(kNote0End, kNote1End, midi1);
    note1.setOriginalMidiNote(midi1);
    note1.setOriginalDeltaPitch(std::vector<float>(kNote1End - kNote0End, 0.0f));
    project.addNote(note0);
    project.addNote(note1);
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    return project;
}

void testRealRenderOfTheAffectedItems()
{
    Project affected0 = makeSeamProject(57.0f, 59.0f);
    Project affected1 = makeSeamProject(60.0f, 62.0f);
    Project affected2 = makeSeamProject(62.0f, 64.0f);
    Project untouched = makeSeamProject(64.0f, 65.0f);
    const auto untouchedAudio = untouched.getAudioData().waveform;
    const auto untouchedNotes = valuesOf(untouched);

    auto specs = std::vector<DocumentNoteRegionSpec>{
        makeSpec("a0", "modA", "a.wav", 0.0, 5.3, 0.0, 5.3, &affected0),
        makeSpec("a1", "modB", "b.wav", 5.3, 10.6, 0.0, 5.3, &affected1),
        makeSpec("a2", "modC", "c.wav", 10.6, 15.9, 0.0, 5.3, &affected2),
        makeSpec("a3", "modD", "d.wav", 15.9, 21.2, 0.0, 5.3, &untouched)};
    const auto index = buildDocumentNoteIndex("track", specs);
    DocumentNoteSelection selection;
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& region : index->regions)
        if (region.key != "a3") // the fourth item is outside the gesture
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
    CHECK(selection.setFromPairs(pairs, *index));
    const auto targets = documentNoteBatch::targetsOf(selection, *index);
    CHECK(targets.size() == 6); // two notes of each of the three items

    // The real operation, applied exactly as the editor does.
    const auto result = documentNoteBatch::shiftPitch(
        targets, [](const Project&, const Note&)
        { return std::optional<float>(1.0f); });
    CHECK(result.applied());
    CHECK(result.projects.size() == 3);

    // The gesture asks for one render per item and the backend renders it.
    documentRenderBatch::Gesture gesture;
    for (auto* project : result.projects)
        gesture.add(project, project == &affected0);
    const auto outcome = documentRenderBatch::flush(
        gesture.take(),
        [](const documentRenderBatch::Request& request)
        {
            return commitSeamChecks::renderAndWait(
                *request.project, SynthesisEngineType::Psola, "batch render");
        });
    CHECK(outcome.complete());
    CHECK(outcome.count() == 3);
    CHECK(!outcome.touched(&untouched));

    for (Project* project : {&affected0, &affected1, &affected2})
    {
        const auto& audio = project->getAudioData();
        // Not silent where the phrase is: the requested render arrived.
        const float peak = commitSeamChecks::peakAbs(
            audio.waveform, kNote0Start * kSeamHop, kNote0End * kSeamHop);
        CHECK(peak > 1.0e-2f);
        // The edited note was really rewritten ...
        const float noteDiff = commitSeamChecks::maxAbsDiff(
            audio.waveform, audio.originalWaveform, kNote0Start * kSeamHop,
            kNote0End * kSeamHop);
        CHECK(noteDiff > 1.0e-3f);
        // ... and the neighbour of the same engine block went with it, which is
        // the D-239 unit (the old per-note commit left the original there).
        const float boundary = commitSeamChecks::maxAbsDiff(
            audio.waveform, audio.originalWaveform, 195 * kSeamHop,
            215 * kSeamHop);
        CHECK(boundary > 1.0e-4f);
        // Outside the committed block the audio is bit for bit what it was.
        const float outside = commitSeamChecks::maxAbsDiffOutside(
            audio.waveform, audio.originalWaveform, 0, 390 * kSeamHop + 4096);
        CHECK(outside <= 1.0e-7f);
        // And the pass closed: nothing dirty left waiting for a render.
        CHECK(!project->hasDirtyNotes());
    }

    // The item outside the gesture was never rendered and never changed.
    const auto& untouchedAfter = untouched.getAudioData();
    CHECK(untouchedAfter.waveform.getNumSamples() == untouchedAudio.getNumSamples());
    CHECK(commitSeamChecks::maxAbsDiff(untouchedAfter.waveform, untouchedAudio, 0,
                                       untouchedAudio.getNumSamples()) == 0.0f);
    CHECK(valuesOf(untouched) == untouchedNotes);
    CHECK(!untouched.hasDirtyNotes());
}

// The measured track of N0: five items, two sources (one take cut in four).
// The whole track selected is two requests, not five: the take renders once.
void testRealTrackFiveItemsTwoRequests()
{
    Project stems;
    Project stems001;
    setLength(stems, 116.5);
    addNotes(stems, 10.737, 35.157, 76, 60.0f);
    addNotes(stems, 42.749, 68.930, 67, 62.0f);
    addNotes(stems, 70.198, 115.787, 98, 64.0f);
    setLength(stems001, 3.8);
    addNotes(stems001, 0.0, 3.66, 30, 61.0f);
    auto specs = std::vector<DocumentNoteRegionSpec>{
        makeSpec("it0", "modA", "Miu_stems_v2.wav", 5.818, 12.924, 0.0, 7.106,
                 &stems),
        makeSpec("it1", "modB", "Miu_stems_v2-001.wav", 12.914, 16.574, 0.0,
                 3.660, &stems001),
        makeSpec("it2", "modA", "Miu_stems_v2.wav", 16.555, 40.975, 10.737,
                 35.157, &stems),
        makeSpec("it3", "modA", "Miu_stems_v2.wav", 48.567, 74.748, 42.749,
                 68.930, &stems),
        makeSpec("it4", "modA", "Miu_stems_v2.wav", 76.016, 121.605, 70.198,
                 115.787, &stems)};
    const auto index = buildDocumentNoteIndex("track", specs);
    PianoRollComponent roll;
    roll.setSize(800, 400);
    roll.setProject(&stems);
    roll.setTrackViewMode(true);
    auto selection = std::make_shared<DocumentNoteSelection>();
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& region : index->regions)
        for (const auto& ref : region.notes)
            if (ref.note != nullptr)
                pairs.push_back({ref.project, ref.note});
    CHECK(selection->setFromPairs(pairs, *index));
    roll.setDocumentNoteContext(index, selection);

    RequestLog log;
    int activeRequests = 0;
    roll.onBatchNotice = [](const juce::String&) {};
    roll.onPitchEditFinished = [&activeRequests] { ++activeRequests; };
    roll.getTrackEditTargets = [&]
    {
        return std::vector<TrackPitchMacroTarget>{
            countingTarget(&stems, log), countingTarget(&stems001, log)};
    };
    PitchUndoManager history;
    roll.setUndoManager(&history);

    CHECK(roll.deleteSelection());
    // Two sources, one request each; the four cuts of the take are one item.
    CHECK(activeRequests == 1);
    CHECK(log.countFor(&stems001) == 1);
    CHECK(log.countFor(&stems) == 0);
    CHECK(log.total() + activeRequests == 2);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    testGestureDeduplicatesByItem();
    testThreeModificationsThreeRequests();
    testTwoWindowsOneRequest();
    testBatchWithoutRenderPathIsRefused();
    testRealRenderOfTheAffectedItems();
    testRealTrackFiveItemsTwoRequests();

    std::printf("BatchRenderTests: ok\n");
    return 0;
}
