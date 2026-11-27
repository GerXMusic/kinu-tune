// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "DocumentNoteBatch.h"

#include "../Undo/TimingAction.h"
#include "../Utils/Constants.h"
#include "../Utils/NotePitch.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/TimingQuantize.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace documentNoteBatch
{
namespace
{
struct Group
{
    Project* project = nullptr;
    std::vector<Target> entries;
};

// Groups in the order the targets came in, which targetsOf() already sorted by
// (region, note index). Each item gets one group however many of its windows
// the selection came from.
std::vector<Group> groupsOf(const std::vector<Target>& targets)
{
    std::vector<Group> groups;
    for (const auto& target : targets)
    {
        if (target.project == nullptr || target.note == nullptr)
            continue;
        auto found = std::find_if(
            groups.begin(), groups.end(),
            [&target](const Group& group) { return group.project == target.project; });
        if (found == groups.end())
        {
            groups.push_back(Group{target.project, {}});
            found = groups.end() - 1;
        }
        found->entries.push_back(target);
    }
    return groups;
}

void markSynthAndRange(const std::vector<Note*>& notes, int& dirtyStart,
                       int& dirtyEnd)
{
    for (auto* note : notes)
    {
        if (note == nullptr)
            continue;
        note->markDirty();
        note->markSynthDirty();
        dirtyStart = std::min(dirtyStart, note->getStartFrame());
        dirtyEnd = std::max(dirtyEnd, note->getEndFrame());
    }
}

void clampF0Range(Project& project, int dirtyStart, int dirtyEnd)
{
    if (dirtyEnd <= dirtyStart)
        return;
    const int frameCount = project.getAudioData().getNumFrames();
    project.setF0DirtyRange(std::max(0, dirtyStart - 24),
                            std::min(frameCount, dirtyEnd + 24));
}
} // namespace

std::vector<Target> targetsOf(const DocumentNoteSelection& selection,
                              const DocumentNoteIndex& index)
{
    struct Candidate
    {
        Target target;
        int regionOrder = 0;
        int noteOrder = 0;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(selection.size()));
    for (const auto& entry : selection.entries())
    {
        if (entry.project == nullptr || entry.note == nullptr)
            continue;

        // Region order and phase come from the published index, not from the
        // set: the index is what the drawing and the windows agree on.
        int regionOrder = -1;
        double offsetSeconds = 0.0;
        for (int i = 0; i < index.regionCount(); ++i)
        {
            const auto& region = index.regions[static_cast<size_t>(i)];
            if (region.key != entry.regionKey)
                continue;
            regionOrder = i;
            offsetSeconds = region.offsetSeconds;
            break;
        }
        if (regionOrder < 0)
            continue; // the window is gone: not a target anymore

        // Index of the note inside its own project: the tiebreak of the order
        // and the proof that the pointer still is one of its notes.
        const auto& notes = entry.project->getNotes();
        int noteOrder = -1;
        for (int i = 0; i < static_cast<int>(notes.size()); ++i)
            if (&notes[static_cast<size_t>(i)] == entry.note)
            {
                noteOrder = i;
                break;
            }
        if (noteOrder < 0)
            continue;

        candidates.push_back(
            {Target{entry.project, entry.note, entry.regionKey, offsetSeconds},
             regionOrder, noteOrder});
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b)
                     {
                         if (a.regionOrder != b.regionOrder)
                             return a.regionOrder < b.regionOrder;
                         return a.noteOrder < b.noteOrder;
                     });

    std::vector<Target> targets;
    targets.reserve(candidates.size());
    std::vector<std::pair<Project*, Note*>> seen;
    for (const auto& candidate : candidates)
    {
        const auto pair =
            std::make_pair(candidate.target.project, candidate.target.note);
        if (std::find(seen.begin(), seen.end(), pair) != seen.end())
            continue;
        seen.push_back(pair);
        targets.push_back(candidate.target);
    }
    return targets;
}

