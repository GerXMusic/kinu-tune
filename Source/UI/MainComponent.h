#pragma once

#include "../Audio/EditorController.h"
#include "../Audio/ResampledLoopAudition.h"
#include "IMainView.h"
#include "../Audio/IO/AudioFileManager.h"
#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Undo/UndoActions.h"
#include "CustomMenuBarLookAndFeel.h"
#include "CustomTitleBar.h"
#include "Commands.h"
#include "Components/AppFont.h"
#include "Main/AuditionOutput.h"
#include "Main/MenuHandler.h"
#include "Main/SettingsManager.h"
#include "ParameterPanel.h"
#include "TrackListPanel.h"
#include "PianoRollComponent.h"
#include "PianoRollWorkspaceView.h"
#include "VariAudioInspector.h"
#include "SettingsOverlay.h"
#include "ToolbarComponent.h"
#include "Workspace/WorkspaceComponent.h"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <thread>
#include <vector>

class UiBrightnessEffect;

class AnalysisBackdrop : public juce::Component {
public:
  AnalysisBackdrop() {
    setInterceptsMouseClicks(true, true);
  }

  void paint(juce::Graphics &g) override {
    g.fillAll(juce::Colours::black.withAlpha(0.40f));
  }
};

class AnalysisProgressPopup : public juce::Component {
public:
  void setProgress(double newProgress) {
    progress = juce::jlimit(0.0, 1.0, newProgress);
    repaint();
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF333333u));
    g.fillRoundedRectangle(bounds, 7.0f);

    g.setColour(juce::Colour(0xFFEFEFEFu));
    g.setFont(AppFont::getFont(16.0f));
    g.drawText(TR("progress.analyzing_title"),
               getLocalBounds().withHeight(44).translated(0, 4),
               juce::Justification::centred, false);

    auto bar = juce::Rectangle<float>(29.0f, 53.0f, bounds.getWidth() - 58.0f, 20.0f);
    const auto barRadius = bar.getHeight() * 0.5f;
    g.setColour(juce::Colour(0xFF191717u));
    g.fillRoundedRectangle(bar, barRadius);

    auto fillBounds = bar.reduced(2.0f);
    auto fill = fillBounds.withWidth(fillBounds.getWidth() * static_cast<float>(progress));
    if (fill.getWidth() > 0.0f) {
      g.setColour(juce::Colour(0xFF9A9A9Au));
      g.fillRoundedRectangle(fill, fillBounds.getHeight() * 0.5f);
    }

    g.setColour(juce::Colour(0xFFEFEFEFu));
    g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
    g.drawText(juce::String(static_cast<int>(std::round(progress * 100.0))) + "%",
               bar.toNearestInt(), juce::Justification::centred, false);
  }

private:
  double progress = 0.0;
};

