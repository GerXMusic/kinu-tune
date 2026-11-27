#include "PluginEditor.h"
#include "HostCompatibility.h"
#include "../Models/Project.h"
#include "../Models/DocumentNoteIndex.h"
#include "../Models/DocumentNoteSelection.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Constants.h"
#include "../Undo/ContextUndoAction.h"
#include "../Undo/PitchUndoManager.h"

#include <algorithm>
#include <cmath>

#if JucePlugin_Enable_ARA
#include "ARADocumentController.h"
#include "ARATempoMapReader.h"
#include "AraSelectionPolicy.h"
#include "AraSourceIdentity.h"
#include "PitchNetAudioModification.h"
#include "SingleWindowPolicy.h"
#include "../Models/TrackIdentity.h"
#include "../UI/CursorFollowPolicy.h"
#endif

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace
{
#if JucePlugin_Enable_ARA
// How often the Regions card re-reads the host's region list.
constexpr int kRegionListRefreshHz = 8;
juce::String araTrackKey(juce::ARARegionSequence*);

// D-178: open editors in construction order. A new editor requests the close
// of the previous host window so only one TuneX window remains.
std::vector<PitchNetAudioProcessorEditor *> openAraEditors;

void requestClosePreviousEditorWindow(PitchNetAudioProcessorEditor *previous,
                                      PitchNetAudioProcessorEditor *current) {
  if (previous == nullptr || current == nullptr)
    return;
  if (std::find(openAraEditors.begin(), openAraEditors.end(), previous) ==
      openAraEditors.end())
    return;

  auto *previousTop = previous->getTopLevelComponent();
  auto *currentTop = current->getTopLevelComponent();
  const bool sameTopLevel =
      previousTop != nullptr && previousTop == currentTop;
  if (!pitchnet::shouldClosePreviousEditor(
          static_cast<int>(openAraEditors.size()), sameTopLevel))
    return;

#if JUCE_WINDOWS
  if (previousTop != nullptr) {
    HWND hwnd = nullptr;
    if (auto *peer = previousTop->getPeer())
      hwnd = reinterpret_cast<HWND>(peer->getNativeHandle());
    if (hwnd == nullptr)
      hwnd = reinterpret_cast<HWND>(previousTop->getWindowHandle());
    if (hwnd != nullptr) {
      HWND root = GetAncestor(hwnd, GA_ROOT);
      ARA_DIAG("D178 close previous hwnd=" + juce::String::toHexString(
                   static_cast<juce::int64>(reinterpret_cast<juce::pointer_sized_int>(hwnd))) +
               " root=" + juce::String::toHexString(
                   static_cast<juce::int64>(reinterpret_cast<juce::pointer_sized_int>(root))) +
               " sameTop=" + (sameTopLevel ? "1" : "0") +
               " editors=" + juce::String(static_cast<int>(openAraEditors.size())));
      PostMessage(root != nullptr ? root : hwnd, WM_CLOSE, 0, 0);
    }
  }
#else
  juce::ignoreUnused(previousTop);
#endif
}
#endif

#if JucePlugin_Enable_ARA
// One item's window of its modification's Project, as the drawing data the
// canvas and the overview place on the host timeline.
//
// N10 (D-350): the preview BORROWS the live Project. No window Project is
// built and no note is copied, so the document index, the drawing and the
// hit-test all read the same Note objects. The only thing copied is the item's
// audio, shared by pointer for the background waveform and the thumbnail; the
// notes' inline waveform is read from the live Project.
MainViewRegionPreview buildRegionPreview(Project &project, double regionStart,
                                         double regionEnd,
                                         double startInModificationSeconds,
                                         const DocumentNoteRegion *indexRegion) {
  MainViewRegionPreview preview;
  preview.hasContent = true;
  preview.project = &project;

  const auto &audioData = project.getAudioData();
  // The source window is the one the index published (ARA's start/end in
  // modification samples) so the drawn window and the indexed notes cannot
  // disagree. Without an index region (a pinned clip of another track) it is
  // the region's own span.
  const double sourceStart =
      indexRegion != nullptr ? indexRegion->sourceStartSeconds
                             : std::max(0.0, startInModificationSeconds);
  const double sourceEnd =
      indexRegion != nullptr
          ? indexRegion->sourceEndSeconds
          : sourceStart + std::max(0.0, regionEnd - regionStart);
  preview.sourceStartSeconds = std::max(0.0, sourceStart);
  preview.sourceEndSeconds = std::max(preview.sourceStartSeconds, sourceEnd);
  // Host time of the Project's zero: the one offset the drawing and the
  // hit-test place this item's notes with.
  preview.projectHostOffsetSeconds =
      indexRegion != nullptr ? indexRegion->offsetSeconds
                             : regionStart - preview.sourceStartSeconds;

  const auto &waveform = audioData.waveform;
  const int numSamples = waveform.getNumSamples();
  const double sampleRate = static_cast<double>(audioData.sampleRate);
  if (numSamples > 0 && sampleRate > 0.0 && waveform.getNumChannels() > 0) {
    const int firstSample = juce::jlimit(
        0, numSamples,
        static_cast<int>(std::floor(preview.sourceStartSeconds * sampleRate)));
    const int lastSample = juce::jlimit(
        firstSample, numSamples,
        static_cast<int>(std::ceil(preview.sourceEndSeconds * sampleRate)));
    if (lastSample > firstSample) {
      const float *data = waveform.getReadPointer(0);
      preview.waveformSamples = std::make_shared<const std::vector<float>>(
          data + firstSample, data + lastSample);
      preview.waveformSampleRate = sampleRate;
      preview.waveformStartSeconds =
          firstSample / sampleRate + preview.projectHostOffsetSeconds;
    }
  }

  return preview;
}

// N1 (D-340): the audio source (file) an item comes from. Items of the same
// take share it, so it groups the index by source.
juce::String araSourceKey(juce::ARAPlaybackRegion &region) {
  auto *modification = region.getAudioModification();
  auto *source = modification != nullptr ? modification->getAudioSource() : nullptr;
  if (source == nullptr)
    return {};
  if (const char *name = source->getName()) {
    const juce::String text = juce::String::fromUTF8(name).trim();
    if (text.isNotEmpty())
      return text;
  }
  return "source " + juce::String::toHexString(static_cast<juce::int64>(
                        reinterpret_cast<std::uintptr_t>(source)));
}

// The item's window of its modification, in modification seconds. ARA's own
// start/end, not the host duration: a trimmed or stretched clip has one and the
// other disagree, and the notes belong to the modification window.
struct AraRegionSourceWindow {
  double startSeconds = 0.0;
  double endSeconds = 0.0;
};

AraRegionSourceWindow araRegionSourceWindow(juce::ARAPlaybackRegion &region) {
  AraRegionSourceWindow window;
  auto *modification = region.getAudioModification();
  auto *source = modification != nullptr ? modification->getAudioSource() : nullptr;
  const double sampleRate = source != nullptr ? source->getSampleRate() : 0.0;
  if (sampleRate <= 0.0) {
    window.endSeconds =
        window.startSeconds + std::max(0.0, region.getDurationInPlaybackTime());
    return window;
  }
  window.startSeconds = static_cast<double>(
      region.getStartInAudioModificationSamples()) / sampleRate;
  window.endSeconds = static_cast<double>(
      region.getEndInAudioModificationSamples()) / sampleRate;
  if (window.endSeconds <= window.startSeconds)
    window.endSeconds =
        window.startSeconds + std::max(0.0, region.getDurationInPlaybackTime());
  return window;
}

const DocumentNoteRegion *findIndexRegion(const DocumentNoteIndex *index,
                                          const juce::String &key) {
  if (index == nullptr)
    return nullptr;
  for (const auto &region : index->regions)
    if (region.key == key)
      return &region;
  return nullptr;
}
#endif

bool isLunaHostProcess()
{
#if JUCE_WINDOWS
  const auto host = juce::File::getSpecialLocation(
      juce::File::hostApplicationPath);
  return host.getFileNameWithoutExtension().equalsIgnoreCase("LUNA");
#else
  return false;
#endif
}


} // namespace

PitchNetAudioProcessorEditor::PitchNetAudioProcessorEditor(
    PitchNetAudioProcessor &p)
    : AudioProcessorEditor(&p), audioProcessor(p)
#if JucePlugin_Enable_ARA
      ,
      AudioProcessorEditorARAExtension(&p)
#endif
{
#if JucePlugin_Enable_ARA
  PitchNetAudioProcessorEditor *previousEditor =
      openAraEditors.empty() ? nullptr : openAraEditors.back();
  openAraEditors.push_back(this);
#endif
  // Fonts must exist before MainComponent constructs labels that retain them.
  initializeUiResources();
  mainView = createMainView(false);

  // Enable keyboard focus for the editor and the main view. This is required so
  // that when the host forwards a key event (see the IPlugView::onKeyDown patch
  // in juce_audio_plugin_client_VST3.cpp), MainComponent is the focused
  // component and its command-key mappings sit in the dispatch chain.
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);

  addAndMakeVisible(*mainView->getComponent());
  mainView->getComponent()->setWantsKeyboardFocus(true);
  mainView->getComponent()->setMouseClickGrabsKeyboardFocus(true);
  audioProcessor.setMainComponent(mainView.get());
  addMouseListener(this, true);

#if JucePlugin_Enable_ARA
  setupARAMode();
#else
  setupNonARAMode();
#endif

  setupCallbacks();

  auto size = audioProcessor.getLastEditorSize();
  if (auto *project = mainView->getProject();
      project && project->uiWidth > 0 && project->uiHeight > 0)
    size = {project->uiWidth, project->uiHeight};
  if (size.x <= 0 || size.y <= 0)
    size = getDefaultMainViewSize(this);
  setSize(size.x, size.y);
  setResizable(true, true);

  // Grab keyboard focus once the host has actually shown the editor.
  requestMainViewKeyboardFocusAsync();

