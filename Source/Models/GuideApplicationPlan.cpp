// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "GuideApplicationPlan.h"

#include <algorithm>
#include <cmath>

namespace
{
// Strict overlap: touching edges do not count (the matcher and R1 ask the same).
bool overlaps(double aStart, double aEnd, double bStart, double bEnd)
{
    return std::min(aEnd, bEnd) - std::max(aStart, bStart) > 0.0;
}

double projectSecondsForFrame(int frame, const MidiGuideSampling& sampling)
{
    if (sampling.sampleRate <= 0)
        return 0.0;
    return static_cast<double>(frame) * sampling.hopSize /
           static_cast<double>(sampling.sampleRate);
}

GuideModificationPlan *findModification(GuideApplicationPlan& plan,
                                        const juce::String& key)
{
    for (auto& modification : plan.modifications)
        if (modification.modificationKey == key)
            return &modification;
    return nullptr;
}

// One note of one modification cannot be targeted twice, however many sibling
// windows of that modification the guide crosses. Best score wins; a tie goes
// to the earlier guide slot, so the result does not depend on visit order.
void addEdit(GuideApplicationPlan& plan, const juce::String& modificationKey,
             const Project* project, const GuidePlannedEdit& edit)
{
    auto* modification = findModification(plan, modificationKey);
    if (modification == nullptr)
    {
        GuideModificationPlan created;
        created.modificationKey = modificationKey;
        created.project = project;
        plan.modifications.push_back(created);
        modification = &plan.modifications.back();
    }

    for (auto& existing : modification->edits)
    {
        if (existing.noteIndex != edit.noteIndex)
            continue;
        const double scoreDelta = edit.score - existing.score;
        const bool better = scoreDelta > 1.0e-9 ||
                            (std::abs(scoreDelta) <= 1.0e-9 &&
                             edit.guideIndex < existing.guideIndex);
        if (better)
            existing = edit;
        return;
    }
    modification->edits.push_back(edit);
}

// A slot resolves against one region: the same one-to-one matcher the
// single-item path uses, with the slot mapped through THAT region's offset.
bool matchRegion(const GuideApplicationRegion& region,
                 const MidiGuideMatcher::GuideSlot& slot,
                 const MidiGuideSampling& sampling, GuidePlannedEdit& edit)
{
    const double sourceStart = slot.startSeconds - region.offsetSeconds;
    const double sourceEnd = slot.endSeconds - region.offsetSeconds;

    const auto& notes = region.project->getNotes();
    std::vector<MidiGuideMatcher::AudioNote> audio;
    audio.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& note = notes[i];
        if (note.isRest() || note.isMuted())
            continue;
        MidiGuideMatcher::AudioNote entry;
        entry.index = static_cast<int>(i);
        entry.startSeconds =
            projectSecondsForFrame(note.getStartFrame(), sampling);
        entry.endSeconds = projectSecondsForFrame(note.getEndFrame(), sampling);
        entry.centerMidi = static_cast<double>(note.getAdjustedMidiNote());
        if (entry.endSeconds <= entry.startSeconds)
            continue;
        if (!overlaps(entry.startSeconds, entry.endSeconds, sourceStart,
                      sourceEnd))
            continue;
        audio.push_back(entry);
    }

    MidiGuideMatcher::GuideSlot local = slot;
    local.startSeconds = sourceStart;
    local.endSeconds = sourceEnd;
    const auto matches = MidiGuideMatcher::match(audio, {local});
    if (matches.empty())
        return false;

    const int noteIndex = matches.front().audioIndex;
    edit.guideIndex = slot.guideIndex;
    edit.noteIndex = noteIndex;
    edit.currentMidi =
        static_cast<double>(notes[static_cast<size_t>(noteIndex)].getMidiNote());
    edit.targetPitch = slot.pitch;
    edit.score = matches.front().score;
    return true;
}
} // namespace

const char* guideSkipReasonName(GuideSkipReason reason)
{
    switch (reason)
    {
    case GuideSkipReason::NoItem:
        return "no-item";
    case GuideSkipReason::NotAnalyzed:
        return "not-analyzed";
    case GuideSkipReason::DifferentSource:
        return "different-source";
    case GuideSkipReason::NoMatch:
        return "no-match";
    }
    return "no-item";
}

int GuideApplicationPlan::skippedCount(GuideSkipReason reason) const
{
    return static_cast<int>(
        std::count_if(skipped.begin(), skipped.end(),
                      [reason](const GuideSkippedSlot& entry) {
                          return entry.reason == reason;
                      }));
}

bool GuideApplicationPlan::appliesTo(const juce::String& modificationKey) const
{
    return std::any_of(
        modifications.begin(), modifications.end(),
        [&modificationKey](const GuideModificationPlan& modification) {
            return modification.modificationKey == modificationKey;
        });
}

