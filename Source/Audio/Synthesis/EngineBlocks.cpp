#include "EngineBlocks.h"

#include <algorithm>
#include <cmath>

namespace engineBlocks {

SynthesisEngineType effectiveEngine(const Note &note,
                                    SynthesisEngineType globalEngine) {
    // D-181: the engine is exactly what the note stores. A large pitch move
    // no longer changes it behind the user's back.
    SynthesisEngineType requested = globalEngine;
    if (const auto mapped = noteEngineToSynthesisEngine(note.getEngine()))
        requested = *mapped;
    return requested;
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

std::optional<SilenceRun>
findLastSilenceRun(const juce::AudioBuffer<float> &source, int sampleRate,
                   int fromSample, int toSample, float thresholdDbfs) {
    // D-302. Mismo criterio de ventana que findSilenceRun (5 ms, hop 1 ms,
    // minimo 20 ms) y misma lectura de canal 0, pero se queda con el ULTIMO
    // tramo valido en vez del mas largo: el que esta pegado al onset. No se
    // factoriza con findSilenceRun a proposito: RenderPlan depende de que esa
    // funcion siga devolviendo el tramo mas largo (D-129) y este parche no
    // toca esa respuesta.
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
        const double db = 20.0 * std::log10(std::max(
                               rms, static_cast<double>(floorLinear)));
        return db < static_cast<double>(thresholdDbfs);
    };

    std::optional<SilenceRun> last;
    // D-302: primero se recogen TODOS los tramos bajos y despues se fusionan
    // los que estan a menos del minimo (20 ms); el minimo se exige al tramo ya
    // fusionado. Si se descartaran los trozos cortos antes de fusionar, un
    // umbral que caiga dentro del vaiven del suelo (perfil de 5 ms, +/-2 dB)
    // partiria el silencio en piezas de 3-15 ms y el ultimo tramo empezaria
    // tarde, dejando audible la cola del soplo.
    const int minGapSamples = std::max(
        1, static_cast<int>(std::lround(sampleRate * kSilenceMinMs / 1000.0)));
    int frame = 0;
    while (frame < numFrames) {
        if (!frameIsSilent(frame)) {
            ++frame;
            continue;
        }
        const int runStart = frame;
        while (frame < numFrames && frameIsSilent(frame))
            ++frame;

        SilenceRun candidate;
        candidate.startSample = fromSample + runStart * hop;
        candidate.endSample = fromSample + (frame - 1) * hop + window;
        if (last && candidate.startSample - last->endSample < minGapSamples)
            last->endSample = candidate.endSample;
        else
            last = candidate;
    }
    if (last && last->getNumSamples() >= minGapSamples)
        return last;
    return std::nullopt;
}

bool canChangeEngineAfter(const Note &previous, const Note &note,
                          const juce::AudioBuffer<float> &source,
                          int sampleRate, int hopSize) {
    if (hopSize <= 0)
        return false;
    // D-262 (T8): the silence that decides the engine is measured on the
    // SOURCE coordinates. A timing edit moves the note in the timeline but
    // does not change the audio that separates the two source spans, so moving
    // a boundary across an engine limit must not change the engine on its own.
    const int sourceGapStart = previous.getSrcEndFrame() * hopSize;
    const int sourceGapEnd = note.getSrcStartFrame() * hopSize;
    if (sourceGapEnd <= sourceGapStart)
        return false;
    if (static_cast<double>(sourceGapEnd - sourceGapStart) * 1000.0 /
            sampleRate <
        kSilenceMinMs)
        return false;
    // The notes must still be separated in the timeline: an engine boundary
    // is only usable where the output has somewhere to splice.
    if (note.getStartFrame() <= previous.getEndFrame())
        return false;
    return findSilenceRun(source, sampleRate, sourceGapStart, sourceGapEnd)
        .has_value();
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
    SynthesisEngineType blockEngine = current.engine;
    for (size_t i = 1; i < range.size(); ++i) {
        const Note &note = *range[i];
        const auto requested = effectiveEngine(note, globalEngine);
        if (requested == blockEngine) {
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
        // D-262 (T8): locate the silence in the SOURCE span, then map its
        // midpoint into the timeline gap (they are identical when the notes
        // were not retimed).
        const int sourceGapStart = previous->getSrcEndFrame() * hopSize;
        const int sourceGapEnd = note.getSrcStartFrame() * hopSize;
        const auto run =
            findSilenceRun(source, sampleRate, sourceGapStart, sourceGapEnd);
        int boundaryFrame = (previous->getEndFrame() + note.getStartFrame()) / 2;
        if (run && sourceGapEnd > sourceGapStart) {
            const int midSample = (run->startSample + run->endSample) / 2;
            const double proportion =
                static_cast<double>(midSample - sourceGapStart) /
                static_cast<double>(sourceGapEnd - sourceGapStart);
            boundaryFrame = static_cast<int>(std::lround(
                static_cast<double>(gapStart) +
                proportion * static_cast<double>(gapEnd - gapStart)));
            if (hopSize > 0)
                boundaryFrame = static_cast<int>(std::lround(
                    static_cast<double>(boundaryFrame) / hopSize));
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
        blockEngine = requested;
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
        if (note.isRest())
            continue;
        const auto stored = effectiveEngine(note, globalEngine);
        if (!haveBlock) {
            blockEngine = stored;
            haveBlock = true;
        }

        const bool selected = isSelected(index);
        const auto wanted = selected ? requestedEngine : stored;
        NoteEngine next = selected ? requested : note.getEngine();
        if (wanted == blockEngine) {
            // Same engine as the current block, regardless of whether the
            // stored value is explicit or Inherit.
            if (selected)
                next = requested;
        } else if (previous == nullptr ||
                   canChangeEngineAfter(*previous, note, source, sampleRate,
                                        hopSize)) {
            blockEngine = wanted;
        } else {
            // The engine request cannot start a new block here. A selected
            // note is refused and stored as the engine it will actually use;
            // an unselected note simply inherits the current block.
            if (selected) {
                ++plan.refused;
                plan.refusals.push_back({index, requested,
                                         EngineRefusalReason::NoSilence});
                next = blockEngine == globalEngine
                           ? NoteEngine::Inherit
                           : noteEngineFromSynthesisEngine(blockEngine);
            }
        }

        // The stored choice matters even when its current sound equals the
        // global engine: choosing Inherit must actually remove an override,
        // and a refused request must store the engine the block will use.
        if (selected && next != note.getEngine())
            plan.edits.push_back({index, note.getEngine(), next});
        previous = &note;
    }

    return plan;
}

BlockEngineEdit planBlockEngineEdit(const std::vector<Note> &notes,
                                    int anchorIndex, NoteEngine requested,
                                    SynthesisEngineType globalEngine,
                                    const juce::AudioBuffer<float> &source,
                                    int sampleRate, int hopSize,
                                    float benefitMinSemitones,
                                    float benefitMaxSemitones) {
    BlockEngineEdit plan;
    if (anchorIndex < 0 || anchorIndex >= static_cast<int>(notes.size()))
        return plan;
    const auto &anchor = notes[static_cast<size_t>(anchorIndex)];
    if (anchor.isRest())
        return plan;

    int lastEnd = 0;
    for (const auto &note : notes)
        if (!note.isRest())
            lastEnd = std::max(lastEnd, note.getEndFrame());
    if (lastEnd <= 0)
        return plan;
    (void)lastEnd;

    // D-258: the unit is the silence-delimited phrase that contains the anchor
    // (D-129), not the engine run: two phrases with the same engine share an
    // engine block, and choosing an engine may not reach across silence.
    std::vector<size_t> order;
    order.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); ++i)
        if (!notes[i].isRest())
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return notes[a].getStartFrame() < notes[b].getStartFrame();
    });
    auto position = order.size();
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == static_cast<size_t>(anchorIndex)) {
            position = i;
            break;
        }
    if (position >= order.size())
        return plan;

    size_t first = position;
    size_t last = position;
    while (first > 0 &&
           !canChangeEngineAfter(notes[order[first - 1]], notes[order[first]],
                                 source, sampleRate, hopSize))
        --first;
    while (last + 1 < order.size() &&
           !canChangeEngineAfter(notes[order[last]], notes[order[last + 1]],
                                 source, sampleRate, hopSize))
        ++last;

    SynthesisEngineType wanted = globalEngine;
    if (const auto mapped = noteEngineToSynthesisEngine(requested))
        wanted = *mapped;

    for (size_t i = first; i <= last; ++i) {
        const int index = static_cast<int>(order[i]);
        const auto &note = notes[static_cast<size_t>(index)];

        plan.blockNotes.push_back(index);
        const auto before = effectiveEngine(note, globalEngine);
        if (before != wanted) {
            ++plan.overwritten;
            plan.changesSound = true;
        }
        if (note.getEngine() != requested)
            plan.edits.push_back({index, note.getEngine(), requested});

        const float shift = note.getAdjustedMidiNote() - note.getOriginalMidiNote();
        if (shift < benefitMinSemitones || shift > benefitMaxSemitones)
            plan.outOfBenefitRange.push_back(index);
    }
    return plan;
}