#if JucePlugin_Enable_ARA
  if (previousEditor != nullptr) {
    juce::Component::SafePointer<PitchNetAudioProcessorEditor> safeThis(this);
    juce::Timer::callAfterDelay(120, [previousEditor, safeThis]() {
      if (safeThis != nullptr)
        requestClosePreviousEditorWindow(previousEditor,
                                         safeThis.getComponent());
    });
  }

  // D-178: ignore the stale host selection for a short grace period while the
  // editor-renderer binding from setupARAMode() settles.
  editorReadyTimeMs = juce::Time::getMillisecondCounterHiRes() + 600.0;
#endif
}

#if JucePlugin_Enable_ARA
juce::AudioProcessorEditorARAExtension *
PitchNetAudioProcessorEditor::getARAClientExtensions() {
  return this;
}
#endif

PitchNetAudioProcessorEditor::~PitchNetAudioProcessorEditor() {
  stopTimer();
#if JucePlugin_Enable_ARA
  openAraEditors.erase(
      std::remove(openAraEditors.begin(), openAraEditors.end(), this),
      openAraEditors.end());
#endif

  // Some ARA hosts briefly overlap editor instances while rebuilding the plug-
  // in view.  Only the editor that still owns the processor binding may tear
  // down shared callbacks; an older editor's destructor must not detach the
  // newer visible editor.
  const bool ownsProcessorMainView =
      audioProcessor.getMainComponent() == mainView.get();
#if JucePlugin_Enable_ARA
  if (auto *araEditorView = getARAEditorView()) {
    araEditorView->removeListener(this);
    if (auto *araDocController = araEditorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  araDocController)) {
        // Do NOT clear the realtime-processor binding or persistence callbacks
        // here: both are owned by the processor and must stay live so ARA
        // playback and project restore keep working headlessly after the editor
        // closes. The processor detaches them in its destructor.
        if (pitchDocController->getMainComponent() == mainView.get()) {
          pitchDocController->releaseEditorProcessor(&audioProcessor);
          pitchDocController->setMainComponent(nullptr);
        }
      }
    }
  }
#endif

  removeMouseListener(this);
  if (ownsProcessorMainView) {
    // Preview/audition is editor-owned in non-ARA mode. Do not leave its
    // per-sample rendering path active after the controls that stop it are gone.
    audioProcessor.stopPluginAudition();
    audioProcessor.getTransportController().clearCallbacks();
    audioProcessor.setMainComponent(nullptr);
  }

  // Destroy all components (and their retained Font/Typeface objects) before
  // releasing the final shared UI resources. This is especially important for
  // DirectWrite memory fonts when a Windows host unloads the plugin DLL.
  mainView.reset();
  shutdownUiResources();
}

void PitchNetAudioProcessorEditor::setupARAMode() {
#if JucePlugin_Enable_ARA
  auto *editorView = getARAEditorView();
  if (!editorView) {
    setupNonARAMode();
    return;
  }

  auto *docController = editorView->getDocumentController();
  if (!docController) {
    setupNonARAMode();
    return;
  }

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);

  if (!pitchDocController) {
    setupNonARAMode();
    return;
  }

  mainView->setRecordControlVisible(false);
  mainView->setOnRecordArmChanged(nullptr);
  // D-180-R3: before this editor binds/activates a project, ask the view to
  // reveal the host edit cursor on the first bound paint.
  {
    double openCursorSeconds = 0.0;
    bool hasOpenCursor = false;
    if (auto *playHead = audioProcessor.getPlayHead()) {
      if (auto pos = playHead->getPosition()) {
        if (auto time = pos->getTimeInSeconds()) {
          openCursorSeconds = *time;
          hasOpenCursor = true;
        }
      }
    }
    if (!hasOpenCursor) {
      const auto transport =
          audioProcessor.getTransportController().getCurrentState();
      if (transport.position.hasTimeInSeconds) {
        openCursorSeconds = transport.position.timeInSeconds;
        hasOpenCursor = true;
      }
    }
    mainView->noteAraEditorOpened(hasOpenCursor ? openCursorSeconds : 0.0);
  }
  // ARA gives us a host link: transport controls and cycle editing are live.
  mainView->setHostTransportControlAvailable(true);

  // Listen for host selection changes to switch the canvas to the clicked
  // region (per-region Projects).
  editorView->addListener(this);

#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  ARA_DIAG("setupARAMode instance=" +
           juce::String(audioProcessor.getDiagInstanceId()) +
           " editor=" + ARA_DIAG_PTR(this) +
           " editorView=" + ARA_DIAG_PTR(editorView));
