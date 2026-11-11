// D-176: the canvas must reach the selected region without waiting for
// playback, and the Rendering card must show the document engine.
#include "../Source/UI/AraViewFocusPolicy.h"
#include "TestAssert.h"

#include <iostream>

using namespace pitchnet;

namespace
{
void testRegionBeyondCurrentTimelineIsReachable()
{
    // A 5 s project window with the selected region at 30-35 s. Without
    // extending the timeline, the scroll clamps to 0 and the notes stay
    // off-screen until playback extends it.
    const auto focus =
        computeAraViewFocus(30.0, 35.0, 0.0, 5.0, 100.0, 1000);
    CHECK(focus.timelineEnd >= 35.0);
    CHECK(focus.scrollX > 0.0);
}

void testRegionInsideTimelineKeepsTheTimeline()
{
    const auto focus =
        computeAraViewFocus(10.0, 15.0, 0.0, 100.0, 100.0, 1000);
    CHECK(focus.timelineEnd == 100.0);
    CHECK(focus.scrollX > 0.0);
}

void testPitchFitUsesNotesBeforeHydration()
{
    CHECK(shouldFitPitchRange(true, false));
    CHECK(shouldFitPitchRange(true, true));
    CHECK(!shouldFitPitchRange(false, false));
}

void testDocumentEngineFallsBackToThePreference()
{
    CHECK(displayedSynthesisEngine(std::nullopt, SynthesisEngineType::Pitchcore) ==
          SynthesisEngineType::Pitchcore);
    CHECK(displayedSynthesisEngine(SynthesisEngineType::Psola,
                                   SynthesisEngineType::Pitchcore) ==
          SynthesisEngineType::Psola);
}
} // namespace

int main()
{
    testRegionBeyondCurrentTimelineIsReachable();
    testRegionInsideTimelineKeepsTheTimeline();
    testPitchFitUsesNotesBeforeHydration();
    testDocumentEngineFallsBackToThePreference();

    std::cout << "ARA view focus tests passed\n";
    return 0;
}
