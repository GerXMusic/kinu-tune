#include "PianoRollComponent.h"
#include <algorithm>
#include <map>
#include "../Utils/PitchMacroScope.h"
#include "../Utils/AppLogger.h"
#include "../Undo/AmplitudeAction.h"
#include "../Undo/InspectorEditAction.h"
#include "../Utils/BasePitchCurve.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/Constants.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/TimecodeFont.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/ScaleUtils.h"
#include "../Utils/NotePitch.h"
#include "../Utils/HarmonyUtils.h"
#include "../Utils/TimingRegionUtils.h"
#include "../Utils/TimingQuantize.h"
#include "../Undo/TimingAction.h"
#include "../Undo/NoteMuteAction.h"
#include "../Undo/NoteDeleteAction.h"
#include "PianoRoll/PianoRollViewHelpers.h"
#include "PianoRoll/TimelineGridSnap.h"
#include "Components/AppFont.h"
#include "PianoRoll/VisualWaveformEnvelope.h"
#include "PianoRoll/States/LoopDragHandler.h"
#include "PianoRoll/States/SelectHandler.h"
#include "PianoRoll/States/SplitHandler.h"
#include "PianoRoll/States/PitchDrawingHandler.h"
#include "PianoRoll/States/TimingHandler.h"
#include "PianoRoll/PitchDrawingConfirmationPanel.h"
#include "Components/PitchPopupMenu.h"
#include "Components/CanvasPopupMenu.h"
#include "EngineColours.h"
#include "BinaryData.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
  juce::Image createSplitCursorImage()
  {
    auto image = juce::ImageFileFormat::loadFrom(
        BinaryData::splitcursor_png,
        static_cast<size_t>(BinaryData::splitcursor_pngSize));
    return image.rescaled(image.getWidth() / 2, image.getHeight() / 2,
                          juce::Graphics::highResamplingQuality);
  }

  juce::Image createMergeCursorImage()
  {
    auto image = juce::ImageFileFormat::loadFrom(
        BinaryData::mergecursor_png,
        static_cast<size_t>(BinaryData::mergecursor_pngSize));
    return image.rescaled(image.getWidth() / 2, (image.getHeight() + 1) / 2,
                          juce::Graphics::highResamplingQuality);
  }

  juce::Image createZoomCursorImage()
  {
    auto image = juce::ImageFileFormat::loadFrom(
        BinaryData::zoom_png,
        static_cast<size_t>(BinaryData::zoom_pngSize));
    return image.rescaled(image.getWidth() / 2, (image.getHeight() + 1) / 2,
                          juce::Graphics::highResamplingQuality);
  }

  using pianoRollView::getScaleAccentColour;
  using pianoRollView::isBlackKey;
  using pianoRollView::ScaleToneState;
  using pianoRollView::getScaleToneState;
  using pianoRollView::isMultipleOf;

  int normalizeTimelineBeatDenominator(int denominator)
  {
    denominator = juce::jlimit(1, 32, denominator);
    int normalized = 1;
    while (normalized < denominator)
      normalized <<= 1;
    const int lower = normalized >> 1;
    if (lower >= 1 && (denominator - lower) < (normalized - denominator))
      normalized = lower;
    return juce::jlimit(1, 32, normalized);
  }

  double gridDivisionToQuarterNotes(TimelineGridDivision division)
  {
    switch (division)
    {
    case TimelineGridDivision::Whole:
      return 4.0;
    case TimelineGridDivision::Half:
      return 2.0;
    case TimelineGridDivision::Quarter:
      return 1.0;
    case TimelineGridDivision::Eighth:
      return 0.5;
    case TimelineGridDivision::Sixteenth:
      return 0.25;
    case TimelineGridDivision::ThirtySecond:
      return 0.125;
    }
    return 1.0;
  }

  struct NoteEditState
  {
    float midiNote;
    float pitchOffset;
    // D-303: el centro de render tambien es parte del estado guardado; si
    // "restablecer" lo dejara fuera, la base quedaria desplazada.
    float renderCenterOffset;
    float volumeDb;
    float formantShift;
    float tilt;
    float tiltFineLeft;
    float tiltFineRight;
    float vibrato;
    float pitchDrift;
    int smoothLeftFrames;
    int smoothRightFrames;
    float deltaScale;
    float deltaOffset;
    std::vector<float> bakedDeltaPitch;
    std::vector<float> deltaPitch;
    float correctionSource; // Correct Pitch reference (the sung pitch)
    // D-268: la transicion al siguiente (D-225) tambien es una edicion guardada:
    // si no se limpia, la nota nunca es neutral y "restablecer" no devuelve la
    // fuente (medido en REAPER: 75 ms persistidos dejaban la nota fuera de
    // isNeutralForOriginalWaveform()).
    int transitionOutMs;
    // D-258/T6: restablecer limpia tambien el motor elegido y el mute. Sin
    // esto, "restablecer original" dejaba la nota re-sintetizada con su motor
    // o silenciada, y no devolvia la fuente exacta.
    NoteEngine engine;
    bool muted;
    NoteEngine blockEngineOverride;

    static NoteEditState capture(const Note& note)
    {
      return {note.getMidiNote(), note.getPitchOffset(),
              note.getRenderCenterOffset(), note.getVolumeDb(),
              note.getFormantShift(),
              note.getTilt(), note.getTiltFineLeft(), note.getTiltFineRight(),
              note.getVibrato(), note.getPitchDrift(),
              note.getSmoothLeftFrames(), note.getSmoothRightFrames(),
              note.getDeltaScale(), note.getDeltaOffset(),
              note.getBakedDeltaPitch(), note.getDeltaPitch(),
              note.getLastNonMacroMidiNote(), note.getTransitionOutMs(),
              note.getEngine(), note.isMuted(),
              note.getBlockEngineOverride()};
    }

    static NoteEditState defaultsFor(const Note& note)
    {
      return {note.getOriginalMidiNote(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
              0.0f, 1.0f, 1.0f, 0, 0, 1.0f, 0.0f, {},
              note.getOriginalDeltaPitch(), note.getOriginalMidiNote(), 0,
              NoteEngine::Inherit, false, NoteEngine::Inherit};
    }

    void applyTo(Note& note) const
    {
      NotePitch::restore(note, midiNote, correctionSource);
      note.setPitchOffset(pitchOffset);
      note.setRenderCenterOffset(renderCenterOffset);
      note.setVolumeDb(volumeDb);
      note.setFormantShift(formantShift);
      note.setTilt(tilt);
      note.setTiltFineLeft(tiltFineLeft);
      note.setTiltFineRight(tiltFineRight);
      note.setVibrato(vibrato);
      note.setPitchDrift(pitchDrift);
      note.setSmoothLeftFrames(smoothLeftFrames);
      note.setSmoothRightFrames(smoothRightFrames);
      note.setDeltaScale(deltaScale);
      note.setDeltaOffset(deltaOffset);
      note.setBakedDeltaPitch(bakedDeltaPitch);
      note.setDeltaPitch(deltaPitch);
      note.setTransitionOutMs(transitionOutMs);
      note.setEngine(engine);
      note.setMuted(muted);
      note.setBlockEngineOverride(blockEngineOverride);
      note.markDirty();
      note.markSynthDirty();
    }
  };

  // Note::deltaPitch is only the active contour when the note has no
  // analysed originalDeltaPitch (legacy projects). Otherwise it is a scratch
  // copy that stays empty after a fresh scan and is filled from the global
  // curve when a drag starts, so comparing it to the original would flag
  // every untouched note as pitch-edited. Real contour edits live in
  // bakedDeltaPitch, which is compared separately.
  bool hasUnrenderedDeltaEdit(const Note& note, const NoteEditState& current,
                              const NoteEditState& defaults)
  {
    if (note.hasOriginalDeltaPitch())
      return false;
    return current.deltaPitch != defaults.deltaPitch;
  }

  bool hasPitchEdits(const Note& note)
  {
    const auto current = NoteEditState::capture(note);
    const auto defaults = NoteEditState::defaultsFor(note);
    return current.midiNote != defaults.midiNote ||
           current.pitchOffset != defaults.pitchOffset ||
           current.renderCenterOffset != defaults.renderCenterOffset ||
           current.transitionOutMs != defaults.transitionOutMs ||
           current.tilt != defaults.tilt ||
           current.tiltFineLeft != defaults.tiltFineLeft ||
           current.tiltFineRight != defaults.tiltFineRight ||
           current.vibrato != defaults.vibrato ||
           current.pitchDrift != defaults.pitchDrift ||
           current.smoothLeftFrames != defaults.smoothLeftFrames ||
           current.smoothRightFrames != defaults.smoothRightFrames ||
           current.deltaScale != defaults.deltaScale ||
           current.deltaOffset != defaults.deltaOffset ||
           current.bakedDeltaPitch != defaults.bakedDeltaPitch ||
           hasUnrenderedDeltaEdit(note, current, defaults);
  }

  bool hasFormantEdits(const Note& note) { return note.getFormantShift() != 0.0f; }
  bool hasAmplitudeEdits(const Note& note) { return note.getVolumeDb() != 0.0f; }
  bool hasTimingEdits(const Note& note)
  {
    return note.getStartFrame() != note.getSrcStartFrame() ||
           note.getEndFrame() != note.getSrcEndFrame();
  }

  class ResetNoteEditsAction final : public UndoableAction
  {
  public:
    ResetNoteEditsAction(Project& project, std::vector<Note*> notes,
                         bool pitch, bool formant, bool amplitude,
                         juce::String name, std::unique_ptr<UndoableAction> timing = {})
        : project(project), notes(std::move(notes)), name(std::move(name)),
          timing(std::move(timing))
    {
      before.reserve(this->notes.size());
      after.reserve(this->notes.size());
      for (const auto* note : this->notes)
      {
        before.push_back(NoteEditState::capture(*note));
        auto state = pitch ? NoteEditState::defaultsFor(*note) : before.back();
        state.formantShift = formant ? 0.0f : before.back().formantShift;
        state.volumeDb = amplitude ? 0.0f : before.back().volumeDb;
        after.push_back(std::move(state));
      }
    }

    void undo() override { if (timing) timing->undo(); apply(before); }
    void redo() override { apply(after); if (timing) timing->redo(); }
    juce::String getName() const override { return name; }

  private:
    void apply(const std::vector<NoteEditState>& states)
    {
      int dirtyStart = std::numeric_limits<int>::max();
      int dirtyEnd = std::numeric_limits<int>::min();
      for (size_t i = 0; i < notes.size() && i < states.size(); ++i)
      {
        if (!notes[i])
          continue;
        states[i].applyTo(*notes[i]);
        dirtyStart = std::min(dirtyStart, notes[i]->getStartFrame());
        dirtyEnd = std::max(dirtyEnd, notes[i]->getEndFrame());
      }
      project.setModified(true);
      PitchCurveProcessor::rebuildBaseFromNotes(project);
      // Same range the edit and its undo mark (Project::markNoteEditDirtyRange).
      project.markNoteEditDirtyRange(dirtyStart, dirtyEnd);
    }

    Project& project;
    std::vector<Note*> notes;
    std::vector<NoteEditState> before;
    std::vector<NoteEditState> after;
    juce::String name;
    std::unique_ptr<UndoableAction> timing;
  };
}

PianoRollComponent::PianoRollComponent()
    : splitMouseCursor(createSplitCursorImage(), 10, 8),
      mergeMouseCursor(createMergeCursorImage(), 10, 8),
      zoomMouseCursor(createZoomCursorImage(), 6, 6)
{
  // Initialize modular components
  coordMapper = std::make_unique<CoordinateMapper>();
  pianoKeysRenderer = std::make_unique<PianoKeysRenderer>();
  gridRenderer = std::make_unique<GridRenderer>();
  timelineRenderer = std::make_unique<TimelineRenderer>();
  waveformBackgroundRenderer = std::make_unique<WaveformBackgroundRenderer>();
  noteRenderer = std::make_unique<NoteRenderer>();
  pitchCurveRenderer = std::make_unique<PitchCurveRenderer>();
  // Track mode draws inactive regions with their own renderer instances. They
  // get no interaction handlers, so drags, hovers and selections on the
  // active region can never show up in them.
  previewNoteRenderer = std::make_unique<NoteRenderer>();
  previewCurveRenderer = std::make_unique<PitchCurveRenderer>();
  previewNoteRenderer->setGreyscale(false);
  scrollZoomController = std::make_unique<ScrollZoomController>();
  pitchEditor = std::make_unique<PitchEditor>();
  boxSelector = std::make_unique<BoxSelector>();
  noteSplitter = std::make_unique<NoteSplitter>();
  pitchToolHandles = std::make_unique<PitchToolHandles>();
  pitchToolController = std::make_unique<PitchToolController>();
  interactionContext = std::make_unique<PianoRollInteractionContext>(*this);

  // Initialize interaction handlers
  loopDragHandler_ = std::make_unique<LoopDragHandler>(*this);
  selectHandler_ = std::make_unique<SelectHandler>(*this);
  splitHandler_ = std::make_unique<SplitHandler>(*this);
  pitchDrawingHandler_ = std::make_unique<PitchDrawingHandler>(*this);
  timingHandler_ = std::make_unique<TimingHandler>(*this);
  currentHandler_ = selectHandler_.get();

  // Wire up components
  pianoKeysRenderer->setCoordinateMapper(coordMapper.get());
  gridRenderer->setCoordinateMapper(coordMapper.get());
  timelineRenderer->setCoordinateMapper(coordMapper.get());
  waveformBackgroundRenderer->setCoordinateMapper(coordMapper.get());
  waveformBackgroundRenderer->setPitchToolController(pitchToolController.get());
  // Lets the waveform renderer request a full-quality rebuild once a zoom settles.
  waveformBackgroundRenderer->onRepaintRequested = [this] { repaint(); };
  noteRenderer->setCoordinateMapper(coordMapper.get());
  noteRenderer->setSelectHandler(selectHandler_.get());
  noteRenderer->setSplitHandler(splitHandler_.get());
  noteRenderer->setPitchEditor(pitchEditor.get());
  noteRenderer->setPitchToolController(pitchToolController.get());
  noteRenderer->setBoxSelector(boxSelector.get());
  pitchCurveRenderer->setCoordinateMapper(coordMapper.get());
  previewNoteRenderer->setCoordinateMapper(coordMapper.get());
  previewCurveRenderer->setCoordinateMapper(coordMapper.get());
  pitchCurveRenderer->setSelectHandler(selectHandler_.get());
  pitchCurveRenderer->setPitchEditor(pitchEditor.get());
  scrollZoomController->setCoordinateMapper(coordMapper.get());
  pitchEditor->setCoordinateMapper(coordMapper.get());
  noteSplitter->setCoordinateMapper(coordMapper.get());

  // Setup scrollZoomController callbacks
  scrollZoomController->onRepaintNeeded = [this]()
  { repaint(); };
  scrollZoomController->onZoomChanged = [this](float pps)
  {
    if (onZoomChanged)
      onZoomChanged(pps);
  };
  scrollZoomController->onScrollChanged = [this](double x)
  {
    if (onScrollChanged)
      onScrollChanged(x);
  };

  // Setup pitchEditor callbacks
  pitchEditor->onNoteSelected = [this](Note *note)
  {
    if (onNoteSelected)
      onNoteSelected(note);
  };
  pitchEditor->onPitchEdited = [this]()
  {
    repaint();
    if (onPitchEdited)
      onPitchEdited();
  };
  pitchEditor->onPitchEditFinished = [this]()
  {
    if (onPitchEditFinished)
      onPitchEditFinished();
  };
  pitchEditor->onBasePitchCacheInvalidated = [this]()
  {
    invalidateBasePitchCache();
  };

  // Setup pitchToolController callbacks
  pitchToolController->onPitchEdited = [this]()
  {
    invalidateWaveformCache();
    updatePitchToolHandlesFromSelection();
    updatePreviewButtonBounds();
    repaint();
    if (onPitchEdited)
      onPitchEdited();
  };

  pitchToolController->onAmplitudeEdited = [this]()
  {
    invalidateWaveformCache();
    updatePitchToolHandlesFromSelection();
    updatePreviewButtonBounds();
    repaint();
    if (onAmplitudeEdited) onAmplitudeEdited();
  };

  // Setup noteSplitter callbacks
  noteSplitter->onNoteSplit = [this]()
  {
    invalidateBasePitchCache();
    repaint();
  };

  addAndMakeVisible(horizontalScrollBar);
  addAndMakeVisible(verticalScrollBar);

  // Use scrollZoomController's scrollbars
  addAndMakeVisible(scrollZoomController->getHorizontalScrollBar());
  addAndMakeVisible(scrollZoomController->getVerticalScrollBar());

  horizontalScrollBar.addListener(this);
  verticalScrollBar.addListener(this);

  // Style scrollbars to match theme
  styleCanvasScrollBar(horizontalScrollBar);
  styleCanvasScrollBar(verticalScrollBar);

  // Set initial scroll range
  verticalScrollBar.setRangeLimits(0, (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) *
                                          pixelsPerSemitone);
  verticalScrollBar.setCurrentRange(0, 500);

  // Default view centered on C3-C4 (MIDI 48-60)
  centerOnPitchRange(48.0f, 60.0f);

  // Enable keyboard focus for shortcuts
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);

  // No extra controls here; overview lives outside the piano roll.
  int previewImageSize = 0;
  if (const auto *previewImageData =
          BinaryData::getNamedResource("preview_png", previewImageSize))
  {
    previewButton.setImage(
        juce::ImageFileFormat::loadFrom(previewImageData, previewImageSize));
    previewButtonWidth = std::max(1, previewButton.getWidth());
    previewButtonHeight = std::max(1, previewButton.getHeight());
  }
  previewButton.setTooltip(TR("tooltip.context_audition"));
  previewButton.setVisible(false);
  previewButton.onClick = [this]()
  {
    if (hoveredNote)
      triggerPreviewForNote(*hoveredNote);
  };
  addAndMakeVisible(previewButton);

  int resetImageSize = 0;
  if (const auto *resetImageData =
          BinaryData::getNamedResource("reset_png", resetImageSize))
  {
    resetButton.setImage(
        juce::ImageFileFormat::loadFrom(resetImageData, resetImageSize));
    resetButtonWidth = std::max(1, resetButton.getWidth());
    resetButtonHeight = std::max(1, resetButton.getHeight());
  }
  resetButton.setTooltip(TR("tooltip.restore_original"));
  resetButton.setVisible(false);
  resetButton.onClick = [this]()
  {
    if (hoveredNote)
      showResetMenu(*hoveredNote);
  };
  addAndMakeVisible(resetButton);

  pitchDrawingConfirmationPanel = std::make_unique<PitchDrawingConfirmationPanel>();
  pitchDrawingConfirmationPanel->onApply = [this]
  {
    if (!pitchDrawingHandler_ || !pitchDrawingHandler_->apply())
      return;

    // The preview has already been rendered. Confirmation only promotes the
    // current transient state into undo history and persistent project state.
    invalidateBasePitchCache();
    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditCommitted)
      onPitchEditCommitted();
  };
  pitchDrawingConfirmationPanel->onCancel = [this]
  {
    if (pitchDrawingHandler_)
      pitchDrawingHandler_->cancel();
  };
  addAndMakeVisible(*pitchDrawingConfirmationPanel);
  pitchDrawingConfirmationPanel->setVisible(false);
}

PianoRollComponent::~PianoRollComponent()
{
  horizontalScrollBar.removeListener(this);
  verticalScrollBar.removeListener(this);
}

int PianoRollComponent::getVisibleContentWidth() const
{
  constexpr int verticalScrollBarSize = APP_SCROLLBAR_THICKNESS;
  return std::max(0, getWidth() - pianoKeysWidth - verticalScrollBarSize);
}

int PianoRollComponent::getVisibleContentHeight() const
{
  constexpr int horizontalScrollBarSize = APP_SCROLLBAR_THICKNESS;
  return std::max(0, getHeight() - headerHeight -
                         (showHorizontalScrollBar ? horizontalScrollBarSize : 0));
}

void PianoRollComponent::setHorizontalScrollBarVisible(bool shouldShow)
{
  if (showHorizontalScrollBar == shouldShow)
    return;

  showHorizontalScrollBar = shouldShow;
  horizontalScrollBar.setVisible(showHorizontalScrollBar);
  resized();
  repaint();
}

void PianoRollComponent::paint(juce::Graphics &g)
{
  // D-180: measurement only.
  const double paintStartMs = juce::Time::getMillisecondCounterHiRes();
  ++paintCount;

  updatePitchToolHandlesFromSelection();

  // Background (solid to keep grid clean)
  g.fillAll(APP_COLOR_BACKGROUND);
  g.setColour(juce::Colour(0xFF232323u));
  g.fillRect(0, 0, getWidth(), headerHeight);

  const int horizontalScrollBarSize =
      showHorizontalScrollBar ? APP_SCROLLBAR_THICKNESS : 0;
  constexpr int verticalScrollBarSize = APP_SCROLLBAR_THICKNESS;
  auto contentBounds = getLocalBounds();

  g.setColour(APP_COLOR_SCROLLBAR_TRACK);
  g.fillRect(getWidth() - verticalScrollBarSize, 0, verticalScrollBarSize,
             headerHeight);
  g.fillRect(getWidth() - verticalScrollBarSize, headerHeight,
             verticalScrollBarSize,
             getHeight() - headerHeight - horizontalScrollBarSize);
  if (showHorizontalScrollBar)
  {
    g.fillRect(0, getHeight() - horizontalScrollBarSize, pianoKeysWidth,
               horizontalScrollBarSize);
    g.fillRect(pianoKeysWidth, getHeight() - horizontalScrollBarSize,
               getWidth() - pianoKeysWidth - verticalScrollBarSize,
               horizontalScrollBarSize);
    g.fillRect(getWidth() - verticalScrollBarSize,
               getHeight() - horizontalScrollBarSize,
               verticalScrollBarSize, horizontalScrollBarSize);
  }

  // Create clipping region for main area (below timelines)
  auto mainArea = contentBounds
                      .withTrimmedLeft(pianoKeysWidth)
                      .withTrimmedTop(headerHeight)
                      .withTrimmedBottom(horizontalScrollBarSize)
                      .withTrimmedRight(verticalScrollBarSize);

  // Draw row backgrounds first, then waveform, then grid lines/content.
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    g.setOrigin(pianoKeysWidth - static_cast<int>(scrollX),
                headerHeight - static_cast<int>(scrollY));
    drawGrid(g, true, false);
  }

  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    // Melodyne carries the waveform inside each note, not as a full-canvas
    // backdrop; drawing the take twice made the editor read as a waveform
    // viewer. Keep the hook for the debug views, but off by default.
    // drawBackgroundWaveform(g, mainArea);
  }

  // Draw scrolled content (grid lines, notes, pitch curves, handles)
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    g.setOrigin(pianoKeysWidth - static_cast<int>(scrollX),
                headerHeight - static_cast<int>(scrollY));

    drawGrid(g, false, true);
    drawMidiGuideGhosts(g);
    drawRegionPreviews(g);
    drawInactiveRegionEdits(g, mainArea);
    drawPinnedTrackNotes(g, mainArea);
    drawAudioSourceRegionOverlay(g);
    drawLoopOverlay(g);

    // Track mode shows the active modification only through its region's
    // window; the rest of the take would sit on top of the other regions.
    std::optional<juce::Graphics::ScopedSaveState> trackClip;
    if (const auto span = getTrackModeActiveSpan())
    {
      trackClip.emplace(g);
      const float height =
          (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
      g.reduceClipRegion(juce::Rectangle<float>(
                             timeToX(span->first), 0.0f,
                             timeToX(span->second) - timeToX(span->first),
                             height)
                             .getSmallestIntegerContainer());
    }

    drawGameChunksDebugOverlay(g);
    if (bypassView)
    {
      // Tarea 3: las notas en gris y en su posicion/altura de fuente (como la
      // melodia original sin modificar) y la curva F0 de la fuente en gris.
      drawNotes(g, NoteRenderPass::Body);
      drawBypassSourceCurve(g);
    }
    else
    {
      drawNotes(g, NoteRenderPass::Body);
      drawNotes(g, NoteRenderPass::HoverShadow);
      drawNotes(g, NoteRenderPass::HoveredBody);
      drawPitchCurves(g);
    }
    if (currentHandler_)
      currentHandler_->draw(g);
    if (!bypassView)
      drawNotes(g, NoteRenderPass::Overlay);
    drawGameValuesDebugOverlay(g);
    drawSelectionRect(g);

    // Draw pitch tool handles in world space (transform applied by g.setOrigin above)
    if (editMode == EditMode::Select && pitchToolHandles && !pitchToolHandles->isEmpty())
    {
      pitchToolHandles->draw(g);
    }
  }

  if (trackGestureActive && trackBoxSelecting) {
    const auto rect = juce::Rectangle<float>(trackGestureStart, trackGestureEnd);
    g.setColour(juce::Colours::white.withAlpha(0.05f)); g.fillRect(rect);
    g.setColour(juce::Colours::white.withAlpha(0.25f)); g.drawRect(rect);
  }
  // Draw timeline (above grid, scrolls horizontally)
  drawTimeline(g);
  drawLoopTimeline(g);

  // Timing boundaries use the full canvas height. Hide the stationary
  // playhead in Timing mode so it cannot be mistaken for an editable edge.
  if (editMode != EditMode::Timing || playbackActive)
  {
    float x = static_cast<float>(pianoKeysWidth) + timeToX(cursorTime) -
              static_cast<float>(scrollX);
    float cursorTop = static_cast<float>(headerHeight);
    float cursorBottom =
        static_cast<float>(getHeight() -
                           horizontalScrollBarSize); // Exclude scrollbar

    // Only draw if cursor is in visible area
    if (x >= pianoKeysWidth && x < getWidth() - verticalScrollBarSize)
    {
      g.setColour(APP_COLOR_PITCH_CURVE);
      g.fillRect(x - 0.5f, cursorTop, 1.0f, cursorBottom - cursorTop);
    }
  }

  // Draw piano keys
  drawPianoKeys(g);

  g.setColour(juce::Colour(0xFF0D0B0Bu));
  g.drawVerticalLine(pianoKeysWidth, 0.0f,
                     static_cast<float>(getHeight() - horizontalScrollBarSize));

  const auto canvasBorderBounds = getLocalBounds()
                                      .withTrimmedRight(verticalScrollBarSize)
                                      .withTrimmedBottom(horizontalScrollBarSize);
  g.setColour(juce::Colour(0xFF3C3C3Cu));
  g.drawRect(canvasBorderBounds, 1);

  // D-180: measurement only.
  lastPaintDurationMs =
      juce::Time::getMillisecondCounterHiRes() - paintStartMs;
  maxPaintDurationMs = std::max(maxPaintDurationMs, lastPaintDurationMs);
}

void PianoRollComponent::resized()
{
  auto bounds = getLocalBounds();
  const int horizontalScrollBarSize =
      showHorizontalScrollBar ? APP_SCROLLBAR_THICKNESS : 0;
  constexpr int verticalScrollBarSize = APP_SCROLLBAR_THICKNESS;

  if (showHorizontalScrollBar)
  {
    horizontalScrollBar.setBounds(
        pianoKeysWidth, bounds.getHeight() - horizontalScrollBarSize,
        bounds.getWidth() - pianoKeysWidth - verticalScrollBarSize,
        horizontalScrollBarSize);
  }
  else
  {
    horizontalScrollBar.setBounds({});
  }

  verticalScrollBar.setBounds(
      bounds.getWidth() - verticalScrollBarSize, headerHeight,
      verticalScrollBarSize,
      bounds.getHeight() - horizontalScrollBarSize - headerHeight);

  updateScrollBars();
  updatePreviewButtonBounds();
  updatePitchDrawingConfirmationPopup();
}

void PianoRollComponent::drawBackgroundWaveform(
    juce::Graphics &g, const juce::Rectangle<int> &visibleArea)
{
  // Cheap when unchanged; picks up a region resize restamping the span.
  waveformBackgroundRenderer->setProjectDrawRange(getTrackModeActiveSpan());
  waveformBackgroundRenderer->draw(g, visibleArea);
}

void PianoRollComponent::invalidateWaveformCache()
{
  waveformBackgroundRenderer->invalidateCache();
}

void PianoRollComponent::beginMacroCurvePreview(const std::vector<Note*>& notes)
{
  pitchCurveRenderer->beginMacroPreview(notes);
}

void PianoRollComponent::beginInactiveMacroPreview(
    const std::vector<TrackPitchMacroTarget>& targets)
{
  if (!trackViewMode || !regionPreviews || !inactiveMacroPreviews.empty()) return;
  for (const auto& region : *regionPreviews) {
    if (region.active || region.pinned || !region.project) continue;
    auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& item)
        { return item.project == region.sourceProject; });
    if (target == targets.end() || (target->isValid && !target->isValid())) continue;
    InactiveMacroPreview preview;
    preview.project = region.project;
    preview.isValid = target->isValid;
    std::multimap<int, const Note*> sourceNotes;
    for (const auto& note : target->project->getNotes())
      sourceNotes.emplace(note.getStartFrame(), &note);
    for (auto& display : region.project->getNotes()) {
      const auto matches = sourceNotes.equal_range(display.getStartFrame() + region.sourceFirstFrame);
      for (auto it = matches.first; it != matches.second; ++it)
        if (it->second->getEndFrame() == display.getEndFrame() + region.sourceFirstFrame) {
          preview.bindings.push_back({&display, it->second,
                                     display.getMidiNote(), display.getPitchDrift()});
          break;
        }
    }
    inactiveMacroPreviews.push_back(std::move(preview));
  }
  lastInactiveMacroUpdate = 0.0;
}

void PianoRollComponent::endMacroCurvePreview()
{
  pitchCurveRenderer->endMacroPreview();
  for (auto& preview : inactiveMacroPreviews)
    for (const auto& binding : preview.bindings) {
      NotePitch::applyCorrection(*binding.display, binding.originalMidi);
      binding.display->setPitchDrift(binding.originalDrift);
    }
  inactiveMacroPreviews.clear();
  inactiveLayer = {};
}

