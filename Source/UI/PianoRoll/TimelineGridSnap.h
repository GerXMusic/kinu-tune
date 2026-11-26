// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../Audio/Engine/HostTempoMap.h"

/**
 * R4 (D-317): snap a PROJECT time to the host's musical grid.
 *
 * Project time is what the editor draws in; the musical grid lives in host
 * time. The path is project -> host (display offset) -> quarter -> round to a
 * whole number of gridQuarterNotes -> host -> project, all through the tempo
 * map, so a project with two tempos or a 3/4 bar snaps where the ruler draws
 * the lines instead of on a fixed number of seconds.
 *
 * The display offset may be negative (a region that windows the back half of a
 * take and sits near the start of the timeline), so nothing here is clamped;
 * the caller clamps to the visible start, as it already does.
 */
inline double snapProjectSecondsToHostGrid(const HostTempoMap &tempoMap,
                                           double timelineDisplayOffsetSeconds,
                                           double projectSeconds,
                                           double gridQuarterNotes)
{
  const double hostSeconds = projectSeconds + timelineDisplayOffsetSeconds;
  const double snappedHost =
      tempoMap.snapTimeToGrid(hostSeconds, gridQuarterNotes);
  return snappedHost - timelineDisplayOffsetSeconds;
}
