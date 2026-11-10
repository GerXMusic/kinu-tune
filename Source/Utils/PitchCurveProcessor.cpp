#include "PitchCurveProcessor.h"
#include "BasePitchCurve.h"
#include "CurveResampler.h"
#include "PitchToolOperations.h"
#include "../Utils/Constants.h"
#include <algorithm>
#include <cmath>

namespace
{
    inline float safeFreqToMidi(float freq)
    {
        if (freq <= 0.0f)
            return 0.0f;
        return freqToMidi(freq);
    }

    void ensureSizes(AudioData& audioData, int totalFrames)
    {
        if (totalFrames <= 0)
            return;

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        if (audioData.deltaPitch.size() != static_cast<size_t>(totalFrames))
            audioData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);
    }

    const std::vector<float>& sourceDeltaFor(const Note& note)
    {
        return note.getActiveDeltaPitch();
    }

    PitchToolOperations::AdjacentNoteContext buildAdjacentContext(
        const std::vector<Note>& allNotes, const Note& targetNote)
    {
        PitchToolOperations::AdjacentNoteContext ctx;

        const Note* prevNote = nullptr;
        int prevNoteEndFrame = -1;
        for (const auto& candidate : allNotes) {
            if (&candidate == &targetNote || candidate.isRest())
                continue;
            const int candidateEnd = candidate.getEndFrame();
            if (candidateEnd <= targetNote.getStartFrame() && candidateEnd > prevNoteEndFrame) {
                prevNote = &candidate;
                prevNoteEndFrame = candidateEnd;
            }
        }

        const Note* nextNote = nullptr;
        int nextNoteStartFrame = std::numeric_limits<int>::max();
        for (const auto& candidate : allNotes) {
            if (&candidate == &targetNote || candidate.isRest())
                continue;
            const int candidateStart = candidate.getStartFrame();
            if (candidateStart >= targetNote.getEndFrame() && candidateStart < nextNoteStartFrame) {
                nextNote = &candidate;
                nextNoteStartFrame = candidateStart;
            }
        }

        if (prevNote) {
            const auto& prevDelta = sourceDeltaFor(*prevNote);
            if (!prevDelta.empty()) {
                ctx.hasLeft = true;
                ctx.leftBoundaryDelta = prevDelta.back();
            }
        }
        if (nextNote) {
            const auto& nextDelta = sourceDeltaFor(*nextNote);
            if (!nextDelta.empty()) {
                ctx.hasRight = true;
                ctx.rightBoundaryDelta = nextDelta.front();
            }
        }

        return ctx;
    }

    std::vector<BasePitchCurve::NoteSegment> collectNoteSegments(const std::vector<Note>& notes)
    {
        std::vector<BasePitchCurve::NoteSegment> segments;
        segments.reserve(notes.size());

        for (const auto& note : notes)
        {
            if (note.isRest())
                continue;

            BasePitchCurve::NoteSegment seg;
            seg.startFrame = note.getStartFrame();
            seg.endFrame = note.getEndFrame();
            // Base pitch already includes per-note offset
            seg.midiNote = note.getMidiNote() + note.getPitchOffset()
                         - (note.getTiltLeft() + note.getTiltRight()) / 2.0f;
            // D-182: how far this note was moved from where it was sung.
            seg.shiftSemitones =
                note.getAdjustedMidiNote() - note.getOriginalMidiNote();
            segments.push_back(seg);
        }

        // Ensure segments are sorted by start frame for stable generation
        std::sort(segments.begin(), segments.end(),
                  [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
        return segments;
    }

    // D-182: the jump the user hears when a melody is inverted is born in the
    // target curve, not in the engine. The base pitch is a smoothed step and
    // the stored delta is relative to the *original* base; moving one side of
    // a boundary leaves the other side's delta carrying the old 80 ms step,
    // so widening the base alone only removes part of it. The fix smooths the
    // shift itself (target - source), which is the quantity an edit changes,
    // and then writes it back into the global delta. The source contour is not
    // touched: target = source + smooth(shift), which is the same model as the
    // core's buildPitchCurve() and preserves D-053.
    // Chosen by measurement (D-182): 0.24 s removed 49 % of the boundary
    // step but left one extra detector hit at the 6477 frontier; 0.36 s is
    // the next measured value and is compared in the same run.
    constexpr double kEditedBoundaryGlideSec = 0.36;
    // 60 cents: below that the difference is intonation, not the edit the
    // user made. It keeps boundaries like +12/+46 and +6/-48 cents out of the
    // glide; the measured 54-cent boundary was the only one that got worse.
    constexpr float kEditedBoundaryMinShiftDeltaSemitones = 0.60f;

    // D-189: the D-182 glide is NOT adopted (negative listening, 2026-11-06),
    // so it must be off by default. It stays in the code behind this switch;
    // the tests enable it explicitly to attribute behaviour.
    bool gEditedBoundaryGlideEnabled = false;
    // A gap is an onset, not a legato note frontier. The source contour there
    // is the singer's attack and must not be smoothed away (D-053); only
    // contiguous notes get the glide.
    constexpr int kEditedBoundaryMaxGapFrames = 1;

    std::vector<int> collectEditedBoundaryFrames(const std::vector<Note>& notes)
    {
        std::vector<const Note*> ordered;
        ordered.reserve(notes.size());
        for (const auto& note : notes)
            if (!note.isRest())
                ordered.push_back(&note);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Note* a, const Note* b) {
                      if (a->getStartFrame() != b->getStartFrame())
                          return a->getStartFrame() < b->getStartFrame();
                      return a->getEndFrame() < b->getEndFrame();
                  });

        std::vector<int> boundaries;
        for (size_t i = 1; i < ordered.size(); ++i)
        {
            const int gap = ordered[i]->getStartFrame() -
                            ordered[i - 1]->getEndFrame();
            if (gap > kEditedBoundaryMaxGapFrames)
                continue;
            const float left = ordered[i - 1]->getAdjustedMidiNote() -
                               ordered[i - 1]->getOriginalMidiNote();
            const float right = ordered[i]->getAdjustedMidiNote() -
                                ordered[i]->getOriginalMidiNote();
            const bool anyEdited =
                std::abs(left) > 0.001f || std::abs(right) > 0.001f;
            if (anyEdited &&
                std::abs(right - left) >=
                    kEditedBoundaryMinShiftDeltaSemitones)
                boundaries.push_back(
                    (ordered[i - 1]->getEndFrame() +
                     ordered[i]->getStartFrame()) / 2);
        }
        return boundaries;
    }

    // D-205: the base is smoothed (80 ms cosine) but the delta is composed note
    // by note, so two adjacent notes whose stored deltas do not meet leave a
    // step in the composed f0. Replace that step with the same cosine ramp
    // centred on the join. Only joins with an audible delta step are touched
    // (D-182 uses the same 60-cent criterion), so a continuous analysis delta
    // stays bit-identical and the D-182 glide is not modified.
    constexpr double kDeltaJoinWindowSec = 0.080;
    constexpr float kDeltaJoinMinStepCents = 60.0f;

    void applyDeltaJoinContinuity(Project& project, int totalFrames,
                                  int rangeStart = 0, int rangeEnd = -1)
    {
        auto& audioData = project.getAudioData();
        if (totalFrames <= 0 ||
            static_cast<int>(audioData.deltaPitch.size()) < totalFrames)
            return;

        std::vector<const Note*> ordered;
        ordered.reserve(project.getNotes().size());
        for (const auto& note : project.getNotes())
            if (!note.isRest())
                ordered.push_back(&note);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Note* a, const Note* b) {
                      if (a->getStartFrame() != b->getStartFrame())
                          return a->getStartFrame() < b->getStartFrame();
                      return a->getEndFrame() < b->getEndFrame();
                  });

        struct Join {
            int boundary;
            float step;
        };
        std::vector<Join> joins;
        for (size_t i = 1; i < ordered.size(); ++i)
        {
            if (ordered[i]->getStartFrame() != ordered[i - 1]->getEndFrame())
                continue; // a gap is an onset, not a legato join (D-053)
            const int boundary = ordered[i]->getStartFrame();
            if (boundary <= 0 || boundary >= totalFrames)
                continue;
            if (rangeEnd >= 0 && (boundary < rangeStart || boundary >= rangeEnd))
                continue;
            const float step =
                audioData.deltaPitch[static_cast<size_t>(boundary)] -
                audioData.deltaPitch[static_cast<size_t>(boundary - 1)];
            if (std::abs(step) * 100.0f < kDeltaJoinMinStepCents)
                continue;
            joins.push_back({boundary, step});
        }
        if (joins.empty())
            return;

        const double hopSec =
            static_cast<double>(HOP_SIZE) /
            static_cast<double>(std::max(1, audioData.sampleRate));
        const int halfFrames = std::max(
            1, static_cast<int>(std::lround(0.5 * kDeltaJoinWindowSec / hopSec)));

        for (const auto& join : joins)
        {
            const int first = std::max(0, join.boundary - halfFrames);
            const int last = std::min(totalFrames - 1, join.boundary + halfFrames);
            const double span = static_cast<double>(2 * halfFrames);
            for (int frame = first; frame <= last; ++frame)
            {
                const double t = std::clamp(
                    static_cast<double>(frame - (join.boundary - halfFrames)) / span,
                    0.0, 1.0);
                const double w = 0.5 * (1.0 - std::cos(M_PI * t));
                auto& value = audioData.deltaPitch[static_cast<size_t>(frame)];
                if (frame < join.boundary)
                    value += static_cast<float>(join.step * w);
                else
                    value -= static_cast<float>(join.step * (1.0 - w));
            }
        }
    }

    // D-189: rangeStart/rangeEnd limit which boundaries are re-smoothed. The
    // partial rebuild paths pass their padded window so a preview never
    // re-smooths the whole project, which made the drag path-dependent.
    void applyEditedBoundaryGlide(Project& project, int totalFrames,
                                  int rangeStart = 0, int rangeEnd = -1)
    {
        if (!gEditedBoundaryGlideEnabled)
            return;
        auto& audioData = project.getAudioData();
        if (totalFrames <= 0 ||
            static_cast<int>(audioData.basePitch.size()) < totalFrames ||
            static_cast<int>(audioData.deltaPitch.size()) < totalFrames ||
            static_cast<int>(audioData.denseF0.size()) < totalFrames)
            return;

        auto boundaries = collectEditedBoundaryFrames(project.getNotes());
        if (rangeEnd >= 0)
            boundaries.erase(
                std::remove_if(boundaries.begin(), boundaries.end(),
                               [rangeStart, rangeEnd](int boundary) {
                                   return boundary < rangeStart ||
                                          boundary >= rangeEnd;
                               }),
                boundaries.end());
        if (boundaries.empty())
            return;

        const double hopSec =
            static_cast<double>(HOP_SIZE) /
            static_cast<double>(std::max(1, audioData.sampleRate));
        const int halfFrames = std::max(
            1, static_cast<int>(std::lround(0.5 * kEditedBoundaryGlideSec /
                                            hopSec)));

        std::vector<float> shift(static_cast<size_t>(totalFrames), 0.0f);
        std::vector<uint8_t> valid(static_cast<size_t>(totalFrames), 0);
        for (int frame = 0; frame < totalFrames; ++frame)
        {
            const float sourceHz = audioData.denseF0[static_cast<size_t>(frame)];
            const float base = audioData.basePitch[static_cast<size_t>(frame)];
            if (sourceHz <= 0.0f || base <= 0.0f)
                continue;
            const float target =
                base + audioData.deltaPitch[static_cast<size_t>(frame)];
            shift[static_cast<size_t>(frame)] = target - freqToMidi(sourceHz);
            valid[static_cast<size_t>(frame)] = 1;
        }

        // Cosine kernel over the glide width.
        std::vector<double> kernel(static_cast<size_t>(2 * halfFrames + 1), 0.0);
        double kernelSum = 0.0;
        for (int j = -halfFrames; j <= halfFrames; ++j)
        {
            const double x =
                M_PI * static_cast<double>(j) /
                static_cast<double>(halfFrames + 1);
            const double w = 0.5 * (1.0 + std::cos(x));
            kernel[static_cast<size_t>(j + halfFrames)] = w;
            kernelSum += w;
        }
        if (kernelSum <= 0.0)
            return;
        for (auto& w : kernel)
            w /= kernelSum;

        // Wide shift at every frame, then blend it in only around boundaries
        // where an edit changed the shift.
        std::vector<float> wide(static_cast<size_t>(totalFrames), 0.0f);
        for (int frame = 0; frame < totalFrames; ++frame)
        {
            if (!valid[static_cast<size_t>(frame)])
                continue;
            double acc = 0.0;
            double weight = 0.0;
            for (int j = -halfFrames; j <= halfFrames; ++j)
            {
                const int idx = std::clamp(frame + j, 0, totalFrames - 1);
                if (!valid[static_cast<size_t>(idx)])
                    continue;
                const double w = kernel[static_cast<size_t>(j + halfFrames)];
                acc += shift[static_cast<size_t>(idx)] * w;
                weight += w;
            }
            if (weight > 0.0)
                wide[static_cast<size_t>(frame)] =
                    static_cast<float>(acc / weight);
        }

        std::vector<float> blend(static_cast<size_t>(totalFrames), 0.0f);
        for (const int boundary : boundaries)
        {
            const int first = std::max(0, boundary - halfFrames);
            const int last = std::min(totalFrames - 1, boundary + halfFrames);
            for (int frame = first; frame <= last; ++frame)
            {
                const double distance =
                    std::fabs(static_cast<double>(frame - boundary)) /
                    static_cast<double>(halfFrames);
                const float w = static_cast<float>(
                    0.5 * (1.0 + std::cos(
                                     M_PI * std::min(1.0, distance))));
                blend[static_cast<size_t>(frame)] =
                    std::max(blend[static_cast<size_t>(frame)], w);
            }
        }

        for (int frame = 0; frame < totalFrames; ++frame)
        {
            const float w = blend[static_cast<size_t>(frame)];
            if (w <= 0.0f || !valid[static_cast<size_t>(frame)])
                continue;
            const float sourceMidi =
                freqToMidi(audioData.denseF0[static_cast<size_t>(frame)]);
            const float smoothed =
                shift[static_cast<size_t>(frame)] +
                w * (wide[static_cast<size_t>(frame)] -
                     shift[static_cast<size_t>(frame)]);
            const float target = sourceMidi + smoothed;
            audioData.deltaPitch[static_cast<size_t>(frame)] =
                target - audioData.basePitch[static_cast<size_t>(frame)];
        }
    }
} // namespace