void PianoRollComponent::invalidateBasePitchCache()
{
  pitchCurveRenderer->invalidateBasePitchCache();
}

void PianoRollComponent::drawGrid(juce::Graphics &g, bool drawRowBackgrounds,
                                  bool drawGridLines)
{
  GridRenderer::Params params;
  const bool scaleViewActive =
      selectedScaleMode != ScaleMode::None &&
      selectedScaleMode != ScaleMode::Chromatic;
  params.scaleMode = scaleViewActive
                         ? previewScaleMode.value_or(selectedScaleMode)
                         : selectedScaleMode;
  params.scaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  params.pitchAxisOffsetSemitones =
      ScaleUtils::getReferenceOffsetSemitones(pitchReferenceHz);
  params.timelineDisplayMode = timelineDisplayMode;
  params.tempoMap = hostTempoMap != nullptr && hostTempoMap->isValid()
                        ? hostTempoMap.get()
                        : nullptr;
  params.gridQuarterNotes = getTimelineGridQuarterNotes();
  params.gridSeconds = getTimelineGridSeconds();
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.timelineDuration = getTimelineDuration();
  params.componentWidth = getWidth();
  params.visibleContentWidth = getVisibleContentWidth();
  params.visibleContentHeight = getVisibleContentHeight();
  params.drawRowBackgrounds = drawRowBackgrounds;
  params.drawGridLines = drawGridLines;
  gridRenderer->draw(g, params);
}

void PianoRollComponent::drawAudioSourceRegionOverlay(juce::Graphics &g)
{
  if (trackViewMode) return;
  if (!project)
    return;

  const auto &audioData = project->getAudioData();
  const double duration = static_cast<double>(audioData.getDuration());
  if (audioData.waveform.getNumSamples() <= 0 || duration <= 0.0)
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  auto drawRange = [&](double startSeconds, double endSeconds,
                       bool drawBoundaries) {
    startSeconds = std::max(0.0, startSeconds);
    endSeconds = std::max(startSeconds, endSeconds);
    if (endSeconds <= startSeconds)
      return;
    const float startX = timeToX(startSeconds);
    const float endX = timeToX(endSeconds);
    // No backdrop fill: with the take no longer drawn behind the canvas, a
    // translucent rectangle across the region read as a stray box. Only the
    // source boundaries stay, faintly.
    if (drawBoundaries) {
      g.setColour(juce::Colours::white.withAlpha(0.12f));
      g.fillRect(startX - 0.5f, 0.0f, 1.0f, height);
      g.fillRect(endX - 0.5f, 0.0f, 1.0f, height);
    }
  };

  if (!audioData.playbackRegionRanges.empty()) {
    for (const auto &[start, end] : audioData.playbackRegionRanges)
      drawRange(start, end, true);
  } else {
    // Standalone is one continuous source: shade all visible bars as its
    // region and do not show a false audio-end boundary in the canvas.
    const double visibleEndSeconds =
        xToTime(static_cast<float>(scrollX + getVisibleContentWidth()));
    drawRange(audioData.timelineOffsetSeconds,
              std::max(duration, visibleEndSeconds), false);
  }
}

void PianoRollComponent::setRegionPreviews(MainViewRegionPreviewList previews)
{
  regionPreviews = std::move(previews);
  waveformBackgroundRenderer->setRegionPreviews(
      withoutPinnedRegions(regionPreviews));
  regionPreviewsEndSeconds = 0.0;
  if (regionPreviews)
    for (const auto &region : *regionPreviews)
      regionPreviewsEndSeconds =
          std::max(regionPreviewsEndSeconds, region.endSeconds);

  // The pointer may now sit over a region that changed state.
  if (swallowGestureUntilMouseUp && !isMouseButtonDown())
    swallowGestureUntilMouseUp = false;

  // N2: reused window copies can predate the current selection (the preview
  // cache is keyed by the project revision, not by selection), so the flags are
  // mirrored again after every publication.
  syncSelectionToRegionCopies();

  updateScrollBars();
  repaint();
}

const MainViewRegionPreview *PianoRollComponent::findActiveRegionPreview() const
{
  if (!regionPreviews)
    return nullptr;
  for (const auto &region : *regionPreviews)
    if (region.active)
      return &region;
  return nullptr;
}

bool PianoRollComponent::previewContains(const MainViewRegionPreview &region,
                                         double projectSeconds) const
{
  // Previews are placed in host-timeline seconds; the canvas is in the active
  // modification's time.
  return region.contains(projectToTimeline(projectSeconds));
}

const MainViewRegionPreview* PianoRollComponent::findTrackRegionAtPointer(
    juce::Point<float> point, bool notesOnly) const
{
  if (!trackViewMode || !regionPreviews) return nullptr;
  const double hostTime = projectToTimeline(xToTime(point.x - pianoKeysWidth + scrollX));
  const float worldY = point.y - headerHeight + scrollY;
  const MainViewRegionPreview* background = nullptr;
  // Stable drawing order, with actual note hits before empty region space.
  // The routing/bound-region flag must not influence interaction priority.
  for (auto it = regionPreviews->rbegin(); it != regionPreviews->rend(); ++it) {
    const auto& region = *it;
    if (region.pinned || !region.contains(hostTime)) continue;
    if (!background) background = &region;
    const bool live = region.sourceProject && region.sourceProject == project;
    const Project* display = live ? project : region.project.get();
    if (!display) continue;
    const double offset = region.projectHostOffsetSeconds -
        (live ? framesToSeconds(region.sourceFirstFrame) : 0.0);
    for (const auto& note : display->getNotes()) {
      if (note.isRest()) continue;
      const double start = framesToSeconds(note.getVisualStartFrame()) + offset;
      const double end = framesToSeconds(note.getVisualEndFrame()) + offset;
      const float top = coordMapper->midiToY(note.getAdjustedMidiNote());
      if (hostTime >= start && hostTime < end && worldY >= top && worldY < top + pixelsPerSemitone)
        return &region;
    }
  }
  return notesOnly ? nullptr : background;
}

bool PianoRollComponent::prepareTrackPointer(juce::Point<float> point, bool notesOnly)
{
  if (!trackViewMode || !onInactiveRegionClicked || trackGestureActive || isMouseButtonDown()) return false;
  const float worldX = point.x - pianoKeysWidth + scrollX;
  const float worldY = point.y - headerHeight + scrollY;
  if (hoveredNote && pitchToolHandles && pitchToolHandles->containsLayoutPoint(worldX, worldY))
    return false;
  if (pitchDrawingHandler_ && pitchDrawingHandler_->hasAnchors()) return false;
  const auto* region = findTrackRegionAtPointer(point, notesOnly);
  if (!region) return false;
  const double offset = region->projectHostOffsetSeconds - framesToSeconds(region->sourceFirstFrame);
  const auto* boundWindow = findActiveRegionPreview();
  if (region->sourceProject == project && juce::approximatelyEqual(offset, projectToTimeline(0.0)) &&
      boundWindow && boundWindow->key == region->key)
    return false;
  const auto key = region->key;
  const auto* before = project;
  onInactiveRegionClicked(key);
  return before != project || !juce::approximatelyEqual(offset, projectToTimeline(0.0));
}

void PianoRollComponent::drawInactiveRegionEdits(
    juce::Graphics &g, const juce::Rectangle<int> &mainArea)
{
  if (!trackViewMode || !regionPreviews || regionPreviews->empty() ||
      mainArea.isEmpty() || pixelsPerSecond <= 0.0f)
    return;

  // Preserve the active region's waveform geometry with a light-centred grey
  // note gradient, then desaturate the remaining edits in the cached layer.
  // Playhead and hover repaints only blit it.
  InactiveLayerKey key;
  key.scrollX = scrollX;
  key.scrollY = scrollY;
  key.pixelsPerSecond = pixelsPerSecond;
  key.pixelsPerSemitone = pixelsPerSemitone;
  key.width = mainArea.getWidth();
  key.height = mainArea.getHeight();
  key.previews = regionPreviews.get();
  key.displayOffset = projectToTimeline(0.0);
  key.showDeltaPitch = showDeltaPitch;
  key.showBasePitch = showBasePitch;

  const double now = juce::Time::getMillisecondCounterHiRes();
  const bool updateMacro = !inactiveMacroPreviews.empty() &&
                          now - lastInactiveMacroUpdate >= 33.0;
  if (updateMacro) lastInactiveMacroUpdate = now;
  if (!inactiveLayer.isValid() || !(key == inactiveLayerKey) || updateMacro)
  {
    inactiveLayerKey = key;
    inactiveLayer = juce::Image(juce::Image::ARGB, key.width, key.height, true);
    juce::Graphics lg(inactiveLayer);

    const double visibleHostStart =
        projectToTimeline(scrollX / pixelsPerSecond);
    const double visibleHostEnd = projectToTimeline(
        (scrollX + key.width) / pixelsPerSecond);
    const float height =
        (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
    const double savedScrollX = coordMapper->getScrollX();

    for (const auto &region : *regionPreviews)
    {
      if (region.active || region.pinned || region.project == nullptr ||
          region.endSeconds <= visibleHostStart ||
          region.startSeconds >= visibleHostEnd)
        continue;

      // Draw the region's window as if the canvas were scrolled onto it: the
      // window project's zero sits at this canvas time.
      const double zero = timelineToProject(region.projectHostOffsetSeconds);
      const double localScrollX = scrollX - zero * pixelsPerSecond;
      coordMapper->setScrollX(localScrollX);

      juce::Graphics::ScopedSaveState state(lg);
      lg.addTransform(juce::AffineTransform::translation(
          static_cast<float>(-localScrollX), static_cast<float>(-scrollY)));
      const double windowStart =
          region.startSeconds - region.projectHostOffsetSeconds;
      const double windowEnd =
          region.endSeconds - region.projectHostOffsetSeconds;
      lg.reduceClipRegion(
          juce::Rectangle<float>(
              static_cast<float>(windowStart * pixelsPerSecond), 0.0f,
              static_cast<float>((windowEnd - windowStart) * pixelsPerSecond),
              height)
              .getSmallestIntegerContainer());

      auto* curveRenderer = previewCurveRenderer.get();
      auto macro = std::find_if(inactiveMacroPreviews.begin(), inactiveMacroPreviews.end(),
          [&](const auto& item) { return item.project == region.project; });
      if (macro != inactiveMacroPreviews.end() &&
          (!macro->isValid || macro->isValid())) {
        if (!macro->renderer) {
          macro->renderer = std::make_unique<PitchCurveRenderer>();
          macro->renderer->setCoordinateMapper(coordMapper.get());
          macro->renderer->setProject(region.project.get());
          std::vector<Note*> notes;
          for (const auto& binding : macro->bindings) notes.push_back(binding.display);
          macro->renderer->beginMacroPreview(notes);
        }
        for (const auto& binding : macro->bindings) {
          NotePitch::applyCorrection(*binding.display, binding.source->getMidiNote());
          binding.display->setPitchDrift(binding.source->getPitchDrift());
        }
        curveRenderer = macro->renderer.get();
      } else {
        previewCurveRenderer->setProject(region.project.get());
      }
      previewNoteRenderer->setProject(region.project.get());
      previewNoteRenderer->draw(lg, NoteRenderer::Pass::Body, false, getWidth());

      PitchCurveRenderer::Params params;
      params.showDeltaPitch = showDeltaPitch;
      params.showBasePitch = showBasePitch;
      params.showUvInterpolationDebug = false;
      params.showActualF0Debug = false;
      params.showCleanedF0Debug = false;
      params.showVocoderF0Debug = false;
      params.componentWidth = getWidth();
      curveRenderer->draw(lg, params);
    }

    coordMapper->setScrollX(savedScrollX);
    previewNoteRenderer->setProject(nullptr);
    previewCurveRenderer->setProject(nullptr);

  }

  // World coordinates: the graphics origin is already scrolled. JUCE draws
  // images with the current colour's opacity, and the region outlines just
  // set a faint one - so set it explicitly. Slightly below full opacity to
  // keep inactive regions a step darker than the active one.
  constexpr float inactiveLayerOpacity = 1.0f;
  juce::Graphics::ScopedSaveState opaque(g);
  g.setOpacity(inactiveLayerOpacity);
  g.drawImageAt(inactiveLayer, static_cast<int>(scrollX),
                static_cast<int>(scrollY));
}

void PianoRollComponent::drawPinnedTrackNotes(
    juce::Graphics &g, const juce::Rectangle<int> &mainArea)
{
  if (!trackViewMode || !regionPreviews || regionPreviews->empty() ||
      mainArea.isEmpty() || pixelsPerSecond <= 0.0f)
    return;

  bool anyPinned = false;
  for (const auto &region : *regionPreviews)
    anyPinned = anyPinned || (region.pinned && region.project != nullptr);
  if (!anyPinned)
    return;

  InactiveLayerKey key;
  key.scrollX = scrollX;
  key.scrollY = scrollY;
  key.pixelsPerSecond = pixelsPerSecond;
  key.pixelsPerSemitone = pixelsPerSemitone;
  key.width = mainArea.getWidth();
  key.height = mainArea.getHeight();
  key.previews = regionPreviews.get();
  key.displayOffset = projectToTimeline(0.0);

  if (!pinnedLayer.isValid() || !(key == pinnedLayerKey))
  {
    pinnedLayerKey = key;
    // Bodies first, into their own image, so they can be dimmed exactly like
    // inactive regions; the outlines then go on top at full strength.
    juce::Image bodies(juce::Image::ARGB, key.width, key.height, true);
    pinnedLayer = juce::Image(juce::Image::ARGB, key.width, key.height, true);

    const double visibleHostStart =
        projectToTimeline(scrollX / pixelsPerSecond);
    const double visibleHostEnd = projectToTimeline(
        (scrollX + key.width) / pixelsPerSecond);
    const float height =
        (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
    const double savedScrollX = coordMapper->getScrollX();

    const auto drawPass = [&](juce::Image &target, bool outlines)
    {
      juce::Graphics lg(target);
      for (const auto &region : *regionPreviews)
      {
        if (!region.pinned || region.project == nullptr ||
            region.endSeconds <= visibleHostStart ||
            region.startSeconds >= visibleHostEnd)
          continue;

        // Same placement as drawInactiveRegionEdits().
        const double zero = timelineToProject(region.projectHostOffsetSeconds);
        const double localScrollX = scrollX - zero * pixelsPerSecond;
        coordMapper->setScrollX(localScrollX);

        juce::Graphics::ScopedSaveState state(lg);
        lg.addTransform(juce::AffineTransform::translation(
            static_cast<float>(-localScrollX), static_cast<float>(-scrollY)));
        const double windowStart =
            region.startSeconds - region.projectHostOffsetSeconds;
        const double windowEnd =
            region.endSeconds - region.projectHostOffsetSeconds;
        lg.reduceClipRegion(
            juce::Rectangle<float>(
                static_cast<float>(windowStart * pixelsPerSecond) - 2.0f, 0.0f,
                static_cast<float>((windowEnd - windowStart) * pixelsPerSecond) + 4.0f,
                height)
                .getSmallestIntegerContainer());

        previewNoteRenderer->setProject(region.project.get());
        previewNoteRenderer->setFillBodies(!outlines);
        previewNoteRenderer->setOutlineColour(
            outlines ? std::optional<juce::Colour>(region.outlineColour)
                     : std::nullopt);
        previewNoteRenderer->draw(lg, NoteRenderer::Pass::Body, false,
                                  getWidth());
      }
    };

    drawPass(bodies, false);
    bodies.desaturate();
    {
      // Darker than inactive regions (0.6), so pinned notes read as
      // background reference; the outline stays at full strength.
      constexpr float pinnedBodyOpacity = 0.35f;
      juce::Graphics lg(pinnedLayer);
      lg.setOpacity(pinnedBodyOpacity);
      lg.drawImageAt(bodies, 0, 0);
    }
    drawPass(pinnedLayer, true);

    coordMapper->setScrollX(savedScrollX);
    previewNoteRenderer->setProject(nullptr);
    previewNoteRenderer->setFillBodies(true);
    previewNoteRenderer->setOutlineColour(std::nullopt);
  }

  juce::Graphics::ScopedSaveState opaque(g);
  g.setOpacity(1.0f);
  g.drawImageAt(pinnedLayer, static_cast<int>(scrollX),
                static_cast<int>(scrollY));
}

void PianoRollComponent::drawRegionPreviews(juce::Graphics &g)
{
  if (!regionPreviews || regionPreviews->empty())
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  // Previews carry host-timeline seconds; compare in host time and place
  // them on the canvas through the active modification's display offset.
  const double visibleStart =
      projectToTimeline(xToTime(static_cast<float>(scrollX)));
  const double visibleEnd = projectToTimeline(
      xToTime(static_cast<float>(scrollX + getVisibleContentWidth())));
  const auto hx = [this](double hostSeconds) {
    return timeToX(timelineToProject(hostSeconds));
  };

  // drawAudioSourceRegionOverlay() paints the active backdrop from the canvas
  // project. While that project is missing (analysis pending) the active
  // region still gets its backdrop from the preview.
  const bool projectDrawsActiveBackdrop =
      project != nullptr &&
      project->getAudioData().waveform.getNumSamples() > 0 &&
      project->getAudioData().getDuration() > 0.0f;

  for (const auto &region : *regionPreviews)
  {
    // Pinned tracks draw notes only; their region edges would clutter this
    // track's own boundaries.
    if (region.pinned)
      continue;
    if (region.endSeconds <= visibleStart || region.startSeconds >= visibleEnd ||
        region.endSeconds <= region.startSeconds)
      continue;

    const float x1 = hx(region.startSeconds);
    const float x2 = hx(region.endSeconds);

    // The active region no longer gets a translucent backdrop (it read as a
    // stray rectangle); only its boundaries remain. Inactive regions keep a
    // faint wash so they still read as separate takes.
    if (!region.active)
    {
      g.setColour(juce::Colours::white.withAlpha(0.03f));
      g.fillRect(x1, 0.0f, x2 - x1, height);
    }
    g.setColour(juce::Colours::white.withAlpha(region.active ? 0.12f : 0.20f));
    g.fillRect(x1 - 0.5f, 0.0f, 1.0f, height);
    g.fillRect(x2 - 0.5f, 0.0f, 1.0f, height);

  }
}

void PianoRollComponent::drawLoopOverlay(juce::Graphics &g)
{
  if (!project)
    return;

  double loopStartSeconds = 0.0;
  double loopEndSeconds = 0.0;
  bool loopEnabled = false;
  if (loopDragHandler_ && loopDragHandler_->isDragging())
  {
    loopStartSeconds = loopDragHandler_->getDragStartSeconds();
    loopEndSeconds = loopDragHandler_->getDragEndSeconds();
    loopEnabled = true;
  }
  else
  {
    const auto &loopRange = project->getLoopRange();
    loopStartSeconds = loopRange.startSeconds;
    loopEndSeconds = loopRange.endSeconds;
    loopEnabled = loopRange.enabled;
  }

  if (loopStartSeconds > loopEndSeconds)
    std::swap(loopStartSeconds, loopEndSeconds);

  if (loopEndSeconds <= loopStartSeconds)
    return;

  if (!loopEnabled)
    return;

  const float startX = timeToX(loopStartSeconds);
  const float endX = timeToX(loopEndSeconds);

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  g.setColour(juce::Colours::white.withAlpha(0.04f));
  g.fillRect(startX, 0.0f, endX - startX, height);
}

void PianoRollComponent::drawGameChunksDebugOverlay(juce::Graphics &g)
{
  if (!showSegmentsDebug || !project)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.segmentChunkRanges.empty())
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  g.setColour(juce::Colours::orange.withAlpha(0.10f));
  for (const auto &range : audioData.segmentChunkRanges)
  {
    int startFrame = std::max(0, range.first);
    int endFrame = std::max(startFrame, range.second);
    if (endFrame <= startFrame)
      continue;

    const float x1 = framesToSeconds(startFrame) * pixelsPerSecond;
    const float x2 = framesToSeconds(endFrame) * pixelsPerSecond;
    g.fillRect(x1, 0.0f, std::max(1.0f, x2 - x1), height);
  }

  g.setColour(juce::Colours::orange.withAlpha(0.75f));
  for (const auto &range : audioData.segmentChunkRanges)
  {
    int startFrame = std::max(0, range.first);
    int endFrame = std::max(startFrame, range.second);
    if (endFrame <= startFrame)
      continue;

    const float x = framesToSeconds(startFrame) * pixelsPerSecond;
    g.drawVerticalLine(static_cast<int>(x), 0.0f, height);
  }
}

void PianoRollComponent::drawGameValuesDebugOverlay(juce::Graphics &g)
{
  if (!showGameValuesDebug || !project)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.segmentDebugChunks.empty())
    return;

  const int totalFrames = static_cast<int>(audioData.f0.size());
  if (totalFrames <= 0)
    return;

  const int visibleStartFrame = std::max(
      0, static_cast<int>(scrollX / pixelsPerSecond * audioData.sampleRate /
                          HOP_SIZE));
  const int visibleEndFrame = std::min(
      totalFrames, static_cast<int>((scrollX + getVisibleContentWidth()) /
                                    pixelsPerSecond * audioData.sampleRate /
                                    HOP_SIZE) +
                       1);
  if (visibleEndFrame <= visibleStartFrame)
    return;

  const float contentHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  g.setFont(juce::FontOptions(10.5f));
  const int maxChunks = 60;
  int chunksDrawn = 0;

  for (const auto &chunk : audioData.segmentDebugChunks)
  {
    const int startFrame = std::max(0, chunk.startFrame);
    const int endFrame = std::max(startFrame, chunk.endFrame);
    if (endFrame <= startFrame)
      continue;
    if (endFrame <= visibleStartFrame || startFrame >= visibleEndFrame)
      continue;

    int noteCount = 0;
    int restCount = 0;
    int eventLabelsInChunk = 0;

    const float x1 = framesToSeconds(startFrame) * pixelsPerSecond;
    const float x2 = framesToSeconds(endFrame) * pixelsPerSecond;
    const float width = x2 - x1;
    if (width < 8.0f)
      continue;

    const float chunkSeconds =
        static_cast<float>(endFrame - startFrame) * HOP_SIZE /
        static_cast<float>(audioData.sampleRate);

    // Chunk boundary and chunk-level debug label.
    g.setColour(juce::Colours::orange.withAlpha(0.78f));
    g.drawVerticalLine(static_cast<int>(x1), 0.0f, contentHeight);

    // Raw GAME event markers/labels inside this chunk.
    for (size_t i = 0; i < chunk.events.size(); ++i)
    {
      const auto &ev = chunk.events[i];
      if (ev.endFrame <= startFrame || ev.startFrame >= endFrame)
        continue;

      const int overlapStart = std::max(startFrame, ev.startFrame);
      const int overlapEnd = std::min(endFrame, ev.endFrame);
      if (overlapEnd <= overlapStart)
        continue;

      const float ex1 = framesToSeconds(overlapStart) * pixelsPerSecond;
      const float ex2 = framesToSeconds(overlapEnd) * pixelsPerSecond;
      const float ew = std::max(1.0f, ex2 - ex1);

      if (ev.isRest)
      {
        ++restCount;
        // Red: rest segments placed on nearby note lane (not at top).
        float anchorMidi = 60.0f;
        bool foundAnchor = false;
        for (int k = static_cast<int>(i) - 1; k >= 0; --k)
        {
          if (!chunk.events[static_cast<size_t>(k)].isRest)
          {
            anchorMidi = chunk.events[static_cast<size_t>(k)].midiNote;
            foundAnchor = true;
            break;
          }
        }
        if (!foundAnchor)
        {
          for (size_t k = i + 1; k < chunk.events.size(); ++k)
          {
            if (!chunk.events[k].isRest)
            {
              anchorMidi = chunk.events[k].midiNote;
              foundAnchor = true;
              break;
            }
          }
        }
        anchorMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                                  static_cast<float>(MAX_MIDI_NOTE),
                                  anchorMidi);
        const float yCenter =
            midiToY(anchorMidi) + pixelsPerSemitone * 0.5f;
        const float restBandHeight = std::max(6.0f, pixelsPerSemitone * 0.62f);
        const float restBandTop = yCenter - restBandHeight * 0.5f;
        g.setColour(juce::Colours::red.withAlpha(0.55f));
        g.fillRect(ex1, restBandTop, ew, restBandHeight);
        g.setColour(juce::Colours::red.withAlpha(0.95f));
        g.drawVerticalLine(static_cast<int>(ex1), restBandTop,
                           restBandTop + restBandHeight);

        if (ew > 40.0f)
        {
          juce::String restTag = "rest";
          if (i == 0)
            restTag = "pre-rest";
          else if (i + 1 == chunk.events.size())
            restTag = "post-rest";
          g.setColour(juce::Colours::white.withAlpha(0.95f));
          g.drawFittedText(restTag + " d:" + juce::String(overlapEnd - overlapStart),
                           static_cast<int>(ex1) + 2,
                           static_cast<int>(restBandTop),
                           static_cast<int>(ew) - 3,
                           static_cast<int>(restBandHeight),
                           juce::Justification::centredLeft, 1, 0.85f);
        }
        else if (ew > 12.0f)
        {
          g.setColour(juce::Colours::white.withAlpha(0.95f));
          g.drawFittedText("R", static_cast<int>(ex1) + 1,
                           static_cast<int>(restBandTop),
                           static_cast<int>(ew) - 1,
                           static_cast<int>(restBandHeight),
                           juce::Justification::centredLeft, 1, 1.0f);
        }
        continue;
      }

      ++noteCount;

      // Black: midi segments (placed on their pitch row).
      const float noteMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                                          static_cast<float>(MAX_MIDI_NOTE),
                                          ev.midiNote);
      const float yCenter = midiToY(noteMidi) + pixelsPerSemitone * 0.5f;
      const float h = std::max(6.0f, pixelsPerSemitone * 0.72f);
      const float ny = yCenter - h * 0.5f;
      g.setColour(juce::Colours::black.withAlpha(0.84f));
      g.fillRoundedRectangle(ex1, ny, ew, h, 2.0f);

      if (ew > 60.0f && eventLabelsInChunk < 80)
      {
        const juce::String noteLabel =
            "ev#" + juce::String(static_cast<int>(i)) + " m:" +
            juce::String(ev.midiNote, 2) + " f:" + juce::String(overlapStart) +
            "-" + juce::String(overlapEnd) + " d:" +
            juce::String(overlapEnd - overlapStart) + " att:" +
            juce::String(ev.attachedStartFrame) + " durS:" +
            juce::String(ev.durationSeconds, 3);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawFittedText(noteLabel, static_cast<int>(ex1) + 3,
                         static_cast<int>(ny) - 15,
                         static_cast<int>(std::min(320.0f, ew)), 13,
                         juce::Justification::centredLeft, 1, 0.70f);
        ++eventLabelsInChunk;
      }
      else if (ew > 22.0f)
      {
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawFittedText("m:" + juce::String(ev.midiNote, 1),
                         static_cast<int>(ex1) + 2, static_cast<int>(ny),
                         static_cast<int>(ew) - 3, static_cast<int>(h),
                         juce::Justification::centredLeft, 1, 0.9f);
      }
      else if (ew > 8.0f)
      {
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawVerticalLine(static_cast<int>(ex1 + 0.5f), ny, ny + h);
      }
    }

    const juce::String label =
        "S" + juce::String(chunk.chunkIndex) + " f:" +
        juce::String(startFrame) + "-" + juce::String(endFrame) + " len:" +
        juce::String(endFrame - startFrame) + "f/" +
        juce::String(chunkSeconds, 2) + "s n:" + juce::String(noteCount) +
        " r:" + juce::String(restCount) + " ev:" +
        juce::String(static_cast<int>(chunk.events.size())) + " rstTh:" +
        juce::String(chunk.shortRestThreshold);

    const int textX = static_cast<int>(x1 + 3.0f);
    const int textY = 16;
    const int textWidth = std::max(40, static_cast<int>(width - 6.0f));
    const int textHeight = 14;

    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRect(static_cast<float>(textX), static_cast<float>(textY),
               static_cast<float>(textWidth), static_cast<float>(textHeight));
    g.setColour(juce::Colours::white.withAlpha(0.96f));
    g.drawFittedText(label, textX + 2, textY, textWidth - 4, textHeight,
                     juce::Justification::centredLeft, 1, 0.8f);

    ++chunksDrawn;
    if (chunksDrawn >= maxChunks)
      break;
  }
}

void PianoRollComponent::drawTimeline(juce::Graphics &g)
{
  TimelineRenderer::TimelineParams params;
  params.displayMode = timelineDisplayMode;
  params.tempoMap = hostTempoMap != nullptr && hostTempoMap->isValid()
                        ? hostTempoMap.get()
                        : nullptr;
  params.gridQuarterNotes = getTimelineGridQuarterNotes();
  params.beatNumerator = timelineBeatNumerator;
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.componentWidth = getWidth();
  timelineRenderer->drawTimeline(g, params);
}

void PianoRollComponent::drawLoopTimeline(juce::Graphics &g)
{
  TimelineRenderer::LoopParams params;
  params.displayMode = timelineDisplayMode;
  params.tempoMap = hostTempoMap != nullptr && hostTempoMap->isValid()
                        ? hostTempoMap.get()
                        : nullptr;
  params.gridQuarterNotes = getTimelineGridQuarterNotes();
  params.beatNumerator = timelineBeatNumerator;
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.componentWidth = getWidth();
  params.loopStartSeconds = 0.0;
  params.loopEndSeconds = 0.0;
  params.loopEnabled = false;
  // Match prior behaviour: if there's no project, draw only the empty gutter.
  if (project)
  {
    if (loopDragHandler_ && loopDragHandler_->isDragging())
    {
      params.loopStartSeconds = loopDragHandler_->getDragStartSeconds();
      params.loopEndSeconds = loopDragHandler_->getDragEndSeconds();
      params.loopEnabled = true;
    }
    else
    {
      const auto &loopRange = project->getLoopRange();
      params.loopStartSeconds = loopRange.startSeconds;
      params.loopEndSeconds = loopRange.endSeconds;
      params.loopEnabled = loopRange.enabled;
    }
  }
  timelineRenderer->drawLoopTimeline(g, params);
}

