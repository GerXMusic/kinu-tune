#pragma once

#include "../../Audio/Engine/HostTempoMap.h"

#include <vector>

/**
 * R3 (D-316): the grid and the ruler read the host tempo map through this
 * single pass, so both draw the same lines and the local channel can publish
 * them without a second implementation.
 *
 * Time contract: hostStart/hostEnd and the returned hostSeconds are HOST
 * timeline seconds (HostTempoMap's axis). The renderer turns them into project
 * time with CoordinateMapper::timelineToProject; the debug command reports
 * them as they are.
 */
enum class TempoGridLevel
{
    Subdivision, // gridQuarterNotes, the user's grid
    Beat,        // bar lines + beats of each signature, no subdivisions
    Bar          // bar lines only, possibly every 2nd/4th/... bar
};

struct TempoGridPlan
{
    TempoGridLevel level = TempoGridLevel::Beat;
    int barStride = 1;
};

struct TempoGridLine
{
    double hostSeconds = 0.0;
    double quarter = 0.0;
    int barIndex = 0;
    int barNumber = 1;
    int beatInBar = 0;
    HostTempoMap::GridLineKind kind = HostTempoMap::GridLineKind::Beat;
};

// Smallest bar length (in quarters) of the bars overlapping the range.
double minBarQuartersInRange(const HostTempoMap &map, double qStart, double qEnd);

// Smallest beat length (in quarters), so 3/4 and 7/8 thin correctly.
double minBeatQuartersInRange(const HostTempoMap &map, double qStart, double qEnd);

// Thinning by the FASTEST section of the range (minSecondsPerQuarter): the
// finest level whose lines keep at least minPixelsPerLine between them.
TempoGridPlan planTempoGrid(const HostTempoMap &map, double hostStart,
                            double hostEnd, double gridQuarterNotes,
                            double pixelsPerSecond, double minPixelsPerLine);

// Same, but never finer than bars (the ruler only labels bars).
TempoGridPlan planTempoGridBars(const HostTempoMap &map, double hostStart,
                                double hostEnd, double pixelsPerSecond,
                                double minPixelsPerLine);

// Lines of the plan inside the host window. An invalid map yields none, and
// the caller keeps its constant-BPM fallback.
std::vector<TempoGridLine> tempoGridLines(const HostTempoMap &map,
                                          double hostStart, double hostEnd,
                                          double gridQuarterNotes,
                                          const TempoGridPlan &plan);
