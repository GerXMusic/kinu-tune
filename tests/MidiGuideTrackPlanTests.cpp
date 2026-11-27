// R8 (guia MIDI, D-361): el plan de aplicacion por pista resuelve, sin escribir
// nada, que modificacion/nota recibe cada nota de guia. Dos casos:
//  (1) sintetico: la misma fuente en dos ventanas, otra fuente, un item sin
//      analizar y un hueco; los cuatro motivos de skip, sin duplicados y con
//      resultado determinista aunque cambie el orden de las regiones;
//  (2) real: las ventanas medidas del item 2 y del item 4 (offset 5,818184),
//      guia de 111 notas calibrada a la medida R1 (55 dentro / 53 fuera): el
//      item 2 propone sus 55 cambios y el item 4 no propone ninguno.
#include "TestAssert.h"

#include "../Source/Models/GuideApplicationPlan.h"
#include "../Source/Models/MidiGuide.h"
#include "../Source/Models/Project.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr int kSampleRate = 44100;
constexpr int kHopSize = 512;
const MidiGuideSampling kSampling{kSampleRate, kHopSize};

bool near(double a, double b, double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

int frameForProjectSeconds(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * kSampleRate / static_cast<double>(kHopSize)));
}

Note makeNote(double startSeconds, double endSeconds, float midi)
{
    return Note(frameForProjectSeconds(startSeconds),
                frameForProjectSeconds(endSeconds), midi);
}

HostTempoMap tempo120()
{
    return HostTempoMap::constant(120.0);
}

// 120 BPM: one quarter is 0.5 s, so quarters = host seconds * 2.
GuideNote guideSlot(double hostStart, double hostEnd, float pitch)
{
    GuideNote note;
    note.startQuarter = hostStart * 2.0;
    note.endQuarter = hostEnd * 2.0;
    note.pitch = pitch;
    return note;
}

GuideApplicationRegion makeRegion(const char* modificationKey,
                                  const char* sourceKey, double hostStart,
                                  double hostEnd, double sourceStart,
                                  double sourceEnd, double offset,
                                  const Project* project)
{
    GuideApplicationRegion region;
    region.modificationKey = modificationKey;
    region.sourceKey = sourceKey;
    region.hostStartSeconds = hostStart;
    region.hostEndSeconds = hostEnd;
    region.sourceStartSeconds = sourceStart;
    region.sourceEndSeconds = sourceEnd;
    region.offsetSeconds = offset;
    region.project = project;
    return region;
}

const GuideModificationPlan* findPlan(const GuideApplicationPlan& plan,
                                      const char* key)
{
    for (const auto& modification : plan.modifications)
        if (modification.modificationKey == juce::String(key))
            return &modification;
    return nullptr;
}

