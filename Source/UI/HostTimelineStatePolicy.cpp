// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "HostTimelineStatePolicy.h"

#include "../Models/Project.h"

bool shouldPersistHostCursorTempo(
    const std::shared_ptr<const HostTempoMap> &tempoMap)
{
    // No map or an unusable one: the constant BPM is all the editor has, so it
    // is what the project must remember.
    return tempoMap == nullptr || !tempoMap->isValid();
}

bool applyHostTimelineStateToProject(
    Project &project, const std::shared_ptr<const HostTempoMap> &tempoMap,
    double bpm, int numerator, int denominator)
{
    if (!shouldPersistHostCursorTempo(tempoMap))
        return false;

    project.setTimelineTempoBpm(bpm);
    project.setTimelineBeatSignature(numerator, denominator);
    return true;
}