#endif

  auto safeEditorForHistory = juce::Component::SafePointer<PitchNetAudioProcessorEditor>(this);
  audioProcessor.onAraUndoManagerRequested = [safeEditorForHistory](const juce::String& key) -> PitchUndoManager* {
    if (!safeEditorForHistory) return nullptr;
    auto* editor = safeEditorForHistory.getComponent();
    for (auto* region : editor->collectAraPlaybackRegions()) {
      if (pitchnetRegionKey(*region) != key ||
          pitchnetRegionSelector(*region) != editor->audioProcessor.getActiveAraRegionSelector()) continue;
      auto* sequence = region->getRegionSequence();
      if (!sequence) return nullptr;
      auto* manager = editor->audioProcessor.getTrackUndoManager(araTrackKey(sequence));
      const juce::String regionName(region->getName());
      const juce::String label = regionName.isNotEmpty() ? regionName : "Clip";
      const auto trackKey = araTrackKey(sequence);
      manager->decorateAction = [safeEditorForHistory, key, label, trackKey](std::unique_ptr<UndoableAction> action) {
        if (action->ownsProjectContext()) return action;
        auto* editor = safeEditorForHistory.getComponent();
        if (!editor) return std::unique_ptr<UndoableAction>{};
        Project* expected = editor->mainView->getProject();
        auto prepare = [safeEditorForHistory, key, expected, trackKey] {
          if (!safeEditorForHistory) return false;
          auto* editor = safeEditorForHistory.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions()) {
            if (pitchnetRegionKey(*live) != key || araTrackKey(live->getRegionSequence()) != trackKey) continue;
            std::uint64_t revision = 0; bool canvas = false;
            if (editor->audioProcessor.getAraProjectForPreview(
                live->getAudioModification<PitchNetAudioModification>(), revision, canvas) != expected)
              return false;
            editor->activateAraRegion(live, false);
            return true;
          }
          return false;
        };
        return std::unique_ptr<UndoableAction>(new ContextUndoAction(
            std::move(action), key, label, std::move(prepare)));
      };
      return manager;
    }
    return nullptr;
  };

  mainView->setTrackPitchMacroTargets([this, pitchDocController] {
    std::vector<TrackPitchMacroTarget> targets;
    const auto regions = collectAraPlaybackRegions();
    juce::ARARegionSequence* sequence = nullptr;
    for (auto* region : regions)
      if (pitchnetRegionSelector(*region) == audioProcessor.getActiveAraRegionSelector())
        sequence = region->getRegionSequence();
    if (!sequence) return targets;
    std::map<juce::String, size_t> indices;
    for (auto* region : regions) {
      if (region->getRegionSequence() != sequence) continue;
      auto* modification = region->getAudioModification<PitchNetAudioModification>();
      if (!modification) continue;
      const auto key = pitchnetModificationKey(*modification);
      std::uint64_t revision = 0;
      bool fromCanvas = false;
      auto* project = const_cast<Project*>(audioProcessor.getAraProjectForPreview(
          modification, revision, fromCanvas));
      if (project && audioProcessor.araRegionProjectNeedsSourceHydration(key)) {
        pitchDocController->requestRegionCanvasAnalysis(region);
        project = const_cast<Project*>(audioProcessor.getAraProjectForPreview(
            modification, revision, fromCanvas));
      }
      if (!project || audioProcessor.araRegionProjectNeedsSourceHydration(key)) {
        mainView->setStatusMessage("Wait for all regions in this track to finish analysis.");
        return std::vector<TrackPitchMacroTarget>{};
      }
      auto found = indices.find(key);
      if (found == indices.end()) {
        const size_t index = targets.size();
        indices.emplace(key, index);
        TrackPitchMacroTarget target;
        target.project = project;
        // Resolve the current project by key rather than retaining a host object
        // in undo history. A split/replacement invalidates this action safely.
        auto safeEditor = juce::Component::SafePointer<PitchNetAudioProcessorEditor>(this);
        target.isValid = [safeEditor, key, project] {
          if (!safeEditor) return false;
          auto* editor = safeEditor.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions())
            if (pitchnetRegionKey(*live) == key) {
              std::uint64_t rev = 0; bool canvas = false;
              return editor->audioProcessor.getAraProjectForPreview(
                  live->getAudioModification<PitchNetAudioModification>(), rev, canvas) == project;
            }
          return false;
        };
        target.onChanged = [safeEditor, key, project](bool render) {
          if (!safeEditor) return;
          auto* editor = safeEditor.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions())
            if (pitchnetRegionKey(*live) == key) {
              auto* mod = live->getAudioModification<PitchNetAudioModification>();
              if (render) {
                juce::MemoryBlock archive;
                if (editor->audioProcessor.serializeAraRegionProject(key, archive))
                  mod->setProjectArchive(archive.getData(), archive.getSize());
                editor->audioProcessor.requestPluginProjectRender(*project, mod);
              }
              editor->regionPreviewCache.clear();
              editor->lastPublishedPreviewSignature.clear();
              editor->refreshAraRegionList();
              return;
            }
        };
        target.onCommit = [safeEditor, key] {
          if (!safeEditor) return;
          auto* editor = safeEditor.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions())
            if (pitchnetRegionKey(*live) == key) {
              juce::MemoryBlock archive;
              if (editor->audioProcessor.serializeAraRegionProject(key, archive))
                live->getAudioModification<PitchNetAudioModification>()->setProjectArchive(
                    archive.getData(), archive.getSize());
              return;
            }
        };
        targets.push_back(std::move(target));
        found = indices.find(key);
      }
      const auto* source = modification->getAudioSource();
      const double rate = source ? source->getSampleRate() : 0.0;
      if (rate > 0.0) {
        const double framesPerSample = project->getAudioData().sampleRate / (rate * HOP_SIZE);
        const int first = static_cast<int>(std::floor(
            region->getStartInAudioModificationSamples() * framesPerSample));
        const int last = static_cast<int>(std::ceil(
            region->getEndInAudioModificationSamples() * framesPerSample));
        targets[found->second].frames.emplace_back(first, last);
      }
    }
    return targets;
  });

  // Clicking inside an inactive region on the canvas makes it active in place.
  mainView->setOnRegionActivationRequested(
      [this](const juce::String &regionKey) {
        activateAraRegionByKey(regionKey, false);
      });
  // Clicking an inactive track in the left side panel switches to it.
  mainView->setOnTrackSelected([this](const juce::String &trackKey) {
    activateAraTrackByKey(trackKey);
  });
  // Pinning a track draws its notes on the canvas (Track mode).
  mainView->setOnTrackPinChanged([this](const juce::String &trackKey,
                                        bool pinned) {
    if (pinned)
      pinnedTrackKeys.insert(trackKey);
    else
      pinnedTrackKeys.erase(trackKey);
    lastPublishedTrackSignature.clear();
    lastPublishedPreviewSignature.clear();
    refreshAraRegionList();
  });
  // Pinned notes are outlined in the track colour, which this switch changes.
  mainView->setOnTrackColourModeChanged([this] {
    lastPublishedPreviewSignature.clear();
    refreshAraRegionList();
  });
  // Clip / Track toggle. Track mode needs the other regions' previews; Clip
  // mode drops them. Republish right away rather than on the next poll.
  mainView->setOnTrackViewModeChanged([this](bool) {
    lastPublishedPreviewSignature.clear();
    lastPublishedRegionSignature.clear();
    refreshAraRegionList();
  });
  mainView->setTrackViewModeAvailable(true);

  // Connect ARA controller to UI
  pitchDocController->setMainComponent(mainView.get());
  // Bind via the processor so the realtime-processor link outlives the editor:
  // ARA playback/bounce must keep working after the UI is closed.
  audioProcessor.setAraDocumentController(pitchDocController);
  pitchDocController->setEditorProcessor(&audioProcessor);
  audioProcessor.bindAraUndoManager(nullptr);
  // Persistence callbacks are owned by the processor (set in didBindToARA) so
  // that saved-project restore works with the UI closed; the editor must not
  // install editor-capturing callbacks that would dangle on close.
  mainView->setOnRequestBackendRender([this](const Project &project) {
    audioProcessor.requestPluginProjectRender(project);
  });
  mainView->setOnRequestBackendPreview(
      [pitchDocController](const Project &project, int startFrame,
                           int endFrame) {
        const auto &audioData = project.getAudioData();
        const double sampleRate =
            audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                                     : static_cast<double>(SAMPLE_RATE);
        pitchDocController->startPreviewRange(
            static_cast<double>(startFrame) * HOP_SIZE / sampleRate,
            static_cast<double>(endFrame) * HOP_SIZE / sampleRate);
      });
  mainView->setOnStopBackendPreview(
      [pitchDocController]() { pitchDocController->stopPreview(); });
  mainView->setOnRequestDragAudition(
      [pitchDocController](const juce::AudioBuffer<float> &buffer,
                           double sampleRate, bool wholeSegment) {
        pitchDocController->startPreviewAudio(buffer, sampleRate, wholeSegment);
      });
  mainView->setOnStopDragAudition(
      [pitchDocController]() { pitchDocController->stopPreview(); });

  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          if (shouldPlay)
            playbackController->requestStartPlayback();
          else
            playbackController->requestStopPlayback();
          return;
        }
      }
    }

    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          playbackController->requestStopPlayback();
          playbackController->requestSetPlaybackPosition(0.0);
          return;
        }
      }
    }

    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          playbackController->requestSetPlaybackPosition(timeInSeconds);
          return;
        }
      }
    }

    audioProcessor.requestHostSeek(timeInSeconds);
  });

  mainView->setOnRequestHostLoopRange(
      [this](double startSeconds, double endSeconds, bool enabled,
             bool hasRange) {
        if (auto *araEditorView = getARAEditorView()) {
          if (auto *araDocController = araEditorView->getDocumentController()) {
            if (auto *playbackController =
                    araDocController->getHostPlaybackController()) {
              if (hasRange && endSeconds > startSeconds)
                playbackController->requestSetCycleRange(
                    startSeconds, endSeconds - startSeconds);

              playbackController->requestEnableCycle(enabled);
              return;
            }
          }
        }

        juce::ignoreUnused(startSeconds, endSeconds, enabled, hasRange);
      });

  setupHostTransportUiSync(true);
  // VocalNet's working Studio One Event FX path reads the editor view's
  // current selection directly instead of waiting for a future listener
  // callback. Studio One can open the editor with a region already selected
  // without subsequently announcing a selection change.
  if (audioProcessor.wrapperType == juce::AudioProcessor::wrapperType_AAX)
    startTimerHz(60);
  else
    // Hosts announce region add/remove through the document controller, not to
    // the editor, so the Regions card polls for its list. Slow enough to be
    // free, fast enough that dropping a region in feels immediate.
    startTimerHz(kRegionListRefreshHz);

  refreshAraRegionList();
  refreshHostTempoMap();

  // D-178: the editor renderer carries this instance's region sequence (track)
  // even when the playback renderer has no regions yet (REAPER). Use that
  // sequence before considering the shared document or the host selection.
  if (auto *editorRenderer =
          audioProcessor.getEditorRenderer<PitchNetEditorRenderer>()) {
    for (auto *sequence :
         editorRenderer->getRegionSequences<juce::ARARegionSequence>()) {
      if (sequence == nullptr)
        continue;
      const auto &seqRegions =
          sequence->getPlaybackRegions<juce::ARAPlaybackRegion>();
      if (seqRegions.empty())
        continue;
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
      ARA_DIAG("setupARAMode editorRenderer sequence=" +
               juce::String(sequence->getName()) +
               " regions=" + juce::String(static_cast<int>(seqRegions.size())));
#endif
      if (pitchDocController->processPlaybackRegions(
              seqRegions, audioProcessor.getSampleRate())) {
        audioProcessor.setActiveAraRegion(seqRegions.front());
        return;
      }
    }
  }

  // The playback renderer contains the regions assigned to this plugin
  // instance. Use that set to select the correct track/region sequence rather
  // than guessing from the first source in the shared ARA document.
  if (auto *renderer = audioProcessor.getPlaybackRenderer()) {
    if (pitchDocController->processPlaybackRegions(
            renderer->getPlaybackRegions(), audioProcessor.getSampleRate())) {
      // Hosts are not required to send a selection-change notification when
      // an ARA editor first opens. Activate the region discovered from this
      // instance's renderer immediately instead of relying on onNewSelection().
      audioProcessor.setActiveAraRegion(
          pitchDocController->getCurrentPlaybackRegion());
      return;
    }
  }

  // Check for existing audio sources
  auto *juceDocument = docController->getDocument();
  if (pitchDocController->processExistingAudioSources(
          static_cast<juce::ARADocument *>(juceDocument))) {
    audioProcessor.setActiveAraRegion(
        pitchDocController->getCurrentPlaybackRegion());
    return;
  }

  // Last resort: the host selection itself.
  if (syncInitialARASelectionFromHost())
    return;
#endif
}

void PitchNetAudioProcessorEditor::timerCallback() {
  syncAAXARAPlayheadStateFromHost();

#if JucePlugin_Enable_ARA
  // The AAX playhead sync needs 60 Hz; the region list does not. Divide down
  // rather than running a second timer.
  if (--regionListRefreshCountdown <= 0) {
    const int interval = juce::jmax(1, getTimerInterval());
    regionListRefreshCountdown =
        juce::jmax(1, (1000 / kRegionListRefreshHz) / interval);
    refreshAraRegionList();
    // D-310: same cadence; the ARA content reader must stay off the audio
    // thread and off the paint path.
    refreshHostTempoMap();
  }
#endif
}

void PitchNetAudioProcessorEditor::syncAAXARAPlayheadStateFromHost() {
#if JucePlugin_Enable_ARA
  if (audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_AAX)
    return;

  if (!getARAEditorView() || !mainView)
    return;

  auto *playHead = audioProcessor.getPlayHead();
  if (!playHead)
    return;

  const auto positionInfo = playHead->getPosition();
  if (!positionInfo.hasValue()) {
    if (lastSyncedHostPlayState) {
      lastSyncedHostPlayState = false;
      mainView->updateHostPlaybackState(false);
    }
    return;
  }

  double timeInSeconds = 0.0;
  if (auto time = positionInfo->getTimeInSeconds())
    timeInSeconds = *time;
  else if (auto samples = positionInfo->getTimeInSamples()) {
    const double sampleRate = audioProcessor.getHostSampleRate();
    if (sampleRate > 0.0)
      timeInSeconds = static_cast<double>(*samples) / sampleRate;
  }

  const bool isPlaying = positionInfo->getIsPlaying();
  const bool positionChanged = !hasSyncedHostPlayhead ||
                               !juce::approximatelyEqual(
                                   lastSyncedHostPlayheadSeconds,
                                   timeInSeconds);
  const bool playStateChanged =
      !hasSyncedHostPlayhead || lastSyncedHostPlayState != isPlaying;

  if (positionChanged) {
    lastSyncedHostPlayheadSeconds = timeInSeconds;
    mainView->updatePlaybackPosition(timeInSeconds);
  }

  if (playStateChanged) {
    lastSyncedHostPlayState = isPlaying;
    mainView->updateHostPlaybackState(isPlaying);
  }

  hasSyncedHostPlayhead = true;
#endif
}

