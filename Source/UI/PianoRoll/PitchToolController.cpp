#include "PitchToolController.h"
#include "../../Undo/AmplitudeAction.h"
#include "../../Utils/PitchCurveProcessor.h"
#include "../../Utils/ScaleUtils.h"
#include "../../Utils/NotePitch.h"

#include <algorithm>
#include <limits>

PitchToolController::PitchToolController()
{
}

float PitchToolController::getAmplitudePreviewGain(const Note& note) const
{
  if (!dragging || activeHandleType != PitchToolHandles::HandleType::Amplitude)
    return 1.0f;
  for (size_t i = 0; i < affectedNotes.size(); ++i)
    if (affectedNotes[i] == &note && i < originalParams.size())
      return std::pow(10.0f, (note.getVolumeDb() - originalParams[i].volumeDb) / 20.0f);
  return 1.0f;
}

bool PitchToolController::mouseDown(const juce::MouseEvent& e,
                                    const PitchToolHandles& handles,
                                    const std::vector<Note*>& selectedNotes,
                                    const CoordinateMapper& mapper)
{
  juce::ignoreUnused(mapper);


  const int hitIndex = handles.hitTest(e.position.x, e.position.y);
  if (hitIndex < 0) {
    return false;
  }

  const auto& handle = handles.getHandle(hitIndex);
  activeHandleType = handle.type;
  activeHandleNote = handle.note;

  // The visible top controls belong to the hovered note. When it is part of a
  // multi-note selection, slopes and vibrato are group edits, so capture the
  // complete selection. Hovering an unselected note must still only affect
  // that note rather than an unrelated selection.
  affectedNotes.clear();
  const bool handleIsSelected =
      handle.note && std::find(selectedNotes.begin(), selectedNotes.end(),
                               handle.note) != selectedNotes.end();
  const bool isGroupEdit = handleIsSelected && selectedNotes.size() > 1 &&
      (handle.type == PitchToolHandles::HandleType::TiltLeft ||
       handle.type == PitchToolHandles::HandleType::TiltRight ||
       handle.type == PitchToolHandles::HandleType::Vibrato ||
       handle.type == PitchToolHandles::HandleType::PitchDrift ||
       handle.type == PitchToolHandles::HandleType::Formant ||
       handle.type == PitchToolHandles::HandleType::Amplitude ||
       handle.type == PitchToolHandles::HandleType::CorrectPitch);
  if (isGroupEdit || !handle.note)
    affectedNotes = selectedNotes;
  else
    affectedNotes.push_back(handle.note);
  if (affectedNotes.empty())
    return false;
  // Capture original transformation parameters (not curves)
  originalParams.clear();
  originalParams.reserve(affectedNotes.size());
  for (auto* note : affectedNotes)
  {
    if (note)
    {
      auto params = TransformParams::fromNote(*note);
      // D-306: inclinar ya no mueve el cuerpo, asi que el punto de partida es
      // el centro visible tal cual. Antes se guardaba el midiNote sin la media
      // del tilt y cada gesto la volvia a sumar.
      originalParams.push_back(params);
    }
    else
    {
      originalParams.emplace_back();
    }
  }

  dragStartPos = e.position;
  dragging = true;
  return true;
}

bool PitchToolController::mouseDrag(const juce::MouseEvent& e,
                                    std::vector<Note*>& selectedNotes,
                                    const CoordinateMapper& mapper)
{
  juce::ignoreUnused(selectedNotes);

  if (!dragging)
    return false;

  const float deltaX = e.position.x - dragStartPos.x;
  const float deltaY = e.position.y - dragStartPos.y;

  applyOperation(affectedNotes, activeHandleType, deltaX, deltaY, mapper);
  return true;
}

