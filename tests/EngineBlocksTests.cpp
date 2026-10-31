// Engine blocks (M2 paso 2, D-125 + D-129).
//
// The rule under test: a note may render with a different engine only when a
// usable silence (D-129: -55 dBFS for at least 20 ms) sits between it and the
// previous note. Otherwise the note inherits the block's engine. These checks
// exercise the grouping itself, the silence probe, and the predicate the UI
// will use to refuse a change, all without rendering a single sample.

#include "../Source/Audio/Synthesis/EngineBlocks.h"
#include "../Source/Models/Note.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kHop = HOP_SIZE;  // frames the notes live on
constexpr int kFrames = 60;

// A source where everything is a steady tone (-12 dBFS) except the spans
// explicitly cleared, which are digital silence.
juce::AudioBuffer<float> makeSource(std::vector<std::pair<int, int>> silent) {
    juce::AudioBuffer<float> buffer(1, kFrames * kHop);
    for (int i = 0; i < buffer.getNumSamples(); ++i) {
        const float value =
            0.25f * std::sin(2.0f * 3.14159265f * 220.0f *
                             static_cast<float>(i) / kSampleRate);
        buffer.setSample(0, i, value);
    }
    for (const auto &span : silent) {
        for (int s = span.first; s < span.second && s < buffer.getNumSamples();
             ++s)
            buffer.setSample(0, s, 0.0f);
    }
    return buffer;
}

Note makeNote(int startFrame, int endFrame, NoteEngine engine, float midi = 60.0f) {
    Note note(startFrame, endFrame, midi);
    note.setEngine(engine);
    return note;
}

void checkPlanCoversRange(const std::vector<engineBlocks::Block> &plan,
                          int startFrame, int endFrame) {
    CHECK(!plan.empty());
    CHECK(plan.front().startFrame == startFrame);
    CHECK(plan.back().endFrame == endFrame);
    for (size_t i = 0; i < plan.size(); ++i) {
        CHECK(plan[i].endFrame > plan[i].startFrame);
        if (i > 0)
            CHECK(plan[i].startFrame == plan[i - 1].endFrame);
    }
}

void testFindSilenceRunNeedsTwentyMilliseconds() {
    // 10 ms of silence: too short to be a frontier.
    {
        auto source = makeSource({{kHop * 10, kHop * 10 + 480}});
        CHECK(!engineBlocks::findSilenceRun(source, kSampleRate, kHop * 10,
                                            kHop * 12)
                   .has_value());
    }
    // 30 ms of silence: usable, and the run never reaches outside it.
    {
        const int from = kHop * 10;
        const int to = kHop * 14;
        const int silentStart = from + 400;
        const int silentEnd = silentStart + kSampleRate * 30 / 1000;
        auto source = makeSource({{silentStart, silentEnd}});
        const auto run =
            engineBlocks::findSilenceRun(source, kSampleRate, from, to);
        CHECK(run.has_value());
        if (run) {
            CHECK(run->startSample >= from);
            CHECK(run->endSample <= to);
            CHECK(run->startSample >= silentStart);
            CHECK(run->endSample <= silentEnd);
            CHECK(run->getNumSamples() >= kSampleRate * 20 / 1000);
        }
    }
}

void testSilenceRunRejectsTone() {
    auto source = makeSource({});
    CHECK(!engineBlocks::findSilenceRun(source, kSampleRate, kHop * 10,
                                        kHop * 20)
               .has_value());
}

void testSingleEngineIsOneBlock() {
    auto source = makeSource({});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(12, 20, NoteEngine::Psola),
        makeNote(20, 28, NoteEngine::Psola),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                              SynthesisEngineType::Vocoder,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 1);
    CHECK(plan.front().startFrame == 0);
    CHECK(plan.front().endFrame == kFrames);
    CHECK(plan.front().engine == SynthesisEngineType::Psola);
    checkPlanCoversRange(plan, 0, kFrames);
}