#if JucePlugin_Enable_ARA
bool PitchNetAudioProcessorEditor::syncInitialARASelectionFromHost() {
  auto *editorView = getARAEditorView();
  if (editorView == nullptr)
    return false;

  const auto &viewSelection = editorView->getViewSelection();
  const auto regions =
      viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>();
  if (regions.empty() || regions.front() == nullptr)
    return false;

  onNewSelection(viewSelection);
  return true;
}

size_t PitchNetAudioProcessorEditor::documentControllerRegionCount() const {
  if (auto *editorView = getARAEditorView()) {
    if (auto *docController = editorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  docController))
        return pitchDocController->getCurrentPlaybackRegions().size();
    }
  }
  return 0;
}

size_t PitchNetAudioProcessorEditor::documentRegionCount() const {
  size_t count = 0;
  if (auto *editorView = getARAEditorView()) {
    if (auto *docController = editorView->getDocumentController()) {
      if (auto *document =
              static_cast<juce::ARADocument *>(docController->getDocument())) {
        for (auto *sequence :
             document->getRegionSequences<juce::ARARegionSequence>())
          if (sequence != nullptr)
            count += sequence->getPlaybackRegions<juce::ARAPlaybackRegion>().size();
      }
    }
  }
  return count;
}

std::vector<juce::ARAPlaybackRegion *>
PitchNetAudioProcessorEditor::collectAraPlaybackRegions() const {
  // A union, not a first-hit lookup. Hosts disagree about what a plug-in
  // instance "owns": some assign every region of the track to the renderer,
  // some assign only the one being played, and per-event hosts assign exactly
  // one. Taking the first non-empty source therefore listed a single region
  // and hid the rest of the track. Instead: gather regions from every source
  // we have, then expand to the full region sequence (track) each of them
  // belongs to, so every clip on the track is selectable no matter which one
  // the host handed us.
  std::vector<juce::ARAPlaybackRegion *> regions;
  std::vector<juce::ARARegionSequence *> sequences;

  const auto isUsable = [](juce::ARAPlaybackRegion *region) {
    return region != nullptr && region->getAudioModification() != nullptr &&
           region->getAudioModification()->getAudioSource() != nullptr;
  };

  const auto addSequence = [&sequences](juce::ARARegionSequence *sequence) {
    if (sequence != nullptr &&
        std::find(sequences.begin(), sequences.end(), sequence) ==
            sequences.end())
      sequences.push_back(sequence);
  };

  const auto addRegions =
      [&](const std::vector<juce::ARAPlaybackRegion *> &candidates) {
        for (auto *candidate : candidates) {
          if (!isUsable(candidate))
            continue;
          if (std::find(regions.begin(), regions.end(), candidate) ==
              regions.end())
            regions.push_back(candidate);
          addSequence(candidate->getRegionSequence());
        }
      };

  // Regions this instance renders.
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    addRegions(renderer->getPlaybackRegions<juce::ARAPlaybackRegion>());
  if (auto *editorRenderer = audioProcessor.getEditorRenderer())
    addRegions(editorRenderer->getPlaybackRegions<juce::ARAPlaybackRegion>());

  auto *editorView = getARAEditorView();
  if (editorView == nullptr)
    return regions;

  // What the host currently has selected, including the track when only a
  // track is selected.
  const auto &viewSelection = editorView->getViewSelection();
  addRegions(viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>());
  addRegions(viewSelection.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>());
  for (auto *sequence :
       viewSelection.getRegionSequences<juce::ARARegionSequence>())
    addSequence(sequence);

  auto *docController = editorView->getDocumentController();
  if (docController == nullptr)
    return regions;

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);

  // What the document controller has been working with.
  if (pitchDocController != nullptr) {
    addRegions(pitchDocController->getCurrentPlaybackRegions());
    if (auto *current = pitchDocController->getCurrentPlaybackRegion())
      addRegions({current});
  }

  auto *document =
      static_cast<juce::ARADocument *>(docController->getDocument());

  // Every clip on each track we touched. This is what makes the other regions
  // of a track appear when the host only told us about one of them.
  for (size_t i = 0; i < sequences.size(); ++i)
    addRegions(sequences[i]->getPlaybackRegions<juce::ARAPlaybackRegion>());

  // Nothing anywhere: fall back to the whole document, so the card still lists
  // what PitchNet has detected even before a selection reaches us.
  if (regions.empty() && document != nullptr) {
    for (auto *sequence :
         document->getRegionSequences<juce::ARARegionSequence>())
      if (sequence != nullptr)
        addRegions(sequence->getPlaybackRegions<juce::ARAPlaybackRegion>());
  }

  // Timeline order, so the list reads the way the arrangement does.
  std::stable_sort(regions.begin(), regions.end(),
                   [](juce::ARAPlaybackRegion *a, juce::ARAPlaybackRegion *b) {
                     return a->getStartInPlaybackTime() <
                            b->getStartInPlaybackTime();
                   });
  return regions;
}

void PitchNetAudioProcessorEditor::refreshHostTempoMap() {
  auto *editorView = getARAEditorView();
  if (editorView == nullptr || mainView == nullptr)
    return;

  // Pick the region being edited; fall back to the host selection and finally
  // to any region, so the map is published even before a region is "current"
  // (the tempo map belongs to the musical context, not to the clip).
  juce::ARAPlaybackRegion *region = nullptr;
  if (auto *docController = editorView->getDocumentController()) {
    if (auto *pitchDocController =
            juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<
                PitchNetDocumentController>(docController))
      region = pitchDocController->getCurrentPlaybackRegion();

    if (region == nullptr) {
      const auto selected =
          editorView->getViewSelection()
              .getPlaybackRegions<juce::ARAPlaybackRegion>();
      if (!selected.empty())
        region = selected.front();
    }
  }
  if (region == nullptr) {
    const auto regions = collectAraPlaybackRegions();
    if (!regions.empty())
      region = regions.front();
  }

  ARATempoMapReader::Probe probe;
  auto map = ARATempoMapReader::readForRegion(region, &probe);

  juce::String signature = probe.toString();
  if (map != nullptr)
    signature << " | " << juce::String(map->describe().c_str());
  if (signature == lastTempoMapSignature)
    return;
  lastTempoMapSignature = signature;

  mainView->updateHostTempoMap(map);

#if PITCHNET_TEMPO_MAP_PROBE
  // G1 probe (one-off builds only): the numbers go next to the region starts
  // so the "song time vs modification time" question can be answered without
  // guessing. Remove with the PITCHNET_TEMPO_MAP_PROBE option.
  auto logFile = PlatformPaths::getLogFile("tempo_map_probe.log");
  juce::String line;
  line << juce::Time::getCurrentTime().toString(true, true, true, false)
       << " srate=" << audioProcessor.getHostSampleRate();
  if (region != nullptr) {
    line << " regionPlayback=" << juce::String(region->getStartInPlaybackTime(), 6)
         << " regionModSamples="
         << juce::String(region->getStartInAudioModificationSamples());
  }
  line << " " << probe.toString();
  if (map != nullptr)
    line << " | " << juce::String(map->describe().c_str());
  logFile.appendText(line + "\n");
#endif
}

void PitchNetAudioProcessorEditor::refreshAraRegionList() {
  if (mainView == nullptr || getARAEditorView() == nullptr)
    return;

  // D-180: measurement only. The 8 Hz poll must not be the hidden cost behind a
  // slow repaint.
  const double refreshStartMs = juce::Time::getMillisecondCounterHiRes();
  struct RefreshReporter {
    IMainView *view = nullptr;
    double startMs = 0.0;
    ~RefreshReporter() {
      if (view != nullptr)
        view->noteAraRegionRefreshDuration(
            juce::Time::getMillisecondCounterHiRes() - startMs);
    }
  } refreshReporter{mainView.get(), refreshStartMs};

  const auto regions = collectAraPlaybackRegions();

  std::vector<MainViewRegionEntry> entries;
  entries.reserve(regions.size());

  int index = 0;
  for (auto *region : regions) {
    ++index;

    juce::String name;
    // getEffectiveName() falls back through modification to audio source, so
    // this is the name the host shows for the clip wherever it set one.
    if (const char *effectiveName = region->getEffectiveName())
      name = juce::String::fromUTF8(effectiveName).trim();
    if (name.isEmpty())
      name = "Region " + juce::String(index);

    // Two clips of the same take share a name; number the repeats so the list
    // stays usable.
    int duplicates = 0;
    for (const auto &existing : entries)
      if (existing.name == name || existing.name.startsWith(name + " ("))
        ++duplicates;
    if (duplicates > 0)
      name << " (" << (duplicates + 1) << ")";

    // The list selects a playback region, so it is keyed by the region
    // selector. Keying it by the modification would give every sibling of a
    // split clip the same entry key: none of them could be told apart, and all
    // of them would render as active.
    entries.push_back({pitchnetRegionSelector(*region), name});
  }

  const auto activeSelector = audioProcessor.getActiveAraRegionSelector();

  audioProcessor.updateAraTrackAnalysisQueue(regions);
  publishAraRegionPreviews(regions, entries, activeSelector);
  publishAraTrackList(regions, entries, activeSelector);

  juce::String signature = activeSelector;
  for (const auto &entry : entries)
    signature << "\n" << entry.key << "\t" << entry.name;

  if (regionListPublished && signature == lastPublishedRegionSignature)
    return;

  regionListPublished = true;
  lastPublishedRegionSignature = signature;

  // Only fires when the list actually changes, so this stays quiet - one line
  // per change saying what the region list holds and where it came from.
  juce::String diagnostic;
  diagnostic << "ARA region list: " << static_cast<int>(entries.size())
             << " region(s), active='" << activeSelector << "'";
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    diagnostic << " renderer="
               << static_cast<int>(
                      renderer->getPlaybackRegions<juce::ARAPlaybackRegion>()
                          .size());
  if (auto *editorRenderer = audioProcessor.getEditorRenderer())
    diagnostic << " editorRenderer="
               << static_cast<int>(
                      editorRenderer
                          ->getPlaybackRegions<juce::ARAPlaybackRegion>()
                          .size());
  diagnostic << " docController="
             << static_cast<int>(documentControllerRegionCount())
             << " document=" << static_cast<int>(documentRegionCount());
  for (size_t i = 0; i < entries.size() && i < regions.size(); ++i)
    diagnostic << "\n    [" << static_cast<int>(i) << "] '" << entries[i].name
               << "' " << juce::String(regions[i]->getStartInPlaybackTime(), 3)
               << "s key=" << entries[i].key;
  LOG(diagnostic);
}

