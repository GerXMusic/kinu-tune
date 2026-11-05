#pragma once

#include "../../Models/Note.h"
#include "../SynthesisEngineType.h"
#include "LargeShiftEnginePolicy.h"

#include <optional>
#include <vector>

/**
 * Engine blocks (M2 paso 2, D-125 + D-129).
 *
 * The product rule, confirmed by the user on 2026-10-30: a note's engine may
 * only change **between blocks, and only when there is silence between them**.
 * Two neighbouring notes with no usable silence between them cannot have
 * different engines: the second one inherits the block's engine.
 *
 * A block is derived at render time and never stored. The block's engine is
 * the engine of the notes it contains; a silence between two notes with the
 * same engine is not a boundary (nothing changes there).
 *
 * Silence is measured on the source audio with the criterion registered as
 * D-129 (plan/m2/m2-silencio-umbral.md): RMS over a 5 ms window, 1 ms hop,
 * absolute dBFS, a run of at least 20 ms below -55 dBFS. That threshold was
 * the highest one with zero silences over voiced audio in the 13 closed takes;
 * it is a parameter of this rule, not a quality criterion.
 */
namespace engineBlocks {

/// D-129: the silence that allows an engine change between blocks.
inline constexpr float kSilenceThresholdDbfs = -55.0f;
inline constexpr double kSilenceMinMs = 20.0;
inline constexpr double kSilenceWindowMs = 5.0;
inline constexpr double kSilenceHopMs = 1.0;
/// Convenience floor, same as the measurement tool: 20*log10(1e-6).
inline constexpr float kSilenceFloorDbfs = -120.0f;

/// One contiguous span of frames rendered with a single engine.
struct Block {
    int startFrame = 0;
    int endFrame = 0;  // exclusive
    SynthesisEngineType engine = SynthesisEngineType::Psola;

    int getNumFrames() const { return endFrame - startFrame; }
};

/// Absolute sample range [startSample, endSample) below the threshold.
struct SilenceRun {
    int startSample = 0;
    int endSample = 0;

    int getNumSamples() const { return endSample - startSample; }
};

/// The concrete engine a note asks for; Inherit (and anything unmapped) falls
/// back to the instance-wide engine. D-180: a Pitchcore request with a shift
/// above kPitchcoreMaxShiftCents is rendered by the vocoder when available.
SynthesisEngineType effectiveEngine(const Note &note,
                                    SynthesisEngineType globalEngine,
                                    bool vocoderAvailable = true);

/**
 * Longest run of silence in [fromSample, toSample) of @p source, measured on
 * channel 0. Returns nothing when there is no run of at least kSilenceMinMs.
 */
std::optional<SilenceRun>
findSilenceRun(const juce::AudioBuffer<float> &source, int sampleRate,
               int fromSample, int toSample);

/**
 * Whether @p note is allowed to use an engine different from the one its
 * predecessor @p previous ends up with: true only when the gap between them
 * holds a usable silence (D-129). This is the predicate the UI uses to refuse
 * a change, so both sides answer the same question.
 */
bool canChangeEngineAfter(const Note &previous, const Note &note,
                          const juce::AudioBuffer<float> &source,
                          int sampleRate, int hopSize);

/// One note whose stored engine changes, with the value it had before.
struct EngineEdit {
    int noteIndex = 0;
    NoteEngine previous = NoteEngine::Inherit;
    NoteEngine next = NoteEngine::Inherit;
};

struct EngineEditPlan {
    std::vector<EngineEdit> edits;
    /// Selected notes that asked for a change and could not take it (no
    /// usable silence before them). They keep the block's engine instead.
    int refused = 0;
};

/**
 * Works out what a "set engine on the selection" gesture would store, honouring
 * the same rule the renderer applies: a note only takes a different engine when
 * the gap with its predecessor holds a usable silence. Notes that cannot change
 * are left on the engine of the block they are in, and counted in `refused`, so
 * what the UI shows is what the renderer will do.
 *
 * @p selection holds indices into @p notes; order does not matter. Notes are
 * walked in start-frame order, so a selected run can move together.
 */
EngineEditPlan planEngineEdits(const std::vector<Note> &notes,
                               const std::vector<int> &selection,
                               NoteEngine requested,
                               SynthesisEngineType globalEngine,
                               const juce::AudioBuffer<float> &source,
                               int sampleRate, int hopSize);

/**
 * Blocks covering exactly [startFrame, endFrame), in order and contiguous.
 *
 * - Notes are the ones overlapping the range; rests do not carry an engine.
 * - The block starts with the first note's effective engine (or @p
 *   globalEngine when the range has no notes).
 * - A note asking for a different engine only starts a new block when the gap
 *   with its predecessor holds a usable silence; the boundary sits at the
 *   middle of that silence. Otherwise the note stays in the current block and
 *   therefore renders with the block's engine.
 *
 * @p source is the pristine source audio the notes were detected from, and
 * @p hopSize the frame grid the notes live on.
 */
std::vector<Block> buildPlan(const std::vector<Note> &notes, int startFrame,
                             int endFrame, SynthesisEngineType globalEngine,
                             const juce::AudioBuffer<float> &source,
                             int sampleRate, int hopSize,
                             bool vocoderAvailable = true);

}  // namespace engineBlocks
