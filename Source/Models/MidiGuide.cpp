// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuide.h"

namespace
{
struct Candidate
{
    int audioIndex = -1;
    int guideIndex = -1;
    double score = 0.0;
    double pitchDistance = 0.0;
    double guideStart = 0.0;
    double audioStart = 0.0;
};

// score desc, then |Δpitch| asc, then earlier guide, then earlier audio: the
// same total order the roadmap asks for, so matching is reproducible.
bool better(const Candidate& a, const Candidate& b)
{
    if (a.score != b.score)
        return a.score > b.score;
    if (a.pitchDistance != b.pitchDistance)
        return a.pitchDistance < b.pitchDistance;
    if (a.guideStart != b.guideStart)
        return a.guideStart < b.guideStart;
    if (a.audioStart != b.audioStart)
        return a.audioStart < b.audioStart;
    return a.audioIndex < b.audioIndex;
}
} // namespace

std::vector<MidiGuideMatcher::Match>
MidiGuideMatcher::match(const std::vector<AudioNote>& audioNotes,
                        const std::vector<GuideSlot>& guideSlots,
                        double maxPitchDistance, double minScore)
{
    std::vector<Candidate> candidates;
    candidates.reserve(audioNotes.size() * 2);

    for (const auto& audio : audioNotes)
    {
        const double audioDuration = audio.endSeconds - audio.startSeconds;
        if (audioDuration <= 0.0)
            continue;

        for (const auto& guide : guideSlots)
        {
            const double guideDuration = guide.endSeconds - guide.startSeconds;
            if (guideDuration <= 0.0)
                continue;

            const double overlap = std::min(audio.endSeconds, guide.endSeconds) -
                                   std::max(audio.startSeconds, guide.startSeconds);
            if (overlap <= 0.0)
                continue;

            const double pitchDistance =
                std::abs(guide.pitch - audio.centerMidi);
            if (pitchDistance > maxPitchDistance)
                continue;

            const double shortest = std::min(audioDuration, guideDuration);
            if (shortest <= 0.0)
                continue;

            const double score = overlap / shortest - 0.05 * pitchDistance;
            if (score < minScore)
                continue;

            Candidate candidate;
            candidate.audioIndex = audio.index;
            candidate.guideIndex = guide.guideIndex;
            candidate.score = score;
            candidate.pitchDistance = pitchDistance;
            candidate.guideStart = guide.startSeconds;
            candidate.audioStart = audio.startSeconds;
            candidates.push_back(candidate);
        }
    }

    std::sort(candidates.begin(), candidates.end(), better);

    std::vector<Match> matches;
    std::vector<bool> guideUsed;

    for (const auto& slot : guideSlots)
        if (slot.guideIndex >= 0 &&
            static_cast<size_t>(slot.guideIndex) >= guideUsed.size())
            guideUsed.resize(static_cast<size_t>(slot.guideIndex) + 1, false);

    for (const auto& candidate : candidates)
    {
        const bool audioAlreadyMatched =
            std::any_of(matches.begin(), matches.end(),
                        [&candidate](const Match& m) {
                            return m.audioIndex == candidate.audioIndex;
                        });
        if (audioAlreadyMatched)
            continue;
        if (candidate.guideIndex < 0 ||
            static_cast<size_t>(candidate.guideIndex) >= guideUsed.size())
            continue;
        if (guideUsed[static_cast<size_t>(candidate.guideIndex)])
            continue;

        guideUsed[static_cast<size_t>(candidate.guideIndex)] = true;
        matches.push_back(
            {candidate.audioIndex, candidate.guideIndex, candidate.score});
    }

    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                  return a.audioIndex < b.audioIndex;
              });
    return matches;
}
