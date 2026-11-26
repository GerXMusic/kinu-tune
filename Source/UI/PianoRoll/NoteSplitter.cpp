#include "NoteSplitter.h"
#include "../../Utils/NoteAmplitude.h"
#include "../../Utils/Constants.h"
#include "../../Utils/CurveResampler.h"
#include "../../Utils/NotePitch.h"
#include <algorithm>
#include <cmath>

namespace
{
float calculatePitchCenter(const AudioData& audioData, int startFrame,
                           int endFrame, float fallback)
{
    const int f0Size = static_cast<int>(audioData.f0.size());
    const int start = std::clamp(startFrame, 0, f0Size);
    const int end = std::clamp(endFrame, start, f0Size);

    double midiSum = 0.0;
    int voicedCount = 0;
    for (int frame = start; frame < end; ++frame) {
        const bool voiced = audioData.voicedMask.empty() ||
                            (frame < static_cast<int>(audioData.voicedMask.size()) &&
                             audioData.voicedMask[static_cast<size_t>(frame)]);
        const float f0 = audioData.f0[static_cast<size_t>(frame)];
        if (voiced && f0 > 0.0f && std::isfinite(f0)) {
            midiSum += freqToMidi(f0);
            ++voicedCount;
        }
    }

    if (voicedCount > 0)
        return static_cast<float>(midiSum / voicedCount);

    // D-303: sin f0 (proyecto sin analisis cargado, o tramo entero sordo) la
    // curva compuesta tambien se puede leer de base + delta en el mismo rango.
    const int curveSize = static_cast<int>(
        std::min(audioData.basePitch.size(), audioData.deltaPitch.size()));
    const int curveStart = std::clamp(startFrame, 0, curveSize);
    const int curveEnd = std::clamp(endFrame, curveStart, curveSize);
    for (int frame = curveStart; frame < curveEnd; ++frame) {
        const float base = audioData.basePitch[static_cast<size_t>(frame)];
        if (base > 0.0f && std::isfinite(base)) {
            midiSum += base + audioData.deltaPitch[static_cast<size_t>(frame)];
            ++voicedCount;
        }
    }

    return voicedCount > 0
               ? static_cast<float>(midiSum / voicedCount)
               : fallback;
}
}

Note* NoteSplitter::findNoteAt(float x, float y) {
    if (!project || !coordMapper)
        return nullptr;

    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();

    for (auto& note : project->getNotes()) {
        if (note.isRest())
            continue;

        float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
        float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
        float noteY = coordMapper->midiToY(note.getAdjustedMidiNote());
        float noteH = pixelsPerSemitone;

        if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH) {
            return &note;
        }
    }

    return nullptr;
}

