// R2 (guia MIDI): a ghost is stored in HOST seconds and the canvas maps it to
// project seconds while painting. Changing the active item only changes the
// display offset, so the ghost must not move relative to the audio; the same
// ghost list must also not survive a document change. Measured pair used as the
// fixture: guide Lead 13.704-73.962 s, active item host 76.016-121.605, source
// 70.198-115.787 (offset 5.818232 s), where nothing matched inside the item.
#include "TestAssert.h"

#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/UI/PianoRoll/CoordinateMapper.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
// Measured values (no re-measurement here, they are the fixture).
constexpr double kOffset = 5.818182;      // 76.016414 - 70.198232
constexpr double kGuideHostStart = 13.704374;
constexpr double kGuideHostEnd = 73.962000;
constexpr double kItemSourceStart = 70.198232;
constexpr double kItemSourceEnd = 115.786875;

bool near(double a, double b, double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

MidiGuideGhost makeGhost(double startHost, double endHost, float pitch = 60.0f)
{
    MidiGuideGhost ghost;
    ghost.startHostSeconds = startHost;
    ghost.endHostSeconds = endHost;
    ghost.pitch = pitch;
    ghost.label = "C4";
    return ghost;
}

// One ghost, two views: the same host instant sits on the same host time in
// both, but on different project seconds (and therefore different x).
void testSameHostSameInstantDifferentProjectPosition()
{
    CoordinateMapper sourceView; // offset 0: project == host
    CoordinateMapper trackView;  // active item: offset 5.818232 s
    trackView.setTimelineDisplayOffset(kOffset);

    const double host = kGuideHostStart;
    const double sourceProject = midiGuideGhostProjectSeconds(
        host, sourceView.getTimelineDisplayOffset());
    const double trackProject = midiGuideGhostProjectSeconds(
        host, trackView.getTimelineDisplayOffset());

    CHECK(near(sourceProject, host));
    CHECK(near(sourceProject - trackProject, kOffset));

    // What must not move: mapping the drawn position back gives the same host.
    CHECK(near(sourceView.projectToTimeline(sourceProject), host));
    CHECK(near(trackView.projectToTimeline(trackProject), host));

    // Same host instant maps to the same x in host space...
    sourceView.setPixelsPerSecond(100.0f);
    trackView.setPixelsPerSecond(100.0f);
    const double hostX = sourceView.timeToX(host);
    CHECK(near(hostX, trackView.timeToX(host)));

    // ...while the canvas x (project axis) differs by exactly the offset.
    // timeToX returns float, hence the looser tolerance.
    CHECK(near(sourceView.timeToX(sourceProject) -
                   trackView.timeToX(trackProject),
               kOffset * 100.0, 1.0e-3));

    // And the helper agrees with the mapper the painter uses.
    CHECK(near(sourceProject, sourceView.timelineToProject(host)));
    CHECK(near(trackProject, trackView.timelineToProject(host)));
}

// The list belongs to one document: re-binding the same project keeps it (an
// edit or an offset change repaints, it does not rebuild), a new one or none
// drops it.
void testDocumentChangeDropsTheList()
{
    Project project;
    MidiGuideGhostLayer layer;
    layer.bindProject(&project);
    layer.set({makeGhost(kGuideHostStart, kGuideHostStart + 0.4),
               makeGhost(kGuideHostStart + 0.5, kGuideHostStart + 0.9)});
    CHECK(layer.size() == 2);

    layer.bindProject(&project); // same document: keep
    CHECK(layer.size() == 2);

    Project other;
    layer.bindProject(&other); // different document: drop
    CHECK(layer.empty());

    layer.set({makeGhost(kGuideHostStart, kGuideHostStart + 0.4)});
    layer.bindProject(nullptr); // no document: drop
    CHECK(layer.empty());
}

// A new offset recalculates the canvas position and leaves the stored host
// seconds untouched, which is what makes item changes harmless.
void testOffsetChangeRecalculatesOnlyTheCanvas()
{
    MidiGuideGhostLayer layer;
    layer.set({makeGhost(kGuideHostStart, kGuideHostEnd)});

    const double zero = layer.startProjectSeconds(0, 0.0);
    const double moved = layer.startProjectSeconds(0, kOffset);
    CHECK(near(zero, kGuideHostStart));
    CHECK(near(zero - moved, kOffset));
    CHECK(near(layer.endProjectSeconds(0, 0.0) - kOffset,
               layer.endProjectSeconds(0, kOffset)));

    CHECK(near(layer.all().front().startHostSeconds, kGuideHostStart));
    CHECK(near(layer.firstHostSeconds(), kGuideHostStart));
    CHECK(near(layer.lastHostSeconds(), kGuideHostEnd));
}

// With the measured pair the guide (host seconds) ends before the active item
// begins, so its ghost list does not overlap the item window at all: the same
// arithmetic R1 measured (0 guide notes inside the active item).
void testMeasuredGuideDoesNotReachTheActiveItem()
{
    MidiGuideGhostLayer layer;
    layer.set({makeGhost(kGuideHostStart, kGuideHostEnd)});

    const double startProject = layer.startProjectSeconds(0, kOffset);
    const double endProject = layer.endProjectSeconds(0, kOffset);

    CHECK(near(startProject, kGuideHostStart - kOffset));
    CHECK(near(endProject, kGuideHostEnd - kOffset));
    CHECK(endProject < kItemSourceStart);
    CHECK(endProject <= kItemSourceEnd);
}
} // namespace

int main()
{
    testSameHostSameInstantDifferentProjectPosition();
    testDocumentChangeDropsTheList();
    testOffsetChangeRecalculatesOnlyTheCanvas();
    testMeasuredGuideDoesNotReachTheActiveItem();

    std::printf("MidiGuideGhostTests: all checks passed\n");
    return 0;
}