int countOutOfBenefitRange(const std::vector<Note> &notes,
                           float benefitMinSemitones,
                           float benefitMaxSemitones) {
    int count = 0;
    for (const auto &note : notes) {
        if (note.isRest())
            continue;
        const float shift =
            note.getAdjustedMidiNote() - note.getOriginalMidiNote();
        if (shift < benefitMinSemitones || shift > benefitMaxSemitones)
            ++count;
    }
    return count;
}

std::vector<Block> blocksToCommit(const std::vector<Block> &plan,
                                  int dirtyStart, int dirtyEnd) {
    std::vector<Block> selected;
    if (plan.empty())
        return selected;

    const int first = std::min(dirtyStart, dirtyEnd);
    const int last = std::max(dirtyStart, dirtyEnd);
    const bool isPoint = (first == last);

    for (const auto &block : plan) {
        const bool intersects =
            isPoint ? (block.startFrame <= first && first < block.endFrame)
                    : (block.startFrame < last && block.endFrame > first);
        if (intersects)
            selected.push_back(block);
    }

    if (selected.empty()) {
        // The dirty range sits outside the plan (clamped override, a range
        // before the first block): commit the block that owns its start, or
        // the first one. Never return nothing when there is a plan to write.
        for (const auto &block : plan) {
            if (block.startFrame <= first && first < block.endFrame) {
                selected.push_back(block);
                break;
            }
        }
        if (selected.empty())
            selected.push_back(plan.front());
    }

    return selected;
}

