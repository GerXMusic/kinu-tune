// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuideGhosts.h"

double midiGuideGhostProjectSeconds(double hostSeconds,
                                    double timelineDisplayOffsetSeconds)
{
    return hostSeconds - timelineDisplayOffsetSeconds;
}

double midiGuideGhostHostSeconds(double projectSeconds,
                                 double timelineDisplayOffsetSeconds)
{
    return projectSeconds + timelineDisplayOffsetSeconds;
}

void MidiGuideGhostLayer::bindProject(const Project *project)
{
    // Same document (an edit, a new tempo map, a region offset change): the
    // list still belongs to it, and the painter re-maps each frame.
    if (project == boundProject)
        return;

    // A different document (or none): its guides are not these, so drawing the
    // old list over the new item would be wrong even before the offset.
    boundProject = project;
    ghosts.clear();
}

void MidiGuideGhostLayer::set(std::vector<MidiGuideGhost> newGhosts)
{
    ghosts = std::move(newGhosts);
}

double MidiGuideGhostLayer::firstHostSeconds() const
{
    if (ghosts.empty())
        return 0.0;
    double first = ghosts.front().startHostSeconds;
    for (const auto &ghost : ghosts)
        first = std::min(first, ghost.startHostSeconds);
    return first;
}

double MidiGuideGhostLayer::lastHostSeconds() const
{
    if (ghosts.empty())
        return 0.0;
    double last = ghosts.front().endHostSeconds;
    for (const auto &ghost : ghosts)
        last = std::max(last, ghost.endHostSeconds);
    return last;
}

double MidiGuideGhostLayer::startProjectSeconds(
    std::size_t index, double timelineDisplayOffsetSeconds) const
{
    if (index >= ghosts.size())
        return 0.0;
    return midiGuideGhostProjectSeconds(ghosts[index].startHostSeconds,
                                        timelineDisplayOffsetSeconds);
}

double MidiGuideGhostLayer::endProjectSeconds(
    std::size_t index, double timelineDisplayOffsetSeconds) const
{
    if (index >= ghosts.size())
        return 0.0;
    return midiGuideGhostProjectSeconds(ghosts[index].endHostSeconds,
                                        timelineDisplayOffsetSeconds);
}