//============================================================================
// (1) Sintetico
//============================================================================
void testSyntheticTrack()
{
    // Two windows of the same source (a1, a2), one item of another source (b1)
    // that overlaps a2's tail, an analysed-free item (c1) and a gap 40-50 s.
    Project a1Project;
    Project a2Project;
    Project bProject;
    a1Project.getNotes().push_back(makeNote(1.2, 1.8, 57.0f));
    a2Project.getNotes().push_back(makeNote(11.2, 11.8, 57.0f)); // ventana 10-20
    bProject.getNotes().push_back(makeNote(1.2, 1.8, 57.0f));  // host 20.2

    std::vector<GuideApplicationRegion> regions = {
        makeRegion("a1", "stem", 0.0, 10.0, 0.0, 10.0, 0.0, &a1Project),
        makeRegion("a2", "stem", 10.0, 20.0, 10.0, 20.0, 0.0, &a2Project),
        makeRegion("b1", "other", 19.0, 29.0, 0.0, 10.0, 19.0, &bProject),
        makeRegion("c1", "stem", 30.0, 40.0, 20.0, 30.0, 10.0, nullptr),
    };

    GuideSource guide;
    guide.id = "synthetic";
    // g1 duplicates g0's note (dedupe: it must not become a second edit).
    // g7 overlaps a2 and b1 (different sources) with no note in either.
    guide.notes = {
        guideSlot(1.0, 2.0, 60.0f),    // 0: a1, note at 1.2-1.8
        guideSlot(1.3, 1.6, 60.0f),    // 1: a1, same note -> dedupe
        guideSlot(11.0, 12.0, 60.0f),  // 2: a2, note at 11.2-11.8
        guideSlot(20.0, 21.0, 60.0f),  // 3: b1, note at 20.2-20.8
        guideSlot(31.0, 32.0, 60.0f),  // 4: c1, no project -> not-analyzed
        guideSlot(41.0, 42.0, 60.0f),  // 5: gap -> no-item
        guideSlot(5.0, 6.0, 60.0f),    // 6: a1 with no note -> no-match
        guideSlot(19.2, 19.8, 60.0f),  // 7: a2+b1, different sources
    };

    const auto plan = planGuideApplication(guide, tempo120(), regions, kSampling);

    CHECK(plan.guideNotes == 8);
    CHECK(plan.modifications.size() == 3);
    CHECK(plan.editedNotes == 3);
    CHECK(plan.changedNotes == 3);
    CHECK(plan.appliedSlots == 4); // g0, g1 (same note), g2, g3

    const auto* a1 = findPlan(plan, "a1");
    const auto* a2 = findPlan(plan, "a2");
    const auto* b1 = findPlan(plan, "b1");
    CHECK(a1 != nullptr);
    CHECK(a2 != nullptr);
    CHECK(b1 != nullptr);
    CHECK(findPlan(plan, "c1") == nullptr);
    CHECK(!plan.appliesTo("c1"));

    CHECK(a1->project == &a1Project);
    CHECK(a1->edits.size() == 1); // the duplicate slot did not add a second edit
    CHECK(a1->edits[0].noteIndex == 0);
    CHECK(a1->edits[0].guideIndex == 0); // tie -> earlier slot wins
    CHECK(near(a1->edits[0].targetPitch, 60.0));
    CHECK(near(a1->edits[0].currentMidi, 57.0));
    CHECK(a1->guideNotes == 2); // both slots resolved to it
    CHECK(near(a1->hostStartSeconds, 1.0));
    CHECK(near(a1->hostEndSeconds, 2.0));

    CHECK(a2->edits.size() == 1);
    CHECK(a2->edits[0].noteIndex == 0);
    CHECK(b1->edits.size() == 1);
    CHECK(b1->edits[0].noteIndex == 0);

    CHECK(plan.skipped.size() == 4);
    CHECK(plan.skippedCount(GuideSkipReason::NoItem) == 1);
    CHECK(plan.skippedCount(GuideSkipReason::NotAnalyzed) == 1);
    CHECK(plan.skippedCount(GuideSkipReason::NoMatch) == 1);
    CHECK(plan.skippedCount(GuideSkipReason::DifferentSource) == 1);
    CHECK(plan.skippedCount(GuideSkipReason::NoItem) +
              plan.skippedCount(GuideSkipReason::NotAnalyzed) +
              plan.skippedCount(GuideSkipReason::NoMatch) +
              plan.skippedCount(GuideSkipReason::DifferentSource) ==
          plan.guideNotes - plan.appliedSlots);
    CHECK(plan.skipped[0].guideIndex == 4);
    CHECK(plan.skipped[0].reason == GuideSkipReason::NotAnalyzed);
    CHECK(plan.skipped[1].guideIndex == 5);
    CHECK(plan.skipped[1].reason == GuideSkipReason::NoItem);
    CHECK(plan.skipped[2].guideIndex == 6);
    CHECK(plan.skipped[2].reason == GuideSkipReason::NoMatch);
    CHECK(plan.skipped[3].guideIndex == 7);
    CHECK(plan.skipped[3].reason == GuideSkipReason::DifferentSource);

    // The plan is read-only: the notes keep their centre.
    CHECK(near(a1Project.getNotes()[0].getMidiNote(), 57.0f));
    CHECK(near(a2Project.getNotes()[0].getMidiNote(), 57.0f));
    CHECK(near(bProject.getNotes()[0].getMidiNote(), 57.0f));

    // Same input twice -> identical plan, same order.
    const auto again = planGuideApplication(guide, tempo120(), regions, kSampling);
    CHECK(again.modifications.size() == plan.modifications.size());
    for (size_t m = 0; m < plan.modifications.size(); ++m)
    {
        CHECK(again.modifications[m].modificationKey ==
              plan.modifications[m].modificationKey);
        CHECK(again.modifications[m].edits.size() ==
              plan.modifications[m].edits.size());
        for (size_t e = 0; e < plan.modifications[m].edits.size(); ++e)
        {
            CHECK(again.modifications[m].edits[e].noteIndex ==
                  plan.modifications[m].edits[e].noteIndex);
            CHECK(again.modifications[m].edits[e].guideIndex ==
                  plan.modifications[m].edits[e].guideIndex);
            CHECK(again.modifications[m].edits[e].targetPitch ==
                  plan.modifications[m].edits[e].targetPitch);
        }
    }
    CHECK(again.skipped.size() == plan.skipped.size());
    for (size_t s = 0; s < plan.skipped.size(); ++s)
    {
        CHECK(again.skipped[s].guideIndex == plan.skipped[s].guideIndex);
        CHECK(again.skipped[s].reason == plan.skipped[s].reason);
        CHECK(again.skipped[s].sourceKey == plan.skipped[s].sourceKey);
    }

    // Reversing the region list must not change the outcome.
    std::vector<GuideApplicationRegion> reversed(regions.rbegin(),
                                                 regions.rend());
    const auto swapped =
        planGuideApplication(guide, tempo120(), reversed, kSampling);
    CHECK(swapped.modifications.size() == plan.modifications.size());
    CHECK(swapped.editedNotes == plan.editedNotes);
    CHECK(swapped.appliedSlots == plan.appliedSlots);
    CHECK(swapped.skippedCount(GuideSkipReason::DifferentSource) == 1);
    for (size_t s = 0; s < plan.skipped.size(); ++s)
    {
        CHECK(swapped.skipped[s].guideIndex == plan.skipped[s].guideIndex);
        CHECK(swapped.skipped[s].reason == plan.skipped[s].reason);
        CHECK(swapped.skipped[s].sourceKey == plan.skipped[s].sourceKey);
    }
}

