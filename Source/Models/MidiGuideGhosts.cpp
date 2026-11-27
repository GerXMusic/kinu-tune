// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuideGhosts.h"

#include "../JuceHeader.h"

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

std::vector<MidiGuideGhost>
midiGuideGhostsFor(const GuideSource& guide, const HostTempoMap& tempoMap)
{
    std::vector<MidiGuideGhost> ghosts;
    if (guide.notes.empty())
        return ghosts;

    ghosts.reserve(guide.notes.size());
    for (const auto& note : guide.notes)
    {
        MidiGuideGhost ghost;
        // R2/D-313: host seconds, not project seconds. The canvas applies the
        // display offset while painting, so the list survives an item change.
        ghost.startHostSeconds = tempoMap.timeForQuarter(note.startQuarter);
        ghost.endHostSeconds = tempoMap.timeForQuarter(note.endQuarter);
        ghost.pitch = note.pitch;
        ghost.label =
            juce::MidiMessage::getMidiNoteName(
                juce::jlimit(0, 127,
                             static_cast<int>(std::lround(note.pitch))),
                true, true, 3)
                .toStdString();
        if (ghost.endHostSeconds <= ghost.startHostSeconds)
            continue;
        ghosts.push_back(ghost);
    }
    return ghosts;
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