Result shiftPitch(
    const std::vector<Target>& targets,
    const std::function<std::optional<float>(const Project&, const Note&)>&
        semitonesFor)
{
    Result result;
    const auto groups = groupsOf(targets);
    if (groups.empty())
        return result;

    struct Change
    {
        Note* note = nullptr;
        float midi = 0.0f;
        float source = 0.0f;
    };
    struct Plan
    {
        Project* project = nullptr;
        std::vector<Change> changes;
    };

    // Plan first: one note that cannot move does not cancel the others, but
    // nothing is written until the whole selection has been worked out.
    std::vector<Plan> plans;
    constexpr float minMidi = static_cast<float>(MIN_MIDI_NOTE);
    constexpr float maxMidi = static_cast<float>(MAX_MIDI_NOTE);
    for (const auto& group : groups)
    {
        Plan plan{group.project, {}};
        for (const auto& target : group.entries)
        {
            if (target.note == nullptr)
                continue;
            const auto semitones = semitonesFor(*group.project, *target.note);
            if (!semitones)
                continue;
            const auto edit = NotePitch::editTarget(*group.project, *target.note,
                                                    *semitones, minMidi, maxMidi);
            if (!edit)
                continue;
            plan.changes.push_back(
                Change{target.note, edit->midi, edit->source});
        }
        if (!plan.changes.empty())
            plans.push_back(std::move(plan));
    }
    if (plans.empty())
        return result;

    for (auto& plan : plans)
    {
        std::vector<Note*> notes;
        notes.reserve(plan.changes.size());
        int dirtyStart = std::numeric_limits<int>::max();
        int dirtyEnd = std::numeric_limits<int>::min();
        for (const auto& change : plan.changes)
        {
            NotePitch::restore(*change.note, change.midi, change.source);
            notes.push_back(change.note);
        }
        markSynthAndRange(notes, dirtyStart, dirtyEnd);
        // The same rebuild the single-item nudge does after it writes.
        PitchCurveProcessor::rebuildBaseFromNotes(*plan.project);
        plan.project->markNoteEditDirtyRange(dirtyStart, dirtyEnd);
        plan.project->setModified(true);
        result.projects.push_back(plan.project);
        result.notes += static_cast<int>(plan.changes.size());
    }
    result.status = Result::Status::Applied;
    return result;
}

Result setTilt(const std::vector<Target>& targets, TiltLayer layer, float value)
{
    Result result;
    const auto groups = groupsOf(targets);
    if (groups.empty())
        return result;

    struct Plan
    {
        Project* project = nullptr;
        std::vector<Note*> notes;
        int dirtyStart = std::numeric_limits<int>::max();
        int dirtyEnd = std::numeric_limits<int>::min();
    };
    std::vector<Plan> plans;
    for (const auto& group : groups)
    {
        Plan plan{group.project, {}, std::numeric_limits<int>::max(),
                       std::numeric_limits<int>::min()};
        for (const auto& target : group.entries)
        {
            auto* note = target.note;
            if (note == nullptr || note->isRest())
                continue;
            const float current = layer == TiltLayer::General ? note->getTilt()
                                  : layer == TiltLayer::FineLeft
                                      ? note->getTiltFineLeft()
                                      : note->getTiltFineRight();
            if (juce::approximatelyEqual(current, value))
                continue; // already there: a second pass changes nothing
            if (layer == TiltLayer::General)
                note->setTilt(value);
            else if (layer == TiltLayer::FineLeft)
                note->setTiltFineLeft(value);
            else
                note->setTiltFineRight(value);
            plan.notes.push_back(note);
            plan.dirtyStart = std::min(plan.dirtyStart, note->getStartFrame());
            plan.dirtyEnd = std::max(plan.dirtyEnd, note->getEndFrame());
        }
        if (!plan.notes.empty())
            plans.push_back(std::move(plan));
    }
    if (plans.empty())
        return result;

    for (auto& plan : plans)
    {
        for (auto* note : plan.notes)
        {
            note->markDirty();
            note->markSynthDirty();
        }
        // D-308: the three tilt layers live in the delta curve, so the delta is
        // what is rebuilt (the body is the centre and does not move).
        PitchCurveProcessor::rebuildDeltaForNotes(*plan.project, plan.notes);
        plan.project->markNoteEditDirtyRange(plan.dirtyStart, plan.dirtyEnd);
        plan.project->setModified(true);
        result.projects.push_back(plan.project);
        result.notes += static_cast<int>(plan.notes.size());
    }
    result.status = Result::Status::Applied;
    return result;
}

Result setMuted(const std::vector<Target>& targets, bool muted)
{
    Result result;
    const auto groups = groupsOf(targets);
    if (groups.empty())
        return result;

    struct Plan
    {
        Project* project = nullptr;
        std::vector<Note*> notes;
        int dirtyStart = std::numeric_limits<int>::max();
        int dirtyEnd = std::numeric_limits<int>::min();
    };
    std::vector<Plan> plans;
    for (const auto& group : groups)
    {
        Plan plan{group.project, {}, std::numeric_limits<int>::max(),
                       std::numeric_limits<int>::min()};
        for (const auto& target : group.entries)
        {
            auto* note = target.note;
            if (note == nullptr || note->isMuted() == muted)
                continue;
            note->setMuted(muted);
            plan.notes.push_back(note);
            plan.dirtyStart = std::min(plan.dirtyStart, note->getStartFrame());
            plan.dirtyEnd = std::max(plan.dirtyEnd, note->getEndFrame());
        }
        if (!plan.notes.empty())
            plans.push_back(std::move(plan));
    }
    if (plans.empty())
        return result;

    for (auto& plan : plans)
    {
        int dirtyStart = plan.dirtyStart;
        int dirtyEnd = plan.dirtyEnd;
        markSynthAndRange(plan.notes, dirtyStart, dirtyEnd);
        clampF0Range(*plan.project, dirtyStart, dirtyEnd);
        plan.project->setModified(true);
        result.projects.push_back(plan.project);
        result.notes += static_cast<int>(plan.notes.size());
    }
    result.status = Result::Status::Applied;
    return result;
}

