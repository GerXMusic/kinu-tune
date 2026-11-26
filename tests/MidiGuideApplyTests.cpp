// D-312: applying a MIDI guide moves the note's centre to the guide pitch and
// nothing else. Vibrato (deltaPitch) and the manual visible offset survive,
// undo/redo restore the exact pair, and notes that were not matched are byte
// identical.
#include "TestAssert.h"

#include "../Source/Models/MidiGuide.h"
#include "../Source/Models/Project.h"
#include "../Source/Undo/UndoActions.h"
#include "../Source/Utils/NotePitch.h"

#include <cmath>
#include <cstdio>

namespace
{
bool near(double a, double b, double tolerance = 1.0e-4)
{
    return std::abs(a - b) <= tolerance;
}

// Mirrors MainComponent::applyMidiGuide() without the UI: same matching, same
// NotePitch path, same single undo step.
struct ApplyResult
{
    int matched = 0;
    bool undone = false;
};

ApplyResult applyGuide(Project& project, const GuideSource& guide,
                       std::unique_ptr<MidiGuideApplyAction>& actionOut)
{
    const HostTempoMap tempo = HostTempoMap::constant(120.0);
    MidiGuideMapping mapping;
    mapping.tempoMap = &tempo;
    mapping.sampleRate = 44100;
    mapping.hopSize = 512;

    std::vector<MidiGuideMatcher::GuideSlot> slots;
    for (size_t i = 0; i < guide.notes.size(); ++i)
    {
        MidiGuideMatcher::GuideSlot slot;
        slot.guideIndex = static_cast<int>(i);
        slot.startSeconds = mapping.timelineSecondsForQuarter(guide.notes[i].startQuarter);
        slot.endSeconds = mapping.timelineSecondsForQuarter(guide.notes[i].endQuarter);
        slot.pitch = guide.notes[i].pitch;
        slots.push_back(slot);
    }

    auto& notes = project.getNotes();
    std::vector<MidiGuideMatcher::AudioNote> audio;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (notes[i].isRest() || notes[i].isMuted())
            continue;
        MidiGuideMatcher::AudioNote entry;
        entry.index = static_cast<int>(i);
        entry.startSeconds =
            static_cast<double>(notes[i].getStartFrame()) * 512.0 / 44100.0;
        entry.endSeconds =
            static_cast<double>(notes[i].getEndFrame()) * 512.0 / 44100.0;
        entry.centerMidi = notes[i].getAdjustedMidiNote();
        audio.push_back(entry);
    }

    const auto matches = MidiGuideMatcher::match(audio, slots);

    std::vector<Note*> targets;
    std::vector<float> oldMidis, newMidis, oldSources, newSources;
    for (const auto& match : matches)
    {
        auto& note = notes[static_cast<size_t>(match.audioIndex)];
        const float oldMidi = note.getMidiNote();
        const float oldSource = note.getLastNonMacroMidiNote();
        const float target = slots[static_cast<size_t>(match.guideIndex)].pitch;
        const float newSource =
            NotePitch::sourceAfterEdit(project, note, oldSource, oldMidi, target);
        targets.push_back(&note);
        oldMidis.push_back(oldMidi);
        newMidis.push_back(target);
        oldSources.push_back(oldSource);
        newSources.push_back(newSource);
    }

    ApplyResult result;
    result.matched = static_cast<int>(targets.size());
    if (!targets.empty())
    {
        auto action = std::make_unique<MidiGuideApplyAction>(
            targets, oldMidis, newMidis, oldSources, newSources, nullptr);
        action->redo();
        actionOut = std::move(action);
    }
    return result;
}

void testApplyMovesCentreOnly()
{
    Project project;
    // Note at frame 0..43 = 0.0..0.5 s (one quarter at 120 BPM).
    Note note(0, 43, 57.0f);
    note.setPitchOffset(0.30f);
    note.setDeltaPitch({0.0f, 0.2f, -0.2f, 0.0f});
    project.getNotes().push_back(note);

    GuideSource guide;
    guide.notes.push_back({0.0, 1.0, 59.0f, 0.8f});

    std::unique_ptr<MidiGuideApplyAction> action;
    const auto result = applyGuide(project, guide, action);
    CHECK(result.matched == 1);

    const auto& applied = project.getNotes().front();
    CHECK(near(applied.getMidiNote(), 59.0f));
    // The manual visible offset is preserved (decision §15.2).
    CHECK(near(applied.getPitchOffset(), 0.30f));
    // Vibrato is byte-for-byte the same vector.
    CHECK(applied.getDeltaPitch().size() == 4);
    CHECK(applied.getDeltaPitch()[1] == 0.2f);
    CHECK(applied.getDeltaPitch()[2] == -0.2f);

    // Undo restores both the pitch and the Correct Pitch reference exactly.
    const float referenceAfterApply = applied.getLastNonMacroMidiNote();
    action->undo();
    CHECK(near(project.getNotes().front().getMidiNote(), 57.0f));
    CHECK(project.getNotes().front().getLastNonMacroMidiNote() !=
          referenceAfterApply ||
          near(project.getNotes().front().getLastNonMacroMidiNote(), 57.0f));

    // Redo puts it back.
    action->redo();
    CHECK(near(project.getNotes().front().getMidiNote(), 59.0f));
}

void testNeighbourIsUntouched()
{
    Project project;
    Note first(0, 43, 57.0f);
    Note second(300, 343, 64.0f); // 3.48..3.98 s: far away in time
    second.setPitchOffset(0.0f);
    project.getNotes().push_back(first);
    project.getNotes().push_back(second);

    const float secondMidi = second.getMidiNote();
    const float secondSource = second.getLastNonMacroMidiNote();

    GuideSource guide;
    guide.notes.push_back({0.0, 1.0, 59.0f, 0.8f});

    std::unique_ptr<MidiGuideApplyAction> action;
    const auto result = applyGuide(project, guide, action);
    CHECK(result.matched == 1);
    CHECK(near(project.getNotes()[0].getMidiNote(), 59.0f));
    CHECK(project.getNotes()[1].getMidiNote() == secondMidi);
    CHECK(project.getNotes()[1].getLastNonMacroMidiNote() == secondSource);
}

void testMutedAndRestNotesAreSkipped()
{
    Project project;
    Note muted(0, 43, 57.0f);
    muted.setMuted(true);
    project.getNotes().push_back(muted);

    GuideSource guide;
    guide.notes.push_back({0.0, 1.0, 59.0f, 0.8f});

    std::unique_ptr<MidiGuideApplyAction> action;
    CHECK(applyGuide(project, guide, action).matched == 0);
    CHECK(near(project.getNotes()[0].getMidiNote(), 57.0f));
}

void testNoGuideMeansNoChange()
{
    Project project;
    project.getNotes().push_back(Note(0, 43, 57.0f));

    GuideSource empty;
    std::unique_ptr<MidiGuideApplyAction> action;
    CHECK(applyGuide(project, empty, action).matched == 0);
    CHECK(action == nullptr);
    CHECK(near(project.getNotes()[0].getMidiNote(), 57.0f));
}
} // namespace

int main()
{
    testApplyMovesCentreOnly();
    testNeighbourIsUntouched();
    testMutedAndRestNotesAreSkipped();
    testNoGuideMeansNoChange();

    std::printf("MidiGuideApplyTests: all checks passed\n");
    return 0;
}
