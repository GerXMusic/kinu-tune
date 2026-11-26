// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include <memory>

#include "../Audio/Engine/HostTempoMap.h"

class Project;

/**
 * R4 (D-317): what the host timeline state is allowed to persist.
 *
 * With a real host tempo map the cursor BPM and the time signature are display
 * only: the map is the truth and the project keeps whatever constant fallback
 * it was saved with. Storing the cursor value froze the BPM of wherever the
 * cursor happened to be when the project was last touched, and reopening the
 * project then showed that number as its tempo.
 *
 * Without a usable map the cursor values ARE the fallback, so they are stored
 * exactly as before.
 */
bool shouldPersistHostCursorTempo(
    const std::shared_ptr<const HostTempoMap> &tempoMap);

// Applies the host timeline state to the project under the rule above. Returns
// true when it wrote to the project.
bool applyHostTimelineStateToProject(
    Project &project, const std::shared_ptr<const HostTempoMap> &tempoMap,
    double bpm, int numerator, int denominator);
