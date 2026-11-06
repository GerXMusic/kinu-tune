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

// D-180-R: a selection focus must not hide an edit cursor that was already on
// screen while the transport is stopped. Focus still wins during playback.
inline bool shouldRestoreCursorAfterFocus(bool transportPlaying,
                                          bool cursorWasVisible,
                                          bool cursorVisibleAfter)
{
    return !transportPlaying && cursorWasVisible && !cursorVisibleAfter;
}

// D-180-R: PianoRollComponent::updateScrollBars() updates the scrollbar range
// during layout. The scrollbar can echo a transient range back through
// scrollBarMoved(), collapsing the scroll to the start. The caller guards that
// window and restores the pre-layout scroll.
inline bool isTransientScrollbarReset(bool layoutGuardActive,
                                      double previousScroll,
                                      double newScroll,
                                      bool cursorWasVisible)
{
    return layoutGuardActive && cursorWasVisible && previousScroll > 100.0 &&
           newScroll < 1.0;
}

// D-180-R3: on editor open the host cursor must be seeded when the transport is
// stopped and the host supplied a position.
inline bool shouldSeedCursorOnOpen(bool transportPlaying, bool hasHostCursor)
{
    return !transportPlaying && hasHostCursor;
}

// D-180-R4: a region focus is only applied once its project is bound and no
// longer needs source hydration; otherwise the previous view stays on screen.
inline bool shouldApplyRegionFocus(bool projectReady)
{
    return projectReady;
}

// D-180-R5: while the transport is playing, a track switch must not scroll to
// the new region's start; keep the current (followed) view until the playhead
// is available.
inline bool shouldApplyFocusScroll(bool transportPlaying)
{
#if defined(PITCHNET_D180_R5_LEGACY)
    // D-180-R5 measurement only: pre-fix behaviour, focus scroll even in play.
    (void)transportPlaying;
    return true;
#else
    return !transportPlaying;
#endif
}
} // namespace pitchnet