namespace PitchCurveProcessor
{
    std::vector<float> interpolateWithUvMask(const std::vector<float>& pitchHz,
                                             const std::vector<bool>& uvMask)
    {
        if (pitchHz.empty())
            return {};

        const int n = static_cast<int>(pitchHz.size());
        std::vector<float> dense(pitchHz);

        auto isVoicedFrame = [&](int i) -> bool {
            if (i < 0 || i >= n)
                return false;

            const bool hasPitch = pitchHz[static_cast<size_t>(i)] > 0.0f;
            if (!hasPitch)
                return false;

            // If uvMask is provided, respect it (out-of-range treated as unvoiced).
            // If uvMask is missing, fall back to pitch presence.
            if (!uvMask.empty())
                return i < static_cast<int>(uvMask.size()) && uvMask[static_cast<size_t>(i)];
            return true;
        };

        int nextVoiced = -1;
        auto findNext = [&](int idx) -> int {
            for (int i = idx; i < n; ++i)
            {
                if (isVoicedFrame(i))
                    return i;
            }
            return -1;
        };

        int lastVoiced = -1;
        nextVoiced = findNext(0);

        for (int i = 0; i < n; ++i)
        {
            const bool voiced = isVoicedFrame(i);
            if (voiced)
            {
                lastVoiced = i;
                if (i == nextVoiced)
                    nextVoiced = findNext(i + 1);
                continue;
            }

            // Update next voiced lazily
            if (nextVoiced != -1 && nextVoiced < i)
                nextVoiced = findNext(i + 1);

            float prevVal = (lastVoiced >= 0) ? dense[static_cast<size_t>(lastVoiced)] : 0.0f;
            float nextVal = (nextVoiced >= 0) ? dense[static_cast<size_t>(nextVoiced)] : 0.0f;

            if (prevVal <= 0.0f && nextVal <= 0.0f)
            {
                dense[static_cast<size_t>(i)] = 0.0f;
                continue;
            }

            if (prevVal <= 0.0f)
            {
                dense[static_cast<size_t>(i)] = nextVal;
                continue;
            }
            if (nextVal <= 0.0f)
            {
                dense[static_cast<size_t>(i)] = prevVal;
                continue;
            }

            const float t = (nextVoiced > i) ? static_cast<float>(i - lastVoiced) /
                                               static_cast<float>(nextVoiced - lastVoiced)
                                             : 0.0f;
            const float logA = std::log(prevVal);
            const float logB = std::log(nextVal);
            dense[static_cast<size_t>(i)] = std::exp(logA * (1.0f - t) + logB * t);
        }

        return dense;
    }

