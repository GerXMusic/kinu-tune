#pragma once

#include <algorithm>

namespace pitchnet
{
// D-180: the canvas must follow a host seek even when the transport is stopped.
// Before this policy the scroll only moved during playback, so a cursor placed
// outside the current window left the notes parked at the project start until
// the user pressed play.
inline bool shouldFollowCursor(bool transportPlaying, bool hostSeekMovedCursor)
{
#if defined(PITCHNET_D180_LEGACY)
    // D-180 measurement only: pre-fix rule, the view followed playback alone.
    (void)hostSeekMovedCursor;
    return transportPlaying;
#else
    // D-180: a host seek is a user action on the timeline, not playback drift;
    // it moves the view whether or not the transport is running.
    if (hostSeekMovedCursor)
        return true;
    return transportPlaying;
#endif
}

// D-180: a selection moves the view to the selected region and suppresses
// playback follow for a moment so the selection is not yanked away while the
// transport is already running. When the transport starts from a stop, that
// suppression must not survive: the first playback frame has to show where the
// playhead actually is.
inline bool shouldClearFollowSuppression(bool wasPlaying, bool nowPlaying)
{
#if defined(PITCHNET_D180_LEGACY)
    // D-180 measurement only: the old code kept the selection suppression.
    (void)wasPlaying;
    (void)nowPlaying;
    return false;
#else
    return !wasPlaying && nowPlaying;
#endif
}

// Returns the scroll that keeps `cursorSeconds` inside the visible content.
// The calculation mirrors PianoRollComponent::clampScrollX(): the minimum is
// the timeline start and the maximum is the timeline duration minus the visible
// width, so a returned value is always accepted by setScrollX().
inline double scrollXToRevealCursor(double cursorSeconds,
                                    double currentScrollX,
                                    double pixelsPerSecond,
                                    int visibleWidth,
                                    double timelineStart,
                                    double timelineDuration)
{
    if (pixelsPerSecond <= 0.0 || visibleWidth <= 0)
        return currentScrollX;

    const double width = static_cast<double>(visibleWidth);
    const double cursorX = cursorSeconds * pixelsPerSecond;
    if (cursorX >= currentScrollX && cursorX <= currentScrollX + width)
        return currentScrollX;

    const double minScroll = timelineStart * pixelsPerSecond;
    const double maxScroll = std::max(
        minScroll,
        timelineDuration * pixelsPerSecond - width);
    const double target = cursorX - width * 0.3;
    return std::clamp(target, minScroll, maxScroll);
}
} // namespace pitchnet
