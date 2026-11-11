#pragma once

#include <vector>

namespace pitchnet
{
// The target the editor should activate for one host selection.
//
// The selection lists are ordered by importance, most relevant first, as the
// ARA host delivers them. The three inputs carry, for each selected playback
// region / effective playback region / region sequence, the index of its
// region sequence within the editor's selected-sequence list. -1 means the
// object does not belong to any selected sequence.
enum class AraSelectionSource
{
    none,
    playbackRegion,
    effectiveRegion,
    regionSequence
};

struct AraSelectionTarget
{
    AraSelectionSource source = AraSelectionSource::none;
    int index = -1;
};

inline AraSelectionTarget chooseAraSelectionTarget(
    const std::vector<int>& playbackRegionSequence,
    const std::vector<int>& effectiveRegionSequence,
    const std::vector<int>& selectedSequence)
{
    // D-175: an explicitly selected track (region sequence) wins over stale
    // playback regions left in the host selection. The first playback region is
    // used only when it belongs to that track, which keeps item selection
    // working as before.
    if (!selectedSequence.empty())
    {
        const int sequence = selectedSequence.front();

        if (!playbackRegionSequence.empty() &&
            playbackRegionSequence.front() == sequence)
            return {AraSelectionSource::playbackRegion, 0};

        for (int i = 0; i < static_cast<int>(effectiveRegionSequence.size()); ++i)
            if (effectiveRegionSequence[static_cast<size_t>(i)] == sequence)
                return {AraSelectionSource::effectiveRegion, i};

        for (int i = 0; i < static_cast<int>(playbackRegionSequence.size()); ++i)
            if (playbackRegionSequence[static_cast<size_t>(i)] == sequence)
                return {AraSelectionSource::playbackRegion, i};

        // The selected track has no region in the selection lists. The caller
        // resolves the sequence's own first region.
        return {AraSelectionSource::regionSequence, -1};
    }

    if (!playbackRegionSequence.empty())
        return {AraSelectionSource::playbackRegion, 0};
    if (!effectiveRegionSequence.empty())
        return {AraSelectionSource::effectiveRegion, 0};
    return {};
}
} // namespace pitchnet