void testDifferentEngineWithoutSilenceInherits() {
    // Two touching notes asking for different engines: no room for a frontier,
    // so the second one renders with the block's engine.
    auto source = makeSource({});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(12, 20, NoteEngine::Pitchcore),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                              SynthesisEngineType::Psola,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 1);
    CHECK(plan.front().engine == SynthesisEngineType::Psola);
    checkPlanCoversRange(plan, 0, kFrames);
}

void testDifferentEngineWithSilenceSplits() {
    // A 100 ms gap with 30 ms of digital silence inside: two blocks, and the
    // boundary falls inside the silence, never inside a note.
    const int gapStart = 12 * kHop;
    const int gapEnd = 22 * kHop;
    const int silentStart = gapStart + 1200;
    const int silentEnd = silentStart + kSampleRate * 30 / 1000;
    auto source = makeSource({{silentStart, silentEnd}});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(22, 30, NoteEngine::Pitchcore),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                              SynthesisEngineType::Psola,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 2);
    CHECK(plan[0].engine == SynthesisEngineType::Psola);
    CHECK(plan[1].engine == SynthesisEngineType::Pitchcore);
    CHECK(plan[1].startFrame * kHop >= silentStart);
    CHECK(plan[1].startFrame * kHop <= silentEnd);
    checkPlanCoversRange(plan, 0, kFrames);
}

void testSilenceOutsideTheGapDoesNotSplit() {
    // The silence is inside the second note, not between the notes: the note
    // cannot change engine because it has no gap at all.
    auto source = makeSource({{22 * kHop + 400, 22 * kHop + 4000}});
    std::vector<Note> notes{
        makeNote(4, 18, NoteEngine::Psola),
        makeNote(18, 30, NoteEngine::Pitchcore),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                              SynthesisEngineType::Psola,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 1);
    CHECK(plan.front().engine == SynthesisEngineType::Psola);
}

void testInheritFollowsTheBlockEngine() {
    // Inherit asks for nothing: it renders with the instance engine and never
    // forces a frontier, even with plenty of silence around.
    const int silentStart = 12 * kHop + 800;
    const int silentEnd = silentStart + kSampleRate * 40 / 1000;
    auto source = makeSource({{silentStart, silentEnd}});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Inherit),
        makeNote(22, 30, NoteEngine::Inherit),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                              SynthesisEngineType::Vocoder,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 1);
    CHECK(plan.front().engine == SynthesisEngineType::Vocoder);

    // Inherit is resolved from the current global preference each time; the
    // per-note field does not pin or mutate that preference.
    const auto classicPlan = engineBlocks::buildPlan(
        notes, 0, kFrames, SynthesisEngineType::Psola, source, kSampleRate,
        kHop);
    CHECK(classicPlan.size() == 1);
    CHECK(classicPlan.front().engine == SynthesisEngineType::Psola);
}

void testExplicitEngineEqualToGlobalDoesNotSplit() {
    const int silentStart = 12 * kHop + 800;
    const int silentEnd = silentStart + kSampleRate * 40 / 1000;
    auto source = makeSource({{silentStart, silentEnd}});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Inherit),
        makeNote(22, 30, NoteEngine::Psola),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                              SynthesisEngineType::Psola,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 1);
    CHECK(plan.front().engine == SynthesisEngineType::Psola);
}

void testEmptyRangeUsesGlobalEngine() {
    auto source = makeSource({});
    const std::vector<Note> notes;
    const auto plan = engineBlocks::buildPlan(notes, 10, 20,
                                              SynthesisEngineType::Pitchcore,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 1);
    CHECK(plan.front().startFrame == 10);
    CHECK(plan.front().endFrame == 20);
    CHECK(plan.front().engine == SynthesisEngineType::Pitchcore);
}

void testRestsDoNotCarryAnEngine() {
    // A rest between the notes widens the gap; the engine frontier needs the
    // silence in the audio, not the rest's existence, but the rest itself is
    // never a block with its own engine.
    const int silentStart = 12 * kHop + 800;
    const int silentEnd = silentStart + kSampleRate * 40 / 1000;
    auto source = makeSource({{silentStart, silentEnd}});
    Note rest(12, 22, 0.0f);
    rest.setRest(true);
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        rest,
        makeNote(22, 30, NoteEngine::Pitchcore),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                              SynthesisEngineType::Psola,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 2);
    CHECK(plan[0].engine == SynthesisEngineType::Psola);
    CHECK(plan[1].engine == SynthesisEngineType::Pitchcore);
    checkPlanCoversRange(plan, 0, kFrames);
}

