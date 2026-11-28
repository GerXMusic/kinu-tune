#include "Main/DeferredRenderQueue.h"
#pragma once

#include <unordered_map>

#include "../Audio/EditorController.h"
#include "../Audio/ResampledLoopAudition.h"
#include "IMainView.h"
#include "../Audio/IO/AudioFileManager.h"
#include "../JuceHeader.h"
#include "../Models/DocumentMidiGuides.h"
#include "../Models/MidiGuideLive.h"
#include "../Models/Project.h"
#include "../Undo/UndoActions.h"
#include "CustomMenuBarLookAndFeel.h"
#include "CustomTitleBar.h"
#include "Commands.h"
#include "Components/AppFont.h"
#include "Main/AuditionOutput.h"
#include "Main/MenuHandler.h"
#include "Main/RenderState.h"
#include "Main/SettingsManager.h"
#include "ParameterPanel.h"
#include "TrackListPanel.h"
#include "EnginePanel.h"
#include "MidiGuidePanel.h"
#include "HarmonyPanel.h"
#include "GuiTypes.h"
#include "PianoRollComponent.h"
#include "PianoRollWorkspaceView.h"
#include "VariAudioInspector.h"
#include "SettingsOverlay.h"
#include "ToolbarComponent.h"
#include "Workspace/WorkspaceComponent.h"

#include <atomic>
#include <array>
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
  void updateDocumentNoteIndex(DocumentNoteIndexPtr index) override;
  void releaseBorrowedProject(const Project *project) override;
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
  void updateHostTempoMap(
      std::shared_ptr<const HostTempoMap> tempoMap) override;

  // D-311: MIDI guides. The drawer drives these; MainComponent owns the state
  // (which guide is selected, whether the ghosts show). R5b/D-356: the guides
  // themselves are the DOCUMENT's (midiGuideStore), not the active Project's,
  // so changing item neither loses them nor shows another take's list. Apply
  // still edits the active item through the normal note-edit path (NotePitch +
  // undo + render); applying to every item is R8/R9.
  void importMidiGuideFromFile();
  void applyMidiGuide();
  void removeSelectedMidiGuide();
  void setMidiGuideSelected(int index);
  void setMidiGuideGhostsVisible(bool visible);
  // V4c (D-378): the "Show guide" button. Pure plan + public view setters:
  // scroll horizontal minimo al inicio de la guia y ajuste vertical que la
  // incluye. No dirty, no undo, no render. Never called on open/reopen.
  void revealMidiGuideInView();  // Rebuilds the drawer list, the ghost layer and the published snapshot from
  // the document store + tempo map. Never reads the active Project's copy.
  void refreshMidiGuideUi();
  // Materializes any guide_*.json the REAPER script left in the config folder
  // (same source_id replaces its own guide). D-314: files, never sockets.
  void syncLiveMidiGuides();
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
  void noteAraEditorOpened(double hostCursorSeconds) override;