//============================================================================
// (2) Real: item 2 (55 dentro) e item 4 (0 cambios)
//============================================================================
constexpr double kOffset = 5.818184; // projectToTimeline(0), medido

// Guia calibrada a la medida R1: 111 notas, 55 casan dentro del item 2 y 53
// caen fuera de los dos items de este caso (en la pista completa caen en los
// items 0/1/3, que no se incluyen aqui para medir el par medido).
std::vector<GuideNote> measuredGuide()
{
    std::vector<GuideNote> notes;
    const int inside = 58; // 55 con nota + 3 sin nota (no-match)
    const double insideStart = 16.6;
    const double insideEnd = 40.9;
    const double inStep = (insideEnd - insideStart) / inside;
    for (int i = 0; i < inside; ++i)
    {
        const double start = insideStart + inStep * i;
        notes.push_back(guideSlot(start, start + inStep * 0.9, 60.0f));
    }
    const int outside = 53;
    const double outsideStart = 41.0;
    const double outsideEnd = 73.9;
    const double outStep = (outsideEnd - outsideStart) / outside;
    for (int i = 0; i < outside; ++i)
    {
        const double start = outsideStart + outStep * i;
        notes.push_back(guideSlot(start, start + outStep * 0.9, 60.0f));
    }
    return notes;
}