namespace {
// Session-local identity of a track (region sequence) for the track list.
// Never stored; resolved by re-walking the document's live sequences.
juce::String araTrackKey(juce::ARARegionSequence *sequence) {
  return juce::String::toHexString(static_cast<juce::int64>(
      reinterpret_cast<std::uintptr_t>(sequence)));
}

// R5d (D-358) / R11b (D-365): stable identity of one track for the document's
// guide store. Neither the RegionSequence ADDRESS (recreated when the editor
// closes and reopens, R5c) nor the ARADocument address (recreated when the
// project closes and reopens, R11) is part of it. The key is the track's own
// audio sources - identified by the names ARA restores with the project - plus
// the track name. See Models/TrackIdentity.h and Plugin/AraSourceIdentity.h.
juce::String stableTrackKey(juce::ARADocument *document,
                            juce::ARARegionSequence *sequence) {
  if (sequence == nullptr)
    return {};
  int ordinal = 0;
  if (document != nullptr) {
    const auto sequences =
        document->getRegionSequences<juce::ARARegionSequence>();
    for (size_t i = 0; i < sequences.size(); ++i)
      if (sequences[i] == sequence) {
        ordinal = static_cast<int>(i);
        break;
      }
  }
  return trackKeyFromSources(araIdentity::sequenceSourceIdentities(*sequence),
                             juce::String(sequence->getName()), ordinal);
}

// The track colour the DAW sends through ARA. Hosts set it on the region
// sequence (the track) itself; some only colour the clips, so fall back to the
// first clip's colour on that track. hasHostColour is false when the DAW sent
// neither; the UI then uses its default colour.
juce::Colour araTrackColour(juce::ARARegionSequence &sequence,
                            bool &hasHostColour,
                            const char **source = nullptr) {
  const auto toColour = [](const ARA::ARAColor *araColour) {
    return juce::Colour::fromFloatRGBA(araColour->r, araColour->g,
                                       araColour->b, 1.0f);
  };
  hasHostColour = true;
  if (const ARA::ARAColor *araColour = sequence.getColor()) {
    if (source != nullptr)
      *source = "track";
    return toColour(araColour);
  }
  for (auto *region : sequence.getPlaybackRegions<juce::ARAPlaybackRegion>())
    if (region != nullptr)
      if (const ARA::ARAColor *regionColour = region->getColor()) {
        if (source != nullptr)
          *source = "clip";
        return toColour(regionColour);
      }
  hasHostColour = false;
  if (source != nullptr)
    *source = "none";
  return {};
}
} // namespace

bool PitchNetAudioProcessorEditor::activateAraTrackByKey(
    const juce::String &trackKey) {
  if (trackKey.isEmpty())
    return false;

  auto *editorView = getARAEditorView();
  auto *docController =
      editorView != nullptr ? editorView->getDocumentController() : nullptr;
  auto *document =
      docController != nullptr
          ? static_cast<juce::ARADocument *>(docController->getDocument())
          : nullptr;
  if (document == nullptr)
    return false;

  // Resolve against live sequences: the track may have been removed since the
  // list was published.
  juce::ARARegionSequence *sequence = nullptr;
  for (auto *candidate :
       document->getRegionSequences<juce::ARARegionSequence>())
    if (candidate != nullptr && araTrackKey(candidate) == trackKey) {
      sequence = candidate;
      break;
    }

  if (sequence == nullptr) {
    lastPublishedTrackSignature.clear();
    return false;
  }

  std::vector<juce::ARAPlaybackRegion *> candidates;
  for (auto *region : sequence->getPlaybackRegions<juce::ARAPlaybackRegion>())
    if (region != nullptr && region->getAudioModification() != nullptr &&
        region->getAudioModification()->getAudioSource() != nullptr)
      candidates.push_back(region);

  // A track with no usable clip has nothing to put on the canvas.
  if (candidates.empty())
    return false;

  juce::ARAPlaybackRegion *target = nullptr;
  if (const auto remembered = lastActiveRegionByTrack.find(trackKey);
      remembered != lastActiveRegionByTrack.end())
    for (auto *region : candidates)
      if (pitchnetRegionSelector(*region) == remembered->second) {
        target = region;
        break;
      }

  if (target == nullptr)
    target = *std::min_element(
        candidates.begin(), candidates.end(),
        [](juce::ARAPlaybackRegion *a, juce::ARAPlaybackRegion *b) {
          return a->getStartInPlaybackTime() < b->getStartInPlaybackTime();
        });

  activateAraRegion(target, true);
  return true;
}

void PitchNetAudioProcessorEditor::publishAraTrackList(
    const std::vector<juce::ARAPlaybackRegion *> &regions,
    const std::vector<MainViewRegionEntry> &entries,
    const juce::String &activeSelector) {
  if (mainView == nullptr)
    return;

  auto *editorView = getARAEditorView();
  auto *docController =
      editorView != nullptr ? editorView->getDocumentController() : nullptr;
  auto *document =
      docController != nullptr
          ? static_cast<juce::ARADocument *>(docController->getDocument())
          : nullptr;
  if (document == nullptr)
    return;

  // The active track is the one holding the region the canvas shows. Before a
  // region is active, fall back to the track this instance renders.
  juce::ARARegionSequence *activeSequence = nullptr;
  for (size_t i = 0; i < regions.size() && i < entries.size(); ++i)
    if (entries[i].key == activeSelector) {
      activeSequence = regions[i]->getRegionSequence();
      break;
    }
  if (activeSequence == nullptr)
    if (auto *renderer = audioProcessor.getPlaybackRenderer())
      for (auto *region :
           renderer->getPlaybackRegions<juce::ARAPlaybackRegion>())
        if (region != nullptr && region->getRegionSequence() != nullptr) {
          activeSequence = region->getRegionSequence();
          break;
        }

  // The ARA document only holds region sequences for tracks PitchNet is on.
  auto sequences = document->getRegionSequences<juce::ARARegionSequence>();
  sequences.erase(std::remove(sequences.begin(), sequences.end(), nullptr),
                  sequences.end());
  // Host track order.
  std::stable_sort(sequences.begin(), sequences.end(),
                   [](juce::ARARegionSequence *a, juce::ARARegionSequence *b) {
                     return a->getOrderIndex() < b->getOrderIndex();
                   });

  const auto keyFor = araTrackKey;

  std::vector<MainViewTrackEntry> tracks;
  tracks.reserve(sequences.size());
  int index = 0;
  juce::String colourDiagnostic;
  for (auto *sequence : sequences) {
    ++index;

    juce::String name;
    if (const char *sequenceName = sequence->getName())
      name = juce::String::fromUTF8(sequenceName).trim();
    if (name.isEmpty())
      name = "Track " + juce::String(index);

    const char *colourSource = "none";
    bool hasHostColour = false;
    const juce::Colour colour =
        araTrackColour(*sequence, hasHostColour, &colourSource);

    const auto key = keyFor(sequence);
    tracks.push_back({key, name, colour, hasHostColour,
                      pinnedTrackKeys.count(key) > 0});
    colourDiagnostic << "\n    '" << name << "' colour=";
    if (hasHostColour)
      colourDiagnostic << "#" << colour.toDisplayString(false) << " ("
                       << colourSource << ")";
    else
      colourDiagnostic << "none (DAW sent no colour)";
  }

  const juce::String activeKey =
      activeSequence != nullptr ? keyFor(activeSequence) : juce::String();

  // The active track's pin is always off; a track that became active loses
  // its pin. Forget pins of tracks that no longer exist.
  for (auto it = pinnedTrackKeys.begin(); it != pinnedTrackKeys.end();) {
    const bool exists =
        std::any_of(tracks.begin(), tracks.end(),
                    [&](const MainViewTrackEntry &t) { return t.key == *it; });
    if (!exists || *it == activeKey)
      it = pinnedTrackKeys.erase(it);
    else
      ++it;
  }
  for (auto &track : tracks)
    track.pinned = pinnedTrackKeys.count(track.key) > 0;
  if (activeKey.isNotEmpty() && activeSelector.isNotEmpty())
    lastActiveRegionByTrack[activeKey] = activeSelector;

  juce::String signature = activeKey;
  for (const auto &track : tracks)
    signature << "\n" << track.key << "\t" << track.name << "\t"
              << (track.hasHostColour ? track.colour.toString() : "-") << "\t"
              << (track.pinned ? "P" : "-");

  if (signature == lastPublishedTrackSignature)
    return;
  lastPublishedTrackSignature = signature;
  mainView->updateTrackList(tracks, activeKey);

  // One line per change: where each track's colour came from, so a host that
  // sends no ARA colour is easy to spot.
  LOG("ARA track list: " + juce::String(static_cast<int>(tracks.size())) +
      " track(s)" + colourDiagnostic);
}

