// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

class Project;

/**
 * Guide ghosts (R2 of the guide-integration fix, D-313): one guide note as the
 * piano roll draws it.
 *
 * The guide is song-level and lives on the HOST timeline, so a ghost stores
 * host seconds. The canvas draws in project seconds, so the painter applies the
 * display offset (host -> project, the same one CoordinateMapper publishes) on
 * every frame.
 *
 * Axes, kept explicit:
 *   ghost      : host seconds (timeline), never project seconds
 *   canvas     : project seconds; project = host - timelineDisplayOffset
 *
 * Storing project seconds (the previous behaviour) baked in the offset of
 * whichever item was active when the list was built: moving to another item
 * (offset 5.818 s in the measured track) left the ghosts that far off the guide
 * and they were never rebuilt.
 */
struct MidiGuideGhost
{
    double startHostSeconds = 0.0;
    double endHostSeconds = 0.0;
    float pitch = 60.0f;
    std::string label;
};

/** Canvas axis (project seconds) for a host instant. */
double midiGuideGhostProjectSeconds(double hostSeconds,
                                    double timelineDisplayOffsetSeconds);

/** Host instant for a project instant (the inverse, for the probes). */
double midiGuideGhostHostSeconds(double projectSeconds,
                                 double timelineDisplayOffsetSeconds);

/**
 * The ghost list a piano roll shows. One document at a time: a new project
 * drops the previous list (its guide list is not this one's), while the same
 * project (an edit, an offset change) keeps it because the painter re-maps.
 */
class MidiGuideGhostLayer
{
public:
    void bindProject(const Project *project);
    void clear() { ghosts.clear(); }
    void set(std::vector<MidiGuideGhost> newGhosts);

    bool empty() const { return ghosts.empty(); }
    std::size_t size() const { return ghosts.size(); }
    const std::vector<MidiGuideGhost> &all() const { return ghosts; }

    // Host span of the list (0/0 when empty), published by guide_debug.
    double firstHostSeconds() const;
    double lastHostSeconds() const;

    // One ghost on canvas time with the offset the mapper publishes now.
    double startProjectSeconds(std::size_t index,
                               double timelineDisplayOffsetSeconds) const;
    double endProjectSeconds(std::size_t index,
                             double timelineDisplayOffsetSeconds) const;

private:
    std::vector<MidiGuideGhost> ghosts;
    const Project *boundProject = nullptr;
};