void testMeasuredItem2AndItem4()
{
    Project item2;
    Project item4;
    Project item0;
    Project item1;
    Project item3;

    // Item 2: a note under the first 55 slots (the last 3 stay unmatched).
    const auto guideNotes = measuredGuide();
    for (int i = 0; i < 55; ++i)
        item2.getNotes().push_back(
            makeNote(guideNotes[static_cast<size_t>(i)].startQuarter / 2.0 - kOffset,
                     guideNotes[static_cast<size_t>(i)].endQuarter / 2.0 - kOffset,
                     57.0f));

    // Item 4: notes inside its window but the guide never reaches it.
    for (int i = 0; i < 110; ++i)
    {
        const double start = 70.2 + i * 0.4;
        item4.getNotes().push_back(makeNote(start, start + 0.3, 57.0f));
    }
    // Item 1 is another source: its notes are too far in pitch to match.
    for (int i = 0; i < 7; ++i)
        item1.getNotes().push_back(makeNote(0.2 + i * 0.4, 0.5 + i * 0.4, 40.0f));

    GuideSource guide;
    guide.id = "measured";
    guide.notes = guideNotes;

    std::vector<GuideApplicationRegion> pair = {
        makeRegion("m2", "v2-stem", 16.555, 40.975, 16.555 - kOffset,
                   40.975 - kOffset, kOffset, &item2),
        makeRegion("m4", "v2-stem", 76.016413, 121.605056,
                   70.198229, 115.786872, kOffset, &item4),
    };

    const auto plan = planGuideApplication(guide, tempo120(), pair, kSampling);
    CHECK(plan.guideNotes == 111);

    const auto* m2 = findPlan(plan, "m2");
    CHECK(m2 != nullptr);
    CHECK(m2->edits.size() == 55); // medido: 55 dentro
    CHECK(m2->guideNotes == 55);
    CHECK(near(m2->edits.front().targetPitch, 60.0));
    CHECK(near(m2->edits.front().currentMidi, 57.0));
    CHECK(m2->edits.front().noteIndex == 0);
    CHECK(m2->edits.back().noteIndex == 54);

    // Item 4: 0 cambios (no aparece en el plan, no es un grupo vacio).
    CHECK(findPlan(plan, "m4") == nullptr);
    CHECK(!plan.appliesTo("m4"));
    CHECK(plan.changedNotes == 55);

    // 55 aplicadas + 53 fuera (no-item) + 3 dentro sin nota (no-match) = 111.
    CHECK(plan.appliedSlots == 55);
    CHECK(plan.skipped.size() == 56);
    CHECK(plan.skippedCount(GuideSkipReason::NoItem) == 53);
    CHECK(plan.skippedCount(GuideSkipReason::NoMatch) == 3);
    CHECK(plan.skippedCount(GuideSkipReason::NotAnalyzed) == 0);
    CHECK(plan.skippedCount(GuideSkipReason::DifferentSource) == 0);

    // Nothing was written.
    CHECK(near(item2.getNotes()[0].getMidiNote(), 57.0f));
    CHECK(near(item2.getNotes()[54].getMidiNote(), 57.0f));

    // The full track (5 items, 2 sources) keeps item 2 at 55 and item 4 at 0:
    // SCOPE=A adds item 3's own edits instead of stealing or dropping item 2's.
    for (const auto& note : guideNotes)
    {
        const double start = note.startQuarter / 2.0;
        if (start + 0.1 < 48.567 || start > 74.748)
            continue;
        item3.getNotes().push_back(makeNote(start - kOffset,
                                            note.endQuarter / 2.0 - kOffset,
                                            57.0f));
    }
    std::vector<GuideApplicationRegion> track = {
        makeRegion("m0", "v2-stem", 5.818184, 12.924, 0.0, 7.105816, kOffset,
                   &item0),
        makeRegion("m1", "Miu_stems_v2-001.wav", 12.914, 16.574, 0.0, 3.660,
                   12.914, &item1),
        makeRegion("m2", "v2-stem", 16.555, 40.975, 16.555 - kOffset,
                   40.975 - kOffset, kOffset, &item2),
        makeRegion("m3", "v2-stem", 48.567, 74.748, 42.748816, 68.929816,
                   kOffset, &item3),
        makeRegion("m4", "v2-stem", 76.016413, 121.605056, 70.198229,
                   115.786872, kOffset, &item4),
    };
    const auto trackPlan =
        planGuideApplication(guide, tempo120(), track, kSampling);
    const auto* trackM2 = findPlan(trackPlan, "m2");
    CHECK(trackM2 != nullptr);
    CHECK(trackM2->edits.size() == 55);
    CHECK(findPlan(trackPlan, "m4") == nullptr);
    CHECK(!trackPlan.appliesTo("m4"));
    const auto* trackM3 = findPlan(trackPlan, "m3");
    CHECK(trackM3 != nullptr);
    CHECK(!trackM3->edits.empty());
    CHECK(trackPlan.appliedSlots > plan.appliedSlots);
    CHECK(trackPlan.guideNotes == 111);
    // No note of item 2's project appears twice after adding the other items.
    for (size_t i = 1; i < trackM2->edits.size(); ++i)
        CHECK(trackM2->edits[i - 1].noteIndex < trackM2->edits[i].noteIndex);
}
} // namespace

int main()
{
    testSyntheticTrack();
    testMeasuredItem2AndItem4();

    std::printf("MidiGuideTrackPlanTests: all checks passed\n");
    return 0;
}
