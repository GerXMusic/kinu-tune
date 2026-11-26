// R1 (guia MIDI): Apply only edits the active item. The guide is song-level, so
// the same guide may cover several items; this test pins the region window that
// makes the filter explicit, including the real measured pair of windows
// (source 70.198-115.787 s, offset 5.818 s, guide 13.704-73.962 s), where the
// guide does not reach inside the item at all.
#include "TestAssert.h"

#include "../Source/Models/MidiGuide.h"
#include "../Source/Models/MidiGuideRegionScope.h"
#include "../Source/Models/Project.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr int kSampleRate = 44100;
constexpr int kHopSize = 512;

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

GuideSource makeGuide(double firstSecond, double lastSecond, int notes,
                      float pitch)
{
    // Spread `notes` slots end to end across [firstSecond, lastSecond].
    GuideSource guide;
    guide.id = "test";
    guide.label = "Test";
    guide.kind = "file";
    const double step = (lastSecond - firstSecond) / static_cast<double>(notes);
    for (int i = 0; i < notes; ++i)
    {
        const double start = firstSecond + step * i;
        const double end = firstSecond + step * (i + 1);
        GuideNote note;
        // 120 BPM: one quarter is 0.5 s.
        note.startQuarter = start * 2.0;
        note.endQuarter = end * 2.0;
        note.pitch = pitch;
        guide.notes.push_back(note);
    }
    return guide;
}

// The real measured case: the guide stops before the item starts.
void testRealPairMatchesNothingInside()
{
    Project project;
    project.getAudioData().playbackRegionRanges = {{70.198232, 115.786875}};

    const double offset = 5.818182; // projectToTimeline(0)
    const auto windows = midiGuideScopeWindowsFor(project, offset);
    CHECK(windows.valid);
    CHECK(near(windows.sourceStartSeconds, 70.198232));
    CHECK(near(windows.sourceEndSeconds, 115.786875));
    CHECK(near(windows.hostStartSeconds(), 76.016414, 1.0e-3));
    CHECK(near(windows.hostEndSeconds(), 121.605057, 1.0e-3));

    // Note below the item (project 65 s -> timeline 70.818 s, inside the guide
    // span) and note inside the item (project 80 s, no guide reaches it).
    project.getNotes().push_back(makeNote(65.0, 66.0, 57.0f));
    project.getNotes().push_back(makeNote(80.0, 81.0, 57.0f));

    const auto guide = makeGuide(13.704374, 73.962000, 111, 60.0f);
    CHECK(guide.notes.size() == 111);

    const auto plan = planMidiGuideScope(project, guide, tempo120(), windows,
                                        kSampleRate, kHopSize);

    CHECK(plan.report.guideNotes == 111);
    CHECK(near(plan.report.guideHostStartSeconds, 13.704374, 1.0e-3));
    CHECK(near(plan.report.guideHostEndSeconds, 73.962000, 1.0e-3));
    CHECK(plan.report.guideNotesInRegion == 0);
    CHECK(plan.report.matchedInRegion == 0);
    CHECK(plan.report.changesInRegion == 0);
    CHECK(plan.changes.empty());
    // The note inside the item stays; the one below it is dropped.
    CHECK(plan.audio.size() == 1);
    CHECK(plan.audio[0].index == 1);
    // Without the window the old path matched the note below the item: that is
    // exactly the bug the window closes.
    CHECK(plan.report.matchedOutsideRegion >= 1);
    // Nothing in the project itself moves: the plan is read-only.
    CHECK(near(project.getNotes()[0].getMidiNote(), 57.0f));
    CHECK(near(project.getNotes()[1].getMidiNote(), 57.0f));
}

