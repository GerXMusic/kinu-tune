#pragma once

#include <optional>

#include "../../JuceHeader.h"
#include "../../Audio/Synthesis/EngineBlocks.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

// Forward declarations to avoid header cycles. Implementations include the
// full handler headers in NoteRenderer.cpp.
class SelectHandler;
class SplitHandler;
class PitchEditor;
class BoxSelector;
class PitchToolController;

/**
 * Draws note bodies (with inline waveform), selection outlines, delta-scale
 * and delta-offset handles, drag-position labels, and the split-guide marker.
 *
 * Coupled to the interaction state of the piano roll, which is supplied via
 * handler pointers rather than copied into a params struct.
 */
class NoteRenderer
{
public:
  enum class Pass
  {
    Body,
    Overlay,
    HoverShadow,
    HoveredBody
  };

  NoteRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *m) { coordMapper = m; }
  void setProject(Project *p) { project = p; }
  void setSelectHandler(SelectHandler *h) { selectHandler = h; }
  void setSplitHandler(SplitHandler *h) { splitHandler = h; }
  void setPitchEditor(PitchEditor *e) { pitchEditor = e; }
  void setPitchToolController(PitchToolController *c) { pitchToolController = c; }
  void setBoxSelector(BoxSelector *b) { boxSelector = b; }
  void setHoveredNote(Note *note) { hoveredNote = note; }
  void setShowNoteFramesDebug(bool show) { showNoteFramesDebug = show; }
  // Inactive regions in Track mode: note bodies use a grey ramp (light along
  // the centre line, dark at the edges) instead of the pitch-accuracy colours.
  void setGreyscale(bool shouldUseGreyscale) { greyscale = shouldUseGreyscale; }
  // Tarea 3 (2026-11-22): vista de bypass. La nota se dibuja en su posicion y
  // altura de FUENTE (las ediciones no se ven), como Melodyne con el bypass.
  void setOriginalPitchView(bool shouldUseOriginal)
  {
    originalPitchView = shouldUseOriginal;
  }
  // Pinned tracks: stroke each note body in this colour. Combine with
  // setFillBodies(false) to draw the outline on its own.
  void setOutlineColour(std::optional<juce::Colour> colour) { outlineColour = colour; }
  void setFillBodies(bool shouldFill) { fillBodies = shouldFill; }
  // Cubase VariAudio look: flat rectangular segments on the pitch row and a
  // pitch-name label on the hovered segment, instead of waveform-shaped blobs.
  void setSegmentStyle(bool shouldUseSegments) { segmentStyle = shouldUseSegments; }
  void setPreviewPlaybackState(bool active, int startFrame, int endFrame,
                               double currentTime)
  {
    previewPlaybackActive = active;
    previewStartFrame = startFrame;
    previewEndFrame = endFrame;
    previewCurrentTime = currentTime;
  }

  // P1b fase 6: barrido del bloque que se esta renderizando. Sin barrido
  // (active == false) no se dibuja nada.
  void setSweepState(bool active, int startFrame, int endFrame, int remaining)
  {
    sweepActive = active;
    sweepStartFrame = startFrame;
    sweepEndFrame = endFrame;
    sweepRemaining = remaining;
    if (active && !wasSweepActive)
      sweepStartMs = juce::Time::getMillisecondCounterHiRes();
    wasSweepActive = active;
  }

  // D-309: engine drawer open. Every non-rest note gets a 1-1.5 px border in
  // the colour of its visual block (the D-258 silence-delimited phrase); the
  // block of a selected note is drawn thicker and brighter. The map belongs to
  // PianoRollComponent and outlives the paint.
  void setEngineBlockView(bool active,
                          const engineBlocks::VisualBlockMap *map,
                          std::vector<int> blocksOfSelection)
  {
    engineBlockView = active;
    engineBlockMap = active ? map : nullptr;
    activeEngineBlocks = std::move(blocksOfSelection);
  }

  // D-309: colour of the border sweep, already resolved by PianoRollComponent
  // (one visual block -> its engine colour; several blocks -> neutral). An
  // empty optional while the engine view is open means the neutral sweep.
  void setSweepBorderColour(std::optional<juce::Colour> colour)
  {
    sweepBorderColour = colour;
  }

  void draw(juce::Graphics &g, Pass pass, bool splitModeActive,
            int componentWidth);

private:
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;
  SelectHandler *selectHandler = nullptr;
  SplitHandler *splitHandler = nullptr;
  PitchEditor *pitchEditor = nullptr;
  PitchToolController *pitchToolController = nullptr;
  BoxSelector *boxSelector = nullptr;
  Note *hoveredNote = nullptr;
  bool showNoteFramesDebug = false;
  bool greyscale = false;
  bool originalPitchView = false;
  std::optional<juce::Colour> outlineColour;
  bool fillBodies = true;
  bool segmentStyle = true;
  bool previewPlaybackActive = false;
  int previewStartFrame = 0;
  int previewEndFrame = 0;
  double previewCurrentTime = 0.0;
  bool sweepActive = false;
  bool wasSweepActive = false;
  int sweepStartFrame = 0;
  int sweepEndFrame = 0;
  int sweepRemaining = 0;
  double sweepStartMs = 0.0;
  // D-309
  bool engineBlockView = false;
  const engineBlocks::VisualBlockMap *engineBlockMap = nullptr;
  std::vector<int> activeEngineBlocks;
  std::optional<juce::Colour> sweepBorderColour;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteRenderer)
};