class MainComponent : public juce::Component,
                      public juce::Timer,
                      public juce::KeyListener,
                      public juce::FileDragAndDropTarget,
                      public juce::ApplicationCommandTarget,
                      public IMainView {
public:
  using juce::Component::keyPressed;
  explicit MainComponent(bool enableAudioDevice = true);
  ~MainComponent() override;

  void paint(juce::Graphics &g) override;
  void paintOverChildren(juce::Graphics &g) override;
  void resized() override;

  void timerCallback() override;

  // KeyListener
  bool keyPressed(const juce::KeyPress &key,
                  juce::Component *originatingComponent) override;

  // ApplicationCommandTarget interface
  juce::ApplicationCommandTarget* getNextCommandTarget() override;
  void getAllCommands(juce::Array<juce::CommandID>& commands) override;
  void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
  bool perform(const ApplicationCommandTarget::InvocationInfo& info) override;

  // Mouse handling for window dragging on macOS
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;

  // FileDragAndDropTarget
  bool isInterestedInFileDrag(const juce::StringArray &files) override;
  void filesDropped(const juce::StringArray &files, int x, int y) override;

  // Opens a project or audio file chosen outside the UI (command line, file
  // manager). Ignored in plugin mode and for files that no longer exist.
  void openFileFromPath(const juce::File &file);

  // Plugin mode
  bool isPluginMode() const { return !enableAudioDeviceFlag; }
  juce::Component *getComponent() override { return this; }
  Project *getProject() const override {
    return editorController ? editorController->getProject() : nullptr;
  }
  std::unique_ptr<Project>
  exchangeProject(std::unique_ptr<Project> project) override;
  Vocoder *getVocoder() const override {
    return editorController ? editorController->getVocoder() : nullptr;
  }
  void bindBackendController(EditorController *controller) override;
  void bindUndoManager(PitchUndoManager *manager) override;
  MainViewViewportState getViewportState() const override;
  void restoreViewportState(const MainViewViewportState &state) override;
  bool hasAnalyzedProject() const override;
  void bindRealtimeProcessor(RealtimePitchProcessor &processor) override;
  juce::String serializeProjectJson() const override;
  bool restoreProjectJson(const juce::String &json) override;
  bool restoreProjectSnapshot(const Project &project) override;
  void setStatusMessage(const juce::String &message) override {
    toolbar.setStatusMessage(message);
  }
  void showAnalysisProgress(double progress) override;
  void hideAnalysisProgress() override;
  void finishBackendRender(bool success) override;
  void setTrackPitchMacroTargets(
      std::function<std::vector<TrackPitchMacroTarget>()> callback) override {
    pianoRoll.getTrackEditTargets = callback;
    pianoRollView.getTrackPitchMacroTargets = std::move(callback);
  }
  void setOnProjectDataChanged(std::function<void()> callback) override {
    onProjectDataChanged = std::move(callback);
  }
  void setOnPitchEditFinished(std::function<void()> callback) override {
    onPitchEditFinished = std::move(callback);
  }
  void setOnRequestBackendRender(
      std::function<void(const Project &)> callback) override {
    onRequestBackendRender = std::move(callback);
  }
  // The host preview and audition callbacks go straight to AuditionOutput,
  // the only code allowed to sound them (see there).
  void setOnRequestBackendPreview(
      std::function<void(const Project &, int, int)> callback) override {
    auditionOutput.setStartHostPreview(std::move(callback));
  }
  void setOnStopBackendPreview(std::function<void()> callback) override {
    auditionOutput.setStopHostPreview(std::move(callback));
  }
  void setOnRequestDragAudition(
      std::function<void(const juce::AudioBuffer<float> &, double, bool)> callback) override {
    auditionOutput.setPlayHostLoop(std::move(callback));
  }
  void setOnStopDragAudition(std::function<void()> callback) override {
    auditionOutput.setStopHostLoop(std::move(callback));
  }
  void setOnRequestHostPlayState(
      std::function<void(bool)> callback) override {
    onRequestHostPlayState = std::move(callback);
  }
  void setOnRequestHostStop(std::function<void()> callback) override {
    onRequestHostStop = std::move(callback);
  }
  void setOnRequestHostSeek(
      std::function<void(double)> callback) override {
    onRequestHostSeek = std::move(callback);
  }
  void setOnRequestHostLoopRange(
      std::function<void(double, double, bool, bool)> callback) override {
    onRequestHostLoopRange = std::move(callback);
  }
  void setOnRecordArmChanged(std::function<void(bool)> callback) override {
    onRecordArmChanged = std::move(callback);
  }
  void setRecordControlVisible(bool visible) override {
    toolbar.setRecordControlVisible(visible);
  }
  void updateTrackList(const std::vector<MainViewTrackEntry> &tracks,
                       const juce::String &activeKey) override;
  void setOnTrackSelected(
      std::function<void(const juce::String &)> callback) override {
    trackListPanel.onTrackSelected = std::move(callback);
  }
  void setOnTrackPinChanged(
      std::function<void(const juce::String &, bool)> callback) override {
    trackListPanel.onTrackPinChanged = std::move(callback);
  }
  bool getUseDawTrackColour() const override {
    return trackListPanel.getUseDawTrackColour();
  }
  void setOnTrackColourModeChanged(std::function<void()> callback) override {
    onTrackColourModeChanged = std::move(callback);
  }
  void updateRegionPreviews(MainViewRegionPreviewList previews) override;
  void setTrackViewModeAvailable(bool available) override;
  bool isTrackViewMode() const override {
    return pianoRollView.isTrackViewMode();
  }
  void setOnTrackViewModeChanged(std::function<void(bool)> callback) override {
    onTrackViewModeChanged = std::move(callback);
  }
  void setOnRegionActivationRequested(
      std::function<void(const juce::String &)> callback) override {
    onRegionActivationRequested = std::move(callback);
  }
  void setHostTransportControlAvailable(bool available) override;
  bool isHostTransportControlAvailable() const {
    return hostTransportControlAvailable;
  }
  void updateRecordArmState(bool armed) override {
    toolbar.setRecordArmed(armed);
  }
  void beginLiveRecording(double sampleRate,
                          double timelineOffsetSeconds) override;
  void appendLiveRecordingAudio(
      const juce::AudioBuffer<float> &buffer) override;
  juce::Point<int> getSavedWindowSize() const;

  // Check if ARA mode is active.
  bool isARAModeActive() const;

  // Plugin mode - host audio handling
  void setHostAudio(const juce::AudioBuffer<float> &buffer,
                    double sampleRate,
                    double timelineOffsetSeconds = 0.0) override;
  void updateHostAudioTimelineOffset(double timelineOffsetSeconds) override;
  void clearHostAudio() override;
  void focusTimelineRange(double startSeconds, double endSeconds) override;

  // Plugin mode callbacks
  std::function<void()>
      onProjectDataChanged; // Called when project data is ready or changed
  std::function<void()>
      onPitchEditFinished; // Called when pitch editing is finished
                           // (Melodyne-style: triggers real-time update)
  std::function<void(const Project &)> onRequestBackendRender;
  // Everything that sounds besides playback goes through here; it holds the
  // host preview and audition callbacks (see AuditionOutput).
  AuditionOutput auditionOutput;

  // Plugin mode - request host transport control (optional; only works if host
  // supports it)
  std::function<void(bool shouldPlay)> onRequestHostPlayState;
  std::function<void()> onRequestHostStop;
  std::function<void(double timeInSeconds)> onRequestHostSeek;
  std::function<void(double startSeconds, double endSeconds, bool enabled,
                     bool hasRange)>
      onRequestHostLoopRange;
  std::function<void(bool armed)> onRecordArmChanged;

  // Plugin mode - update playback position from host
  void updatePlaybackPosition(double timeSeconds) override;
  void setTimelineDisplayOffset(double seconds) override;
  void updateHostPlaybackState(bool isPlaying) override;
  void updateHostTimelineState(double bpm, int numerator,
                               int denominator) override;
  void updateHostLoopRange(double startSeconds, double endSeconds,
                           bool enabled, bool hasRange) override;
  void notifyHostStopped() override; // Called when host stops playback
  void triggerResynthesis() override; // Triggered by DAW parameter automation

  // D-180: measurement only (see plan/m2/d180-criterio.md).
  void noteAraSelectionStarted() override;
  void noteAraRegionRefreshDuration(double durationMs) override;
  void noteAraArchiveRestoreDuration(double durationMs) override;
  void noteAraActivationDuration(double durationMs) override;
  void noteAraActivationBreakdown(double cancelMs, double copyMs,
                                  double parseMs, double installMs,
                                  double focusMs, double bindMs,
                                  double totalMs) override;
  void noteAraLayoutStarted() override;
  void noteAraLayoutFinished() override;

private:
  // tests/AuditionHandoffTests.cpp drives the audition hand-offs directly.
  friend struct MainComponentTestAccess;
  void openFile();
  void exportFile();
  void exportMidiFile();
  void play();
  void pause();
  void stop();
  // scrollToCursor=false leaves the view where it is (used when stop/finish
  // rewinds the cursor while Auto Scroll is off).
  void seek(double time, bool scrollToCursor = true);
  void previewNoteRegion(int startFrame, int endFrame);
  void finishPreviewRegion(bool restorePosition);
  void auditionDraggedNote(const Note &note);
  // Mouse up: the feedback stops (VariAudio-style).
  void finishDraggedNoteAudition();
  // Stops any drag audition at once (mouse up, audition switched off,
  // teardown).
  void cancelDraggedNoteAudition();
  // Another audition or a transport command takes over: forget the drag
  // audition without restoring the transport it saved. stopSound also
  // silences it (transport commands); a new audition replaces it instead.
  void abandonDragAudition(bool stopSound);
  void dispatchPendingDragAudition();
  void auditionPianoKey(int midiNote);
  void finishPianoKeyAudition();
  void jumpTransport(bool forward);
  void resynthesizeIncremental(); // Incremental synthesis on edit
  // Engine change: the audio was rendered by the previous engine, so every
  // edited note is rendered again by the new one (Project::markEditedNotesDirty).
  void rerenderEditedNotes();
  // Status line under the engine radios (ParameterPanel::setRenderStatus). Fed
  // from every place a render starts or finishes.
  void showRenderStatus(ParameterPanel::RenderStatus status);
  void showSettings();
  void openRecentFile(const juce::File &file);
  void addRecentFile(const juce::File &file);
  void refreshRecentFilesMenu();

  void onNoteSelected(Note *note);
  void onPitchEdited();
  // Per-note engine (M2 paso 2, D-125). Applies the requested engine to the
  // inspector's targets through the silence rule, as one undo step, and
  // re-renders. Returns how many selected notes the rule refused.
  int applyNoteEngineToSelection(NoteEngine engine);
  int applyNoteEngineToIndices(const std::vector<int> &indices,
                               NoteEngine engine);
  std::optional<NoteEngine> getSelectionNoteEngine() const;
  void onZoomChanged(float pixelsPerSecond);
  void reinterpolateUV(int startFrame,
                       int endFrame); // Re-infer UV regions using FCPE
  void notifyProjectDataChanged();
  // D-174: a restored document carries its own engine; apply it to the panel
  // and to the controllers after the project lands in the canvas.
  void applyProjectSynthesisEngine();
  void fitAnalyzedPitchRangeToView(Project &project);
  // Whole timeline horizontally, the sung range vertically (Shift+F).
  void zoomToFull();
  void applyUiBrightness(double brightnessPercent);
  // Local MCP commands are polled on the message thread, never the audio thread.
  void pollMcpControl();

  void reloadInferenceModels(bool async = false);
  bool isInferenceBusy() const;
  // Keeps the Rendering card's device row in step with the execution provider
  // chosen in Settings - the list is per-provider, and so is the default.
  void refreshRenderDeviceOptions();
  void applyCachedHostLoopRange();
  void checkForUpdatesOnLaunch();
  void showUpdateAvailablePopup(const juce::String &latestVersion,
                                const juce::String &releaseNotes);
  void skipUpdateVersion(const juce::String &version);
  void showAboutPopup();

  void loadAudioFile(const juce::File &file);
  void clearProjectForNewLoad();
  void openProjectFile(const juce::File &file);
  void analyzeAudio();
  void analyzeAudio(
      Project &targetProject,
      const std::function<void(double, const juce::String &)> &onProgress,
      std::function<void()> onComplete = nullptr);
  void segmentIntoNotes();
  void segmentIntoNotes(Project &targetProject);

  void saveProject(bool saveAs = false);
  void restoreProjectPresentation();

  void undo();
  void redo();
  // Shared by undo, redo and the note Restore menu, so all three process a
  // history change identically.
  void afterHistoryChange(bool requiresResynthesis);
  void setEditMode(EditMode mode);
  void setToolGroupEnabled(bool enabled);

  std::unique_ptr<EditorController> ownedEditorController;
  EditorController *editorController = nullptr;
  juce::String mcpInstanceId { juce::Uuid().toString() };
  juce::uint32 mcpLastPollMs = 0;
  juce::uint32 mcpLastHeartbeatMs = 0;
  juce::uint32 mcpRenderGeneration = 0;
  bool mcpRenderPending = false;
  std::unique_ptr<PitchUndoManager> ownedUndoManager;
  PitchUndoManager *undoManager = nullptr;
  std::unique_ptr<juce::ApplicationCommandManager> commandManager;
  bool toolGroupEnabled = true;
  bool previewRegionActive = false;
  double previewRegionEndTime = 0.0;
  double previewRegionReturnTime = 0.0;
  double previewRegionStartTime = 0.0;
  double previewRegionWallClockStartMs = 0.0;
  bool previewRegionWasProjectPlaying = false;
  std::unique_ptr<ResampledLoopAudition> resampledLoopAudition;
  bool dragAuditionEnabled = false;
  bool noteDragAuditionActive = false;
  // Transport or another preview took over during the drag: stay silent
  // until the mouse goes up.
  bool noteDragAuditionSuppressed = false;
  bool noteDragAuditionBackendPreviewStarted = false;
  bool noteDragAuditionPending = false;
  bool noteDragAuditionWasPlaying = false;
  double noteDragAuditionReturnTime = 0.0;
  int noteDragAuditionStartFrame = 0;
  int noteDragAuditionEndFrame = 0;
  juce::int64 noteDragAuditionLastInputMs = 0;
  float pendingNoteDragAuditionShift = 0.0f;
  float pendingNoteDragAuditionMidiNote = 60.0f;
  std::vector<float> pendingNoteDragAuditionSource;
  static constexpr juce::int64 noteDragAuditionDebounceMs = 20;
  // Segment and pitch of the current rendering (compared, never read).
  const Note *lastAuditionedNote = nullptr;
  float lastAuditionedMidi = 0.0f;

  // New modular components
  std::unique_ptr<AudioFileManager> fileManager;
  std::unique_ptr<MenuHandler> menuHandler;
  std::unique_ptr<SettingsManager> settingsManager;

  const bool enableAudioDeviceFlag;

  CustomMenuBarLookAndFeel menuBarLookAndFeel;
  juce::MenuBarComponent menuBar;
#if JUCE_MAC
  juce::PopupMenu macExtraAppleMenuItems;
#endif
  ToolbarComponent toolbar;
  WorkspaceComponent workspace;
  PianoRollComponent pianoRoll;
  PianoRollWorkspaceView pianoRollView;
  VariAudioInspector variAudioInspector; // left column, VariAudio-style
  ParameterPanel parameterPanel;
  TrackListPanel trackListPanel; // content of the left side panel
  std::function<void()> onTrackColourModeChanged;
  std::unique_ptr<UiBrightnessEffect> uiBrightnessEffect;
  AnalysisBackdrop analysisBackdrop;
  AnalysisProgressPopup analysisProgressPopup;

  // Shared across editors so tooltips can extend beyond their native windows.
  std::shared_ptr<juce::TooltipWindow> tooltipWindow;

  std::unique_ptr<SettingsOverlay> settingsOverlay;

  std::unique_ptr<juce::FileChooser> fileChooser;
  juce::StringArray recentFiles;

  bool isPlaying = false;
  bool hostTransportControlAvailable = true;
  std::function<void(const juce::String &)> onRegionActivationRequested;
  std::function<void(bool)> onTrackViewModeChanged;
  // True while a canvas click is switching the active region. The clicked
  // region is already on screen, so its project must not refit the view.
  bool regionActivationFromCanvas = false;
  bool liveRecordingActive = false;
  bool pianoKeyAuditionActive = false;
  bool pianoKeyAuditionWasPlaying = false;
  double pianoKeyAuditionReturnTime = 0.0;

  // Sync flag to prevent infinite loops
  bool isSyncingZoom = false;

  // The update worker must finish before networking shuts down.
  std::unique_ptr<juce::WebInputStream> updateRequest;
  std::thread updateCheckThread;
  std::atomic<bool> stoppingUpdateCheck{false};

  // Async load state
  std::atomic<bool> isLoadingAudio{false};
  std::atomic<double> loadingProgress{0.0};
  juce::CriticalSection loadingMessageLock;
  juce::String loadingMessage;
  juce::String lastLoadingMessage;

  // Async render state (plugin mode) is managed by EditorController
  // Incremental synthesis coalescing
  std::atomic<bool> pendingIncrementalResynth{false};

  // Cursor update throttling
  std::atomic<double> pendingCursorTime{0.0};
  std::atomic<bool> hasPendingCursorUpdate{false};

  // D-180: measurement only. All fields are written and read on the message
  // thread; the MCP poll and the 60 Hz timer live there. No value changes what
  // the view or the renderer does.
  struct UiLatencyTrace {
    double selectionStartedMs = 0.0;
    double selectionBoundMs = 0.0;
    double selectionFirstPaintMs = 0.0;
    juce::int64 selectionPaintBase = 0;

    double playStateMs = 0.0;
    double playFirstPaintMs = 0.0;
    juce::int64 playPaintBase = 0;
    double playCursorMs = 0.0;
    double playCursorFirstPaintMs = 0.0;
    juce::int64 playCursorPaintBase = 0;
    bool playCursorVisible = false;

    double stoppedSeekMs = 0.0;
    double stoppedSeekTarget = 0.0;
    double stoppedSeekAppliedMs = 0.0;
    double stoppedSeekFirstPaintMs = 0.0;
    juce::int64 stoppedSeekPaintBase = 0;
    bool stoppedSeekCursorVisible = false;

    double lastTimerMs = 0.0;
    double maxTimerGapMs = 0.0;
    double lastExchangeMs = 0.0;
    double maxExchangeMs = 0.0;
    double lastRegionRefreshMs = 0.0;
    double maxRegionRefreshMs = 0.0;
    double lastArchiveRestoreMs = 0.0;
    double maxArchiveRestoreMs = 0.0;
    double lastActivationMs = 0.0;
    double maxActivationMs = 0.0;
    double lastActivationCancelMs = 0.0;
    double lastActivationCopyMs = 0.0;
    double lastActivationParseMs = 0.0;
    double lastActivationInstallMs = 0.0;
    double lastActivationFocusMs = 0.0;
    double lastActivationBindMs = 0.0;
    double lastActivationTotalMs = 0.0;

    int resizeCount = 0;
    double lastResizeAtMs = 0.0;
    double lastResizeScrollBefore = 0.0;
    double lastResizeScrollAfter = 0.0;
    double lastResizeCursor = 0.0;
    bool lastResizeCursorVisibleBefore = false;
    bool lastResizeCursorVisibleAfter = false;
    int lastResizeWidth = 0;
    int lastResizeHeight = 0;
    double firstResizeScrollBefore = 0.0;
    double firstResizeScrollAfter = 0.0;
    bool firstResizeCursorVisibleBefore = false;
    bool firstResizeCursorVisibleAfter = false;
    int firstResizeWidth = 0;
    double firstResizePps = 0.0;
    double firstResizeDuration = 0.0;
    int firstResizeVisibleWidth = 0;
    double lastLayoutScrollBefore = 0.0;
    double lastLayoutScrollAfter = 0.0;
    double lastLayoutPpsBefore = 0.0;
    double lastLayoutDurationBefore = 0.0;
    int lastLayoutVisibleWidthBefore = 0;
    int layoutCount = 0;
    bool layoutScrollGuardActive = false;
    double layoutScrollGuardScroll = 0.0;
    double layoutScrollGuardUntilMs = 0.0;
    bool layoutScrollGuardCursorVisible = false;
    int layoutScrollGuardRestoreCount = 0;
    double lastObservedScrollX = 0.0;
    int scrollResetCount = 0;
    double scrollResetFrom = 0.0;
    double scrollResetTo = 0.0;
    double scrollResetAtMs = 0.0;
    double lastCallbackScrollX = 0.0;
    int callbackResetCount = 0;
    double callbackResetFrom = 0.0;
    double callbackResetTo = 0.0;
    double callbackResetAtMs = 0.0;

    double lastFocusAtMs = 0.0;
    double lastFocusSpanStart = 0.0;
    double lastFocusScrollX = 0.0;
    double lastFocusCursorTime = 0.0;
    bool lastFocusCursorWasVisible = false;
    bool lastFocusCursorVisibleAfter = false;
    bool lastFocusCursorRestored = false;
    int focusRestoreCount = 0;

    double selectionScrollAtBound = 0.0;
    double selectionScrollAtPaint = 0.0;
    bool selectionCursorVisibleAtPaint = false;
  };
  UiLatencyTrace latencyTrace;
  // A host seek arrived while the transport is stopped: the view must follow
  // it without waiting for playback (D-180 A).
  bool pendingStoppedHostSeek = false;

  double nowMs() const;
  bool cursorIsVisible(double timeSeconds) const;
  void updateUiLatencyPaintTrace();

  bool hasCachedHostTimelineState = false;
  double cachedHostTempoBpm = 120.0;
  int cachedHostBeatNumerator = 4;
  int cachedHostBeatDenominator = 4;

  bool hasCachedHostLoopRange = false;
  double cachedHostLoopStartSeconds = 0.0;
  double cachedHostLoopEndSeconds = 0.0;
  bool cachedHostLoopEnabled = false;
  bool cachedHostLoopHasRange = false;
  double suppressPlaybackFollowUntilMs = 0.0;
  // D-176: the initial ARA selection arrives before the editor is sized. Keep
  // the span and apply it once the canvas has a visible width.
  bool hasPendingTimelineFocus = false;
  bool hasPendingPitchFit = false;
  bool pendingViewRetryScheduled = false;
  int pendingViewRetries = 0;
  double pendingTimelineFocusStart = 0.0;
  double pendingTimelineFocusEnd = 0.0;
  juce::int64 lastCursorUpdateTime = 0;

#if JUCE_MAC
  juce::ComponentDragger dragger;
#endif

  // Rebuilds the menu bar and command names after a language change; the
  // child components refresh themselves through their own watchers.
  void refreshLocalisedText();

  // Declared last so it is torn down before what it refreshes.
  LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
