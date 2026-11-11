#pragma once

#include <algorithm>
#include <optional>

#include "../Audio/SynthesisEngineType.h"

namespace pitchnet
{
// D-176: where the canvas must look after the active region changes.
//
// Pure policy so the viewport behaviour can be tested without a host. The
// timeline end must cover the region span before the scroll is clamped;
// otherwise a region that starts beyond the current project window is
// unreachable and the canvas stays at the project start until playback
// extends the timeline.
struct AraViewFocus
{
    double timelineEnd = 0.0;
    double scrollX = 0.0;
};

inline AraViewFocus computeAraViewFocus(double spanStart, double spanEnd,
                                        double timelineStart,
                                        double timelineDuration,
                                        double pixelsPerSecond,
                                        int visibleWidth)
{
    // D-176: extend the timeline to cover the selected region before clamping
    // the scroll. A restored ARA project can be shorter than the modification
    // span; without the extension the canvas stays at the project start until
    // playback extends the timeline.
    const double start = std::max(timelineStart, spanStart);
    const double end = std::max(start, spanEnd);
    const double currentEnd = timelineStart + std::max(0.0, timelineDuration);
    const double timelineEnd = std::max(currentEnd, end);

    double newScrollX = start * pixelsPerSecond;
    const double rangeWidth = (end - start) * pixelsPerSecond;
    if (rangeWidth > 0.0 &&
        rangeWidth < static_cast<double>(visibleWidth) * 0.75)
        newScrollX += rangeWidth * 0.5 -
                      static_cast<double>(visibleWidth) * 0.5;
    else
        newScrollX -= static_cast<double>(visibleWidth) * 0.12;

    const double maxScroll = std::max(
        timelineStart * pixelsPerSecond,
        timelineEnd * pixelsPerSecond - static_cast<double>(visibleWidth));
    newScrollX = std::clamp(newScrollX, timelineStart * pixelsPerSecond,
                            std::max(timelineStart * pixelsPerSecond, maxScroll));

    return {timelineEnd, newScrollX};
}

// D-176: a restored ARA project can carry notes before its waveform/F0 are
// hydrated. The vertical fit must use the notes instead of waiting for the
// audio analysis state.
inline bool shouldFitPitchRange(bool hasNotes, bool hasAnalyzed)
{
    return hasAnalyzed || hasNotes;
}

// D-176: the Rendering card shows the document's engine, or the shared
// preference for a legacy document without its own value.
inline SynthesisEngineType displayedSynthesisEngine(
    const std::optional<SynthesisEngineType> &documentEngine,
    SynthesisEngineType preference)
{
    return documentEngine.value_or(preference);
}
} // namespace pitchnet