void PianoRollComponent::drawNotes(juce::Graphics &g, NoteRenderPass pass)
{
  auto rendererPass = NoteRenderer::Pass::Overlay;
  if (pass == NoteRenderPass::Body)
    rendererPass = NoteRenderer::Pass::Body;
  else if (pass == NoteRenderPass::HoverShadow)
    rendererPass = NoteRenderer::Pass::HoverShadow;
  else if (pass == NoteRenderPass::HoveredBody)
    rendererPass = NoteRenderer::Pass::HoveredBody;

  const bool splitModeActive = editMode == EditMode::Split;
  noteRenderer->setHoveredNote(hoveredNote);
  noteRenderer->setShowNoteFramesDebug(showNoteFramesDebug);
  // Tarea 3: con el bypass, las notas se ven en gris y en su posicion y altura
  // de fuente (la melodia original, sin las modificaciones).
  noteRenderer->setGreyscale(bypassView);
  noteRenderer->setOriginalPitchView(bypassView);
  noteRenderer->setPreviewPlaybackState(previewPlaybackActive,
                                        previewPlaybackStartFrame,
                                        previewPlaybackEndFrame,
                                        previewPlaybackCurrentTime);
  // D-309: the engine drawer pushes the visual block map and the blocks of the
  // explicit selection; the renderer only looks them up per note.
  if (engineViewActive)
  {
    const auto &map = ensureEngineBlockMap();
    noteRenderer->setEngineBlockView(true, &map,
                                     getActiveEngineBlockIndices());
  }
  else
  {
    noteRenderer->setEngineBlockView(false, nullptr, {});
  }
  // D-309: inactive regions never paint engine borders, but they must not
  // draw the white sweep line either while the engine drawer is open.
  if (previewNoteRenderer)
    previewNoteRenderer->setEngineBlockView(engineViewActive, nullptr, {});
  noteRenderer->draw(g, rendererPass, splitModeActive, getWidth());
}

std::vector<float> PianoRollComponent::getBypassCurveCents(int firstFrame,
                                                         int lastFrame) const
{
  std::vector<float> curve;
  if (project == nullptr || lastFrame <= firstFrame)
    return curve;
  const auto &audio = project->getAudioData();
  const auto &f0 = audio.denseF0.empty() ? audio.f0 : audio.denseF0;
  const int total = static_cast<int>(f0.size());
  const int first = juce::jlimit(0, total, firstFrame);
  const int last = juce::jlimit(first, total, lastFrame);
  curve.reserve(static_cast<size_t>(last - first));
  for (int frame = first; frame < last; ++frame)
  {
    const float hz = f0[static_cast<size_t>(frame)];
    const float midi = hz > 0.0f ? 69.0f + 12.0f * std::log2(hz / 440.0f) : 0.0f;
    curve.push_back(midi * 100.0f);
  }
  return curve;
}

void PianoRollComponent::drawBypassSourceCurve(juce::Graphics &g)
{
  if (project == nullptr)
    return;
  const auto &audio = project->getAudioData();
  const auto &f0 = audio.denseF0.empty() ? audio.f0 : audio.denseF0;
  if (f0.empty() || audio.sampleRate <= 0.0)
    return;
  const auto &voiced = audio.voicedMask;
  const double frameSeconds =
      static_cast<double>(HOP_SIZE) / audio.sampleRate;
  juce::Path path;
  bool started = false;
  for (int frame = 0; frame < static_cast<int>(f0.size()); ++frame)
  {
    const bool isVoiced = voiced.empty() ||
                          (frame < static_cast<int>(voiced.size()) &&
                           static_cast<bool>(voiced[static_cast<size_t>(frame)]));
    const float hz = f0[static_cast<size_t>(frame)];
    if (!isVoiced || hz <= 0.0f)
    {
      started = false;
      continue;
    }
    const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const float x = timeToX(frame * frameSeconds);
    // midiToY() devuelve el borde superior de la fila; el centro esta medio
    // semitono mas abajo (mismo criterio que NoteRenderer). Sin esto la curva
    // de la fuente se dibujaba medio semitono por encima de las notas.
    const float y = midiToY(midi) + pixelsPerSemitone * 0.5f;
    if (!started)
    {
      path.startNewSubPath(x, y);
      started = true;
    }
    else
    {
      path.lineTo(x, y);
    }
  }
  g.setColour(juce::Colour(0xFF9A9A9A));
  g.strokePath(path, juce::PathStrokeType(1.5f));
}

void PianoRollComponent::setEngineViewActive(bool active)
{
  if (engineViewActive == active)
    return;
  engineViewActive = active;
  if (engineViewActive)
    ensureEngineBlockMap(); // build before the first paint, not inside it
  repaint();
}

void PianoRollComponent::setGlobalSynthesisEngine(SynthesisEngineType engine)
{
  if (globalSynthesisEngine == engine)
    return;
  globalSynthesisEngine = engine;
  engineBlockCache.valid = false; // the fingerprint also covers it
  if (engineViewActive)
  {
    ensureEngineBlockMap();
    repaint();
  }
}

juce::uint64 PianoRollComponent::computeEngineBlockFingerprint() const
{
  if (project == nullptr)
    return 0;
  // FNV-1a over the cheap fields: times, stored engines, rest/deleted, the
  // global engine and the source buffer. Silence is never scanned here; only
  // when this changes.
  juce::uint64 hash = 1469598103934665603ull;
  auto mix = [&hash](juce::uint64 value)
  {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto &notes = project->getNotes();
  mix(static_cast<juce::uint64>(notes.size()));
  mix(static_cast<juce::uint64>(static_cast<int>(globalSynthesisEngine)));
  const auto &audio = project->getAudioData();
  const auto &source = audio.originalWaveform.getNumSamples() > 0
                           ? audio.originalWaveform
                           : audio.waveform;
  mix(static_cast<juce::uint64>(source.getNumSamples()));
  mix(static_cast<juce::uint64>(
      reinterpret_cast<juce::pointer_sized_int>(source.getReadPointer(0))));
  mix(static_cast<juce::uint64>(static_cast<int>(audio.sampleRate)));
  for (const auto &note : notes)
  {
    mix(static_cast<juce::uint64>(note.getStartFrame()));
    mix(static_cast<juce::uint64>(note.getEndFrame()));
    mix(static_cast<juce::uint64>(note.getSrcStartFrame()));
    mix(static_cast<juce::uint64>(note.getSrcEndFrame()));
    mix(static_cast<juce::uint64>(static_cast<int>(note.getEngine()) + 1));
    mix(note.isRest() ? 1u : 0u);
    mix(note.isDeleted() ? 1u : 0u);
  }
  return hash;
}

void PianoRollComponent::rebuildEngineBlockMap()
{
  engineBlockCache.valid = true;
  engineBlockCache.fingerprint = computeEngineBlockFingerprint();
  engineBlockCache.notePointers.clear();
  engineBlockCache.map.blocks.clear();
  engineBlockCache.map.blockOfNote.clear();
  if (project == nullptr)
    return;

  auto &notes = project->getNotes();
  engineBlockCache.notePointers.reserve(notes.size());
  for (auto &note : notes)
    engineBlockCache.notePointers.push_back(&note);

  const auto &audio = project->getAudioData();
  const auto &source = audio.originalWaveform.getNumSamples() > 0
                           ? audio.originalWaveform
                           : audio.waveform;
  engineBlockCache.map = engineBlocks::buildVisualBlocks(
      notes, globalSynthesisEngine, source,
      static_cast<int>(audio.sampleRate + 0.5), HOP_SIZE);
  ++engineBlockBuildCount;
}

const engineBlocks::VisualBlockMap &PianoRollComponent::ensureEngineBlockMap()
{
  if (project == nullptr)
  {
    engineBlockCache = EngineBlockCache{};
    return engineBlockCache.map;
  }
  const auto fingerprint = computeEngineBlockFingerprint();
  if (!engineBlockCache.valid ||
      fingerprint != engineBlockCache.fingerprint)
    rebuildEngineBlockMap();
  return engineBlockCache.map;
}

std::optional<SynthesisEngineType>
PianoRollComponent::getNoteBlockEngine(const Note &note)
{
  if (project == nullptr || note.isRest())
    return std::nullopt;
  const auto &map = ensureEngineBlockMap();
  const auto *first = project->getNotes().data();
  const auto *last = first + project->getNotes().size();
  if (&note < first || &note >= last)
    return std::nullopt;
  const int index = static_cast<int>(&note - first);
  const int blockIndex = map.blockForNote(index);
  if (blockIndex < 0)
    return std::nullopt;
  return map.blocks[static_cast<size_t>(blockIndex)].engine;
}

std::vector<int> PianoRollComponent::getActiveEngineBlockIndices()
{
  std::vector<int> blocks;
  if (project == nullptr)
    return blocks;
  const auto &map = ensureEngineBlockMap();
  auto *first = project->getNotes().data();
  for (Note *note : getSelectedNotes())
  {
    if (note == nullptr || note->isRest())
      continue;
    const int index = static_cast<int>(note - first);
    const int blockIndex = map.blockForNote(index);
    if (blockIndex >= 0 &&
        std::find(blocks.begin(), blocks.end(), blockIndex) == blocks.end())
      blocks.push_back(blockIndex);
  }
  return blocks;
}

bool PianoRollComponent::hasExplicitEngineSelection() const
{
  if (project == nullptr)
    return false;
  for (const auto &note : project->getNotes())
    if (note.isSelected() && !note.isRest() && !note.isDeleted())
      return true;
  return false;
}

std::vector<Note *> PianoRollComponent::getActiveEngineBlockNotes()
{
  std::vector<Note *> result;
  if (project == nullptr)
    return result;
  const auto &map = ensureEngineBlockMap();
  for (Note *note : getSelectedNotes())
  {
    if (note == nullptr || note->isRest())
      continue;
    const auto *first = project->getNotes().data();
    const int index = static_cast<int>(note - first);
    const int blockIndex = map.blockForNote(index);
    if (blockIndex < 0)
      continue;
    for (const int noteIndex :
         map.blocks[static_cast<size_t>(blockIndex)].noteIndices)
    {
      Note *candidate = &project->getNotes()[static_cast<size_t>(noteIndex)];
      if (std::find(result.begin(), result.end(), candidate) == result.end())
        result.push_back(candidate);
    }
  }
  return result;
}

void PianoRollComponent::setSweepState(bool active, int startFrame,
                                       int endFrame, int remaining)
{
  int sweepStart = startFrame;
  int sweepEnd = endFrame;
  std::optional<juce::Colour> sweepColour;
  bool neutral = false;
  if (active && engineViewActive && project != nullptr &&
      sweepEnd > sweepStart)
  {
    // Expand the dirty range to the first/last note of every visual block it
    // touches. One block sweeps in its engine colour; several (or none) use
    // the neutral colour, still as a border light, never the white line.
    const auto &map = ensureEngineBlockMap();
    std::vector<int> touched;
    for (const auto &block : map.blocks)
      if (block.startFrame < sweepEnd && block.endFrame > sweepStart)
        touched.push_back(block.index);
    if (!touched.empty())
    {
      int firstFrame = std::numeric_limits<int>::max();
      int lastFrame = std::numeric_limits<int>::min();
      for (const int blockIndex : touched)
      {
        const auto &block = map.blocks[static_cast<size_t>(blockIndex)];
        firstFrame = std::min(firstFrame, block.startFrame);
        lastFrame = std::max(lastFrame, block.endFrame);
      }
      sweepStart = firstFrame;
      sweepEnd = lastFrame;
      if (touched.size() == 1)
        sweepColour = engineColours::forEngine(
            map.blocks[static_cast<size_t>(touched.front())].engine);
      else
        neutral = true;
    }
  }

  lastSweepStartFrame = sweepStart;
  lastSweepEndFrame = sweepEnd;
  lastSweepNeutral = neutral;
  lastSweepActive = active;

  if (noteRenderer)
  {
    noteRenderer->setSweepState(active, sweepStart, sweepEnd, remaining);
    noteRenderer->setSweepBorderColour(sweepColour);
  }
  if (previewNoteRenderer)
    previewNoteRenderer->setSweepState(active, startFrame, endFrame,
                                       remaining);
}

void PianoRollComponent::setBypassView(bool active)
{
  if (bypassView == active)
    return;
  bypassView = active;
  repaint();
}

void PianoRollComponent::drawPitchCurves(juce::Graphics &g)
{
  PitchCurveRenderer::Params params;
  params.showDeltaPitch = showDeltaPitch;
  params.showBasePitch = showBasePitch;
  params.showUvInterpolationDebug = showUvInterpolationDebug;
  params.showActualF0Debug = showActualF0Debug;
  params.showCleanedF0Debug = showCleanedF0Debug;
  params.showVocoderF0Debug = showVocoderF0Debug;
  params.hidePitchCurves = false;
  params.componentWidth = getWidth();

  if (editMode == EditMode::PitchDrawing && pitchDrawingHandler_ &&
      pitchDrawingHandler_->hasAnchors())
  {
    auto oldParams = params;
    oldParams.showDeltaPitch = true;
    oldParams.showBasePitch = false;
    oldParams.showUvInterpolationDebug = false;
    oldParams.showActualF0Debug = false;
    oldParams.showCleanedF0Debug = false;
    oldParams.showVocoderF0Debug = false;
    oldParams.pitchCurveAlpha = 0.24f;
    oldParams.midiCurveOverride = &pitchDrawingHandler_->getOriginalMidiCurve();
    pitchCurveRenderer->draw(g, oldParams);

    params.showDeltaPitch = true;
    params.pitchCurveAlpha = 1.0f;
    params.midiCurveOverride = &pitchDrawingHandler_->getPreviewMidiCurve();
  }
  pitchCurveRenderer->draw(g, params);
}

void PianoRollComponent::drawPianoKeys(juce::Graphics &g)
{
  ScaleMode activeScaleMode = ScaleMode::Chromatic;
  if (snapToSemitoneDrag && dragSnapMode == DragSnapMode::Scale)
  {
    const ScaleMode preferredMode =
        project != nullptr ? project->getPreferredScaleMode() : ScaleMode::Major;
    activeScaleMode = previewScaleMode.value_or(preferredMode);
  }
  const int activeScaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  const int horizontalScrollBarSize =
      showHorizontalScrollBar ? APP_SCROLLBAR_THICKNESS : 0;
  pianoKeysRenderer->draw(g, getHeight(), horizontalScrollBarSize,
                          activeScaleMode, activeScaleRootNote,
                          ScaleUtils::getReferenceOffsetSemitones(
                              pitchReferenceHz));
}

float PianoRollComponent::midiToY(float midiNote) const
{
  return (MAX_MIDI_NOTE - midiNote) * pixelsPerSemitone;
}

float PianoRollComponent::yToMidi(float y) const
{
  return MAX_MIDI_NOTE - y / pixelsPerSemitone;
}

float PianoRollComponent::timeToX(double time) const
{
  return static_cast<float>(time * pixelsPerSecond);
}

double PianoRollComponent::xToTime(float x) const
{
  return x / pixelsPerSecond;
}

double PianoRollComponent::getTimelineQuarterNoteSeconds() const
{
  const double bpm = juce::jlimit(20.0, 300.0, timelineTempoBpm);
  return bpm > 0.0 ? 60.0 / bpm : (60.0 / 120.0);
}

double PianoRollComponent::getTimelineBeatSeconds() const
{
  const int denominator = normalizeTimelineBeatDenominator(timelineBeatDenominator);
  return getTimelineQuarterNoteSeconds() * (4.0 / static_cast<double>(denominator));
}

double PianoRollComponent::getTimelineBarSeconds() const
{
  const int numerator = juce::jmax(1, timelineBeatNumerator);
  return getTimelineBeatSeconds() * static_cast<double>(numerator);
}

double PianoRollComponent::getTimelineGridSeconds() const
{
  const double quarterNotes = gridDivisionToQuarterNotes(timelineGridDivision);
  return getTimelineQuarterNoteSeconds() * quarterNotes;
}

double PianoRollComponent::getTimelineGridQuarterNotes() const
{
  return gridDivisionToQuarterNotes(timelineGridDivision);
}

bool PianoRollComponent::shouldSnapCycleToGrid() const
{
  if (timelineDisplayMode != TimelineDisplayMode::Beats || !timelineSnapCycle)
    return false;

  // R4 (D-317): with a real host map the grid is musical (quarters), so the
  // seconds interval is no longer the reference.
  if (hostTempoMap != nullptr && hostTempoMap->isValid())
    return getTimelineGridQuarterNotes() > 0.0;

  return getTimelineGridSeconds() > 1.0e-6;
}

double PianoRollComponent::snapTimeToTimelineGrid(double timeSeconds) const
{
  if (!shouldSnapCycleToGrid())
    return std::max(getTimelineStart(), timeSeconds);

  // R4 (D-317): project -> host -> quarter -> round -> host -> project, so the
  // snap lands on the lines the ruler draws with a real tempo map.
  if (hostTempoMap != nullptr && hostTempoMap->isValid())
    return std::max(
        getTimelineStart(),
        snapProjectSecondsToHostGrid(*hostTempoMap,
                                     coordMapper->getTimelineDisplayOffset(),
                                     timeSeconds,
                                     getTimelineGridQuarterNotes()));

  const double interval = getTimelineGridSeconds();
  const double snapped =
      coordMapper->snapProjectTimeToTimelineGrid(timeSeconds, interval);
  return std::max(getTimelineStart(), snapped);
}

bool PianoRollComponent::isCanvasPoint(const juce::MouseEvent &e) const
{
  return e.x >= pianoKeysWidth && e.y >= headerHeight &&
         e.x < pianoKeysWidth + getVisibleContentWidth() &&
         e.y < headerHeight + getVisibleContentHeight();
}

bool PianoRollComponent::isModifierZoomDrag(const juce::MouseEvent &e) const
{
#if JUCE_MAC
  return e.mods.isCommandDown();
#else
  return e.mods.isCtrlDown();
#endif
}

void PianoRollComponent::applyModifierZoomDrag(const juce::MouseEvent &e)
{
  const float deltaX = e.position.x - modifierZoomLastPosition.x;
  const float deltaY = modifierZoomLastPosition.y - e.position.y;
  modifierZoomLastPosition = e.position;

  if (std::abs(deltaX) < 0.01f && std::abs(deltaY) < 0.01f)
    return;

  const float mouseX = static_cast<float>(e.x - pianoKeysWidth);
  const float mouseY = static_cast<float>(e.y - headerHeight);

  if (std::abs(deltaX) >= 0.01f && !modifierZoomVerticalOnly)
  {
    const float zoomFactorX = std::pow(1.0065f, deltaX);
    const double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));
    const int visibleWidth = getVisibleContentWidth();
    const double totalTime = getTimelineLength();
    const float minPpsX =
        (visibleWidth > 0 && totalTime > 0.0)
            ? std::max(MIN_PIXELS_PER_SECOND,
                       static_cast<float>(visibleWidth / totalTime))
            : MIN_PIXELS_PER_SECOND;
    const float newPpsX = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND,
                                       pixelsPerSecond * zoomFactorX);

    pixelsPerSecond = newPpsX;
    coordMapper->setPixelsPerSecond(newPpsX);

    const float newMouseX = static_cast<float>(timeAtMouse * newPpsX);
    scrollX = clampScrollX(static_cast<double>(newMouseX - mouseX));
    coordMapper->setScrollX(scrollX);
  }

  if (std::abs(deltaY) >= 0.01f)
  {
    const float zoomFactorY = std::pow(1.0065f, deltaY);
    setPixelsPerSemitone(pixelsPerSemitone * zoomFactorY, mouseY);
  }

  updateScrollBars();
  repaint();

  if (std::abs(deltaX) >= 0.01f && !modifierZoomVerticalOnly && onZoomChanged)
    onZoomChanged(pixelsPerSecond);
}

bool PianoRollComponent::isModifierPanDrag(const juce::MouseEvent &e) const
{
  return e.mods.isAltDown();
}

void PianoRollComponent::applyModifierPanDrag(const juce::MouseEvent &e)
{
  const float deltaX = e.position.x - modifierPanLastPosition.x;
  const float deltaY = e.position.y - modifierPanLastPosition.y;
  modifierPanLastPosition = e.position;

  if (std::abs(deltaX) < 0.01f && std::abs(deltaY) < 0.01f)
    return;

  const double previousScrollX = scrollX;
  setScrollX(scrollX - static_cast<double>(deltaX));
  setScrollY(scrollY - static_cast<double>(deltaY));

  if (std::abs(scrollX - previousScrollX) >= 0.01 && onScrollChanged)
    onScrollChanged(scrollX);
}

std::vector<juce::Rectangle<float>> PianoRollComponent::trackNoteBounds(const Note& note, const Project& owner) const
{
  std::vector<juce::Rectangle<float>> result;
  if (!regionPreviews) return result;
  for (const auto& region : *regionPreviews) {
    if (region.pinned || region.sourceProject != &owner) continue;
    const double offset = region.projectHostOffsetSeconds - framesToSeconds(region.sourceFirstFrame);
    const double start = framesToSeconds(note.getVisualStartFrame()) + offset;
    const double end = framesToSeconds(note.getVisualEndFrame()) + offset;
    if (end <= region.startSeconds || start >= region.endSeconds) continue;
    const float left = pianoKeysWidth + timeToX(timelineToProject(std::max(start, region.startSeconds))) - scrollX;
    const float right = pianoKeysWidth + timeToX(timelineToProject(std::min(end, region.endSeconds))) - scrollX;
    const float y = headerHeight + coordMapper->midiToY(note.getAdjustedMidiNote()) - scrollY;
    result.emplace_back(left, y, std::max(4.0f, right - left), pixelsPerSemitone);
  }
  return result;
}

void PianoRollComponent::setDocumentNoteContext(
    DocumentNoteIndexPtr index,
    std::shared_ptr<DocumentNoteSelection> selection)
{
  documentNoteIndex = std::move(index);
  documentNoteSelection = std::move(selection);
}

bool PianoRollComponent::selectAllDocumentNotes()
{
  if (!trackViewMode || documentNoteIndex == nullptr ||
      documentNoteSelection == nullptr)
    return false;
  documentNoteSelection->selectAll(*documentNoteIndex);
  applyDocumentSelection();
  return true;
}

void PianoRollComponent::applyDocumentSelection()
{
  // Only a live document may derive the flags: without an index the set holds
  // pointers of a previous publication and must not touch this item's notes.
  if (documentSelectionAvailable())
    documentNoteSelection->applyToNotes();
  syncSelectionToRegionCopies();
  updatePitchToolHandlesFromSelection();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::syncSelectionToRegionCopies()
{
  if (!regionPreviews) return;
  // N9 (D-349): the copies' flags are a projection of the document selection,
  // written from the selection module (the only writer of Note::selected).
  for (const auto& region : *regionPreviews) {
    if (region.active || region.pinned || !region.project ||
        !region.sourceProject)
      continue;
    DocumentNoteSelection::copyFlagsToWindow(*region.project,
                                             *region.sourceProject,
                                             region.sourceFirstFrame);
  }
  inactiveLayer = {};
  repaint();
}

void PianoRollComponent::selectOnlyNote(Project *owner, Note *note)
{
  // N9 (D-349): every "this note becomes the selection" gesture lands here. In
  // track mode the document set is the target (and the rest of the document is
  // released); in clip mode the item's own flags are.
  if (owner == nullptr || note == nullptr)
    return;
  if (documentSelectionAvailable())
  {
    documentNoteSelection->setFromPairs({{owner, note}}, *documentNoteIndex);
    applyDocumentSelection();
    return;
  }
  owner->deselectAllNotes();
  DocumentNoteSelection::setNoteSelected(*owner, *note, true);
}

void PianoRollComponent::toggleNoteSelection(Project *owner, Note *note)
{
  if (owner == nullptr || note == nullptr)
    return;
  if (documentSelectionAvailable())
  {
    documentNoteSelection->toggle(owner, note, *documentNoteIndex);
    applyDocumentSelection();
    return;
  }
  DocumentNoteSelection::setNoteSelected(*owner, *note, !note->isSelected());
}

void PianoRollComponent::clearNoteSelection()
{
  // The whole document when it is published, the active item otherwise.
  if (documentSelectionAvailable())
  {
    documentNoteSelection->clear();
    applyDocumentSelection();
    return;
  }
  if (project != nullptr)
    project->deselectAllNotes();
}

void PianoRollComponent::applyItemBoxSelection(const std::vector<Note *> &notes)
{
  if (project == nullptr)
    return;
  if (documentSelectionAvailable())
  {
    std::vector<std::pair<Project *, Note *>> pairs;
    pairs.reserve(notes.size());
    for (auto *note : notes)
      if (note != nullptr)
        pairs.push_back({project, note});
    documentNoteSelection->setFromPairs(pairs, *documentNoteIndex);
    applyDocumentSelection();
    return;
  }
  project->deselectAllNotes();
  for (auto *note : notes)
    if (note != nullptr)
      DocumentNoteSelection::setNoteSelected(*project, *note, true);
}

void PianoRollComponent::syncTrackPreviewSelection()
{
  // N2: the real notes carry the flags; the copies are only a drawing cache.
  syncSelectionToRegionCopies();
}

bool PianoRollComponent::beginTrackGesture(const juce::MouseEvent& e)
{
  if (!trackViewMode || editMode != EditMode::Select || !getTrackEditTargets ||
      !e.mods.isLeftButtonDown() || !isCanvasPoint(e)) return false;
  trackGestureTargets = mouseDownTrackTargets ? std::move(*mouseDownTrackTargets)
                                              : getTrackEditTargets();
  mouseDownTrackTargets.reset();
  if (trackGestureTargets.empty()) return false;
  const float worldX = e.x - pianoKeysWidth + scrollX;
  const float worldY = e.y - headerHeight + scrollY;
  if ((hoveredNote && pitchToolHandles && pitchToolHandles->containsLayoutPoint(worldX, worldY)) ||
      findPreviewButtonNoteAt(worldX, worldY)) return false;
  Note* hit = nullptr;
  Project* hitOwner = nullptr;
  std::vector<Project*> selectedOwners;
  trackGestureNotes.clear();
  for (auto& target : trackGestureTargets) {
    for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames)) {
      for (const auto& bounds : trackNoteBounds(*note, *target.project))
        if (bounds.contains(e.position)) { hit = note; hitOwner = target.project; }
      if (note->isSelected()) {
        trackGestureNotes.push_back({note, target.project, note->getMidiNote(), note->getMidiNote(),
                                     note->getLastNonMacroMidiNote(), note->getLastNonMacroMidiNote()});
        if (std::find(selectedOwners.begin(), selectedOwners.end(), target.project) == selectedOwners.end())
          selectedOwners.push_back(target.project);
      }
    }
  }
  const bool additive = e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isShiftDown();
  if (hit && additive) {
    // N2: shift/cmd-click toggles the document selection, so it reaches notes
    // of any item of the track and the same take cannot count twice.
    if (documentSelectionAvailable())
      documentNoteSelection->toggle(hitOwner, hit, *documentNoteIndex);
    else
      DocumentNoteSelection::setNoteSelected(*hitOwner, *hit, !hit->isSelected());
    applyDocumentSelection();
    trackGestureNotes.clear();
  } else if (hit && hit->isSelected() && selectedOwners.size() > 1) {
    std::vector<Note*> activeNotes;
    for (const auto& item : trackGestureNotes)
      if (item.owner == project) activeNotes.push_back(item.note);
    beginMacroCurvePreview(activeNotes);
    beginInactiveMacroPreview(trackGestureTargets);
  } else if (!hit) {
    trackBoxSelecting = true;
    trackBoxInitialSelection.clear();
    if (additive) {
      if (documentSelectionAvailable())
        for (const auto& entry : documentNoteSelection->entries())
          trackBoxInitialSelection.push_back({entry.project, entry.note});
      else
        for (const auto& item : trackGestureNotes)
          trackBoxInitialSelection.push_back({item.owner, item.note});
    }
    if (!additive) {
      if (documentSelectionAvailable())
        documentNoteSelection->clear();
      else
        for (auto& target : trackGestureTargets)
          target.project->deselectAllNotes();
    }
    trackGestureNotes.clear();
    applyDocumentSelection();
  } else {
    if (!hit->isSelected()) {
      if (documentSelectionAvailable())
        documentNoteSelection->clear();
      else
        for (auto& target : trackGestureTargets)
          target.project->deselectAllNotes();
    }
    applyDocumentSelection();
    return false;
  }
  trackGestureActive = true;
  trackGestureStart = trackGestureEnd = e.position;
  return true;
}

void PianoRollComponent::updateTrackGesture(const juce::MouseEvent& e)
{
  trackGestureEnd = e.position;
  if (trackBoxSelecting) {
    // N2: the marquee writes the document selection, so one rectangle can
    // select notes of several items (and of both takes) and a note shown by two
    // sibling windows still counts once.
    const auto rect = juce::Rectangle<float>(trackGestureStart, trackGestureEnd);
    const bool useDocument = documentSelectionAvailable();
    std::vector<std::pair<Project*, Note*>> wanted;
    wanted.reserve(trackBoxInitialSelection.size());
    for (const auto& initial : trackBoxInitialSelection)
      if (initial.project != nullptr && initial.note != nullptr)
        wanted.push_back({initial.project, initial.note});
    for (auto& target : trackGestureTargets) {
      if (target.isValid && !target.isValid()) continue;
      for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames)) {
        bool selected = std::any_of(
            trackBoxInitialSelection.begin(), trackBoxInitialSelection.end(),
            [&](const TrackSelectionPair& initial) {
              return initial.project == target.project && initial.note == note;
            });
        for (const auto& bounds : trackNoteBounds(*note, *target.project))
          selected |= rect.intersects(bounds);
        if (selected)
          wanted.push_back({target.project, note});
      }
    }
    if (useDocument)
      documentNoteSelection->setFromPairs(wanted, *documentNoteIndex);
    else
    {
      for (auto& target : trackGestureTargets)
        target.project->deselectAllNotes();
      for (const auto& pair : wanted)
        DocumentNoteSelection::setNoteSelected(*pair.first, *pair.second, true);
    }
    applyDocumentSelection();
  } else {
    float delta = (trackGestureStart.y - e.position.y) / pixelsPerSemitone;
    // One offset for every note, resolved on the first note's on-screen pitch
    // (midi + offset) like a multi-note drag, so all notes keep their intervals.
    if (!trackGestureNotes.empty()) {
      const auto& first = trackGestureNotes.front();
      const float anchor = first.midi + first.note->getPitchOffset();
      delta = pitchEditor->resolveDragOffset(delta, anchor, resolveDragSnapKind(e.mods));
    }
    for (auto& item : trackGestureNotes) {
      auto target = std::find_if(trackGestureTargets.begin(), trackGestureTargets.end(),
          [&](const auto& target) { return target.project == item.owner; });
      if (target != trackGestureTargets.end() && (!target->isValid || target->isValid())) {
        // Keep the Correct Pitch reference (see NotePitch).
        NotePitch::applyEdit(*item.note, *item.owner, item.source, item.midi,
                             item.midi + delta);
      }
    }
    repaint();
  }
}

