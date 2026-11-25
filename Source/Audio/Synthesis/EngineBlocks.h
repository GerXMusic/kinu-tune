#pragma once

#include "../../Models/Note.h"
#include "../SynthesisEngineType.h"

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
/// back to the instance-wide engine. D-181: the request is honoured as stored;
/// there is no hidden engine swap based on the size of the pitch shift.
SynthesisEngineType effectiveEngine(const Note &note,
                                    SynthesisEngineType globalEngine);

/**
 * Longest run of silence in [fromSample, toSample) of @p source, measured on
 * channel 0. Returns nothing when there is no run of at least kSilenceMinMs.
 */
std::optional<SilenceRun>
findSilenceRun(const juce::AudioBuffer<float> &source, int sampleRate,
               int fromSample, int toSample);

/**
 * D-302: LAST run of silence in [fromSample, toSample). Same window (5 ms),
 * hop (1 ms) and minimum length (20 ms) as findSilenceRun, but it returns the
 * run closest to the end of the range: the one next to a note's onset, which
 * is what deleting a note needs. findSilenceRun keeps answering the D-129
 * question (longest run) and is not affected.
 *
 * The threshold is a parameter because the attack of a deleted note is
 * measured against the take's own body level and not only against the
 * absolute D-129 value; the default keeps the D-129 reading. The minimum
 * length is asked of the whole reported run: runs closer to each other than
 * the minimum are merged first, and a few milliseconds of wobble over the
 * threshold do not end a real silence.
 */
std::optional<SilenceRun>
findLastSilenceRun(const juce::AudioBuffer<float> &source, int sampleRate,
                   int fromSample, int toSample,
                   float thresholdDbfs = kSilenceThresholdDbfs);

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

/// Why a requested engine change could not start a new block (D-185).
enum class EngineRefusalReason {
    None,
    /// D-129: no run of at least 20 ms below -55 dBFS before the note.
    NoSilence,
};

/// A selected note whose requested engine was refused, kept so the UI and the
/// MCP can explain it (D-185). The rule itself is D-125/D-129 and does not
/// change: the note still renders with its block's engine.
struct RefusedEdit {
    int noteIndex = 0;
    NoteEngine requested = NoteEngine::Inherit;
    EngineRefusalReason reason = EngineRefusalReason::NoSilence;
};

struct EngineEditPlan {
    std::vector<EngineEdit> edits;
    /// Selected notes that asked for a change and could not take it (no
    /// usable silence before them). They keep the block's engine instead.
    int refused = 0;
    /// One entry per refused note, with what it asked for and why it lost.
    std::vector<RefusedEdit> refusals;
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
 * D-257/D-258: one engine change applied to a whole engine block.
 *
 * The unit of the gesture is the block `buildPlan` gives for the anchor note:
 * all its non-rest notes store @p requested explicitly, and nothing outside it
 * changes. @p overwritten counts the notes whose *effective* engine changes;
 * @p outOfBenefitRange lists the notes whose target pitch is outside
 * [-1.5, +2.5] semitones from their source (the user's Vocoder-benefit range,
 * informational when choosing the classic engine).
 */
struct BlockEngineEdit {
    std::vector<EngineEdit> edits;
    std::vector<int> blockNotes;
    int overwritten = 0;
    std::vector<int> outOfBenefitRange;
    bool changesSound = false;
};

/**
 * Works out the block-wide engine change for the note at @p anchorIndex.
 * Returns an empty plan when the anchor is invalid, a rest, or outside every
 * block. See D-258.
 */
BlockEngineEdit planBlockEngineEdit(const std::vector<Note> &notes,
                                    int anchorIndex, NoteEngine requested,
                                    SynthesisEngineType globalEngine,
                                    const juce::AudioBuffer<float> &source,
                                    int sampleRate, int hopSize,
                                    float benefitMinSemitones = -1.5f,
                                    float benefitMaxSemitones = 2.5f);

/**
 * D-257/D-267 (T5): numero de notas del proyecto cuyo desplazamiento
 * `getAdjustedMidiNote() - getOriginalMidiNote()` queda fuera del rango de
 * beneficio del Vocoder. Es el mismo predicado que usa el aviso de D-258 y el
 * que publica el canal de control (`out_of_range_count`), para que UI y canal
 * no puedan discrepar. Las notas rest no cuentan.
 */
int countOutOfBenefitRange(const std::vector<Note> &notes,
                           float benefitMinSemitones = -1.5f,
                           float benefitMaxSemitones = 2.5f);

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
                             int sampleRate, int hopSize);

/**
 * The render unit that owns a dirty span (D-239).
 *
 * The block is the unit that is both rendered and committed: an engine's
 * output depends on the whole voiced run it belongs to (PSOLA and the
 * classical core anchor their grain lattice once per run), so splicing a
 * note-sized piece of a new render against old audio leaves the two phases
 * mismatched. Committing the whole block moves every splice to a silence
 * boundary, where it is inaudible, and keeps untouched blocks bit for bit
 * unchanged (D-125 b).
 *
 * Returns every block that intersects [dirtyStart, dirtyEnd), so a range that
 * crosses a block boundary commits both blocks (conservative: the boundary is
 * a silence, nothing sounding is rewritten twice). A point selects the block
 * that contains it. When the range falls outside the plan, the block
 * containing @p dirtyStart is returned, or the first block if none does; an
 * empty plan returns empty.
 */
std::vector<Block> blocksToCommit(const std::vector<Block> &plan,
                                  int dirtyStart, int dirtyEnd);

/// One frame span the renderer will write back into the composite.
struct CommitRange {
    int startFrame = 0;
    int endFrame = 0;
};

/**
 * Commit spans for a render pass (D-239).
 *
 * The dirty notes choose the spans; a dirty note outside [renderStartFrame,
 * renderEndFrame) belongs to another pass and is ignored. The span is the WHOLE
 * engine block for every engine: the block is the render unit, its edges are
 * silences or padding, and a note-sized splice is what used to mix two phases
 * (PSOLA/Pitchcore) or two vocoder generations inside a sounding run.
 *
 * When there is no dirty-note anchor (global pitch/formant, DAW automation,
 * timing undo) the F0 range selects whole blocks as well.
 */
std::vector<CommitRange>
commitRangesForNotes(const std::vector<Note> &notes,
                     const std::vector<Block> &plan, bool hasDirtyNoteAnchors,
                     int f0DirtyStart, int f0DirtyEnd, int renderStartFrame,
                     int renderEndFrame);

}  // namespace engineBlocks
