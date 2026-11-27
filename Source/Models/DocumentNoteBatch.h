// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "DocumentNoteIndex.h"
#include "DocumentNoteSelection.h"

#include <functional>
#include <optional>
#include <vector>

/**
 * Document-level batch edits (N3, decision D-343).
 *
 * One operation over the selection of the whole track: every selected note is
 * edited in its own item (its own Project), in a deterministic order (region,
 * note index). The notes stay the same ones the index publishes: this layer
 * copies nothing and keeps no second set of edits.
 *
 * Rules of the phase:
 *   - pitch goes through NotePitch::editTarget (the same function the drag and
 *     the arrow keys use), never through a raw setter and never through a
 *     second copy of the clamping/reference maths;
 *   - an operation plans the whole selection before writing, so a rejected one
 *     leaves every note of every item exactly as it was (no half batch);
 *   - no undo is registered here. The document-level single undo step is N4;
 *     the single-item paths keep their own actions.
 */
namespace documentNoteBatch
{
// One selected note, with the window the set attached it to.
struct Target
{
    Project* project = nullptr; // not owned; the note's own item
    Note* note = nullptr;       // inside project->getNotes(); never copied
    juce::String regionKey;     // lowest-index window that shows the note
    double offsetSeconds = 0.0; // host = source + offset, for a track grid
};

enum class TiltLayer
{
    General = 0, // D-308: the general layer (the body is the centre)
    FineLeft,
    FineRight
};

struct Result
{
    enum class Status
    {
        Nothing,  // nothing to do: the operation is a no-op
        Applied,  // at least one note changed
        Rejected  // refused as a whole; nothing was written
    };

    Status status = Status::Nothing;
    int notes = 0;                 // notes the operation changed
    std::vector<Project*> projects; // touched items, in (region, note) order
    juce::String reason;           // machine token when rejected

    bool applied() const { return status == Status::Applied; }
    bool rejected() const { return status == Status::Rejected; }
};

/** The selection as targets, deduplicated by (Project*, Note*) and sorted by
    (region key, index of the note in its project): the deterministic order of
    the batch. A note of a project the index does not show is left out. */
std::vector<Target> targetsOf(const DocumentNoteSelection& selection,
                              const DocumentNoteIndex& index);

/** Relative pitch move. `semitonesFor` gets the note's own project, so each
    item uses its own reference Hz and scale. */
Result shiftPitch(
    const std::vector<Target>& targets,
    const std::function<std::optional<float>(const Project&, const Note&)>&
        semitonesFor);

/** One tilt layer for the whole selection, written field by field. */
Result setTilt(const std::vector<Target>& targets, TiltLayer layer, float value);

/** Mute/unmute the whole selection. */
Result setMuted(const std::vector<Target>& targets, bool muted);

/** Delete (tombstone + rest) the whole selection. */
Result deleteNotes(const std::vector<Target>& targets);

/** Quantize the timing of the selection to the host grid, one grid for the
    track: each item's phase comes from the window offset of its notes. Rejected
    (nothing written) when sibling windows of one item disagree on the phase or
    when the quantized plan of any item would collapse or overlap a note. */
Result quantize(const std::vector<Target>& targets, double gridSeconds,
                int strengthPercent);
} // namespace documentNoteBatch