bool NoteSplitter::splitNoteAtFrame(Note* note, int splitFrame) {
    if (!note || !project)
        return false;

    int startFrame = note->getStartFrame();
    int endFrame = note->getEndFrame();

    // Keep at least one analysis frame on each side of the split.
    if (splitFrame <= startFrame || splitFrame >= endFrame)
        return false;

    // Store original note data for undo
    Note originalNote = *note;

    // D-303: el corte separa dos centros. El visible/editable de cada mitad
    // pasa a ser la media local de la curva blanca actual; el de audio (el que
    // construye la base del motor) se conserva en renderCenterOffset para que
    // el corte no mueva la afinacion. Se capturan ANTES de mutar la nota.
    const float parentEditCenter = note->getAdjustedMidiNote();
    const float parentSource = note->getLastNonMacroMidiNote();
    const float parentAudioCenter = note->getMidiNote() + note->getPitchOffset() +
                                    note->getRenderCenterOffset();
    const float parentOriginal = note->getOriginalMidiNote();
    const auto& audioForSplit = project->getAudioData();
    const float leftMean =
        calculatePitchCenter(audioForSplit, startFrame, splitFrame, parentEditCenter);
    const float rightMean =
        calculatePitchCenter(audioForSplit, splitFrame, endFrame, parentEditCenter);

    // D-308: a split used to rebuild each half's tilt ramp from its own new
    // duration, so an inclined note jumped by 2*tilt at the cut. The fix is to
    // commit the parent's transformed contour into `bakedDeltaPitch` and leave
    // both halves neutral (all three tilt layers = 0). The baked curve is the
    // parent's active delta (resampled to its frame count) plus the three
    // ramps evaluated over the parent's frames, split at the cut. The base is
    // the render centre, which both halves keep in renderCenterOffset, so the
    // composite curve (base + delta) is the same before and after the split.
    const int parentFrames = endFrame - startFrame;
    std::vector<float> parentCurve = note->getActiveDeltaPitch();
    if (!parentCurve.empty() &&
        static_cast<int>(parentCurve.size()) != parentFrames)
        parentCurve = CurveResampler::resampleLinear(parentCurve, parentFrames);
    if (!parentCurve.empty() && parentCurve.size() > 1)
    {
        const float parentTilt = note->getTilt();
        const float parentFineLeft = note->getTiltFineLeft();
        const float parentFineRight = note->getTiltFineRight();
        const float invLastIndex = 1.0f / static_cast<float>(parentCurve.size() - 1);
        for (size_t i = 0; i < parentCurve.size(); ++i)
        {
            const float p = static_cast<float>(i) * invLastIndex;
            parentCurve[i] += parentTilt * (1.0f - 2.0f * p);
            if (p < 0.5f)
                parentCurve[i] += parentFineLeft * (1.0f - 2.0f * p);
            else if (p > 0.5f)
                parentCurve[i] += parentFineRight * (2.0f * p - 1.0f);
        }
    }
    std::vector<float> leftBaked, rightBaked;
    if (!parentCurve.empty())
    {
        const int splitOffset =
            std::clamp(splitFrame - startFrame, 0,
                       static_cast<int>(parentCurve.size()));
        leftBaked.assign(parentCurve.begin(), parentCurve.begin() + splitOffset);
        rightBaked.assign(parentCurve.begin() + splitOffset, parentCurve.end());
    }

    // Create the second note (right part)
    Note secondNote;
    secondNote.setStartFrame(splitFrame);
    secondNote.setEndFrame(endFrame);
    const int srcStartFrame = note->getSrcStartFrame();
    const int srcEndFrame = note->getSrcEndFrame();
    const double splitRatio = static_cast<double>(splitFrame - startFrame) /
                              static_cast<double>(endFrame - startFrame);
    const int sourceDuration = srcEndFrame - srcStartFrame;
    const int sourceSplitCandidate =
        srcStartFrame + static_cast<int>(
                            std::lround(splitRatio * sourceDuration));
    const int srcSplitFrame = sourceDuration >= 2
                                  ? std::clamp(sourceSplitCandidate,
                                               srcStartFrame + 1,
                                               srcEndFrame - 1)
                                  : std::clamp(sourceSplitCandidate,
                                               srcStartFrame, srcEndFrame);
    secondNote.setSrcStartFrame(srcSplitFrame);
    secondNote.setSrcEndFrame(srcEndFrame);
    NotePitch::restore(secondNote, note->getMidiNote(), note->getLastNonMacroMidiNote());
    secondNote.setLyric(note->getLyric());
    secondNote.setPhoneme(note->getPhoneme());
    secondNote.setVolumeDb(note->getVolumeDb());
    secondNote.setSelected(note->isSelected());
    secondNote.setVibrato(note->getVibrato());
    secondNote.setPitchDrift(note->getPitchDrift());
    secondNote.setFormantShift(note->getFormantShift());
    secondNote.setSmoothLeftFrames(note->getSmoothLeftFrames());
    secondNote.setSmoothRightFrames(note->getSmoothRightFrames());
    secondNote.setTransitionOutMs(note->getTransitionOutMs());
    // D-225: the transition belongs to the union with the note on the right,
    // so the tail keeps the original value and the new inner union (first
    // part -> second part) starts at 0.
    note->setTransitionOutMs(0);
    secondNote.setDeltaScale(note->getDeltaScale());
    secondNote.setDeltaOffset(note->getDeltaOffset());
    secondNote.setPitchOffset(0.0f);

    // Split originalDeltaPitch if available.
    if (note->hasOriginalDeltaPitch()) {
        const auto& origDelta = note->getOriginalDeltaPitch();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(origDelta.size())));
        std::vector<float> leftDelta(origDelta.begin(), origDelta.begin() + splitOffset);
        std::vector<float> rightDelta(origDelta.begin() + splitOffset, origDelta.end());
        note->setOriginalDeltaPitch(std::move(leftDelta));
        secondNote.setOriginalDeltaPitch(std::move(rightDelta));
    } else {
        // Fallback: extract from global deltaPitch if originalDeltaPitch is missing
        auto& audioData = project->getAudioData();
        const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
        if (totalFrames > 0) {
            int leftLen = splitFrame - startFrame;
            int rightLen = endFrame - splitFrame;
            if (leftLen > 0) {
                std::vector<float> leftDelta(static_cast<size_t>(leftLen));
                for (int i = 0; i < leftLen; ++i) {
                    int gIdx = startFrame + i;
                    if (gIdx >= 0 && gIdx < totalFrames)
                        leftDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(gIdx)];
                }
                note->setOriginalDeltaPitch(std::move(leftDelta));
            }
            if (rightLen > 0) {
                std::vector<float> rightDelta(static_cast<size_t>(rightLen));
                for (int i = 0; i < rightLen; ++i) {
                    int gIdx = splitFrame + i;
                    if (gIdx >= 0 && gIdx < totalFrames)
                        rightDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(gIdx)];
                }
                secondNote.setOriginalDeltaPitch(std::move(rightDelta));
            }
        }
    }

    // Keep per-note pitch data aligned with the new frame ranges.
    if (note->hasDeltaPitch()) {
        const auto& delta = note->getDeltaPitch();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::clamp(splitOffset, 0, static_cast<int>(delta.size()));
        std::vector<float> leftDelta(delta.begin(), delta.begin() + splitOffset);
        std::vector<float> rightDelta(delta.begin() + splitOffset, delta.end());
        note->setDeltaPitch(std::move(leftDelta));
        secondNote.setDeltaPitch(std::move(rightDelta));
    }

    // D-308: the tilt contour is already committed in the baked delta above;
    // both halves stay neutral so no ramp is rebuilt from the new durations.
    if (!leftBaked.empty() || !rightBaked.empty())
    {
        note->setBakedDeltaPitch(std::move(leftBaked));
        secondNote.setBakedDeltaPitch(std::move(rightBaked));
    }
    note->setTilt(0.0f);
    note->setTiltFineLeft(0.0f);
    note->setTiltFineRight(0.0f);
    secondNote.setTilt(0.0f);
    secondNote.setTiltFineLeft(0.0f);
    secondNote.setTiltFineRight(0.0f);

    const auto& f0Values = note->getF0Values();
    if (!f0Values.empty()) {
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::clamp(splitOffset, 0, static_cast<int>(f0Values.size()));
        std::vector<float> leftF0(f0Values.begin(), f0Values.begin() + splitOffset);
        std::vector<float> rightF0(f0Values.begin() + splitOffset, f0Values.end());
        note->setF0Values(std::move(leftF0));
        secondNote.setF0Values(std::move(rightF0));
    }

    // D-303: cortar no mueve la afinacion, pero cada mitad vuelve a mostrar y a
    // editar su propia media local.
    //
    // El centro visible/editable de cada mitad pasa a ser la media de la curva
    // blanca actual en su tramo; la correccion (lastNonMacro - midi) se lleva
    // el mismo delta que el centro. El centro de AUDIO no cambia: se conserva en
    // renderCenterOffset como (centro original - media local), y la base que
    // construye el motor sigue saliendo del centro original. Las curvas de
    // desviacion se reparten tal cual, asi que la curva compuesta (base +
    // delta) queda identica y solo cambian los bordes.
    //
    // D-285 (2.3) no recentraba por esto: mover el centro de la base dejaba un
    // escalon de 14,6 c medido en SplitPitchNeutralityTests. Ese escalon ya no
    // puede ocurrir porque la base no depende del centro visible.
    const auto& audioData = project->getAudioData();
    NotePitch::restore(*note, leftMean, parentSource + (leftMean - parentEditCenter));
    note->setRenderCenterOffset(parentAudioCenter - leftMean);
    note->setPitchOffset(0.0f);
    NotePitch::restore(secondNote, rightMean,
                       parentSource + (rightMean - parentEditCenter));
    secondNote.setRenderCenterOffset(parentAudioCenter - rightMean);
    secondNote.setPitchOffset(0.0f);
    secondNote.setOriginalMidiNote(parentOriginal);

    // Modify the first note (left part)
    note->setEndFrame(splitFrame);
    note->setSrcEndFrame(srcSplitFrame);

    NoteAmplitude::update(*note, audioData);
    NoteAmplitude::update(secondNote, audioData);

    // Splitting is a structural edit, so both segments inherit the source
    // note's processing state rather than treating the new tail as clean.
    const bool sourceIsDirty = originalNote.isDirty();
    const bool sourceIsSynthDirty = originalNote.isSynthDirty();
    const bool sourceHasRenderedEdit = originalNote.hasRenderedEdit();
    note->setDirty(sourceIsDirty);
    note->setSynthDirty(sourceIsSynthDirty);
    note->setRenderedEdit(sourceHasRenderedEdit);
    secondNote.setDirty(sourceIsDirty);
    secondNote.setSynthDirty(sourceIsSynthDirty);
    secondNote.setRenderedEdit(sourceHasRenderedEdit);

    // Save first note BEFORE addNote (addNote may invalidate note pointer due to vector reallocation)
    Note firstNote = *note;

    // Add the second note to project
    project->addNote(secondNote);

    // Create undo action - don't pass callback to avoid lifetime issues
    // UI refresh is handled by UndoManager's onUndoRedo callback
    if (undoManager) {
        auto action = std::make_unique<NoteSplitAction>(
            project, originalNote, firstNote, secondNote, nullptr);
        undoManager->addAction(std::move(action));
    }

    if (onNoteSplit)
        onNoteSplit();

    return true;
}