GuideApplicationPlan
planGuideApplication(const GuideSource& guide, const HostTempoMap& tempo,
                     const std::vector<GuideApplicationRegion>& regions,
                     const MidiGuideSampling& sampling)
{
    GuideApplicationPlan plan;

    MidiGuideMapping mapping;
    mapping.tempoMap = &tempo;
    mapping.sampleRate = sampling.sampleRate;
    mapping.hopSize = sampling.hopSize;

    // Guide slots on the host timeline (the axis of the item windows).
    std::vector<MidiGuideMatcher::GuideSlot> slots;
    slots.reserve(guide.notes.size());
    for (size_t i = 0; i < guide.notes.size(); ++i)
    {
        const auto& note = guide.notes[i];
        MidiGuideMatcher::GuideSlot slot;
        slot.guideIndex = static_cast<int>(i);
        slot.startSeconds = mapping.timelineSecondsForQuarter(note.startQuarter);
        slot.endSeconds = mapping.timelineSecondsForQuarter(note.endQuarter);
        slot.pitch = static_cast<double>(note.pitch);
        if (slot.endSeconds <= slot.startSeconds)
            continue; // no duration: nothing a note could sound over
        slots.push_back(slot);
    }
    plan.guideNotes = static_cast<int>(slots.size());

    // Slot -> modification -> slots it takes, so the per-modification span and
    // count do not depend on the order regions are visited in.
    struct TakenSlot
    {
        int guideIndex = -1;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        juce::String modificationKey;
    };
    std::vector<TakenSlot> taken;

    for (const auto& slot : slots)
    {
        std::vector<size_t> overlapping;
        for (size_t r = 0; r < regions.size(); ++r)
            if (overlaps(slot.startSeconds, slot.endSeconds,
                         regions[r].hostStartSeconds,
                         regions[r].hostEndSeconds))
                overlapping.push_back(r);

        if (overlapping.empty())
        {
            plan.skipped.push_back(
                {slot.guideIndex, GuideSkipReason::NoItem, juce::String()});
            continue;
        }

        std::vector<juce::String> sources;
        bool analysed = false;
        bool applied = false;
        for (size_t r : overlapping)
        {
            const auto& region = regions[r];
            if (region.project == nullptr)
                continue; // an item that is on the track but not analysed
            analysed = true;

            if (std::find(sources.begin(), sources.end(), region.sourceKey) ==
                sources.end())
                sources.push_back(region.sourceKey);

            GuidePlannedEdit edit;
            if (!matchRegion(region, slot, sampling, edit))
                continue;

            addEdit(plan, region.modificationKey, region.project, edit);
            const bool alreadyTaken = std::any_of(
                taken.begin(), taken.end(),
                [&slot, &region](const TakenSlot& entry) {
                    return entry.guideIndex == slot.guideIndex &&
                           entry.modificationKey == region.modificationKey;
                });
            if (!alreadyTaken)
                taken.push_back({slot.guideIndex, slot.startSeconds,
                                 slot.endSeconds, region.modificationKey});
            applied = true;
        }

        // Sorted so the reported key does not depend on the order the caller
        // lists its regions in.
        std::sort(sources.begin(), sources.end());

        if (!analysed)
        {
            std::vector<juce::String> overlappingSources;
            for (size_t r : overlapping)
                if (std::find(overlappingSources.begin(),
                              overlappingSources.end(),
                              regions[r].sourceKey) ==
                    overlappingSources.end())
                    overlappingSources.push_back(regions[r].sourceKey);
            std::sort(overlappingSources.begin(), overlappingSources.end());
            plan.skipped.push_back({slot.guideIndex,
                                    GuideSkipReason::NotAnalyzed,
                                    overlappingSources.front()});
            continue;
        }
        if (applied)
        {
            ++plan.appliedSlots;
            continue;
        }

        // Nothing took the slot. Overlapping items of two or more sources and
        // no note that fits means the slot cannot be assigned to one source
        // (SCOPE=A still applies it independently when one source does match);
        // same-source items mean the audio simply did not fit.
        const auto reason = sources.size() >= 2
                                ? GuideSkipReason::DifferentSource
                                : GuideSkipReason::NoMatch;
        plan.skipped.push_back({slot.guideIndex, reason, sources.front()});
    }

    // Deterministic output independent of region order: keys sorted, edits by
    // note, skips by guide index.
    std::sort(plan.modifications.begin(), plan.modifications.end(),
              [](const GuideModificationPlan& a,
                 const GuideModificationPlan& b) {
                  return a.modificationKey < b.modificationKey;
              });

    for (auto& modification : plan.modifications)
    {
        std::sort(modification.edits.begin(), modification.edits.end(),
                  [](const GuidePlannedEdit& a, const GuidePlannedEdit& b) {
                      if (a.noteIndex != b.noteIndex)
                          return a.noteIndex < b.noteIndex;
                      return a.guideIndex < b.guideIndex;
                  });

        bool hasSpan = false;
        for (const auto& entry : taken)
        {
            if (entry.modificationKey != modification.modificationKey)
                continue;
            if (!hasSpan)
            {
                modification.hostStartSeconds = entry.startSeconds;
                modification.hostEndSeconds = entry.endSeconds;
                hasSpan = true;
            }
            modification.hostStartSeconds =
                std::min(modification.hostStartSeconds, entry.startSeconds);
            modification.hostEndSeconds =
                std::max(modification.hostEndSeconds, entry.endSeconds);
            ++modification.guideNotes;
        }
    }

    std::sort(plan.skipped.begin(), plan.skipped.end(),
              [](const GuideSkippedSlot& a, const GuideSkippedSlot& b) {
                  return a.guideIndex < b.guideIndex;
              });

    for (const auto& modification : plan.modifications)
    {
        plan.editedNotes += static_cast<int>(modification.edits.size());
        for (const auto& edit : modification.edits)
            if (std::abs(edit.targetPitch - edit.currentMidi) > 1.0e-4)
                ++plan.changedNotes;
    }

    return plan;
}
