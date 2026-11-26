#include "PitchCurveRenderer.h"
#include "PitchEditor.h"
#include "States/SelectHandler.h"
#include "../../Utils/BasePitchCurve.h"
#include "../../Utils/PitchCurveProcessor.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>

void PitchCurveRenderer::beginMacroPreview(const std::vector<Note*>& notes)
{
  if (!project || !macroPreview.empty()) return;
  updateBasePitchCacheIfNeeded();
  for (const auto* note : notes)
    if (note)
    {
      macroPreview.emplace(note, MacroPreview{note->getMidiNote(), note->getPitchDrift(),
          PitchCurveProcessor::getDriftPreviewResponse(*project, *note),
          note->getMidiNote(), note->getPitchDrift()});
      macroNotes.push_back(note);
    }
  std::sort(macroNotes.begin(), macroNotes.end(), [](const Note* a, const Note* b)
  { return a->getStartFrame() < b->getStartFrame(); });
  lastMacroPreviewUpdate = 0.0;
}

float PitchCurveRenderer::macroOffset(const Note& note, int frame, bool includeDrift) const
{
  const auto it = macroPreview.find(&note);
  if (it == macroPreview.end()) return 0.0f;
  const auto& preview = it->second;
  float offset = preview.displayMidi - preview.midi;
  const int local = frame - note.getStartFrame();
  if (includeDrift && local >= 0 && local < static_cast<int>(preview.driftResponse.size()))
    offset += (preview.displayDrift - preview.drift) * preview.driftResponse[static_cast<size_t>(local)];
  return offset;
}

void PitchCurveRenderer::invalidateBasePitchCache()
{
  cacheInvalidated = true;
  cachedNoteCount = 0;
  cachedBasePitch.clear();
  cachedBasePitch.shrink_to_fit();
}

bool PitchCurveRenderer::shouldBreakPolyline(bool hasPreviousNote,
                                             float previousVisualEndFrame,
                                             float visualStartFrame,
                                             int previousRegion,
                                             int currentRegion,
                                             float toleranceFrames)
{
  if (!hasPreviousNote)
    return true; // first visible note, or the previous one was culled
  if (previousRegion != currentRegion)
    return true;
  return std::abs(visualStartFrame - previousVisualEndFrame) > toleranceFrames;
}

void PitchCurveRenderer::updateBasePitchCacheIfNeeded()
{
  if (!project)
  {
    cachedBasePitch.clear();
    cachedNoteCount = 0;
    cachedTotalFrames = 0;
    return;
  }

  const auto &notes = project->getNotes();
  const auto &audioData = project->getAudioData();
  const int totalFrames = static_cast<int>(audioData.f0.size());

  size_t currentNoteCount = 0;
  for (const auto &note : notes)
  {
    if (!note.isRest())
      ++currentNoteCount;
  }

  // Note: precise check would compare note positions/pitches but is expensive.
  if (cacheInvalidated || cachedNoteCount != currentNoteCount ||
      cachedTotalFrames != totalFrames || cachedBasePitch.empty())
  {
    if (currentNoteCount > 0 && totalFrames > 0)
    {
      std::vector<BasePitchCurve::NoteSegment> noteSegments;
      noteSegments.reserve(currentNoteCount);
      for (const auto &note : notes)
      {
        if (!note.isRest())
        {
          // D-303/D-308: same base centre the audio uses. Since D-308 the base
          // no longer subtracts the tilt mean: the body is the centre and the
          // three tilt layers live only in the delta.
          const float audioCenter = note.getMidiNote() + note.getPitchOffset() +
                                    note.getRenderCenterOffset();
          noteSegments.push_back(
              {note.getStartFrame(), note.getEndFrame(), audioCenter,
               audioCenter - note.getOriginalMidiNote()});
        }
      }

      if (!noteSegments.empty())
      {
        cachedBasePitch =
            BasePitchCurve::generateForNotes(noteSegments, totalFrames);
        cachedNoteCount = currentNoteCount;
        cachedTotalFrames = totalFrames;
        cacheInvalidated = false;
      }
      else
      {
        cachedBasePitch.clear();
        cachedNoteCount = 0;
        cachedTotalFrames = 0;
        cacheInvalidated = false;
      }
    }
    else
    {
      cachedBasePitch.clear();
      cachedNoteCount = 0;
      cachedTotalFrames = 0;
      cacheInvalidated = false;
    }
  }
}