bool NoteSplitter::splitNoteAtX(Note* note, float x) {
    if (!note || !coordMapper)
        return false;

    // Convert X coordinate to frame
    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    double time = x / pixelsPerSecond;
    int frame = static_cast<int>(time * SAMPLE_RATE / HOP_SIZE);

    return splitNoteAtFrame(note, frame);
}

std::pair<Note*, Note*> NoteSplitter::findMergeableNotesAt(float x, float y)
{
    if (!project || !coordMapper)
        return {};

    const auto &chunkRanges = project->getAudioData().segmentChunkRanges;
    const auto getRegionIndex = [&chunkRanges](const Note &note)
    {
        if (chunkRanges.empty())
            return 0;

        const int midpoint = note.getSrcStartFrame() +
                             std::max(0, note.getSrcDurationFrames()) / 2;
        for (size_t i = 0; i < chunkRanges.size(); ++i)
            if (midpoint >= chunkRanges[i].first && midpoint < chunkRanges[i].second)
                return static_cast<int>(i);
        return -1;
    };

    auto &notes = project->getNotes();
    const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
    constexpr float boundaryHitPadding = 5.0f;

    for (size_t i = 0; i < notes.size(); ++i)
    {
        auto &left = notes[i];
        if (left.isRest())
            continue;

        for (size_t j = i + 1; j < notes.size(); ++j)
        {
            auto &right = notes[j];
            if (right.isRest())
                continue;

            // Only expose merge on a true split: the output and source frame
            // ranges must remain continuous across the boundary.
            if (left.getEndFrame() != right.getStartFrame() ||
                left.getSrcEndFrame() != right.getSrcStartFrame() ||
                getRegionIndex(left) < 0 || getRegionIndex(left) != getRegionIndex(right))
                break;

            const float boundaryX = 0.5f * framesToSeconds(
                left.getEndFrame() + right.getStartFrame()) * pixelsPerSecond;
            const float lineTop = std::min(
                coordMapper->midiToY(left.getAdjustedMidiNote()),
                coordMapper->midiToY(right.getAdjustedMidiNote()));
            const float lineBottom = std::max(
                coordMapper->midiToY(left.getAdjustedMidiNote()),
                coordMapper->midiToY(right.getAdjustedMidiNote())) + pixelsPerSemitone;

            if (std::abs(x - boundaryX) <= boundaryHitPadding &&
                y >= lineTop && y <= lineBottom)
                return {&left, &right};

            break;
        }
    }

    return {};
}