private:
  // tests/AuditionHandoffTests.cpp drives the audition hand-offs directly.
  friend struct MainComponentTestAccess;
  // R9 (D-362): Apply with an explicit scope ("track" = SCOPE=A, the default;
  // "active" = R1). Returns the JSON report the local channel publishes.
  juce::var applyMidiGuideWithScope(const juce::String &scope);
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
  // P1b (render al Play): an edit, an undo/redo, an engine change, an MCP
  // command or DAW automation only marks. Nothing renders until Play.
  void markRenderDirty();
  // D-249 (politica A): soltar la edicion agenda el render. markRenderDirty()
  // agenda siempre (la regla D-247 pasa a ser estructural); el servicio del
  // timer lo coalesce y lo lanza pasado el debounce, si el transporte no esta
  // ya esperando un flush.
  void serviceEditRender();
  double editRenderRequestedAtMs = 0.0;
  // D-285: red por si un cierre se pierde igual. Suelta la cadena, re-arma un
  // intento (uno por revision) y lo registra. Se sirve desde el timer.
  void rescueStalledRender();
  // D-287: el audio publicado tiene que ser el de la ultima revision. Si un
  // pase se pierde (medido en vivo: pass=44 < dirty=47, todo en reposo y el
  // buffer aun del motor anterior), se vuelve a pedir el render de las notas
  // editadas, una vez por revision. Sin esto el editor puede quedarse sonando
  // un motor que ya no esta en el modelo y decirlo `Published`/`quiescent`.
  void catchUpStaleAudio();
  // D-285/3 (corregido en D-286): si la BASE quedo desfasada de las notas (se
  // ve la curva en otro sitio), se reconstruye la base desde las notas al
  // cargar. Solo al cargar, y una vez por carga. El contador cuenta esas
  // reparaciones y se publica en el heartbeat (`curve_repair_attempts`).
  int curveRepairAttempts = 0;
  // D-266 (T6): un unico estado derivado para el render de la UI. Agrupa
  // editRenderScheduled, editRenderInFlight, mcpRenderPending, flushChainActive
  // y deferredRenderPending con el mismo significado y la misma vida (D-261),
  // y anade la revision/rango/generacion del pase en vuelo.
  RenderState renderState;
  // P1b: renders the dirty blocks, one per pass, and only then starts the
  // transport. The cursor waits for the last block.
  void flushDirtyBlocks();
  // P1b fase 3: borde de arranque del Play del host. Devuelve true si el
  // transporte se retiene para renderizar y se reanuda al terminar la cadena.
  bool flushDirtyBlocksForHostPlay() override;
  bool isFlushChainActive() const override { return renderState.flushChainActive; }
  // P1b fase 6: cola de bloques pendientes. El barrido se dibuja desde aqui.
  DeferredRenderQueue deferredRenders;
  void pushSweepState();
  bool playAfterFlush = false;
  // D-227: an edit made while a render is in flight must not be swallowed by
  // that render's completion. markRenderDirty bumps the revision; the flush
  // records the value it requested and re-renders instead of clearing when it
  // no longer matches (a stale render published and the dirty state wiped is
  // the "sometimes it does not smooth" report).
  std::uint64_t dirtyRevision = 0;
  std::uint64_t flushRenderRevision = 0;
  // D-287: revision del ultimo pase que cerro con exito y con instantanea: la
  // que el audio publicado representa. `publishedRevision < dirtyRevision` =
  // el audio va por detras del modelo (medido en vivo: 44 < 47 con el buffer
  // aun del motor anterior y el estado diciendo `Published`).
  std::uint64_t publishedRevision = 0;
  std::uint64_t lastCatchUpRevision = 0;
  // D-290: tope de pases por revision en una cadena. Un cierre que vuelve a
  // pedir pase sin que nada haya avanzado no puede re-lanzarse sin fin: el Play
  // se queda retenido y el editor parece colgado (medido en vivo: phase
  // Rendering, generation subiendo, el transporte sin arrancar). Agotado el
  // tope se suelta la cadena y el transporte suena con el ultimo publicado.
  int chainPassesForRevision = 0;
  std::uint64_t chainPassesRevision = 0;
  static constexpr int kMaxChainPassesPerRevision = 8;
  // Tope global de la cadena: aunque cada pase suba la revision (por ejemplo
  // una edicion que se re-marca sola), la cadena no puede durar mas de esto.
  int chainPassesTotal = 0;
  static constexpr int kMaxChainPassesTotal = 32;
  // D-285: presupuesto de reintento tras un cierre fallido, por revision.
  std::uint64_t lastRetryRevision = 0;
  // D-285/2: cuantos reintentos lleva esta revision (para no girar sin fin) y
  // la espera entre ellos. Un cierre fallido no puede dejar el trabajo sin
  // nadie que lo intente: se agenda y, agotados los intentos, se deja pendiente
  // hasta la siguiente edicion o Play (que reinician el presupuesto).
  int retryAttemptsForRevision = 0;
  static constexpr double kRetryBackoffMs = 1500.0;
  static constexpr int kMaxRetryAttempts = 3;
  // D-254: lo que el render en vuelo incluyo, por revision. El cierre limpia
  // solo eso (Project::clearDirtyForPass); una edicion que llega despues
  // conserva su dirty y pide su propio pase. Sustituye el barrido global
  // (clearAllDirty), que borraba tambien lo que el render no habia visto.
  std::vector<std::pair<int, std::uint64_t>> flushDirtyNoteSnapshot;
  // D-292: cuantas notas habia al capturar la instantanea. Si la estructura
  // cambio (split/merge) antes del cierre, el indice ya no identifica la misma
  // nota y la instantanea se descarta: el trabajo queda sucio y se reconstruye
  // el plan en la cadena siguiente.
  int flushSnapshotNoteCount = 0;
  int flushF0SnapshotStart = -1;
  int flushF0SnapshotEnd = -1;
  std::uint64_t flushF0SnapshotRevision = 0;
  bool flushSnapshotValid = false;
  double lastHostPlaybackSeconds = 0.0;
  double flushResumePosition = 0.0;
  // P1b fase 4: el guardado espera al flush y se reanuda cuando este termina.
  bool resumeSaveAfterFlush = false;
  bool saveAsAfterFlush = false;
  // Engine change: the audio was rendered by the previous engine, so every
  // edited note is rendered again by the new one (Project::markEditedNotesDirty).
  void rerenderEditedNotes();
  // D-279: one path for the inspector button, the canvas menu and the
  // Melodyne keyboard commands.
  void applyCorrectPitchMacro();
  void showQuantizeTimeMacroMenu(juce::Component* anchor = nullptr,
                                 juce::Point<int> screenPoint = {});
  void resetSelection(PianoRollComponent::NoteRestoreMode mode);
  int lastQuantizeStrengthPercent = 100;
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
  // D-267 (S5): conmuta el bypass del documento por el mismo camino que el
  // boton del rail: estado, dirty, render y publicacion.
  void applyBypassProcessing(bool bypassed);
  std::optional<NoteEngine> getSelectionNoteEngine() const;
  // D-258: resumen del ultimo cambio de motor por bloque, para la UI y el canal
  // de control (cuantas notas se sobrescriben y cuales quedan fuera del rango
  // de beneficio del Vocoder).
  int lastBlockEngineOverwritten = 0;
  std::vector<int> lastBlockEngineOutOfRange;
  // D-267: indices de las notas del bloque cuyo motor almacenado cambia (la
  // "lista de sobrescritas" del aviso) y el texto del aviso no modal.
  std::vector<int> lastBlockEngineOverwrittenIndices;
  // D-258: aviso del ultimo cambio de motor por bloque (texto para la UI).
  // No se usa un dialogo modal dentro del plugin: el reporter de REAPER
  // convirtio ese callback en un cierre (c000041d).
  juce::String lastEngineBlockMessage;
  // D-185: note index -> engine that the last gesture asked for and D-129
  // refused. Session state only; the stored engine is already the block's.
  std::unordered_map<int, NoteEngine> lastRefusedEngineRequests;
  void onZoomChanged(float pixelsPerSecond);
  void reinterpolateUV(int startFrame,
                       int endFrame); // Re-infer UV regions using FCPE
  void notifyProjectDataChanged();
  // D-174: a restored document carries its own engine; apply it to the panel
  // and to the controllers after the project lands in the canvas.
  void applyProjectSynthesisEngine();
  void fitAnalyzedPitchRangeToView(Project &project);
  // V4c: the project-explicit path, so tests and the button share one code.
  void revealMidiGuideInView(Project &project);
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
  // D-266: la generacion del render vive en renderState.generation.
  std::unique_ptr<PitchUndoManager> ownedUndoManager;
  // D-267 (T2): canal de medida. `probe_audio` guarda el tramo medido para que
  // la siguiente llamada pueda diffear contra el (audio viejo vs nuevo).
  std::vector<float> lastProbeSamples;
  int lastProbeFirstSample = 0;  PitchUndoManager *undoManager = nullptr;
  std::unique_ptr<juce::ApplicationCommandManager> commandManager;
  bool toolGroupEnabled = true;
  bool previewRegionActive = false;
  // V6: el aviso de modelos GAME vive en el rail. Guardamos su texto para no
  // borrar un aviso ajeno (p. ej. uno de motor) cuando la comprobacion vuelve
  // a decir "ya hay modelos".
  juce::String gameModelsNoticeText;
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
  EnginePanel enginePanel;       // rail drawer: project and block engine
  MidiGuidePanel midiGuidePanel; // rail drawer: MIDI guides (placeholder)
  HarmonyPanel railHarmonyPanel; // rail drawer: harmony macro
  EditorUiMode editorUiMode = EditorUiMode::Basic;
  void setEditorUiMode(EditorUiMode mode);
  void refreshEditorChrome();
  std::function<void()> onTrackColourModeChanged;
  std::unique_ptr<UiBrightnessEffect> uiBrightnessEffect;
  AnalysisBackdrop analysisBackdrop;
  AnalysisProgressPopup analysisProgressPopup;

  // Shared across editors so tooltips can extend beyond their native windows.
  std::shared_ptr<juce::TooltipWindow> tooltipWindow;

  std::unique_ptr<SettingsOverlay> settingsOverlay;

  std::unique_ptr<juce::FileChooser> fileChooser;
  std::unique_ptr<juce::FileChooser> midiGuideFileChooser;
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

    // D-180-R3/R4: one sample per paint observed by the 60 Hz timer. The
    // intermediate frame of a track switch lasts ~500 ms, so this records it.
    struct PaintSample {
      double atMs = 0.0;
      double scrollX = 0.0;
      double cursorTime = 0.0;
      int visibleWidth = 0;
      double pixelsPerSecond = 0.0;
      bool cursorVisible = false;
      bool playbackActive = false;
      juce::int64 paintCount = 0;
      int projectNotes = 0;
    };
    static constexpr int kPaintHistorySize = 96;
    std::array<PaintSample, kPaintHistorySize> paintHistory{};
    int paintHistoryCount = 0;
    int paintHistoryNext = 0;
    juce::int64 lastSampledPaintCount = 0;

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
  // D-180-R3: the next incoming project (editor open) should reveal the cursor
  // before its first paint.
  bool pendingOpenCursorReveal = false;

  double nowMs() const;
  bool cursorIsVisible(double timeSeconds) const;
  void updateUiLatencyPaintTrace();

  bool hasCachedHostTimelineState = false;
  double cachedHostTempoBpm = 120.0;
  int cachedHostBeatNumerator = 4;
  int cachedHostBeatDenominator = 4;
  // D-310: the host tempo map snapshot. Null means "no usable host map"; the
  // cached BPM above is then the fallback, exactly as before.
  std::shared_ptr<const HostTempoMap> hostTempoMap;

  // N1 (D-340): document-level index of every note of the track, published by
  // the plug-in (points at the real Note objects; this view never copies or
  // edits them). Null in clip mode or outside ARA.
  DocumentNoteIndexPtr documentNoteIndex;

  // N2 (D-340): the document selection the index enumerates. Shared with the
  // piano roll, which writes it from the marquee, shift-click and select-all;
  // Note::selected is derived from it and notes_debug publishes it.
  std::shared_ptr<DocumentNoteSelection> documentNoteSelection =
      std::make_shared<DocumentNoteSelection>();

  // D-311: MIDI guides. R5b/D-356: the store and the snapshot are the document
  // truth; the ghosts are host seconds and the painter re-maps them.
  // R5d/D-358: the store belongs to the document (PitchNetDocumentController)
  // and this view only borrows it, so closing and reopening the editor in the
  // same process keeps the guides. Without a document (standalone / non-ARA)
  // the local store backs the UI.
  DocumentMidiGuideStore localMidiGuideStore;
  DocumentMidiGuideStore *midiGuideStore = &localMidiGuideStore;
  DocumentMidiGuidesPtr documentMidiGuides;
  // IMainView: attach/detach the document's store (R5d/D-358).
  void setMidiGuideStore(DocumentMidiGuideStore *store) override;
  // IMainView: the document's store changed outside the view (V3/D-374). Only
  // republishes; never writes to the store.
  void midiGuidesChanged() override;
  // One-time migration of the per-Project "midiGuides" cache and the write-back
  // of the compatibility copy after a change.
  void importLegacyMidiGuides();
  void writeMidiGuideCompatCache();
  int selectedMidiGuide = -1;
  bool showMidiGuideGhosts = true;
  // V4c-b: span of the SELECTED guide, filled from the guide model on every
  // refresh no matter the ghosts tick. The "Show guide" button must not depend
  // on the ghost layer: with the tick off that layer is empty by design.
  bool hasMidiGuideSpan = false;
  double midiGuideHostStart = 0.0;
  double midiGuideHostEnd = 0.0;
  float midiGuidePitchMin = 0.0f;
  float midiGuidePitchMax = 0.0f;
  // V4c-c: last "Show guide" plan, published by guide_debug so a real press can
  // be told apart from "no change was needed" without guessing.
  int midiGuideRevealCount = 0;
  bool midiGuideRevealFromModel = false;
  bool midiGuideRevealScrollRequested = false;
  double midiGuideRevealScrollX = 0.0;
  float midiGuideRevealPitchMin = 0.0f;
  float midiGuideRevealPitchMax = -1.0f;
  // R10/D-363: content hash of the guide as it was when the user last applied
  // it. Empty = never applied, and then the "changed" warning is not shown.
  juce::String lastAppliedMidiGuideHash;
  // mtime+size cache for the live folder; a poll that changes nothing does not
  // re-parse a single file.
  midiGuideLive::LiveDirectoryScanner liveGuideScanner;

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
