#pragma once

#include "../JuceHeader.h"
#include "../Audio/Synthesis/EngineBlocks.h"
#include "../Audio/Engine/HostTempoMap.h"
#include "../Models/Project.h"
#include "../Models/MidiGuideGhosts.h"
#include "../Utils/Constants.h"
#include "../Utils/HarmonyUtils.h"
#include "../Utils/TransformParams.h"
#include "../Utils/Localization.h"
#include "../Utils/BasePitchPreview.h"
#include "../Undo/UndoActions.h"
#include "Commands.h"
#include "GuiTypes.h"
#include "PianoRoll/BoxSelector.h"
#include "PianoRoll/CoordinateMapper.h"
#include "PianoRoll/GridRenderer.h"
#include "PianoRoll/NoteRenderer.h"
#include "PianoRoll/NoteSplitter.h"
#include "PianoRoll/PianoKeysRenderer.h"
#include "PianoRoll/PitchCurveRenderer.h"
#include "PianoRoll/PitchEditor.h"
#include "PianoRoll/TimelineRenderer.h"
#include "PianoRoll/WaveformBackgroundRenderer.h"
#include "PianoRoll/PitchToolController.h"
#include "PianoRoll/PitchToolHandles.h"
#include "PianoRoll/PianoRollViewState.h"
#include "PianoRoll/ScrollZoomController.h"
#include "Buttons.h"
#include "RegionPreview.h"

#include <memory>
#include <optional>

class PitchUndoManager;
class PianoRollInteractionContext;

// Interaction handler forward declarations
class InteractionHandler;
class LoopDragHandler;
class SelectHandler;
class SplitHandler;
class PitchDrawingHandler;
class TimingHandler;
class PitchDrawingConfirmationPanel;

/**
 * Piano roll component for displaying and editing notes.
 * Supports DPI-aware scaling for multi-monitor setups.
 */