void PianoRollComponent::finishTrackGesture()
{
  trackGestureActive = false;
  trackBoxSelecting = false;
  endMacroCurvePreview();
  auto notes = trackGestureNotes;
  auto targets = trackGestureTargets;
  notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const auto& item) {
    auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& target)
        { return target.project == item.owner; });
    return target == targets.end() || (target->isValid && !target->isValid());
  }), notes.end());
  for (auto& item : notes) {
    item.finalMidi = item.note->getMidiNote();
    item.finalSource = item.note->getLastNonMacroMidiNote();
  }
  if (std::none_of(notes.begin(), notes.end(), [](const auto& item) {
      return std::abs(item.finalMidi - item.midi) > 0.0001f; })) { repaint(); return; }
  auto safe = juce::Component::SafePointer<PianoRollComponent>(this);
  auto apply = [safe, notes, targets](bool redo) {
    if (!safe) return;
    for (const auto& target : targets) {
      if (target.isValid && !target.isValid()) continue;
      int first = std::numeric_limits<int>::max(), last = -1;
      for (const auto& item : notes) {
        if (item.owner != target.project) continue;
        NotePitch::restore(*item.note, redo ? item.finalMidi : item.midi,
                           redo ? item.finalSource : item.source);
        item.note->markDirty(); item.note->markSynthDirty();
        first = std::min(first, item.note->getStartFrame());
        last = std::max(last, item.note->getEndFrame());
      }
      if (last < first) continue;
      PitchCurveProcessor::rebuildBaseFromNotes(*target.project);
      target.project->markNoteEditDirtyRange(first, last);
      target.project->setModified(true);
      if (target.project == safe->project) {
        if (safe->onPitchEdited) safe->onPitchEdited();
        if (safe->onPitchEditFinished) safe->onPitchEditFinished();
      } else if (target.onChanged) target.onChanged(true);
    }
    safe->invalidateBasePitchCache(); safe->repaint();
  };
  class TrackDragAction final : public UndoableAction {
  public:
    explicit TrackDragAction(std::function<void(bool)> apply) : apply(std::move(apply)) {}
    void undo() override { apply(false); }
    void redo() override { apply(true); }
    juce::String getName() const override { return "Drag Track Notes"; }
    bool ownsProjectContext() const override { return true; }
    bool requiresAudioResynthesis() const override { return false; }
  private: std::function<void(bool)> apply;
  };
  apply(true);
  if (undoManager) undoManager->addAction(std::make_unique<TrackDragAction>(apply));
}

void PianoRollComponent::mouseDown(const juce::MouseEvent &e)
{
  swallowGestureUntilMouseUp = false;
  mouseDownTrackTargets.reset(); // fetched at most once for this click
  // D-285/2: un gesto nuevo empieza con el primer clic. La marca de «acabo de
  // cortar» solo vive hasta el mouseDoubleClick del mismo doble clic.
  if (e.getNumberOfClicks() < 2)
    splitJustPerformed = false;

  if (!project)
  {
    // The active region may still be analysing; the other regions stay
    // clickable so the user can move on to one that is ready.
    if (e.mods.isLeftButtonDown() && isCanvasPoint(e) && onInactiveRegionClicked)
    {
      if (const auto *region = findTrackRegionAtPointer(e.position))
      {
        const juce::String key = region->key;
        swallowGestureUntilMouseUp = true;
        onInactiveRegionClicked(key);
        repaint();
      }
    }
    return;
  }

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);
  if (e.getNumberOfClicks() <= 1)
    clickStartedOnSegment =
        isCanvasPoint(e) && findNoteAt(adjustedX, adjustedY) != nullptr;

  if (e.mods.isPopupMenu() && isCanvasPoint(e))
  {
    // One menu for the whole piano roll: note actions when the pointer is on a
    // segment, the edit block and the tool strip in the same popup. The old
    // floating pill is hidden first so the two never coexist.
    if (onHideToolPill)
      onHideToolPill();
    showCanvasMenuAt(e.getScreenPosition(), findNoteAt(adjustedX, adjustedY));
    return;
  }
  if (onHideToolPill)
    onHideToolPill();

  // Boton central: sobre el ruler, scrub de transporte (como REAPER); sobre el
  // lienzo, herramienta de mano para moverse por el piano roll (Melodyne, pedido
  // 2026-11-23).
  if (e.mods.isMiddleButtonDown() && e.x >= pianoKeysWidth)
  {
    if (e.y < headerHeight)
    {
      middleButtonScrubActive = true;
      double time = std::max(getTimelineStart(), xToTime(adjustedX));
      setCursorTime(time);
      if (onSeek)
        onSeek(time);
    }
    else
    {
      middlePanActive = true;
      middlePanLastPosition = e.position;
      setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }
    return;
  }

  // Cmd-click on a segment toggles its selection (VariAudio); zoom-drag
  // stays available from empty canvas.
  const bool cmdClickOnSegment =
      editMode == EditMode::Select && findNoteAt(adjustedX, adjustedY) != nullptr;
  if (isCanvasPoint(e) && isModifierZoomDrag(e) && !cmdClickOnSegment)
  {
    modifierZoomDragActive = true;
    modifierZoomVerticalOnly = false;
    modifierZoomLastPosition = e.position;
    return;
  }

  if (isCanvasPoint(e) && e.mods.isAltDown() && trackViewMode)
  {
    if (const auto* region = findTrackRegionAtPointer(e.position, true)) {
      const double offset = region->projectHostOffsetSeconds - framesToSeconds(region->sourceFirstFrame);
      const auto* boundWindow = findActiveRegionPreview();
      if (onInactiveRegionClicked && (region->sourceProject != project ||
          !juce::approximatelyEqual(offset, projectToTimeline(0.0)) ||
          !boundWindow || boundWindow->key != region->key)) {
        const auto key = region->key;
        onInactiveRegionClicked(key);
        adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
        adjustedY = e.y - headerHeight + static_cast<float>(scrollY);
      }
    }
  }
  // On a segment: Alt ignora el iman (sin snap) y Alt-click sin arrastre corta el
  // segmento; en el resto del lienzo Alt-drag hace zoom vertical.
  const bool altNoteDrag =
      editMode == EditMode::Select && findNoteAt(adjustedX, adjustedY) != nullptr;
  if (isCanvasPoint(e) && isModifierPanDrag(e) && !altNoteDrag)
  {
    // Plain drag on empty canvas pans, so Option/Alt-drag zooms vertically
    // (Cmd-drag keeps zooming both axes).
    modifierZoomDragActive = true;
    modifierZoomVerticalOnly = true;
    modifierZoomLastPosition = e.position;
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    return;
  }

  // D-285: un arrastre con clic izquierdo en vacio dibuja el recuadro de
  // seleccion (Melodyne), no panea. El paneo queda en el boton central (arriba)
  // y en Alt-arrastre. El clic simple en vacio sigue deseleccionando: el
  // SelectHandler arranca un recuadro de tamano cero y lo cierra sin notas.
  // (Antes este bloque ponia modifierPanDragActive y se comia el recuadro.)

  // Handle timeline clicks - seek to position
  if (e.y < timelineHeight && e.x >= pianoKeysWidth)
  {
    double time = std::max(getTimelineStart(), xToTime(adjustedX));
    setCursorTime(time);
    if (onSeek)
      onSeek(time);
    return;
  }

  // Clicking a note name auditions its labelled pitch without changing the
  // current edit or transport state.
  if (e.x < pianoKeysWidth && e.y >= headerHeight &&
      e.y < getHeight() - (showHorizontalScrollBar ? 8 : 0))
  {
    const float referenceOffset =
        ScaleUtils::getReferenceOffsetSemitones(pitchReferenceHz);
    // yToMidi() returns the note at the row's top edge and descends across
    // the row. Ceiling keeps every pixel in this rendered row mapped to its
    // label; rounding would switch to the adjacent note halfway down.
    const int midiNote = static_cast<int>(
        std::ceil(yToMidi(adjustedY) - referenceOffset));
    if (midiNote >= MIN_MIDI_NOTE && midiNote <= MAX_MIDI_NOTE &&
        onPianoKeyAudition)
    {
      pianoKeyAuditionMouseDown = true;
      onPianoKeyAudition(midiNote);
    }
    return;
  }

  // Handle loop timeline drag (always active, priority over edit modes)
  if (loopDragHandler_->mouseDown(e, adjustedX, adjustedY))
    return;

  // Ignore clicks outside main area
  if (e.y < headerHeight || e.x < pianoKeysWidth)
    return;

  if (beginTrackGesture(e)) return;

  // Bind the project owning this pointer hit internally, then deliver the
  // same event to its tool. Routing does not privilege the bound region.
  if (e.mods.isLeftButtonDown() && onInactiveRegionClicked && isCanvasPoint(e))
  {
    const auto* region = findTrackRegionAtPointer(e.position);
    const bool handleHit = hoveredNote && pitchToolHandles &&
        pitchToolHandles->containsLayoutPoint(adjustedX, adjustedY);
    const double regionOffset = region ? region->projectHostOffsetSeconds -
        framesToSeconds(region->sourceFirstFrame) : projectToTimeline(0.0);
    const auto* boundWindow = findActiveRegionPreview();
    if (region && !handleHit && (region->sourceProject != project ||
        !juce::approximatelyEqual(regionOffset, projectToTimeline(0.0)) ||
        !boundWindow || boundWindow->key != region->key))
    {
      const juce::String key = region->key; // previews may be replaced below
      const auto* requestedProject = region->sourceProject;
      const double previousHostScroll = getHostScrollSeconds();
      const double previousScrollY = scrollY;

      onInactiveRegionClicked(key);
      repaint();

      // The new region may belong to another modification (new project) or be
      // a sibling of this one (same project, new window and offset). Either
      // way the host view must not have moved for the click to carry on.
      const bool viewStill =
          std::abs(getHostScrollSeconds() - previousHostScroll) * pixelsPerSecond < 0.5 &&
          juce::approximatelyEqual(scrollY, previousScrollY);
      const bool continuesIntoRegion = project != nullptr && project == requestedProject &&
          viewStill && juce::approximatelyEqual(projectToTimeline(0.0), regionOffset);
      if (!continuesIntoRegion)
      {
        swallowGestureUntilMouseUp = true;
        return;
      }

      // Same screen point, now in the new region's time.
      adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
      adjustedY = e.y - headerHeight + static_cast<float>(scrollY);
      setHoveredNote(nullptr);
    }
    else if (trackViewMode && project != nullptr && !region && !handleHit)
    {
      // Outside every region in Track mode: the active take's audio there is
      // hidden, so nothing may be edited.
      swallowGestureUntilMouseUp = true;
      return;
    }
  }

  if (editMode == EditMode::Select)
  {
    if (auto *note = findPreviewButtonNoteAt(adjustedX, adjustedY))
    {
      triggerPreviewForNote(*note);
      return;
    }
  }

  // Delegate to current mode handler
  if (currentHandler_)
  {
    currentHandler_->mouseDown(e, adjustedX, adjustedY);
    updatePreviewButtonBounds();
  }
}

void PianoRollComponent::showCanvasMenuAt(juce::Point<int> screenPoint, Note* note)
{
  canvasPopupMenu::Content::State state;
  state.hasNote = note != nullptr;
  state.noteMuted = note != nullptr && note->isMuted();
  state.showAdvancedTools = editorUiMode == EditorUiMode::Advanced;
  state.canUndo = undoManager && undoManager->canUndo() && bool(onUndoRequested);
  state.canRedo = undoManager && undoManager->canRedo() && bool(onRedoRequested);
  state.activeToolIndex = static_cast<int>(currentEditorTool);

  if (note != nullptr)
  {
    const auto targets = getResetTargetNotes(*note);
    const auto anyTarget = [&targets](bool (*predicate)(const Note&))
    {
      return std::any_of(targets.begin(), targets.end(),
                         [predicate](const Note* n) { return n && predicate(*n); });
    };
    // Con varias notas seleccionadas el menu ofrece siempre restablecer: tras
    // cortar y seleccionar las dos mitades los predicados de "editada" no lo ven
    // (pedido 2026-11-23).
    state.canRestore = targets.size() > 1 || anyTarget(hasPitchEdits) ||
                       anyTarget(hasFormantEdits) || anyTarget(hasAmplitudeEdits) ||
                       anyTarget(hasTimingEdits);
  }

  juce::PopupMenu menu;
  // The custom item paints the two cards; the menu window itself is invisible.
  menu.setLookAndFeel(&canvasPopupMenu::getLookAndFeel());
  auto choice = std::make_shared<int>(0);
  menu.addCustomItem(1, std::make_unique<canvasPopupMenu::Content>(choice, state));

  juce::Component::SafePointer<PianoRollComponent> safeThis(this);
  Note* notePtr = note;
  menu.showMenuAsync(
      juce::PopupMenu::Options()
          .withTargetScreenArea(juce::Rectangle<int>(screenPoint.x, screenPoint.y, 1, 1))
          .withDeletionCheck(*this),
      [safeThis, notePtr, choice, screenPoint](int result)
      {
        if (safeThis == nullptr || !safeThis->project || result == 0)
          return;
        const int action = *choice;
        // The note may have been deleted while the async menu was open; drop
        // the pointer before any action touches it.
        Note* clickedNote = notePtr;
        if (clickedNote != nullptr)
        {
          auto& notes = safeThis->project->getNotes();
          if (std::none_of(notes.begin(), notes.end(),
                           [clickedNote](const Note& candidate)
                           { return &candidate == clickedNote; }))
            clickedNote = nullptr;
        }
        // Melodyne applies a context-menu action to the clicked note. If it
        // was not part of the selection, make it the selection first (the
        // mute path below already does the same for a single note).
        const auto ensureClickedNoteSelected = [&]()
        {
          if (clickedNote == nullptr)
            return;
          auto selected = safeThis->getSelectedNotes();
          if (std::find(selected.begin(), selected.end(), clickedNote) !=
              selected.end())
            return;
          safeThis->selectOnlyNote(safeThis->project, clickedNote);
          safeThis->updatePitchToolHandlesFromSelection();
          safeThis->repaint();
        };
        // Macros act on the selection (or on every note when nothing is
        // selected), exactly like the inspector buttons.
        if (action == 1)
        {
          ensureClickedNoteSelected();
          if (safeThis->onCorrectPitchMacroRequested)
            safeThis->onCorrectPitchMacroRequested();
          return;
        }
        if (action == 2)
        {
          ensureClickedNoteSelected();
          if (safeThis->onQuantizeTimeMenuRequested)
            safeThis->onQuantizeTimeMenuRequested(screenPoint);
          return;
        }
        if (action == 3)
          return; // Leveling is not implemented; the row is disabled.
        // Tools, select all, undo and redo do not need the note.
        if ((action >= 20 && action <= 25) || action == 7 || action == 8 ||
            action == 9)
        {
          safeThis->applyCanvasMenuChoice(action);
          return;
        }
        if (clickedNote == nullptr)
          return;
        if (action == 4 || action == 5)
        {
          // D-245: si la nota forma parte de la seleccion, se silencia la
          // seleccion; si no, solo esta nota.
          auto selected = safeThis->getSelectedNotes();
          const bool inSelection =
              std::find(selected.begin(), selected.end(), clickedNote) != selected.end();
          if (!inSelection)
            selected.assign(1, clickedNote);
          safeThis->setMutedForNotes(selected, action == 4);
        }
        else if (action == 6)
        {
          // D-299: igual que silenciar, la accion recae sobre la seleccion; si
          // la nota pulsada no estaba en ella, pasa a ser la seleccion.
          ensureClickedNoteSelected();
          safeThis->deleteSelection();
        }
        else if (action == 10)
          safeThis->showResetMenu(*clickedNote, screenPoint);
      });
}
void PianoRollComponent::applyCanvasMenuChoice(int choice)
{
  if (choice >= 20 && choice <= 25)
  {
    const auto tool = static_cast<EditorTool>(choice - 20);
    if (onEditorToolRequested)
    {
      onEditorToolRequested(tool);
      return;
    }
    const EditMode mode = tool == EditorTool::Pitch        ? EditMode::PitchDrawing
                          : tool == EditorTool::Separation ? EditMode::Split
                          : tool == EditorTool::Timing     ? EditMode::Timing
                                                           : EditMode::Select;
    if (onEditModeRequested)
      onEditModeRequested(mode);
    else
      setEditMode(mode);
    return;
  }

  if (!project)
    return;
  if (choice == 7)
  {
    // N2: in track mode "select all" is the whole document (every visible note
    // of every item, deduplicated); in clip mode it stays the active project.
    if (!selectAllDocumentNotes())
      project->selectAllNotes();
    updatePitchToolHandlesFromSelection();
    updatePreviewButtonBounds();
    repaint();
  }
  else if (choice == 8 && onUndoRequested)
    onUndoRequested();
  else if (choice == 9 && onRedoRequested)
    onRedoRequested();
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent &e)
{
  if (toolPillGesture)
  {
    if (onToolPillDrag)
      onToolPillDrag(e.getScreenPosition());
    return;
  }
  if (trackGestureActive) { updateTrackGesture(e); return; }
  if (e.mods.isPopupMenu())
    return;
  if (swallowGestureUntilMouseUp)
    return;
  if (middlePanActive)
  {
    const auto delta = e.position - middlePanLastPosition;
    middlePanLastPosition = e.position;
    horizontalScrollBar.setCurrentRangeStart(clampScrollX(scrollX - delta.x));
    verticalScrollBar.setCurrentRangeStart(scrollY - delta.y);
    return;
  }
  if (middleButtonScrubActive)
  {
    float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
    double time = std::max(getTimelineStart(), xToTime(adjustedX));
    setCursorTime(time);
    if (onSeek)
      onSeek(time);
    return;
  }

  if (modifierZoomDragActive)
  {
    applyModifierZoomDrag(e);
    return;
  }

  if (modifierPanDragActive)
  {
    applyModifierPanDrag(e);
    return;
  }

  // Throttle repaints during drag to ~60fps max
  juce::int64 now = juce::Time::getMillisecondCounter();
  bool shouldRepaint = (now - lastDragRepaintTime) >= minDragRepaintInterval;
  juce::ignoreUnused(shouldRepaint);

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Loop drag has priority (always active)
  if (loopDragHandler_->mouseDrag(e, adjustedX, adjustedY))
  {
    if (shouldRepaint)
    {
      repaint();
      lastDragRepaintTime = now;
    }
    return;
  }

  // Delegate to current mode handler
  if (currentHandler_ && currentHandler_->mouseDrag(e, adjustedX, adjustedY))
  {
    updatePreviewButtonBounds();
    if (shouldRepaint)
    {
      repaint();
      lastDragRepaintTime = now;
    }
  }
}

void PianoRollComponent::mouseUp(const juce::MouseEvent &e)
{
  // Whatever path this event takes (note drag, box selection, popup, early
  // return), the marquee must not outlive the button release: a stuck
  // selection rectangle would stay painted over the canvas.
  struct MarqueeEnder
  {
    BoxSelector *selector;
    ~MarqueeEnder()
    {
      if (selector != nullptr)
        selector->endSelection();
    }
  } marqueeEnder{boxSelector.get()};

  if (toolPillGesture)
  {
    toolPillGesture = false;
    if (onToolPillRelease)
      onToolPillRelease(e.getScreenPosition());
    return;
  }
  if (trackGestureActive) { finishTrackGesture(); return; }
  if (e.mods.isPopupMenu())
    return;
  if (swallowGestureUntilMouseUp)
  {
    swallowGestureUntilMouseUp = false;
    return;
  }
  if (middleButtonScrubActive)
  {
    middleButtonScrubActive = false;
    return;
  }

  if (middlePanActive)
  {
    middlePanActive = false;
    updateMouseCursorForEditMode();
    return;
  }

  if (pianoKeyAuditionMouseDown)
  {
    pianoKeyAuditionMouseDown = false;
    if (onPianoKeyAuditionFinished)
      onPianoKeyAuditionFinished();
    return;
  }

  if (modifierZoomDragActive)
  {
    modifierZoomDragActive = false;
    return;
  }

  if (modifierPanDragActive)
  {
    modifierPanDragActive = false;
    if (!e.mods.isAltDown())
      updateMouseCursorForEditMode();
    return;
  }

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Loop drag has priority (always active)
  if (loopDragHandler_->mouseUp(e, adjustedX, adjustedY))
    return;

  // Delegate to current mode handler
  if (currentHandler_)
  {
    currentHandler_->mouseUp(e, adjustedX, adjustedY);
    updatePreviewButtonBounds();
  }
}

void PianoRollComponent::mouseMove(const juce::MouseEvent &e)
{
  // N8 (D-348): hovering a note must not open or change the inspector's
  // notebook. Region activation stays in the click paths (mouseDown and the
  // Alt-click), which call prepareTrackPointer() themselves; doing it here
  // swapped the active project and the inspector just by passing the pointer.

  const bool showZoomCursor = isCanvasPoint(e) && isModifierZoomDrag(e);
  const float adjustedX =
      e.x - pianoKeysWidth + static_cast<float>(scrollX);
  const float adjustedY =
      e.y - headerHeight + static_cast<float>(scrollY);

  Note *noteAtPointer = nullptr;
  if (e.y >= headerHeight && e.x >= pianoKeysWidth)
    noteAtPointer = findNoteAt(adjustedX, adjustedY);

  const bool altControlsNoteSnap =
      editMode == EditMode::Select && noteAtPointer != nullptr;
  if (!showZoomCursor && isCanvasPoint(e) && e.mods.isAltDown() &&
      !altControlsNoteSnap)
  {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor); // Alt-drag: vertical zoom
    return;
  }

  // Hide the bound take outside the track's playback windows, rather than
  // treating any one region as the interaction boundary.
  const bool overHoverControls = hoveredNote &&
      ((pitchToolHandles && pitchToolHandles->containsLayoutPoint(adjustedX, adjustedY)) ||
       findPreviewButtonNoteAt(adjustedX, adjustedY));
  if (!showZoomCursor && isCanvasPoint(e) && trackViewMode && !overHoverControls &&
      findTrackRegionAtPointer(e.position) == nullptr)
  {
    setHoveredNote(nullptr);
    loopDragHandler_->mouseMove(e, adjustedX, adjustedY);
    setMouseCursor(loopDragHandler_->isHoveringHandle()
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::NormalCursor);
    return;
  }

  // A note directly under the pointer owns the hover. The retired per-note
  // controls no longer extend that area, so leaving the note clears it.
  Note *noteUnderMouse = noteAtPointer;
  if (!noteUnderMouse && hoveredNote && pitchToolHandles &&
      !pitchToolHandles->isEmpty() &&
      pitchToolHandles->hitTest(adjustedX, adjustedY) >= 0)
  {
    // Kept for the future floating tools section: a handle hit keeps the
    // owner visible.
    noteUnderMouse = hoveredNote;
  }
  // Pitch Drawing mode uses its own point hover treatment. Split mode must wait until
  // its handler has resolved merge-boundary hover before changing hoveredNote.
  // Otherwise the preview/reset child components are shown and hidden again
  // during the same mouse event, which can stall the Windows mouse-message
  // loop while a custom cursor is active over a boundary.
  if (editMode != EditMode::Split)
    setHoveredNote(editMode == EditMode::PitchDrawing ? nullptr : noteUnderMouse);

  // Loop timeline cursor handling (always active)
  loopDragHandler_->mouseMove(e, adjustedX, adjustedY);

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseMove(e, adjustedX, adjustedY);

  // A merge boundary draws its own two-note hover treatment. Resolve the
  // split-mode hover once, after boundary hit-testing, so child controls never
  // enter a transient visible state over the boundary.
  if (editMode == EditMode::Split)
    setHoveredNote(splitHandler_ && splitHandler_->isHoveringMergeBoundary()
                       ? nullptr
                       : noteUnderMouse);

  // Pitch tool handle hover (uses raw event coordinates, not world-adjusted)
  if (editMode == EditMode::Select && pitchToolHandles && !pitchToolHandles->isEmpty() &&
      e.y >= headerHeight && e.x >= pianoKeysWidth)
  {
    int hitIndex = pitchToolHandles->hitTest(adjustedX, adjustedY);
    if (hitIndex != hoveredPitchToolHandle)
    {
      hoveredPitchToolHandle = hitIndex;
      pitchToolHandles->setHoveredHandleIndex(hitIndex);
      repaint();
    }
  }
  else if (hoveredPitchToolHandle != -1)
  {
    hoveredPitchToolHandle = -1;
    if (pitchToolHandles)
      pitchToolHandles->setHoveredHandleIndex(-1);
    repaint();
  }

  // Cycle handles take priority over every edit-mode cursor. Once the pointer
  // leaves a handle, the active tool immediately restores its canvas cursor.
  if (loopDragHandler_ && loopDragHandler_->isHoveringHandle())
  {
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
  }
  else if (showZoomCursor)
  {
    setMouseCursor(zoomMouseCursor);
  }
  else if (editMode == EditMode::Select)
  {
    // Melodyne: sobre la franja superior de la nota el cursor anuncia el corte.
    const Note *noteUnderPointer =
        (isCanvasPoint(e) && e.y >= headerHeight)
            ? findNoteAt(adjustedX, adjustedY)
            : nullptr;
    if (noteUnderPointer != nullptr && isNoteTopZone(*noteUnderPointer, adjustedY))
      setMouseCursor(splitMouseCursor);
    else
      setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::Split)
  {
    if (!isCanvasPoint(e))
      setMouseCursor(juce::MouseCursor::NormalCursor);
    else
    {
      const bool hoveringMergeBoundary =
          splitHandler_ && splitHandler_->isHoveringMergeBoundary();
      const bool hoveringNote =
          splitHandler_ && splitHandler_->getSplitGuideNote() != nullptr;
      setMouseCursor(hoveringMergeBoundary
                         ? mergeMouseCursor
                         : (hoveringNote ? splitMouseCursor
                                         : juce::MouseCursor::NormalCursor));
    }
  }
  else if (editMode == EditMode::PitchDrawing)
  {
    if (!isCanvasPoint(e) || !pitchDrawingHandler_)
      setMouseCursor(juce::MouseCursor::NormalCursor);
    else if (pitchDrawingHandler_->isHoveringAnchor())
      setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    else
      setMouseCursor(pitchDrawingHandler_->isPointerOverPitchRegion()
                         ? juce::MouseCursor::CrosshairCursor
                         : juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::Timing)
  {
    // TimingHandler owns edge-specific cursor arbitration.
  }
}

void PianoRollComponent::mouseEnter(const juce::MouseEvent &e)
{
  // The pointer may enter from a toolbar child whose pointing-hand cursor was
  // active when the edit mode changed. Re-run canvas cursor arbitration on
  // entry instead of waiting for a later move event.
  mouseMove(e);
}

void PianoRollComponent::modifierKeysChanged(
    const juce::ModifierKeys &modifiers)
{
  const auto mousePosition = getMouseXYRelative();
  const bool mouseOverCanvas =
      mousePosition.x >= pianoKeysWidth && mousePosition.y >= headerHeight &&
      mousePosition.x < pianoKeysWidth + getVisibleContentWidth() &&
      mousePosition.y < headerHeight + getVisibleContentHeight();
  const float worldX = mousePosition.x - pianoKeysWidth +
                       static_cast<float>(scrollX);
  const float worldY = mousePosition.y - headerHeight +
                       static_cast<float>(scrollY);
  const bool pointerOverNote =
      mouseOverCanvas && editMode == EditMode::Select &&
      findNoteAt(worldX, worldY) != nullptr;

#if JUCE_MAC
  const bool zoomModifierDown = modifiers.isCommandDown();
#else
  const bool zoomModifierDown = modifiers.isCtrlDown();
#endif

  if (zoomModifierDown && mouseOverCanvas)
    setMouseCursor(zoomMouseCursor);
  else if (modifierPanDragActive)
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
  else if (modifiers.isAltDown() && mouseOverCanvas && !pointerOverNote)
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
  else
    updateMouseCursorForEditMode();
}

void PianoRollComponent::mouseExit(const juce::MouseEvent &)
{
  if (editMode == EditMode::PitchDrawing && pitchDrawingHandler_)
    pitchDrawingHandler_->clearHover();

  if (previewButton.isMouseOverOrDragging() ||
      resetButton.isMouseOverOrDragging())
    return;

  setHoveredNote(nullptr);
}

