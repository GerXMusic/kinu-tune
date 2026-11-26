#include "../Main/DeferredRenderQueue.h"
#include "NoteRenderer.h"
#include "PitchToolController.h"
#include "PitchToolHandles.h"
#include "BoxSelector.h"
#include "PitchEditor.h"
#include "States/SelectHandler.h"
#include "States/SplitHandler.h"
#include "VisualWaveformEnvelope.h"
#include "../Components/AppFont.h"
#include "../EngineColours.h"
#include "../../Utils/Constants.h"
#include "../../Utils/ScaleUtils.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
struct NoteGradientColours
{
  juce::Colour centre;
  juce::Colour side;
};

NoteGradientColours getNoteGradientColours()
{
  // Steel-blue note body, the colour the editor used before the Melodyne
  // palette experiment. The waveform inside stays white and the pitch curve
  // on top is the only element that tells the tuning.
  static const juce::Colour centre(0xFF5E93D6u);
  static const juce::Colour side(0xFF304861u);
  return {centre, side};
}

void setNoteGradientFill(juce::Graphics &g,
                         const juce::Rectangle<float> &bounds,
                         float centreY,
                         const NoteGradientColours &colours)
{
  const float height = bounds.getHeight();
  if (height <= 0.0f || !std::isfinite(height))
  {
    g.setColour(colours.centre);
    return;
  }

  const float top = bounds.getY();
  const float bottom = bounds.getBottom();
  const float gradientCentre =
      juce::jlimit(0.0f, 1.0f, (centreY - top) / height);

  juce::ColourGradient gradient(colours.side, bounds.getCentreX(), top,
                                colours.side, bounds.getCentreX(), bottom,
                                false);
  gradient.addColour(gradientCentre, colours.centre);
  g.setGradientFill(gradient);
}

bool isContinuousSplitPair(const Note &left, const Note &right)
{
  return left.getEndFrame() == right.getStartFrame() &&
         left.getSrcEndFrame() == right.getSrcStartFrame();
}
} // namespace