class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener,
                           public juce::KeyListener,
                           public juce::TooltipClient
{
  friend class PianoRollInteractionContext;

public:
  using juce::Component::keyPressed;

  static constexpr int pianoKeysWidth = 30;
  static constexpr int timelineHeight = 18;
  static constexpr int loopTimelineHeight = 16;
  static constexpr int headerHeight = timelineHeight + loopTimelineHeight;
  static constexpr juce::int64 minDragRepaintInterval = 16; // ~60fps max

  PianoRollComponent();
  ~PianoRollComponent() override;

  // Snap for a note drag: Shift = Off, Cmd = Absolute, Alt = Relative,
  // otherwise the configured mode (VariAudio Pitch Snap Mode).
  PitchEditor::DragSnapKind resolveDragSnapKind(
      const juce::ModifierKeys &mods) const;
  void setAbsoluteDragSnap(bool absolute) { absoluteDragSnap = absolute; }
  bool getAbsoluteDragSnap() const { return absoluteDragSnap; }

  // True where a drag should pan the view: no segment, control or button,
  // and outside the waveform band (component coordinates).
  bool isEmptyCanvasPoint(juce::Point<float> position);

  // Forget hover and smart-control pointers into the note list. Call after
  // anything that may rebuild the list (undo/redo, merge, split).
  void dropTransientNotePointers();

  // Adjacent segment that shares a boundary in time and source, or nullptr.
  Note *findContiguousNeighbour(const Note &note, bool next) const;

  // Inspector sliders (VariAudio section). They act on the selected
  // segments, or on every segment when nothing is selected; one slider
  // gesture is one undo step.
  enum class InspectorParam { CorrectPitch, FlattenCurve, PitchDrift, Formant, Volume, Transition, TiltLeft, TiltRight, Tilt };
  // D-279: Melodyne's Reset Individual Edits categories.
  enum class NoteRestoreMode { Pitch, Formant, Amplitude, Timing, All };
  std::vector<Note *> getInspectorTargets() const;
  // Value shown for the first target, or nullopt when there are no targets.
  std::optional<float> getInspectorValue(InspectorParam param) const;
  void beginInspectorEdit(InspectorParam param);
  void setInspectorEditValue(float value);
  void endInspectorEdit();
  bool isInspectorEditActive() const { return inspectorEditActive; }

  // VariAudio "Show All Smart Controls" (false = default set only).
  void setShowAllSmartControls(bool showAll);
  bool getShowAllSmartControls() const { return showAllSmartControls; }

  // The Basic/Advanced choice also decides whether the Formant and Amplitude
  // tools appear in the unified context menu.
  void setEditorUiMode(EditorUiMode mode) { editorUiMode = mode; }
  void setCurrentEditorTool(EditorTool tool) { currentEditorTool = tool; }

  // Glue a selected note with its contiguous neighbour (VariAudio
  // smart-control glue, moved from the note overlay to the inspector). It
  // requires a selection and the merge is undoable through NoteSplitter.
  bool canGlueTarget(bool next) const;
  bool glueTargetNote(bool next);

  // Moves the selected notes (or every note when nothing is selected) by a
  // scale-degree interval in the project key, as one undoable step.
  HarmonyUtils::Result applyHarmonyInterval(HarmonyUtils::Interval interval,
                                            bool up);

  // M4 v1 (D-242): cuantiza las fronteras de las notas seleccionadas (o de
  // todas) a la rejilla visible del host, con fuerza 0..100, como un paso
  // undoable. Fuerza 0 no toca nada. Devuelve false si no hay proyecto, no hay
  // rejilla o el resultado colapsaria una nota o crearia un solape nuevo.
  bool quantizeTimeSelection(int strengthPercent);

  // M5 v1 (D-245): silencia o reactiva las notas seleccionadas. Si todas ya
  // estaban silenciadas, las reactiva. Es undoable y vuelve a renderizar por
  // bloque. Devuelve false si no hay proyecto o no hay seleccion.
  bool muteSelection();
  // M5 v1 (D-245): fija el estado de silencio de una lista explicita de notas
  // (menu de nota, seleccion). Undoable y con re-render por bloque.
  bool setMutedForNotes(const std::vector<Note *> &notes, bool muted);

  // M5 v2 (D-299): elimina la seleccion. La nota queda como lapida silenciosa
  // (deleted + rest), undoable y con re-render por bloque. Devuelve false si
  // no hay proyecto ni notas que eliminar.
  bool deleteSelection();

  // D-267 (T7): reset de una nota por indice, por el mismo camino que el boton
  // y el menu (ResetNoteEditsAction: pitch, formante, amplitud, tiempo, motor y
  // mute, con undo y re-render). Devuelve false si el indice no es valido, la
  // nota es una pausa o no hay proyecto.
  bool resetNoteToOriginal(int index);
  // D-279: Melodyne's Edit > Reset Individual Edits, applied to the selection.
  // With no selection they are no-ops, exactly like Melodyne grays them out.
  void resetSelection(NoteRestoreMode mode);
  // D-279: Melodyne's "Merge Notes": one selected note plus the next one.
  bool mergeSelectedNotes();

  // D-279: the canvas context menu asks MainComponent for the two macros, so
  // the menu, the inspector button and the keyboard command share one path.
  std::function<void()> onCorrectPitchMacroRequested;
  std::function<void(juce::Point<int>)> onQuantizeTimeMenuRequested;

  // Tarea 3 (2026-11-22): vista de bypass estilo Melodyne. Con el bypass
  // activo se dibuja solo la curva F0 de la FUENTE en gris, sin bloques de nota
  // ni la curva editada. getBypassCurveCents() devuelve exactamente la curva
  // que se dibuja, para poder medirla.
  void setBypassView(bool active);
  bool isBypassView() const { return bypassView; }
  std::vector<float> getBypassCurveCents(int firstFrame, int lastFrame) const;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseMove(const juce::MouseEvent &e) override;
  void mouseEnter(const juce::MouseEvent &e) override;
  void mouseExit(const juce::MouseEvent &e) override;
  juce::String getTooltip() override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override;
  void mouseMagnify(const juce::MouseEvent &e, float scaleFactor) override;
  void modifierKeysChanged(const juce::ModifierKeys &modifiers) override;

  // Focus handling - re-grab focus when lost (important for plugin mode)
  void focusLost(FocusChangeType cause) override;
  void focusGained(FocusChangeType cause) override;

  // KeyListener
  bool keyPressed(const juce::KeyPress &key) override;
  bool keyPressed(const juce::KeyPress &key,
                  juce::Component *originatingComponent) override;

  void requestCanvasKeyboardFocus();

  /** Melodyne canvas menu: one popup with the note actions (when `note` is
      given), undo/redo, select all and the tool strip. */
  void showCanvasMenuAt(juce::Point<int> screenPoint, Note* note = nullptr);
  /** Kept for callers that already have a note; same unified popup. */
  void showNoteMenuAt(Note& note, juce::Point<int> screenPoint);

  // ScrollBar::Listener
  void scrollBarMoved(juce::ScrollBar *scrollBar,
                      double newRangeStart) override;

  // Project
  void setProject(Project *proj);
  // P1b fase 6: estado del barrido por primitivas, para no arrastrar la cola
  // hasta el renderer. Se reenvia a los dos renderers de notas.
  // D-309: con el cajon Motor abierto el rango se expande a la primera y la
  // ultima nota del bloque visual afectado y el barrido enciende bordes.
  void setSweepState(bool active, int startFrame, int endFrame, int remaining);

  // D-309: engine drawer view. Closed = the editor keeps its usual look and the
  // white sweep; open = every non-rest note gets its block's engine border and
  // the sweep lights those borders instead of drawing a white line.
  void setEngineViewActive(bool active);
  bool isEngineViewActive() const { return engineViewActive; }
  void setGlobalSynthesisEngine(SynthesisEngineType engine);
  SynthesisEngineType getGlobalSynthesisEngine() const
  {
    return globalSynthesisEngine;
  }
  /// Engine the renderer will use for the visual block that contains @p note,
  /// or nothing when the note is a rest / outside every phrase.
  std::optional<SynthesisEngineType> getNoteBlockEngine(const Note &note);
  /// Notes of the visual block(s) of the explicit selection; empty when
  /// nothing is selected (the block picker then stays disabled).
  std::vector<Note *> getActiveEngineBlockNotes();
  std::vector<int> getActiveEngineBlockIndices();
  bool hasExplicitEngineSelection() const;
  /// D-309 measurement: how many times the visual map was rebuilt. The cache
  /// is reused while the fingerprint (times, engines, rest/deleted, global
  /// engine, source) does not change.
  int getEngineBlockBuildCount() const { return engineBlockBuildCount; }
  // D-309 measurement: the range the last sweep was expanded to and whether it
  // resolved to the neutral colour (several blocks).
  int getSweepStartFrame() const { return lastSweepStartFrame; }
  int getSweepEndFrame() const { return lastSweepEndFrame; }
  bool isSweepNeutral() const { return lastSweepNeutral; }
  bool isSweepActive() const { return lastSweepActive; }
  /// D-180/D-309 measurement only: clears the paint counters so a test can
  /// measure the steady state without the first paint's one-off cost.
  void resetPaintStats()
  {
    paintCount = 0;
    lastPaintDurationMs = 0.0;
    maxPaintDurationMs = 0.0;
  }
  void beginLiveRecordingWaveform(double sampleRate,
                                  double timelineOffsetSeconds);
  void appendLiveRecordingWaveform(const juce::AudioBuffer<float> &buffer);
  // Extends the plugin canvas to include the furthest host playhead position.
  bool extendTimelineTo(double endSeconds);
  Project *getProject() const { return project; }
  std::vector<Note *> getSelectedNotes() const;
  PianoRollInteractionContext &getInteractionContext()
  {
    return *interactionContext;
  }

  // Undo Manager
  void setUndoManager(PitchUndoManager *manager);
  PitchUndoManager *getUndoManager() const { return undoManager; }

  // Cursor
  void setCursorTime(double time);
  void setPlaybackActive(bool active);
  double getCursorTime() const { return cursorTime; }

  // Zoom with optional center point
  void setPixelsPerSecond(float pps, bool centerOnCursor = false);
  void setPixelsPerSemitone(float pps, float anchorContentY = -1.0f);
  float getPixelsPerSecond() const { return pixelsPerSecond; }
  float getPixelsPerSemitone() const { return pixelsPerSemitone; }

  // Scale-grid visualization
  void setScaleMode(ScaleMode mode);
  void setScaleRootNote(int noteInOctave);
  void refreshScaleFromProject();
  void setScaleRootPreview(std::optional<int> noteInOctave);
  void setScaleModePreview(std::optional<ScaleMode> mode);
  void setSnapToSemitoneDrag(bool enabled);
  void setDragSnapMode(DragSnapMode mode);
  void setPitchReferenceHz(int hz);
  void setTimelineDisplayMode(TimelineDisplayMode mode);

  // Where the drawn content currently sits on the host timeline. Display only:
  // nothing in the project moves. May be negative. See CoordinateMapper.
  void setTimelineDisplayOffset(double seconds);
  double projectToTimeline(double projectSeconds) const;
  double timelineToProject(double timelineSeconds) const;
  // R4 (D-317): persistToProject = false is the host's cursor value, which is
  // display/fallback only while a real host tempo map exists.
  void setTimelineBeatSignature(int numerator, int denominator,
                                bool persistToProject = true);
  void setTimelineTempoBpm(double bpm, bool persistToProject = true);
  // D-310: the host tempo map snapshot. Null means "no usable host map" and
  // the constant BPM above stays the fallback. Published by the editor timer;
  // consumed by the grid phase and by the MIDI guide when placing ghosts.
  void setHostTempoMap(std::shared_ptr<const HostTempoMap> tempoMap);
  std::shared_ptr<const HostTempoMap> getHostTempoMap() const {
    return hostTempoMap;
  }

  // D-311/R2: MIDI guide ghosts, in HOST seconds (MidiGuideGhosts.h). Read-only
  // layer: no hit-test, no edit. The caller rebuilds the list from the selected
  // guide and the active tempo map; the painter applies host -> project with
  // the coordinate mapper, so changing the active item moves nothing.
  using MidiGuideGhost = ::MidiGuideGhost;
  void setMidiGuideGhosts(std::vector<MidiGuideGhost> ghosts);
  bool hasMidiGuideGhosts() const { return !midiGuideGhosts.empty(); }
  size_t getMidiGuideGhostCount() const { return midiGuideGhosts.size(); }
  const MidiGuideGhostLayer &getMidiGuideGhosts() const {
    return midiGuideGhosts;
  }
  void setTimelineGridDivision(TimelineGridDivision division);
  // R3: the current grid division in quarter notes (1.0 = quarter).
  double getTimelineGridQuarterNotes() const;
  void setTimelineSnapCycle(bool enabled);

  // Enable/disable cycle (loop) range editing on the timeline strip.
  // Turned off in non-ARA plugin mode, where the host transport is not ours
  // to drive.
  void setCycleEditEnabled(bool enabled);
  bool isCycleEditEnabled() const { return cycleEditEnabled; }

  // Scroll
  void setScrollX(double x);
  double getScrollX() const { return scrollX; }

  // D-180: measurement only. Counts completed paints and reports how long the
  // last one took, so a blocked message thread can be told apart from a late
  // state change. No behaviour depends on these values.
  juce::int64 getPaintCount() const { return paintCount; }
  double getLastPaintDurationMs() const { return lastPaintDurationMs; }
  double getMaxPaintDurationMs() const { return maxPaintDurationMs; }
  void setScrollY(double y);
  double getScrollY() const { return scrollY; }
  // End of the scrollable timeline, in the canvas's (project) time.
  double getTimelineDuration() const;
  // Start of the scrollable timeline: 0 in Clip mode; the host's time zero,
  // expressed in project time, in Track mode (may be negative).
  double getTimelineStart() const;
  double getTimelineLength() const;
  // Host-timeline seconds at the left edge of the view.
  double getHostScrollSeconds() const;

  // Track mode (ARA): show the whole track on the host timeline. The canvas
  // stays in the active modification's time; only its scrollable range
  // widens to the song, the active take is clipped to its region, and the
  // other regions (setRegionPreviews) are drawn and clickable. Clip mode is
  // the plain source view.
  void setTrackViewMode(bool shouldShowTrack);
  bool isTrackViewMode() const { return trackViewMode; }
  void centerOnPitchRange(float minMidi, float maxMidi);
  bool centerOnCurrentPitchRange();
  void fitPitchRangeToView(float minMidi, float maxMidi);
  int getVisibleContentWidth() const;
  int getVisibleContentHeight() const;
  void setHorizontalScrollBarVisible(bool shouldShow);

  // Edit mode
  void setEditMode(EditMode mode);
  EditMode getEditMode() const { return editMode; }

  // Cancel any transient pitch edit before undo/redo changes project history.
  void cancelPitchDrawingPreview();

  // View settings
  void setShowDeltaPitch(bool show)
  {
    showDeltaPitch = show;
    repaint();
  }
  void setShowBasePitch(bool show)
  {
    showBasePitch = show;
    repaint();
  }
  void setShowSegmentsDebug(bool show)
  {
    showSegmentsDebug = show;
    repaint();
  }
  void setShowGameValuesDebug(bool show)
  {
    showGameValuesDebug = show;
    repaint();
  }
  void setShowNoteFramesDebug(bool show)
  {
    showNoteFramesDebug = show;
    repaint();
  }
  void setShowUvInterpolationDebug(bool show)
  {
    showUvInterpolationDebug = show;
    repaint();
  }
  void setShowActualF0Debug(bool show)
  {
    showActualF0Debug = show;
    repaint();
  }
  void setShowCleanedF0Debug(bool show)
  {
    showCleanedF0Debug = show;
    repaint();
  }
  void setShowVocoderF0Debug(bool show)
  {
    showVocoderF0Debug = show;
    repaint();
  }
  bool getShowDeltaPitch() const { return showDeltaPitch; }
  bool getShowBasePitch() const { return showBasePitch; }

  // Callbacks
  std::function<void(EditMode)> onEditModeRequested;
  // A tool chosen in the unified context menu; the workspace updates the top
  // bar through the same path as the floating pill.
  std::function<void(EditorTool)> onEditorToolRequested;
  // Melodyne: a right click on the canvas asks the workspace for the tool pill
  // instead of opening the menu directly. The pill's "more" slot calls back
  // to show the same canvas menu.
  std::function<void(juce::Point<int>)> onShowToolPill;
  std::function<void(juce::Point<int>)> onShowCanvasMenu;
  std::function<void(juce::Point<int>)> onToolPillDrag;
  std::function<void(juce::Point<int>)> onToolPillRelease;
  std::function<void()> onHideToolPill;
  // Option/Alt-double-click on empty space (VariAudio: zoom to show all).
  std::function<void()> onZoomFullRequested;
  std::function<void()> onUndoRequested;
  std::function<void()> onRedoRequested;
  std::function<void(Note *)> onNoteSelected;
  std::function<void()> onPitchEdited;
  std::function<void()> onPitchEditFinished; // Called when dragging ends
  std::function<void()> onAmplitudeEdited; // Gain commit, undo, or redo
  // Restore actions are applied here and then handed to the host exactly as
  // an undo/redo is: same UI refresh, same resynthesis trigger, nothing sent
  // to the plugin before the render lands. The argument mirrors
  // UndoableAction::requiresAudioResynthesis().
  std::function<void(bool requiresResynthesis)> onHistoryActionApplied;
  std::function<void()> onPitchPreviewRenderRequested;
  std::function<void()> onPitchEditCommitted;
  std::function<void(const Note &)> onNoteDragAudition;
  std::function<void()> onNoteDragAuditionFinished;
  std::function<void(int midiNote)> onPianoKeyAudition;
  std::function<void()> onPianoKeyAuditionFinished;
  std::function<void()> onCursorMoved;
  std::function<void(double)> onSeek;
  // Invoked for a double-click on unoccupied canvas space. The owner seeks
  // before toggling transport so playback starts or stops at that position.
  std::function<void(double)> onCanvasEmptyDoubleClick;
  std::function<void(float)> onZoomChanged;
  std::function<void(double)> onScrollChanged;
  std::function<void(const LoopRange &)> onLoopRangeChanged;
  std::function<void(int, int)> onPreviewRegionRequested;
  std::function<void(int, int)>
      onReinterpolateUV; // Called to re-infer UV regions (startFrame, endFrame)

  void setPreviewPlaybackState(bool active, int startFrame, int endFrame);
  void setPreviewPlaybackPosition(double timeSeconds);

  // ARA: every region of the track, drawn behind the editable project. The
  // active entry keeps the region backdrop; the others are dimmed read-only
  // previews. A click inside an inactive region fires onInactiveRegionClicked
  // so the owner can make it active; if the owner swaps the project in
  // synchronously, the same click continues into the new region (selecting
  // or dragging the note under the pointer).
  void setRegionPreviews(MainViewRegionPreviewList previews);
  std::function<void(const juce::String &)> onInactiveRegionClicked;

private:
  enum class NoteRenderPass
  {
    Body,
    Overlay,
    HoverShadow,
    HoveredBody
  };

  void drawBackgroundWaveform(juce::Graphics &g,
                              const juce::Rectangle<int> &visibleArea);
  void drawGrid(juce::Graphics &g, bool drawRowBackgrounds = true,
                bool drawGridLines = true);
  void drawTimeline(juce::Graphics &g);
  void drawLoopTimeline(juce::Graphics &g);
  void drawNotes(juce::Graphics &g, NoteRenderPass pass);
  // D-309: visual engine blocks (D-258 phrases), rebuilt only when the cheap
  // fingerprint changes. Called from paint and from the public queries.
  const engineBlocks::VisualBlockMap &ensureEngineBlockMap();
  juce::uint64 computeEngineBlockFingerprint() const;
  void rebuildEngineBlockMap();
  struct EngineBlockCache
  {
    bool valid = false;
    juce::uint64 fingerprint = 0;
    engineBlocks::VisualBlockMap map;
    std::vector<Note *> notePointers;
  };
  EngineBlockCache engineBlockCache;
  SynthesisEngineType globalSynthesisEngine = SynthesisEngineType::Psola;
  bool engineViewActive = false;
  int engineBlockBuildCount = 0;
  int lastSweepStartFrame = 0;
  int lastSweepEndFrame = 0;
  bool lastSweepNeutral = false;
  bool lastSweepActive = false;
  void drawPitchCurves(juce::Graphics &g);
  // D-311: read-only guide layer, drawn below the notes.
  void drawMidiGuideGhosts(juce::Graphics &g);
  void drawBypassSourceCurve(juce::Graphics &g);
  bool bypassView = false;
  void drawPianoKeys(juce::Graphics &g);
  void drawSelectionRect(juce::Graphics &g); // Box selection rectangle
  void drawAudioSourceRegionOverlay(juce::Graphics &g);
  void drawRegionPreviews(juce::Graphics &g);
  // Track mode: inactive regions' notes and pitch curves, drawn with the same
  // renderers as the active region and then greyed.
  // Notes of pinned tracks: inactive-region bodies, outlined in track colour.
  void drawPinnedTrackNotes(juce::Graphics &g,
                            const juce::Rectangle<int> &mainArea);
  void drawInactiveRegionEdits(juce::Graphics &g,
                               const juce::Rectangle<int> &mainArea);
  const MainViewRegionPreview *findActiveRegionPreview() const;
  bool previewContains(const MainViewRegionPreview &region,
                       double projectSeconds) const;
  double clampScrollX(double x) const;
  // The active region's span in project time, only in Track mode.
  std::optional<std::pair<double, double>> getTrackModeActiveSpan() const;
  const MainViewRegionPreview* findTrackRegionAtPointer(juce::Point<float>, bool notesOnly = false) const;
  bool prepareTrackPointer(juce::Point<float>, bool notesOnly = false);
  void drawLoopOverlay(juce::Graphics &g);
  void drawGameChunksDebugOverlay(juce::Graphics &g);
  void drawGameValuesDebugOverlay(juce::Graphics &g);
  void updatePitchToolHandlesFromSelection();
  void updatePitchDrawingConfirmationPopup();

  float midiToY(float midiNote) const;
  float yToMidi(float y) const;
  float timeToX(double time) const;
  double xToTime(float x) const;
  double getTimelineQuarterNoteSeconds() const;
  double getTimelineBeatSeconds() const;
  double getTimelineBarSeconds() const;
  double getTimelineGridSeconds() const;
  bool shouldSnapCycleToGrid() const;
  double snapTimeToTimelineGrid(double timeSeconds) const;
  bool isCanvasPoint(const juce::MouseEvent &e) const;
  bool isModifierZoomDrag(const juce::MouseEvent &e) const;
  void applyModifierZoomDrag(const juce::MouseEvent &e);
  bool isModifierPanDrag(const juce::MouseEvent &e) const;
  void applyModifierPanDrag(const juce::MouseEvent &e);
  void updateMouseCursorForEditMode();

  Note *findNoteAt(float x, float y);
  Note *findPreviewButtonNoteAt(float x, float y) const;
  juce::Rectangle<float> getPreviewButtonBounds(const Note &note) const;
  juce::Rectangle<float> getNoteHoverShadowBounds(const Note &note) const;
  juce::Rectangle<float> getResetButtonBounds(const Note &note) const;
  juce::Rectangle<float> getPreviewHoverBounds(const Note &note) const;

  // Melodyne: la franja superior de la nota es la zona de corte (el cursor cambia)
  // y el doble clic ahi separa; el doble clic en el cuerpo afina al 100 %.
  bool isNoteTopZone(const Note &note, float worldY) const;
  static constexpr float noteTopZoneRatio = 0.32f;
  // D-285/2: el doble clic en la franja superior corta en el segundo
  // `mouseDown`; el `mouseDoubleClick` que llega DESPUES encontraba ese corte
  // recien hecho en el puntero y lo volvia a unir («hace el corte y lo cancela
  // al instante»). El corte deja esta marca para que ese doble clic no lo una.
  bool splitJustPerformed = false;
  juce::Rectangle<int> getPreviewButtonLocalBounds(const Note &note) const;
  juce::Rectangle<int> getResetButtonLocalBounds(const Note &note) const;
  void updatePreviewButtonBounds();
  void triggerPreviewForNote(Note &note);
  std::vector<Note *> getResetTargetNotes(Note &note) const;
  void resetNoteEdits(Note &note, NoteRestoreMode mode = NoteRestoreMode::Pitch);
  std::unique_ptr<UndoableAction> createResetTimingAction(Note &note);
  void resetNoteTiming(Note &note);
  // Runs a Restore action, records it, and reports it via
  // onHistoryActionApplied so it is processed like undo/redo.
  void applyRestoreAction(std::unique_ptr<UndoableAction> action);
  void showResetMenu(Note &note, juce::Point<int> screenPoint = {});
  void setHoveredNote(Note *note);
  void updateScrollBars();
  bool nudgeSelectedNotesBySemitones(int semitoneDelta);
  // Absolute snap: moves each selected segment onto the next semitone (or
  // scale tone) in the given direction.
  bool nudgeSelectedNotesToNextPitch(int direction);
  // Shifts the selection by cents, ignoring snapping (Shift+Up/Down).
  bool nudgeSelectedNotesByCents(int cents);
  // Moves the selection to the previous/next segment (Left/Right).
  bool selectAdjacentSegment(bool next);
  // Selects the clicked segment and every later one with the same pitch.
  void selectFollowingSamePitch(Note &note);
  // Glue / split smart controls. Returns true when the click was consumed.
  bool handleSmartControlClick(float worldX, float worldY,
                               const juce::ModifierKeys &mods);
  // Moves each target note by the semitone count the callback returns (no
  // value or zero leaves the note alone) as one undoable step.
  bool shiftNotesBySemitones(
      const std::vector<Note *> &targets,
      const std::function<std::optional<float>(const Note &)> &shiftForNote);
  void reapplyBasePitchForNote(
      Note *note); // Recalculate F0 from base pitch + delta after undo/redo

  Project *project = nullptr;
  PitchUndoManager *undoManager = nullptr;
  // VariAudio look and smart controls (see NoteRenderer::setSegmentStyle).
  bool segmentStyle = true;
  bool showAllSmartControls = true; // VariAudio "Show All", the default here
  EditorUiMode editorUiMode = EditorUiMode::Basic;
  EditorTool currentEditorTool = EditorTool::Main;
  // Inspector slider gesture in progress.
  bool inspectorEditActive = false;
  InspectorParam inspectorEditParam = InspectorParam::CorrectPitch;
  std::vector<Note *> inspectorEditNotes;
  std::vector<TransformParams> inspectorEditOldParams;
  std::vector<float> inspectorEditSourceMidi;
  float correctionTargetFor(float sourceMidi) const;
  bool absoluteDragSnap = true; // Pitch Snap Mode Absolute by default
  // The first click of the current (double-)click landed on a segment. The
  // double-click splits it, after which the double-click point may hit no
  // segment; it must still not count as an empty-space double-click.
  bool clickStartedOnSegment = false;

  // New modular components
  std::unique_ptr<CoordinateMapper> coordMapper;
  std::unique_ptr<PianoKeysRenderer> pianoKeysRenderer;
  std::unique_ptr<GridRenderer> gridRenderer;
  std::unique_ptr<TimelineRenderer> timelineRenderer;
  std::unique_ptr<WaveformBackgroundRenderer> waveformBackgroundRenderer;
  double liveTimelineEndSeconds = 0.0;
  double hostTimelineEndSeconds = 0.0;
  double liveRecordingSampleRate = 0.0;
  std::unique_ptr<NoteRenderer> noteRenderer;
  std::unique_ptr<PitchCurveRenderer> pitchCurveRenderer;
  std::unique_ptr<NoteRenderer> previewNoteRenderer;
  std::unique_ptr<PitchCurveRenderer> previewCurveRenderer;
  struct InactiveLayerKey
  {
    double scrollX = 0.0, scrollY = 0.0, displayOffset = 0.0;
    float pixelsPerSecond = 0.0f, pixelsPerSemitone = 0.0f;
    int width = 0, height = 0;
    const void *previews = nullptr;
    bool showDeltaPitch = false, showBasePitch = false;
    bool operator==(const InactiveLayerKey &o) const
    {
      return scrollX == o.scrollX && scrollY == o.scrollY &&
             displayOffset == o.displayOffset &&
             pixelsPerSecond == o.pixelsPerSecond &&
             pixelsPerSemitone == o.pixelsPerSemitone && width == o.width &&
             height == o.height && previews == o.previews &&
             showDeltaPitch == o.showDeltaPitch &&
             showBasePitch == o.showBasePitch;
    }
  };
  struct InactiveMacroPreview {
    struct Binding {
      Note* display;
      const Note* source;
      float originalMidi, originalDrift;
    };
    std::shared_ptr<Project> project;
    std::function<bool()> isValid;
    std::vector<Binding> bindings;
    std::unique_ptr<PitchCurveRenderer> renderer;
  };
  std::vector<InactiveMacroPreview> inactiveMacroPreviews;
  double lastInactiveMacroUpdate = 0.0;
  struct TrackGestureNote {
    Note* note;
    Project* owner;
    float midi, finalMidi;
    // Correct Pitch reference before and after the drag.
    float source, finalSource;
  };
  bool trackGestureActive = false, trackBoxSelecting = false;
  juce::Point<float> trackGestureStart, trackGestureEnd;
  std::vector<TrackPitchMacroTarget> trackGestureTargets;
  // Track targets fetched once per mouseDown (fetching requests analysis and
  // sets a status message), shared by the empty-canvas test and
  // beginTrackGesture.
  std::optional<std::vector<TrackPitchMacroTarget>> mouseDownTrackTargets;
  std::vector<TrackGestureNote> trackGestureNotes;
  std::vector<const Note*> trackBoxInitialSelection;
  std::vector<juce::Rectangle<float>> trackNoteBounds(const Note&, const Project&) const;
  void syncTrackPreviewSelection();
  InactiveLayerKey inactiveLayerKey;
  juce::Image inactiveLayer;
  InactiveLayerKey pinnedLayerKey;
  juce::Image pinnedLayer;
  std::unique_ptr<ScrollZoomController> scrollZoomController;
  std::unique_ptr<PitchEditor> pitchEditor;
  std::unique_ptr<BoxSelector> boxSelector;
  std::unique_ptr<NoteSplitter> noteSplitter;
  std::unique_ptr<PitchToolHandles> pitchToolHandles;
  std::unique_ptr<PitchToolController> pitchToolController;

  std::unique_ptr<PianoRollInteractionContext> interactionContext;

  PianoRollViewState viewState;
  int &hoveredPitchToolHandle = viewState.hoveredPitchToolHandle;
  Note *hoveredNote = nullptr;
  Button previewButton;
  Button resetButton;
  int previewButtonWidth = 28;
  int previewButtonHeight = 28;
  int resetButtonWidth = 28;
  int resetButtonHeight = 28;
  bool previewPlaybackActive = false;
  int previewPlaybackStartFrame = 0;
  int previewPlaybackEndFrame = 0;
  double previewPlaybackCurrentTime = 0.0;

  float &pixelsPerSecond = viewState.pixelsPerSecond;
  float &pixelsPerSemitone = viewState.pixelsPerSemitone;

  double &cursorTime = viewState.cursorTime;
  bool playbackActive = false;
  double &scrollX = viewState.scrollX;
  double &scrollY = viewState.scrollY;

  // Edit mode
  EditMode &editMode = viewState.editMode;
  juce::MouseCursor splitMouseCursor;
  juce::MouseCursor mergeMouseCursor;
  juce::MouseCursor zoomMouseCursor;

  // View settings
  bool &showDeltaPitch = viewState.showDeltaPitch;
  bool &showBasePitch = viewState.showBasePitch;
  bool &showSegmentsDebug = viewState.showSegmentsDebug;
  bool &showGameValuesDebug = viewState.showGameValuesDebug;
  bool &showNoteFramesDebug = viewState.showNoteFramesDebug;
  bool &showUvInterpolationDebug = viewState.showUvInterpolationDebug;
  bool &showActualF0Debug = viewState.showActualF0Debug;
  bool &showCleanedF0Debug = viewState.showCleanedF0Debug;
  bool &showVocoderF0Debug = viewState.showVocoderF0Debug;
  bool &snapToSemitoneDrag = viewState.snapToSemitoneDrag;
  DragSnapMode &dragSnapMode = viewState.dragSnapMode;
  int &pitchReferenceHz = viewState.pitchReferenceHz;
  TimelineDisplayMode &timelineDisplayMode = viewState.timelineDisplayMode;
  int &timelineBeatNumerator = viewState.timelineBeatNumerator;
  int &timelineBeatDenominator = viewState.timelineBeatDenominator;
  double &timelineTempoBpm = viewState.timelineTempoBpm;
  // D-310: host tempo map snapshot (null = constant-BPM fallback).
  std::shared_ptr<const HostTempoMap> hostTempoMap;
  // D-311/R2: MIDI guide ghosts (read-only overlay under the notes), stored in
  // host seconds; see MidiGuideGhosts.h.
  MidiGuideGhostLayer midiGuideGhosts;
  TimelineGridDivision &timelineGridDivision = viewState.timelineGridDivision;
  bool &timelineSnapCycle = viewState.timelineSnapCycle;
  bool &cycleEditEnabled = viewState.cycleEditEnabled;
  ScaleMode &selectedScaleMode = viewState.selectedScaleMode;
  int &selectedScaleRootNote = viewState.selectedScaleRootNote;
  std::optional<int> &previewScaleRootNote = viewState.previewScaleRootNote;
  std::optional<ScaleMode> &previewScaleMode = viewState.previewScaleMode;

  // Interaction handlers (state machine pattern)
  std::unique_ptr<LoopDragHandler> loopDragHandler_;
  std::unique_ptr<SelectHandler> selectHandler_;
  std::unique_ptr<SplitHandler> splitHandler_;
  std::unique_ptr<PitchDrawingHandler> pitchDrawingHandler_;
  std::unique_ptr<TimingHandler> timingHandler_;
  InteractionHandler *currentHandler_ = nullptr;

  std::unique_ptr<PitchDrawingConfirmationPanel> pitchDrawingConfirmationPanel;

  // Scrollbars
  juce::ScrollBar horizontalScrollBar{false};
  juce::ScrollBar verticalScrollBar{true};
  bool showHorizontalScrollBar = true;

public:
  void invalidateWaveformCache();
  void invalidateBasePitchCache();
  void beginMacroCurvePreview(const std::vector<Note*>& notes);
  void endMacroCurvePreview();
  std::function<std::vector<TrackPitchMacroTarget>()> getTrackEditTargets;
  bool beginTrackGesture(const juce::MouseEvent&);
  void updateTrackGesture(const juce::MouseEvent&);
  void finishTrackGesture();
  void beginInactiveMacroPreview(const std::vector<TrackPitchMacroTarget>& targets);

private:
  // D-180: measurement only (message thread).
  juce::int64 paintCount = 0;
  double lastPaintDurationMs = 0.0;
  double maxPaintDurationMs = 0.0;

  // Mouse drag throttling
  juce::int64 lastDragRepaintTime = 0;
  bool modifierZoomDragActive = false;
  // Option/Alt-drag zooms vertically only (Cmd-drag zooms both axes).
  bool modifierZoomVerticalOnly = false;
  juce::Point<float> modifierZoomLastPosition;
  bool modifierPanDragActive = false;
  juce::Point<float> modifierPanLastPosition;
  bool pianoKeyAuditionMouseDown = false;
  bool middleButtonScrubActive = false;
  // Boton central sobre el lienzo: herramienta de mano (pan mientras se mantiene).
  bool middlePanActive = false;
  juce::Point<float> middlePanLastPosition;
  MainViewRegionPreviewList regionPreviews;
  bool trackViewMode = false;
  double regionPreviewsEndSeconds = 0.0;
  // Set when a click only activated another region (its project was not
  // ready yet), so the rest of that gesture must not edit anything.
  bool swallowGestureUntilMouseUp = false;
  bool toolPillGesture = false;

  // Re-applies the hover-button tooltips after a language change; the menus
  // and handle labels are built on demand, so they need nothing here.
  void refreshLocalisedText();

  // Shared bottom block of the canvas/note menu: undo, redo, select all and
  // the tool strip. `choice` is the 1-based id set by the content.
  void applyCanvasMenuChoice(int choice);

  LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};