juce::String PianoRollComponent::getTooltip()
{
  if (!pitchToolHandles || pitchToolHandles->isEmpty())
    return {};

  const auto mousePosition = getMouseXYRelative();
  const float adjustedX = mousePosition.x - pianoKeysWidth +
                          static_cast<float>(scrollX);
  const float adjustedY = mousePosition.y - headerHeight +
                          static_cast<float>(scrollY);
  const int handleIndex = pitchToolHandles->hitTest(adjustedX, adjustedY);
  if (handleIndex < 0)
    return {};

  switch (pitchToolHandles->getHandle(handleIndex).type)
  {
    case PitchToolHandles::HandleType::TiltLeft: return TR("handle.tilt_left");
    case PitchToolHandles::HandleType::Vibrato: return TR("handle.vibrato");
    case PitchToolHandles::HandleType::PitchDrift: return TR("handle.pitch_drift");
    case PitchToolHandles::HandleType::Amplitude: return TR("handle.amplitude");
    case PitchToolHandles::HandleType::Formant: return TR("handle.formant");
    case PitchToolHandles::HandleType::TiltRight: return TR("handle.tilt_right");
    case PitchToolHandles::HandleType::CorrectPitch: return TR("handle.correct_pitch");
    case PitchToolHandles::HandleType::GlueLeft: return TR("handle.glue_left");
    case PitchToolHandles::HandleType::GlueRight: return TR("handle.glue_right");
    default: return {};
  }
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent &e)
{
  // JUCE dispatches double-clicks after mouseUp clears the scrub state.
  // Keep repeated middle clicks from triggering transport or edit actions.
  if (e.mods.isMiddleButtonDown() || e.mods.isPopupMenu())
    return;

  if (!project)
    return;

  // VariAudio: Option/Alt-double-click where there is no segment shows the
  // whole event (as Shift+F does). A double-click that began on a segment
  // (whose first Option-click split it) is not one.
  if (e.mods.isAltDown() && isCanvasPoint(e))
  {
    const float worldX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
    const float worldY = e.y - headerHeight + static_cast<float>(scrollY);
    if (findNoteAt(worldX, worldY) == nullptr && !clickStartedOnSegment)
    {
      if (onZoomFullRequested)
        onZoomFullRequested();
      return;
    }
  }

  // VariAudio: Shift-double-click selects this and every later segment of
  // the same pitch.
  if (editMode == EditMode::Select && e.mods.isShiftDown() && isCanvasPoint(e))
  {
    const float worldX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
    const float worldY = e.y - headerHeight + static_cast<float>(scrollY);
    if (auto *note = findNoteAt(worldX, worldY))
    {
      selectFollowingSamePitch(*note);
      return;
    }
  }

  if (e.y >= timelineHeight && e.y < headerHeight && e.x >= pianoKeysWidth)
  {
    auto loopRange = project->getLoopRange();
    if (loopRange.endSeconds > loopRange.startSeconds)
    {
      project->setLoopEnabled(false);
      if (onLoopRangeChanged)
        onLoopRangeChanged(project->getLoopRange());
      repaint();
    }
    return;
  }

  // Ignore double-clicks outside main area
  if (e.y < headerHeight || e.x < pianoKeysWidth)
    return;

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // An empty-space double click works uniformly across track regions.
  const auto* pointerRegion = findTrackRegionAtPointer(e.position);
  if (pointerRegion && pointerRegion->sourceProject != project) {
    bool overNote = false;
    if (getTrackEditTargets)
      for (const auto& target : getTrackEditTargets())
        for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames))
          for (const auto& bounds : trackNoteBounds(*note, *target.project))
            overNote |= bounds.contains(e.position);
    if (!overNote && onCanvasEmptyDoubleClick)
      onCanvasEmptyDoubleClick(std::max(getTimelineStart(), xToTime(adjustedX)));
    return;
  }

  // Anchors may be placed vertically outside their owning note rectangle, so
  // give Pitch Drawing mode first chance to remove one before the empty-canvas
  // double-click transport behavior runs.
  if (editMode == EditMode::PitchDrawing && pitchDrawingHandler_ &&
      pitchDrawingHandler_->removeAnchorAt(adjustedX, adjustedY))
    return;

  // D-285: doble clic sobre un corte ya hecho vuelve a unir (Melodyne). Va
  // antes del afinar del cuerpo para que un corte no se convierta en un cambio
  // de afinacion. Solo une verdaderos cortes (misma nota partida, D-225).
  //
  // D-285/2: si el corte lo acaba de hacer el `mouseDown` de ESTE mismo doble
  // clic, no se une: se marca y se consume, para que el gesto deje el corte.
  const bool splitJustPerformed = this->splitJustPerformed;
  this->splitJustPerformed = false;
  if (editMode == EditMode::Select && !e.mods.isAnyModifierKeyDown() &&
      !splitJustPerformed)
  {
    if (auto mergeable = noteSplitter->findMergeableNotesAt(adjustedX, adjustedY);
        mergeable.first != nullptr && mergeable.second != nullptr)
    {
      if (noteSplitter->mergeNotes(mergeable.first, mergeable.second))
      {
        // mergeNotes rebuilds the note list; pointers into it are no longer safe.
        dropTransientNotePointers();
        updatePitchToolHandlesFromSelection();
        repaint();
      }
      return;
    }
  }

  // Melodyne: en la franja superior de la nota el doble clic es un corte (ya
  // hecho por el mouseDown, que es quien conoce los clics). Aqui no se afina:
  // cortar no debe cambiar la afinacion de ninguna mitad (D-285). Sin esta
  // guarda, el segundo clic cortaba y este afinaba, que es el sintoma
  // «cortar parece afinar».
  if (editMode == EditMode::Select && !e.mods.isAnyModifierKeyDown())
    if (auto *topZoneNote = findNoteAt(adjustedX, adjustedY))
      if (isNoteTopZone(*topZoneNote, adjustedY))
        return;

  // Melodyne: doble clic en el cuerpo de la nota (fuera de la franja superior)
  // afina al 100 % hacia el semitono mas cercano. Pasa por el inspector para que
  // sea un paso de undo como los sliders.
  if (editMode == EditMode::Select && !e.mods.isAnyModifierKeyDown())
  {
    if (auto *note = findNoteAt(adjustedX, adjustedY))
    {
      if (!isNoteTopZone(*note, adjustedY) &&
          !(pitchToolHandles && pitchToolHandles->hitTest(adjustedX, adjustedY) >= 0))
      {
        selectOnlyNote(project, note);
        beginInspectorEdit(InspectorParam::CorrectPitch);
        setInspectorEditValue(100.0f);
        endInspectorEdit();
        updatePitchToolHandlesFromSelection();
        repaint();
        return;
      }
    }
  }

  // Restablecer a original: Option/Alt + doble clic en el segmento. El doble clic
  // simple afina (arriba corta), y el menu contextual sigue teniendo
  // «Restablecer original».
  if (editMode == EditMode::Select && e.mods.isAltDown() && !e.mods.isCommandDown())
    if (auto *note = findNoteAt(adjustedX, adjustedY))
      if (!(pitchToolHandles && pitchToolHandles->hitTest(adjustedX, adjustedY) >= 0))
      {
        // Only the double-clicked segment, and only when it was edited (no
        // empty undo steps). A Correct Pitch reference that drifted (nudges
        // that went out and back) counts as an edit too.
        if (!note->isNeutralForOriginalWaveform() ||
            std::abs(note->getLastNonMacroMidiNote() - note->getOriginalMidiNote()) > 1.0e-4f)
        {
          applyRestoreAction(std::make_unique<ResetNoteEditsAction>(
              *project, std::vector<Note *>{note}, true, true, true,
              "Restore All Edits", createResetTimingAction(*note)));
          dropTransientNotePointers();
        }
        return;
      }

  // Keep note-specific double-click interactions with their edit handlers.
  // Everywhere else on the canvas, a double-click controls transport at the
  // clicked time rather than changing the current edit selection.
  if (!findNoteAt(adjustedX, adjustedY))
  {
    if (onCanvasEmptyDoubleClick)
      onCanvasEmptyDoubleClick(std::max(getTimelineStart(), xToTime(adjustedX)));
    return;
  }

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseDoubleClick(e, adjustedX, adjustedY);
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent &e,
                                        const juce::MouseWheelDetails &wheel)
{
  float scrollMultiplier = wheel.isSmooth ? 200.0f : 80.0f;
  const int visibleHeight = getVisibleContentHeight();
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineDuration();
  const float minPpsForFill =
      visibleHeight > 0
          ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
          : MIN_PIXELS_PER_SEMITONE;
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime))
          : MIN_PIXELS_PER_SECOND;

  bool isOverPianoKeys = e.x < pianoKeysWidth;
  bool isOverTimeline = e.y < headerHeight;

  // Option/Alt + wheel: vertical zoom around the pointer (Cmd + wheel is
  // horizontal).
  if (e.mods.isAltDown() && !e.mods.isCommandDown() && !e.mods.isCtrlDown())
  {
    const float mouseY = static_cast<float>(e.y - headerHeight);
    const float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
    float newPps = pixelsPerSemitone * zoomFactor;
    if (zoomFactor < 1.0f)
    {
      const float range = minPps * 0.35f;
      const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (pixelsPerSemitone - minPps) / range) : 0.0f;
      newPps = pixelsPerSemitone * (1.0f + (zoomFactor - 1.0f) * t);
    }
    setPixelsPerSemitone(juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, newPps), mouseY);
    return;
  }

  // Hover-based zoom (no modifier keys needed)
  if (!e.mods.isCommandDown() && !e.mods.isCtrlDown())
  {
    // Over piano keys: vertical zoom
    if (isOverPianoKeys)
    {
      float mouseY = e.y - headerHeight;

      float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
      if (zoomFactor < 1.0f)
      {
        const float range = minPps * 0.35f;
        const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (pixelsPerSemitone - minPps) / range) : 0.0f;
        zoomFactor = 1.0f + (zoomFactor - 1.0f) * t; // elastic resistance near min
      }
      float newPps = pixelsPerSemitone * zoomFactor;
      newPps = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, newPps);
      setPixelsPerSemitone(newPps, mouseY);
      return;
    }

    // Over timeline: horizontal zoom
    if (isOverTimeline)
    {
      // Calculate time at mouse position before zoom
      float mouseX = e.x - pianoKeysWidth;
      double timeAtMouse = (mouseX + scrollX) / pixelsPerSecond;

      float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
      float newPps = pixelsPerSecond * zoomFactor;
      newPps =
          juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, newPps);
      pixelsPerSecond = newPps;
      coordMapper->setPixelsPerSecond(newPps);

      // Adjust scroll position to keep time at mouse position fixed
      double newScrollX = timeAtMouse * pixelsPerSecond - mouseX;
      newScrollX = clampScrollX(newScrollX);
      scrollX = newScrollX;
      coordMapper->setScrollX(newScrollX);

      updateScrollBars();
      repaint();
      if (onZoomChanged)
        onZoomChanged(pixelsPerSecond);
      return;
    }

    // Normal scrolling in grid area
    float deltaX = wheel.deltaX;
    float deltaY = wheel.deltaY;

    if (e.mods.isShiftDown() && std::abs(deltaX) < 0.001f)
    {
      deltaX = deltaY;
      deltaY = 0.0f;
    }

    if (std::abs(deltaX) > 0.001f)
    {
      double newScrollX = scrollX - deltaX * scrollMultiplier;
      newScrollX = clampScrollX(newScrollX);
      horizontalScrollBar.setCurrentRangeStart(newScrollX);
    }

    if (std::abs(deltaY) > 0.001f)
    {
      double newScrollY = scrollY - deltaY * scrollMultiplier;
      verticalScrollBar.setCurrentRangeStart(newScrollY);
    }
    return;
  }

  // Key-based zoom in grid area: Cmd/Ctrl + wheel zooms BOTH axes around the
  // pointer (pedido el 2026-11-23, como REAPER). Cmd/Ctrl + Shift mantiene el
  // zoom vertical solo, para mover un eje cuando haga falta.
  if (e.mods.isCommandDown() || e.mods.isCtrlDown())
  {
    float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
    const float mouseY = static_cast<float>(e.y - headerHeight);

    if (!e.mods.isShiftDown())
    {
      // Horizontal: mantener el tiempo bajo el puntero
      float mouseX = static_cast<float>(e.x - pianoKeysWidth);
      double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));

      float newPpsX = pixelsPerSecond * zoomFactor;
      newPpsX = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, newPpsX);

      pixelsPerSecond = newPpsX;
      coordMapper->setPixelsPerSecond(newPpsX);
      const float newMouseX = static_cast<float>(timeAtMouse * newPpsX);
      scrollX = clampScrollX(static_cast<double>(newMouseX - mouseX));
      coordMapper->setScrollX(scrollX);
    }

    // Vertical: mantener la nota bajo el puntero
    float newPpsY = pixelsPerSemitone * zoomFactor;
    if (zoomFactor < 1.0f)
    {
      const float range = minPps * 0.35f;
      const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (pixelsPerSemitone - minPps) / range) : 0.0f;
      newPpsY = pixelsPerSemitone * (1.0f + (zoomFactor - 1.0f) * t);
    }
    setPixelsPerSemitone(juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, newPpsY), mouseY);

    updateScrollBars();
    repaint();

    if (onZoomChanged)
      onZoomChanged(pixelsPerSecond);
  }
}

void PianoRollComponent::mouseMagnify(const juce::MouseEvent &e,
                                      float scaleFactor)
{
  // Pinch-to-zoom on trackpad - horizontal zoom, center on mouse position
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineLength();
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime))
          : MIN_PIXELS_PER_SECOND;
  float mouseX = static_cast<float>(e.x - pianoKeysWidth);
  double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));

  float newPps = pixelsPerSecond * scaleFactor;
  newPps = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, newPps);

  // Adjust scroll to keep mouse position stable
  pixelsPerSecond = newPps;
  coordMapper->setPixelsPerSecond(newPps);
  float newMouseX = static_cast<float>(timeAtMouse * newPps);
  scrollX = clampScrollX(static_cast<double>(newMouseX - mouseX));
  coordMapper->setScrollX(scrollX);

  coordMapper->setPixelsPerSecond(newPps);
  updateScrollBars();
  repaint();

  if (onZoomChanged)
    onZoomChanged(pixelsPerSecond);
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar *scrollBar,
                                        double newRangeStart)
{
  if (scrollBar == &horizontalScrollBar)
  {
    scrollX = clampScrollX(newRangeStart);
    coordMapper->setScrollX(scrollX);

    // Notify scroll changed for synchronization
    if (onScrollChanged)
      onScrollChanged(scrollX);
  }
  else if (scrollBar == &verticalScrollBar)
  {
    scrollY = newRangeStart;
    coordMapper->setScrollY(newRangeStart);
  }
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setProject(Project *proj)
{
  if (pitchDrawingHandler_)
    pitchDrawingHandler_->cancel();
  liveTimelineEndSeconds = 0.0;
  liveRecordingSampleRate = 0.0;
  project = proj;
  // R2/D-313: the ghost list belongs to the previous document. A different
  // project (or none) drops it; the same one keeps it because the painter
  // re-maps host -> project on every frame.
  midiGuideGhosts.bindProject(proj);
  hoveredNote = nullptr;
  // D-309: the visual block map belongs to the previous document.
  engineBlockCache = EngineBlockCache{};
  setPreviewPlaybackState(false, 0, 0);
  selectedScaleMode =
      project != nullptr ? project->getScaleMode() : ScaleMode::Chromatic;
  selectedScaleRootNote = project != nullptr ? project->getScaleRootNote() : 0;
  pitchReferenceHz = project != nullptr ? project->getPitchReferenceHz() : 440;
  snapToSemitoneDrag = project != nullptr ? project->getSnapToSemitones() : false;
  dragSnapMode = project != nullptr
                     ? project->getDragSnapMode()
                     : DragSnapMode::Chromatic;
  timelineDisplayMode = project != nullptr
                            ? project->getTimelineDisplayMode()
                            : TimelineDisplayMode::Beats;
  timelineBeatNumerator = project != nullptr ? project->getTimelineBeatNumerator() : 4;
  timelineBeatDenominator =
      project != nullptr ? project->getTimelineBeatDenominator() : 4;
  timelineTempoBpm = project != nullptr ? project->getTimelineTempoBpm() : 120.0;
  timelineGridDivision = project != nullptr
                             ? project->getTimelineGridDivision()
                             : TimelineGridDivision::Quarter;
  timelineSnapCycle = project != nullptr ? project->getTimelineSnapCycle() : false;
  previewScaleRootNote.reset();
  previewScaleMode.reset();

  // Update modular components
  gridRenderer->setProject(proj);
  timelineRenderer->setProject(proj);
  waveformBackgroundRenderer->setProject(proj); // also clears its cache
  noteRenderer->setProject(proj);
  pitchCurveRenderer->setProject(proj); // also clears its cache
  scrollZoomController->setProject(proj);
  pitchEditor->setProject(proj);
  pitchEditor->setSnapToSemitoneDragEnabled(snapToSemitoneDrag);
  pitchEditor->setDragSnapMode(dragSnapMode);
  pitchEditor->setPitchReferenceHz(pitchReferenceHz);
  noteSplitter->setProject(proj);
  pitchToolController->setProject(proj);

  // Note: waveform and base-pitch caches are cleared by their renderers'
  // setProject calls above.

  updatePitchToolHandlesFromSelection();

  updateScrollBars();
  repaint();
}

void PianoRollComponent::beginLiveRecordingWaveform(
    double sampleRate, double timelineOffsetSeconds)
{
  waveformBackgroundRenderer->beginLiveWaveform(sampleRate,
                                                timelineOffsetSeconds);
  liveRecordingSampleRate = sampleRate;
  liveTimelineEndSeconds = std::max(0.0, timelineOffsetSeconds);
  updateScrollBars();
  repaint();
}

void PianoRollComponent::appendLiveRecordingWaveform(
    const juce::AudioBuffer<float> &buffer)
{
  waveformBackgroundRenderer->appendLiveWaveform(buffer);
  if (liveRecordingSampleRate > 0.0)
    liveTimelineEndSeconds +=
        static_cast<double>(buffer.getNumSamples()) / liveRecordingSampleRate;
  updateScrollBars();
  repaint();
}

bool PianoRollComponent::extendTimelineTo(double endSeconds)
{
  const double newEndSeconds = std::max(0.0, endSeconds);
  if (newEndSeconds <= hostTimelineEndSeconds)
    return false;

  hostTimelineEndSeconds = newEndSeconds;
  updateScrollBars();
  repaint();
  return true;
}

void PianoRollComponent::refreshScaleFromProject()
{
  selectedScaleRootNote = project ? project->getScaleRootNote() : 0;
  selectedScaleMode = project ? project->getScaleMode() : ScaleMode::Chromatic;
  previewScaleRootNote.reset();
  previewScaleMode.reset();
  repaint();
}

void PianoRollComponent::setScaleMode(ScaleMode mode)
{
  if (selectedScaleMode == mode && !previewScaleMode.has_value())
    return;

  selectedScaleMode = mode;
  if (project != nullptr)
    project->setScaleMode(mode);
  previewScaleMode.reset();
  repaint();
}

void PianoRollComponent::setScaleRootNote(int noteInOctave)
{
  const int normalized = juce::jlimit(-1, 11, noteInOctave);
  const bool changed = selectedScaleRootNote != normalized;
  if (!changed && !previewScaleRootNote.has_value())
    return;

  selectedScaleRootNote = normalized;
  if (project != nullptr && changed)
    project->setScaleRootNote(normalized);
  previewScaleRootNote.reset();
  repaint();
}

void PianoRollComponent::setScaleRootPreview(std::optional<int> noteInOctave)
{
  std::optional<int> normalizedPreview;
  if (noteInOctave.has_value())
    normalizedPreview = juce::jlimit(-1, 11, *noteInOctave);

  if (previewScaleRootNote == normalizedPreview)
    return;

  previewScaleRootNote = normalizedPreview;
  repaint();
}

void PianoRollComponent::setScaleModePreview(std::optional<ScaleMode> mode)
{
  if (mode.has_value() &&
      (selectedScaleMode == ScaleMode::Chromatic ||
       selectedScaleMode == ScaleMode::None) &&
      !(snapToSemitoneDrag && dragSnapMode == DragSnapMode::Scale))
    mode.reset();

  if (previewScaleMode == mode)
    return;

  previewScaleMode = mode;
  repaint();
}

void PianoRollComponent::setSnapToSemitoneDrag(bool enabled)
{
  if (snapToSemitoneDrag == enabled)
    return;

  snapToSemitoneDrag = enabled;
  if (project != nullptr)
    project->setSnapToSemitones(enabled);
  pitchEditor->setSnapToSemitoneDragEnabled(enabled);
  repaint();
}

void PianoRollComponent::setDragSnapMode(DragSnapMode mode)
{
  if (dragSnapMode == mode)
    return;

  dragSnapMode = mode;
  if (project != nullptr)
    project->setDragSnapMode(mode);
  pitchEditor->setDragSnapMode(mode);
  repaint();
}

void PianoRollComponent::setPitchReferenceHz(int hz)
{
  const int normalized = juce::jlimit(420, 450, hz);
  if (pitchReferenceHz == normalized)
    return;

  pitchReferenceHz = normalized;
  if (project != nullptr)
    project->setPitchReferenceHz(normalized);
  pitchEditor->setPitchReferenceHz(normalized);
  repaint();
}

void PianoRollComponent::setTimelineDisplayMode(TimelineDisplayMode mode)
{
  if (timelineDisplayMode == mode)
    return;

  timelineDisplayMode = mode;
  if (project != nullptr)
    project->setTimelineDisplayMode(mode);
  repaint();
}

void PianoRollComponent::setTimelineBeatSignature(int numerator, int denominator,
                                                 bool persistToProject)
{
  const int normalizedNumerator = juce::jlimit(1, 32, numerator);
  const int normalizedDenominator = normalizeTimelineBeatDenominator(denominator);
  if (timelineBeatNumerator == normalizedNumerator &&
      timelineBeatDenominator == normalizedDenominator)
    return;

  timelineBeatNumerator = normalizedNumerator;
  timelineBeatDenominator = normalizedDenominator;
  if (project != nullptr && persistToProject)
    project->setTimelineBeatSignature(normalizedNumerator, normalizedDenominator);
  repaint();
}

void PianoRollComponent::setTimelineTempoBpm(double bpm, bool persistToProject)
{
  const double normalized = juce::jlimit(20.0, 300.0, bpm);
  if (std::abs(timelineTempoBpm - normalized) < 1.0e-6)
    return;

  timelineTempoBpm = normalized;
  if (project != nullptr && persistToProject)
    project->setTimelineTempoBpm(normalized);
  repaint();
}

void PianoRollComponent::setHostTempoMap(
    std::shared_ptr<const HostTempoMap> tempoMap)
{
  if (hostTempoMap == tempoMap)
    return;

  hostTempoMap = std::move(tempoMap);
  // R3 (D-316): the grid and the ruler now read this map, so a new snapshot
  // must be drawn at once.
  repaint();
}

void PianoRollComponent::setMidiGuideGhosts(std::vector<MidiGuideGhost> ghosts)
{
  const auto &current = midiGuideGhosts.all();
  if (ghosts.size() == current.size())
  {
    bool same = true;
    for (size_t i = 0; i < ghosts.size() && same; ++i)
    {
      const auto& a = ghosts[i];
      const auto& b = current[i];
      same = std::abs(a.startHostSeconds - b.startHostSeconds) < 1.0e-6 &&
             std::abs(a.endHostSeconds - b.endHostSeconds) < 1.0e-6 &&
             std::abs(a.pitch - b.pitch) < 1.0e-6 && a.label == b.label;
    }
    if (same)
      return;
  }

  midiGuideGhosts.set(std::move(ghosts));
  repaint();
}

void PianoRollComponent::drawMidiGuideGhosts(juce::Graphics &g)
{
  if (midiGuideGhosts.empty() || coordMapper == nullptr)
    return;

  // Read-only layer: dashed cyan blocks under the notes. Deliberately not the
  // grey source curve retired in D-305: this is the imported reference.
  const juce::Colour ghostColour(0xFF35C4CF);
  const double visibleStart = xToTime(0.0f);
  const double visibleEnd = xToTime(static_cast<float>(getWidth()));

  const float dashLengths[] = {4.0f, 3.0f};
  juce::PathStrokeType stroke(1.2f);

  for (const auto &ghost : midiGuideGhosts.all())
  {
    // R2/D-313: host -> project here, with the offset the mapper publishes
    // now. The ghost keeps its host instant whatever item is active, so
    // selecting another item cannot leave the list where the old one put it.
    const double startProject =
        coordMapper->timelineToProject(ghost.startHostSeconds);
    const double endProject =
        coordMapper->timelineToProject(ghost.endHostSeconds);

    if (endProject < visibleStart || startProject > visibleEnd)
      continue;

    const float x = timeToX(startProject);
    const float right = timeToX(endProject);
    const float width = juce::jmax(2.0f, right - x);
    auto rect = juce::Rectangle<float>(x, midiToY(ghost.pitch), width,
                                       juce::jmax(6.0f, pixelsPerSemitone));
    rect = rect.reduced(0.0f, 1.0f);

    g.setColour(ghostColour.withAlpha(0.10f));
    g.fillRoundedRectangle(rect, 3.0f);

    juce::Path outline;
    outline.addRoundedRectangle(rect, 3.0f);
    juce::Path dashed;
    stroke.createDashedStroke(dashed, outline, dashLengths, 2);
    g.setColour(ghostColour.withAlpha(0.85f));
    g.fillPath(dashed);

    if (width > 26.0f)
    {
      g.setColour(ghostColour.withAlpha(0.9f));
      g.setFont(AppFont::getFont(juce::jmin(11.0f, pixelsPerSemitone * 0.9f)));
      g.drawText(juce::String(ghost.label),
                 rect.withTrimmedLeft(3.0f).toNearestInt(),
                 juce::Justification::centredLeft, false);
    }
  }
}

void PianoRollComponent::setTimelineGridDivision(TimelineGridDivision division)
{
  if (timelineGridDivision == division)
    return;

  timelineGridDivision = division;
  if (project != nullptr)
    project->setTimelineGridDivision(division);
  repaint();
}

void PianoRollComponent::setTimelineSnapCycle(bool enabled)
{
  if (timelineSnapCycle == enabled)
    return;

  timelineSnapCycle = enabled;
  if (project != nullptr)
    project->setTimelineSnapCycle(enabled);
}

void PianoRollComponent::setCycleEditEnabled(bool enabled)
{
  if (cycleEditEnabled == enabled)
    return;

  cycleEditEnabled = enabled;
  if (!enabled && loopDragHandler_)
    loopDragHandler_->cancel();
  setMouseCursor(juce::MouseCursor::NormalCursor);
  repaint();
}

void PianoRollComponent::setUndoManager(PitchUndoManager *manager)
{
  undoManager = manager;
  pitchEditor->setUndoManager(manager);
  noteSplitter->setUndoManager(manager);
}

bool PianoRollComponent::nudgeSelectedNotesBySemitones(int semitoneDelta)
{
  if (semitoneDelta == 0)
    return false;

  if (documentBatchApplies())
    return applyDocumentBatch(
        "Nudge Pitch",
        [semitoneDelta](const std::vector<documentNoteBatch::Target> &targets)
        {
          return documentNoteBatch::shiftPitch(
              targets, [semitoneDelta](const Project &, const Note &)
              { return std::optional<float>(static_cast<float>(semitoneDelta)); });
        });

  if (project == nullptr)
    return false;

  return shiftNotesBySemitones(
      project->getSelectedNotes(), [semitoneDelta](const Note &)
      { return std::optional<float>(static_cast<float>(semitoneDelta)); });
}

bool PianoRollComponent::nudgeSelectedNotesToNextPitch(int direction)
{
  if (direction == 0)
    return false;

  const float reference = ScaleUtils::getReferenceOffsetSemitones(pitchReferenceHz);
  const auto shiftFor = [=](const Project &owner, const Note &note)
      -> std::optional<float>
  {
    const ScaleMode mode = owner.getPreferredScaleMode();
    const int root = owner.getScaleRootNote();
    const bool useScale = owner.getDragSnapMode() == DragSnapMode::Scale &&
                          mode != ScaleMode::None && mode != ScaleMode::Chromatic &&
                          root >= 0;
    // Absolute: step from the nearest semitone (or scale tone) so the
    // segment lands exactly on the next one.
    const float adjusted = note.getAdjustedMidiNote();
    int target = static_cast<int>(std::round(adjusted - reference)) + direction;
    if (useScale)
      for (int guard = 0;
           guard < 12 && !ScaleUtils::isPitchClassInScale(mode, target, root);
           ++guard)
        target += direction;
    return std::optional<float>(static_cast<float>(target) + reference - adjusted);
  };

  // N3: the batch reads the scale of each note's own item, not the active one.
  // N4: the whole batch is one undo step.
  if (documentBatchApplies())
    return applyDocumentBatch(
        "Nudge Pitch",
        [&shiftFor](const std::vector<documentNoteBatch::Target> &targets)
        { return documentNoteBatch::shiftPitch(targets, shiftFor); });

  if (project == nullptr)
    return false;

  Project *const owner = project;
  return shiftNotesBySemitones(
      project->getSelectedNotes(),
      [&shiftFor, owner](const Note &note)
      { return shiftFor(*owner, note); });
}

bool PianoRollComponent::nudgeSelectedNotesByCents(int cents)
{
  if (cents == 0)
    return false;

  if (documentBatchApplies())
    return applyDocumentBatch(
        "Nudge Pitch",
        [cents](const std::vector<documentNoteBatch::Target> &targets)
        {
          return documentNoteBatch::shiftPitch(
              targets, [cents](const Project &, const Note &)
              { return std::optional<float>(static_cast<float>(cents) / 100.0f); });
        });

  if (project == nullptr)
    return false;

  return shiftNotesBySemitones(
      project->getSelectedNotes(), [cents](const Note &)
      { return std::optional<float>(static_cast<float>(cents) / 100.0f); });
}

PitchEditor::DragSnapKind
PianoRollComponent::resolveDragSnapKind(const juce::ModifierKeys &mods) const
{
  using Kind = PitchEditor::DragSnapKind;
  // Option/Alt desactiva el iman (pedido el 2026-11-23, como REAPER/Melodyne).
  // Shift sigue valiendo como alias historico.
  if (mods.isAltDown())
    return Kind::Off;
  if (mods.isShiftDown())
    return Kind::Off;
  if (mods.isCommandDown())
    return Kind::Absolute;
  if (!snapToSemitoneDrag)
    return Kind::Off;
  return absoluteDragSnap ? Kind::Absolute : Kind::Relative;
}

std::vector<Note *> PianoRollComponent::getInspectorTargets() const
{
  // N8 (D-348): in track mode the inspector speaks in document terms. The
  // objective is the document selection when there is one, otherwise every
  // visible note of the published index. Both point at the real Notes; this
  // function copies neither the notes nor their edits. It never follows the
  // project a hover left active.
  if (documentSelectionAvailable())
  {
    std::vector<Note *> targets;
    if (!documentNoteSelection->isEmpty())
    {
      targets.reserve(static_cast<size_t>(documentNoteSelection->size()));
      for (const auto &entry : documentNoteSelection->entries())
        if (entry.note != nullptr && !entry.note->isRest() &&
            !entry.note->isDeleted())
          targets.push_back(entry.note);
    }
    else
    {
      targets.reserve(documentNoteIndex->notes.size());
      for (const auto &ref : documentNoteIndex->notes)
        if (ref.note != nullptr && !ref.note->isRest() && !ref.note->isDeleted())
          targets.push_back(ref.note);
    }
    return targets;
  }

  std::vector<Note *> selected, all;
  if (project == nullptr)
    return all;
  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;
    all.push_back(&note);
    if (note.isSelected())
      selected.push_back(&note);
  }
  return selected.empty() ? all : selected;
}