void PitchCurveRenderer::draw(juce::Graphics &g, const Params &params)
{
  if (!project || !coordMapper)
    return;

  if (params.hidePitchCurves)
    return;

  const double now = juce::Time::getMillisecondCounterHiRes();
  if (!macroPreview.empty() && now - lastMacroPreviewUpdate >= 33.0)
  {
    for (auto& entry : macroPreview)
    {
      entry.second.displayMidi = entry.first->getMidiNote();
      entry.second.displayDrift = entry.first->getPitchDrift();
    }
    lastMacroPreviewUpdate = now;
  }
  const auto &audioData = project->getAudioData();
  if (audioData.f0.empty())
    return;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();

  const float globalOffset = project->getGlobalPitchOffset();

  // Draw the same composed pitch used by synthesis. basePitch already includes
  // any per-note pitch offset, including the live drag preview.
  if (params.showDeltaPitch)
  {
    // White pitch contour over the steel-blue notes: blue on blue was hard
    // to read, and white is what Melodyne uses over a coloured segment.
    const auto curveColour = juce::Colours::white;
    g.setColour(curveColour.withMultipliedAlpha(
        juce::jlimit(0.0f, 1.0f, params.pitchCurveAlpha)));

    juce::Path path;
    bool pathStarted = false;
    auto strokeCurrentPath = [&]()
    {
      if (pathStarted)
        g.strokePath(path, juce::PathStrokeType(1.35f));
      path.clear();
      pathStarted = false;
    };

    const auto &chunkRanges = audioData.segmentChunkRanges;
    auto getRegionIndex = [&chunkRanges](const Note &note)
    {
      if (chunkRanges.empty())
        return 0;

      const int midpoint = note.getSrcStartFrame() +
                           std::max(0, note.getSrcDurationFrames()) / 2;
      for (size_t i = 0; i < chunkRanges.size(); ++i)
      {
        if (midpoint >= chunkRanges[i].first &&
            midpoint < chunkRanges[i].second)
          return static_cast<int>(i);
      }
      return -1;
    };

    bool hasPreviousNote = false;
    float previousVisualEndFrame = -1.0f;
    int previousRegion = -1;
    // D-304: una sola polilinea mientras las notas sean contiguas y de la misma
    // region. previousFinalMidi sobrevive a la frontera para que el filtro de
    // saltos (>0,75 st) tambien actue ahi.
    float previousFinalMidi = -1.0f;
    const bool drawingPreview = params.midiCurveOverride != nullptr;

    // Only build path geometry for notes that can touch the viewport. Without
    // this, every repaint (drag, zoom, scroll) walked and stroked the pitch
    // curve of the entire project, which scales with song length and is
    // especially costly with the Direct2D renderer (geometry is rebuilt and
    // tessellated per frame). Path coordinates are in world space, so the
    // visible range is [scrollX, scrollX + componentWidth].
    constexpr float cullPaddingPx = 16.0f;
    const float cullLeft = static_cast<float>(scrollX) - cullPaddingPx;
    const float cullRight = static_cast<float>(scrollX) +
                            static_cast<float>(params.componentWidth) +
                            cullPaddingPx;

    for (const auto &note : project->getNotes())
    {
      if (note.isRest())
        continue;

      {
        const float cullStartFrame = note.getVisualStartFrame();
        const float cullEndFrame = note.getVisualEndFrame();
        const float noteLeft =
            framesToSeconds(std::min(cullStartFrame, cullEndFrame) - 1.0f) *
            pixelsPerSecond;
        const float noteRight =
            framesToSeconds(std::max(cullStartFrame, cullEndFrame) + 1.0f) *
            pixelsPerSecond;
        if (noteRight < cullLeft || noteLeft > cullRight)
        {
          // Break the polyline here so the next visible note does not join
          // across the skipped (off-screen) span.
          strokeCurrentPath();
          hasPreviousNote = false;
          previousFinalMidi = -1.0f;
          continue;
        }
      }

      const int startFrame = note.getStartFrame();
      const int endFrame =
          std::min(note.getEndFrame(), static_cast<int>(audioData.f0.size()));
      const float visualStartFrame = note.getVisualStartFrame();
      const float visualEndFrame = note.getVisualEndFrame();
      const int currentRegion = getRegionIndex(note);

      // D-304: la curva compuesta se dibuja continua entre notas contiguas
      // ("Las notas de la melodia original son continuas en la vista normal").
      // Solo se corta en un hueco, al cambiar de region o cuando la nota
      // anterior se salto por culling. Antes se recortaba cada nota a su propio
      // tramo y el trazo se partia en cada frontera, incluso donde la altura
      // era la misma.
      if (shouldBreakPolyline(hasPreviousNote, previousVisualEndFrame,
                              visualStartFrame, previousRegion,
                              currentRegion))
      {
        strokeCurrentPath();
        previousFinalMidi = -1.0f;
      }

      // Clip long notes to the viewport before constructing the path.
      const int sourceLength = std::max(1, endFrame - startFrame);
      const float visualLength = visualEndFrame - visualStartFrame;
      int firstVisible = startFrame;
      int lastVisible = endFrame;
      if (visualLength > 0.0f)
      {
        const double left = scrollX / pixelsPerSecond * audioData.sampleRate / HOP_SIZE;
        const double right = (scrollX + params.componentWidth) / pixelsPerSecond
                             * audioData.sampleRate / HOP_SIZE;
        firstVisible = std::max(startFrame, startFrame + static_cast<int>(
            std::floor((left - visualStartFrame) / visualLength * sourceLength)) - 1);
        lastVisible = std::min(endFrame, startFrame + static_cast<int>(
            std::ceil((right - visualStartFrame) / visualLength * sourceLength)) + 1);
      }
      if (firstVisible >= lastVisible)
      {
        // D-304: the note has no drawable frame inside the viewport (it is
        // still within the cull padding). Do not bridge the polyline across
        // it: a straight line would run over the skipped note.
        strokeCurrentPath();
        hasPreviousNote = false;
        previousFinalMidi = -1.0f;
        continue;
      }
      for (int i = firstVisible; i < lastVisible; ++i)
      {
        float finalMidi = 0.0f;
        if (params.midiCurveOverride &&
            i < static_cast<int>(params.midiCurveOverride->size()))
        {
          finalMidi = (*params.midiCurveOverride)[static_cast<size_t>(i)] +
                      globalOffset;
        }
        else
        {
          const float baseMidi =
              (i < static_cast<int>(audioData.basePitch.size()))
                  ? audioData.basePitch[static_cast<size_t>(i)]
                  : ((i < static_cast<int>(audioData.f0.size()) &&
                      audioData.f0[static_cast<size_t>(i)] > 0.0f)
                         ? freqToMidi(audioData.f0[static_cast<size_t>(i)])
                         : 0.0f);
          const float deltaMidi =
              (i < static_cast<int>(audioData.deltaPitch.size()))
                  ? audioData.deltaPitch[static_cast<size_t>(i)]
                  : 0.0f;
          finalMidi = baseMidi + deltaMidi + globalOffset + macroOffset(note, i, true);
        }

        if (finalMidi > 0.0f)
        {
          // A jump of most of a semitone between consecutive frames is a
          // detector outlier, not vibrato. Break the line instead of painting
          // a tall vertical stroke inside the note. A drawn preview is the
          // user's own contour and must never be broken this way.
          if (!drawingPreview && pathStarted && previousFinalMidi > 0.0f &&
              std::abs(finalMidi - previousFinalMidi) > 0.75f)
            strokeCurrentPath();
          previousFinalMidi = finalMidi;

          const float position =
              (static_cast<float>(i - startFrame) + 0.5f) /
              static_cast<float>(sourceLength);
          const float visualFrame =
              visualStartFrame + position * (visualEndFrame - visualStartFrame) -
              0.5f;
          const float x = framesToSeconds(visualFrame) * pixelsPerSecond;
          const float y = coordMapper->midiToY(finalMidi) +
                          pixelsPerSemitone * 0.5f;
          if (!pathStarted)
          {
            path.startNewSubPath(x, y);
            pathStarted = true;
          }
          else
          {
            path.lineTo(x, y);
          }
        }
        else
        {
          strokeCurrentPath();
          previousFinalMidi = -1.0f;
        }
      }

      // D-304: do not flush here. The polyline continues into the next note
      // when it is contiguous; it is only stroked at a gap, a region change,
      // an unvoiced frame or a real jump.
      hasPreviousNote = true;
      previousVisualEndFrame = visualEndFrame;
      previousRegion = currentRegion;
    }

    strokeCurrentPath();
  }

  auto drawHzDebugCurve =
      [&](const std::vector<float> &curve, juce::Colour colour,
          float strokeWidth, float midiOffset = 0.0f)
  {
    if (curve.empty())
      return;

    const double visibleStartTime = scrollX / pixelsPerSecond;
    const double visibleEndTime = (scrollX + params.componentWidth) / pixelsPerSecond;
    const int visStartFrame =
        std::max(0, static_cast<int>(visibleStartTime * audioData.sampleRate /
                                     HOP_SIZE));
    const int visEndFrame = std::min(
        static_cast<int>(curve.size()),
        static_cast<int>(visibleEndTime * audioData.sampleRate / HOP_SIZE) + 1);

    g.setColour(colour);
    juce::Path path;
    bool pathStarted = false;

    for (int i = visStartFrame; i < visEndFrame; ++i)
    {
      const float f0 = curve[static_cast<size_t>(i)];
      if (f0 <= 0.0f)
      {
        pathStarted = false;
        continue;
      }

      const float midi = freqToMidi(f0) + midiOffset;
      const float x = framesToSeconds(i) * pixelsPerSecond;
      const float y = coordMapper->midiToY(midi) + pixelsPerSemitone * 0.5f;
      if (!pathStarted)
      {
        path.startNewSubPath(x, y);
        pathStarted = true;
      }
      else
      {
        path.lineTo(x, y);
      }
    }

    g.strokePath(path, juce::PathStrokeType(strokeWidth));
  };

  if (params.showActualF0Debug)
  {
    drawHzDebugCurve(audioData.rawF0, juce::Colours::aqua.withAlpha(0.90f),
                     1.7f);
  }

  if (params.showCleanedF0Debug)
  {
    drawHzDebugCurve(audioData.cleanedF0,
                     juce::Colours::orange.withAlpha(0.90f), 1.7f);
  }

  if (params.showUvInterpolationDebug)
  {
    drawHzDebugCurve(audioData.denseF0,
                     juce::Colours::magenta.withAlpha(0.86f), 1.7f);
  }

  if (params.showVocoderF0Debug)
  {
    const auto vocoderF0 = project->getAdjustedF0();
    if (!vocoderF0.empty())
      drawHzDebugCurve(vocoderF0, juce::Colours::lime.withAlpha(0.86f), 2.0f);
    else
      drawHzDebugCurve(audioData.f0, juce::Colours::lime.withAlpha(0.86f),
                       2.0f, globalOffset);
  }

  // Draw base pitch curve as dashed line. Uses cached base pitch to avoid
  // expensive recalculation on every repaint.
  if (params.showBasePitch)
  {
    const bool useLiveBasePreview =
        selectHandler && pitchEditor &&
        (selectHandler->isSingleNoteDragging() || pitchEditor->isDraggingMultiNotes());
    if (!useLiveBasePreview)
      updateBasePitchCacheIfNeeded();

    const auto &basePitchCurve =
        useLiveBasePreview ? audioData.basePitch : cachedBasePitch;
    if (!basePitchCurve.empty())
    {
      const double visibleStartTime = scrollX / pixelsPerSecond;
      const double visibleEndTime = (scrollX + params.componentWidth) / pixelsPerSecond;
      const int visStartFrame =
          std::max(0, static_cast<int>(visibleStartTime * audioData.sampleRate /
                                       HOP_SIZE));
      const int visEndFrame = std::min(
          static_cast<int>(basePitchCurve.size()),
          static_cast<int>(visibleEndTime * audioData.sampleRate / HOP_SIZE) + 1);

      g.setColour((greyscale ? APP_COLOR_SECONDARY.withSaturation(0.0f)
                             : APP_COLOR_SECONDARY)
                      .withAlpha(0.6f));
      juce::Path basePath;
      bool basePathStarted = false;

      for (int i = visStartFrame; i < visEndFrame; ++i)
      {
        if (i >= 0 && i < static_cast<int>(basePitchCurve.size()))
        {
          float baseMidi = basePitchCurve[static_cast<size_t>(i)];
          if (!macroNotes.empty())
          {
            auto it = std::upper_bound(macroNotes.begin(), macroNotes.end(), i,
                [](int frame, const Note* note) { return frame < note->getStartFrame(); });
            if (it != macroNotes.begin())
            {
              const auto* note = *std::prev(it);
              if (i < note->getEndFrame())
                baseMidi += macroOffset(*note, i, false);
            }
          }
          if (baseMidi > 0.0f)
          {
            const float x = framesToSeconds(i) * pixelsPerSecond;
            const float y = coordMapper->midiToY(baseMidi) +
                            pixelsPerSemitone * 0.5f;

            if (!basePathStarted)
            {
              basePath.startNewSubPath(x, y);
              basePathStarted = true;
            }
            else
            {
              basePath.lineTo(x, y);
            }
          }
          else if (basePathStarted)
          {
            // Break path at unvoiced regions - draw current segment before
            // breaking.
            juce::Path dashedPath;
            juce::PathStrokeType stroke(1.5f);
            const float dashLengths[] = {4.0f, 4.0f};
            stroke.createDashedStroke(dashedPath, basePath, dashLengths, 2);
            g.strokePath(dashedPath, juce::PathStrokeType(1.5f));
            basePath.clear();
            basePathStarted = false;
          }
        }
      }

      if (basePathStarted)
      {
        juce::Path dashedPath;
        juce::PathStrokeType stroke(1.5f);
        const float dashLengths[] = {4.0f, 4.0f};
        stroke.createDashedStroke(dashedPath, basePath, dashLengths, 2);
        g.strokePath(dashedPath, juce::PathStrokeType(1.5f));
      }
    }
  }
}
