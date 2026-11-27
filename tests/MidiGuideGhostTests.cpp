// R2 (guia MIDI): un fantasma se guarda en segundos HOST y el canvas lo pasa a
// segundos de proyecto mientras pinta. Cambiar de item solo cambia el offset de
// dibujo, asi que el fantasma no se mueve respecto al audio; y R5b (D-356) fija
// que la lista es del DOCUMENTO: cambiar de item ya no la borra. Fixture = el par
// medido: guia Lead 13.704-73.962 s, item activo host 76.016-121.605, fuente
// 70.198-115.787 (offset 5.818232 s), donde nada emparejaba dentro del item.
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

// The list belongs to the DOCUMENT (R5b, D-356): changing the active item must
// not drop it (R5 measured 111 -> 0 ghosts when switching from item 4 to 1).
// The layer knows no Project at all; only an explicit clear (a new document)
// empties it.
void testDocumentChangeKeepsTheList()
{
    MidiGuideGhostLayer layer;
    layer.set({makeGhost(kGuideHostStart, kGuideHostStart + 0.4),
               makeGhost(kGuideHostStart + 0.5, kGuideHostStart + 0.9)});
    CHECK(layer.size() == 2);

    // Another item only changes the offset the canvas publishes; the list is
    // untouched and still starts at the same host instant.
    CHECK(near(layer.startProjectSeconds(0, kOffset), kGuideHostStart - kOffset));
    CHECK(near(layer.all().front().startHostSeconds, kGuideHostStart));
    CHECK(layer.size() == 2);

    layer.clear(); // a new document
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
    testDocumentChangeKeepsTheList();
    testOffsetChangeRecalculatesOnlyTheCanvas();
    testMeasuredGuideDoesNotReachTheActiveItem();

    std::printf("MidiGuideGhostTests: all checks passed\n");
    return 0;
}