// A cut item: the same guide spans both halves, and only the half that is the
// active project changes. The active project carries the second half.
void testCutItemOnlyChangesWhatIsInside()
{
    Project firstHalf;
    Project secondHalf;
    firstHalf.getAudioData().playbackRegionRanges = {{0.0, 50.0}};
    secondHalf.getAudioData().playbackRegionRanges = {{50.0, 100.0}};

    // front() is the active region, so a project holding both ranges would pick
    // the first one; that is the documented rule and is asserted here so a
    // future change of rule cannot pass silently.
    Project bothHalves;
    bothHalves.getAudioData().playbackRegionRanges = {{0.0, 50.0}, {50.0, 100.0}};
    const auto bothWindows = midiGuideScopeWindowsFor(bothHalves, 0.0);
    CHECK(near(bothWindows.sourceStartSeconds, 0.0));
    CHECK(near(bothWindows.sourceEndSeconds, 50.0));

    firstHalf.getNotes().push_back(makeNote(25.0, 26.0, 57.0f));
    secondHalf.getNotes().push_back(makeNote(25.0, 26.0, 57.0f));
    secondHalf.getNotes().push_back(makeNote(75.0, 76.0, 57.0f));

    const auto guide = makeGuide(0.0, 100.0, 100, 60.0f); // 1 s slots

    const auto firstPlan = planMidiGuideScope(
        firstHalf, guide, tempo120(),
        midiGuideScopeWindowsFor(firstHalf, 0.0), kSampleRate, kHopSize);
    CHECK(firstPlan.report.matchedInRegion == 1);
    CHECK(firstPlan.changes.size() == 1);
    CHECK(firstPlan.changes[0].noteIndex == 0);

    const auto secondPlan = planMidiGuideScope(
        secondHalf, guide, tempo120(),
        midiGuideScopeWindowsFor(secondHalf, 0.0), kSampleRate, kHopSize);
    // The note at project 25 s is outside [50, 100]: one match, the one at 75 s.
    CHECK(secondPlan.report.matchedInRegion == 1);
    CHECK(secondPlan.changes.size() == 1);
    CHECK(secondPlan.changes[0].noteIndex == 1);
    CHECK(near(secondPlan.changes[0].targetPitch, 60.0));
    CHECK(secondPlan.report.changesInRegion == 1);

    // And a plan over one project never reads or writes the other one.
    CHECK(near(firstHalf.getNotes()[0].getMidiNote(), 57.0f));
    CHECK(near(secondHalf.getNotes()[0].getMidiNote(), 57.0f));
    CHECK(near(secondHalf.getNotes()[1].getMidiNote(), 57.0f));
    CHECK(firstHalf.getNotes().size() == 1);
    CHECK(secondHalf.getNotes().size() == 2);
    CHECK(near(firstHalf.getAudioData().playbackRegionRanges.front().first,
               0.0));
    CHECK(near(secondHalf.getAudioData().playbackRegionRanges.front().first,
               50.0));
}

// Outside ARA there is no region, so the window must not filter anything or
// Apply would stop working in the standalone editor.
void testNoRegionKeepsEverySlot()
{
    Project project; // playbackRegionRanges empty
    project.getNotes().push_back(makeNote(10.0, 11.0, 57.0f));

    const auto windows = midiGuideScopeWindowsFor(project, 0.0);
    CHECK(!windows.valid);

    const auto guide = makeGuide(0.0, 20.0, 20, 60.0f);
    const auto plan = planMidiGuideScope(project, guide, tempo120(), windows,
                                        kSampleRate, kHopSize);
    CHECK(plan.slots.size() == guide.notes.size());
    CHECK(plan.audio.size() == 1);
    CHECK(plan.report.matchedInRegion == 1);
    CHECK(plan.report.matchedOutsideRegion == 0);
}

// A guide note that only clips the edge of the item still counts as inside.
void testEdgeOverlapCountsAsInside()
{
    Project project;
    project.getAudioData().playbackRegionRanges = {{50.0, 100.0}};
    project.getNotes().push_back(makeNote(75.0, 76.0, 57.0f));

    GuideSource guide;
    GuideNote note;
    note.startQuarter = 98.0; // 49 s
    note.endQuarter = 152.0;  // 76 s: crosses the item start
    note.pitch = 60.0f;
    guide.notes.push_back(note);

    const auto plan = planMidiGuideScope(
        project, guide, tempo120(), midiGuideScopeWindowsFor(project, 0.0),
        kSampleRate, kHopSize);
    CHECK(plan.report.guideNotesInRegion == 1);
    CHECK(plan.report.matchedInRegion == 1);
}
} // namespace

int main()
{
    testRealPairMatchesNothingInside();
    testCutItemOnlyChangesWhatIsInside();
    testNoRegionKeepsEverySlot();
    testEdgeOverlapCountsAsInside();

    std::printf("MidiGuideRegionScopeTests: all checks passed\n");
    return 0;
}
