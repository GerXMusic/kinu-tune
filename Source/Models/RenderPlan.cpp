// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "RenderPlan.h"

#include "../Audio/Synthesis/EngineBlocks.h"
#include "../Utils/Constants.h"

#include <algorithm>

namespace renderPlan {

Plan build(const Project &project, SynthesisEngineType globalEngine,
           std::uint64_t revision)
{
    Plan plan;
    plan.revision = revision;
    plan.globalEngine = globalEngine;

    const auto &notes = project.getNotes();
    const auto &audio = project.getAudioData();

    // Same D-129 boundary rule as the editor's block plan: silence measured on
    // the source span (D-262), because moving a note in time does not invent or
    // remove silence in the audio.
    const auto &source = audio.originalWaveform.getNumSamples() > 0
                             ? audio.originalWaveform
                             : audio.waveform;
    constexpr int kHop = HOP_SIZE;
    const int frameCount = static_cast<int>(audio.f0.size());

    Block current;
    int previousSourceEnd = -1;
    auto flushBlock = [&]() {
        if (current.notes.empty())
            return;
        plan.blocks.push_back(std::move(current));
        current = Block{};
    };

    for (int index = 0; index < static_cast<int>(notes.size()); ++index)
    {
        const auto &note = notes[static_cast<size_t>(index)];
        // D-299: la lapida eliminada es un rest, pero tiene que entrar en el
        // plan para que exista un pase que aplique su silencio.
        if ((note.isRest() && !note.isDeleted()) || !note.isDirty())
            continue;

        bool sameBlock = current.notes.empty();
        if (!sameBlock && previousSourceEnd >= 0)
        {
            const bool silence = engineBlocks::findSilenceRun(
                source, audio.sampleRate, previousSourceEnd * kHop,
                note.getSrcStartFrame() * kHop).has_value();
            sameBlock = !silence;
        }
        if (!sameBlock)
            flushBlock();

        if (current.notes.empty())
        {
            current.startFrame = note.getStartFrame();
            current.endFrame = note.getEndFrame();
        }
        else
        {
            current.startFrame = std::min(current.startFrame, note.getStartFrame());
            current.endFrame = std::max(current.endFrame, note.getEndFrame());
        }
        current.notes.push_back(
            {index, note.getDirtyRevision(), note.getStartFrame(), note.getEndFrame()});
        previousSourceEnd = note.getSrcEndFrame();
    }
    flushBlock();

    // D-248 padding: 60 frames of context inside the block, clipped at the
    // midpoint between neighbouring blocks (that midpoint is silence by
    // construction, so a block never extends into the next one).
    for (size_t i = 0; i < plan.blocks.size(); ++i)
    {
        auto &block = plan.blocks[i];
        int rangedStart = std::max(0, block.startFrame - 60);
        int rangedEnd = frameCount > 0
                            ? std::min(frameCount, block.endFrame + 60)
                            : block.endFrame + 60;
        if (i > 0)
        {
            const auto &previous = plan.blocks[i - 1];
            rangedStart = std::max(
                rangedStart, (previous.startFrame + previous.endFrame) / 2);
        }
        if (i + 1 < plan.blocks.size())
        {
            const auto &next = plan.blocks[i + 1];
            rangedEnd = std::min(rangedEnd,
                                 (next.startFrame + next.endFrame) / 2);
        }
        if (rangedEnd <= rangedStart)
            rangedEnd = rangedStart + 1;
        block.startFrame = rangedStart;
        block.endFrame = rangedEnd;
    }

    plan.f0StartFrame = project.getF0DirtyRange().first;
    plan.f0EndFrame = project.getF0DirtyRange().second;
    plan.f0Revision = project.getF0DirtyRevision();
    return plan;
}

} // namespace renderPlan