bool NoteSplitter::mergeNotes(Note *first, Note *second)
{
    if (!project || !first || !second || first->isRest() || second->isRest() ||
        first->getEndFrame() != second->getStartFrame() ||
        first->getSrcEndFrame() != second->getSrcStartFrame())
        return false;

    const Note firstNote = *first;
    const Note secondNote = *second;
    Note mergedNote = firstNote;
    mergedNote.setEndFrame(secondNote.getEndFrame());
    mergedNote.setSrcEndFrame(secondNote.getSrcEndFrame());

    NoteAmplitude::update(mergedNote, project->getAudioData());

    auto append = [](auto left, const auto &right)
    {
        left.insert(left.end(), right.begin(), right.end());
        return left;
    };
    mergedNote.setF0Values(append(firstNote.getF0Values(), secondNote.getF0Values()));

    // Restore a single pitch center over the merged duration. Each half may
    // have been rebased when it was split, so convert its deviations through
    // the current absolute pitch before expressing them around the new center.
    //
    // D-303: los dos centros que importan son los de AUDIO (visible + offset).
    // Si las dos mitades comparten centro de audio vienen de un corte: base y
    // delta ya son comunes, asi que solo se concatenan (rebasarlos moveria la
    // curva) y la nota unida muestra la media local de la union. Si no lo
    // comparten se mantiene el camino de recalculo/rebase con los centros de
    // audio.
    const float firstAudio =
        firstNote.getAdjustedMidiNote() + firstNote.getRenderCenterOffset();
    const float secondAudio =
        secondNote.getAdjustedMidiNote() + secondNote.getRenderCenterOffset();
    const bool sameCenter = std::abs(firstAudio - secondAudio) <= 1.0e-4f;
    const float mergedPitchCenter =
        calculatePitchCenter(project->getAudioData(), mergedNote.getStartFrame(),
                             mergedNote.getEndFrame(), firstAudio);
    const auto mergeDeviation = [mergedPitchCenter](
                                  const std::vector<float> &firstValues,
                                  float firstCenter,
                                  const std::vector<float> &secondValues,
                                  float secondCenter)
    {
        std::vector<float> values;
        values.reserve(firstValues.size() + secondValues.size());
        for (const auto value : firstValues)
            values.push_back(value + firstCenter - mergedPitchCenter);
        for (const auto value : secondValues)
            values.push_back(value + secondCenter - mergedPitchCenter);
        return values;
    };
    if (sameCenter) {
        // Misma base: concatenar sin rebasar. Los valores ya son relativos al
        // mismo centro de audio.
        mergedNote.setOriginalDeltaPitch(
            append(firstNote.getOriginalDeltaPitch(),
                   secondNote.getOriginalDeltaPitch()));
        mergedNote.setDeltaPitch(
            append(firstNote.getDeltaPitch(), secondNote.getDeltaPitch()));
        if (firstNote.hasBakedDeltaPitch() || secondNote.hasBakedDeltaPitch()) {
            mergedNote.setBakedDeltaPitch(append(
                firstNote.getActiveDeltaPitch(), secondNote.getActiveDeltaPitch()));
        }
    } else {
        mergedNote.setOriginalDeltaPitch(mergeDeviation(
            firstNote.getOriginalDeltaPitch(), firstNote.getOriginalMidiNote(),
            secondNote.getOriginalDeltaPitch(), secondNote.getOriginalMidiNote()));
        mergedNote.setDeltaPitch(mergeDeviation(
            firstNote.getDeltaPitch(), firstAudio,
            secondNote.getDeltaPitch(), secondAudio));
        if (firstNote.hasBakedDeltaPitch() || secondNote.hasBakedDeltaPitch()) {
            const auto& firstActive = firstNote.hasBakedDeltaPitch()
                                          ? firstNote.getBakedDeltaPitch()
                                          : firstNote.getOriginalDeltaPitch();
            const auto& secondActive = secondNote.hasBakedDeltaPitch()
                                           ? secondNote.getBakedDeltaPitch()
                                           : secondNote.getOriginalDeltaPitch();
            mergedNote.setBakedDeltaPitch(mergeDeviation(
                firstActive, firstAudio, secondActive, secondAudio));
        }
    }
    if (sameCenter) {
        // D-303: la referencia de correccion se conserva ponderando por
        // duracion, porque cada mitad pudo recentrarse en su media local.
        const float firstFrames =
            static_cast<float>(std::max(1, firstNote.getDurationFrames()));
        const float secondFrames =
            static_cast<float>(std::max(1, secondNote.getDurationFrames()));
        const float mergedSource =
            (firstNote.getLastNonMacroMidiNote() * firstFrames +
             secondNote.getLastNonMacroMidiNote() * secondFrames) /
            (firstFrames + secondFrames);
        NotePitch::restore(mergedNote, mergedPitchCenter, mergedSource);
        mergedNote.setRenderCenterOffset(firstAudio - mergedPitchCenter);
        mergedNote.setOriginalMidiNote(firstNote.getOriginalMidiNote());
    } else {
        // Merging is pitch-neutral: keep the first segment's gap between the
        // Correct Pitch reference (the sung pitch) and the pitch.
        NotePitch::restore(mergedNote, mergedPitchCenter,
                           mergedPitchCenter + firstNote.getLastNonMacroMidiNote() -
                               firstNote.getMidiNote());
        mergedNote.setRenderCenterOffset(0.0f);
        mergedNote.setOriginalMidiNote(mergedPitchCenter);
    }
    mergedNote.setPitchOffset(0.0f);
    const bool isDirty = firstNote.isDirty() || secondNote.isDirty();
    const bool isSynthDirty = firstNote.isSynthDirty() || secondNote.isSynthDirty();
    mergedNote.setDirty(isDirty);
    mergedNote.setRenderedEdit(firstNote.hasRenderedEdit() ||
                               secondNote.hasRenderedEdit());
    if (isSynthDirty)
        mergedNote.markSynthDirty();
    else
        mergedNote.setSynthDirty(false);

    *first = mergedNote;
    project->removeNoteByStartFrame(secondNote.getStartFrame());

    if (undoManager)
        undoManager->addAction(std::make_unique<NoteMergeAction>(
            project, firstNote, secondNote, mergedNote, nullptr));

    if (onNoteSplit)
        onNoteSplit();

    return true;
}