void PitchNetAudioProcessorEditor::publishAraRegionPreviews(
    const std::vector<juce::ARAPlaybackRegion *> &regions,
    const std::vector<MainViewRegionEntry> &entries,
    const juce::String &activeSelector) {
  if (mainView == nullptr)
    return;

  // Clip mode shows one modification in its own time and draws no other
  // regions. Publish nothing, and skip the work of building previews.
  if (!mainView->isTrackViewMode()) {
    regionPreviewCache.clear();
    if (lastPublishedPreviewSignature != "clip") {
      lastPublishedPreviewSignature = "clip";
      mainView->updateRegionPreviews(nullptr);
    }
    // N1 (D-340): no track index outside track mode; notes_debug says so
    // instead of addressing a single item as if it were the track.
    documentNoteIndex.reset();
    publishDocumentNoteIndex(nullptr);
    return;
  }

  // Only the track of the active region: other tracks have their own plug-in
  // instance and must not be drawn over this one.
  juce::ARARegionSequence *activeSequence = nullptr;
  for (size_t i = 0; i < regions.size() && i < entries.size(); ++i)
    if (entries[i].key == activeSelector) {
      activeSequence = regions[i]->getRegionSequence();
      break;
    }

  // N1 (D-340): one read-only index of every note the track's items hold. It
  // points at the real Note objects (no copy, no second owner) and is the
  // single enumeration track mode is drawn from. Built before the previews so
  // their note shapes come from it.
  std::vector<DocumentNoteRegionSpec> noteSpecs;
  for (size_t i = 0; i < regions.size() && i < entries.size(); ++i) {
    auto *region = regions[i];
    if (activeSequence != nullptr && region->getRegionSequence() != activeSequence)
      continue;
    DocumentNoteRegionSpec spec;
    spec.key = entries[i].key;
    spec.name = entries[i].name;
    if (auto *modification = region->getAudioModification()) {
      spec.modificationKey = pitchnetModificationKey(*modification);
      spec.sourceKey = araSourceKey(*region);
      spec.savedNotes = savedNoteCountForModification(
          region->getAudioModification<PitchNetAudioModification>());
    }
    spec.hostStartSeconds = std::max(0.0, region->getStartInPlaybackTime());
    spec.hostEndSeconds =
        std::max(spec.hostStartSeconds, region->getEndInPlaybackTime());
    const auto sourceWindow = araRegionSourceWindow(*region);
    spec.sourceStartSeconds = sourceWindow.startSeconds;
    spec.sourceEndSeconds = sourceWindow.endSeconds;
    std::uint64_t revision = 0;
    bool fromCanvas = false;
    spec.project = const_cast<Project *>(audioProcessor.getAraProjectForPreview(
        region->getAudioModification<PitchNetAudioModification>(), revision,
        fromCanvas));
    noteSpecs.push_back(std::move(spec));
  }
  // R5d (D-358): the index's track key is the STABLE one (the guide store binds
  // to it in updateDocumentNoteIndex), not the RegionSequence address.
  juce::String activeTrackKey;
  if (activeSequence != nullptr) {
    auto *editorView = getARAEditorView();
    auto *docController =
        editorView != nullptr ? editorView->getDocumentController() : nullptr;
    auto *document = docController != nullptr
                         ? static_cast<juce::ARADocument *>(
                               docController->getDocument())
                         : nullptr;
    activeTrackKey = stableTrackKey(document, activeSequence);
  }
  documentNoteIndex = buildDocumentNoteIndex(activeTrackKey, noteSpecs);
  publishDocumentNoteIndex(documentNoteIndex);

  auto previews = std::make_shared<std::vector<MainViewRegionPreview>>();
  juce::String signature = "track\n" + activeSelector;
  std::map<juce::String, RegionPreviewCacheEntry> nextCache;

  // Builds (or reuses) one region's preview and appends it.
  const auto addPreview = [&](juce::ARAPlaybackRegion *region,
                              const juce::String &regionKey,
                              const juce::String &regionName, bool isActive,
                              bool pinned, juce::Colour outlineColour,
                              const DocumentNoteRegion *indexRegion) {
    // regionKey is the region selector (which window); edits are owned by
    // the modification, so the Project is looked up by the modification key.
    const double start = std::max(0.0, region->getStartInPlaybackTime());
    const double end = std::max(start, region->getEndInPlaybackTime());
    double startInModification = 0.0;
    if (auto *modification = region->getAudioModification())
      if (auto *source = modification->getAudioSource())
        if (source->getSampleRate() > 0.0)
          startInModification =
              static_cast<double>(
                  region->getStartInAudioModificationSamples()) /
              source->getSampleRate();

    std::uint64_t revision = 0;
    bool fromCanvas = false;
    // N10 (D-350): every item, the active one included, is drawn from the live
    // Project the modification owns. The active item's Project is the canvas's.
    const Project* sourceProject = audioProcessor.getAraProjectForPreview(
        region->getAudioModification<PitchNetAudioModification>(), revision, fromCanvas);
    auto* project = const_cast<Project*>(sourceProject);

    RegionPreviewCacheEntry cacheEntry;
    const auto cached = regionPreviewCache.find(regionKey);
    const bool reusable =
        cached != regionPreviewCache.end() &&
        cached->second.project == project &&
        cached->second.fromCanvas == fromCanvas &&
        cached->second.revision == revision &&
        juce::approximatelyEqual(cached->second.startSeconds, start) &&
        juce::approximatelyEqual(cached->second.endSeconds, end) &&
        juce::approximatelyEqual(cached->second.startInModificationSeconds,
                                 startInModification);

    if (reusable) {
      cacheEntry = cached->second;
    } else {
      cacheEntry.project = project;
      cacheEntry.fromCanvas = fromCanvas;
      cacheEntry.revision = revision;
      cacheEntry.startSeconds = start;
      cacheEntry.endSeconds = end;
      cacheEntry.startInModificationSeconds = startInModification;
      if (project != nullptr)
        cacheEntry.preview = buildRegionPreview(*project, start, end,
                                                startInModification, indexRegion);
    }

    auto &preview = cacheEntry.preview;
    preview.key = regionKey;
    preview.name = regionName;
    preview.startSeconds = start;
    preview.endSeconds = end;
    preview.active = isActive;
    preview.pinned = pinned;
    preview.outlineColour = outlineColour;
    previews->push_back(preview);

    signature << "\n" << regionKey << "\t" << regionName << "\t"
              << juce::String(start, 6) << "\t" << juce::String(end, 6)
              << "\t" << juce::String(startInModification, 6) << "\t"
              << (isActive ? "A" : "-") << (fromCanvas ? "C" : "S") << "\t"
              << juce::String::toHexString(
                     static_cast<juce::int64>(
                         reinterpret_cast<std::uintptr_t>(sourceProject)))
              << "\t" << juce::String(static_cast<juce::int64>(revision));
    if (pinned)
      signature << "\tP" << outlineColour.toString();
    nextCache[regionKey] = std::move(cacheEntry);
  };

  for (size_t i = 0; i < regions.size() && i < entries.size(); ++i) {
    auto *region = regions[i];
    if (activeSequence != nullptr &&
        region->getRegionSequence() != activeSequence)
      continue;
    addPreview(region, entries[i].key, entries[i].name,
               entries[i].key == activeSelector, false, {},
               findIndexRegion(documentNoteIndex.get(), entries[i].key));
  }

  // Pinned tracks: every clip, drawn as notes outlined in the track colour.
  // Never the active track - its own clips are already here.
  if (!pinnedTrackKeys.empty()) {
    auto *editorView = getARAEditorView();
    auto *docController =
        editorView != nullptr ? editorView->getDocumentController() : nullptr;
    auto *document =
        docController != nullptr
            ? static_cast<juce::ARADocument *>(docController->getDocument())
            : nullptr;
    const bool useDawColour = mainView->getUseDawTrackColour();
    if (document != nullptr)
      for (auto *sequence :
           document->getRegionSequences<juce::ARARegionSequence>()) {
        if (sequence == nullptr || sequence == activeSequence ||
            pinnedTrackKeys.count(araTrackKey(sequence)) == 0)
          continue;
        MainViewTrackEntry track;
        track.colour = araTrackColour(*sequence, track.hasHostColour);
        const auto outline = resolveTrackColour(track, useDawColour);
        for (auto *region :
             sequence->getPlaybackRegions<juce::ARAPlaybackRegion>())
          if (region != nullptr && region->getAudioModification() != nullptr)
            addPreview(region, pitchnetRegionSelector(*region), {}, false, true,
                       outline, nullptr);
      }
  }

  regionPreviewCache = std::move(nextCache);

  if (signature == lastPublishedPreviewSignature)
    return;
  lastPublishedPreviewSignature = signature;
  mainView->updateRegionPreviews(std::move(previews));
}

// N1 (D-340): publish the note index only when what it enumerates changed. The
// entries hold Note pointers, so an edit that moves a note's pitch is visible
// through them without a republication; membership, frames and counts are what
// this signature watches.
void PitchNetAudioProcessorEditor::publishDocumentNoteIndex(
    DocumentNoteIndexPtr index) {
  juce::String signature;
  if (index == nullptr) {
    signature = "none";
  } else {
    signature << index->trackKey << "\t" << index->totalNotes << "\t"
              << index->projectNotes << "\t"
              << static_cast<int>(index->outsideNotes.size()) << "\t"
              << index->regionCount();
    for (const auto &region : index->regions)
      signature << "\n"
                << region.key << "\t" << region.modificationKey << "\t"
                << juce::String(region.sourceStartSeconds, 6) << "\t"
                << juce::String(region.sourceEndSeconds, 6) << "\t"
                << (region.analyzed ? "A" : "-") << "\t"
                << region.notesInWindow << "\t" << region.savedNotes << "\t"
                << juce::String::toHexString(static_cast<juce::int64>(
                       reinterpret_cast<std::uintptr_t>(region.project)));
    juce::int64 frames = 0;
    for (const auto &ref : index->notes)
      frames += static_cast<juce::int64>(ref.firstFrame) * 31 + ref.lastFrame;
    signature << "\nf" << juce::String(frames);
  }

  if (signature == lastPublishedNoteIndexSignature)
    return;
  lastPublishedNoteIndexSignature = signature;
  if (mainView != nullptr)
    mainView->updateDocumentNoteIndex(std::move(index));
}