std::vector<CommitRange>
commitRangesForNotes(const std::vector<Note> &notes,
                     const std::vector<Block> &plan, bool hasDirtyNoteAnchors,
                     int f0DirtyStart, int f0DirtyEnd, int renderStartFrame,
                     int renderEndFrame) {
    std::vector<CommitRange> ranges;

    auto addRange = [&](int start, int end) {
        if (end <= start)
            return;
        const bool duplicate = std::any_of(
            ranges.begin(), ranges.end(), [&](const CommitRange &range) {
                return range.startFrame == start && range.endFrame == end;
            });
        if (!duplicate)
            ranges.push_back({start, end});
    };

    // D-250: the commit unit is the note phrase inside the engine block, not
    // the whole block. An engine block only breaks where the engine changes, so
    // a phrase separated from the next one by silence lives in the SAME block;
    // committing the whole block rewrote the neighbouring phrase with a
    // different grain phase (S6: 0.6068 of difference in the block nobody
    // edited). Silence inside a block is still a phase boundary, so the commit
    // of an anchor stops at the middle of the silence around its phrase: the
    // splice stays inaudible and untouched phrases stay bit for bit.
    constexpr int kPhraseGapToleranceFrames = 1;
    std::vector<size_t> sortedNotes;
    sortedNotes.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); ++i)
        // D-299: la lapida eliminada sigue siendo un ancla de commit, aunque
        // sea un rest; el silencio se aplica al final del pase.
        if (!notes[i].isRest() || notes[i].isDeleted())
            sortedNotes.push_back(i);
    std::stable_sort(sortedNotes.begin(), sortedNotes.end(),
                     [&](size_t a, size_t b) {
                         return notes[a].getStartFrame() <
                                notes[b].getStartFrame();
                     });
    auto sortedPosition = [&](size_t noteIndex) {
        for (size_t i = 0; i < sortedNotes.size(); ++i)
            if (sortedNotes[i] == noteIndex)
                return i;
        return sortedNotes.size();
    };

    // Commits the phrase that owns @p noteIndex, clipped to its engine block.
    auto addPhraseFor = [&](size_t noteIndex) {
        const auto &note = notes[noteIndex];
        const auto blocks = blocksToCommit(plan, note.getStartFrame(),
                                           note.getEndFrame());
        if (blocks.empty())
            return;
        const Block *block = &blocks.front();
        for (const auto &candidate : blocks) {
            if (candidate.startFrame <= note.getStartFrame() &&
                note.getStartFrame() < candidate.endFrame) {
                block = &candidate;
                break;
            }
        }

        size_t position = sortedPosition(noteIndex);
        if (position >= sortedNotes.size())
            return;
        size_t first = position;
        size_t last = position;
        while (first > 0) {
            const auto &previous = notes[sortedNotes[first - 1]];
            const auto &current = notes[sortedNotes[first]];
            if (current.getStartFrame() - previous.getEndFrame() >
                kPhraseGapToleranceFrames)
                break;
            --first;
        }
        while (last + 1 < sortedNotes.size()) {
            const auto &current = notes[sortedNotes[last]];
            const auto &next = notes[sortedNotes[last + 1]];
            if (next.getStartFrame() - current.getEndFrame() >
                kPhraseGapToleranceFrames)
                break;
            ++last;
        }

        int start = notes[sortedNotes[first]].getStartFrame();
        int end = notes[sortedNotes[last]].getEndFrame();
        if (first == 0 ||
            notes[sortedNotes[first - 1]].getEndFrame() <= block->startFrame) {
            start = block->startFrame;
        } else {
            start = std::max(
                block->startFrame,
                (notes[sortedNotes[first - 1]].getEndFrame() + start) / 2);
        }
        if (last + 1 >= sortedNotes.size() ||
            notes[sortedNotes[last + 1]].getStartFrame() >= block->endFrame) {
            end = block->endFrame;
        } else {
            end = std::min(
                block->endFrame,
                (end + notes[sortedNotes[last + 1]].getStartFrame()) / 2);
        }
        addRange(start, end);
    };

    if (hasDirtyNoteAnchors) {
        for (size_t index = 0; index < notes.size(); ++index) {
            const auto &note = notes[index];
            if ((note.isRest() && !note.isDeleted()) || !note.isDirty())
                continue;
            if (note.getEndFrame() <= renderStartFrame ||
                note.getStartFrame() >= renderEndFrame)
                continue;
            addPhraseFor(index);
        }
    }
    if (!hasDirtyNoteAnchors && f0DirtyStart >= 0 && f0DirtyEnd > f0DirtyStart) {
        for (const auto &block :
             blocksToCommit(plan, f0DirtyStart, f0DirtyEnd))
            addRange(block.startFrame, block.endFrame);
    }

    std::sort(ranges.begin(), ranges.end(),
              [](const CommitRange &a, const CommitRange &b) {
                  return a.startFrame < b.startFrame;
              });
    return ranges;
}

}  // namespace engineBlocks