std::vector<Project *> PianoRollComponent::inspectorTargetProjects() const
{
  // N8 (D-348): same order as getInspectorTargets(), same pointers. Needed so
  // Correct Pitch reads each note's own scale instead of the active item's.
  if (documentSelectionAvailable())
  {
    std::vector<Project *> owners;
    if (!documentNoteSelection->isEmpty())
    {
      owners.reserve(static_cast<size_t>(documentNoteSelection->size()));
      for (const auto &entry : documentNoteSelection->entries())
        if (entry.note != nullptr && !entry.note->isRest() &&
            !entry.note->isDeleted())
          owners.push_back(entry.project);
    }
    else
    {
      owners.reserve(documentNoteIndex->notes.size());
      for (const auto &ref : documentNoteIndex->notes)
        if (ref.note != nullptr && !ref.note->isRest() && !ref.note->isDeleted())
          owners.push_back(ref.project);
    }
    return owners;
  }
  return std::vector<Project *>(getInspectorTargets().size(), project);
}

int PianoRollComponent::getInspectorTargetCount() const
{
  return static_cast<int>(getInspectorTargets().size());
}

int PianoRollComponent::getInspectorTargetItemCount() const
{
  // N8 (D-348): "items" are the windows the objective is shown in, the same
  // unit the batch notice uses: three contiguous cuts of one take are three
  // items and one Project.
  if (!documentSelectionAvailable())
    return getInspectorTargets().empty() ? 0 : 1;

  std::vector<juce::String> keys;
  const auto addKey = [&keys](const juce::String &key)
  {
    if (std::find(keys.begin(), keys.end(), key) == keys.end())
      keys.push_back(key);
  };
  if (!documentNoteSelection->isEmpty())
  {
    for (const auto &entry : documentNoteSelection->entries())
      if (entry.note != nullptr && !entry.note->isRest() && !entry.note->isDeleted())
        addKey(entry.regionKey);
  }
  else
  {
    for (const auto &ref : documentNoteIndex->notes)
    {
      if (ref.note == nullptr || ref.note->isRest() || ref.note->isDeleted())
        continue;
      if (ref.regionIndex < 0 || ref.regionIndex >= documentNoteIndex->regionCount())
        continue;
      addKey(documentNoteIndex->regions[static_cast<size_t>(ref.regionIndex)].key);
    }
  }
  return static_cast<int>(keys.size());
}

bool PianoRollComponent::inspectorTargetsAreSelection() const
{
  // N8 (D-348): a non-empty document selection is a selection; the
  // "all visible notes" fallback never is, however many items it reaches.
  if (documentSelectionAvailable())
    return !documentNoteSelection->isEmpty();
  const auto targets = getInspectorTargets();
  return !targets.empty() && targets.front()->isSelected();
}

bool PianoRollComponent::inspectorBatchApplies() const
{
  // N8 (D-348): the objective may be the whole document with nothing selected.
  // Then a gesture needs the batch path as soon as the index has notes outside
  // the active item; with the selection it is the N3 rule (another item).
  if (!documentSelectionAvailable())
    return false;
  if (!documentNoteSelection->isEmpty())
    return documentBatchApplies();
  for (const auto &ref : documentNoteIndex->notes)
    if (ref.project != nullptr && ref.project != project)
      return true;
  return false;
}

float PianoRollComponent::correctionTargetFor(float sourceMidi) const
{
  if (project != nullptr)
    return ScaleUtils::correctionTargetFor(*project, sourceMidi, pitchReferenceHz);
  return ScaleUtils::snapMidiToSemitone(sourceMidi, pitchReferenceHz);
}

float PianoRollComponent::correctionTargetFor(const Project &owner,
                                              float sourceMidi) const
{
  return ScaleUtils::correctionTargetFor(owner, sourceMidi, pitchReferenceHz);
}

const Project &PianoRollComponent::inspectorOwner(size_t index) const
{
  if (!inspectorEditProjects.empty() &&
      index < inspectorEditProjects.size() &&
      inspectorEditProjects[index] != nullptr)
    return *inspectorEditProjects[index];
  return *project;
}

std::optional<float> PianoRollComponent::inspectorValueForTarget(
    InspectorParam param, const Note &note, const Project *owner) const
{
  switch (param)
  {
    case InspectorParam::CorrectPitch:
    {
      // Share of the original deviation from the target pitch that has been
      // removed: 0% = as sung, 100% = exactly on the semitone/scale tone. A
      // pitch outside 0-100% shows 0%, which is where a gesture starts from
      // (beginInspectorEdit re-anchors it), so the slider never jumps.
      // N8 (D-348): the reference is the note's own item; a document target may
      // belong to any item of the track.
      const Project *scaleOwner = owner != nullptr ? owner : project;
      if (scaleOwner == nullptr)
        return 0.0f;
      const auto share = ScaleUtils::correctionShare(
          *scaleOwner, note.getLastNonMacroMidiNote(), note.getMidiNote(),
          pitchReferenceHz);
      return share ? *share * 100.0f : 0.0f;
    }
    case InspectorParam::FlattenCurve:
      return juce::jlimit(0.0f, 100.0f, (1.0f - note.getVibrato()) * 100.0f);
    case InspectorParam::PitchDrift:
      return juce::jlimit(0.0f, 100.0f, (1.0f - note.getPitchDrift()) * 100.0f);
    case InspectorParam::Formant:
      return note.getFormantShift();
    case InspectorParam::Volume:
      return note.getVolumeDb();
    case InspectorParam::Transition:
      // D-225: Transition requires a selection. With nothing selected the
      // slider shows no value (and is disabled); it never falls back to "all
      // segments" like the other sliders.
      if (!note.isSelected())
        return std::nullopt;
      return static_cast<float>(note.getTransitionOutMs());
    case InspectorParam::TiltLeft:
      return note.getTiltFineLeft();
    case InspectorParam::TiltRight:
      return note.getTiltFineRight();
    case InspectorParam::Tilt:
      // D-308: the general layer. The body is the centre and the fines are
      // separate controls, so this no longer folds in a "common" part.
      return note.getTilt();
  }
  return std::nullopt;
}

std::optional<float>
PianoRollComponent::getInspectorValue(InspectorParam param) const
{
  const auto targets = getInspectorTargets();
  if (targets.empty())
    return std::nullopt;
  const auto owners = inspectorTargetProjects();
  const Project *owner = owners.empty() ? project : owners.front();
  return inspectorValueForTarget(param, *targets.front(), owner);
}

bool PianoRollComponent::inspectorValueIsMixed(InspectorParam param) const
{
  // N8 (D-348): the row says "—" when the objective's notes disagree. Without
  // this, the first note of the document order would pass for all of them.
  const auto targets = getInspectorTargets();
  if (targets.size() < 2)
    return false;
  const auto owners = inspectorTargetProjects();
  const Project *firstOwner = owners.empty() ? project : owners.front();
  const auto first = inspectorValueForTarget(param, *targets.front(), firstOwner);
  if (!first)
    return false;
  for (size_t i = 1; i < targets.size(); ++i)
  {
    const Project *owner = i < owners.size() ? owners[i] : project;
    const auto value =
        inspectorValueForTarget(param, *targets[i], owner);
    if (value && std::abs(*value - *first) > 1.0e-4f)
      return true;
  }
  return false;
}

void PianoRollComponent::beginInspectorEdit(InspectorParam param)
{
  if (inspectorEditActive)
    endInspectorEdit();
  // N3: two sliders need the active item itself — Transition joins the note
  // with the next one of its item and Volume is applied to that item's audio on
  // release. Over a selection that reaches other items they are refused with a
  // notice instead of being applied to a part of the selection.
  // N8 (D-348): the objective may be the whole document, so the item-bound
  // refusal follows the same test as the batch routing below.
  if (inspectorBatchApplies() &&
      (param == InspectorParam::Transition || param == InspectorParam::Volume))
  {
    if (onBatchNotice)
      onBatchNotice(TR("notice.batch_item_bound"));
    return;
  }
  // D-225: Transition is the only slider that requires an explicit selection;
  // without one it must not fall back to "every segment" (that would apply a
  // gesture to the whole project).
  if (param == InspectorParam::Transition)
  {
    const bool anySelected =
        project != nullptr &&
        std::any_of(project->getNotes().begin(), project->getNotes().end(),
                    [](const Note &note) { return note.isSelected(); });
    if (!anySelected)
      return;
  }
  inspectorEditProjects.clear();
  inspectorEditNotes.clear();
  inspectorBatchBefore.clear();
  if (inspectorBatchApplies())
  {
    // One gesture over the whole document objective: every note with its own
    // item, in (region, note) order. With no selection that objective is every
    // visible note of the index (D-348), not just the active item.
    for (const auto &target : documentBatchTargets())
      if (target.project != nullptr && target.note != nullptr &&
          !target.note->isRest() && !target.note->isDeleted())
      {
        inspectorEditNotes.push_back(target.note);
        inspectorEditProjects.push_back(target.project);
      }
    // N4: the exact values before the gesture, per item, for the single undo
    // step. Captured before the Correct Pitch reference is re-anchored below.
    std::vector<Project *> owners;
    for (auto *owner : inspectorEditProjects)
      if (std::find(owners.begin(), owners.end(), owner) == owners.end())
        owners.push_back(owner);
    // N5 (D-345): the gesture is refused before it writes when one of its items
    // cannot ask for its render yet: applied mute it would be worse than not
    // applied (D-247).
    if (!batchRenderPathAvailable(owners))
    {
      inspectorEditNotes.clear();
      inspectorEditProjects.clear();
      inspectorBatchBefore.clear();
      if (onBatchNotice)
        onBatchNotice(TR("notice.batch_render_pending"));
      return;
    }
    inspectorBatchBefore = documentNoteBatch::captureStates(owners);
  }
  else
  {
    inspectorEditNotes = getInspectorTargets();
  }
  if (inspectorEditNotes.empty())
    return;
  inspectorEditActive = true;
  inspectorEditParam = param;
  inspectorEditOldParams.clear();
  inspectorEditSourceMidi.clear();
  for (size_t i = 0; i < inspectorEditNotes.size(); ++i)
  {
    auto *note = inspectorEditNotes[i];
    const Project &owner = inspectorOwner(i);
    inspectorEditOldParams.push_back(TransformParams::fromNote(*note));
    // A pitch that is not a 0-100% correction of its reference (older
    // projects, pitch drawing) starts over from where it is, so the slider
    // never jumps. The undo step restores the old reference.
    if (param == InspectorParam::CorrectPitch &&
        !ScaleUtils::correctionShare(owner, note->getLastNonMacroMidiNote(),
                                     note->getMidiNote(), pitchReferenceHz))
      NotePitch::setCorrectionSource(*note, note->getMidiNote());
    inspectorEditSourceMidi.push_back(note->getLastNonMacroMidiNote());
  }
}

void PianoRollComponent::setInspectorEditValue(float value)
{
  if (!inspectorEditActive)
    return;
  const bool batch = !inspectorEditProjects.empty();
  if (!batch && project == nullptr)
    return;

  int minFrame = std::numeric_limits<int>::max();
  int maxFrame = std::numeric_limits<int>::min();
  for (size_t i = 0; i < inspectorEditNotes.size(); ++i)
  {
    auto *note = inspectorEditNotes[i];
    // N3: each note is edited against its own item, so one gesture can span
    // the whole document selection.
    const Project &owner = inspectorOwner(i);
    switch (inspectorEditParam)
    {
      case InspectorParam::CorrectPitch:
      {
        const float source = inspectorEditSourceMidi[i];
        const float target = correctionTargetFor(owner, source);
        NotePitch::applyCorrection(
            *note, target + (source - target) * (1.0f - value / 100.0f));
        break;
      }
      case InspectorParam::FlattenCurve:
        note->setVibrato(1.0f - value / 100.0f);
        break;
      case InspectorParam::PitchDrift:
        note->setPitchDrift(1.0f - value / 100.0f);
        break;
      case InspectorParam::Formant:
        note->setFormantShift(value);
        break;
      case InspectorParam::Volume:
        // Gain is applied to the audio once, on release (AmplitudeAction).
        note->setVolumeDb(value);
        break;
      case InspectorParam::Transition:
        note->setTransitionOutMs(juce::roundToInt(value));
        break;
      case InspectorParam::TiltLeft:
        // D-308: each control writes only its own layer. The left fine moves
        // the edge->centre half; the right half is untouched.
        note->setTiltFineLeft(value);
        break;
      case InspectorParam::TiltRight:
        note->setTiltFineRight(value);
        break;
      case InspectorParam::Tilt:
        // D-308: the general layer (left = centre + value, right = centre -
        // value). The body and the fines stay where they are.
        note->setTilt(value);
        break;
    }
    note->markDirty();
    minFrame = std::min(minFrame, note->getStartFrame());
    maxFrame = std::max(maxFrame, note->getEndFrame());
  }

  if (inspectorEditParam == InspectorParam::Transition)
  {
    // D-225: the transition belongs to the union with the NEXT note. Both
    // sides must be rebuilt from their source contour before the join
    // smoother runs; rebuilding only the left note would leave the right
    // note's old smoothing baked in.
    std::vector<Note*> affected = inspectorEditNotes;
    for (auto* note : inspectorEditNotes)
    {
      if (note == nullptr)
        continue;
      for (auto& candidate : project->getNotes())
      {
        if (&candidate == note || candidate.isRest())
          continue;
        if (candidate.getStartFrame() == note->getEndFrame())
        {
          affected.push_back(&candidate);
          maxFrame = std::max(maxFrame, candidate.getEndFrame());
          break;
        }
      }
    }
    PitchCurveProcessor::rebuildDeltaForNotes(*project, affected);
    project->markNoteEditDirtyRange(minFrame, maxFrame);
    invalidateBasePitchCache();
    if (onPitchEdited)
      onPitchEdited();
  }
  else if (inspectorEditParam != InspectorParam::Volume)
  {
    // D-308: the base no longer depends on the tilt (the body is the centre
    // and the three layers live in the delta), so every pitch preview is a
    // delta rebuild. In a batch every item rebuilds its own notes.
    if (batch)
    {
      std::vector<Project *> owners;
      std::vector<std::vector<Note *>> groups;
      for (size_t i = 0; i < inspectorEditNotes.size(); ++i)
      {
        auto *owner = inspectorEditProjects[i];
        auto found = std::find(owners.begin(), owners.end(), owner);
        if (found == owners.end())
        {
          owners.push_back(owner);
          groups.push_back({});
          found = owners.end() - 1;
        }
        groups[static_cast<size_t>(found - owners.begin())]
            .push_back(inspectorEditNotes[i]);
      }
      for (size_t group = 0; group < owners.size(); ++group)
      {
        int first = std::numeric_limits<int>::max();
        int last = std::numeric_limits<int>::min();
        for (auto *note : groups[group])
        {
          first = std::min(first, note->getStartFrame());
          last = std::max(last, note->getEndFrame());
        }
        PitchCurveProcessor::rebuildDeltaForNotes(*owners[group], groups[group]);
        owners[group]->markNoteEditDirtyRange(first, last);
      }
    }
    else
    {
      PitchCurveProcessor::rebuildDeltaForNotes(*project, inspectorEditNotes);
      project->markNoteEditDirtyRange(minFrame, maxFrame);
    }
    invalidateBasePitchCache();
    if (onPitchEdited)
      onPitchEdited();
  }
  repaint();
}

void PianoRollComponent::endInspectorEdit()
{
  if (!inspectorEditActive)
    return;
  inspectorEditActive = false;
  auto notes = std::move(inspectorEditNotes);
  auto projects = std::move(inspectorEditProjects);
  auto oldParams = std::move(inspectorEditOldParams);
  inspectorEditSourceMidi.clear();
  inspectorEditNotes.clear();
  inspectorEditProjects.clear();
  // N3: a gesture over the document selection carries one item per note; the
  // classic one carries none and everything belongs to the active project.
  const bool batch = !projects.empty();
  if (notes.empty() || (!batch && project == nullptr))
  {
    inspectorBatchBefore.clear();
    return;
  }
  // References as they were before the gesture (beginInspectorEdit may have
  // re-anchored them).
  std::vector<float> oldSource;
  for (const auto &params : oldParams)
    oldSource.push_back(params.correctionSource);

  if (inspectorEditParam == InspectorParam::Volume)
  {
    if (batch)
      return; // refused at begin: it needs the active item
    std::vector<float> before, after;
    for (size_t i = 0; i < notes.size(); ++i)
    {
      before.push_back(oldParams[i].volumeDb);
      after.push_back(notes[i]->getVolumeDb());
    }
    if (before == after)
      return;
    auto action = std::make_unique<AmplitudeAction>(
        *project, notes, before, after, pitchToolController->onAmplitudeEdited,
        "Change Volume");
    action->redo();
    if (undoManager)
      undoManager->addAction(std::move(action));
    return;
  }

  std::vector<TransformParams> newParams;
  std::vector<float> newSource;
  bool changed = false;
  for (size_t i = 0; i < notes.size(); ++i)
  {
    newParams.push_back(TransformParams::fromNote(*notes[i]));
    newSource.push_back(notes[i]->getLastNonMacroMidiNote());
    // A re-anchored reference alone is not an edit.
    auto compared = newParams.back();
    compared.correctionSource = oldParams[i].correctionSource;
    changed |= !(compared == oldParams[i]);
  }
  if (!changed)
  {
    for (size_t i = 0; i < notes.size(); ++i)
      NotePitch::setCorrectionSource(*notes[i], oldParams[i].correctionSource);
    inspectorBatchBefore.clear();
    return;
  }

  std::vector<Project *> owners;
  if (batch)
  {
    for (size_t i = 0; i < notes.size(); ++i)
    {
      auto *owner = projects[i];
      if (std::find(owners.begin(), owners.end(), owner) == owners.end())
        owners.push_back(owner);
      notes[i]->markSynthDirty();
    }
    for (auto *owner : owners)
    {
      std::vector<Note *> group;
      int first = std::numeric_limits<int>::max();
      int last = std::numeric_limits<int>::min();
      for (size_t i = 0; i < notes.size(); ++i)
        if (projects[i] == owner)
        {
          group.push_back(notes[i]);
          first = std::min(first, notes[i]->getStartFrame());
          last = std::max(last, notes[i]->getEndFrame());
        }
      PitchCurveProcessor::rebuildBaseFromNotes(*owner);
      owner->markNoteEditDirtyRange(first, last);
    }
  }
  else
  {
    int minFrame = std::numeric_limits<int>::max();
    int maxFrame = std::numeric_limits<int>::min();
    for (auto *note : notes)
    {
      note->markSynthDirty();
      minFrame = std::min(minFrame, note->getStartFrame());
      maxFrame = std::max(maxFrame, note->getEndFrame());
    }
    PitchCurveProcessor::rebuildBaseFromNotes(*project);
    project->markNoteEditDirtyRange(minFrame, maxFrame);
  }
  invalidateBasePitchCache();

  // Indexed by InspectorParam: every new parameter must get its name here, or
  // the cast below reads past the table and the undo action is built from a
  // garbage pointer (crash on slider release). The static_assert keeps the two
  // lists in sync at compile time.
  static const char *names[] = {"Correct Pitch", "Flatten Curve", "Pitch Drift",
                                "Shift Formant", "Change Volume", "Transition",
                                "Tilt Left", "Tilt Right", "Tilt"};
  static_assert(sizeof(names) / sizeof(names[0]) ==
                    static_cast<size_t>(InspectorParam::Tilt) + 1,
                "undo name table out of sync with InspectorParam");
  // N4 (D-344): a gesture that crossed items is ONE document-level undo step:
  // the record captured at beginInspectorEdit holds the values of every note of
  // every item, so one Ctrl+Z puts all of them back and Ctrl+Y redoes it. The
  // single-item gesture keeps its own InspectorEditAction (same values, one
  // project).
  if (!batch && undoManager)
    undoManager->addAction(std::make_unique<InspectorEditAction>(
        project, notes, oldParams, newParams, oldSource, newSource,
        names[static_cast<int>(inspectorEditParam)]));

  if (batch)
  {
    auto changes = documentNoteBatch::changedSince(inspectorBatchBefore);
    inspectorBatchBefore.clear();
    if (!changes.empty() && undoManager)
    {
      auto safe = juce::Component::SafePointer<PianoRollComponent>(this);
      auto notifyLater = [safe](const std::vector<Project *> &touched)
      {
        if (!safe)
          return;
        safe->notifyBatchProjects(touched);
        safe->repaint();
      };
      undoManager->addAction(
          std::make_unique<documentNoteBatch::DocumentBatchAction>(
              std::move(changes), std::move(notifyLater),
              names[static_cast<int>(inspectorEditParam)]));
    }
    notifyBatchProjects(owners);
    repaint();
    announceBatchApplied(static_cast<int>(notes.size()),
                         documentBatchItemCount(documentBatchTargets()));
    return;
  }

  if (onPitchEdited)
    onPitchEdited();
  if (onPitchEditFinished)
    onPitchEditFinished();
  repaint();
}

void PianoRollComponent::setShowAllSmartControls(bool showAll)
{
  showAllSmartControls = showAll;
  updatePitchToolHandlesFromSelection();
  repaint();
}

Note *PianoRollComponent::findContiguousNeighbour(const Note &note,
                                                  bool next) const
{
  if (project == nullptr)
    return nullptr;

  // Same rule as NoteSplitter::findMergeableNotesAt: never across the
  // boundary between two analysed chunks.
  const auto &chunkRanges = project->getAudioData().segmentChunkRanges;
  const auto chunkIndex = [&chunkRanges](const Note &candidate)
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
  const int noteChunk = chunkIndex(note);
  if (noteChunk < 0)
    return nullptr;

  for (auto &other : project->getNotes())
  {
    if (&other == &note || other.isRest() || chunkIndex(other) != noteChunk)
      continue;
    const bool joins =
        next ? other.getStartFrame() == note.getEndFrame() &&
                   other.getSrcStartFrame() == note.getSrcEndFrame()
             : other.getEndFrame() == note.getStartFrame() &&
                   other.getSrcEndFrame() == note.getSrcStartFrame();
    if (joins)
      return &other;
  }
  return nullptr;
}

bool PianoRollComponent::isEmptyCanvasPoint(juce::Point<float> position)
{
  if (project == nullptr || position.x < pianoKeysWidth || position.y < headerHeight ||
      position.x >= pianoKeysWidth + getVisibleContentWidth() ||
      position.y >= headerHeight + getVisibleContentHeight())
    return false;

  const float worldX = position.x - pianoKeysWidth + static_cast<float>(scrollX);
  const float worldY = position.y - headerHeight + static_cast<float>(scrollY);
  if (findNoteAt(worldX, worldY) != nullptr || findPreviewButtonNoteAt(worldX, worldY))
    return false;
  if (hoveredNote && pitchToolHandles &&
      pitchToolHandles->containsLayoutPoint(worldX, worldY))
    return false;

  // Track mode: segments of the other regions count as content too. While
  // other regions are still analysing no targets come back; keep the old
  // behaviour then (their notes cannot be hit-tested, so do not pan).
  if (trackViewMode && getTrackEditTargets)
  {
    if (!mouseDownTrackTargets)
      mouseDownTrackTargets = getTrackEditTargets();
    if (mouseDownTrackTargets->empty())
      return false;
    for (auto& target : *mouseDownTrackTargets)
    {
      if (target.isValid && !target.isValid())
        continue;
      for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames))
        for (const auto& bounds : trackNoteBounds(*note, *target.project))
          if (bounds.contains(position))
            return false;
    }
  }

  // The waveform band is drawn centred in the canvas, in screen space.
  const float canvasHeight = static_cast<float>(getVisibleContentHeight());
  const float centreY = static_cast<float>(headerHeight) + canvasHeight * 0.5f;
  const float halfBand = canvasHeight * WaveformBackgroundRenderer::bandHeightRatio * 0.5f;
  const float level = waveformBackgroundRenderer
                          ? waveformBackgroundRenderer->envelopeAt(
                                static_cast<double>(worldX) / pixelsPerSecond)
                          : 0.0f;
  constexpr float silenceLevel = 0.01f;
  constexpr float margin = 3.0f;
  const bool onWaveform = level > silenceLevel &&
                          std::abs(position.y - centreY) <= level * halfBand + margin;
  return !onWaveform;
}

void PianoRollComponent::dropTransientNotePointers()
{
  // Last resort: a gesture still holding note pointers here cannot be
  // committed safely any more (the list may already be rebuilt).
  inspectorEditActive = false;
  inspectorEditNotes.clear();
  inspectorEditProjects.clear();
  if (selectHandler_)
    selectHandler_->cancel();  inspectorEditOldParams.clear();
  inspectorEditSourceMidi.clear();
  hoveredNote = nullptr;
  if (pitchToolHandles)
  {
    pitchToolHandles->clear();
    pitchToolHandles->setHoveredHandleIndex(-1);
  }
  hoveredPitchToolHandle = -1;
  updatePreviewButtonBounds();
  repaint();
}

bool PianoRollComponent::handleSmartControlClick(float worldX, float worldY,
                                                 const juce::ModifierKeys &mods)
{
  if (project == nullptr || !pitchToolHandles || pitchToolHandles->isEmpty())
    return false;
  // Cmd/Ctrl-click edits the selection, even over a smart control.
  if (mods.isCommandDown() || mods.isCtrlDown())
    return false;
  const int index = pitchToolHandles->hitTest(worldX, worldY);
  if (index < 0)
    return false;

  const auto handle = pitchToolHandles->getHandle(index);
  if (handle.note == nullptr)
    return false;

  bool changed = false;
  switch (handle.type)
  {
    case PitchToolHandles::HandleType::GlueLeft:
      if (auto *previous = findContiguousNeighbour(*handle.note, false))
        changed = noteSplitter->mergeNotes(previous, handle.note);
      break;
    case PitchToolHandles::HandleType::GlueRight:
      if (auto *next = findContiguousNeighbour(*handle.note, true))
        changed = noteSplitter->mergeNotes(handle.note, next);
      break;
    default:
      return false;
  }

  // Merging rebuilds the note list, so drop pointers into it.
  dropTransientNotePointers();
  juce::ignoreUnused(changed);
  return true;
}

bool PianoRollComponent::selectAdjacentSegment(bool next)
{
  if (project == nullptr)
    return false;

  std::vector<Note *> ordered;
  for (auto &note : project->getNotes())
    if (!note.isRest())
      ordered.push_back(&note);
  if (ordered.empty())
    return false;
  std::sort(ordered.begin(), ordered.end(), [](const Note *a, const Note *b)
            { return a->getStartFrame() < b->getStartFrame(); });

  // Anchor on the last selected segment when moving right, the first when
  // moving left, so repeated presses walk through the take.
  int anchor = -1;
  for (int i = 0; i < static_cast<int>(ordered.size()); ++i)
    if (ordered[static_cast<size_t>(i)]->isSelected())
    {
      if (next || anchor < 0)
        anchor = i;
      if (!next)
        break;
    }

  int target = anchor < 0 ? (next ? 0 : static_cast<int>(ordered.size()) - 1)
                          : anchor + (next ? 1 : -1);
  target = juce::jlimit(0, static_cast<int>(ordered.size()) - 1, target);

  selectOnlyNote(project, ordered[static_cast<size_t>(target)]);
  updatePitchToolHandlesFromSelection();
  if (onNoteSelected)
    onNoteSelected(ordered[static_cast<size_t>(target)]);
  repaint();
  return true;
}

void PianoRollComponent::selectFollowingSamePitch(Note &note)
{
  if (project == nullptr)
    return;
  const int pitch = static_cast<int>(std::round(note.getAdjustedMidiNote()));
  project->deselectAllNotes();
  for (auto &other : project->getNotes())
    if (!other.isRest() && other.getStartFrame() >= note.getStartFrame() &&
        static_cast<int>(std::round(other.getAdjustedMidiNote())) == pitch)
      DocumentNoteSelection::setNoteSelected(*project, other, true);
  updatePitchToolHandlesFromSelection();
  repaint();
}