// The host-stored project archive is read once per modification (a JSON parse
// of a song-length project is not something to repeat 8 times a second). -1
// means it could not be read, never "zero notes".
int PitchNetAudioProcessorEditor::savedNoteCountForModification(
    PitchNetAudioModification *modification) {
  if (modification == nullptr)
    return -1;
  const auto key = pitchnetModificationKey(*modification);
  const auto cached = savedNoteCountsByModification.find(key);
  if (cached != savedNoteCountsByModification.end())
    return cached->second;

  int count = -1;
  juce::MemoryBlock archive;
  if (modification->copyProjectArchive(archive) && archive.getSize() > 0) {
    const auto json = juce::JSON::parse(juce::String::fromUTF8(
        static_cast<const char *>(archive.getData()),
        static_cast<int>(archive.getSize())));
    if (auto *object = json.getDynamicObject())
      if (auto *notes = object->getProperty("notes").getArray())
        count = static_cast<int>(notes->size());
  }
  savedNoteCountsByModification[key] = count;
  return count;
}

void PitchNetAudioProcessorEditor::activateAraRegionByKey(
    const juce::String &regionKey, bool focusView) {
  if (regionKey.isEmpty())
    return;

  // Match against freshly collected regions rather than a stored pointer: the
  // host may have destroyed a region since the list was last published.
  juce::ARAPlaybackRegion *target = nullptr;
  for (auto *region : collectAraPlaybackRegions()) {
    if (pitchnetRegionSelector(*region) == regionKey) {
      target = region;
      break;
    }
  }

  if (target == nullptr) {
    // Whatever the card is showing no longer exists; republish on the next tick.
    lastPublishedRegionSignature.clear();
    return;
  }

  activateAraRegion(target, focusView);
}

void PitchNetAudioProcessorEditor::activateAraRegion(
    juce::ARAPlaybackRegion *target, bool focusView) {
  if (target == nullptr)
    return;

  if (auto *araEditorView = getARAEditorView()) {
    if (auto *araDocController = araEditorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  araDocController))
        pitchDocController->setCurrentPlaybackRegion(target);
    }
  }

  const double activationStartMs = juce::Time::getMillisecondCounterHiRes();
  audioProcessor.setActiveAraRegion(target);
  const auto activationMs =
      juce::Time::getMillisecondCounterHiRes() - activationStartMs;
  if (mainView != nullptr)
  {
    mainView->noteAraActivationDuration(activationMs);
    mainView->noteAraArchiveRestoreDuration(
        audioProcessor.getLastArchiveRestoreMs());
    const auto timing = audioProcessor.getLastAraActivationTiming();
    mainView->noteAraActivationBreakdown(
        timing.cancelMs, timing.copyMs, timing.parseMs, timing.installMs,
        timing.focusMs, timing.bindMs, timing.totalMs);
  }
  if (focusView) {
    // D-180-R4: do not focus a region whose project is not ready; wait for the
    // bind so the previous view stays on screen instead of scroll 0.
#if defined(PITCHNET_D180_R34_LEGACY)
    const bool projectReady = true;
#else
    const auto activeKey = audioProcessor.getActiveAraRegionKey();
    const bool projectReady =
        audioProcessor.hasAraRegionProject(activeKey) &&
        !audioProcessor.araRegionProjectNeedsSourceHydration(activeKey);
#endif
    if (pitchnet::shouldApplyRegionFocus(projectReady)) {
      // The canvas is in modification time; the region's host position is not.
      const auto span = audioProcessor.activeRegionSpanInModificationTime();
      mainView->focusTimelineRange(span.first, span.second);
    }
  }

  // The active key just changed. Publish now rather than on the next poll so
  // the canvas never shows the new region twice (as project and as preview)
  // or the old one not at all.
  lastPublishedRegionSignature.clear();
  refreshAraRegionList();
}

void PitchNetAudioProcessorEditor::onNewSelection(
    const juce::ARAViewSelection &viewSelection) {
  // D-178: the host may still report the previously selected track when this
  // window opens. setupARAMode() establishes the window's own track from the
  // editor renderer sequence first; ignore the stale initial selection during
  // the grace period.
  if (juce::Time::getMillisecondCounterHiRes() < editorReadyTimeMs) {
    refreshAraRegionList();
    return;
  }

  // D-180: measurement only. Starts the clock for selection -> first correct
  // repaint; the project binding and the paint are reported separately.
  if (mainView != nullptr)
    mainView->noteAraSelectionStarted();

  // Switch the canvas to whichever region the user selected in the host. Each
  // region/track carries its own persistent Project, so this swaps what is
  // shown and edited without disturbing the others. Prefer an explicitly
  // selected playback region; fall back to the effective selection (e.g. a
  // time-range/marquee selection that implies a set of regions).
  auto regions = viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>();
  const auto effective =
      viewSelection.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
  const auto sequences =
      viewSelection.getRegionSequences<juce::ARARegionSequence>();

  std::vector<juce::ARAPlaybackRegion *> rendererRegions;
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    rendererRegions = renderer->getPlaybackRegions<juce::ARAPlaybackRegion>();

  const auto isAssignedToThisInstance =
      [&rendererRegions](juce::ARAPlaybackRegion *region) {
        return rendererRegions.empty() ||
               std::find(rendererRegions.begin(), rendererRegions.end(),
                         region) != rendererRegions.end();
      };

  const auto pickAssignedRegion =
      [&isAssignedToThisInstance](
          const std::vector<juce::ARAPlaybackRegion *> &candidates)
      -> juce::ARAPlaybackRegion * {
    for (auto *candidate : candidates)
      if (candidate != nullptr && isAssignedToThisInstance(candidate))
        return candidate;

    return nullptr;
  };

  const auto sequenceIndexOf =
      [&sequences](juce::ARAPlaybackRegion *region) -> int {
    if (region == nullptr)
      return -1;
    auto *sequence = region->getRegionSequence();
    if (sequence == nullptr)
      return -1;
    for (int i = 0; i < static_cast<int>(sequences.size()); ++i)
      if (sequences[static_cast<size_t>(i)] == sequence)
        return i;
    return -1;
  };

  juce::ARAPlaybackRegion *target = nullptr;
  bool activatedByTrack = false;
  if (sequences.empty()) {
    if (!regions.empty())
      target = pickAssignedRegion(regions);
    if (target == nullptr && !effective.empty())
      target = pickAssignedRegion(effective);
  } else {
    // D-175: an explicit track selection wins over stale playback regions.
    // The first selected sequence is the most relevant one, as the host orders
    // the list; the policy keeps an item click working when that item is on
    // the selected track.
    std::vector<int> regionSequences;
    regionSequences.reserve(regions.size());
    for (auto *region : regions)
      regionSequences.push_back(sequenceIndexOf(region));

    std::vector<int> effectiveSequences;
    effectiveSequences.reserve(effective.size());
    for (auto *region : effective)
      effectiveSequences.push_back(sequenceIndexOf(region));

    std::vector<int> selectedSequences;
    selectedSequences.reserve(sequences.size());
    for (int i = 0; i < static_cast<int>(sequences.size()); ++i)
      selectedSequences.push_back(i);

    const auto choice = pitchnet::chooseAraSelectionTarget(
        regionSequences, effectiveSequences, selectedSequences);

    if (choice.source == pitchnet::AraSelectionSource::playbackRegion &&
        choice.index >= 0 && choice.index < static_cast<int>(regions.size()))
      target = regions[static_cast<size_t>(choice.index)];
    else if (choice.source ==
                 pitchnet::AraSelectionSource::effectiveRegion &&
             choice.index >= 0 &&
             choice.index < static_cast<int>(effective.size()))
      target = effective[static_cast<size_t>(choice.index)];
    else if (choice.source ==
                 pitchnet::AraSelectionSource::regionSequence &&
             !sequences.empty()) {
      // The selected track has no region in this selection: return to the clip
      // last edited on that track, else its first region.
      const auto trackKey = araTrackKey(sequences.front());
      if (!trackKey.isEmpty()) {
        if (activateAraTrackByKey(trackKey))
          activatedByTrack = true;
      }
    }
  }

#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  {
    juce::String log;
    log << "onNewSelection instance=" << audioProcessor.getDiagInstanceId()
        << " editor=" << ARA_DIAG_PTR(this)
        << " regions=" << static_cast<int>(regions.size())
        << " effective=" << static_cast<int>(effective.size())
        << " sequences=" << static_cast<int>(sequences.size())
        << " renderer=" << static_cast<int>(rendererRegions.size());
    for (auto *region : regions)
      if (region != nullptr)
        log << " R=" << pitchnetRegionSelector(*region);
    for (auto *region : effective)
      if (region != nullptr)
        log << " E=" << pitchnetRegionSelector(*region);
    for (auto *sequence : sequences)
      if (sequence != nullptr)
        log << " S=" << juce::String(sequence->getName());
    log << " target="
        << (target != nullptr ? pitchnetRegionSelector(*target)
                              : juce::String("none"))
        << " track=" << (activatedByTrack ? "yes" : "no");
    ARA_DIAG(log);
  }
