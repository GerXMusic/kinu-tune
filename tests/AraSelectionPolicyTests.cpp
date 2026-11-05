// D-175: the window must follow the selected region sequence (track), not
// just the first playback region in a stale multi-item selection.
#include "../Source/Plugin/AraSelectionPolicy.h"
#include "TestAssert.h"

#include <iostream>
#include <vector>

using pitchnet::AraSelectionSource;
using pitchnet::chooseAraSelectionTarget;

namespace
{
void testTrackSelectionBeatsStaleFirstItem()
{
    // REAPER reports both items as playback regions, A/1 first, while the
    // selected track is T2 (sequence 1). The target must be A/2, not A/1.
    const std::vector<int> regions{0, 1};
    const std::vector<int> effective{0, 1};
    const std::vector<int> sequences{1};

    const auto target = chooseAraSelectionTarget(regions, effective, sequences);
    CHECK(target.source == AraSelectionSource::effectiveRegion);
    CHECK(target.index == 1);
}

void testItemSelectionStillWinsWhenItIsOnTheSelectedTrack()
{
    // The first playback region is the clicked item on the selected track.
    const std::vector<int> regions{1, 0};
    const std::vector<int> effective{1, 0};
    const std::vector<int> sequences{1};

    const auto target = chooseAraSelectionTarget(regions, effective, sequences);
    CHECK(target.source == AraSelectionSource::playbackRegion);
    CHECK(target.index == 0);
}

void testTrackSelectionUsesEffectiveRegions()
{
    // A track-only selection: no explicit playback regions, but the effective
    // list contains the regions of every selected sequence.
    const std::vector<int> regions;
    const std::vector<int> effective{0, 1};
    const std::vector<int> sequences{1};

    const auto target = chooseAraSelectionTarget(regions, effective, sequences);
    CHECK(target.source == AraSelectionSource::effectiveRegion);
    CHECK(target.index == 1);
}

void testSelectedTrackWithNoRegionInTheListsFallsBackToItsSequence()
{
    // The selected track has no item in the current selection lists; the
    // caller resolves the sequence's own first region.
    const std::vector<int> regions{0};
    const std::vector<int> effective{0};
    const std::vector<int> sequences{1};

    const auto target = chooseAraSelectionTarget(regions, effective, sequences);
    CHECK(target.source == AraSelectionSource::regionSequence);
    CHECK(target.index == -1);
}

void testNoSequenceKeepsTheOldPriority()
{
    const std::vector<int> regions{0};
    const std::vector<int> effective{1};
    const std::vector<int> sequences;

    const auto target = chooseAraSelectionTarget(regions, effective, sequences);
    CHECK(target.source == AraSelectionSource::playbackRegion);
    CHECK(target.index == 0);
}

void testOnlyEffectiveRegionsKeepsTheOldPriority()
{
    const std::vector<int> regions;
    const std::vector<int> effective{1};
    const std::vector<int> sequences;

    const auto target = chooseAraSelectionTarget(regions, effective, sequences);
    CHECK(target.source == AraSelectionSource::effectiveRegion);
    CHECK(target.index == 0);
}

void testUpstreamDeviceSelectionUsesTheFirstSequence()
{
    // Upstream plugins can report a region from another track; the selected
    // sequence still decides which track the window should show.
    const std::vector<int> regions{0};
    const std::vector<int> effective{0};
    const std::vector<int> sequences{1};

    const auto target = chooseAraSelectionTarget(regions, effective, sequences);
    CHECK(target.source == AraSelectionSource::regionSequence);
    CHECK(target.index == -1);
}

void testEmptySelection()
{
    const std::vector<int> empty;
    const auto target = chooseAraSelectionTarget(empty, empty, empty);
    CHECK(target.source == AraSelectionSource::none);
    CHECK(target.index == -1);
}
} // namespace

int main()
{
    testTrackSelectionBeatsStaleFirstItem();
    testItemSelectionStillWinsWhenItIsOnTheSelectedTrack();
    testTrackSelectionUsesEffectiveRegions();
    testSelectedTrackWithNoRegionInTheListsFallsBackToItsSequence();
    testNoSequenceKeepsTheOldPriority();
    testOnlyEffectiveRegionsKeepsTheOldPriority();
    testUpstreamDeviceSelectionUsesTheFirstSequence();
    testEmptySelection();

    std::cout << "ARA selection policy tests passed\n";
    return 0;
}