bool PianoRollComponent::quantizeTimeSelection(int strengthPercent)
{
  // The visible grid is the reference: tempo, beat signature and subdivision
  // already come from the host (updateHostTimelineState).
  const double gridSeconds = getTimelineGridSeconds();

  // N3: with a selection that reaches other items, every item is quantized
  // against the same host grid (its phase comes from the window offset), and
  // the whole operation is refused if any item's plan is unsafe.
  // N4: one Ctrl+Z undoes the whole quantize, neighbours included.
  if (documentBatchApplies())
  {
    if (gridSeconds <= 1.0e-9)
      return false;
    return applyDocumentBatch(
        "Quantize Time",
        [gridSeconds, strengthPercent](
            const std::vector<documentNoteBatch::Target> &targets)
        {
          return documentNoteBatch::quantize(targets, gridSeconds,
                                             strengthPercent);
        });
  }

  if (project == nullptr)
    return false;

  if (gridSeconds <= 1.0e-9)
    return false;

  // An empty selection means "every note", the same convention the harmony
  // macros use.
  auto selected = getSelectedNotes();
  std::unordered_set<const Note *> selectedSet(selected.begin(), selected.end());

  auto &notes = project->getNotes();
  std::vector<timingQuantize::Span> spans;
  spans.reserve(notes.size());
  for (auto &note : notes)
    spans.push_back({note.getStartFrame(), note.getEndFrame(),
                     !note.isRest() && !selectedSet.empty() &&
                         selectedSet.count(&note) > 0});

  if (selectedSet.empty())
    for (size_t i = 0; i < notes.size(); ++i)
      spans[i].selected = !notes[i].isRest();

  const double gridFrames =
      gridSeconds * static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
  std::vector<timingQuantize::Span> quantized;
  if (!timingQuantize::quantizeSpans(spans, gridFrames, strengthPercent,
                                     quantized))
    return false;

  std::vector<NoteTimingState> before;
  std::vector<NoteTimingState> after;
  for (size_t i = 0; i < notes.size(); ++i)
  {
    if (spans[i].startFrame == quantized[i].startFrame &&
        spans[i].endFrame == quantized[i].endFrame)
      continue;
    before.push_back(NoteTimingState::capture(notes[i]));
    after.push_back({&notes[i], quantized[i].startFrame, quantized[i].endFrame});
  }

  if (before.empty())
    return false;

  applyNoteTimingStates(*project, after);
  invalidateBasePitchCache();
  if (undoManager)
    undoManager->addAction(
        std::make_unique<TimingAction>(*project, before, after));
  if (onPitchEditFinished)
    onPitchEditFinished();
  repaint();
  return true;
}

bool PianoRollComponent::setMutedForNotes(const std::vector<Note *> &notes,
                                          bool muted)
{
  // N3: when the caller passed exactly the active item's part of a document
  // selection, the action is the whole selection, each note in its own item.
  // With a single clicked note that is not in the selection, nothing changes:
  // it stays the one-note action it always was.
  if (documentBatchApplies())
  {
    const auto activeSelected = getSelectedNotes();
    const bool callerMeantSelection =
        notes.size() == activeSelected.size() &&
        std::all_of(notes.begin(), notes.end(),
                    [&activeSelected](const Note *note)
                    {
                      return std::find(activeSelected.begin(),
                                       activeSelected.end(),
                                       note) != activeSelected.end();
                    });
    if (callerMeantSelection)
      return applyDocumentBatch(
          muted ? "Mute Notes" : "Unmute Notes",
          [muted](const std::vector<documentNoteBatch::Target> &targets)
          { return documentNoteBatch::setMuted(targets, muted); });
  }

  if (project == nullptr || notes.empty())
    return false;

  std::vector<NoteMuteAction::Entry> entries;
  entries.reserve(notes.size());
  int dirtyStart = notes.front()->getStartFrame();
  int dirtyEnd = notes.front()->getEndFrame();
  for (auto *note : notes)
  {
    if (note == nullptr || note->isMuted() == muted)
      continue;
    entries.push_back({note, note->isMuted(), muted});
    note->setMuted(muted);
    note->markDirty();
    note->markSynthDirty();
    dirtyStart = std::min(dirtyStart, note->getStartFrame());
    dirtyEnd = std::max(dirtyEnd, note->getEndFrame());
  }
  if (entries.empty())
    return false;

  const int frameCount = project->getAudioData().getNumFrames();
  project->setF0DirtyRange(std::max(0, dirtyStart - 24),
                           std::min(frameCount, dirtyEnd + 24));
  project->setModified(true);

  if (undoManager)
    undoManager->addAction(
        std::make_unique<NoteMuteAction>(*project, std::move(entries)));
  if (onPitchEditFinished)
    onPitchEditFinished();
  repaint();
  return true;
}

bool PianoRollComponent::deleteSelection()
{
  // N3: one delete over the document selection, each note in its own item.
  // N4: the tombstones of every item go back with one Ctrl+Z.
  if (documentBatchApplies())
    return applyDocumentBatch(
        "Delete Notes",
        [](const std::vector<documentNoteBatch::Target> &targets)
        { return documentNoteBatch::deleteNotes(targets); });

  if (project == nullptr)
    return false;

  auto selected = getSelectedNotes();
  if (selected.empty())
    return false;

  std::vector<NoteDeleteAction::Entry> entries;
  entries.reserve(selected.size());
  int dirtyStart = -1;
  int dirtyEnd = -1;
  for (auto *note : selected)
  {
    if (note == nullptr || note->isRest() || note->isDeleted())
      continue;
    NoteDeleteAction::Entry entry{note, note->isDeleted(), note->isRest(),
                                  note->isSelected(), /*afterDeleted=*/true,
                                  /*afterRest=*/true, /*afterSelected=*/false};
    entries.push_back(entry);
    note->setDeleted(true);
    note->setRest(true);
    DocumentNoteSelection::setNoteSelected(*project, *note, false);
    note->markDirty();
    note->markSynthDirty();
    dirtyStart = dirtyStart < 0 ? note->getStartFrame()
                               : std::min(dirtyStart, note->getStartFrame());
    dirtyEnd = dirtyEnd < 0 ? note->getEndFrame()
                            : std::max(dirtyEnd, note->getEndFrame());
  }
  if (entries.empty())
    return false;

  if (dirtyEnd > dirtyStart)
  {
    const int frameCount = project->getAudioData().getNumFrames();
    project->setF0DirtyRange(std::max(0, dirtyStart - 24),
                             std::min(frameCount, dirtyEnd + 24));
  }
  project->setModified(true);

  if (undoManager)
    undoManager->addAction(
        std::make_unique<NoteDeleteAction>(*project, std::move(entries)));
  if (onPitchEditFinished)
    onPitchEditFinished();
  repaint();
  return true;
}

bool PianoRollComponent::muteSelection()
{
  // N3: the whole document selection, toggled on its own notes.
  if (documentBatchApplies())
  {
    bool allMuted = true;
    for (const auto &target : documentBatchTargets())
      if (target.note != nullptr)
        allMuted = allMuted && target.note->isMuted();
    const bool mute = !allMuted;
    return applyDocumentBatch(
        mute ? "Mute Notes" : "Unmute Notes",
        [mute](const std::vector<documentNoteBatch::Target> &targets)
        { return documentNoteBatch::setMuted(targets, mute); });
  }

  if (project == nullptr)
    return false;

  auto selected = getSelectedNotes();
  if (selected.empty())
    return false;

  bool allMuted = true;
  for (const auto *note : selected)
    allMuted = allMuted && note->isMuted();
  return setMutedForNotes(selected, ! allMuted);
}

HarmonyUtils::Result PianoRollComponent::applyHarmonyInterval(
    HarmonyUtils::Interval interval, bool up)
{
  if (project == nullptr)
    return HarmonyUtils::Result::NoNotes;

  // N3: harmony is not a batch operation yet, and its "empty selection means
  // every note" fallback must never fire when the selection is in other items:
  // it would transpose the whole active item instead.
  if (documentBatchApplies())
  {
    if (onBatchNotice)
      onBatchNotice(TR("notice.batch_item_bound"));
    return HarmonyUtils::Result::NoNotes;
  }

  // Harmony follows the project key even when drag-snap is set to chromatic.
  const ScaleMode keyMode = project->getPreferredScaleMode();
  const int keyRoot = project->getScaleRootNote();
  if (interval != HarmonyUtils::Interval::Octave &&
      !HarmonyUtils::supportsDiatonicIntervals(keyMode, keyRoot))
    return HarmonyUtils::Result::ScaleUnsupported;

  // One click should turn a duplicated lead track into a harmony line, so an
  // empty selection means "every note".
  auto targets = project->getSelectedNotes();
  if (targets.empty())
    for (auto &note : project->getNotes())
      targets.push_back(&note);

  const int referenceHz = pitchReferenceHz;
  int outOfRangeCount = 0;
  const bool moved = shiftNotesBySemitones(
      targets,
      [interval, up, keyMode, keyRoot, referenceHz,
       &outOfRangeCount](const Note &note) -> std::optional<float>
      {
        const float adjustedMidi = note.getAdjustedMidiNote();
        const auto shift = HarmonyUtils::intervalShiftSemitones(
            adjustedMidi, interval, up, keyMode, keyRoot, referenceHz);
        if (!shift)
          return std::nullopt;

        // Clamping at the editor's range would land on an arbitrary,
        // possibly out-of-key pitch, so such notes stay where they are.
        const float target = adjustedMidi + static_cast<float>(*shift);
        if (target < static_cast<float>(MIN_MIDI_NOTE) ||
            target > static_cast<float>(MAX_MIDI_NOTE))
        {
          ++outOfRangeCount;
          return std::nullopt;
        }
        return static_cast<float>(*shift);
      });

  if (outOfRangeCount > 0)
    return moved ? HarmonyUtils::Result::PartlyOutOfRange
                 : HarmonyUtils::Result::OutOfRange;
  return moved ? HarmonyUtils::Result::Applied : HarmonyUtils::Result::NoNotes;
}

bool PianoRollComponent::shiftNotesBySemitones(
    const std::vector<Note *> &targets,
    const std::function<std::optional<float>(const Note &)> &shiftForNote)
{
  if (project == nullptr || targets.empty())
    return false;

  const auto &selectedNotes = targets;

  constexpr float minMidi = static_cast<float>(MIN_MIDI_NOTE);
  constexpr float maxMidi = static_cast<float>(MAX_MIDI_NOTE);

  std::vector<Note *> notesToMove;
  std::vector<float> oldMidis;
  std::vector<float> newMidis;
  // Harmony and arrow-key moves keep the Correct Pitch reference (see
  // NotePitch).
  std::vector<float> oldSources;
  std::vector<float> newSources;
  notesToMove.reserve(selectedNotes.size());
  oldMidis.reserve(selectedNotes.size());
  newMidis.reserve(selectedNotes.size());

  int dirtyStartFrame = std::numeric_limits<int>::max();
  int dirtyEndFrame = std::numeric_limits<int>::min();

  for (auto *note : selectedNotes)
  {
    if (!note || note->isRest())
      continue;

    const auto semitoneDelta = shiftForNote(*note);
    if (!semitoneDelta)
      continue;

    // N3: the clamp and the carried reference are NotePitch::editTarget's, the
    // same function the document-level batch uses, so a selection gives the
    // same values whether it is edited one item at a time or all at once.
    const auto edit = NotePitch::editTarget(*project, *note, *semitoneDelta,
                                            minMidi, maxMidi);
    if (!edit)
      continue;

    notesToMove.push_back(note);
    oldMidis.push_back(note->getMidiNote());
    newMidis.push_back(edit->midi);
    oldSources.push_back(note->getLastNonMacroMidiNote());
    newSources.push_back(edit->source);
    dirtyStartFrame = std::min(dirtyStartFrame, note->getStartFrame());
    dirtyEndFrame = std::max(dirtyEndFrame, note->getEndFrame());
  }

  if (notesToMove.empty())
    return false;

  auto rebuildAndNotify =
      [this, dirtyStartFrame, dirtyEndFrame](const std::vector<Note *> &)
  {
    if (project == nullptr)
      return;

    PitchCurveProcessor::rebuildBaseFromNotes(*project);
    invalidateBasePitchCache();

    project->markNoteEditDirtyRange(dirtyStartFrame, dirtyEndFrame);

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();

    repaint();
  };

  if (undoManager)
  {
    auto *projectPtr = project;
    auto action = std::make_unique<MultiNoteMidiNudgeAction>(
        notesToMove, oldMidis, newMidis,
        [projectPtr, dirtyStartFrame,
         dirtyEndFrame](const std::vector<Note *> &notes)
        {
          if (!projectPtr)
            return;
          PitchCurveProcessor::rebuildBaseFromNotes(*projectPtr);
          for (auto *note : notes)
            if (note)
              note->markSynthDirty();
          projectPtr->markNoteEditDirtyRange(dirtyStartFrame, dirtyEndFrame);
        },
        oldSources, newSources);
    undoManager->addAction(std::move(action));
  }

  for (size_t i = 0; i < notesToMove.size(); ++i)
  {
    NotePitch::restore(*notesToMove[i], newMidis[i], newSources[i]);
    notesToMove[i]->markDirty();
    notesToMove[i]->markSynthDirty();
  }

  rebuildAndNotify(notesToMove);
  return true;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress &key)
{
  const auto mods = key.getModifiers();
  if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
    return false;

  const int keyCode = key.getKeyCode();
  if (keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey)
  {
    // VariAudio: Up/Down move by a semitone; Shift moves in cent steps and
    // ignores the pitch snap mode.
    const int direction = keyCode == juce::KeyPress::upKey ? 1 : -1;
    if (mods.isShiftDown())
      return nudgeSelectedNotesByCents(direction);
    if (project != nullptr &&
        resolveDragSnapKind({}) == PitchEditor::DragSnapKind::Absolute)
      return nudgeSelectedNotesToNextPitch(direction);
    return nudgeSelectedNotesBySemitones(direction);
  }
  if (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey)
    return selectAdjacentSegment(keyCode == juce::KeyPress::rightKey);

  return false;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress &key,
                                    juce::Component *)
{
  return keyPressed(key);
}

void PianoRollComponent::focusLost(FocusChangeType cause)
{
  juce::ignoreUnused(cause);
  // Don't automatically re-grab focus - let the host manage focus normally
  // Focus will be re-acquired when user clicks on the piano roll
}

void PianoRollComponent::focusGained(FocusChangeType cause)
{
  juce::ignoreUnused(cause);
  // Focus gained - nothing special needed
}

void PianoRollComponent::setTimelineDisplayOffset(double seconds)
{
  if (!coordMapper ||
      juce::approximatelyEqual(coordMapper->getTimelineDisplayOffset(), seconds))
    return;

  const double previous = coordMapper->getTimelineDisplayOffset();
  coordMapper->setTimelineDisplayOffset(seconds);

  if (trackViewMode)
  {
    // The canvas is in the active modification's time, so a new offset (a
    // different region became active, or it moved) shifts where the host
    // timeline sits on it. Move the scroll with it so the track itself stays
    // still on screen.
    scrollX -= (seconds - previous) * pixelsPerSecond;
    coordMapper->setViewStartSeconds(getTimelineStart());
    coordMapper->setScrollX(scrollX);
    updateScrollBars();
  }

  // Region previews and the Track-mode clip are placed through the offset.
  waveformBackgroundRenderer->setProjectDrawRange(getTrackModeActiveSpan());
  invalidateWaveformCache();
  updatePreviewButtonBounds();
  repaint();
}

double PianoRollComponent::getHostScrollSeconds() const
{
  return projectToTimeline(scrollX / std::max(1.0e-6, (double)pixelsPerSecond));
}

double PianoRollComponent::projectToTimeline(double projectSeconds) const
{
  return coordMapper ? coordMapper->projectToTimeline(projectSeconds)
                     : projectSeconds;
}

double PianoRollComponent::timelineToProject(double timelineSeconds) const
{
  return coordMapper ? coordMapper->timelineToProject(timelineSeconds)
                     : timelineSeconds;
}

void PianoRollComponent::setCursorTime(double time)
{
  if (std::abs(cursorTime - time) < 0.0001)
    return; // Skip if no change

  // Calculate dirty rectangle for cursor position
  // Include full height so the old and new cursor lines are fully repainted.
  auto getCursorRect = [this](double t) -> juce::Rectangle<int>
  {
    float x =
        static_cast<float>(t * pixelsPerSecond - scrollX) + pianoKeysWidth;
    constexpr int cursorHalfWidth = 2;
    int rectX = static_cast<int>(x) - cursorHalfWidth;
    int rectWidth = cursorHalfWidth * 2 + 1;
    return juce::Rectangle<int>(rectX, 0, rectWidth, getHeight());
  };

  const auto oldCursorRect = getCursorRect(cursorTime);
  cursorTime = time;
  const auto cursorDirtyArea =
      oldCursorRect.getUnion(getCursorRect(cursorTime))
          .getIntersection(getLocalBounds());

  // Erase the old playhead and draw the new one in a single paint operation.
  // Separate invalidations can be presented independently when the positions
  // no longer overlap at high horizontal zoom, producing a visible blink.
  if (!cursorDirtyArea.isEmpty())
    repaint(cursorDirtyArea);

  if (onCursorMoved)
    onCursorMoved();
}

void PianoRollComponent::setPlaybackActive(bool active)
{
  if (playbackActive == active)
    return;

  playbackActive = active;
  repaint();
}

void PianoRollComponent::setPixelsPerSecond(float pps, bool centerOnCursor)
{
  float oldPps = pixelsPerSecond;
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineLength();
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime))
          : MIN_PIXELS_PER_SECOND;
  float newPps =
      juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, pps);

  if (std::abs(oldPps - newPps) < 0.01f)
    return; // No significant change

  if (centerOnCursor)
  {
    // Calculate cursor position relative to view
    float cursorX = static_cast<float>(cursorTime * oldPps);
    float cursorRelativeX = cursorX - static_cast<float>(scrollX);

    // Calculate new scroll position to keep cursor at same relative position
    float newCursorX = static_cast<float>(cursorTime * newPps);
    pixelsPerSecond = newPps;
    coordMapper->setPixelsPerSecond(newPps);
    scrollX = clampScrollX(static_cast<double>(newCursorX - cursorRelativeX));
    coordMapper->setScrollX(scrollX);
  }

  pixelsPerSecond = newPps;
  coordMapper->setPixelsPerSecond(newPps);
  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();

  // Don't call onZoomChanged here to avoid infinite recursion
  // The caller is responsible for synchronizing other components
}

void PianoRollComponent::setPixelsPerSemitone(float pps, float anchorContentY)
{
  const float oldPps = pixelsPerSemitone;
  const int visibleHeight = getVisibleContentHeight();
  const float minPpsForFill =
      visibleHeight > 0
          ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
          : MIN_PIXELS_PER_SEMITONE;
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);

  const float newPps = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, pps);
  if (std::abs(oldPps - newPps) < 0.01f)
    return;

  float effectiveAnchorY = anchorContentY;
  if (effectiveAnchorY < 0.0f)
    effectiveAnchorY = static_cast<float>(visibleHeight) * 0.5f;
  effectiveAnchorY = juce::jlimit(0.0f, static_cast<float>(visibleHeight),
                                  effectiveAnchorY);

  const float midiAtAnchor =
      MAX_MIDI_NOTE -
      (effectiveAnchorY + static_cast<float>(scrollY)) / oldPps;

  pixelsPerSemitone = newPps;
  coordMapper->setPixelsPerSemitone(pixelsPerSemitone);

  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY =
      std::max(0.0, totalHeight - static_cast<double>(visibleHeight));
  const double anchoredScrollY =
      (MAX_MIDI_NOTE - midiAtAnchor) * pixelsPerSemitone - effectiveAnchorY;
  scrollY = juce::jlimit(0.0, maxScrollY, anchoredScrollY);
  coordMapper->setScrollY(scrollY);

  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setScrollX(double x)
{
  const double clampedX = clampScrollX(x);

  if (std::abs(scrollX - clampedX) < 0.01)
    return; // No significant change

  scrollX = clampedX;
  coordMapper->setScrollX(clampedX);
  horizontalScrollBar.setCurrentRangeStart(clampedX);

  // Don't call onScrollChanged here to avoid infinite recursion
  // The caller is responsible for synchronizing other components

  repaint();
  updatePreviewButtonBounds();
}

void PianoRollComponent::setScrollY(double y)
{
  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY = std::max(
      0.0, totalHeight - static_cast<double>(getVisibleContentHeight()));
  const double clampedY = juce::jlimit(0.0, maxScrollY, y);

  if (std::abs(scrollY - clampedY) < 0.01)
    return;

  scrollY = clampedY;
  coordMapper->setScrollY(clampedY);
  verticalScrollBar.setCurrentRangeStart(clampedY);
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::centerOnPitchRange(float minMidi, float maxMidi)
{
  // Calculate center MIDI note
  float centerMidi = (minMidi + maxMidi) / 2.0f;

  // Calculate Y position for center
  float centerY = midiToY(centerMidi);

  // Get visible height
  int visibleHeight = getHeight() - 8; // scrollbar height

  // Calculate scroll position to center the pitch range
  double newScrollY = centerY - visibleHeight / 2.0;

  // Clamp to valid range
  double totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  newScrollY =
      juce::jlimit(0.0, std::max(0.0, totalHeight - visibleHeight), newScrollY);

  scrollY = newScrollY;
  coordMapper->setScrollY(newScrollY);
  verticalScrollBar.setCurrentRangeStart(newScrollY);
  updatePreviewButtonBounds();
  repaint();
}

bool PianoRollComponent::centerOnCurrentPitchRange()
{
  if (project == nullptr)
    return false;

  float minMidi = std::numeric_limits<float>::max();
  float maxMidi = std::numeric_limits<float>::lowest();
  bool hasNotePitch = false;

  for (const auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;

    const float midi = note.getAdjustedMidiNote();
    minMidi = std::min(minMidi, midi);
    maxMidi = std::max(maxMidi, midi);
    hasNotePitch = true;

    for (const float delta : note.getActiveDeltaPitch())
    {
      minMidi = std::min(minMidi, midi + delta);
      maxMidi = std::max(maxMidi, midi + delta);
    }
  }

  if (!hasNotePitch)
    return false;

  minMidi -= 1.0f;
  maxMidi += 1.0f;
  if (maxMidi - minMidi < 2.0f)
  {
    const float centerMidi = (minMidi + maxMidi) * 0.5f;
    minMidi = centerMidi - 1.0f;
    maxMidi = centerMidi + 1.0f;
  }

  centerOnPitchRange(minMidi, maxMidi);
  return true;
}

void PianoRollComponent::fitPitchRangeToView(float minMidi, float maxMidi)
{
  if (!std::isfinite(minMidi) || !std::isfinite(maxMidi))
    return;

  if (minMidi > maxMidi)
    std::swap(minMidi, maxMidi);

  minMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                         static_cast<float>(MAX_MIDI_NOTE), minMidi);
  maxMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                         static_cast<float>(MAX_MIDI_NOTE), maxMidi);

  const int visibleHeight = getVisibleContentHeight();
  if (visibleHeight <= 0)
    return;

  const float rangeSemitones = std::max(1.0f, maxMidi - minMidi + 1.0f);
  const float minPpsForFill =
      static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1);
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
  pixelsPerSemitone = juce::jlimit(
      minPps, MAX_PIXELS_PER_SEMITONE,
      static_cast<float>(visibleHeight) / rangeSemitones);
  coordMapper->setPixelsPerSemitone(pixelsPerSemitone);

  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY =
      std::max(0.0, totalHeight - static_cast<double>(visibleHeight));
  const double lowestNoteBottomY =
      (static_cast<double>(MAX_MIDI_NOTE) - static_cast<double>(minMidi) +
       1.0) *
      static_cast<double>(pixelsPerSemitone);

  scrollY = juce::jlimit(
      0.0, maxScrollY,
      lowestNoteBottomY - static_cast<double>(visibleHeight));
  coordMapper->setScrollY(scrollY);

  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setEditMode(EditMode mode)
{
  const EditMode previousMode = editMode;
  if (previousMode == EditMode::PitchDrawing && mode != EditMode::PitchDrawing &&
      pitchDrawingHandler_)
  {
    pitchDrawingHandler_->cancel();
  }
  if (previousMode == EditMode::Timing && mode != EditMode::Timing &&
      timingHandler_)
    timingHandler_->cancel();
  editMode = mode;

  if (mode == EditMode::PitchDrawing)
    setHoveredNote(nullptr);

  // Clear split guide when leaving split mode
  if (mode != EditMode::Split && splitHandler_)
  {
    splitHandler_->clearGuide();
  }

  updateMouseCursorForEditMode();

  // Update currentHandler_ based on the new mode
  switch (mode)
  {
  case EditMode::Select:
    currentHandler_ = selectHandler_.get();
    break;
  case EditMode::Split:
    currentHandler_ = splitHandler_.get();
    break;
  case EditMode::PitchDrawing:
    currentHandler_ = pitchDrawingHandler_.get();
    break;
  case EditMode::Timing:
    currentHandler_ = timingHandler_.get();
    break;
  }

  if (mode != EditMode::Select)
  {
    hoveredPitchToolHandle = -1;
    if (pitchToolHandles)
      pitchToolHandles->setHoveredHandleIndex(-1);
  }
  updatePitchToolHandlesFromSelection();
  updatePitchDrawingConfirmationPopup();

  repaint();
}

void PianoRollComponent::updateMouseCursorForEditMode()
{
  if (editMode == EditMode::Split)
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::PitchDrawing)
  {
    const auto pointer = getMouseXYRelative();
    const bool overCanvas = pointer.x >= pianoKeysWidth &&
                            pointer.y >= headerHeight &&
                            pointer.x < pianoKeysWidth + getVisibleContentWidth() &&
                            pointer.y < headerHeight + getVisibleContentHeight();
    const float worldX = pointer.x - pianoKeysWidth +
                         static_cast<float>(scrollX);
    if (overCanvas && pitchDrawingHandler_ &&
        pitchDrawingHandler_->isPitchRegionAtWorldX(worldX))
      setMouseCursor(pitchDrawingHandler_->isHoveringAnchor()
                         ? juce::MouseCursor::DraggingHandCursor
                         : juce::MouseCursor::CrosshairCursor);
    else
      setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::Timing)
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
}

std::vector<Note *> PianoRollComponent::getSelectedNotes() const
{
  if (!project)
    return {};

  std::vector<Note *> selected;
  for (auto &note : project->getNotes())
  {
    if (note.isDeleted())
      continue;
    if (note.isSelected())
      selected.push_back(&note);
  }
  return selected;
}

bool PianoRollComponent::hasDocumentSelection() const
{
  return documentSelectionAvailable() && !documentNoteSelection->isEmpty();
}

bool PianoRollComponent::documentBatchApplies() const
{
  // N8 (D-348): the objective is the document selection, read from its entries
  // (each points at a real Note of its own item), never from the project a
  // hover left active. One entry in another item is enough for the batch path:
  // everything inside the active item keeps its own single-item path (and undo).
  if (!hasDocumentSelection())
    return false;
  for (const auto &entry : documentNoteSelection->entries())
    if (entry.project != nullptr && entry.project != project)
      return true;
  return false;
}

std::vector<documentNoteBatch::Target>
PianoRollComponent::documentBatchTargets() const
{
  if (!documentSelectionAvailable())
    return {};
  if (!documentNoteSelection->isEmpty())
    return documentNoteBatch::targetsOf(*documentNoteSelection, *documentNoteIndex);
  // N8 (D-348): no selection in track mode means "all segments", and the
  // objective is every visible note of the document. A temporary set is only a
  // list of pointers: no note and no edit is copied.
  DocumentNoteSelection allVisible;
  allVisible.selectAll(*documentNoteIndex);
  return documentNoteBatch::targetsOf(allVisible, *documentNoteIndex);
}

void PianoRollComponent::notifyBatchProjects(
    const std::vector<Project *> &projects)
{
  bool activeTouched = false;
  for (auto *owner : projects)
  {
    if (owner == nullptr)
      continue;
    if (owner == project)
      activeTouched = true;
  }
  if (activeTouched)
  {
    invalidateBasePitchCache();
    if (onPitchEdited)
      onPitchEdited();
  }
  // N5 (D-345): the render is not asked for per window nor per note: the items
  // that changed go into the gesture's queue, deduplicated by Project, and the
  // flush sends one request for each (D-247: whoever dirties the audio requests
  // its render). Three cuts of one take are one request, not three.
  queueBatchRenders(projects);
  // A delete cleared the flags of its notes: the set must drop them.
  documentNoteSelection->refresh(documentNoteIndex.get());
  applyDocumentSelection();
  flushBatchRenders();
}

std::vector<TrackPitchMacroTarget> PianoRollComponent::batchRenderTargets() const
{
  if (!getTrackEditTargets)
    return {};
  return getTrackEditTargets();
}

bool PianoRollComponent::batchRenderPathAvailable(
    const std::vector<Project *> &projects) const
{
  if (projects.empty())
    return true;
  // No backend target provider at all: this editor publishes no track audio (it
  // is not the plug-in in track mode), so an item of the document cannot be left
  // mute and there is nothing to request.
  if (!getTrackEditTargets)
    return true;
  const auto targets = batchRenderTargets();
  for (auto *owner : projects)
  {
    if (owner == nullptr)
      continue;
    // The active item renders through the editor itself (markRenderDirty and the
    // canvas request), which is the path a single-item edit already uses.
    if (owner == project && onPitchEditFinished)
      continue;
    const auto target = std::find_if(
        targets.begin(), targets.end(),
        [owner](const TrackPitchMacroTarget &candidate)
        { return candidate.project == owner; });
    const bool usable = target != targets.end() &&
                        !(target->isValid && !target->isValid()) &&
                        bool(target->onChanged);
    if (!usable)
      return false;
  }
  return true;
}

void PianoRollComponent::queueBatchRenders(
    const std::vector<Project *> &projects)
{
  for (auto *owner : projects)
    if (owner != nullptr)
      pendingBatchRenders.add(owner, owner == project);
}

bool PianoRollComponent::requestBatchRender(
    const documentRenderBatch::Request &request,
    const std::vector<TrackPitchMacroTarget> &targets)
{
  auto *owner = request.project;
  if (owner == nullptr)
    return false;
  // The active item: the editor's own path (D-249), the same one a single-item
  // edit uses. It marks the dirty range and schedules the render: one request.
  if (request.active && onPitchEditFinished)
  {
    onPitchEditFinished();
    return true;
  }
  // No provider: nothing publishes this item's audio, so there is no request to
  // make and no mute to prevent (the editor is not the ARA track).
  if (!getTrackEditTargets)
    return true;
  for (const auto &target : targets)
    if (target.project == owner)
    {
      if (target.isValid && !target.isValid())
        return false;
      if (!target.onChanged)
        return false;
      // The backend target persists the item's archive and asks the backend for
      // the render of this modification (the same path the track drag uses).
      target.onChanged(true);
      return true;
    }
  return false;
}