    void rebuildCurvesFromSource(Project& project,
                                 const std::vector<float>& sourcePitchHz)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = static_cast<int>(sourcePitchHz.size());
        ensureSizes(audioData, totalFrames);

        // Ensure delta is built from a dense F0 source that includes UV
        // head/tail fill (same behavior expectation as F0 interpolation).
        std::vector<float> denseSource = sourcePitchHz;
        if (!audioData.voicedMask.empty() &&
            audioData.voicedMask.size() == sourcePitchHz.size())
        {
            denseSource = interpolateWithUvMask(sourcePitchHz, audioData.voicedMask);
        }

        auto segments = collectNoteSegments(project.getNotes());
        if (!segments.empty())
        {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
        {
            // Fallback: derive base from source pitch directly
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
            for (int i = 0; i < totalFrames; ++i)
                audioData.basePitch[static_cast<size_t>(i)] = safeFreqToMidi(denseSource[i]);
        }

        // Dense delta: midi(source) - base
        audioData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        for (int i = 0; i < totalFrames; ++i)
        {
            const float base = audioData.basePitch[static_cast<size_t>(i)];
            const float midi = safeFreqToMidi(denseSource[i]);
            audioData.deltaPitch[static_cast<size_t>(i)] = midi - base;
        }

        // Initialize originalDeltaPitch in each note from computed deltaPitch
        // This preserves the pristine pitch curve for non-destructive transformations
        for (auto& note : project.getNotes())
        {
            if (note.isRest()) continue;

            const int startFrame = note.getStartFrame();
            const int endFrame = note.getEndFrame();
            const int numFrames = endFrame - startFrame;

            if (numFrames <= 0) continue;

            std::vector<float> origDelta(static_cast<size_t>(numFrames));
            for (int i = 0; i < numFrames; ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    origDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
            }
            note.setOriginalDeltaPitch(std::move(origDelta));
        }

        // D-205 is not applied here: this is the detection/initial-load path
        // and the unedited analysis curve stays bit-identical. The join
        // smoother runs when an edit rebuilds the target (rebuildBaseFromNotes
        // and the preview paths).

        // Cache base F0 (Hz) for backwards compatibility
        audioData.baseF0.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i)
            audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);

        composeF0InPlace(project, /*applyUvMask=*/false);
    }

    std::vector<float> getDriftPreviewResponse(const Project& project, const Note& note)
    {
        const int frames = note.getEndFrame() - note.getStartFrame();
        if (frames <= 0 || sourceDeltaFor(note).empty()) return {};
        auto source = CurveResampler::resampleLinear(sourceDeltaFor(note), frames);
        const auto context = buildAdjacentContext(project.getNotes(), note);
        auto zero = PitchToolOperations::applyAllTransformations(source,
            note.getTiltLeft(), note.getTiltRight(), note.getVibrato(),
            note.getSmoothLeftFrames(), note.getSmoothRightFrames(), context, 0.0f);
        auto one = PitchToolOperations::applyAllTransformations(source,
            note.getTiltLeft(), note.getTiltRight(), note.getVibrato(),
            note.getSmoothLeftFrames(), note.getSmoothRightFrames(), context, 1.0f);
        for (size_t i = 0; i < one.size(); ++i)
            one[i] = (one[i] - zero[i]) * note.getDeltaScale();
        return one;
    }

    void rebuildBaseFromNotes(Project& project)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = audioData.getNumFrames();
        ensureSizes(audioData, totalFrames);

        auto segments = collectNoteSegments(project.getNotes());
        if (!segments.empty())
        {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
        {
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        }

        // CRITICAL: Rebuild deltaPitch from Note objects NON-DESTRUCTIVELY
        // Clear global deltaPitch first
        audioData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        
        // Composite each note's deltaPitch curve (WITH transformations applied) into audioData.deltaPitch
        const auto& allNotes = project.getNotes();
        for (const auto& note : allNotes)
        {
            if (note.isRest())
                continue;
                
            // A committed baked contour overrides the immutable analysis
            // contour. Legacy projects may only have deltaPitch.
            const auto& rawSourceData = sourceDeltaFor(note);
            if (rawSourceData.empty())
                continue;

            const int startFrame = note.getStartFrame();
            const int endFrame = note.getEndFrame();
            const int numFrames = endFrame - startFrame;
            if (numFrames <= 0)
                continue;

            // Resample stored delta to match the current output frame count.
            std::vector<float> resampledBuf;
            const std::vector<float>* sourceDataPtr = &rawSourceData;
            if (static_cast<int>(rawSourceData.size()) != numFrames)
            {
                resampledBuf = CurveResampler::resampleLinear(rawSourceData, numFrames);
                sourceDataPtr = &resampledBuf;
            }
            const auto& sourceData = *sourceDataPtr;
            
            // Build adjacent note context
            auto adjacentContext = buildAdjacentContext(allNotes, note);
            
            // Apply all transformation parameters NON-DESTRUCTIVELY
            std::vector<float> transformedDelta = PitchToolOperations::applyAllTransformations(
                sourceData,
                note.getTiltLeft(),
                note.getTiltRight(),
                note.getVibrato(),
                note.getSmoothLeftFrames(),
                note.getSmoothRightFrames(),
                adjacentContext,
                note.getPitchDrift()
            );

            // Apply per-note delta scale/offset (from delta control handles)
            const float dScale = note.getDeltaScale();
            const float dOffset = note.getDeltaOffset();
            if (std::abs(dScale - 1.0f) > 0.0001f || std::abs(dOffset) > 0.0001f)
            {
                for (auto& v : transformedDelta)
                    v = v * dScale + dOffset;
            }
            
            for (int i = 0; i < numFrames && i < static_cast<int>(transformedDelta.size()); ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    audioData.deltaPitch[static_cast<size_t>(globalIdx)] = transformedDelta[static_cast<size_t>(i)];
            }
        }

        // D-205: make the composed delta continuous at contiguous joins.
        applyDeltaJoinContinuity(project, totalFrames);

        // D-182: smooth the edited note boundaries before caching F0.
        applyEditedBoundaryGlide(project, totalFrames);

        // Update cached baseF0
        audioData.baseF0.resize(static_cast<size_t>(totalFrames));
        for (int i = 0; i < totalFrames; ++i)
            audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);

        composeF0InPlace(project, /*applyUvMask=*/false);
    }

    void rebuildBaseFromNotesForDrag(Project& project, const std::vector<Note*>& affectedNotes)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = audioData.getNumFrames();
        ensureSizes(audioData, totalFrames);

        // 1. Regenerate basePitch from ALL notes (needed for correct pitch display)
        auto segments = collectNoteSegments(project.getNotes());
        if (!segments.empty())
        {
            audioData.basePitch = BasePitchCurve::generateForNotes(segments, totalFrames);
        }

        if (audioData.basePitch.size() != static_cast<size_t>(totalFrames))
        {
            audioData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
        }

        // 2. Only rebuild deltaPitch for the affected notes (not all notes)
        //    First, zero the affected range in the global deltaPitch array
        int minFrame = totalFrames;
        int maxFrame = 0;
        for (auto* note : affectedNotes)
        {
            if (!note || note->isRest())
                continue;
            minFrame = std::min(minFrame, note->getStartFrame());
            maxFrame = std::max(maxFrame, note->getEndFrame());
        }
        if (minFrame >= maxFrame)
            return;

        // D-183 R6: the D-182 glide writes into the transition window, which
        // reaches into the neighbouring notes. Rebuild every note that
        // overlaps that window, not only the dragged ones, or the next drag
        // leaves glided delta outside the zeroed range and the restore is not
        // exact.
        constexpr int kGlidePadFrames = 32;
        minFrame = std::max(0, minFrame - kGlidePadFrames);
        maxFrame = std::min(totalFrames, maxFrame + kGlidePadFrames);
        for (int i = minFrame; i < maxFrame; ++i)
            audioData.deltaPitch[static_cast<size_t>(i)] = 0.0f;

        auto& allNotes = project.getNotes();
        std::vector<Note*> notesToWrite;
        notesToWrite.reserve(allNotes.size());
        for (auto& candidate : allNotes)
        {
            if (candidate.isRest())
                continue;
            if (candidate.getEndFrame() <= minFrame ||
                candidate.getStartFrame() >= maxFrame)
                continue;
            notesToWrite.push_back(&candidate);
        }
        for (auto* note : notesToWrite)
        {
            if (!note || note->isRest())
                continue;

            const auto& rawSourceData = sourceDeltaFor(*note);
            if (rawSourceData.empty())
                continue;

            const int startFrame = note->getStartFrame();
            const int endFrame = note->getEndFrame();
            const int numFrames = endFrame - startFrame;
            if (numFrames <= 0)
                continue;

            std::vector<float> resampledBuf;
            const std::vector<float>* sourceDataPtr = &rawSourceData;
            if (static_cast<int>(rawSourceData.size()) != numFrames)
            {
                resampledBuf = CurveResampler::resampleLinear(rawSourceData, numFrames);
                sourceDataPtr = &resampledBuf;
            }
            const auto& sourceData = *sourceDataPtr;

            auto adjacentContext = buildAdjacentContext(allNotes, *note);

            std::vector<float> transformedDelta = PitchToolOperations::applyAllTransformations(
                sourceData,
                note->getTiltLeft(),
                note->getTiltRight(),
                note->getVibrato(),
                note->getSmoothLeftFrames(),
                note->getSmoothRightFrames(),
                adjacentContext,
                note->getPitchDrift()
            );

            const float dScale = note->getDeltaScale();
            const float dOffset = note->getDeltaOffset();
            if (std::abs(dScale - 1.0f) > 0.0001f || std::abs(dOffset) > 0.0001f)
            {
                for (auto& v : transformedDelta)
                    v = v * dScale + dOffset;
            }

            for (int i = 0; i < numFrames && i < static_cast<int>(transformedDelta.size()); ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    audioData.deltaPitch[static_cast<size_t>(globalIdx)] = transformedDelta[static_cast<size_t>(i)];
            }
        }

        // D-205: same join continuity inside the rebuilt window.
        applyDeltaJoinContinuity(project, totalFrames, minFrame, maxFrame);

        // D-182: smooth the edited note boundaries before caching F0. D-189:
        // limited to this padded window so the next drag step is exact.
        applyEditedBoundaryGlide(project, totalFrames, minFrame, maxFrame);

        // 3. Update baseF0 only for the affected range
        audioData.baseF0.resize(static_cast<size_t>(totalFrames));
        for (int i = minFrame; i < maxFrame; ++i)
            audioData.baseF0[static_cast<size_t>(i)] = midiToFreq(audioData.basePitch[static_cast<size_t>(i)]);

        // 4. Recompose f0 only for the affected range (with padding for smoothing)
        const int f0Start = std::max(0, minFrame - 60);
        const int f0End = std::min(totalFrames, maxFrame + 60);
        for (int i = f0Start; i < f0End; ++i)
        {
            const float base = audioData.basePitch[static_cast<size_t>(i)];
            const float delta = audioData.deltaPitch[static_cast<size_t>(i)];
            audioData.f0[static_cast<size_t>(i)] = midiToFreq(base + delta);
        }
    }

    void rebuildDeltaForNotes(Project& project, const std::vector<Note*>& affectedNotes)
    {
        auto& audioData = project.getAudioData();
        const int totalFrames = audioData.getNumFrames();
        if (totalFrames <= 0 || affectedNotes.empty())
            return;

        auto& allNotes = project.getNotes();
        int minAffectedFrame = totalFrames;
        int maxAffectedFrame = 0;

        for (auto* note : affectedNotes)
        {
            if (!note || note->isRest())
                continue;
            minAffectedFrame = std::min(minAffectedFrame, note->getStartFrame());
            maxAffectedFrame = std::max(maxAffectedFrame, note->getEndFrame());
        }
        if (minAffectedFrame >= maxAffectedFrame)
            return;

        // D-189: the D-182 glide reads the per-frame shift from deltaPitch and
        // writes into the neighbouring notes. This preview path used to refresh
        // only the edited notes, so a second preview smoothed already-smoothed
        // values: the drag drifted and the commit recalculated a different
        // curve. Rebuild every note that overlaps the glide window (D-183).
        constexpr int kGlidePadFrames = 32;
        minAffectedFrame = std::max(0, minAffectedFrame - kGlidePadFrames);
        maxAffectedFrame = std::min(totalFrames, maxAffectedFrame + kGlidePadFrames);
        for (int i = minAffectedFrame; i < maxAffectedFrame; ++i)
            audioData.deltaPitch[static_cast<size_t>(i)] = 0.0f;

        std::vector<Note*> notesToWrite;
        notesToWrite.reserve(allNotes.size());
        for (auto& candidate : allNotes)
        {
            if (candidate.isRest())
                continue;
            if (candidate.getEndFrame() <= minAffectedFrame ||
                candidate.getStartFrame() >= maxAffectedFrame)
                continue;
            notesToWrite.push_back(&candidate);
        }

        for (auto* note : notesToWrite)
        {
            if (!note || note->isRest())
                continue;

            const auto& rawSourceData = sourceDeltaFor(*note);
            if (rawSourceData.empty())
                continue;

            const int startFrame = note->getStartFrame();
            const int endFrame = note->getEndFrame();
            const int numFrames = endFrame - startFrame;
            if (numFrames <= 0)
                continue;

            // Resample stored delta to match the current output frame count.
            std::vector<float> resampledBuf;
            const std::vector<float>* sourceDataPtr = &rawSourceData;
            if (static_cast<int>(rawSourceData.size()) != numFrames)
            {
                resampledBuf = CurveResampler::resampleLinear(rawSourceData, numFrames);
                sourceDataPtr = &resampledBuf;
            }
            const auto& sourceData = *sourceDataPtr;

            // Build adjacent note context
            auto adjacentContext = buildAdjacentContext(allNotes, *note);

            std::vector<float> transformedDelta = PitchToolOperations::applyAllTransformations(
                sourceData,
                note->getTiltLeft(),
                note->getTiltRight(),
                note->getVibrato(),
                note->getSmoothLeftFrames(),
                note->getSmoothRightFrames(),
                adjacentContext,
                note->getPitchDrift()
            );

            // Apply per-note delta scale/offset
            const float dScale = note->getDeltaScale();
            const float dOffset = note->getDeltaOffset();
            if (std::abs(dScale - 1.0f) > 0.0001f || std::abs(dOffset) > 0.0001f)
            {
                for (auto& v : transformedDelta)
                    v = v * dScale + dOffset;
            }

            for (int i = 0; i < numFrames && i < static_cast<int>(transformedDelta.size()); ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    audioData.deltaPitch[static_cast<size_t>(globalIdx)] = transformedDelta[static_cast<size_t>(i)];
            }
        }

        // D-205: same join continuity inside the rebuilt window.
        applyDeltaJoinContinuity(project, totalFrames, minAffectedFrame,
                                 maxAffectedFrame);

        // D-182: an edited boundary can smooth the delta a little past the
        // affected notes, so recompose with padding. D-189 limits the glide to
        // this rebuilt window so the preview is idempotent.
        applyEditedBoundaryGlide(project, totalFrames, minAffectedFrame,
                                 maxAffectedFrame);

        // Recompose f0 only for affected range
        if (minAffectedFrame < maxAffectedFrame)
        {
            const int rangeStart = std::max(0, minAffectedFrame - 32);
            const int rangeEnd = std::min(totalFrames, maxAffectedFrame + 32);
            for (int i = rangeStart; i < rangeEnd; ++i)
            {
                const float base = audioData.basePitch[static_cast<size_t>(i)];
                const float delta = audioData.deltaPitch[static_cast<size_t>(i)];
                audioData.f0[static_cast<size_t>(i)] = midiToFreq(base + delta);
            }
        }
    }

    void setEditedBoundaryGlideEnabled(bool enabled)
    {
        gEditedBoundaryGlideEnabled = enabled;
    }

    std::vector<float> composeF0(const Project& project,
                                 bool applyUvMask,
                                 float globalPitchOffset)
    {
        const auto& audioData = project.getAudioData();
        const int totalFrames = static_cast<int>(audioData.basePitch.size());
        std::vector<float> result(static_cast<size_t>(totalFrames), 0.0f);

        for (int i = 0; i < totalFrames; ++i)
        {
            bool isVoiced = (i < static_cast<int>(audioData.voicedMask.size())) ? audioData.voicedMask[i] : true;
            if (applyUvMask && !isVoiced)
                continue;

            const float base = audioData.basePitch[static_cast<size_t>(i)];
            const float delta = (i < static_cast<int>(audioData.deltaPitch.size()))
                                    ? audioData.deltaPitch[static_cast<size_t>(i)]
                                    : 0.0f;
            const float midi = base + delta + globalPitchOffset;
            result[static_cast<size_t>(i)] = midiToFreq(midi);
        }

        return result;
    }

    void composeF0InPlace(Project& project,
                          bool applyUvMask,
                          float globalPitchOffset)
    {
        auto composed = composeF0(project, applyUvMask, globalPitchOffset);
        auto& audioData = project.getAudioData();
        audioData.f0 = std::move(composed);
    }
} // namespace PitchCurveProcessor
