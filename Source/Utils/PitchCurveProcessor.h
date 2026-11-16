#pragma once

#include "../Models/Project.h"
#include <vector>

namespace PitchCurveProcessor
{
    // D-223/D-224: share of the D-221 ramp over the D-205 baseline in a legato
    // union. It is an engine law, not a per-note control (Melodyne exposes one
    // duration); the user tuned the value by ear. Exposed so the regression
    // test reads the same number the engine uses.
    inline constexpr float kTransitionBlendShare = 0.50f;

    // D-183 R6 test hook: the D-182 glide must be attributable on/off.
    void setEditedBoundaryGlideEnabled(bool enabled);

    /**
     * Linearly interpolate pitch through unvoiced regions using the uv mask.
     * Returns a dense pitch (Hz) array with the same length as the input.
     */
    std::vector<float> interpolateWithUvMask(const std::vector<float>& pitchHz,
                                             const std::vector<bool>& uvMask);

    /**
     * Rebuild base pitch (midi) from current notes and keep existing delta.
     * Ensures base/delta are dense and aligned to the project frame count,
     * then composes audioData.f0 (without applying the uv mask).
     */
    void rebuildBaseFromNotes(Project& project);

    // Cache the linear drift response once for interactive display previews.
    std::vector<float> getDriftPreviewResponse(const Project& project, const Note& note);

    /**
     * D-226/D-229: grow [startFrame, endFrame) (exclusive) with every
     * transition window that crosses it, and with the WHOLE notes that window
     * touches. The ramp is symmetric around the boundary, so half of it lives
     * in the neighbouring note; and a curve change inside a note moves that
     * note's whole PSOLA grain lattice, so committing only the window splices
     * new audio against old-lattice audio. Re-rendering the full notes is what
     * the user's manual "render the notes again" did.
     *
     * A window is the one the engine already uses (buildDeltaJoins) for a
     * user transition (transitionMs > 0), including the D-205 baseline it
     * mixes with. Frames outside the notes touched are not changed. Returns
     * true when the range changed.
     */
    bool expandRangeForJoinSmoothing(const Project& project,
                                     int& startFrame, int& endFrame);

    /**
     * D-230: the whole-note span a transition change of `leftNote` can touch,
     * computed from the union of the old and new values. When a transition is
     * removed (new value 0), the current curve no longer contains its window,
     * so expandRangeForJoinSmoothing cannot see it; the old ramp still lives
     * in the right note's audio and must be rewritten. Returns false when the
     * note has no contiguous right neighbour or both values are 0.
     *
     * This does not modify the project; the caller records the span with
     * Project::addTransitionCommitRange() and widens its dirty range, so the
     * next render both renders and commits it.
     */
    bool transitionChangeCommitRange(const Project& project,
                                     const Note& leftNote,
                                     int oldTransitionMs,
                                     int newTransitionMs,
                                     int& startFrame, int& endFrame);

    /**
     * Rebuild delta pitch and f0 only for specific notes (partial rebuild).
     * Skips basePitch regeneration (assumes positions unchanged).
     * Much faster than rebuildBaseFromNotes() when only transformation
     * parameters change on a few notes.
     */
    void rebuildDeltaForNotes(Project& project, const std::vector<Note*>& affectedNotes);

    /**
     * Lightweight rebuild for interactive note drag.
     * Regenerates basePitch from ALL notes (needed for correct pitch display),
     * but only processes deltaPitch for the specified affected notes instead
     * of iterating every note.  Recomposes f0 only for the affected range.
     * Much faster than rebuildBaseFromNotes() during interactive drag.
     */
    void rebuildBaseFromNotesForDrag(Project& project, const std::vector<Note*>& affectedNotes);

    /**
     * Rebuild base and delta from a source pitch (Hz). This is used after
     * detection/segmentation or when we need to recompute delta from edited
     * curves. The uv mask is kept as-is; the composed f0 omits uv masking.
     */
    void rebuildCurvesFromSource(Project& project,
                                 const std::vector<float>& sourcePitchHz);

    /**
     * Compose f0 (Hz) from base + delta + optional global offset.
     * When applyUvMask is true, frames marked unvoiced are forced to 0 for
     * synthesis; when false the curve stays dense for UI display.
     */
    std::vector<float> composeF0(const Project& project,
                                 bool applyUvMask,
                                 float globalPitchOffset = 0.0f);

    /**
     * Convenience to update audioData.f0 in-place using composeF0.
     */
    void composeF0InPlace(Project& project,
                          bool applyUvMask,
                          float globalPitchOffset = 0.0f);
} // namespace PitchCurveProcessor

