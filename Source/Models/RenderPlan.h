// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "Project.h"

#include <cstdint>
#include <vector>

/**
 * D-292: immutable render plan for one revision.
 *
 * The plan is built ONCE at the start of a render chain from the project's
 * current dirty state and is never modified while that chain runs. The backend
 * walks its blocks with a local index; no cursor is stored in the Project, so an
 * edit that arrives mid-chain cannot retarget the chain that is already running.
 *
 * A block is the same unit the renderer already commits (D-239): contiguous
 * edited notes separated by a D-129 silence, padded and clipped at the midpoint
 * of the silence (D-248). Each block carries the notes it contains with the
 * dirty revision they had when the plan was built, which is what the closing
 * step uses to decide whether the note's audio was really committed.
 */
namespace renderPlan {

struct NoteRef
{
    int index = -1;
    std::uint64_t dirtyRevision = 0;
    int startFrame = 0;
    int endFrame = 0;
};

struct Block
{
    int startFrame = 0;
    int endFrame = 0;
    std::vector<NoteRef> notes;
};

struct Plan
{
    // Revision of the editor model this plan was built from. Informative: the
    // clearing decisions compare each note's own dirtyRevision, not this.
    std::uint64_t revision = 0;
    std::vector<Block> blocks;
    // F0 range/revision captured with the plan. A plan with no blocks but a
    // valid F0 range still performs one pass (a curve-only edit).
    int f0StartFrame = -1;
    int f0EndFrame = -1;
    std::uint64_t f0Revision = 0;

    bool empty() const { return blocks.empty() && f0StartFrame < 0; }
};

/** Snapshot of the project's dirty state. Deterministic: same state, same plan. */
Plan build(const Project &project, std::uint64_t revision = 0);

} // namespace renderPlan