void testPredicateMatchesThePlan() {
    // The UI's predicate and the renderer's plan must agree: if
    // canChangeEngineAfter says no, buildPlan does not split; if it says yes,
    // it does.
    const int silentStart = 12 * kHop + 800;
    const int silentEnd = silentStart + kSampleRate * 40 / 1000;
    auto source = makeSource({{silentStart, silentEnd}});

    const Note first = makeNote(4, 12, NoteEngine::Psola);
    const Note withSilence = makeNote(22, 30, NoteEngine::Pitchcore);
    const Note withoutSilence = makeNote(12, 20, NoteEngine::Pitchcore);

    CHECK(engineBlocks::canChangeEngineAfter(first, withSilence, source,
                                             kSampleRate, kHop));
    CHECK(!engineBlocks::canChangeEngineAfter(first, withoutSilence, source,
                                              kSampleRate, kHop));

    const auto split = engineBlocks::buildPlan(
        {first, withSilence}, 0, kFrames, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    CHECK(split.size() == 2);

    const auto merged = engineBlocks::buildPlan(
        {first, withoutSilence}, 0, kFrames, SynthesisEngineType::Psola,
        source, kSampleRate, kHop);
    CHECK(merged.size() == 1);
}

void testSelectionMovesTogetherWhenThereIsSilence() {
    // Two notes selected, a usable silence between them: both take the
    // requested engine, and the stored field matches what will render.
    const int silentStart = 12 * kHop + 800;
    const int silentEnd = silentStart + kSampleRate * 40 / 1000;
    auto source = makeSource({{silentStart, silentEnd}});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(22, 30, NoteEngine::Psola),
    };
    const auto plan = engineBlocks::planEngineEdits(
        notes, {0, 1}, NoteEngine::Pitchcore, SynthesisEngineType::Psola,
        source, kSampleRate, kHop);
    CHECK(plan.refused == 0);
    CHECK(plan.edits.size() == 2);
    for (const auto &edit : plan.edits)
        CHECK(edit.next == NoteEngine::Pitchcore);
}

void testSelectionRefusesWhenThereIsNoSilence() {
    // Touching notes: only the first can break away. The second keeps the
    // block's engine and is reported as refused, so the UI can say so.
    auto source = makeSource({});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(12, 20, NoteEngine::Psola),
    };
    const auto plan = engineBlocks::planEngineEdits(
        notes, {1}, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    CHECK(plan.refused == 1);
    CHECK(plan.edits.size() == 1);
    if (!plan.edits.empty())
        CHECK(plan.edits.front().next == NoteEngine::Inherit);
}

void testRefusedNoteGetsTheBlockEngineStored() {
    // A project can hold a pinned engine that the renderer ignores (written by
    // an older build, or by a rule that changed). Asking for it again does not
    // grant it: the field is corrected to the engine that does render it.
    auto source = makeSource({});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(12, 20, NoteEngine::Vocoder),
    };
    const auto plan = engineBlocks::planEngineEdits(
        notes, {1}, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    CHECK(plan.refused == 1);
    CHECK(plan.edits.size() == 1);
    if (!plan.edits.empty()) {
        CHECK(plan.edits.front().previous == NoteEngine::Vocoder);
        // The block renders with the global engine (PSOLA), so the field goes
        // back to Inherit rather than pinning a copy of it.
        CHECK(plan.edits.front().next == NoteEngine::Inherit);
    }
}

void testInheritRequestReturnsANoteToTheGlobalEngine() {
    auto source = makeSource({{12 * kHop + 800,
                               12 * kHop + 800 + kSampleRate * 40 / 1000}});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(22, 30, NoteEngine::Pitchcore),
    };
    const auto plan = engineBlocks::planEngineEdits(
        notes, {1}, NoteEngine::Inherit, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    CHECK(plan.refused == 0);
    CHECK(plan.edits.size() == 1);
    if (!plan.edits.empty())
        CHECK(plan.edits.front().next == NoteEngine::Inherit);
}