#endif

  if (!activatedByTrack && target == nullptr) {
    if (!regions.empty())
      target = regions.front();
    else if (!effective.empty())
      target = effective.front();
    else if (!sequences.empty()) {
      const auto &seqRegions =
          sequences.front()->getPlaybackRegions<juce::ARAPlaybackRegion>();
      if (!seqRegions.empty())
        target = seqRegions.front();
    }
  }

  if (!activatedByTrack && target != nullptr) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *pitchDocController =
                juce::ARADocumentControllerSpecialisation::
                    getSpecialisedDocumentController<
                        PitchNetDocumentController>(araDocController))
          pitchDocController->setCurrentPlaybackRegion(target);
      }
    }
    const double activationStartMs = juce::Time::getMillisecondCounterHiRes();
    audioProcessor.setActiveAraRegion(target);
    const auto activationMs =
        juce::Time::getMillisecondCounterHiRes() - activationStartMs;
    if (mainView != nullptr)
    {
      mainView->noteAraActivationDuration(activationMs);
      mainView->noteAraArchiveRestoreDuration(
          audioProcessor.getLastArchiveRestoreMs());
      const auto timing = audioProcessor.getLastAraActivationTiming();
      mainView->noteAraActivationBreakdown(
          timing.cancelMs, timing.copyMs, timing.parseMs, timing.installMs,
          timing.focusMs, timing.bindMs, timing.totalMs);
    }
    // The canvas is in modification time; the region's host position is not.
    // D-180-R4: while the new project is still hydrating, keep the previous
    // view instead of focusing the new region's start (which paints scroll 0).
#if defined(PITCHNET_D180_R34_LEGACY)
    const bool projectReady = true;
#else
    const auto activeKey = audioProcessor.getActiveAraRegionKey();
    const bool projectReady =
        audioProcessor.hasAraRegionProject(activeKey) &&
        !audioProcessor.araRegionProjectNeedsSourceHydration(activeKey);
#endif
    const auto span = audioProcessor.activeRegionSpanInModificationTime();
    if (mainView != nullptr &&
        pitchnet::shouldApplyRegionFocus(projectReady))
      mainView->focusTimelineRange(span.first, span.second);
  }

  // The selection is also what tells us which track's regions to list, so
  // update the Regions card now instead of waiting for the next poll.
  refreshAraRegionList();
}
#endif

void PitchNetAudioProcessorEditor::setupNonARAMode() {
  // Without ARA the plugin cannot drive the host transport, so play / stop /
  // cycle and cycle-range editing are disabled - capture is the only transport
  // action available here.
  mainView->setHostTransportControlAvailable(false);
  mainView->setRecordControlVisible(true);
  mainView->updateRecordArmState(audioProcessor.isCaptureArmed());
  mainView->setOnRecordArmChanged([this](bool armed) {
    if (armed)
      audioProcessor.startCapture();
    else
      audioProcessor.stopCapture();
  });
  mainView->setOnRequestBackendRender([this](const Project &project) {
    audioProcessor.requestPluginProjectRender(project);
  });
  mainView->setOnRequestBackendPreview(
      [this](const Project &project, int startFrame, int endFrame) {
        const auto &audioData = project.getAudioData();
        const double sampleRate =
            audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                                     : static_cast<double>(SAMPLE_RATE);
        audioProcessor.startPluginPreview(
            static_cast<double>(startFrame) * HOP_SIZE / sampleRate,
            static_cast<double>(endFrame) * HOP_SIZE / sampleRate);
      });
  mainView->setOnStopBackendPreview(
      [this]() { audioProcessor.stopPluginPreview(); });
  mainView->setOnRequestDragAudition(
      [this](const juce::AudioBuffer<float> &buffer, double sampleRate,
             bool wholeSegment) {
        audioProcessor.startPluginAudition(buffer, sampleRate, wholeSegment);
      });
  mainView->setOnStopDragAudition(
      [this]() { audioProcessor.stopPluginAudition(); });

  // Setup host transport control callbacks for non-ARA mode
  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    audioProcessor.requestHostSeek(timeInSeconds);
  });

  mainView->setOnRequestHostLoopRange(nullptr);

  setupHostTransportUiSync(true);
}

void PitchNetAudioProcessorEditor::setupHostTransportUiSync(
    bool includePlayState) {
  auto safeMain =
      juce::Component::SafePointer<juce::Component>(mainView->getComponent());


  if (includePlayState) {
    audioProcessor.getTransportController().setPlayStateCallback(
        [safeMain](bool isPlaying) {
          auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
          if (!view)
            return;

          // P1b: los playState que provocan nuestro propio stop/start durante
          // el flush no deben reabrir la cadena.
          if (view->isFlushChainActive())
            return;
          // P1b fase 3: en el borde de arranque, con bloques sucios, el
          // transporte se retiene (stop -> flush -> start). Sin sucios esto
          // devuelve false y el Play pasa sin ciclo stop/start.
          if (isPlaying && view->flushDirtyBlocksForHostPlay())
            return;

          view->updateHostPlaybackState(isPlaying);
        });
  } else {
    audioProcessor.getTransportController().setPlayStateCallback(nullptr);
  }

  audioProcessor.getTransportController().setPositionCallback(
      [safeMain](double timeInSeconds) {
        auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
        if (!view)
          return;

        view->updatePlaybackPosition(timeInSeconds);
      });

  audioProcessor.getTransportController().setLoopCallback(
      [safeMain](const HostSyncService::LoopInfo &loop) {
        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->updateHostLoopRange(loop.loopStartSeconds, loop.loopEndSeconds,
                                    loop.isLoopEnabled, loop.hasLoopPoints);
      });

  audioProcessor.getTransportController().setTempoCallback(
      [safeMain](const HostSyncService::TempoInfo &tempo) {
        if (!tempo.hasBpm && !tempo.hasTimeSignature)
          return;

        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->updateHostTimelineState(tempo.bpm, tempo.timeSigNumerator,
                                        tempo.timeSigDenominator);
      });

  const auto hostState =
      audioProcessor.getTransportController().getCurrentState();
  if (hostState.tempo.hasBpm || hostState.tempo.hasTimeSignature)
    if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
      view->updateHostTimelineState(hostState.tempo.bpm,
                                    hostState.tempo.timeSigNumerator,
                                    hostState.tempo.timeSigDenominator);

  syncHostLoopSnapshotFromPlayHead();
}

void PitchNetAudioProcessorEditor::syncHostLoopSnapshotFromPlayHead() {
  auto *playHead = audioProcessor.getPlayHead();
  if (!playHead)
    return;

  auto posInfo = playHead->getPosition();
  if (!posInfo.hasValue())
    return;

  double startSeconds = 0.0;
  double endSeconds = 0.0;
  bool hasRange = false;

  if (auto loopPoints = posInfo->getLoopPoints()) {
    if (auto bpm = posInfo->getBpm()) {
      if (*bpm > 0.0) {
        startSeconds = loopPoints->ppqStart * 60.0 / *bpm;
        endSeconds = loopPoints->ppqEnd * 60.0 / *bpm;
        hasRange = endSeconds > startSeconds;
      }
    }
  }

  if (auto *view = mainView.get())
    view->updateHostLoopRange(startSeconds, endSeconds,
                              posInfo->getIsLooping(), hasRange);
}

void PitchNetAudioProcessorEditor::setupCallbacks() {
  // When project data changes (analysis complete or synthesis complete)
  mainView->setOnProjectDataChanged([this]() {
    if (auto *project = mainView->getProject())
      audioProcessor.updateProjectStateFromEditor(*project);
    mainView->bindRealtimeProcessor(audioProcessor.getRealtimeProcessor());
    audioProcessor.getRealtimeProcessor().invalidate();
  });

  // onPitchEditFinished is handled by onProjectDataChanged (called after async
  // synthesis completes) No need for separate callback here
}

void PitchNetAudioProcessorEditor::paint(juce::Graphics &) {
  // MainComponent handles all painting
}

void PitchNetAudioProcessorEditor::resized() {
  audioProcessor.setLastEditorSize(getWidth(), getHeight());
  if (mainView != nullptr)
    mainView->noteAraLayoutStarted();
  mainView->getComponent()->setBounds(getLocalBounds());
  if (mainView != nullptr)
    mainView->noteAraLayoutFinished();
  requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::parentHierarchyChanged() {
  AudioProcessorEditor::parentHierarchyChanged();
  applyLunaSoftwareRenderer();
}

void PitchNetAudioProcessorEditor::visibilityChanged() {
  if (isVisible()) {
    applyLunaSoftwareRenderer();
    requestMainViewKeyboardFocusAsync();
  }
}

void PitchNetAudioProcessorEditor::mouseDown(const juce::MouseEvent &e) {
  juce::ignoreUnused(e);
  requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocus() {
  auto *component = mainView ? mainView->getComponent() : nullptr;
  if (!component || (!component->isShowing() && !component->isOnDesktop()))
    return;

  if (auto *focused = juce::Component::getCurrentlyFocusedComponent()) {
    if (focused == this || isParentOf(focused) || focused == component ||
        component->isParentOf(focused))
      return;
  }

  if (auto *peer = getPeer())
    peer->grabFocus();

  grabKeyboardFocus();
  component->grabKeyboardFocus();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocusAsync() {
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safeThis(this);
  juce::MessageManager::callAsync([safeThis]() {
    if (safeThis != nullptr) {
      safeThis->applyLunaSoftwareRenderer();
      safeThis->requestMainViewKeyboardFocus();
    }
  });
}

void PitchNetAudioProcessorEditor::applyLunaSoftwareRenderer() {
#if JUCE_WINDOWS
  if (lunaSoftwareRendererApplied || !isLunaHostProcess())
    return;

  auto *peer = getPeer();
  if (!peer)
    return;

  const auto engines = peer->getAvailableRenderingEngines();
  const int softwareRenderer = engines.indexOf("Software Renderer");
  if (softwareRenderer < 0) {
    lunaSoftwareRendererApplied = true;
    LOG("LUNA host detected, but JUCE software renderer is unavailable");
    return;
  }

  lunaSoftwareRendererApplied = true;
  if (peer->getCurrentRenderingEngine() != softwareRenderer)
    peer->setCurrentRenderingEngine(softwareRenderer);

  LOG("LUNA host detected: forcing JUCE software renderer");
#endif
}