bool PitchToolController::mouseUp(const juce::MouseEvent& e,
                                  PitchUndoManager* undoManager,
                                  std::function<void(int, int)> onRangeChanged)
{
  juce::ignoreUnused(e);

  if (!dragging)
    return false;

  if (activeHandleType == PitchToolHandles::HandleType::Amplitude && project)
  {
    std::vector<float> before, after;
    for (size_t i = 0; i < affectedNotes.size(); ++i)
    {
      before.push_back(originalParams[i].volumeDb);
      after.push_back(affectedNotes[i] ? affectedNotes[i]->getVolumeDb() : before.back());
    }
    auto action = std::make_unique<AmplitudeAction>(
        *project, affectedNotes, before, after, onAmplitudeEdited);
    dragging = false;
    activeHandleType = PitchToolHandles::HandleType::None;
    activeHandleNote = nullptr;
    affectedNotes.clear();
    originalParams.clear();
    if (before != after)
    {
      action->redo();
      if (undoManager) undoManager->addAction(std::move(action));
    }
    return true;
  }

  // The drag previews through rebuildDeltaForNotes(): the base is the note
  // centre and does not depend on the tilt (D-308).
  // Commit through the same full rebuild PitchToolAction's undo and redo use,
  // so the edit renders from the curves its undo would reproduce.
  if (project)
  {
    for (auto* note : affectedNotes)
      if (note)
        note->markSynthDirty();
    PitchCurveProcessor::rebuildBaseFromNotes(*project);
  }

  // Capture new transformation parameters (not curves)
  std::vector<TransformParams> newParams;
  newParams.reserve(affectedNotes.size());
  for (auto* note : affectedNotes)
  {
    if (note)
      newParams.push_back(TransformParams::fromNote(*note));
    else
      newParams.emplace_back();
  }

  // D-306: inclinar ya no toca midiNote, asi que los parametros capturados al
  // empezar el gesto SON el estado anterior (no hay media del tilt que sumar).
  std::vector<TransformParams> undoOldParams = originalParams;

  auto action = std::make_unique<PitchToolAction>(
      project, affectedNotes, undoOldParams, newParams, onRangeChanged);

  if (undoManager)
    undoManager->addAction(std::move(action));

  if (onRangeChanged)
  {
    for (auto* note : affectedNotes)
    {
      if (note)
        onRangeChanged(note->getStartFrame(), note->getEndFrame());
    }
  }

  dragging = false;
  activeHandleType = PitchToolHandles::HandleType::None;
  activeHandleNote = nullptr;
  affectedNotes.clear();
  originalParams.clear();
  return true;
}

