#include "EngineBlocks.h"

#include <algorithm>
#include <cmath>

namespace engineBlocks {

SynthesisEngineType effectiveEngine(const Note &note,
                                    SynthesisEngineType globalEngine) {
    if (const auto mapped = noteEngineToSynthesisEngine(note.getEngine()))
        return *mapped;
    return globalEngine;
}

std::optional<SilenceRun>
findSilenceRun(const juce::AudioBuffer<float> &source, int sampleRate,
               int fromSample, int toSample) {
    if (source.getNumChannels() <= 0 || sampleRate <= 0 ||
        source.getNumSamples() <= 0)
        return std::nullopt;

    const int totalSamples = source.getNumSamples();
    fromSample = std::clamp(fromSample, 0, totalSamples);
    toSample = std::clamp(toSample, fromSample, totalSamples);
    if (toSample - fromSample <= 0)
        return std::nullopt;

    const int window = std::max(1, static_cast<int>(std::lround(
                                       sampleRate * kSilenceWindowMs / 1000.0)));
    const int hop = std::max(1, static_cast<int>(std::lround(
                                    sampleRate * kSilenceHopMs / 1000.0)));
    const int minFrames = std::max(
        1, static_cast<int>(std::ceil(kSilenceMinMs / kSilenceHopMs)));

    // Every window must sit inside the gap: the caller bounds the search to
    // the space between two notes, and a window reaching into a note would
    // measure audio that is not in the gap.
    if (toSample - fromSample < window)
        return std::nullopt;

    const float *data = source.getReadPointer(0);
    const float floorLinear = std::pow(10.0f, kSilenceFloorDbfs / 20.0f);
    const int numFrames = 1 + (toSample - fromSample - window) / hop;

    auto frameIsSilent = [&](int frame) {
        const int start = fromSample + frame * hop;
        double energy = 0.0;
        for (int i = 0; i < window; ++i)
            energy += static_cast<double>(data[start + i]) *
                      static_cast<double>(data[start + i]);
        const double rms = std::sqrt(energy / window);
        const double db = 20.0 * std::log10(std::max(rms, static_cast<double>(
                                                              floorLinear)));
        return db < static_cast<double>(kSilenceThresholdDbfs);
    };

    SilenceRun best;
    bool found = false;
    int frame = 0;
    while (frame < numFrames) {
        if (!frameIsSilent(frame)) {
            ++frame;
            continue;
        }
        const int runStart = frame;
        while (frame < numFrames && frameIsSilent(frame))
            ++frame;
        const int runFrames = frame - runStart;
        if (runFrames < minFrames)
            continue;

        SilenceRun candidate;
        candidate.startSample = fromSample + runStart * hop;
        candidate.endSample = fromSample + (frame - 1) * hop + window;
        if (!found || candidate.getNumSamples() > best.getNumSamples()) {
            best = candidate;
            found = true;
        }
    }

    if (!found)
        return std::nullopt;
    return best;
}

bool canChangeEngineAfter(const Note &previous, const Note &note,
                          const juce::AudioBuffer<float> &source,
                          int sampleRate, int hopSize) {
    if (hopSize <= 0)
        return false;
    const int gapStart = previous.getEndFrame() * hopSize;
    const int gapEnd = note.getStartFrame() * hopSize;
    if (gapEnd <= gapStart)
        return false;
    if (static_cast<double>(gapEnd - gapStart) * 1000.0 / sampleRate <
        kSilenceMinMs)
        return false;
    return findSilenceRun(source, sampleRate, gapStart, gapEnd).has_value();
}

std::vector<Block> buildPlan(const std::vector<Note> &notes, int startFrame,
                             int endFrame, SynthesisEngineType globalEngine,
                             const juce::AudioBuffer<float> &source,
                             int sampleRate, int hopSize) {
    std::vector<Block> blocks;
    if (endFrame <= startFrame)
        return blocks;

    std::vector<const Note *> range;
    for (const auto &note : notes) {
        if (note.isRest())
            continue;
        if (note.getEndFrame() <= startFrame || note.getStartFrame() >= endFrame)
            continue;
        range.push_back(&note);
    }
    std::stable_sort(range.begin(), range.end(),
                     [](const Note *a, const Note *b) {
                         return a->getStartFrame() < b->getStartFrame();
                     });

    if (range.empty()) {
        blocks.push_back({startFrame, endFrame, globalEngine});
        return blocks;
    }

    Block current;
    current.startFrame = startFrame;
    current.engine = effectiveEngine(*range.front(), globalEngine);

    const Note *previous = range.front();
    for (size_t i = 1; i < range.size(); ++i) {
        const Note &note = *range[i];
        const auto requested = effectiveEngine(note, globalEngine);
        if (requested == current.engine) {
            previous = &note;
            continue;
        }
        if (!canChangeEngineAfter(*previous, note, source, sampleRate,
                                  hopSize)) {
            // No usable silence: the note belongs to the current block and
            // renders with the block's engine (D-125/D-129). Its own request
            // is not honoured, and that is the point of the rule.
            previous = &note;
            continue;
        }

        const auto gapStart = previous->getEndFrame() * hopSize;
        const auto gapEnd = note.getStartFrame() * hopSize;
        const auto run = findSilenceRun(source, sampleRate, gapStart, gapEnd);
        int boundaryFrame = (previous->getEndFrame() + note.getStartFrame()) / 2;
        if (run) {
            const int midSample =
                (run->startSample + run->endSample) / 2;
            boundaryFrame =
                hopSize > 0 ? static_cast<int>(std::lround(
                                 static_cast<double>(midSample) / hopSize))
                            : boundaryFrame;
        }
        boundaryFrame =
            std::clamp(boundaryFrame, previous->getEndFrame(),
                       note.getStartFrame());
        boundaryFrame = std::max(boundaryFrame, current.startFrame + 1);
        boundaryFrame = std::min(boundaryFrame, endFrame - 1);

        if (boundaryFrame <= current.startFrame) {
            previous = &note;
            continue;
        }

        current.endFrame = boundaryFrame;
        blocks.push_back(current);
        current.startFrame = boundaryFrame;
        current.engine = requested;
        previous = &note;
    }

    current.endFrame = endFrame;
    if (current.endFrame > current.startFrame)
        blocks.push_back(current);
    return blocks;
}

EngineEditPlan planEngineEdits(const std::vector<Note> &notes,
                               const std::vector<int> &selection,
                               NoteEngine requested,
                               SynthesisEngineType globalEngine,
                               const juce::AudioBuffer<float> &source,
                               int sampleRate, int hopSize) {
    EngineEditPlan plan;
    if (notes.empty() || selection.empty())
        return plan;

    SynthesisEngineType requestedEngine = globalEngine;
    if (const auto mapped = noteEngineToSynthesisEngine(requested))
        requestedEngine = *mapped;

    std::vector<int> order;
    order.reserve(notes.size());
    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
        order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return notes[static_cast<size_t>(a)].getStartFrame() <
               notes[static_cast<size_t>(b)].getStartFrame();
    });

    auto isSelected = [&](int index) {
        return std::find(selection.begin(), selection.end(), index) !=
               selection.end();
    };

    // The engine the block currently being walked renders with. Notes that are
    // not selected still move it: the plan has to see the project as it is.
    const Note *previous = nullptr;
    SynthesisEngineType blockEngine = globalEngine;
    bool haveBlock = false;

    for (int index : order) {
        const Note &note = notes[static_cast<size_t>(index)];
        const auto stored = effectiveEngine(note, globalEngine);
        if (!haveBlock) {
            blockEngine = stored;
            haveBlock = true;
        }

        if (!isSelected(index)) {
            blockEngine = stored;
            previous = &note;
            continue;
        }

        NoteEngine next = requested;
        if (requestedEngine == blockEngine) {
            // Same engine the block already renders with: just store it, no
            // silence is needed because nothing changes for the audio.
            blockEngine = requestedEngine;
        } else if (previous == nullptr ||
                   canChangeEngineAfter(*previous, note, source, sampleRate,
                                        hopSize)) {
            blockEngine = requestedEngine;
        } else {
            // No usable silence before this note: it cannot break away from
            // the block, so it keeps the engine that will actually render it.
            ++plan.refused;
            next = blockEngine == globalEngine
                       ? NoteEngine::Inherit
                       : noteEngineFromSynthesisEngine(blockEngine);
        }

        if (next != note.getEngine()) {
            // Only record a change that changes what renders. Unpinning a note
            // whose engine already equals the global one (a refusal that lands
            // on the same sound) is not an edit the user asked for.
            auto effectiveOf = [&](NoteEngine value) {
                const auto mapped = noteEngineToSynthesisEngine(value);
                return mapped ? *mapped : globalEngine;
            };
            if (effectiveOf(next) != stored)
                plan.edits.push_back({index, note.getEngine(), next});
        }
        previous = &note;
    }

    return plan;
}

}  // namespace engineBlocks