void testInheritClearsAnOverrideEvenWhenAudioAlreadyUsesGlobal() {
    auto source = makeSource({});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
    };
    const auto plan = engineBlocks::planEngineEdits(
        notes, {0}, NoteEngine::Inherit, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    CHECK(plan.refused == 0);
    CHECK(plan.edits.size() == 1);
    if (!plan.edits.empty())
        CHECK(plan.edits.front().next == NoteEngine::Inherit);
}

void testUnselectedNotesKeepTheirEngineAndTheirBlock() {
    // The middle note is not selected and stays Pitchcore; the third note,
    // touching it, cannot go back to the global engine.
    auto source = makeSource({});
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        makeNote(12, 20, NoteEngine::Pitchcore),
        makeNote(20, 28, NoteEngine::Inherit),
    };
    const auto plan = engineBlocks::planEngineEdits(
        notes, {2}, NoteEngine::Psola, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    // It already renders as Pitchcore, so nothing changes in the audio; the
    // field is corrected to say so.
    CHECK(plan.refused == 1);
    CHECK(plan.edits.size() == 1);
    if (!plan.edits.empty())
        CHECK(plan.edits.front().next == NoteEngine::Pitchcore);
}

void testRestsDoNotBecomeThePreviousNoteForEngineEdits() {
    // Rest placeholders carry no engine and must not create or break a block.
    auto source = makeSource({});
    Note rest(12, 22, 0.0f);
    rest.setRest(true);
    std::vector<Note> notes{
        makeNote(4, 12, NoteEngine::Psola),
        rest,
        makeNote(22, 30, NoteEngine::Psola),
    };
    const auto plan = engineBlocks::planEngineEdits(
        notes, {2}, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    CHECK(plan.refused == 1);
    CHECK(plan.edits.empty());
}

void testTwoTouchingNotesWithDifferentEnginesKeepOneEngine() {
    // The case the user called absurd: two contiguous notes, no silence,
    // different engines. The plan must not produce two motors there.
    auto source = makeSource({});
    std::vector<Note> notes{
        makeNote(0, 10, NoteEngine::Vocoder),
        makeNote(10, 20, NoteEngine::Pitchcore),
        makeNote(20, 30, NoteEngine::Psola),
    };
    const auto plan = engineBlocks::buildPlan(notes, 0, 30,
                                              SynthesisEngineType::Vocoder,
                                              source, kSampleRate, kHop);
    CHECK(plan.size() == 1);
    CHECK(plan.front().engine == SynthesisEngineType::Vocoder);
}

}  // namespace

int main() {
    testFindSilenceRunNeedsTwentyMilliseconds();
    testSilenceRunRejectsTone();
    testSingleEngineIsOneBlock();
    testDifferentEngineWithoutSilenceInherits();
    testDifferentEngineWithSilenceSplits();
    testSilenceOutsideTheGapDoesNotSplit();
    testInheritFollowsTheBlockEngine();
    testExplicitEngineEqualToGlobalDoesNotSplit();
    testEmptyRangeUsesGlobalEngine();
    testRestsDoNotCarryAnEngine();
    testPredicateMatchesThePlan();
    testSelectionMovesTogetherWhenThereIsSilence();
    testSelectionRefusesWhenThereIsNoSilence();
    testRefusedNoteGetsTheBlockEngineStored();
    testInheritRequestReturnsANoteToTheGlobalEngine();
    testInheritClearsAnOverrideEvenWhenAudioAlreadyUsesGlobal();
    testUnselectedNotesKeepTheirEngineAndTheirBlock();
    testRestsDoNotBecomeThePreviousNoteForEngineEdits();
    testTwoTouchingNotesWithDifferentEnginesKeepOneEngine();

    std::cout << "EngineBlocksTests: all checks passed\n";
    return 0;
}