void PitchToolController::applyOperation(std::vector<Note*>& notes,
                                         PitchToolHandles::HandleType type,
                                         float dragDeltaX,
                                         float dragDeltaY,
                                         const CoordinateMapper& mapper)
{
  if (!project)
    return;  // Cannot proceed without project reference

  const float pixelsPerSemitone = juce::jmax(1.0f, mapper.getPixelsPerSemitone());
  const float semitoneDelta = -dragDeltaY / pixelsPerSemitone;

  // Debug logging

  for (size_t i = 0; i < notes.size(); ++i)
  {
    auto* note = notes[i];
    if (!note || i >= originalParams.size())
      continue;

    // Restore original parameters before applying new transformation
    const auto& origParams = originalParams[i];
    if (type == PitchToolHandles::HandleType::Amplitude)
    {
      // Ten pixels per dB, independent of pitch zoom. Leave pitch state alone.
      note->setVolumeDb(juce::jlimit(-60.0f, 24.0f,
          std::round((origParams.volumeDb - dragDeltaY / 10.0f) * 10.0f) / 10.0f));
      continue;
    }
    origParams.applyToNote(*note);

    // The pitch when the gesture began, and the pitch this handle moves the
    // segment to (written through NotePitch after the switch). D-306: tilt is
    // the only gesture that no longer writes the body, so liveMidi stays put.
    const float origLiveMidi = origParams.midiNote;
    float liveMidi = origLiveMidi;

    // Apply new transformation by updating the appropriate parameter
    switch (type)
    {
      case PitchToolHandles::HandleType::TiltLeft:
      {
        // Drag UP = positive semitoneDelta = left edge goes UP. D-308: the
        // corner handles write the left fine layer (edge -> centre); the body
        // and the other layers do not move.
        note->setTiltFineLeft(origParams.tiltFineLeft + semitoneDelta);
        break;
      }
      case PitchToolHandles::HandleType::TiltRight:
      {
        // D-308: the right corner writes the right fine layer (centre -> edge).
        note->setTiltFineRight(origParams.tiltFineRight + semitoneDelta);
        break;
      }
      case PitchToolHandles::HandleType::Formant:
      {
        note->setFormantShift(std::round((origParams.formantShift + semitoneDelta) * 10.0f) / 10.0f);
        break;
      }
      case PitchToolHandles::HandleType::PitchDrift:
      {
        note->setPitchDrift(origParams.pitchDrift - dragDeltaY / 100.0f);
        break;
      }
      case PitchToolHandles::HandleType::Vibrato:
      {
        // Additive accumulation from original value (consistent with tilt handles)
        // Drag UP increases modulation; drag DOWN decreases it.
        const float dragDelta = -dragDeltaY / 100.0f;
        note->setVibrato(origParams.vibrato + dragDelta);
        break;
      }
      case PitchToolHandles::HandleType::CorrectPitch:
      {
        // VariAudio "Correct Pitch", on the inspector slider's scale: the
        // share of the sung deviation (from the correction reference) that
        // is removed. Dragging up adds to the share the segment had when the
        // drag began, 60 px for the full correction. The reference itself
        // is left alone, so the slider shows the same value afterwards.
        // D-306: the visible center IS the pitch (the tilt mean is no longer
        // folded into midiNote), so the reference starts at origParams.midiNote.
        const float startMidi = origParams.midiNote;
        const int referenceHz = project->getPitchReferenceHz();
        float source = origParams.correctionSource;
        // A pitch that is not a 0-100% correction of its reference (older
        // projects, pitch drawing) starts over from where it is, so the drag
        // never jumps.
        if (!ScaleUtils::correctionShare(*project, source, startMidi, referenceHz))
          source = startMidi;
        NotePitch::setCorrectionSource(*note, source);
        const float target = ScaleUtils::correctionTargetFor(*project, source, referenceHz);
        const float deviation = source - target;
        const float startShare =
            std::abs(deviation) < 1.0e-3f
                ? 1.0f
                : juce::jlimit(0.0f, 1.0f, 1.0f - (startMidi - target) / deviation);
        const float share =
            juce::jlimit(0.0f, 1.0f, startShare - dragDeltaY / 60.0f);
        NotePitch::applyCorrection(*note, target + deviation * (1.0f - share));
        break;
      }
      case PitchToolHandles::HandleType::SmoothLeft:
      {
        // Drag RIGHT (positive X) into note = more smoothing
        // 5px per additional frame, range [5, 50]
        const int transitionFrames = juce::jlimit(5, 50, 5 + static_cast<int>(dragDeltaX / 5.0f));
        note->setSmoothLeftFrames(transitionFrames);
        break;
      }
      case PitchToolHandles::HandleType::SmoothRight:
      {
        // Drag LEFT (negative X) into note = more smoothing
        // Use -dragDeltaX so LEFT drag increases frames
        const int transitionFrames = juce::jlimit(5, 50, 5 + static_cast<int>(-dragDeltaX / 5.0f));
        note->setSmoothRightFrames(transitionFrames);
        break;
      }
      case PitchToolHandles::HandleType::None:
      default:
        continue;  // Skip this note
    }

    // The other handles move the segment as an edit, which keeps the Correct
    // Pitch reference by the shared rule (see NotePitch). Tilt does not move
    // the body, so it no longer goes through NotePitch (D-306).
    if (type != PitchToolHandles::HandleType::CorrectPitch &&
        std::abs(liveMidi - origLiveMidi) > 1.0e-4f)
      NotePitch::applyEdit(*note, *project, origParams.correctionSource,
                           origLiveMidi, liveMidi);

    note->markDirty();
  }

  if (type == PitchToolHandles::HandleType::Amplitude)
  {
    if (onPitchEdited) onPitchEdited();
    return;
  }

  // NON-DESTRUCTIVE preview. D-308: the base no longer depends on the tilt
  // (the body is the centre and every tilt layer lives in the delta), so all
  // pitch gestures, tilt included, only rebuild the delta.
  PitchCurveProcessor::rebuildDeltaForNotes(*project, notes);
  // Mark dirty range for synthesis
  if (!notes.empty())
  {
    int minFrame = std::numeric_limits<int>::max();
    int maxFrame = std::numeric_limits<int>::min();
    for (const auto* note : notes)
    {
      minFrame = std::min(minFrame, note->getStartFrame());
      maxFrame = std::max(maxFrame, note->getEndFrame());
    }
    project->markNoteEditDirtyRange(minFrame, maxFrame);
  }

  // Trigger visual update
  if (onPitchEdited)
    onPitchEdited();
}

void PitchToolController::cancel()
{
  if (!dragging)
    return;

  // Restore original transformation parameters
  for (size_t i = 0; i < affectedNotes.size(); ++i)
  {
    if (i < originalParams.size() && affectedNotes[i])
    {
      // D-306: params.midiNote is the live body (tilt no longer migrates into
      // it), so applyToNote restores the pitch and the reference in one go.
      originalParams[i].applyToNote(*affectedNotes[i]);
    }
  }

  dragging = false;
  activeHandleType = PitchToolHandles::HandleType::None;
  activeHandleNote = nullptr;
  affectedNotes.clear();
  originalParams.clear();
}