void NoteRenderer::draw(juce::Graphics &g, Pass pass, bool splitModeActive,
                        int componentWidth)
{
  if (!project || !coordMapper)
    return;

  // P1b fase 6: barrido del bloque en render. Un borde brillante recorre el
  // bloque pendiente, en Overlay para quedar por encima de los blobs. D-309:
  // con el cajon Motor abierto no hay linea blanca; el barrido enciende los
  // bordes de motor, que viven en el pase Body.
  if (pass == Pass::Overlay && sweepActive && !engineBlockView)
  {
    const double sampleRate = project->getAudioData().sampleRate;
    if (sampleRate > 0.0)
    {
      const double startSeconds =
          static_cast<double>(sweepStartFrame) * HOP_SIZE / sampleRate;
      const double endSeconds =
          static_cast<double>(sweepEndFrame) * HOP_SIZE / sampleRate;
      const float pps = coordMapper->getPixelsPerSecond();
      const double viewStart = coordMapper->getViewStartSeconds();
      const double scroll = coordMapper->getScrollX();
      const float x0 =
          static_cast<float>((startSeconds - viewStart) * pps - scroll);
      const float x1 =
          static_cast<float>((endSeconds - viewStart) * pps - scroll);
      const float width = x1 > x0 ? x1 - x0 : 0.0f;
      // Full pitch height, never the repaint clip: a partial repaint made the
      // sweep a box and a line cut mid-canvas. Only the bright edge is drawn;
      // the old 10 % fill read as a stray translucent rectangle over the notes.
      const float height =
          static_cast<float>(MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) *
          coordMapper->getPixelsPerSemitone();
      const double elapsed =
          (juce::Time::getMillisecondCounterHiRes() - sweepStartMs) * 0.001;
      const float barX = x0 + width * sweep::phaseAt(elapsed);

      g.setColour(juce::Colours::white.withAlpha(0.55f));
      g.fillRect(barX, 0.0f, 2.0f, height);
    }
  }

  const bool drawBodies = pass == Pass::Body || pass == Pass::HoveredBody;
  const bool drawOverlays = pass == Pass::Overlay;
  const bool drawHoverShadow = pass == Pass::HoverShadow;
  const bool drawHoveredBody = pass == Pass::HoveredBody;

  if ((drawHoverShadow || drawHoveredBody) && hoveredNote == nullptr)
    return;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();
  const int pitchReferenceHz = project->getPitchReferenceHz();

  // D-309: where the border sweep currently is, in canvas pixels. The range
  // was already expanded by PianoRollComponent to the first and last note of
  // the affected visual block.
  bool engineSweepActive = false;
  float engineSweepBarX = 0.0f;
  if (engineBlockView && sweepActive)
  {
    const double sampleRate = project->getAudioData().sampleRate;
    if (sampleRate > 0.0)
    {
      const double startSeconds =
          static_cast<double>(sweepStartFrame) * HOP_SIZE / sampleRate;
      const double endSeconds =
          static_cast<double>(sweepEndFrame) * HOP_SIZE / sampleRate;
      // World coordinates, same space as the note x below. Subtracting the
      // scroll here put the bar off-canvas as soon as the view was scrolled,
      // so no border ever lit up.
      const float x0 =
          static_cast<float>(startSeconds * pixelsPerSecond);
      const float x1 =
          static_cast<float>(endSeconds * pixelsPerSecond);
      const float width = x1 > x0 ? x1 - x0 : 0.0f;
      const double elapsed =
          (juce::Time::getMillisecondCounterHiRes() - sweepStartMs) * 0.001;
      engineSweepBarX = x0 + width * sweep::phaseAt(elapsed);
      engineSweepActive = true;
    }
  }

  // Pre-allocated scratch buffers to avoid per-note heap allocations
  std::vector<float> waveValues;
  waveValues.reserve(2048);

  const bool isMultiDragging = pitchEditor && pitchEditor->isDraggingMultiNotes();
  const bool isSingleDragging =
      selectHandler && selectHandler->isSingleNoteDragging();
  const bool isDraggingNote = isSingleDragging || isMultiDragging;
  if (drawHoverShadow && isDraggingNote)
    return;

  const std::vector<Note *> *draggedNotes =
      isMultiDragging ? &pitchEditor->getDraggedNotes() : nullptr;
  Note *hoveredMultiDragNote =
      isMultiDragging ? pitchEditor->getHoveredMultiDragNote() : nullptr;
  int selectedNoteCount = 0;
  for (const auto &note : project->getNotes())
  {
    if (!note.isRest() && note.isSelected())
      ++selectedNoteCount;
  }
  const bool showSelectionStatus =
      selectedNoteCount > 1 ||
      (selectedNoteCount > 0 && boxSelector &&
       (boxSelector->isSelecting() || boxSelector->wasLastSelectionFromBox()));

  const auto &audioData = project->getAudioData();
  const auto getSplitHoverLimits = [&](const Note &note)
  {
    int leftLimit = std::numeric_limits<int>::min();
    int rightLimit = std::numeric_limits<int>::max();
    if (!splitModeActive)
      return std::pair {leftLimit, rightLimit};

    std::vector<const Note *> timelineNotes;
    for (const auto &candidate : project->getNotes())
      if (!candidate.isRest())
        timelineNotes.push_back(&candidate);
    std::sort(timelineNotes.begin(), timelineNotes.end(),
              [](const Note *left, const Note *right)
              { return left->getStartFrame() < right->getStartFrame(); });

    const auto &chunkRanges = audioData.segmentChunkRanges;
    const auto getRegionIndex = [&chunkRanges](const Note &candidate)
    {
      if (chunkRanges.empty())
        return 0;
      const int midpoint = candidate.getSrcStartFrame() +
                           std::max(0, candidate.getSrcDurationFrames()) / 2;
      for (size_t i = 0; i < chunkRanges.size(); ++i)
        if (midpoint >= chunkRanges[i].first && midpoint < chunkRanges[i].second)
          return static_cast<int>(i);
      return -1;
    };

    const auto it = std::find(timelineNotes.begin(), timelineNotes.end(), &note);
    if (it == timelineNotes.end())
      return std::pair {leftLimit, rightLimit};

    const auto index = static_cast<size_t>(std::distance(timelineNotes.begin(), it));
    const int noteRegion = getRegionIndex(note);
    if (index > 0 && noteRegion >= 0 &&
        getRegionIndex(*timelineNotes[index - 1]) == noteRegion &&
        isContinuousSplitPair(*timelineNotes[index - 1], note))
    {
      leftLimit = static_cast<int>(std::lround(
          0.5f * framesToSeconds(timelineNotes[index - 1]->getEndFrame() +
                                  note.getStartFrame()) * pixelsPerSecond)) + 2;
    }
    if (index + 1 < timelineNotes.size() && noteRegion >= 0 &&
        getRegionIndex(*timelineNotes[index + 1]) == noteRegion &&
        isContinuousSplitPair(note, *timelineNotes[index + 1]))
    {
      rightLimit = static_cast<int>(std::lround(
          0.5f * framesToSeconds(note.getEndFrame() +
                                  timelineNotes[index + 1]->getStartFrame()) * pixelsPerSecond)) - 2;
    }
    return std::pair {leftLimit, rightLimit};
  };
  const float *globalSamples =
      (drawBodies || drawHoverShadow) && audioData.waveform.getNumSamples() > 0
          ? audioData.waveform.getReadPointer(0)
          : nullptr;
  int globalTotalSamples =
      (drawBodies || drawHoverShadow) ? audioData.waveform.getNumSamples() : 0;

  auto getHighlightedShadowBounds = [&](const Note &note, float x, float y,
                                        float renderedWidth, float noteWidth,
                                        float noteHeight)
  {
    auto shadowVisualBounds =
        juce::Rectangle<float>(x, y, renderedWidth, noteHeight);
    const float *samples = globalSamples;
    int totalSamples = globalTotalSamples;
    int startSample = 0;
    int endSample = 0;
    if (samples && totalSamples > 0)
    {
      const float sampleStartFrame = note.hasTimingPreview()
                                         ? static_cast<float>(note.getSrcStartFrame())
                                         : note.getVisualStartFrame();
      const float sampleEndFrame = note.hasTimingPreview()
                                       ? static_cast<float>(note.getSrcEndFrame())
                                       : note.getVisualEndFrame();
      startSample = static_cast<int>(framesToSeconds(sampleStartFrame) *
                                     audioData.sampleRate);
      endSample = static_cast<int>(framesToSeconds(sampleEndFrame) *
                                   audioData.sampleRate);
      startSample = std::max(0, std::min(startSample, totalSamples - 1));
      endSample = std::max(startSample + 1, std::min(endSample, totalSamples));
    }

    if (samples && totalSamples > 0 && noteWidth > 2.0f &&
        endSample > startSample)
    {
      const int shadowPointCount =
          std::max(2, std::min(512, static_cast<int>(std::ceil(noteWidth)) + 1));
      const auto shadowEnvelope = VisualWaveformEnvelope::build(
          samples, totalSamples, startSample, endSample, shadowPointCount,
          renderedWidth, audioData.sampleRate, pixelsPerSecond, true,
          pitchToolController ? pitchToolController->getAmplitudePreviewGain(note) : 1.0f);
      const float maxSample =
          shadowEnvelope.empty()
              ? 0.0f
              : *std::max_element(shadowEnvelope.begin(), shadowEnvelope.end());

      const float centerY = y + noteHeight * 0.5f;
      const float waveHeight = noteHeight * 3.0f;
      shadowVisualBounds =
          juce::Rectangle<float>(x, centerY - maxSample * waveHeight * 0.5f,
                                 renderedWidth, maxSample * waveHeight);
    }

    return shadowVisualBounds.expanded(4.0f, 4.0f)
        .getSmallestIntegerContainer();
  };

  // Calculate visible time range for culling
  const double visibleStartTime = scrollX / pixelsPerSecond;
  const double visibleEndTime = (scrollX + componentWidth) / pixelsPerSecond;

  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;
    if (drawHoveredBody && &note != hoveredNote)
      continue;

    // Viewport culling: skip notes outside visible area. D-305 removed the
    // ghost of the original segment from the normal view, so the source frame
    // range no longer widens the cull box there. The bypass / original-pitch
    // views draw at source coordinates, so for them the cull box is the source
    // span (same visible notes as before).
    const double noteStartTime = framesToSeconds(
        originalPitchView ? note.getSrcStartFrame() : note.getVisualStartFrame());
    const double noteEndTime = framesToSeconds(
        originalPitchView ? note.getSrcEndFrame() : note.getVisualEndFrame());
    if (noteEndTime < visibleStartTime || noteStartTime > visibleEndTime)
      continue;

    const float x = originalPitchView
                        ? static_cast<float>(
                              framesToSeconds(note.getSrcStartFrame()) *
                              pixelsPerSecond)
                        : static_cast<float>(noteStartTime * pixelsPerSecond);
    const float w = originalPitchView
                        ? framesToSeconds(note.getSrcEndFrame() -
                                          note.getSrcStartFrame()) *
                              pixelsPerSecond
                        : framesToSeconds(note.getVisualDurationFrames()) *
                              pixelsPerSecond;
    const float h = pixelsPerSemitone;
    const float renderedWidth = std::max(w, 4.0f);

    // D-309: visual block of this note, from the map PianoRollComponent built
    // once per fingerprint. The block of a selected note is drawn thicker.
    const int noteIndex =
        static_cast<int>(&note - project->getNotes().data());
    const engineBlocks::VisualBlock *visualBlock = nullptr;
    if (engineBlockView && engineBlockMap != nullptr)
    {
      const int blockIndex = engineBlockMap->blockForNote(noteIndex);
      if (blockIndex >= 0)
        visualBlock =
            &engineBlockMap->blocks[static_cast<size_t>(blockIndex)];
    }
    const bool blockActive =
        visualBlock != nullptr &&
        std::find(activeEngineBlocks.begin(), activeEngineBlocks.end(),
                  visualBlock->index) != activeEngineBlocks.end();

    // Position at grid cell center for MIDI note, then offset by pitch adjustment
    const float noteMidi =
        originalPitchView ? note.getOriginalMidiNote() : note.getMidiNote();
    const float baseGridCenterY =
        coordMapper->midiToY(noteMidi) + pixelsPerSemitone * 0.5f;
    const float pitchOffsetPixels =
        originalPitchView ? 0.0f : -note.getPitchOffset() * pixelsPerSemitone;
    const float y = baseGridCenterY + pitchOffsetPixels - h * 0.5f;
    const bool isPreviewPlaybackNote =
        previewPlaybackActive && note.getEndFrame() > previewStartFrame &&
        note.getStartFrame() < previewEndFrame;
    if (drawHoveredBody && isPreviewPlaybackNote)
      continue;

    if (drawHoverShadow)
    {
      if (&note != hoveredNote)
        continue;
      if (isPreviewPlaybackNote)
        return;

      auto shadowBounds =
          segmentStyle && !greyscale
              ? juce::Rectangle<float>(x, y, renderedWidth, h)
                    .expanded(4.0f).getSmallestIntegerContainer()
              : getHighlightedShadowBounds(note, x, y, renderedWidth, w, h);
      const float minimumHoverWidth =
          segmentStyle && !greyscale ? 0.0f : PitchToolHandles::buttonGroupWidth;
      if (shadowBounds.getWidth() < minimumHoverWidth)
      {
        const float centerX = x + renderedWidth * 0.5f;
        shadowBounds.setX(static_cast<int>(std::floor(centerX - minimumHoverWidth * 0.5f)));
        shadowBounds.setWidth(static_cast<int>(std::ceil(minimumHoverWidth)));
      }
      const auto [leftLimit, rightLimit] = getSplitHoverLimits(note);
      shadowBounds.setX(std::max(shadowBounds.getX(), leftLimit));
      shadowBounds.setRight(std::min(shadowBounds.getRight(), rightLimit));
      g.setColour(juce::Colours::white.withAlpha(0.08f));
      g.fillRoundedRectangle(shadowBounds.toFloat(), 3.0f);
      return;
    }

    if (drawBodies)
    {
      const NoteGradientColours noteColours =
          greyscale
              ? NoteGradientColours{juce::Colour(0xFFCCCCCCu),
                                    juce::Colour(0xFF3A3A3Au)}
              : getNoteGradientColours();
      juce::Rectangle<float> noteVisualBounds(x, y, renderedWidth, h);
      // The shape that was filled, for the pinned-track outline.
      juce::Path bodyShape;
      // Outline-only pass: fill nothing, but build the same shapes.
      std::optional<juce::Graphics::ScopedSaveState> noFill;
      if (!fillBodies)
      {
        noFill.emplace(g);
        g.reduceClipRegion(juce::Rectangle<int>());
      }

      const float *samples = globalSamples;
      int totalSamples = globalTotalSamples;
      int startSample = 0;
      int endSample = 0;
      if (samples && totalSamples > 0)
      {
        const float sampleStartFrame = note.hasTimingPreview()
                                           ? static_cast<float>(note.getSrcStartFrame())
                                           : note.getVisualStartFrame();
        const float sampleEndFrame = note.hasTimingPreview()
                                         ? static_cast<float>(note.getSrcEndFrame())
                                         : note.getVisualEndFrame();
        startSample = static_cast<int>(framesToSeconds(sampleStartFrame) *
                                       audioData.sampleRate);
        endSample = static_cast<int>(framesToSeconds(sampleEndFrame) *
                                     audioData.sampleRate);
        startSample = std::max(0, std::min(startSample, totalSamples - 1));
        endSample = std::max(startSample + 1, std::min(endSample, totalSamples));
      }

      if (segmentStyle)
      {
        // Melodyne-style note: the real amplitude envelope *is* the note. No
        // rectangle, pill or frame is painted, so the blob reads as a
        // waveform instead of a MIDI bar. The old rectangle still defines
        // the hitbox and the smart controls, which are untouched.
        const auto segmentBounds =
            juce::Rectangle<float>(x, y + 1.0f, renderedWidth,
                                   std::max(2.0f, h - 2.0f))
                .reduced(0.0f, 0.5f);
        const bool selected = note.isSelected();
        const float centreY = segmentBounds.getCentreY();
        const float waveHeight =
            std::max(2.0f, segmentBounds.getHeight() - 2.0f);
        const float left = segmentBounds.getX();

        juce::Path wavePath;
        bool hasWave = false;

        // Tarea 3 (2026-11-22): en la vista de bypass las notas van en gris pero
        // conservan la forma de onda real (Melodyne muestra la melodia original
        // con su envolvente, no una barra plana).
        if ((!greyscale || originalPitchView) && samples &&
            totalSamples > 0 && renderedWidth > 2.0f &&
            endSample > startSample)
        {
          const int pointCount = std::max(
              2, std::min(1024,
                          static_cast<int>(std::ceil(renderedWidth)) + 1));
          waveValues = VisualWaveformEnvelope::build(
              samples, totalSamples, startSample, endSample, pointCount,
              renderedWidth, audioData.sampleRate, pixelsPerSecond, true,
              pitchToolController ? pitchToolController->getAmplitudePreviewGain(note)
                                  : 1.0f);

          // The envelope is an absolute RMS, so on quiet material it shrinks
          // to a couple of pixels and the note reads as a plain bar. Lift
          // each note to a similar visual size (capped, so a silent segment
          // is not blown up into noise) while keeping its own shape.
          if (!waveValues.empty())
          {
            const float peak =
                *std::max_element(waveValues.begin(), waveValues.end());
            if (peak > 0.03f)
            {
              const float gain = std::min(6.0f, 0.85f / peak);
              if (gain > 1.0f)
                for (auto &value : waveValues)
                  value = std::min(1.0f, value * gain);
            }
          }

          const size_t numPoints = waveValues.size();
          if (numPoints >= 2)
          {
            const float step =
                renderedWidth / static_cast<float>(numPoints - 1);
            // A silent note still leaves a thin blue ribbon rather than
            // vanishing, so it can be found and clicked.
            const auto amplitudeAt = [&](size_t index)
            {
              return std::max(0.10f, waveValues[index]);
            };

            wavePath.startNewSubPath(
                left, centreY - amplitudeAt(0) * waveHeight * 0.5f);
            for (size_t i = 1; i < numPoints; ++i)
              wavePath.lineTo(left + static_cast<float>(i) * step,
                              centreY -
                                  amplitudeAt(i) * waveHeight * 0.5f);
            for (int i = static_cast<int>(numPoints) - 1; i >= 0; --i)
              wavePath.lineTo(
                  left + static_cast<float>(i) * step,
                  centreY + amplitudeAt(static_cast<size_t>(i)) *
                                waveHeight * 0.5f);
            wavePath.closeSubPath();
            hasWave = true;
            bodyShape = wavePath;
          }
        }

        if (hasWave)
        {
          // Soft drop shadow under the silhouette, not under a rectangle.
          g.setColour(juce::Colours::black.withAlpha(0.30f));
          g.fillPath(wavePath, juce::AffineTransform::translation(0.0f, 1.2f));

          // Dark at both edges, light in the middle, as the user asked: the
          // highlight runs through the centre of the wave, not along the top.
          setNoteGradientFill(g, segmentBounds, centreY, noteColours);
          g.fillPath(wavePath);

          if (selected)
          {
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillPath(wavePath);
          }

          if (isPreviewPlaybackNote)
          {
            const float previewProgressX =
                static_cast<float>(previewCurrentTime * pixelsPerSecond);
            const float clipRight =
                juce::jlimit(left, segmentBounds.getRight(), previewProgressX);
            if (clipRight > left)
            {
              juce::Graphics::ScopedSaveState previewClip(g);
              g.reduceClipRegion(
                  juce::Rectangle<float>(left, segmentBounds.getY(),
                                         clipRight - left,
                                         segmentBounds.getHeight())
                      .getSmallestIntegerContainer());
              g.setColour(juce::Colours::white.withAlpha(0.20f));
              g.fillPath(wavePath);
            }
          }

          noFill.reset();

          // The contour follows the waveform. Selection is a brighter edge,
          // never a frame around the note.
          g.setColour(greyscale
                          ? juce::Colour(0xFF6E6E6Eu)
                          : (selected ? juce::Colours::white.withAlpha(0.96f)
                                      : juce::Colour(0xFF1E4C7Au).withAlpha(0.95f)));
          g.strokePath(wavePath, juce::PathStrokeType(selected ? 1.5f : 1.0f,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }
        else
        {
          // No waveform loaded: keep the note visible as a slim bar instead
          // of an empty rectangle.
          const auto bar = segmentBounds.withSizeKeepingCentre(
              renderedWidth, std::min(segmentBounds.getHeight(), 8.0f));
          bodyShape.addRoundedRectangle(bar, 3.0f);
          setNoteGradientFill(g, bar, bar.getCentreY(), noteColours);
          g.fillRoundedRectangle(bar, 3.0f);
          noFill.reset();
          g.setColour(selected ? juce::Colours::white.withAlpha(0.96f)
                               : juce::Colour(0xFF1E4C7Au).withAlpha(0.95f));
          g.drawRoundedRectangle(bar, 3.0f, selected ? 1.5f : 1.0f);
        }
      }
      else if (samples && totalSamples > 0 && w > 2.0f && endSample > startSample)
      {
        const float centerY = y + h * 0.5f;
        const float waveHeight = h * 3.0f;
        const int pointCount =
            std::max(2, std::min(2048, static_cast<int>(std::ceil(w * 2.0f)) + 1));
        waveValues = VisualWaveformEnvelope::build(
            samples, totalSamples, startSample, endSample, pointCount, w,
            audioData.sampleRate, pixelsPerSecond, true,
            pitchToolController ? pitchToolController->getAmplitudePreviewGain(note) : 1.0f);

        const size_t numPoints = waveValues.size();
        if (numPoints < 2)
        {
          if (isPreviewPlaybackNote)
            g.setColour(juce::Colours::white.withAlpha(0.20f));
          else
            setNoteGradientFill(g, noteVisualBounds,
                                noteVisualBounds.getCentreY(), noteColours);

          g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);

          if (isPreviewPlaybackNote)
          {
            const float previewProgressX =
                static_cast<float>(previewCurrentTime * pixelsPerSecond);
            const float clipRight =
                juce::jlimit(x, x + renderedWidth, previewProgressX);
            if (clipRight > x)
            {
              juce::Graphics::ScopedSaveState previewClipState(g);
              g.reduceClipRegion(
                  juce::Rectangle<float>(x, y, clipRight - x, h)
                      .getSmallestIntegerContainer());
              setNoteGradientFill(g, noteVisualBounds,
                                  noteVisualBounds.getCentreY(), noteColours);
              g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
            }
          }

          noteVisualBounds = {x, y, renderedWidth, h};
          bodyShape.addRoundedRectangle(x, y, renderedWidth, h, 2.0f);
        }
        else
        {
          auto catmullRom = [](float t, float p0, float p1, float p2,
                               float p3) -> float
          {
            const float t2 = t * t;
            const float t3 = t2 * t;
            return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                           (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                           (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
          };

          juce::Path waveformPath;

          waveformPath.startNewSubPath(
              x, centerY - waveValues[0] * waveHeight * 0.5f);

          constexpr int curveSegments = 4;
          for (size_t i = 0; i + 1 < numPoints; ++i)
          {
            const float px1 = (static_cast<float>(i) /
                               static_cast<float>(numPoints - 1)) *
                              w;
            const float px2 = (static_cast<float>(i + 1) /
                               static_cast<float>(numPoints - 1)) *
                              w;

            const size_t idx0 = (i > 0) ? i - 1 : i;
            const size_t idx1 = i;
            const size_t idx2 = i + 1;
            const size_t idx3 = (i + 2 < numPoints) ? i + 2 : i + 1;

            const float val0 = waveValues[idx0];
            const float val1 = waveValues[idx1];
            const float val2 = waveValues[idx2];
            const float val3 = waveValues[idx3];

            for (int seg = 1; seg <= curveSegments; ++seg)
            {
              const float t =
                  static_cast<float>(seg) / static_cast<float>(curveSegments);
              const float px = px1 + (px2 - px1) * t;
              const float val = catmullRom(t, val0, val1, val2, val3);
              const float yPos = centerY - val * waveHeight * 0.5f;
              waveformPath.lineTo(x + px, yPos);
            }
          }

          waveformPath.lineTo(x + w, centerY + waveValues[numPoints - 1] *
                                                   waveHeight * 0.5f);

          for (int i = static_cast<int>(numPoints) - 2; i >= 0; --i)
          {
            const float px1 = (static_cast<float>(i + 1) /
                               static_cast<float>(numPoints - 1)) *
                              w;
            const float px2 =
                (static_cast<float>(i) / static_cast<float>(numPoints - 1)) *
                w;

            const size_t idx0 = (i + 2 < numPoints) ? i + 2 : i + 1;
            const size_t idx1 = i + 1;
            const size_t idx2 = i;
            const size_t idx3 = (i > 0) ? i - 1 : i;

            const float val0 = waveValues[idx0];
            const float val1 = waveValues[idx1];
            const float val2 = waveValues[idx2];
            const float val3 = waveValues[idx3];

            for (int seg = 1; seg <= curveSegments; ++seg)
            {
              const float t =
                  static_cast<float>(seg) / static_cast<float>(curveSegments);
              const float px = px1 + (px2 - px1) * t;
              const float val = catmullRom(t, val0, val1, val2, val3);
              const float yPos = centerY + val * waveHeight * 0.5f;
              waveformPath.lineTo(x + px, yPos);
            }
          }

          waveformPath.closeSubPath();
          noteVisualBounds = waveformPath.getBounds();
          bodyShape = waveformPath;

          if (isPreviewPlaybackNote)
          {
            g.setColour(juce::Colours::white.withAlpha(0.20f));
            g.fillPath(waveformPath);

            const float previewProgressX =
                static_cast<float>(previewCurrentTime * pixelsPerSecond);
            const float clipRight =
                juce::jlimit(x, x + renderedWidth, previewProgressX);
            if (clipRight > x)
            {
              juce::Graphics::ScopedSaveState previewClipState(g);
              const auto clipBounds =
                  juce::Rectangle<float>(x - 2.0f, y - h * 2.0f,
                                         (clipRight - x) + 2.0f, h * 5.0f)
                      .getSmallestIntegerContainer();
              g.reduceClipRegion(clipBounds);
              setNoteGradientFill(g, noteVisualBounds, centerY, noteColours);
              g.fillPath(waveformPath);
            }
          }
          else
          {
            setNoteGradientFill(g, noteVisualBounds, centerY, noteColours);
            g.fillPath(waveformPath);
          }
        }
      }
      else
      {
        if (isPreviewPlaybackNote)
          g.setColour(juce::Colours::white.withAlpha(0.20f));
        else
          setNoteGradientFill(g, noteVisualBounds, noteVisualBounds.getCentreY(),
                              noteColours);

        g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);

        if (isPreviewPlaybackNote)
        {
          const float previewProgressX =
              static_cast<float>(previewCurrentTime * pixelsPerSecond);
          const float clipRight =
              juce::jlimit(x, x + renderedWidth, previewProgressX);
          if (clipRight > x)
          {
            juce::Graphics::ScopedSaveState previewClipState(g);
            g.reduceClipRegion(
                juce::Rectangle<float>(x, y, clipRight - x, h)
                    .getSmallestIntegerContainer());
            setNoteGradientFill(g, noteVisualBounds,
                                noteVisualBounds.getCentreY(), noteColours);
            g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
          }
        }

        noteVisualBounds = {x, y, renderedWidth, h};
        bodyShape.addRoundedRectangle(x, y, renderedWidth, h, 2.0f);
      }

      noFill.reset();
      if (engineBlockView && engineBlockMap != nullptr && !greyscale &&
          !bodyShape.isEmpty())
      {
        // D-309: border in the block's engine colour, following the real
        // silhouette. A dark thin underline keeps the bright colour readable
        // on the steel-blue body; a selected note gets a white outer halo
        // without losing the engine colour.
        const auto engine = visualBlock != nullptr
                                ? visualBlock->engine
                                : SynthesisEngineType::Psola;
        const auto colour = engineColours::forEngine(engine);
        const float borderWidth = blockActive ? 2.2f : 1.2f;

        if (note.isSelected())
        {
          g.setColour(juce::Colours::white.withAlpha(0.88f));
          g.strokePath(bodyShape,
                       juce::PathStrokeType(blockActive ? 4.8f : 4.2f,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
        }
        g.setColour(engineColours::underlineFor(engine));
        g.strokePath(bodyShape,
                     juce::PathStrokeType(borderWidth + 1.0f,
                                          juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
        g.setColour(blockActive ? colour.brighter(0.30f) : colour);
        g.strokePath(bodyShape,
                     juce::PathStrokeType(borderWidth,
                                          juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));

        // Render sweep: the border lights up from the first to the last note
        // of the block as the phase crosses it. One block uses its engine
        // colour; several blocks (or none resolved) use the neutral one.
        const bool inSweepRange =
            note.getEndFrame() >= sweepStartFrame &&
            note.getStartFrame() <= sweepEndFrame;
        const bool sweepLit =
            engineSweepActive && inSweepRange &&
            (x + renderedWidth * 0.5f) <= engineSweepBarX;
        if (sweepLit)
        {
          const auto sweepColour =
              sweepBorderColour.value_or(engineColours::neutralSweep());
          g.setColour(sweepColour.withAlpha(0.18f));
          g.fillPath(bodyShape);
          g.setColour(sweepColour);
          g.strokePath(bodyShape,
                       juce::PathStrokeType(blockActive ? 3.6f : 2.8f,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
        }
      }
      else if (outlineColour.has_value() && !bodyShape.isEmpty())
      {
        g.setColour(*outlineColour);
        g.strokePath(bodyShape, juce::PathStrokeType(1.0f));
      }

      if (showSelectionStatus && note.isSelected() &&
          !(segmentStyle && !greyscale))
      {
        const auto outlineBounds = noteVisualBounds.expanded(1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.72f));
        g.drawRoundedRectangle(outlineBounds, 2.0f, 1.5f);
      }
    }

    if (drawOverlays && segmentStyle && !greyscale && &note == hoveredNote &&
        !isDraggingNote)
    {
      // VariAudio shows the segment's pitch on the segment under the mouse.
      static const char *pitchNames[] = {"C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B"};
      const int midi = static_cast<int>(std::round(
          note.getAdjustedMidiNote() -
          ScaleUtils::getReferenceOffsetSemitones(pitchReferenceHz)));
      const juce::String pitchName =
          juce::String(pitchNames[((midi % 12) + 12) % 12]) +
          juce::String(midi / 12 - 1);
      const auto labelFont = AppFont::getBoldFont(13.0f);
      const float labelWidth =
          static_cast<float>(juce::GlyphArrangement::getStringWidthInt(
              labelFont, pitchName)) + 10.0f;
      const float labelHeight = 16.0f;
      // Inside the top of the segment, as in the manual's figure, so it does
      // not cover the straighten control above the top edge.
      auto label = juce::Rectangle<float>(labelWidth, labelHeight)
                       .withCentre({x + renderedWidth * 0.5f,
                                    y + 2.0f + labelHeight * 0.5f});
      g.setColour(juce::Colour(0xFF104DA1u));
      g.fillRoundedRectangle(label, 2.0f);
      g.setColour(juce::Colours::white);
      g.setFont(labelFont);
      g.drawText(pitchName, label, juce::Justification::centred, false);
    }

    const bool isSingleDragged =
        isSingleDragging && selectHandler->getDraggedNote() == &note;
    const bool isMultiDragged =
        isMultiDragging && draggedNotes &&
        std::find(draggedNotes->begin(), draggedNotes->end(), &note) !=
            draggedNotes->end();
    const bool shouldShowPitchTip =
        isSingleDragged || (isMultiDragged && hoveredMultiDragNote == &note);
    const bool isVibratoDragged =
        pitchToolController && pitchToolController->isDraggingVibrato() &&
        pitchToolController->getActiveHandleNote() == &note;
    const bool isTiltDragged =
        pitchToolController && pitchToolController->isDraggingTilt() &&
        pitchToolController->getActiveHandleNote() == &note;
    const bool isDriftDragged = pitchToolController && pitchToolController->isDragging() &&
        pitchToolController->getActiveHandleType() == PitchToolHandles::HandleType::PitchDrift &&
        pitchToolController->getActiveHandleNote() == &note;
    const bool isFormantDragged = pitchToolController && pitchToolController->isDragging() &&
        pitchToolController->getActiveHandleType() == PitchToolHandles::HandleType::Formant &&
        pitchToolController->getActiveHandleNote() == &note;
    const bool isAmplitudeDragged = pitchToolController && pitchToolController->isDragging() &&
        pitchToolController->getActiveHandleType() == PitchToolHandles::HandleType::Amplitude &&
        pitchToolController->getActiveHandleNote() == &note;
    const bool isCorrectDragged = pitchToolController && pitchToolController->isDragging() &&
        pitchToolController->getActiveHandleType() == PitchToolHandles::HandleType::CorrectPitch &&
        pitchToolController->getActiveHandleNote() == &note;
    if (drawOverlays && (shouldShowPitchTip || isVibratoDragged || isTiltDragged || isFormantDragged || isDriftDragged || isAmplitudeDragged || isCorrectDragged))
    {
      juce::String label;
      if (isCorrectDragged)
      {
        const float adjusted = note.getAdjustedMidiNote();
        const float cents = 100.0f * (adjusted - ScaleUtils::snapMidiToSemitone(
                                                     adjusted, pitchReferenceHz));
        label = (cents > 0.0f ? "+" : "") + juce::String(std::round(cents)) + " ct";
      }
      else if (isAmplitudeDragged)
      {
        const juce::String prefix = note.getVolumeDb() > 0.0f ? "+" : "";
        const float rms = note.getSourceAmplitude();
        const juce::String level = rms > 0.0f
            ? juce::String(20.0f * std::log10(rms) + note.getVolumeDb(), 1)
            : juce::String::fromUTF8("−∞");
        label = level + " dB (" + prefix +
                juce::String(note.getVolumeDb(), 1) + " dB)";
      }
      else if (isDriftDragged)
      {
        label = juce::String(std::round(note.getPitchDrift() * 100.0f)) + " %";
      }
      else if (isFormantDragged)
      {
        label = juce::String(note.getFormantShift(), 1) + " st";
      }
      else if (isVibratoDragged)
      {
        label = juce::String(std::round(note.getVibrato() * 100.0f)) + " %";
      }
      else if (isTiltDragged)
      {
        const float tilt =
            pitchToolController->getActiveHandleType() ==
                    PitchToolHandles::HandleType::TiltLeft
                ? note.getTiltFineLeft()
                : note.getTiltFineRight();
        const juce::String prefix = tilt > 0.0f ? "+" : "";
        label = prefix + juce::String(tilt, 1) + " st";
      }
      else
      {
        const float deltaSemitones =
            note.getAdjustedMidiNote() - note.getOriginalMidiNote();
        const juce::String prefix = deltaSemitones > 0.0f ? "+" : "";
        label = prefix + juce::String(deltaSemitones, 1) + " st";
      }

      const auto labelFont = AppFont::getFont(13.0f);
      const float labelWidth = std::max(
          60.0f, static_cast<float>(juce::GlyphArrangement::getStringWidthInt(
                     labelFont, label)) + 16.0f);
      constexpr float labelHeight = 20.0f;
      const float labelX = x + renderedWidth * 0.5f - labelWidth * 0.5f;
      const auto shadowBounds =
          getHighlightedShadowBounds(note, x, y, renderedWidth, w, h);
      const float labelY =
          shadowBounds.toFloat().getY() - labelHeight + 5.0f;

      g.setColour(juce::Colour(0xFF2E2E2Du));
      g.fillRoundedRectangle(labelX, labelY, labelWidth, labelHeight, 4.0f);
      g.setColour(juce::Colour(0xFFEFEFEFu));
      g.setFont(labelFont);
      g.drawFittedText(label, static_cast<int>(labelX),
                       static_cast<int>(labelY),
                       static_cast<int>(labelWidth),
                       static_cast<int>(labelHeight),
                       juce::Justification::centred, 1);
    }

    if (drawOverlays && showNoteFramesDebug && &note == hoveredNote)
    {
      const Note *nextNote = nullptr;
      for (const auto &candidate : project->getNotes())
      {
        if (candidate.isRest() || &candidate == &note ||
            candidate.getStartFrame() <= note.getStartFrame())
          continue;
        if (nextNote == nullptr ||
            candidate.getStartFrame() < nextNote->getStartFrame())
          nextNote = &candidate;
      }

      juce::String label =
          "begin " + juce::String(note.getStartFrame()) +
          "   end " + juce::String(note.getEndFrame()) +
          "   length " + juce::String(note.getDurationFrames());
      if (nextNote != nullptr)
      {
        const int gapFrames =
            nextNote->getStartFrame() - note.getEndFrame();
        label += "   next begin " + juce::String(nextNote->getStartFrame()) +
                 "   gap " + juce::String(gapFrames);
      }
      else
      {
        label += "   next begin —";
      }

      constexpr float preferredLabelWidth = 350.0f;
      constexpr float labelHeight = 22.0f;
      const float viewportLeft = static_cast<float>(scrollX) + 4.0f;
      const float viewportRight =
          static_cast<float>(scrollX) + static_cast<float>(componentWidth) -
          4.0f;
      const float labelWidth =
          std::min(preferredLabelWidth, viewportRight - viewportLeft);
      const float preferredX =
          x + renderedWidth * 0.5f - labelWidth * 0.5f;
      const float labelX =
          juce::jlimit(viewportLeft, viewportRight - labelWidth, preferredX);

      const auto hoverBounds =
          getHighlightedShadowBounds(note, x, y, renderedWidth, w, h).toFloat();
      const float viewportTop =
          static_cast<float>(coordMapper->getScrollY()) + 4.0f;
      float labelY = hoverBounds.getY() - labelHeight - 3.0f;
      if (labelY < viewportTop)
        labelY = hoverBounds.getBottom() + 3.0f;

      g.setColour(juce::Colour(0xE6282828u));
      g.fillRoundedRectangle(labelX, labelY, labelWidth, labelHeight, 4.0f);
      g.setColour(juce::Colour(0xFFF2F2F2u));
      g.setFont(AppFont::getFont(12.0f));
      g.drawFittedText(label, static_cast<int>(labelX),
                       static_cast<int>(labelY),
                       static_cast<int>(labelWidth),
                       static_cast<int>(labelHeight),
                       juce::Justification::centred, 1);
    }
  }

  // In split mode, show boundaries only for true contiguous splits. A gap in
  // either timeline or source frames starts a separate logical region, even
  // when GAME placed both notes in the same analysis chunk.
  if (drawOverlays && splitModeActive)
  {
    std::vector<const Note *> timelineNotes;
    timelineNotes.reserve(project->getNotes().size());
    for (const auto &note : project->getNotes())
    {
      if (!note.isRest())
        timelineNotes.push_back(&note);
    }
    std::sort(timelineNotes.begin(), timelineNotes.end(),
              [](const Note *left, const Note *right)
              {
                return left->getStartFrame() < right->getStartFrame();
              });

    const auto &chunkRanges = audioData.segmentChunkRanges;
    auto getRegionIndex = [&chunkRanges](const Note &note)
    {
      if (chunkRanges.empty())
        return 0;

      const int midpoint = note.getSrcStartFrame() +
                           std::max(0, note.getSrcDurationFrames()) / 2;
      for (size_t i = 0; i < chunkRanges.size(); ++i)
      {
        if (midpoint >= chunkRanges[i].first && midpoint < chunkRanges[i].second)
          return static_cast<int>(i);
      }
      return -1;
    };

    g.setColour(juce::Colour(0xFF9A9A9Au)); // Pitch-grid note-name colour
    for (size_t i = 0; i + 1 < timelineNotes.size(); ++i)
    {
      const auto &left = *timelineNotes[i];
      const auto &right = *timelineNotes[i + 1];
      const int leftRegion = getRegionIndex(left);
      if (leftRegion < 0 || leftRegion != getRegionIndex(right) ||
          !isContinuousSplitPair(left, right))
        continue;

      const float boundaryX =
          0.5f * framesToSeconds(left.getEndFrame() + right.getStartFrame()) *
          pixelsPerSecond;
      if (boundaryX < static_cast<float>(scrollX) ||
          boundaryX > static_cast<float>(scrollX + componentWidth))
        continue;

      const float leftY = coordMapper->midiToY(left.getAdjustedMidiNote());
      const float rightY = coordMapper->midiToY(right.getAdjustedMidiNote());
      const float lineTop = std::min(leftY, rightY);
      const float lineBottom = std::max(leftY, rightY) + pixelsPerSemitone;

      const bool hoveringMergeBoundary = splitHandler &&
          splitHandler->isMergeBoundary(&left, &right);
      if (hoveringMergeBoundary)
      {
        const int boundaryPixel = static_cast<int>(std::lround(boundaryX));
        int firstHighlightLeft = std::numeric_limits<int>::min();
        int secondHighlightRight = std::numeric_limits<int>::max();
        if (i > 0 && getRegionIndex(*timelineNotes[i - 1]) == leftRegion &&
            isContinuousSplitPair(*timelineNotes[i - 1], left))
        {
          firstHighlightLeft = static_cast<int>(std::lround(
              0.5f * framesToSeconds(timelineNotes[i - 1]->getEndFrame() +
                                      left.getStartFrame()) * pixelsPerSecond)) + 2;
        }
        if (i + 2 < timelineNotes.size() &&
            getRegionIndex(*timelineNotes[i + 2]) == leftRegion &&
            isContinuousSplitPair(right, *timelineNotes[i + 2]))
        {
          secondHighlightRight = static_cast<int>(std::lround(
              0.5f * framesToSeconds(right.getEndFrame() +
                                      timelineNotes[i + 2]->getStartFrame()) * pixelsPerSecond)) - 2;
        }

        const auto highlightNote = [&](const Note &note, float noteY,
                                       int leftLimit, int rightLimit)
        {
          const float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
          const float noteWidth = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
          const float renderedWidth = std::max(noteWidth, 4.0f);
          auto shadowBounds = getHighlightedShadowBounds(
              note, noteX, noteY, renderedWidth, noteWidth, pixelsPerSemitone);
          shadowBounds.setX(std::max(shadowBounds.getX(), leftLimit));
          shadowBounds.setRight(std::min(shadowBounds.getRight(), rightLimit));
          g.setColour(juce::Colours::white.withAlpha(0.08f));
          g.fillRoundedRectangle(shadowBounds.toFloat(), 3.0f);
        };

        highlightNote(left, leftY, firstHighlightLeft, boundaryPixel);
        highlightNote(right, rightY, boundaryPixel, secondHighlightRight);

        const auto drawAdjacentBoundary = [&](const Note &first, const Note &second)
        {
          const float adjacentBoundaryX = 0.5f * framesToSeconds(
              first.getEndFrame() + second.getStartFrame()) * pixelsPerSecond;
          const float firstY = coordMapper->midiToY(first.getAdjustedMidiNote());
          const float secondY = coordMapper->midiToY(second.getAdjustedMidiNote());
          g.setColour(juce::Colour(0xFF9A9A9Au));
          g.drawLine(adjacentBoundaryX, std::min(firstY, secondY),
                     adjacentBoundaryX,
                     std::max(firstY, secondY) + pixelsPerSemitone, 1.5f);
        };
        if (i > 0 && getRegionIndex(*timelineNotes[i - 1]) == leftRegion &&
            isContinuousSplitPair(*timelineNotes[i - 1], left))
          drawAdjacentBoundary(*timelineNotes[i - 1], left);
        if (i + 2 < timelineNotes.size() &&
            getRegionIndex(*timelineNotes[i + 2]) == leftRegion &&
            isContinuousSplitPair(right, *timelineNotes[i + 2]))
          drawAdjacentBoundary(right, *timelineNotes[i + 2]);
        continue;
      }

      g.drawLine(boundaryX, lineTop, boundaryX, lineBottom, 1.5f);
    }
  }

  // Draw split guide line when in split mode and hovering over a note
  if (drawOverlays && splitModeActive && splitHandler &&
      splitHandler->getSplitGuideNote() &&
      splitHandler->getSplitGuideX() >= 0)
  {
    auto *guideNote = splitHandler->getSplitGuideNote();
    const float guideX = splitHandler->getSplitGuideX();
    const int guideFrame = secondsToFrames(guideX / pixelsPerSecond);

    if (guideFrame > guideNote->getStartFrame() &&
        guideFrame < guideNote->getEndFrame())
    {
      const float noteY = coordMapper->midiToY(guideNote->getAdjustedMidiNote());
      const float noteH = pixelsPerSemitone;

      g.setColour(juce::Colour(0xFF9A9A9Au));
      g.drawLine(guideX, noteY, guideX, noteY + noteH, 1.5f);
    }
  }
}