void PianoRollComponent::flushBatchRenders()
{
  if (pendingBatchRenders.empty())
    return;
  const auto requests = pendingBatchRenders.take();
  // Once per flush, not once per item: building the targets walks the host's
  // regions and can trigger an analysis request.
  const auto targets = batchRenderTargets();
  const auto outcome = documentRenderBatch::flush(
      requests,
      [this, &targets](const documentRenderBatch::Request &request)
      { return requestBatchRender(request, targets); });
  if (!outcome.complete() && onBatchNotice)
    // A dirty item without a request is heard mute (D-247). It should not
    // happen — the paths that write check this before writing — but saying it
    // is better than leaving the user with an item that never updates.
    onBatchNotice(TR("notice.batch_render_pending"));
}

bool PianoRollComponent::applyDocumentBatch(
    const juce::String &undoName,
    const std::function<documentNoteBatch::Result(
        const std::vector<documentNoteBatch::Target> &)> &operation)
{
  const auto targets = documentBatchTargets();
  // N5 (D-345): nothing is written until every item of the batch has a path to
  // its render. An edit applied without it would be heard mute or stale (D-247),
  // and undoing it afterwards is not what the user asked for.
  if (!batchRenderPathAvailable(documentNoteBatch::projectsOf(targets)))
  {
    if (onBatchNotice)
      onBatchNotice(TR("notice.batch_render_pending"));
    return false;
  }
  // N4: the record has to exist before the operation writes. It covers every
  // note of every item the selection reaches, because a quantize also moves the
  // shared boundaries of unselected neighbours.
  const auto before =
      documentNoteBatch::captureStates(documentNoteBatch::projectsOf(targets));

  const auto result = operation(targets);
  if (result.rejected())
  {
    if (onBatchNotice)
      onBatchNotice(TR("notice.batch_rejected"));
    return false;
  }
  if (!result.applied())
    return false;

  auto changes = documentNoteBatch::changedSince(before);
  if (!changes.empty() && undoManager)
  {
    auto safe = juce::Component::SafePointer<PianoRollComponent>(this);
    auto notify = [safe](const std::vector<Project *> &projects)
    {
      if (!safe)
        return;
      safe->notifyBatchProjects(projects);
      safe->repaint();
    };
    undoManager->addAction(
        std::make_unique<documentNoteBatch::DocumentBatchAction>(
            std::move(changes), std::move(notify), undoName));
  }

  // The items are counted before the operation: a delete clears the flags of
  // its notes and the set drops them, so afterwards there would be nothing left
  // to count and the user would get no message for the one edit that most needs
  // one.
  const int items = documentBatchItemCount(targets);
  notifyBatchProjects(result.projects);
  repaint();
  // The UI says what happened: one step, this many notes of these many items.
  // The item count is the windows the notes are shown in, not the items' count
  // of Project objects: three contiguous cuts of one take are three items of
  // the track and one project, and the user sees three items.
  announceBatchApplied(result.notes, items);
  return true;
}

int PianoRollComponent::documentBatchItemCount(
    const std::vector<documentNoteBatch::Target> &targets) const
{
  std::vector<juce::String> keys;
  for (const auto &target : targets)
    if (target.note != nullptr &&
        std::find(keys.begin(), keys.end(), target.regionKey) == keys.end())
      keys.push_back(target.regionKey);
  return static_cast<int>(keys.size());
}

void PianoRollComponent::announceBatchApplied(int notes, int items)
{
  if (!onBatchNotice || notes <= 0 || items <= 0)
    return;
  onBatchNotice(TR("notice.batch_applied")
                    .replace("{n}", juce::String(notes))
                    .replace("{items}", juce::String(items)));
}

void PianoRollComponent::updatePitchToolHandlesFromSelection()
{
  // The per-note smart-control cluster was retired (2026-11-22): its
  // parameters now live in the left inspector, so hovering a note no longer
  // draws a second floating tool menu around it. The class stays for the
  // layout tests and for the future floating tools section.
  if (!pitchToolHandles)
    return;
  pitchToolHandles->clear();
  hoveredPitchToolHandle = -1;
  pitchToolHandles->setHoveredHandleIndex(-1);
}

Note *PianoRollComponent::findNoteAt(float x, float y)
{
  if (!project)
    return nullptr;

  for (auto &note : project->getNotes())
  {
    // Skip rest notes
    if (note.isRest())
      continue;

    float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
    float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
    float noteY = midiToY(note.getAdjustedMidiNote());
    float noteH = pixelsPerSemitone;

    if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH)
    {
      return &note;
    }
  }

  return nullptr;
}

juce::Rectangle<float>
PianoRollComponent::getNoteHoverShadowBounds(const Note &note) const
{
  if (!project)
    return {};

  const auto &audioData = project->getAudioData();
  const float noteStartTime = static_cast<float>(framesToSeconds(note.getStartFrame()));
  const float noteEndTime = static_cast<float>(framesToSeconds(note.getEndFrame()));
  const float x = noteStartTime * pixelsPerSecond;
  const float w = (noteEndTime - noteStartTime) * pixelsPerSecond;
  const float renderedWidth = std::max(w, 4.0f);
  const float h = pixelsPerSemitone;
  const float y = midiToY(note.getAdjustedMidiNote());

  // Segment style: the highlight hugs the segment rectangle, like VariAudio.
  if (segmentStyle)
    return juce::Rectangle<float>(x, y, renderedWidth, h).expanded(4.0f, 4.0f);

  juce::Rectangle<float> shadowBounds(x, y, renderedWidth, h);
  const float *samples = nullptr;
  int totalSamples = 0;
  int startSample = 0;
  int endSample = 0;
  if (audioData.waveform.getNumSamples() > 0)
  {
    samples = audioData.waveform.getReadPointer(0);
    totalSamples = audioData.waveform.getNumSamples();
    startSample = static_cast<int>(noteStartTime * audioData.sampleRate);
    endSample = static_cast<int>(noteEndTime * audioData.sampleRate);
    startSample = std::max(0, std::min(startSample, totalSamples - 1));
    endSample = std::max(startSample + 1, std::min(endSample, totalSamples));
  }

  if (samples && totalSamples > 0 && w > 2.0f && endSample > startSample)
  {
    const int pointCount =
        std::max(2, std::min(512, static_cast<int>(std::ceil(w)) + 1));
    const auto envelope = VisualWaveformEnvelope::build(
        samples, totalSamples, startSample, endSample, pointCount,
        renderedWidth, audioData.sampleRate, pixelsPerSecond, true,
        pitchToolController ? pitchToolController->getAmplitudePreviewGain(note) : 1.0f);
    const float maxSample =
        envelope.empty()
            ? 0.0f
            : *std::max_element(envelope.begin(), envelope.end());

    const float centreY = y + h * 0.5f;
    const float waveHeight = h * 3.0f;
    shadowBounds = juce::Rectangle<float>(x, centreY - maxSample * waveHeight * 0.5f,
                                          renderedWidth, maxSample * waveHeight);
  }

  shadowBounds = shadowBounds.expanded(4.0f, 4.0f);
  if (shadowBounds.getWidth() < PitchToolHandles::buttonGroupWidth)
  {
    const float centreX = shadowBounds.getCentreX();
    shadowBounds.setX(centreX - PitchToolHandles::buttonGroupWidth * 0.5f);
    shadowBounds.setWidth(PitchToolHandles::buttonGroupWidth);
  }

  return shadowBounds;
}

juce::Rectangle<float>
PianoRollComponent::getPreviewButtonBounds(const Note &note) const
{
  const auto shadowBounds = getNoteHoverShadowBounds(note);

  constexpr float buttonGap = 4.0f;
  const float buttonWidth = static_cast<float>(previewButtonWidth);
  const float buttonHeight = static_cast<float>(previewButtonHeight);
  const float buttonGroupWidth =
      buttonWidth + buttonGap + static_cast<float>(resetButtonWidth);
  const float buttonX = shadowBounds.getCentreX() - buttonGroupWidth * 0.5f;
  const float buttonY = shadowBounds.getBottom() + 7.0f +
      (editMode == EditMode::Select ? PitchToolHandles::buttonHeight + 10.0f : 0.0f);
  return {buttonX, buttonY, buttonWidth, buttonHeight};
}

bool PianoRollComponent::isNoteTopZone(const Note &note, float worldY) const
{
  // La franja superior de la nota: ahi el cursor se vuelve herramienta de corte.
  // Se mide desde la parte alta de la nota hacia abajo, en coordenadas del lienzo.
  const float top = midiToY(note.getAdjustedMidiNote());
  const float band = juce::jmax(4.0f, pixelsPerSemitone * noteTopZoneRatio);
  return worldY >= top && worldY <= top + band;
}

juce::Rectangle<float>
PianoRollComponent::getPreviewHoverBounds(const Note &note) const
{  const float noteStartTime =
      static_cast<float>(framesToSeconds(note.getStartFrame()));
  const float noteEndTime =
      static_cast<float>(framesToSeconds(note.getEndFrame()));
  const float noteX = noteStartTime * pixelsPerSecond;
  const float noteW = std::max((noteEndTime - noteStartTime) * pixelsPerSecond,
                               4.0f);
  const float noteY = midiToY(note.getAdjustedMidiNote());
  const juce::Rectangle<float> noteBounds(noteX, noteY, noteW,
                                          pixelsPerSemitone);

  return noteBounds.getUnion(getPreviewButtonBounds(note))
      .getUnion(getResetButtonBounds(note))
      .expanded(4.0f);
}

juce::Rectangle<float>
PianoRollComponent::getResetButtonBounds(const Note &note) const
{
  constexpr float buttonGap = 4.0f;
  auto bounds = getPreviewButtonBounds(note);
  return {bounds.getRight() + buttonGap, bounds.getY(),
          static_cast<float>(resetButtonWidth),
          static_cast<float>(resetButtonHeight)};
}

juce::Rectangle<int>
PianoRollComponent::getPreviewButtonLocalBounds(const Note &note) const
{
  const auto bounds =
      getPreviewButtonBounds(note)
          .translated(static_cast<float>(pianoKeysWidth) -
                          static_cast<float>(scrollX),
                      static_cast<float>(headerHeight) -
                          static_cast<float>(scrollY));
  return {static_cast<int>(std::round(bounds.getX())),
          static_cast<int>(std::round(bounds.getY())), previewButtonWidth,
          previewButtonHeight};
}

juce::Rectangle<int>
PianoRollComponent::getResetButtonLocalBounds(const Note &note) const
{
  const auto bounds =
      getResetButtonBounds(note)
          .translated(static_cast<float>(pianoKeysWidth) -
                          static_cast<float>(scrollX),
                      static_cast<float>(headerHeight) -
                          static_cast<float>(scrollY));
  return {static_cast<int>(std::round(bounds.getX())),
          static_cast<int>(std::round(bounds.getY())), resetButtonWidth,
          resetButtonHeight};
}

void PianoRollComponent::updatePreviewButtonBounds()
{
  // The audition/reset pair that used to float beside the hovered note moved
  // out of the note: restore lives in the note context menu and the left
  // inspector, and audition is available from the top bar. Keep the child
  // components hidden so no second menu appears around the note.
  previewButton.setVisible(false);
  resetButton.setVisible(false);
}

Note *PianoRollComponent::findPreviewButtonNoteAt(float x, float y) const
{
  juce::ignoreUnused(x, y);
  return nullptr;
}

std::vector<Note *> PianoRollComponent::getResetTargetNotes(Note &note) const
{
  std::vector<Note *> targets{&note};
  if (!project || editMode != EditMode::Select)
    return targets;

  std::vector<Note *> selected;
  for (auto &candidate : project->getNotes())
    if (!candidate.isRest() && candidate.isSelected())
      selected.push_back(&candidate);
  return note.isSelected() && selected.size() > 1 ? selected : targets;
}

bool PianoRollComponent::canGlueTarget(bool next) const
{
  if (!project)
    return false;
  // N3: gluing is a structural edit of one item (it joins a note with its
  // contiguous neighbour of the same item); over a selection that reaches other
  // items it is not offered as a batch.
  if (documentBatchApplies())
    return false;
  // Glue is a per-note structural edit: unlike the sliders it never falls
  // back to "all segments" when there is no selection.
  for (auto *note : getSelectedNotes())
    if (note != nullptr && !note->isRest() &&
        findContiguousNeighbour(*note, next) != nullptr)
      return true;
  return false;
}

bool PianoRollComponent::glueTargetNote(bool next)
{
  if (documentBatchApplies())
  {
    if (onBatchNotice)
      onBatchNotice(TR("notice.batch_item_bound"));
    return false;
  }
  if (!project || !noteSplitter)
    return false;

  for (auto *note : getSelectedNotes())
  {
    if (note == nullptr || note->isRest())
      continue;
    auto *neighbour = findContiguousNeighbour(*note, next);
    if (neighbour == nullptr)
      continue;
    const bool changed = next ? noteSplitter->mergeNotes(note, neighbour)
                              : noteSplitter->mergeNotes(neighbour, note);
    // mergeNotes rebuilds the note list; pointers into it are no longer safe.
    dropTransientNotePointers();
    repaint();
    return changed;
  }
  return false;
}

void PianoRollComponent::resetSelection(NoteRestoreMode mode)
{
  // N3: the resets are one undoable action over one item's notes; over a
  // selection that reaches other items they are refused with a notice instead
  // of restoring a part of it.
  if (documentBatchApplies())
  {
    if (onBatchNotice)
      onBatchNotice(TR("notice.batch_item_bound"));
    return;
  }
  auto selected = getSelectedNotes();
  if (selected.empty())
    return;
  if (mode == NoteRestoreMode::Timing)
  {
    // Timing has its own action (createResetTimingAction); resetNoteTiming
    // also expands the first note to the whole selection.
    resetNoteTiming(*selected.front());
    return;
  }
  // resetNoteEdits expands a note to the whole selection, so the first one is
  // enough and every target receives the same restore.
  resetNoteEdits(*selected.front(), mode);
}

bool PianoRollComponent::mergeSelectedNotes()
{
  // Melodyne disables Merge Notes unless exactly one note is selected.
  auto selected = getSelectedNotes();
  if (selected.size() != 1)
    return false;
  return glueTargetNote(true);
}

bool PianoRollComponent::resetNoteToOriginal(int index)
{
  if (!project)
    return false;
  auto &notes = project->getNotes();
  if (index < 0 || index >= static_cast<int>(notes.size()))
    return false;
  auto &note = notes[static_cast<size_t>(index)];
  if (note.isRest())
    return false;
  resetNoteEdits(note, NoteRestoreMode::All);
  return true;
}

void PianoRollComponent::resetNoteEdits(Note &note, NoteRestoreMode mode)
{  if (!project)
    return;

  const auto targets = getResetTargetNotes(note);
  if (mode == NoteRestoreMode::Amplitude)
  {
    std::vector<float> before, after(targets.size(), 0.0f);
    for (const auto* target : targets) before.push_back(target->getVolumeDb());
    if (before == after) return;
    applyRestoreAction(std::make_unique<AmplitudeAction>(
        *project, targets, before, after, pitchToolController->onAmplitudeEdited,
        "Restore Amplitude"));
    return;
  }

  const bool all = mode == NoteRestoreMode::All;
  applyRestoreAction(std::make_unique<ResetNoteEditsAction>(
      *project, targets, all || mode == NoteRestoreMode::Pitch,
      all || mode == NoteRestoreMode::Formant, all,
      all ? "Restore All Edits" : mode == NoteRestoreMode::Formant
          ? "Restore Formant" : "Restore Pitch",
      all ? createResetTimingAction(note) : nullptr));
}

void PianoRollComponent::applyRestoreAction(std::unique_ptr<UndoableAction> action)
{
  if (!action)
    return;

  // Same order as MainComponent::undo(): drop any transient drawing preview,
  // change the data, record history, then let the shared post-history path
  // refresh the UI and trigger the render.
  cancelPitchDrawingPreview();
  const bool requiresResynthesis = action->requiresAudioResynthesis();
  LOG("history: apply " + action->getName());
  action->redo();
  if (undoManager)
    undoManager->addAction(std::move(action));

  updatePitchToolHandlesFromSelection();
  updatePreviewButtonBounds();

  if (onHistoryActionApplied)
  {
    onHistoryActionApplied(requiresResynthesis);
    return;
  }

  // No host wiring (e.g. a bare component): keep the old behaviour.
  invalidateBasePitchCache();
  if (onPitchEdited)
    onPitchEdited();
  if (requiresResynthesis && onPitchEditFinished)
    onPitchEditFinished();
  repaint();
}

void PianoRollComponent::refreshLocalisedText()
{
  previewButton.setTooltip(TR("tooltip.context_audition"));
  resetButton.setTooltip(TR("tooltip.restore_original"));
}

void PianoRollComponent::showNoteMenuAt(Note &note, juce::Point<int> screenPoint)
{
  showCanvasMenuAt(screenPoint, &note);
}

void PianoRollComponent::showResetMenu(Note &note, juce::Point<int> screenPoint)
{
  juce::PopupMenu menu;
  menu.setLookAndFeel(&pitchPopupMenu::getLookAndFeel());
  const auto targets = getResetTargetNotes(note);
  const auto anyTarget = [&targets](bool (*predicate)(const Note&))
  {
    return std::any_of(targets.begin(), targets.end(),
                       [predicate](const Note* n) { return n && predicate(*n); });
  };
  const bool pitchChanged = anyTarget(hasPitchEdits);
  const bool formantChanged = anyTarget(hasFormantEdits);
  const bool amplitudeChanged = anyTarget(hasAmplitudeEdits);
  const bool timingChanged = anyTarget(hasTimingEdits);
  const bool anyChanged =
      pitchChanged || formantChanged || amplitudeChanged || timingChanged;

  const auto addItem = [&menu](int id, const juce::String& label, bool enabled)
  {
    juce::PopupMenu::Item item(label);
    item.itemID = id;
    item.isEnabled = enabled;
    item.customComponent = std::make_unique<pitchPopupMenu::MenuItemComponent>(
        label, false, std::function<void()>{}, false).release();
    menu.addItem(std::move(item));
  };
  addItem(1, TR("restore.pitch"), pitchChanged);
  addItem(3, TR("restore.formant"), formantChanged);
  addItem(4, TR("restore.amplitude"), amplitudeChanged);
  addItem(2, TR("restore.timing"), timingChanged);
  menu.addSeparator();
  addItem(5, TR("restore.all"), anyChanged);

  juce::Component::SafePointer<PianoRollComponent> safeThis(this);
  Note* notePtr = &note;
  auto options = juce::PopupMenu::Options().withTargetComponent(&resetButton);
  if (!screenPoint.isOrigin())
    options = juce::PopupMenu::Options().withTargetScreenArea(
        juce::Rectangle<int>(screenPoint.x, screenPoint.y, 1, 1));
  menu.showMenuAsync(options,
                     [safeThis, notePtr](int result)
  {
    if (safeThis == nullptr || !safeThis->project || result == 0)
      return;
    auto& notes = safeThis->project->getNotes();
    if (std::none_of(notes.begin(), notes.end(),
                     [notePtr](const Note& candidate)
                     { return &candidate == notePtr; }))
      return;
    if (result == 1)
      safeThis->resetNoteEdits(*notePtr);
    else if (result == 2)
      safeThis->resetNoteTiming(*notePtr);
    else if (result == 3)
      safeThis->resetNoteEdits(*notePtr, NoteRestoreMode::Formant);
    else if (result == 4)
      safeThis->resetNoteEdits(*notePtr, NoteRestoreMode::Amplitude);
    else if (result == 5)
      safeThis->resetNoteEdits(*notePtr, NoteRestoreMode::All);
  });
}

std::unique_ptr<UndoableAction> PianoRollComponent::createResetTimingAction(Note &note)
{
  if (!project)
    return {};

  auto& notes = project->getNotes();
  const auto targets = getResetTargetNotes(note);
  std::unordered_set<Note*> targetSet(targets.begin(), targets.end());
  std::unordered_map<Note*, NoteTimingState> desired;
  auto ensureDesired = [&](Note* candidate) -> NoteTimingState&
  {
    auto [it, inserted] = desired.emplace(
        candidate, NoteTimingState::capture(*candidate));
    juce::ignoreUnused(inserted);
    return it->second;
  };

  // Selected notes restore their own immutable ranges. Shared-boundary
  // companions outside the selection follow the restored selected edge.
  for (auto* target : targets)
  {
    if (!target)
      continue;

    Note* previous = nullptr;
    Note* next = nullptr;
    for (auto& candidate : notes)
    {
      if (&candidate == target || candidate.isRest())
        continue;
      if (candidate.getEndFrame() <= target->getStartFrame() &&
          (!previous || candidate.getEndFrame() > previous->getEndFrame()))
        previous = &candidate;
      if (candidate.getStartFrame() >= target->getEndFrame() &&
          (!next || candidate.getStartFrame() < next->getStartFrame()))
        next = &candidate;
    }

    const bool sharesLeftBoundary =
        previous && previous->getEndFrame() == target->getStartFrame();
    const bool sharesRightBoundary =
        next && next->getStartFrame() == target->getEndFrame();

    auto& targetState = ensureDesired(target);
    targetState.startFrame = target->getSrcStartFrame();
    targetState.endFrame = target->getSrcEndFrame();
    if (sharesLeftBoundary && targetSet.count(previous) == 0)
      ensureDesired(previous).endFrame = targetState.startFrame;
    if (sharesRightBoundary && targetSet.count(next) == 0)
      ensureDesired(next).startFrame = targetState.endFrame;
  }

  std::vector<NoteTimingState> before;
  std::vector<NoteTimingState> after;
  before.reserve(desired.size());
  after.reserve(desired.size());
  for (const auto& [affectedNote, targetState] : desired)
  {
    before.push_back(NoteTimingState::capture(*affectedNote));
    after.push_back(targetState);
  }

  return std::make_unique<TimingAction>(
      *project, std::move(before), std::move(after), "Restore Timing");
}

void PianoRollComponent::resetNoteTiming(Note &note)
{
  applyRestoreAction(createResetTimingAction(note));
}

void PianoRollComponent::triggerPreviewForNote(Note &note)
{
  if (!project)
    return;

  auto &notes = project->getNotes();
  auto it = std::find_if(notes.begin(), notes.end(),
                         [&note](const Note &candidate)
                         { return &candidate == &note; });
  if (it == notes.end())
    return;

  const auto sourceRegion = timingRegions::getSourceRegion(*project, note);

  auto isInSameRegion = [&](const Note &candidate)
  {
    return timingRegions::belongsTo(candidate, sourceRegion);
  };

  Note *startNote = &note;
  Note *endNote = &note;

  for (auto prev = it; prev != notes.begin();)
  {
    --prev;
    if (!prev->isRest() && isInSameRegion(*prev))
    {
      startNote = &(*prev);
      break;
    }
  }

  for (auto next = std::next(it); next != notes.end(); ++next)
  {
    if (!next->isRest() && isInSameRegion(*next))
    {
      endNote = &(*next);
      break;
    }
  }

  const int startFrame = std::max(0, startNote->getStartFrame());
  const int endFrame = std::max(startFrame, endNote->getEndFrame());
  setPreviewPlaybackState(endFrame > startFrame, startFrame, endFrame);

  if (endFrame > startFrame && onPreviewRegionRequested)
    onPreviewRegionRequested(startFrame, endFrame);
}

void PianoRollComponent::setPreviewPlaybackState(bool active, int startFrame,
                                                 int endFrame)
{
  previewPlaybackActive = active && endFrame > startFrame;
  previewPlaybackStartFrame = previewPlaybackActive ? startFrame : 0;
  previewPlaybackEndFrame = previewPlaybackActive ? endFrame : 0;
  previewPlaybackCurrentTime =
      previewPlaybackActive ? framesToSeconds(startFrame) : 0.0;
  repaint();
}

void PianoRollComponent::setPreviewPlaybackPosition(double timeSeconds)
{
  if (!previewPlaybackActive)
    return;

  if (std::abs(previewPlaybackCurrentTime - timeSeconds) < 0.0001)
    return;

  previewPlaybackCurrentTime = timeSeconds;
  repaint();
}

void PianoRollComponent::setHoveredNote(Note *note)
{
  if (hoveredNote == note)
    return;

  hoveredNote = note;
  updatePitchToolHandlesFromSelection();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::updateScrollBars()
{
  if (project)
  {
    float totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

    int visibleWidth = getVisibleContentWidth();
    int visibleHeight = getVisibleContentHeight();
    scrollX = clampScrollX(scrollX);
    coordMapper->setScrollX(scrollX);

    horizontalScrollBar.setRangeLimits(
        getTimelineStart() * pixelsPerSecond,
        std::max(getTimelineStart() * pixelsPerSecond +
                     static_cast<double>(visibleWidth),
                 getTimelineDuration() * pixelsPerSecond));
    horizontalScrollBar.setCurrentRange(scrollX, visibleWidth);

    verticalScrollBar.setRangeLimits(0, totalHeight);
    verticalScrollBar.setCurrentRange(scrollY, visibleHeight);
  }
}

double PianoRollComponent::getTimelineStart() const
{
  // Clip mode: the canvas starts at the modification's own time zero.
  // Track mode: it starts at the host's time zero, which in the active
  // modification's time can be negative (a region placed later in the song
  // than its offset in the take) or positive.
  return trackViewMode ? timelineToProject(0.0) : 0.0;
}

double PianoRollComponent::getTimelineDuration() const
{
  if (trackViewMode)
  {
    // The track's extent: every region on it plus the active region. The
    // rest of the active take is hidden in Track mode, so it does not extend
    // the timeline.
    double end = std::max(liveTimelineEndSeconds, hostTimelineEndSeconds);
    if (regionPreviewsEndSeconds > 0.0)
      end = std::max(end, timelineToProject(regionPreviewsEndSeconds));
    if (const auto span = getTrackModeActiveSpan())
      end = std::max(end, span->second);
    else if (project != nullptr)
      end = std::max(end, static_cast<double>(
                              project->getAudioData().getDuration()));
    end += getTimelineBarSeconds();
    return std::max(end, getTimelineStart() + getTimelineBarSeconds());
  }

  const double projectDuration =
      project ? static_cast<double>(project->getAudioData().getDuration()) : 0.0;
  double timelineDuration =
      std::max({projectDuration, liveTimelineEndSeconds, hostTimelineEndSeconds});

  // ARA region projects contain a playback range; give the editor one full bar
  // of writable-looking canvas after the real project end without extending
  // the underlying audio or playback range.
  if (project != nullptr &&
      !project->getAudioData().playbackRegionRanges.empty())
    timelineDuration += getTimelineBarSeconds();

  return timelineDuration;
}

double PianoRollComponent::getTimelineLength() const
{
  return std::max(0.0, getTimelineDuration() - getTimelineStart());
}

double PianoRollComponent::clampScrollX(double x) const
{
  const double minScrollX = getTimelineStart() * pixelsPerSecond;
  const double maxScrollX =
      std::max(minScrollX, getTimelineDuration() * pixelsPerSecond -
                               static_cast<double>(getVisibleContentWidth()));
  return juce::jlimit(minScrollX, maxScrollX, x);
}

std::optional<std::pair<double, double>>
PianoRollComponent::getTrackModeActiveSpan() const
{
  if (!trackViewMode || project == nullptr)
    return std::nullopt;
  const auto &ranges = project->getAudioData().playbackRegionRanges;
  if (ranges.empty() || ranges.front().second <= ranges.front().first)
    return std::nullopt;
  return ranges.front();
}

void PianoRollComponent::setTrackViewMode(bool shouldShowTrack)
{
  if (trackViewMode == shouldShowTrack)
    return;

  trackViewMode = shouldShowTrack;
  if (coordMapper)
    coordMapper->setViewStartSeconds(getTimelineStart());
  waveformBackgroundRenderer->setProjectDrawRange(getTrackModeActiveSpan());
  invalidateWaveformCache();
  setHoveredNote(nullptr);
  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::reapplyBasePitchForNote(Note *note)
{
  if (!note || !project)
    return;

  auto &audioData = project->getAudioData();
  int startFrame = note->getStartFrame();
  int endFrame = note->getEndFrame();
  int f0Size = static_cast<int>(audioData.f0.size());

  // Reapply base + delta from dense curves
  for (int i = startFrame; i < endFrame && i < f0Size; ++i)
  {
    float base = (i < static_cast<int>(audioData.basePitch.size()))
                     ? audioData.basePitch[static_cast<size_t>(i)]
                     : 0.0f;
    float delta = (i < static_cast<int>(audioData.deltaPitch.size()))
                      ? audioData.deltaPitch[static_cast<size_t>(i)]
                      : 0.0f;
    audioData.f0[i] = midiToFreq(base + delta);
  }

  // Always set F0 dirty range for synthesis (needed for undo/redo to trigger
  // resynthesis)
  project->markNoteEditDirtyRange(startFrame, endFrame);

  // Trigger repaint
  repaint();
}

void PianoRollComponent::cancelPitchDrawingPreview()
{
  if (pitchDrawingHandler_ && pitchDrawingHandler_->isActive())
    pitchDrawingHandler_->cancel();
}

void PianoRollComponent::updatePitchDrawingConfirmationPopup()
{
  if (!pitchDrawingConfirmationPanel)
    return;

  const bool shouldShow = editMode == EditMode::PitchDrawing && pitchDrawingHandler_ &&
                          pitchDrawingHandler_->hasAnchors();
  pitchDrawingConfirmationPanel->setVisible(shouldShow);
  if (!shouldShow)
    return;

  constexpr int popupWidth = 213;
  constexpr int popupHeight = 30;
  const int canvasWidth = getVisibleContentWidth();
  const int popupX = pianoKeysWidth + std::max(0, (canvasWidth - popupWidth) / 2);
  pitchDrawingConfirmationPanel->setBounds(popupX, headerHeight + 10,
                                     std::min(popupWidth, canvasWidth),
                                     popupHeight);
  pitchDrawingConfirmationPanel->toFront(false);
}

void PianoRollComponent::drawSelectionRect(juce::Graphics &g)
{
  if (!boxSelector || !boxSelector->isSelecting())
    return;

  auto rect = boxSelector->getSelectionRect();

  // Draw semi-transparent fill
  g.setColour(juce::Colours::white.withAlpha(0.05f));
  g.fillRect(rect);

  // Draw border
  g.setColour(juce::Colours::white.withAlpha(0.25f));
  g.drawRect(rect, 1.0f);
}