Result deleteNotes(const std::vector<Target>& targets)
{
    Result result;
    const auto groups = groupsOf(targets);
    if (groups.empty())
        return result;

    struct Plan
    {
        Project* project = nullptr;
        std::vector<Note*> notes;
        int dirtyStart = std::numeric_limits<int>::max();
        int dirtyEnd = std::numeric_limits<int>::min();
    };
    std::vector<Plan> plans;
    for (const auto& group : groups)
    {
        Plan plan{group.project, {}, std::numeric_limits<int>::max(),
                       std::numeric_limits<int>::min()};
        for (const auto& target : group.entries)
        {
            auto* note = target.note;
            // A rest has no audio to silence, and a tombstone is already gone.
            if (note == nullptr || note->isRest() || note->isDeleted())
                continue;
            note->setDeleted(true);
            note->setRest(true);
            note->setSelected(false);
            plan.notes.push_back(note);
            plan.dirtyStart = std::min(plan.dirtyStart, note->getStartFrame());
            plan.dirtyEnd = std::max(plan.dirtyEnd, note->getEndFrame());
        }
        if (!plan.notes.empty())
            plans.push_back(std::move(plan));
    }
    if (plans.empty())
        return result;

    for (auto& plan : plans)
    {
        int dirtyStart = plan.dirtyStart;
        int dirtyEnd = plan.dirtyEnd;
        markSynthAndRange(plan.notes, dirtyStart, dirtyEnd);
        clampF0Range(*plan.project, dirtyStart, dirtyEnd);
        plan.project->setModified(true);
        result.projects.push_back(plan.project);
        result.notes += static_cast<int>(plan.notes.size());
    }
    result.status = Result::Status::Applied;
    return result;
}

Result quantize(const std::vector<Target>& targets, double gridSeconds,
                int strengthPercent)
{
    Result result;
    if (gridSeconds <= 1.0e-9 || strengthPercent <= 0)
        return result;
    const auto groups = groupsOf(targets);
    if (groups.empty())
        return result;

    const double framesPerSecond =
        static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
    const double gridFrames = gridSeconds * framesPerSecond;

    struct Plan
    {
        Project* project = nullptr;
        std::vector<NoteTimingState> after;
    };
    std::vector<Plan> plans;
    int plannedNotes = 0;

    for (const auto& group : groups)
    {
        // One grid for the track: the phase of an item comes from the host
        // offset of its windows. Two sibling windows of the same item that
        // disagree (a stretched clip) have no single grid, and the batch is
        // refused instead of quantizing the item twice with different phases.
        const double phaseSeconds = group.entries.front().offsetSeconds;
        for (const auto& entry : group.entries)
            if (std::abs(entry.offsetSeconds - phaseSeconds) > 1.0e-9)
            {
                result.status = Result::Status::Rejected;
                result.reason = "ambiguous-grid";
                return result;
            }
        const int phaseFrames =
            static_cast<int>(std::llround(phaseSeconds * framesPerSecond));

        auto& notes = group.project->getNotes();
        std::unordered_set<const Note*> selected;        selected.reserve(group.entries.size());
        for (const auto& entry : group.entries)
            selected.insert(entry.note);

        // Same span convention as the single-item macro: every note of the
        // item takes part (a shared boundary moves once), rests included as
        // unselected neighbours.
        std::vector<timingQuantize::Span> spans;
        spans.reserve(notes.size());
        for (const auto& note : notes)
            spans.push_back({note.getStartFrame() - phaseFrames,
                             note.getEndFrame() - phaseFrames,
                             !note.isRest() && selected.count(&note) > 0});

        std::vector<timingQuantize::Span> quantized;
        if (!timingQuantize::quantizeSpans(spans, gridFrames, strengthPercent,
                                           quantized))
        {
            result.status = Result::Status::Rejected;
            result.reason = "unsafe-grid";
            return result;
        }

        Plan plan{group.project, {}};
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const int startFrame = quantized[i].startFrame + phaseFrames;
            const int endFrame = quantized[i].endFrame + phaseFrames;
            if (startFrame == notes[i].getStartFrame() &&
                endFrame == notes[i].getEndFrame())
                continue;
            plan.after.push_back({&notes[i], startFrame, endFrame});
        }
        if (!plan.after.empty())
        {
            plannedNotes += static_cast<int>(plan.after.size());
            plans.push_back(std::move(plan));
        }
    }

    if (plans.empty())
        return result;

    // Every item's plan is valid: from here on nothing can reject the batch.
    for (auto& plan : plans)
    {
        applyNoteTimingStates(*plan.project, plan.after);
        plan.project->setModified(true);
        result.projects.push_back(plan.project);
    }
    result.notes = plannedNotes;
    result.status = Result::Status::Applied;
    return result;
}
} // namespace documentNoteBatch
