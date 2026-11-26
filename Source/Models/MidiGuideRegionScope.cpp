// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuideRegionScope.h"

#include <algorithm>

namespace
{
// Strict overlap: touching edges do not count (the matcher asks the same).
bool overlaps(double aStart, double aEnd, double bStart, double bEnd)
{
    return std::min(aEnd, bEnd) - std::max(aStart, bStart) > 0.0;
}
} // namespace

MidiGuideScopeWindows midiGuideScopeWindowsFor(const Project& project,
                                               double offsetSeconds)
{
    MidiGuideScopeWindows windows;
    windows.offsetSeconds = offsetSeconds;

    const auto& ranges = project.getAudioData().playbackRegionRanges;
    if (!ranges.empty())
    {
        windows.sourceStartSeconds = ranges.front().first;
        windows.sourceEndSeconds = ranges.front().second;
        windows.valid = windows.sourceEndSeconds > windows.sourceStartSeconds;
    }
    return windows;
}

MidiGuideScopedPlan
planMidiGuideScope(const Project& project, const GuideSource& guide,
                   const HostTempoMap& tempo,
                   const MidiGuideScopeWindows& windows, int sampleRate,
                   int hopSize)
{
    MidiGuideScopedPlan plan;
    plan.report.windows = windows;
    plan.report.guideNotes = static_cast<int>(guide.notes.size());

    MidiGuideMapping mapping;
    mapping.tempoMap = &tempo;
    mapping.timelineDisplayOffsetSeconds = windows.offsetSeconds;
    mapping.sampleRate = sampleRate;
    mapping.hopSize = hopSize;

    // Every guide slot, on the timeline (the matcher never sees quarters).
    std::vector<MidiGuideMatcher::GuideSlot> allSlots;
    allSlots.reserve(guide.notes.size());
    for (size_t i = 0; i < guide.notes.size(); ++i)
    {
        const auto& note = guide.notes[i];
        MidiGuideMatcher::GuideSlot slot;
        slot.guideIndex = static_cast<int>(i);
        slot.startSeconds = mapping.timelineSecondsForQuarter(note.startQuarter);
        slot.endSeconds = mapping.timelineSecondsForQuarter(note.endQuarter);
        slot.pitch = static_cast<double>(note.pitch);
        if (slot.endSeconds <= slot.startSeconds)
            continue;
        allSlots.push_back(slot);
    }

    if (!allSlots.empty())
    {
        plan.report.hasGuideSpan = true;
        plan.report.guideHostStartSeconds = allSlots.front().startSeconds;
        plan.report.guideHostEndSeconds = allSlots.front().endSeconds;
        for (const auto& slot : allSlots)
        {
            plan.report.guideHostStartSeconds =
                std::min(plan.report.guideHostStartSeconds, slot.startSeconds);
            plan.report.guideHostEndSeconds =
                std::max(plan.report.guideHostEndSeconds, slot.endSeconds);
        }
    }

    // Every audio note on the same axis; muted and rest notes are skipped, as
    // applying a guide to something that does not sound would be invisible.
    const auto& notes = project.getNotes();
    std::vector<MidiGuideMatcher::AudioNote> allAudio;
    allAudio.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& note = notes[i];
        if (note.isRest() || note.isMuted())
            continue;
        MidiGuideMatcher::AudioNote entry;
        entry.index = static_cast<int>(i);
        entry.startSeconds =
            static_cast<double>(note.getStartFrame()) * hopSize / sampleRate +
            windows.offsetSeconds;
        entry.endSeconds =
            static_cast<double>(note.getEndFrame()) * hopSize / sampleRate +
            windows.offsetSeconds;
        entry.centerMidi = static_cast<double>(note.getAdjustedMidiNote());
        if (entry.endSeconds <= entry.startSeconds)
            continue;
        allAudio.push_back(entry);
    }

    const double hostStart = windows.hostStartSeconds();
    const double hostEnd = windows.hostEndSeconds();

    // Slots outside the active item are dropped: a guide for another item must
    // not edit this one. Without ARA windows nothing is filtered.
    plan.slots.reserve(allSlots.size());
    for (const auto& slot : allSlots)
        if (!windows.valid ||
            overlaps(slot.startSeconds, slot.endSeconds, hostStart, hostEnd))
            plan.slots.push_back(slot);

    // Audio notes on the timeline are compared against the host window, which
    // is the source window shifted by the same offset (the two are equivalent;
    // the source numbers are the ones the debug report publishes).
    plan.audio.reserve(allAudio.size());
    for (const auto& entry : allAudio)
        if (!windows.valid ||
            overlaps(entry.startSeconds, entry.endSeconds, hostStart, hostEnd))
            plan.audio.push_back(entry);

    plan.report.guideNotesInRegion = static_cast<int>(plan.slots.size());

    const auto matches = MidiGuideMatcher::match(plan.audio, plan.slots);

    // The matcher hands back the entry's own index (the note index) and the
    // slot's own guide index, not positions, so both are looked up by id.
    const auto slotFor = [&plan](int guideIndex)
        -> const MidiGuideMatcher::GuideSlot *
    {
        for (const auto &slot : plan.slots)
            if (slot.guideIndex == guideIndex)
                return &slot;
        return nullptr;
    };
    const auto audioFor = [&plan](int noteIndex)
        -> const MidiGuideMatcher::AudioNote *
    {
        for (const auto &entry : plan.audio)
            if (entry.index == noteIndex)
                return &entry;
        return nullptr;
    };

    plan.changes.reserve(matches.size());
    for (const auto &match : matches)
    {
        const auto *slot = slotFor(match.guideIndex);
        const auto *entry = audioFor(match.audioIndex);
        if (slot == nullptr || entry == nullptr)
            continue;
        plan.changes.push_back({match.audioIndex, slot->pitch});
        if (std::abs(slot->pitch - entry->centerMidi) > 1.0e-4)
            ++plan.report.changesInRegion;
    }
    plan.report.matchedInRegion = static_cast<int>(plan.changes.size());

    // What the old, unscoped path would have matched: reported only, never
    // applied. It is the number that shows the bug from the outside.
    if (windows.valid)
    {
        const auto fullMatches = MidiGuideMatcher::match(allAudio, allSlots);
        for (const auto &match : fullMatches)
        {
            const MidiGuideMatcher::GuideSlot *slot = nullptr;
            for (const auto &candidate : allSlots)
                if (candidate.guideIndex == match.guideIndex)
                {
                    slot = &candidate;
                    break;
                }
            const MidiGuideMatcher::AudioNote *entry = nullptr;
            for (const auto &candidate : allAudio)
                if (candidate.index == match.audioIndex)
                {
                    entry = &candidate;
                    break;
                }
            if (slot == nullptr || entry == nullptr)
                continue;
            const bool slotInside =
                overlaps(slot->startSeconds, slot->endSeconds, hostStart,
                         hostEnd);
            const bool noteInside =
                overlaps(entry->startSeconds, entry->endSeconds, hostStart,
                         hostEnd);
            if (!slotInside || !noteInside)
                ++plan.report.matchedOutsideRegion;
        }
    }

    return plan;
}
