#include "Main/DeferredRenderQueue.h"
#include "MainComponent.h"
#include "GpuDeviceList.h"
#include "AraViewFocusPolicy.h"
#include "CursorFollowPolicy.h"
#include "../Plugin/AraDiagnostics.h"
#include "Main/ExportHelper.h"
#include "Main/MacMenuIconHelper.h"
#include "Components/DarkLookAndFeel.h"
#include "../Audio/RealtimePitchProcessor.h"
#include "../Audio/Synthesis/EngineBlocks.h"
#include "../Audio/IO/MidiExporter.h"
#include "../Models/ProjectSerializer.h"
#include "../Utils/AppLogger.h"
#include "../Utils/NotePitch.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/Constants.h"
#include "../Utils/ScaleUtils.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Localization.h"
#include "../Utils/OnnxRuntime.h"
#include "../Utils/OnnxRuntimeLoader.h"
#include "../Utils/PlatformPaths.h"
#include "../Utils/SHA256Utils.h"
#include "../Utils/UI/WindowSizing.h"
#include "BinaryData.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_set>

namespace
{
class EditorTooltipWindow final : public juce::TooltipWindow
{
public:
  juce::String getTipFor(juce::Component &component) override
  {
    for (auto *ancestor = &component; ancestor != nullptr;
         ancestor = ancestor->getParentComponent())
      if (dynamic_cast<MainComponent *>(ancestor) != nullptr)
        return juce::TooltipWindow::getTipFor(component);

    return {};
  }
};

std::shared_ptr<juce::TooltipWindow> getEditorTooltipWindow()
{
  // Share one native window across plugin instances in the host process.
  static std::weak_ptr<juce::TooltipWindow> sharedWindow;
  auto window = sharedWindow.lock();
  if (window == nullptr)
  {
    window = std::make_shared<EditorTooltipWindow>();
    window->setLookAndFeel(&DarkLookAndFeel::getInstance());
    sharedWindow = window;
  }
  return window;
}

constexpr float updateSubtitleFontSize = 14.0f;
constexpr float aboutNoticesFontSize = 12.0f;
const juce::Colour updateLogScrollbarTrack(0xFF0D0B0Bu);
const juce::Colour updateLogScrollbarThumb(0xFF565656u);

juce::File getMcpControlDirectory()
{
  auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                 .getChildFile("kinu-tune-mcp");
  dir.createDirectory();
  return dir;
}

bool writeMcpJsonAtomically(const juce::File &file, const juce::var &value)
{
  const auto temporary = file.getSiblingFile(file.getFileName() + ".tmp");
  if (!temporary.replaceWithText(juce::JSON::toString(value)))
    return false;
  if (file.existsAsFile())
    file.deleteFile();
  return temporary.moveFileTo(file);
}

struct UpdateInfo
{
  juce::String version;
  juce::String releaseNotes;
};

juce::String normalizeVersionString(juce::String version)
{
  version = version.trim();
  if (version.startsWithIgnoreCase("v"))
    version = version.substring(1);
  return version;
}

juce::String getCurrentApplicationVersion()
{
#if defined(JUCE_APPLICATION_VERSION_STRING)
  return JUCE_APPLICATION_VERSION_STRING;
#elif defined(JucePlugin_VersionString)
  return JucePlugin_VersionString;
#else
  return "0.7.2";
#endif
}

std::array<int, 4> parseVersionParts(const juce::String &version)
{
  std::array<int, 4> parts{};
  juce::StringArray tokens;
  tokens.addTokens(normalizeVersionString(version), ".", "");

  for (int i = 0; i < juce::jmin(tokens.size(), static_cast<int>(parts.size())); ++i)
    parts[static_cast<size_t>(i)] = tokens[i].getIntValue();

  return parts;
}

int compareVersions(const juce::String &lhs, const juce::String &rhs)
{
  const auto left = parseVersionParts(lhs);
  const auto right = parseVersionParts(rhs);

  for (size_t i = 0; i < left.size(); ++i)
  {
    if (left[i] < right[i])
      return -1;
    if (left[i] > right[i])
      return 1;
  }

  return 0;
}

std::optional<UpdateInfo> fetchLatestUpdateInfo(juce::WebInputStream &request)
{
  if (!request.connect(nullptr))
    return std::nullopt;

  const auto response = request.readEntireStreamAsString();
  auto parsed = juce::JSON::parse(response);
  auto data = parsed.getProperty("data", {});

  if (!data.isArray())
    return std::nullopt;

  for (const auto &product : *data.getArray())
  {
    if (product.getProperty("appId", "").toString() != "PitchNet")
      continue;

    const auto latestVersion =
        normalizeVersionString(product.getProperty("version", "").toString());

    if (latestVersion.isEmpty() ||
        compareVersions(latestVersion, getCurrentApplicationVersion()) <= 0)
      return std::nullopt;

    UpdateInfo info;
    info.version = latestVersion;
    info.releaseNotes = product.getProperty("log", "").toString();
    return info;
  }

  return std::nullopt;
}

void styleUpdateButton(juce::TextButton &button)
{
  button.setLookAndFeel(&DarkLookAndFeel::getInstance());
  button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF5B5B5Bu));
  button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF3E3E3Eu));
  button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFEFEFEFu));
  button.setColour(juce::TextButton::textColourOnId, juce::Colour(0xFFEFEFEFu));
  button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void styleUpdateLogScrollBars(juce::Component &component)
{
  if (auto *scrollBar = dynamic_cast<juce::ScrollBar *>(&component))
  {
    scrollBar->setLookAndFeel(&DarkLookAndFeel::getInstance());
    scrollBar->setColour(juce::ScrollBar::backgroundColourId, updateLogScrollbarTrack);
    scrollBar->setColour(juce::ScrollBar::trackColourId, updateLogScrollbarTrack);
    scrollBar->setColour(juce::ScrollBar::thumbColourId, updateLogScrollbarThumb);
  }

  for (int i = 0; i < component.getNumChildComponents(); ++i)
    if (auto *child = component.getChildComponent(i))
      styleUpdateLogScrollBars(*child);
}

class UpdateAvailableContent : public juce::Component
{
public:
  UpdateAvailableContent(const juce::String &latestVersion,
                         const juce::String &currentVersion,
                         const juce::String &releaseNotes)
      : latest(latestVersion),
        current(currentVersion),
        cancelButton(TR("update.ask_later")),
        skipButton(TR("update.skip_version")),
        downloadButton(TR("update.download_now"))
  {
    releaseNotesEditor.setMultiLine(true);
    releaseNotesEditor.setReadOnly(true);
    releaseNotesEditor.setScrollbarsShown(true);
    releaseNotesEditor.setScrollBarThickness(8);
    releaseNotesEditor.setCaretVisible(false);
    releaseNotesEditor.setPopupMenuEnabled(false);
    releaseNotesEditor.setText(releaseNotes, juce::dontSendNotification);
    releaseNotesEditor.applyFontToAllText(AppFont::getFont(updateSubtitleFontSize));
    releaseNotesEditor.setColour(juce::TextEditor::backgroundColourId,
                                 juce::Colour(0xFF191717u));
    releaseNotesEditor.setColour(juce::TextEditor::textColourId,
                                 APP_COLOR_TEXT_PRIMARY);
    releaseNotesEditor.setColour(juce::TextEditor::outlineColourId,
                                 juce::Colours::transparentBlack);
    releaseNotesEditor.setColour(juce::TextEditor::focusedOutlineColourId,
                                 juce::Colours::transparentBlack);
    releaseNotesEditor.setColour(juce::TextEditor::shadowColourId,
                                 juce::Colours::transparentBlack);
    releaseNotesEditor.setColour(juce::ScrollBar::backgroundColourId,
                                 updateLogScrollbarTrack);
    releaseNotesEditor.setColour(juce::ScrollBar::trackColourId,
                                 updateLogScrollbarTrack);
    releaseNotesEditor.setColour(juce::ScrollBar::thumbColourId,
                                 updateLogScrollbarThumb);
    releaseNotesEditor.setLookAndFeel(&DarkLookAndFeel::getInstance());
    styleUpdateLogScrollBars(releaseNotesEditor);
    addAndMakeVisible(releaseNotesEditor);

    for (auto *button : {&cancelButton, &skipButton, &downloadButton})
    {
      styleUpdateButton(*button);
      addAndMakeVisible(*button);
    }

    setSize(550, 386);
  }

  ~UpdateAvailableContent() override
  {
    releaseNotesEditor.setLookAndFeel(nullptr);
    cancelButton.setLookAndFeel(nullptr);
    skipButton.setLookAndFeel(nullptr);
    downloadButton.setLookAndFeel(nullptr);
  }

  void resized() override
  {
    auto bounds = getLocalBounds().reduced(24, 20);
    bounds.removeFromTop(36);
    bounds.removeFromTop(40);
    releaseNotesEditor.setBounds(bounds.removeFromTop(231));
    styleUpdateLogScrollBars(releaseNotesEditor);

    auto buttonRow = getLocalBounds().removeFromBottom(46);
    const int buttonY = buttonRow.getY() + 5;
    const int buttonH = 26;
    const int gap = 12;
    const int cancelW = 150;
    const int skipW = 150;
    const int downloadW = 150;
    const int totalW = cancelW + skipW + downloadW + gap * 2;
    int x = (getWidth() - totalW) / 2;

    downloadButton.setBounds(x, buttonY, downloadW, buttonH);
    x += downloadW + gap;
    cancelButton.setBounds(x, buttonY, cancelW, buttonH);
    x += cancelW + gap;
    skipButton.setBounds(x, buttonY, skipW, buttonH);
  }

  void paint(juce::Graphics &g) override
  {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF333333u));
    g.fillRoundedRectangle(bounds, 7.0f);

    g.setColour(juce::Colour(0xFFEFEFEFu));
    g.setFont(AppFont::getFont(16.0f));
    g.drawText(TR("update.title"), 24, 24, getWidth() - 48, 24,
               juce::Justification::centred, false);

    g.setFont(juce::Font(juce::FontOptions(updateSubtitleFontSize)).boldened());
    juce::ignoreUnused(latest, current);
    g.drawFittedText(TR("update.message"),
                     24, 54, getWidth() - 48, 34,
                     juce::Justification::centred, 2);
  }

  std::function<void()> onCancel;
  std::function<void()> onSkip;
  std::function<void()> onDownload;

  juce::TextButton cancelButton;
  juce::TextButton skipButton;
  juce::TextButton downloadButton;

private:
  juce::String latest;
  juce::String current;
  juce::TextEditor releaseNotesEditor;
};

class UpdateAvailableDialog : public juce::DialogWindow
{
public:
  UpdateAvailableDialog(juce::Component *parent,
                        const juce::String &latestVersion,
                        const juce::String &releaseNotes,
                        std::function<void()> onSkipVersion,
                        std::function<void()> onDownloadUpdate)
      : juce::DialogWindow("", APP_COLOR_BACKGROUND, true),
        skipCallback(std::move(onSkipVersion)),
        downloadCallback(std::move(onDownloadUpdate))
  {
    setOpaque(false);
    setUsingNativeTitleBar(false);
    setTitleBarHeight(0);
    setResizable(false, false);
    setTitleBarButtonsRequired(0, false);

    auto *content = new UpdateAvailableContent(latestVersion,
                                               getCurrentApplicationVersion(),
                                               releaseNotes);
    content->onCancel = [this] { closeButtonPressed(); };
    content->onSkip = [this]
    {
      if (skipCallback)
        skipCallback();
      closeButtonPressed();
    };
    content->onDownload = [this]
    {
      if (downloadCallback)
        downloadCallback();
      closeButtonPressed();
    };
    content->cancelButton.onClick = content->onCancel;
    content->skipButton.onClick = content->onSkip;
    content->downloadButton.onClick = content->onDownload;

    setContentOwned(content, true);
    setSize(550, 386);

    if (parent != nullptr)
      centreAroundComponent(parent, getWidth(), getHeight());
    else
      centreWithSize(getWidth(), getHeight());
  }

  void closeButtonPressed() override
  {
    exitModalState(0);
  }

  void paint(juce::Graphics &g) override
  {
    juce::ignoreUnused(g);
  }

private:
  std::function<void()> skipCallback;
  std::function<void()> downloadCallback;
};

class AboutContent : public juce::Component
{
public:
  AboutContent()
      : closeButton(TR("dialog.close"))
  {
    noticesEditor.setMultiLine(true);
    noticesEditor.setReadOnly(true);
    noticesEditor.setScrollbarsShown(true);
    noticesEditor.setScrollBarThickness(8);
    noticesEditor.setCaretVisible(false);
    noticesEditor.setPopupMenuEnabled(false);
    noticesEditor.setText(
        juce::String::fromUTF8(BinaryData::THIRD_PARTY_NOTICES_txt,
                               BinaryData::THIRD_PARTY_NOTICES_txtSize),
        juce::dontSendNotification);
    noticesEditor.applyFontToAllText(AppFont::getFont(aboutNoticesFontSize));
    noticesEditor.setColour(juce::TextEditor::backgroundColourId,
                            juce::Colour(0xFF191717u));
    noticesEditor.setColour(juce::TextEditor::textColourId,
                            APP_COLOR_TEXT_PRIMARY);
    noticesEditor.setColour(juce::TextEditor::outlineColourId,
                            juce::Colours::transparentBlack);
    noticesEditor.setColour(juce::TextEditor::focusedOutlineColourId,
                            juce::Colours::transparentBlack);
    noticesEditor.setColour(juce::TextEditor::shadowColourId,
                            juce::Colours::transparentBlack);
    noticesEditor.setColour(juce::ScrollBar::backgroundColourId,
                            updateLogScrollbarTrack);
    noticesEditor.setColour(juce::ScrollBar::trackColourId,
                            updateLogScrollbarTrack);
    noticesEditor.setColour(juce::ScrollBar::thumbColourId,
                            updateLogScrollbarThumb);
    noticesEditor.setLookAndFeel(&DarkLookAndFeel::getInstance());
    styleUpdateLogScrollBars(noticesEditor);
    addAndMakeVisible(noticesEditor);

    styleUpdateButton(closeButton);
    addAndMakeVisible(closeButton);

    setSize(550, 386);
  }

  ~AboutContent() override
  {
    noticesEditor.setLookAndFeel(nullptr);
    closeButton.setLookAndFeel(nullptr);
  }

  void resized() override
  {
    auto bounds = getLocalBounds().reduced(24, 20);
    noticesEditor.setBounds(bounds.removeFromTop(311));
    styleUpdateLogScrollBars(noticesEditor);

    auto buttonRow = getLocalBounds().removeFromBottom(46);
    const int buttonW = 150;
    const int buttonH = 26;
    closeButton.setBounds((getWidth() - buttonW) / 2, buttonRow.getY() + 5,
                          buttonW, buttonH);
  }

  void paint(juce::Graphics &g) override
  {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF333333u));
    g.fillRoundedRectangle(bounds, 7.0f);
  }

  std::function<void()> onClose;
  juce::TextButton closeButton;

private:
  juce::TextEditor noticesEditor;
};

class AboutDialog : public juce::DialogWindow
{
public:
  explicit AboutDialog(juce::Component *parent)
      : juce::DialogWindow("", APP_COLOR_BACKGROUND, true)
  {
    setOpaque(false);
    setUsingNativeTitleBar(false);
    setTitleBarHeight(0);
    setResizable(false, false);
    setTitleBarButtonsRequired(0, false);

    auto *content = new AboutContent();
    content->onClose = [this] { closeButtonPressed(); };
    content->closeButton.onClick = content->onClose;

    setContentOwned(content, true);
    setSize(550, 386);

    if (parent != nullptr)
      centreAroundComponent(parent, getWidth(), getHeight());
    else
      centreWithSize(getWidth(), getHeight());
  }

  void closeButtonPressed() override
  {
    exitModalState(0);
  }

  void paint(juce::Graphics &g) override
  {
    juce::ignoreUnused(g);
  }
};
} // namespace

class UiBrightnessEffect final : public juce::ImageEffectFilter
{
public:
  void setBrightnessPercent(double percent)
  {
    brightnessFactor = static_cast<float>(
        juce::jlimit(75.0, 200.0, percent) / 100.0);
  }

  void applyEffect(juce::Image &sourceImage, juce::Graphics &destContext,
                   float scaleFactor, float alpha) override
  {
    juce::ignoreUnused(scaleFactor);
    juce::Image::BitmapData pixels(sourceImage,
                                   juce::Image::BitmapData::readWrite);

    const auto adjustedChannel = [this](juce::uint8 channel,
                                        int maximum = 255)
    {
      return static_cast<juce::uint8>(juce::jlimit(
          0, maximum,
          static_cast<int>(std::round(static_cast<float>(channel) *
                                      brightnessFactor))));
    };

    for (int y = 0; y < pixels.height; ++y)
    {
      auto *line = pixels.getLinePointer(y);
      for (int x = 0; x < pixels.width; ++x)
      {
        auto *pixelData = line + x * pixels.pixelStride;
        if (pixels.pixelFormat == juce::Image::RGB)
        {
          auto &pixel = *reinterpret_cast<juce::PixelRGB *>(pixelData);
          pixel.setARGB(255, adjustedChannel(pixel.getRed()),
                        adjustedChannel(pixel.getGreen()),
                        adjustedChannel(pixel.getBlue()));
        }
        else if (pixels.pixelFormat == juce::Image::ARGB)
        {
          auto &pixel = *reinterpret_cast<juce::PixelARGB *>(pixelData);
          const auto pixelAlpha = pixel.getAlpha();
          pixel.setARGB(pixelAlpha,
                        adjustedChannel(pixel.getRed(), pixelAlpha),
                        adjustedChannel(pixel.getGreen(), pixelAlpha),
                        adjustedChannel(pixel.getBlue(), pixelAlpha));
        }
      }
    }

    destContext.setOpacity(alpha);
    destContext.drawImageAt(sourceImage, 0, 0, false);
  }

private:
  float brightnessFactor = 1.0f;
};

MainComponent::MainComponent(bool enableAudioDevice)
    : enableAudioDeviceFlag(enableAudioDevice), pianoRollView(pianoRoll)
{
  // Without the bundled ONNX Runtime analysis is off (see OnnxRuntime.h).
  // Say so when the first editor opens (or analysis is first attempted).
  if (!OnnxRuntime::isAvailable())
    OnnxRuntime::showUnavailableMessageOnce();

  LOG("MainComponent: constructor start");
  setSize(WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight);
  setOpaque(true); // Required for native title bar

  tooltipWindow = getEditorTooltipWindow();

  LOG("MainComponent: creating core components...");
  // Initialize components
  if (enableAudioDeviceFlag)
  {
    ownedEditorController = std::make_unique<EditorController>(true);
    editorController = ownedEditorController.get();
    if (auto *project = editorController->getProject())
      project->setTimelineDisplayMode(TimelineDisplayMode::Time);

  }
  juce::Component::SafePointer<MainComponent> safeThis(this);
  resampledLoopAudition = std::make_unique<ResampledLoopAudition>(
      [safeThis](juce::AudioBuffer<float> preview) mutable
      {
        if (!safeThis || preview.getNumSamples() == 0 ||
            !safeThis->noteDragAuditionActive)
          return;
        double auditionSampleRate = SAMPLE_RATE;
        if (auto *project = safeThis->getProject())
          auditionSampleRate = project->getAudioData().sampleRate > 0
              ? static_cast<double>(project->getAudioData().sampleRate)
              : auditionSampleRate;
        constexpr auto source = AuditionOutput::Source::DraggedSegment;
        if (safeThis->isPluginMode() &&
            !safeThis->noteDragAuditionBackendPreviewStarted)
        {
          // Start the host/ARA preview using the established transport path.
          // The loop below then replaces that path's source with the
          // resampled audition buffer.
          if (auto *project = safeThis->getProject())
            safeThis->auditionOutput.startHostPreview(
                source, *project, safeThis->noteDragAuditionStartFrame,
                safeThis->noteDragAuditionEndFrame);
          safeThis->noteDragAuditionBackendPreviewStarted = true;
        }
        // Whole segment, repeated from its start until mouse up.
        safeThis->auditionOutput.playLoop(source, preview, auditionSampleRate,
                                          /*wholeSegment*/ true);
      });
  auditionOutput.setEngine(
      [safeThis]() -> AudioEngine *
      {
        return safeThis && safeThis->editorController
                   ? safeThis->editorController->getAudioEngine()
                   : nullptr;
      },
      [safeThis] { return safeThis && safeThis->isPluginMode(); });
  // What each source does when another one (stopSound false) or a transport
  // command (stopSound true) takes the output.
  auditionOutput.setOnTakenOver(AuditionOutput::Source::DraggedSegment,
                                [safeThis](bool stopSound)
                                {
                                  if (safeThis)
                                    safeThis->abandonDragAudition(stopSound);
                                });
  auditionOutput.setOnTakenOver(AuditionOutput::Source::PianoKey,
                                [safeThis](bool stopSound)
                                {
                                  if (!safeThis)
                                    return;
                                  if (stopSound)
                                    safeThis->finishPianoKeyAudition();
                                  else // the new loop replaced the tone
                                    safeThis->pianoKeyAuditionActive = false;
                                });
  auditionOutput.setOnTakenOver(AuditionOutput::Source::RegionPreview,
                                [safeThis](bool)
                                {
                                  if (safeThis)
                                    safeThis->finishPreviewRegion(false);
                                });
  if (!isPluginMode())
  {
    ownedUndoManager = std::make_unique<PitchUndoManager>(100);
    undoManager = ownedUndoManager.get();
  }
  commandManager = std::make_unique<juce::ApplicationCommandManager>();
  if (undoManager)
  {
    undoManager->onHistoryChanged = [this]()
    {
      if (commandManager)
        commandManager->commandStatusChanged();
      toolbar.setUndoRedoEnabled(undoManager && undoManager->canUndo(),
                                 undoManager && undoManager->canRedo());
    };
  }

  // Initialize new modular components
  fileManager = std::make_unique<AudioFileManager>();
  settingsManager = std::make_unique<SettingsManager>();
  uiBrightnessEffect = std::make_unique<UiBrightnessEffect>();
  if (!isPluginMode())
    menuHandler = std::make_unique<MenuHandler>();

  LOG("MainComponent: loading ONNX models...");
  if (editorController)
  {
    editorController->setPitchDetectorType(
        settingsManager->getPitchDetectorType());
    editorController->setSynthesisEngineType(
        settingsManager->getSynthesisEngineType());
    editorController->setDeviceConfig(settingsManager->getDevice(),
                                      settingsManager->getGPUDeviceId());
    editorController->reloadInferenceModels(false);
  }

  LOG("MainComponent: wiring up components...");
  recentFiles = settingsManager->getRecentFiles();
  if (menuHandler)
  {
    menuHandler->setUndoManager(undoManager);
    menuHandler->setCommandManager(commandManager.get());
    menuHandler->setPluginMode(false);
    refreshRecentFilesMenu();
    menuHandler->setOnRecentFileSelected(
        [this](const juce::File &file)
        { openRecentFile(file); });
  }
  settingsManager->setVocoder(editorController ? editorController->getVocoder()
                                               : nullptr);

  // Load vocoder settings
  settingsManager->applySettings();

  LOG("MainComponent: initializing audio device...");
  // Initialize audio (standalone app only)
  if (auto *audioEngine = editorController
                              ? editorController->getAudioEngine()
                              : nullptr)
    audioEngine->initializeAudio(
        settingsManager->getAudioDeviceState(),
        settingsManager->getFollowSystemAudioOutput());
  LOG("MainComponent: audio initialized");

  LOG("MainComponent: setting up callbacks...");

  // Initialize view state from settings
  pianoRollView.setOverviewVisible(settingsManager->getOverviewVisible());
  pianoRollView.onOverviewVisibilityChanged = [this](bool visible)
  {
    if (auto *project = getProject())
      project->thumbnailVisible = visible;
    settingsManager->setOverviewVisible(visible);
    settingsManager->saveConfig();
  };
  toolbar.onTrackViewModeChanged = [this](bool track)
  {
    pianoRollView.setTrackViewMode(track);
    toolbar.setTrackViewMode(track);
    settingsManager->setTrackViewMode(track);
    settingsManager->saveConfig();
    if (onTrackViewModeChanged)
      onTrackViewModeChanged(track);
  };
  pianoRoll.setShowDeltaPitch(settingsManager->getShowDeltaPitch());
  pianoRoll.setShowBasePitch(settingsManager->getShowBasePitch());
  pianoRoll.setShowSegmentsDebug(
      settingsManager->getShowSegmentsDebug());
  pianoRoll.setShowGameValuesDebug(
      settingsManager->getShowGameValuesDebug());
  pianoRoll.setShowNoteFramesDebug(
      settingsManager->getShowNoteFramesDebug());
  pianoRoll.setShowUvInterpolationDebug(
      settingsManager->getShowUvInterpolationDebug());
  pianoRoll.setShowActualF0Debug(
      settingsManager->getShowActualF0Debug());
  pianoRoll.setShowCleanedF0Debug(
      settingsManager->getShowCleanedF0Debug());
  pianoRoll.setShowVocoderF0Debug(
      settingsManager->getShowVocoderF0Debug());
  pianoRollView.setShowSegmentsDebug(
      settingsManager->getShowSegmentsDebug());
  dragAuditionEnabled = settingsManager->getLiveAuditionEnabled();
  toolbar.setAuditionEnabled(dragAuditionEnabled);
  pianoRollView.onAutoZoomRequested = [this]()
  {
    if (auto *project = getProject())
      fitAnalyzedPitchRangeToView(*project);
  };
  pianoRoll.onZoomFullRequested = [this] { zoomToFull(); };

  // Add child components - standalone uses menus; plugin instances do not.
#if JUCE_MAC
#else
  if (!isPluginMode() && menuHandler)
  {
    menuBar.setModel(menuHandler.get());
    menuBar.setLookAndFeel(&menuBarLookAndFeel);
    addAndMakeVisible(menuBar);
  }
#endif
  addAndMakeVisible(toolbar);
  addAndMakeVisible(workspace);
  addChildComponent(analysisBackdrop);
  addChildComponent(analysisProgressPopup);

  // Setup workspace with stacked piano roll + overview cards
  workspace.setMainContent(&pianoRollView);
  workspace.getMainCard().setBackgroundColour(juce::Colours::transparentBlack);
  workspace.getMainCard().setBorderColour(juce::Colours::transparentBlack);

  // VariAudio-style inspector: always visible on the left, harmony included,
  // so the one-click harmony buttons never hide behind a panel toggle.
  variAudioInspector.getCurrentProject = [this] { return getProject(); };
  using InspectorParam = PianoRollComponent::InspectorParam;
  variAudioInspector.getParamValue = [this](VariAudioInspector::Param param)
  { return pianoRoll.getInspectorValue(static_cast<InspectorParam>(param)); };
  variAudioInspector.getTargetCount = [this]
  {
    const auto targets = pianoRoll.getInspectorTargets();
    const bool selection = !targets.empty() && targets.front()->isSelected();
    return std::pair<int, bool>{static_cast<int>(targets.size()), selection};
  };
  variAudioInspector.onParamEditBegin = [this](VariAudioInspector::Param param)
  { pianoRoll.beginInspectorEdit(static_cast<InspectorParam>(param)); };
  variAudioInspector.onParamEditValue = [this](float value)
  { pianoRoll.setInspectorEditValue(value); };
  variAudioInspector.onParamEditEnd = [this] { pianoRoll.endInspectorEdit(); };
  variAudioInspector.getNoteEngine = [this]
  { return getSelectionNoteEngine(); };
  variAudioInspector.getEngineStatus =
      [this]() -> std::optional<VariAudioInspector::EngineStatus>
  {
    auto *project = getProject();
    if (project == nullptr)
      return std::nullopt;
    const auto targets = pianoRoll.getInspectorTargets();
    if (targets.empty() || targets.front() == nullptr)
      return std::nullopt;
    const auto &notes = project->getNotes();
    int index = -1;
    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
      if (&notes[static_cast<size_t>(i)] == targets.front())
      {
        index = i;
        break;
      }
    if (index < 0)
      return std::nullopt;
    const auto globalEngine = editorController
                                  ? editorController->getSynthesisEngineType()
                                  : defaultSynthesisEngineType();
    VariAudioInspector::EngineStatus status;
    status.effective = engineBlocks::effectiveEngine(
        notes[static_cast<size_t>(index)], globalEngine);
    if (const auto it = lastRefusedEngineRequests.find(index);
        it != lastRefusedEngineRequests.end())
      status.refusedRequest = it->second;
    return status;
  };
  variAudioInspector.onNoteEngineChanged = [this](NoteEngine engine)
  { applyNoteEngineToSelection(engine); };
  variAudioInspector.onSnapModeChanged =
      [this](VariAudioInspector::SnapMode mode)
  {
    pianoRoll.setSnapToSemitoneDrag(mode != VariAudioInspector::SnapMode::Off);
    if (mode != VariAudioInspector::SnapMode::Off)
      pianoRoll.setAbsoluteDragSnap(mode == VariAudioInspector::SnapMode::Absolute);
    parameterPanel.updateGlobalSliders();
    variAudioInspector.refresh();
  };
  variAudioInspector.onShowAllSmartControlsChanged = [this](bool showAll)
  { pianoRoll.setShowAllSmartControls(showAll); };
  variAudioInspector.onDragSnapModeChanged = [this](DragSnapMode mode)
  {
    pianoRoll.setDragSnapMode(mode);
    parameterPanel.updateGlobalSliders();
    notifyProjectDataChanged();
  };
  auto &harmonyPanel = variAudioInspector.getHarmonyPanel();
  harmonyPanel.onHarmonyRequested =
      [this](HarmonyUtils::Interval interval, bool up)
  { return pianoRoll.applyHarmonyInterval(interval, up); };
  variAudioInspector.isProjectKeyScope = [this] { return isTrackViewMode(); };
  variAudioInspector.onEditorScaleChanged = [this]
  {
    pianoRoll.refreshScaleFromProject();
    parameterPanel.updateGlobalSliders();
    notifyProjectDataChanged();
  };
  workspace.setInspectorContent(&variAudioInspector,
                                VariAudioInspector::preferredWidth);

  // Add parameter panel to workspace.
  workspace.addPanel("parameters", TR("panel.parameters"), &parameterPanel,
                     false);

  // Left side panel (slides in from the left, same width as the right panel).
  workspace.setLeftPanelContent("tracks", TR("panel.tracks"), &trackListPanel);
  trackListPanel.onPreferredHeightChanged = [this]()
  {
    workspace.refreshLeftPanelContentHeight();
  };
  trackListPanel.setUseDawTrackColour(settingsManager->getUseDawTrackColour());
  trackListPanel.onUseDawTrackColourChanged = [this](bool use)
  {
    settingsManager->setUseDawTrackColour(use);
    settingsManager->saveConfig();
    // Pinned notes are outlined in the track colour; redraw them.
    if (onTrackColourModeChanged)
      onTrackColourModeChanged();
  };

  // Configure toolbar for plugin mode
  if (isPluginMode())
    toolbar.setPluginMode(true);
  toolbar.setTransportEnabled(isPluginMode());

  // Set undo manager for piano roll
  pianoRoll.setUndoManager(undoManager);
  parameterPanel.setUndoManager(undoManager);
  parameterPanel.setPluginMode(isPluginMode());
  parameterPanel.setUiBrightness(settingsManager->getUiBrightnessPercent());
  applyUiBrightness(settingsManager->getUiBrightnessPercent());
  parameterPanel.onProjectBound = [this](Project *)
  {
    // Every path that rebinds the project goes through the parameter panel,
    // so the inspector and harmony card follow it from here.
    variAudioInspector.getHarmonyPanel().refreshProject();
    variAudioInspector.refresh();
    if (hasCachedHostTimelineState)
      updateHostTimelineState(cachedHostTempoBpm, cachedHostBeatNumerator,
                              cachedHostBeatDenominator);
  };
  // The Regions card changes the panel's natural height when it appears, and
  // that height is what the side panel scrolls against.
  parameterPanel.onPreferredHeightChanged = [this]()
  {
    workspace.refreshPanelContentHeight("parameters");
  };
  parameterPanel.onUiBrightnessChanged = [this](double brightnessPercent)
  {
    settingsManager->setUiBrightnessPercent(brightnessPercent);
    applyUiBrightness(brightnessPercent);
  };
  parameterPanel.onLanguageChanged = [this](const juce::String &languageCode)
  {
    // The panel has already switched the language; this only records it.
    if (settingsManager == nullptr)
      return;
    settingsManager->setLanguage(languageCode);
    settingsManager->saveConfig();
  };
  parameterPanel.setSynthesisEngine(settingsManager->getSynthesisEngineType());
  parameterPanel.onSynthesisEngineChanged = [this](SynthesisEngineType type)
  {
    settingsManager->setSynthesisEngineType(type);
    settingsManager->saveConfig();
    // The preference above is the default for new documents. This document
    // records its own choice so save/reopen does not hand it the engine of
    // whichever other audio was modified last (D-174, I-19).
    if (auto *project = getProject())
      project->setSynthesisEngine(type);
    if (editorController)
      editorController->setSynthesisEngineType(type);
    // In plugin mode this controller writes the shared engine preference, so
    // processor-owned ARA/background controllers observe the same global engine.
    // Otherwise the switch is only a setting: the audio already rendered by
    // the previous engine would stay until the user happened to edit again.
    rerenderEditedNotes();
    notifyProjectDataChanged();
  };
  parameterPanel.setAiResynthesisAvailable(GpuDeviceList::hasGpuInference());
  refreshRenderDeviceOptions();
  parameterPanel.onRenderDeviceChanged = [this](int deviceId)
  {
    if (settingsManager == nullptr)
      return;

    // Same rule the Settings dialog enforces: swapping the device out from
    // under a running inference is what the guard is there to prevent. Put the
    // button back on the device still in use.
    if (isInferenceBusy())
    {
      refreshRenderDeviceOptions();
      return;
    }

    settingsManager->setGPUDeviceId(deviceId);
    settingsManager->saveConfig();
    settingsManager->applySettings();
    reloadInferenceModels(true);

    // The Settings dialog, if it has been opened, is showing the old device.
    if (settingsOverlay != nullptr &&
        settingsOverlay->getSettingsComponent() != nullptr)
      settingsOverlay->getSettingsComponent()->loadSettings();
  };

  // Setup toolbar callbacks
  toolbar.onPlay = [this]()
  {
    playAfterFlush = true;
    flushDirtyBlocks(); // render the dirty blocks first, one per pass
    if (!flushChainActive)
    {
      playAfterFlush = false;
      play();
    }
  };
  toolbar.onPause = [this]()
  { pause(); };
  toolbar.onStop = [this]()
  { stop(); };
  toolbar.onZoomChanged = [this](float pps)
  { onZoomChanged(pps); };
  pianoRoll.onEditModeRequested = [this](EditMode mode)
  { setEditMode(mode); };
  toolbar.onEditModeChanged = [this](EditMode mode)
  { setEditMode(mode); };
  toolbar.onToggleLoop = [this](bool enabled)
  {
    if (auto *project = getProject())
    {
      project->setLoopEnabled(enabled);
      const auto &range = project->getLoopRange();
      const bool hasValidRange = range.endSeconds > range.startSeconds;
      toolbar.setLoopEnabled(enabled);
      pianoRoll.repaint();

      if (isPluginMode() && onRequestHostLoopRange)
        onRequestHostLoopRange(pianoRoll.projectToTimeline(range.startSeconds),
                               pianoRoll.projectToTimeline(range.endSeconds),
                               range.enabled, hasValidRange);

      if (auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr)
      {
        if (hasValidRange)
          audioEngine->setLoopRange(range.startSeconds, range.endSeconds);
        audioEngine->setLoopEnabled(range.enabled);
      }
    }
    else
    {
      toolbar.setLoopEnabled(false);
      pianoRoll.repaint();
    }
  };
  toolbar.onToggleParameters = [this](bool visible)
  {
    workspace.showPanel("parameters", visible);
  };
  toolbar.onToggleLeftPanel = [this](bool visible)
  {
    workspace.showLeftPanel(visible);
  };
  pianoRoll.onUndoRequested = [this]() { undo(); };
  pianoRoll.onRedoRequested = [this]() { redo(); };
  toolbar.onUndo = [this]() { undo(); };
  toolbar.onRedo = [this]() { redo(); };
  toolbar.onToggleRecord = [this](bool armed) {
    if (onRecordArmChanged)
      onRecordArmChanged(armed);
  };
  toolbar.onToggleAudition = [this](bool enabled)
  {
    dragAuditionEnabled = enabled;
    if (settingsManager)
    {
      settingsManager->setLiveAuditionEnabled(enabled);
      settingsManager->saveConfig();
    }
    if (!dragAuditionEnabled)
      cancelDraggedNoteAudition();
  };
  toolbar.setUndoRedoEnabled(undoManager && undoManager->canUndo(),
                             undoManager && undoManager->canRedo());

  // Removed onRender callback - Melodyne-style: edits automatically trigger
  // real-time processing

  // Setup piano roll callbacks
  pianoRoll.onInactiveRegionClicked = [this](const juce::String &regionKey)
  {
    if (!onRegionActivationRequested)
      return;
    const bool wasFromCanvas = regionActivationFromCanvas;
    regionActivationFromCanvas = true;
    onRegionActivationRequested(regionKey);
    regionActivationFromCanvas = wasFromCanvas;
  };
  pianoRoll.onSeek = [this](double time)
  { seek(time); };
  pianoRoll.onCanvasEmptyDoubleClick = [this](double time)
  {
    seek(time);
    if (isPlaying)
      pause();
    else
      play();
  };
  pianoRoll.onPreviewRegionRequested = [this](int startFrame, int endFrame)
  { previewNoteRegion(startFrame, endFrame); };
  pianoRoll.onNoteSelected = [this](Note *note)
  { onNoteSelected(note); };
  pianoRoll.onPitchEdited = [this]()
  { onPitchEdited(); };
  pianoRoll.onAmplitudeEdited = [this]()
  {
    if (auto* project = getProject())
    {
      if (!isPluginMode() && editorController)
        if (auto* engine = editorController->getAudioEngine())
          engine->loadWaveform(project->getAudioData().waveform,
                               project->getAudioData().sampleRate, true);
      onPitchEdited();
      notifyProjectDataChanged();
    }
  };
  pianoRoll.onHistoryActionApplied = [this](bool requiresResynthesis)
  { afterHistoryChange(requiresResynthesis); };
  pianoRoll.onPitchEditFinished = [this]()
  {
    markRenderDirty();
    // Melodyne-style: trigger real-time processor update in plugin mode
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };
  pianoRoll.onPitchPreviewRenderRequested = [this]()
  {
    markRenderDirty();
  };
  pianoRoll.onPitchEditCommitted = [this]()
  {
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };
  pianoRoll.onNoteDragAudition = [this](const Note &note)
  { auditionDraggedNote(note); };
  pianoRoll.onNoteDragAuditionFinished = [this]()
  { finishDraggedNoteAudition(); };
  pianoRoll.onPianoKeyAudition = [this](int midiNote)
  { auditionPianoKey(midiNote); };
  pianoRoll.onPianoKeyAuditionFinished = [this]()
  { finishPianoKeyAudition(); };
  pianoRoll.onZoomChanged = [this](float pps)
  {
    onZoomChanged(pps);
    pianoRollView.repaintOverviewViewport();
  };
  pianoRoll.onScrollChanged = [this](double x)
  {
    // D-180-R: measurement and guard. PianoRollComponent::updateScrollBars()
    // updates the scrollbar range during a host layout; the bar can echo a
    // transient range back as scroll 0. That is not a user scroll.
    const double cursor = pianoRoll.getCursorTime();
    const double previous = std::max(latencyTrace.lastCallbackScrollX,
                                     latencyTrace.layoutScrollGuardScroll);
    if (pitchnet::isTransientScrollbarReset(
            latencyTrace.layoutScrollGuardActive, previous, x,
            latencyTrace.layoutScrollGuardCursorVisible))
    {
      pianoRoll.setScrollX(latencyTrace.layoutScrollGuardScroll);
      ++latencyTrace.layoutScrollGuardRestoreCount;
      latencyTrace.lastCallbackScrollX =
          latencyTrace.layoutScrollGuardScroll;
      pianoRollView.repaintOverviewViewport();
      return;
    }
    if (latencyTrace.lastCallbackScrollX > 100.0 && x < 1.0 &&
        std::abs(cursor) > 10.0)
    {
      ++latencyTrace.callbackResetCount;
      latencyTrace.callbackResetFrom = latencyTrace.lastCallbackScrollX;
      latencyTrace.callbackResetTo = x;
      latencyTrace.callbackResetAtMs = nowMs();
    }
    latencyTrace.lastCallbackScrollX = x;
    juce::ignoreUnused(x);
    pianoRollView.repaintOverviewViewport();
  };
  pianoRoll.onLoopRangeChanged = [this](const LoopRange &range)
  {
    toolbar.setLoopEnabled(range.enabled);
    pianoRoll.repaint();
    if (isPluginMode() && onRequestHostLoopRange)
      onRequestHostLoopRange(pianoRoll.projectToTimeline(range.startSeconds),
                             pianoRoll.projectToTimeline(range.endSeconds),
                             range.enabled,
                             range.endSeconds > range.startSeconds);

    if (auto *audioEngine = editorController
                                ? editorController->getAudioEngine()
                                : nullptr)
    {
      audioEngine->setLoopRange(range.startSeconds, range.endSeconds);
      audioEngine->setLoopEnabled(range.enabled);
    }
  };

  // Setup parameter panel callbacks
  parameterPanel.onParameterChanged = [this]()
  {
    onPitchEdited();
    notifyProjectDataChanged();
  };
  parameterPanel.onParameterEditFinished = [this]()
  {
    markRenderDirty();
    // Melodyne-style: trigger real-time processor update in plugin mode
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };
  parameterPanel.onScaleRootChanged = [this](int rootNote)
  {
    pianoRoll.setScaleRootNote(rootNote);
  };
  parameterPanel.onScaleModeChanged = [this](ScaleMode mode)
  {
    pianoRoll.setScaleMode(mode);
  };
  parameterPanel.onSnapToSemitonesChanged = [this](bool enabled)
  {
    pianoRoll.setSnapToSemitoneDrag(enabled);
  };
  parameterPanel.onDragSnapModeChanged = [this](DragSnapMode mode)
  {
    pianoRoll.setDragSnapMode(mode);
    notifyProjectDataChanged();
  };
  parameterPanel.onPitchReferenceChanged = [this](int hz)
  {
    pianoRoll.setPitchReferenceHz(hz);
    notifyProjectDataChanged();
  };
  parameterPanel.onTimelineDisplayModeChanged =
      [this](TimelineDisplayMode mode)
  {
    pianoRoll.setTimelineDisplayMode(mode);
    notifyProjectDataChanged();
  };
  parameterPanel.onTimelineBeatSignatureChanged =
      [this](int numerator, int denominator)
  {
    pianoRoll.setTimelineBeatSignature(numerator, denominator);
    notifyProjectDataChanged();
  };
  parameterPanel.onTimelineTempoChanged = [this](double bpm)
  {
    pianoRoll.setTimelineTempoBpm(bpm);
    notifyProjectDataChanged();
  };
  parameterPanel.onTimelineGridDivisionChanged =
      [this](TimelineGridDivision division)
  {
    pianoRoll.setTimelineGridDivision(division);
    notifyProjectDataChanged();
  };
  parameterPanel.onTimelineSnapCycleChanged = [this](bool enabled)
  {
    pianoRoll.setTimelineSnapCycle(enabled);
    notifyProjectDataChanged();
  };
  parameterPanel.setProject(getProject());

  // Sync toolbar toggle with panel visibility
  toolbar.setParametersVisible(workspace.isPanelVisible("parameters"));
  workspace.onPanelVisibilityChanged = [this](const juce::String &id, bool visible)
  {
    if (id == "parameters")
      toolbar.setParametersVisible(visible);
  };
  toolbar.setLeftPanelVisible(workspace.isLeftPanelVisible());
  workspace.onLeftPanelVisibilityChanged = [this](bool visible)
  {
    toolbar.setLeftPanelVisible(visible);
  };
  workspace.onLayoutAnimationUpdated = [this]()
  {
    repaint(0, toolbar.getBottom(), getWidth(), 1);
  };

  // Setup audio engine callbacks
  if (auto *audioEngine = editorController
                              ? editorController->getAudioEngine()
                              : nullptr)
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);

    audioEngine->setPositionCallback([safeThis](double position)
                                     {
      if (safeThis == nullptr)
        return;
      // Throttle cursor updates - store position and let timer handle it
      safeThis->pendingCursorTime.store(position);
      safeThis->hasPendingCursorUpdate.store(true); });

    audioEngine->setFinishCallback([safeThis]()
                                   {
      if (safeThis == nullptr)
        return;
      safeThis->isPlaying = false;
      safeThis->toolbar.setPlaying(false);
      safeThis->pianoRoll.setPlaybackActive(false);
      // With Auto Scroll off, don't jump the view back to the start.
      safeThis->seek(0.0, safeThis->toolbar.isFollowPlayback()); });
  }

  // Set initial project
  pianoRoll.setProject(getProject());
  pianoRollView.setProject(getProject());

  // Register commands with the command manager
  commandManager->registerAllCommandsForTarget(this);
  commandManager->setFirstCommandTarget(this);

  if (menuHandler)
    menuHandler->setApplicationCommandManagerToWatch(commandManager.get());

#if JUCE_MAC
  if (!isPluginMode() && menuHandler)
  {
    macExtraAppleMenuItems = menuHandler->getMacExtraAppleMenu();
    juce::MenuBarModel::setMacMainMenu(menuHandler.get(), &macExtraAppleMenuItems);
    MacMenuIconHelper::applySettingsMenuIcon(TR("command.settings"));
  }
#endif

  // Add command manager key mappings as a KeyListener
  // This enables automatic keyboard shortcut dispatch
  addKeyListener(commandManager->getKeyMappings());
  setWantsKeyboardFocus(true);

  // Load config
  if (enableAudioDeviceFlag)
    settingsManager->loadConfig();

  LOG("MainComponent: starting timer...");
  // Start timer for UI updates
  startTimerHz(60);
  writeMcpJsonAtomically(
      getMcpControlDirectory().getChildFile("instance_" + mcpInstanceId + ".json"),
      juce::var(new juce::DynamicObject()));

  // The launch update check asks Session Loops' server about PitchNet releases
  // and offers their download page, which does not apply to Kinu Tune.
  // checkForUpdatesOnLaunch() stays in place for a future Kinu Tune feed.

  LOG("MainComponent: constructor complete");
}

void MainComponent::checkForUpdatesOnLaunch()
{
  if (updateCheckThread.joinable())
    return;

  const auto skippedVersion = settingsManager
                                  ? settingsManager->getSkippedUpdateVersion()
                                  : juce::String();
  juce::Component::SafePointer<MainComponent> safeThis(this);

  updateRequest = std::make_unique<juce::WebInputStream>(
      juce::URL("https://sessionloops.com/api/app/getApps"), false);
  updateRequest->withConnectionTimeout(3000);

  updateCheckThread = std::thread([this, safeThis, skippedVersion]
  {
    auto updateInfo = fetchLatestUpdateInfo(*updateRequest);
    if (stoppingUpdateCheck.load() || !updateInfo.has_value())
      return;

    if (updateInfo->version == skippedVersion)
      return;

    juce::MessageManager::callAsync([safeThis,
                                     latestVersion = updateInfo->version,
                                     releaseNotes = updateInfo->releaseNotes]
    {
      if (safeThis != nullptr)
        safeThis->showUpdateAvailablePopup(latestVersion, releaseNotes);
    });
  });
}

void MainComponent::showUpdateAvailablePopup(const juce::String &latestVersion,
                                             const juce::String &releaseNotes)
{
  juce::Component::SafePointer<MainComponent> safeThis(this);
  auto *dialog = new UpdateAvailableDialog(
      this,
      latestVersion,
      releaseNotes,
      [safeThis, latestVersion]
      {
        if (safeThis != nullptr)
          safeThis->skipUpdateVersion(latestVersion);
      },
      []
      {
        juce::Process::openDocument("https://sessionloops.com/pitchnet#downloads", "");
      });

  dialog->setVisible(true);
  dialog->toFront(true);
  dialog->enterModalState(true, nullptr, true);
}

void MainComponent::showAboutPopup()
{
  auto *dialog = new AboutDialog(this);
  dialog->setVisible(true);
  dialog->toFront(true);
  dialog->enterModalState(true, nullptr, true);
}

void MainComponent::skipUpdateVersion(const juce::String &version)
{
  if (settingsManager == nullptr)
    return;

  settingsManager->setSkippedUpdateVersion(version);
  settingsManager->saveConfig();
}

void MainComponent::bindBackendController(EditorController *controller)
{
  if (!isPluginMode())
    return;

  editorController = controller;

  if (settingsManager)
  {
    settingsManager->setVocoder(editorController
                                    ? editorController->getVocoder()
                                    : nullptr);
    if (editorController)
    {
      editorController->setPitchDetectorType(
          settingsManager->getPitchDetectorType());
      editorController->setSynthesisEngineType(
          settingsManager->getSynthesisEngineType());
      editorController->setDeviceConfig(settingsManager->getDevice(),
                                        settingsManager->getGPUDeviceId());
      if (!editorController->isSelectedPitchDetectorLoaded())
        editorController->reloadInferenceModels(false);
      settingsManager->applySettings();
      refreshRenderDeviceOptions();
    }
  }

  auto *project = getProject();
  applyProjectSynthesisEngine();
  pianoRoll.setProject(project);
  pianoRollView.setProject(project);
  parameterPanel.setProject(project);
  if (project)
  {
    toolbar.setTotalTime(project->getAudioData().getDuration());
    toolbar.setLoopEnabled(project->getLoopRange().enabled);
  }
}

void MainComponent::bindUndoManager(PitchUndoManager *manager)
{
  if (!isPluginMode())
    return;

  if (undoManager)
    undoManager->onHistoryChanged = nullptr;

  undoManager = manager;
  pianoRoll.setUndoManager(undoManager);
  parameterPanel.setUndoManager(undoManager);

  if (undoManager)
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    undoManager->onHistoryChanged = [safeThis]()
    {
      if (safeThis == nullptr)
        return;

      if (safeThis->commandManager)
        safeThis->commandManager->commandStatusChanged();
      safeThis->toolbar.setUndoRedoEnabled(
          safeThis->undoManager && safeThis->undoManager->canUndo(),
          safeThis->undoManager && safeThis->undoManager->canRedo());
    };
  }

  if (commandManager)
    commandManager->commandStatusChanged();
  toolbar.setUndoRedoEnabled(undoManager && undoManager->canUndo(),
                             undoManager && undoManager->canRedo());
}

std::unique_ptr<Project>
MainComponent::exchangeProject(std::unique_ptr<Project> newProject)
{
  if (!isPluginMode() || editorController == nullptr)
    return newProject;

  // D-180: measurement only.
  const double exchangeStartMs = nowMs();
  const bool incomingProject = newProject != nullptr;

  auto previousProject = editorController->takeProject();
  editorController->setProject(std::move(newProject));

  auto *project = getProject();
  applyProjectSynthesisEngine();
  pianoRoll.setProject(project);
  pianoRollView.setProject(project);
  parameterPanel.setProject(project);
  if (project)
  {
    toolbar.setTotalTime(project->getAudioData().getDuration());
    toolbar.setTransportEnabled(true);
    toolbar.setLoopEnabled(project->getLoopRange().enabled);
    applyCachedHostLoopRange();
    if (pitchnet::shouldFitPitchRange(!project->getNotes().empty(),
                                     hasAnalyzedProject()) &&
        !regionActivationFromCanvas)
      fitAnalyzedPitchRangeToView(*project);
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
    ARA_DIAG("exchangeProject notes=" +
             juce::String(static_cast<int>(project->getNotes().size())) +
             " waveform=" +
             juce::String(project->getAudioData().waveform.getNumSamples()) +
             " analyzed=" + (hasAnalyzedProject() ? "1" : "0") +
             " regionFromCanvas=" + (regionActivationFromCanvas ? "1" : "0") +
             " trackView=" + (pianoRoll.isTrackViewMode() ? "1" : "0"));
#endif
  }

  // D-180-R3: if the cursor was seeded before this bind, reveal it with the
  // pps/width available now. Do not clear the flag: focus and the first paint
  // may still run before the final zoom settles.
  if (incomingProject && pendingOpenCursorReveal)
  {
    const double openCursor = pianoRoll.getCursorTime();
    const int openWidth = pianoRoll.getVisibleContentWidth();
    const double openPps =
        static_cast<double>(pianoRoll.getPixelsPerSecond());
    if (!isPlaying && openCursor > 0.01 && openWidth > 0 && openPps > 0.0)
      pianoRoll.setScrollX(pitchnet::scrollXToRevealCursor(
          openCursor, pianoRoll.getScrollX(), openPps, openWidth,
          pianoRoll.getTimelineStart(), pianoRoll.getTimelineDuration()));
  }
  repaint();

  // D-180: measurement only. The project is bound now; the timer reports the
  // first paint after this point.
  if (incomingProject)
  {
    latencyTrace.selectionBoundMs = nowMs();
    latencyTrace.selectionPaintBase = pianoRoll.getPaintCount();
    latencyTrace.selectionScrollAtBound = pianoRoll.getScrollX();
  }
  latencyTrace.lastExchangeMs = nowMs() - exchangeStartMs;
  latencyTrace.maxExchangeMs =
      std::max(latencyTrace.maxExchangeMs, latencyTrace.lastExchangeMs);
  return previousProject;
}

MainViewViewportState MainComponent::getViewportState() const
{
  MainViewViewportState state;
  state.scrollX = pianoRoll.getScrollX();
  state.scrollY = pianoRoll.getScrollY();
  state.pixelsPerSecond = pianoRoll.getPixelsPerSecond();
  state.pixelsPerSemitone = pianoRoll.getPixelsPerSemitone();
  state.valid = true;
  return state;
}

void MainComponent::restoreViewportState(
    const MainViewViewportState &state)
{
  if (!state.valid)
    return;

  pianoRoll.setPixelsPerSecond(state.pixelsPerSecond, false);
  pianoRoll.setPixelsPerSemitone(state.pixelsPerSemitone, 0.0f);
  pianoRoll.setScrollX(state.scrollX);
  pianoRoll.setScrollY(state.scrollY);
  toolbar.setZoom(pianoRoll.getPixelsPerSecond());
  pianoRollView.refreshOverview();
}

void MainComponent::reloadInferenceModels(bool async)
{
  if (!settingsManager || !editorController)
    return;

  editorController->setDeviceConfig(settingsManager->getDevice(),
                                    settingsManager->getGPUDeviceId());
  editorController->reloadInferenceModels(async);
}

void MainComponent::refreshRenderDeviceOptions()
{
  if (settingsManager == nullptr)
    return;

  const auto executionDevice = settingsManager->getDevice();
  parameterPanel.setRenderDeviceOptions(
      GpuDeviceList::getDeviceNames(executionDevice),
      settingsManager->getGPUDeviceId());
}

bool MainComponent::isInferenceBusy() const
{
  if (isLoadingAudio.load())
    return true;
  if (editorController && editorController->isLoading())
    return true;
  if (editorController && editorController->isInferenceBusy())
    return true;
  return false;
}

MainComponent::~MainComponent()
{
  // Finish the network worker before JUCE tears down its networking state.
  stoppingUpdateCheck.store(true);
  if (updateRequest != nullptr)
    updateRequest->cancel();
  if (updateCheckThread.joinable())
    updateCheckThread.join();
  updateRequest.reset();

  setComponentEffect(nullptr);
  // A drag audition may still be sounding if the editor closes mid-drag; the
  // host preview path must be released while its callbacks are still valid.
  cancelDraggedNoteAudition();
  finishPianoKeyAudition();
  noteDragAuditionActive = false;
  resampledLoopAudition.reset();
#if JUCE_MAC
  if (!isPluginMode() && menuHandler)
    juce::MenuBarModel::setMacMainMenu(nullptr);
#else
  if (!isPluginMode() && menuHandler)
  {
    menuBar.setModel(nullptr);
    menuBar.setLookAndFeel(nullptr);
  }
#endif
  removeKeyListener(commandManager->getKeyMappings());
  stopTimer();
  getMcpControlDirectory().getChildFile("instance_" + mcpInstanceId + ".json")
      .deleteFile();

  if (auto *audioEngine = editorController
                              ? editorController->getAudioEngine()
                              : nullptr)
  {
    audioEngine->clearCallbacks();
    audioEngine->shutdownAudio();
  }

  if (enableAudioDeviceFlag)
    settingsManager->saveConfig();
}

juce::Point<int> MainComponent::getSavedWindowSize() const
{
  if (settingsManager)
    return {settingsManager->getWindowWidth(),
            settingsManager->getWindowHeight()};
  return {WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight};
}

void MainComponent::paint(juce::Graphics &g)
{
  g.fillAll(APP_COLOR_BACKGROUND);
}

void MainComponent::paintOverChildren(juce::Graphics &g)
{
  if (settingsOverlay != nullptr && settingsOverlay->isVisible())
    return;

  constexpr int scrollBarWidth = 8;
  const int y = toolbar.getBottom();
  if (y >= getHeight())
    return;

  const int viewLeft = workspace.getX() + workspace.getMainViewX();
  const int viewRight = workspace.getX() + workspace.getMainViewRight();
  g.setColour(juce::Colour(0xFF3C3C3Cu));
  g.fillRect(viewLeft, y, juce::jmax(0, viewRight - scrollBarWidth - viewLeft), 1);
}

void MainComponent::applyUiBrightness(double brightnessPercent)
{
  const double normalized = juce::jlimit(75.0, 200.0, brightnessPercent);
  uiBrightnessEffect->setBrightnessPercent(normalized);
  setComponentEffect(std::abs(normalized - 100.0) < 0.01
                         ? nullptr
                         : uiBrightnessEffect.get());
  repaint();
}

void MainComponent::resized()
{
  // D-180-R: measurement only. Capture the scroll and cursor before the layout
  // runs; the setters below can clamp it.
  const bool traceResize =
      getProject() != nullptr && getWidth() > 0 && getHeight() > 0;
  const double resizeScrollBefore =
      traceResize ? pianoRoll.getScrollX() : 0.0;
  const double resizeCursor = traceResize ? pianoRoll.getCursorTime() : 0.0;
  const bool resizeCursorVisibleBefore =
      traceResize && cursorIsVisible(resizeCursor);
  const double resizePpsBefore =
      traceResize ? pianoRoll.getPixelsPerSecond() : 0.0;
  const double resizeDurationBefore =
      traceResize ? pianoRoll.getTimelineDuration() : 0.0;
  const int resizeVisibleWidthBefore =
      traceResize ? pianoRoll.getVisibleContentWidth() : 0;

  if (auto *project = getProject(); project && getWidth() > 0 && getHeight() > 0)
  {
    project->uiWidth = getWidth();
    project->uiHeight = getHeight();
  }
  auto bounds = getLocalBounds();

#if !JUCE_MAC
  // Menu bar at top for non-mac platforms
  if (!isPluginMode() && menuHandler)
    menuBar.setBounds(bounds.removeFromTop(24));
#endif

  // Toolbar
  toolbar.setBounds(bounds.removeFromTop(60));

  // Workspace takes remaining space (includes piano roll, panels, and sidebar)
  const double layoutScrollBefore = pianoRoll.getScrollX();
  const double layoutPpsBefore =
      static_cast<double>(pianoRoll.getPixelsPerSecond());
  const double layoutDurationBefore = pianoRoll.getTimelineDuration();
  const int layoutVisibleWidthBefore = pianoRoll.getVisibleContentWidth();
  workspace.setBounds(bounds);
  latencyTrace.lastLayoutScrollBefore = layoutScrollBefore;
  latencyTrace.lastLayoutScrollAfter = pianoRoll.getScrollX();
  latencyTrace.lastLayoutPpsBefore = layoutPpsBefore;
  latencyTrace.lastLayoutDurationBefore = layoutDurationBefore;
  latencyTrace.lastLayoutVisibleWidthBefore = layoutVisibleWidthBefore;
  ++latencyTrace.layoutCount;

  if (settingsOverlay)
    settingsOverlay->setBounds(getLocalBounds());

  analysisBackdrop.setBounds(getLocalBounds());
  analysisProgressPopup.setBounds(getLocalBounds().withSizeKeepingCentre(399, 98));

  if (enableAudioDeviceFlag && settingsManager)
    settingsManager->setWindowSize(getWidth(), getHeight());

  if (hasPendingTimelineFocus)
  {
    const double start = pendingTimelineFocusStart;
    const double end = pendingTimelineFocusEnd;
    hasPendingTimelineFocus = false;
    focusTimelineRange(start, end);
  }

  if (hasPendingPitchFit)
    if (auto *project = getProject())
    {
      hasPendingPitchFit = false;
      fitAnalyzedPitchRangeToView(*project);
    }

  // D-180-R3: re-apply with the layout's final pps/width. The flag stays set
  // until a paint actually shows the cursor.
  if (pendingOpenCursorReveal && getProject() != nullptr && !isPlaying)
  {
    const double openCursor = pianoRoll.getCursorTime();
    const int openWidth = pianoRoll.getVisibleContentWidth();
    const double openPps =
        static_cast<double>(pianoRoll.getPixelsPerSecond());
    if (openCursor > 0.01 && openWidth > 0 && openPps > 0.0)
      pianoRoll.setScrollX(pitchnet::scrollXToRevealCursor(
          openCursor, pianoRoll.getScrollX(), openPps, openWidth,
          pianoRoll.getTimelineStart(), pianoRoll.getTimelineDuration()));
  }


  if (traceResize)
  {
    const bool firstResize = latencyTrace.resizeCount == 0;
    ++latencyTrace.resizeCount;
    latencyTrace.lastResizeAtMs = nowMs();
    latencyTrace.lastResizeScrollBefore = resizeScrollBefore;
    latencyTrace.lastResizeScrollAfter = pianoRoll.getScrollX();
    latencyTrace.lastResizeCursor = resizeCursor;
    latencyTrace.lastResizeCursorVisibleBefore = resizeCursorVisibleBefore;
    latencyTrace.lastResizeCursorVisibleAfter =
        cursorIsVisible(resizeCursor);
    latencyTrace.lastResizeWidth = getWidth();
    latencyTrace.lastResizeHeight = getHeight();
    if (firstResize)
    {
      latencyTrace.firstResizeScrollBefore = resizeScrollBefore;
      latencyTrace.firstResizeScrollAfter = pianoRoll.getScrollX();
      latencyTrace.firstResizeCursorVisibleBefore =
          resizeCursorVisibleBefore;
      latencyTrace.firstResizeCursorVisibleAfter =
          latencyTrace.lastResizeCursorVisibleAfter;
      latencyTrace.firstResizeWidth = getWidth();
      latencyTrace.firstResizePps = resizePpsBefore;
      latencyTrace.firstResizeDuration = resizeDurationBefore;
      latencyTrace.firstResizeVisibleWidth = resizeVisibleWidthBefore;
    }
  }
}

void MainComponent::mouseDown(const juce::MouseEvent &e)
{
  juce::ignoreUnused(e);
}

void MainComponent::mouseDrag(const juce::MouseEvent &e)
{
  juce::ignoreUnused(e);
}

void MainComponent::mouseDoubleClick(const juce::MouseEvent &e)
{
  juce::ignoreUnused(e);
}

void MainComponent::pollMcpControl()
{
  jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
  const auto now = juce::Time::getMillisecondCounter();
  if (now - mcpLastPollMs < 100)
    return;
  mcpLastPollMs = now;

  auto *project = getProject();
  if ((mcpRenderPending && now - mcpLastHeartbeatMs >= 100) ||
      now - mcpLastHeartbeatMs >= 1000)
  {
    mcpLastHeartbeatMs = now;
    auto instance = new juce::DynamicObject();
    instance->setProperty("instance_id", mcpInstanceId);
    instance->setProperty("plugin_editor_open", true);
    instance->setProperty("project_loaded", project != nullptr);
    instance->setProperty("project_notes", project
        ? static_cast<int>(project->getNotes().size()) : 0);
    instance->setProperty("render_pending", mcpRenderPending);
    instance->setProperty("render_generation", static_cast<int>(mcpRenderGeneration));
    // D-174: read the engine this instance will render with. The document
    // value is what survives save/reopen; "none" means it follows the shared
    // preference (legacy document).
    instance->setProperty(
        "synthesis_engine",
        synthesisEngineTypeToString(editorController
                                        ? editorController->getSynthesisEngineType()
                                        : defaultSynthesisEngineType()));
    const auto documentEngine =
        project != nullptr ? project->getSynthesisEngine() : std::nullopt;
    instance->setProperty(
        "document_synthesis_engine",
        documentEngine ? synthesisEngineTypeToString(*documentEngine)
                       : juce::String("none"));
    writeMcpJsonAtomically(
        getMcpControlDirectory().getChildFile("instance_" + mcpInstanceId + ".json"),
        juce::var(instance));
  }

  const auto dir = getMcpControlDirectory();
  const auto requests = dir.findChildFiles(juce::File::findFiles, false,
                                            "request_*.json");
  for (const auto &requestFile : requests)
  {
    const auto request = juce::JSON::parse(requestFile.loadFileAsString());
    if (!request.isObject() || request.getProperty("target_instance", "").toString() != mcpInstanceId)
      continue;

    const auto requestId = request.getProperty("request_id", "").toString();
    if (requestId.isEmpty() || requestId.containsAnyOf("/\\:*?\"<>|"))
      continue;
    auto response = new juce::DynamicObject();
    response->setProperty("request_id", requestId);
    response->setProperty("instance_id", mcpInstanceId);
    response->setProperty("ok", false);

    const auto command = request.getProperty("command", "").toString();
    const bool allowedWithoutProject =
        command == "ui_state" || command == "paint_history" ||
        command == "reset_ui_trace";
    if (project == nullptr && !allowedWithoutProject)
      response->setProperty("error", "El editor no tiene un proyecto analizado.");
    else if (command == "list_notes")
    {
      juce::Array<juce::var> noteArray;
      const auto &notes = project->getNotes();
      const auto currentEngine =
          editorController ? editorController->getSynthesisEngineType()
                           : defaultSynthesisEngineType();
      response->setProperty("sample_rate", project->getAudioData().sampleRate);
      response->setProperty("hop_size", HOP_SIZE);
      response->setProperty(
          "synthesis_engine",
          synthesisEngineTypeToString(currentEngine));
      response->setProperty(
          "document_synthesis_engine",
          project->getSynthesisEngine()
              ? synthesisEngineTypeToString(*project->getSynthesisEngine())
              : juce::String("none"));
      for (int i = 0; i < static_cast<int>(notes.size()); ++i)
      {
        const auto &note = notes[static_cast<size_t>(i)];
        auto noteObject = new juce::DynamicObject();
        noteObject->setProperty("index", i);
        noteObject->setProperty("start_frame", note.getStartFrame());
        noteObject->setProperty("end_frame", note.getEndFrame());
        noteObject->setProperty("midi", note.getMidiNote());
        noteObject->setProperty("rest", note.isRest());
        noteObject->setProperty("engine", noteEngineToString(note.getEngine()));
        // D-181: what the renderer will actually use. With no hidden fallback
        // it must equal the stored request (or the instance engine for
        // Inherit); the MCP check compares both fields for every note.
        noteObject->setProperty(
            "effective_engine",
            synthesisEngineTypeToString(
                engineBlocks::effectiveEngine(note, currentEngine)));
        // D-185: why a request could not take effect (transparency only).
        const auto refusedIt = lastRefusedEngineRequests.find(i);
        const bool refused = refusedIt != lastRefusedEngineRequests.end();
        noteObject->setProperty("refused", refused);
        if (refused)
        {
          noteObject->setProperty("refused_engine",
                                  noteEngineToString(refusedIt->second));
          noteObject->setProperty("refusal_reason", "no_silence_before_note");
          noteObject->setProperty(
              "refusal_detail",
              "sin silencio >=20 ms / -55 dBFS antes de la nota");
        }
        noteObject->setProperty("selected", note.isSelected());
        noteArray.add(juce::var(noteObject));
      }
      response->setProperty("notes", juce::var(noteArray));
      response->setProperty("ok", true);
    }
    else if (command == "list_note_modulators")
    {
      // D-208: read-only raw modulator state per note, same order and index
      // as list_notes. No jlimit / no percentage conversion: the UI formula
      // (1 - v) * 100 is deliberately not applied here.
      juce::Array<juce::var> noteArray;
      const auto &notes = project->getNotes();
      response->setProperty("sample_rate", project->getAudioData().sampleRate);
      response->setProperty("hop_size", HOP_SIZE);
      for (int i = 0; i < static_cast<int>(notes.size()); ++i)
      {
        const auto &note = notes[static_cast<size_t>(i)];
        auto noteObject = new juce::DynamicObject();
        noteObject->setProperty("index", i);
        const auto share = ScaleUtils::correctionShare(
            *project, note.getLastNonMacroMidiNote(), note.getMidiNote(),
            static_cast<int>(FREQ_A4));
        if (share)
          noteObject->setProperty("correctionShare",
                                  static_cast<double>(*share));
        else
          noteObject->setProperty("correctionShare", juce::var());
        noteObject->setProperty("vibrato", note.getVibrato());
        noteObject->setProperty("pitchDrift", note.getPitchDrift());
        noteArray.add(juce::var(noteObject));
      }
      response->setProperty("notes", juce::var(noteArray));
      response->setProperty("ok", true);
    }
    else if (command == "dump_curve")
    {
      // D-182 measurement only: the exact target curve the renderer will use,
      // plus the source curve and the per-frame ratio. No audio change.
      const int totalFrames =
          static_cast<int>(project->getAudioData().melSpectrogram.size());
      int startFrame = static_cast<int>(request.getProperty(
          "start_frame", 0));
      int endFrame = static_cast<int>(request.getProperty(
          "end_frame", totalFrames));
      startFrame = juce::jlimit(0, totalFrames, startFrame);
      endFrame = juce::jlimit(startFrame, totalFrames, endFrame);

      const auto target = project->getAdjustedF0ForRange(startFrame, endFrame);
      const auto &audioData = project->getAudioData();
      const auto &sourceF0 =
          audioData.denseF0.empty() ? audioData.f0 : audioData.denseF0;
      const auto currentEngine =
          editorController ? editorController->getSynthesisEngineType()
                           : defaultSynthesisEngineType();

      juce::Array<juce::var> targetArray;
      juce::Array<juce::var> sourceArray;
      juce::Array<juce::var> ratioArray;
      juce::Array<juce::var> baseArray;
      juce::Array<juce::var> deltaArray;
      for (int frame = startFrame; frame < endFrame; ++frame)
      {
        const int local = frame - startFrame;
        const float targetHz =
            local < static_cast<int>(target.size())
                ? target[static_cast<size_t>(local)]
                : 0.0f;
        const float sourceHz =
            frame < static_cast<int>(sourceF0.size())
                ? sourceF0[static_cast<size_t>(frame)]
                : 0.0f;
        targetArray.add(targetHz);
        sourceArray.add(sourceHz);
        ratioArray.add(targetHz > 0.0f && sourceHz > 0.0f
                           ? targetHz / sourceHz
                           : 1.0f);
        baseArray.add(frame < static_cast<int>(audioData.basePitch.size())
                          ? audioData.basePitch[static_cast<size_t>(frame)]
                          : 0.0f);
        deltaArray.add(frame < static_cast<int>(audioData.deltaPitch.size())
                           ? audioData.deltaPitch[static_cast<size_t>(frame)]
                           : 0.0f);
      }

      // Note frontiers exactly as BasePitchCurve places them: midpoint
      // between the end of one note and the start of the next.
      juce::Array<juce::var> boundaryArray;
      std::vector<const Note *> ordered;
      for (const auto &note : project->getNotes())
        if (!note.isRest() && note.getEndFrame() > startFrame &&
            note.getStartFrame() < endFrame)
          ordered.push_back(&note);
      std::sort(ordered.begin(), ordered.end(),
                [](const Note *a, const Note *b) {
                  return a->getStartFrame() < b->getStartFrame();
                });
      for (size_t i = 1; i < ordered.size(); ++i)
      {
        const auto *left = ordered[i - 1];
        const auto *right = ordered[i];
        auto boundary = new juce::DynamicObject();
        boundary->setProperty("left_index",
                              static_cast<int>(left - project->getNotes().data()));
        boundary->setProperty("right_index",
                              static_cast<int>(right - project->getNotes().data()));
        boundary->setProperty("left_end_frame", left->getEndFrame());
        boundary->setProperty("right_start_frame", right->getStartFrame());
        boundary->setProperty(
            "boundary_frame",
            (left->getEndFrame() + right->getStartFrame()) / 2);
        boundary->setProperty("left_midi", left->getMidiNote());
        boundary->setProperty("right_midi", right->getMidiNote());
        boundary->setProperty(
            "left_shift_cents",
            (left->getAdjustedMidiNote() - left->getOriginalMidiNote()) *
                100.0f);
        boundary->setProperty(
            "right_shift_cents",
            (right->getAdjustedMidiNote() - right->getOriginalMidiNote()) *
                100.0f);
        boundary->setProperty(
            "left_engine",
            synthesisEngineTypeToString(
                engineBlocks::effectiveEngine(*left, currentEngine)));
        boundary->setProperty(
            "right_engine",
            synthesisEngineTypeToString(
                engineBlocks::effectiveEngine(*right, currentEngine)));
        boundaryArray.add(juce::var(boundary));
      }

      response->setProperty("start_frame", startFrame);
      response->setProperty("end_frame", endFrame);
      response->setProperty("sample_rate", audioData.sampleRate);
      response->setProperty("hop_size", HOP_SIZE);
      response->setProperty("target_hz", juce::var(targetArray));
      response->setProperty("source_hz", juce::var(sourceArray));
      response->setProperty("pitch_ratio", juce::var(ratioArray));
      response->setProperty("base_pitch", juce::var(baseArray));
      response->setProperty("delta_pitch", juce::var(deltaArray));
      response->setProperty("boundaries", juce::var(boundaryArray));
      response->setProperty("ok", true);
    }
    else if (command == "set_note_midi")
    {
      // D-182 measurement only: apply the same operation as a note drag
      // (NotePitch::applyEdit + rebuild + dirty range), so an inverted
      // contour is a real user edit, not a hand-written curve.
      const auto indicesVar = request.getProperty("indices", juce::var());
      const auto midiVar = request.getProperty("midi", juce::var());
      std::vector<int> indices;
      std::vector<float> midiValues;
      if (indicesVar.isArray())
        for (const auto &value : *indicesVar.getArray())
          indices.push_back(static_cast<int>(value));
      if (midiVar.isArray())
        for (const auto &value : *midiVar.getArray())
          midiValues.push_back(static_cast<float>(value));

      if (indices.empty() || indices.size() != midiValues.size())
        response->setProperty(
            "error", "set_note_midi necesita indices y midi del mismo tamano.");
      else
      {
        auto &notes = project->getNotes();
        int minFrame = std::numeric_limits<int>::max();
        int maxFrame = std::numeric_limits<int>::min();
        bool valid = true;
        juce::Array<juce::var> applied;
        for (size_t i = 0; i < indices.size(); ++i)
        {
          const int index = indices[i];
          const float newMidi = midiValues[i];
          if (index < 0 || index >= static_cast<int>(notes.size()) ||
              notes[static_cast<size_t>(index)].isRest() ||
              !std::isfinite(newMidi))
          {
            valid = false;
            break;
          }
          auto &note = notes[static_cast<size_t>(index)];
          const float fromMidi = note.getMidiNote();
          const float sourceBefore = note.getLastNonMacroMidiNote();
          NotePitch::applyEdit(note, *project, sourceBefore, fromMidi, newMidi);
          note.setPitchOffset(0.0f);
          note.markDirty();
          note.markSynthDirty();
          minFrame = std::min(minFrame, note.getStartFrame());
          maxFrame = std::max(maxFrame, note.getEndFrame());
          auto entry = new juce::DynamicObject();
          entry->setProperty("index", index);
          entry->setProperty("from_midi", fromMidi);
          entry->setProperty("to_midi", newMidi);
          applied.add(juce::var(entry));
        }

        if (!valid)
          response->setProperty(
              "error", "set_note_midi: indice fuera de rango, nota rest o MIDI no finito; no se aplico nada.");
        else
        {
          PitchCurveProcessor::rebuildBaseFromNotes(*project);
          project->markNoteEditDirtyRange(minFrame, maxFrame);
          project->setModified(true);
          pianoRoll.repaint();
          onPitchEdited();
          markRenderDirty();
          notifyProjectDataChanged();
          if (isPluginMode() && onPitchEditFinished)
            onPitchEditFinished();
          response->setProperty("applied", juce::var(applied));
          response->setProperty("dirty_start", minFrame);
          response->setProperty("dirty_end", maxFrame);
          response->setProperty("ok", true);
        }
      }
    }
    else if (command == "set_note_engine")
    {
      const auto engineText = request.getProperty("engine", "").toString();
      const auto engine = stringToNoteEngine(engineText);
      const bool engineValid = engineText.equalsIgnoreCase("Inherit") ||
          engineText.equalsIgnoreCase("Vocoder") ||
          engineText.equalsIgnoreCase("PSOLA") ||
          engineText.equalsIgnoreCase("Pitchcore");
      const auto indicesVar = request.getProperty("indices", juce::var());
      std::vector<int> indices;
      if (indicesVar.isArray())
        for (const auto &value : *indicesVar.getArray())
          indices.push_back(static_cast<int>(value));
      if (!engineValid || indices.empty())
        response->setProperty("error", "Se requieren índices de nota y un motor válido: Inherit, Vocoder, PSOLA o Pitchcore.");
      else
      {
        const auto &notes = project->getNotes();
        juce::Array<juce::var> before;
        for (const int index : indices)
        {
          if (index < 0 || index >= static_cast<int>(notes.size()))
          {
            response->setProperty("error", "Índice de nota fuera de rango; no se aplicó ningún cambio.");
            break;
          }
          before.add(noteEngineToString(notes[static_cast<size_t>(index)].getEngine()));
        }
        if (before.size() == static_cast<int>(indices.size()))
        {
          const int refused = applyNoteEngineToIndices(indices, engine);
          juce::Array<juce::var> applied;
          juce::Array<juce::var> unchanged;
          for (size_t i = 0; i < indices.size(); ++i)
          {
            const int index = indices[i];
            const auto previous = before[static_cast<int>(i)].toString();
            const auto current = noteEngineToString(project->getNotes()[static_cast<size_t>(index)].getEngine());
            (previous == current ? unchanged : applied).add(index);
          }
          response->setProperty("applied_indices", juce::var(applied));
          response->setProperty("unchanged_indices", juce::var(unchanged));
          response->setProperty("refused", refused);
          response->setProperty("engine", engineText);
          response->setProperty("ok", refused == 0);
          if (refused > 0)
            response->setProperty("error", "D-129 rechazó notas sin silencio válido; ver refused y unchanged_indices.");
        }
      }
    }
    else if (command == "set_synthesis_engine")
    {
      const auto engineText = request.getProperty("engine", "").toString();
      SynthesisEngineType engine = SynthesisEngineType::Psola;
      bool valid = true;
      if (engineText == "PSOLA")
        engine = SynthesisEngineType::Psola;
      else if (engineText == "Pitchcore")
        engine = SynthesisEngineType::Pitchcore;
      else if (engineText == "Vocoder")
        engine = SynthesisEngineType::Vocoder;
      else
        valid = false;

      if (!valid)
        response->setProperty("error", "engine debe ser PSOLA, Pitchcore o Vocoder.");
      else
      {
        // Same path as the Rendering card, so the document records its own
        // engine instead of only the shared preference (D-174, I-19).
        settingsManager->setSynthesisEngineType(engine);
        settingsManager->saveConfig();
        project->setSynthesisEngine(engine);
        if (editorController)
          editorController->setSynthesisEngineType(engine);
        rerenderEditedNotes();
        notifyProjectDataChanged();
        response->setProperty("synthesis_engine", engineText);
        response->setProperty("ok", true);
      }
    }
    else if (command == "reset_ui_trace")
    {
      latencyTrace.resizeCount = 0;
      latencyTrace.firstResizeScrollBefore = 0.0;
      latencyTrace.firstResizeScrollAfter = 0.0;
      latencyTrace.firstResizeCursorVisibleBefore = false;
      latencyTrace.firstResizeCursorVisibleAfter = false;
      latencyTrace.firstResizeWidth = 0;
      latencyTrace.firstResizePps = 0.0;
      latencyTrace.firstResizeDuration = 0.0;
      latencyTrace.firstResizeVisibleWidth = 0;
      latencyTrace.layoutCount = 0;
      latencyTrace.lastLayoutScrollBefore = 0.0;
      latencyTrace.lastLayoutScrollAfter = 0.0;
      latencyTrace.scrollResetCount = 0;
      latencyTrace.scrollResetFrom = 0.0;
      latencyTrace.scrollResetTo = 0.0;
      latencyTrace.scrollResetAtMs = 0.0;
      latencyTrace.lastObservedScrollX = pianoRoll.getScrollX();
      latencyTrace.callbackResetCount = 0;
      latencyTrace.callbackResetFrom = 0.0;
      latencyTrace.callbackResetTo = 0.0;
      latencyTrace.callbackResetAtMs = 0.0;
      latencyTrace.lastCallbackScrollX = pianoRoll.getScrollX();
      latencyTrace.layoutScrollGuardActive = false;
      latencyTrace.layoutScrollGuardScroll = 0.0;
      latencyTrace.layoutScrollGuardUntilMs = 0.0;
      latencyTrace.layoutScrollGuardCursorVisible = false;
      latencyTrace.layoutScrollGuardRestoreCount = 0;
      latencyTrace.focusRestoreCount = 0;
      latencyTrace.paintHistoryCount = 0;
      latencyTrace.paintHistoryNext = 0;
      latencyTrace.lastSampledPaintCount = pianoRoll.getPaintCount();
      response->setProperty("ok", true);
    }
    else if (command == "paint_history")
    {
      juce::Array<juce::var> samples;
      const int count = latencyTrace.paintHistoryCount;
      const int start = count < UiLatencyTrace::kPaintHistorySize
                            ? 0
                            : latencyTrace.paintHistoryNext;
      for (int i = 0; i < count; ++i)
      {
        const int index = (start + i) % UiLatencyTrace::kPaintHistorySize;
        const auto &sample =
            latencyTrace.paintHistory[static_cast<size_t>(index)];
        auto object = new juce::DynamicObject();
        object->setProperty("at_ms", sample.atMs);
        object->setProperty("scroll_x", sample.scrollX);
        object->setProperty("cursor_time", sample.cursorTime);
        object->setProperty("visible_width", sample.visibleWidth);
        object->setProperty("pixels_per_second", sample.pixelsPerSecond);
        object->setProperty("cursor_visible", sample.cursorVisible);
        object->setProperty("playback_active", sample.playbackActive);
        object->setProperty("paint_count",
                            static_cast<juce::int64>(sample.paintCount));
        object->setProperty("project_notes", sample.projectNotes);
        samples.add(juce::var(object));
      }
      response->setProperty("paint_count",
                            static_cast<juce::int64>(pianoRoll.getPaintCount()));
      response->setProperty("samples", juce::var(samples));
      response->setProperty("ok", true);
    }
    else if (command == "ui_state")
    {
      // D-180: measurement only. Internal timestamps let the probe compute the
      // latency without trusting the file-poll interval.
      auto span = [](double fromMs, double toMs) {
        return (fromMs > 0.0 && toMs >= fromMs) ? toMs - fromMs : -1.0;
      };
      response->setProperty("now_ms", nowMs());
      response->setProperty("wall_ms",
                            static_cast<double>(juce::Time::currentTimeMillis()));
      response->setProperty("playback_active", isPlaying);
      response->setProperty("paint_count",
                            static_cast<juce::int64>(pianoRoll.getPaintCount()));
      response->setProperty("paint_last_ms", pianoRoll.getLastPaintDurationMs());
      response->setProperty("paint_max_ms", pianoRoll.getMaxPaintDurationMs());
      response->setProperty("timer_gap_max_ms", latencyTrace.maxTimerGapMs);
      response->setProperty("exchange_last_ms", latencyTrace.lastExchangeMs);
      response->setProperty("exchange_max_ms", latencyTrace.maxExchangeMs);
      response->setProperty("region_refresh_last_ms",
                            latencyTrace.lastRegionRefreshMs);
      response->setProperty("region_refresh_max_ms",
                            latencyTrace.maxRegionRefreshMs);
      response->setProperty("archive_restore_last_ms",
                            latencyTrace.lastArchiveRestoreMs);
      response->setProperty("archive_restore_max_ms",
                            latencyTrace.maxArchiveRestoreMs);
      response->setProperty("activation_last_ms",
                            latencyTrace.lastActivationMs);
      response->setProperty("activation_max_ms",
                            latencyTrace.maxActivationMs);
      response->setProperty("activation_cancel_ms",
                            latencyTrace.lastActivationCancelMs);
      response->setProperty("activation_copy_ms",
                            latencyTrace.lastActivationCopyMs);
      response->setProperty("activation_parse_ms",
                            latencyTrace.lastActivationParseMs);
      response->setProperty("activation_install_ms",
                            latencyTrace.lastActivationInstallMs);
      response->setProperty("activation_focus_ms",
                            latencyTrace.lastActivationFocusMs);
      response->setProperty("activation_bind_ms",
                            latencyTrace.lastActivationBindMs);
      response->setProperty("activation_total_ms",
                            latencyTrace.lastActivationTotalMs);

      response->setProperty("selection_start_ms",
                            latencyTrace.selectionStartedMs);
      response->setProperty("selection_bound_ms",
                            latencyTrace.selectionBoundMs);
      response->setProperty("selection_first_paint_ms",
                            latencyTrace.selectionFirstPaintMs);
      response->setProperty("selection_bind_ms",
                            span(latencyTrace.selectionStartedMs,
                                 latencyTrace.selectionBoundMs));
      response->setProperty("selection_paint_ms",
                            span(latencyTrace.selectionBoundMs,
                                 latencyTrace.selectionFirstPaintMs));
      response->setProperty("selection_total_ms",
                            span(latencyTrace.selectionStartedMs,
                                 latencyTrace.selectionFirstPaintMs));

      response->setProperty("play_state_ms", latencyTrace.playStateMs);
      response->setProperty("play_first_paint_ms",
                            latencyTrace.playFirstPaintMs);
      response->setProperty("play_state_to_paint_ms",
                            span(latencyTrace.playStateMs,
                                 latencyTrace.playFirstPaintMs));
      response->setProperty("play_cursor_ms", latencyTrace.playCursorMs);
      response->setProperty("play_cursor_first_paint_ms",
                            latencyTrace.playCursorFirstPaintMs);
      response->setProperty("play_cursor_to_paint_ms",
                            span(latencyTrace.playCursorMs,
                                 latencyTrace.playCursorFirstPaintMs));
      response->setProperty("play_cursor_visible",
                            latencyTrace.playCursorVisible);

      response->setProperty("stopped_seek_ms", latencyTrace.stoppedSeekMs);
      response->setProperty("stopped_seek_applied_ms",
                            latencyTrace.stoppedSeekAppliedMs);
      response->setProperty("stopped_seek_first_paint_ms",
                            latencyTrace.stoppedSeekFirstPaintMs);
      response->setProperty("stopped_seek_apply_ms",
                            span(latencyTrace.stoppedSeekMs,
                                 latencyTrace.stoppedSeekAppliedMs));
      response->setProperty("stopped_seek_paint_ms",
                            span(latencyTrace.stoppedSeekAppliedMs,
                                 latencyTrace.stoppedSeekFirstPaintMs));
      response->setProperty("stopped_seek_total_ms",
                            span(latencyTrace.stoppedSeekMs,
                                 latencyTrace.stoppedSeekFirstPaintMs));
      response->setProperty("stopped_seek_cursor_visible",
                            latencyTrace.stoppedSeekCursorVisible);

      response->setProperty("scroll_x", pianoRoll.getScrollX());
      response->setProperty("cursor_time", pianoRoll.getCursorTime());
      response->setProperty("pixels_per_second",
                            static_cast<double>(pianoRoll.getPixelsPerSecond()));
      response->setProperty("visible_width",
                            pianoRoll.getVisibleContentWidth());
      response->setProperty("cursor_visible",
                            cursorIsVisible(pianoRoll.getCursorTime()));
      response->setProperty("window_width", getWidth());
      response->setProperty("window_height", getHeight());
      response->setProperty("piano_roll_width", pianoRoll.getWidth());
      response->setProperty("piano_roll_height", pianoRoll.getHeight());
      response->setProperty("resize_count", latencyTrace.resizeCount);
      response->setProperty("resize_at_ms", latencyTrace.lastResizeAtMs);
      response->setProperty("resize_scroll_before",
                            latencyTrace.lastResizeScrollBefore);
      response->setProperty("resize_scroll_after",
                            latencyTrace.lastResizeScrollAfter);
      response->setProperty("resize_cursor", latencyTrace.lastResizeCursor);
      response->setProperty("resize_cursor_visible_before",
                            latencyTrace.lastResizeCursorVisibleBefore);
      response->setProperty("resize_cursor_visible_after",
                            latencyTrace.lastResizeCursorVisibleAfter);
      response->setProperty("resize_width", latencyTrace.lastResizeWidth);
      response->setProperty("resize_height", latencyTrace.lastResizeHeight);
      response->setProperty("first_resize_scroll_before",
                            latencyTrace.firstResizeScrollBefore);
      response->setProperty("first_resize_scroll_after",
                            latencyTrace.firstResizeScrollAfter);
      response->setProperty("first_resize_cursor_visible_before",
                            latencyTrace.firstResizeCursorVisibleBefore);
      response->setProperty("first_resize_cursor_visible_after",
                            latencyTrace.firstResizeCursorVisibleAfter);
      response->setProperty("first_resize_width",
                            latencyTrace.firstResizeWidth);
      response->setProperty("first_resize_pps",
                            latencyTrace.firstResizePps);
      response->setProperty("first_resize_duration",
                            latencyTrace.firstResizeDuration);
      response->setProperty("first_resize_visible_width",
                            latencyTrace.firstResizeVisibleWidth);
      response->setProperty("layout_count", latencyTrace.layoutCount);
      response->setProperty("layout_scroll_before",
                            latencyTrace.lastLayoutScrollBefore);
      response->setProperty("layout_scroll_after",
                            latencyTrace.lastLayoutScrollAfter);
      response->setProperty("layout_pps_before",
                            latencyTrace.lastLayoutPpsBefore);
      response->setProperty("layout_duration_before",
                            latencyTrace.lastLayoutDurationBefore);
      response->setProperty("layout_visible_width_before",
                            latencyTrace.lastLayoutVisibleWidthBefore);
      response->setProperty("scroll_reset_count",
                            latencyTrace.scrollResetCount);
      response->setProperty("scroll_reset_from",
                            latencyTrace.scrollResetFrom);
      response->setProperty("scroll_reset_to",
                            latencyTrace.scrollResetTo);
      response->setProperty("scroll_reset_at_ms",
                            latencyTrace.scrollResetAtMs);
      response->setProperty("callback_reset_count",
                            latencyTrace.callbackResetCount);
      response->setProperty("callback_reset_from",
                            latencyTrace.callbackResetFrom);
      response->setProperty("callback_reset_to",
                            latencyTrace.callbackResetTo);
      response->setProperty("callback_reset_at_ms",
                            latencyTrace.callbackResetAtMs);
      response->setProperty("focus_at_ms", latencyTrace.lastFocusAtMs);
      response->setProperty("focus_span_start",
                            latencyTrace.lastFocusSpanStart);
      response->setProperty("focus_scroll_x", latencyTrace.lastFocusScrollX);
      response->setProperty("focus_cursor_time",
                            latencyTrace.lastFocusCursorTime);
      response->setProperty("focus_cursor_was_visible",
                            latencyTrace.lastFocusCursorWasVisible);
      response->setProperty("focus_cursor_visible_after",
                            latencyTrace.lastFocusCursorVisibleAfter);
      response->setProperty("focus_cursor_restored",
                            latencyTrace.lastFocusCursorRestored);
      response->setProperty("focus_restore_count",
                            latencyTrace.focusRestoreCount);
      response->setProperty("layout_guard_active",
                            latencyTrace.layoutScrollGuardActive);
      response->setProperty("layout_guard_scroll",
                            latencyTrace.layoutScrollGuardScroll);
      response->setProperty("layout_guard_restore_count",
                            latencyTrace.layoutScrollGuardRestoreCount);
      response->setProperty("selection_scroll_at_bound",
                            latencyTrace.selectionScrollAtBound);
      response->setProperty("selection_scroll_at_paint",
                            latencyTrace.selectionScrollAtPaint);
      response->setProperty("selection_cursor_visible_at_paint",
                            latencyTrace.selectionCursorVisibleAtPaint);
      response->setProperty("timeline_start", pianoRoll.getTimelineStart());
      response->setProperty("timeline_duration",
                            pianoRoll.getTimelineDuration());
      response->setProperty("project_notes",
                            project != nullptr
                                ? static_cast<int>(project->getNotes().size())
                                : 0);
      response->setProperty("ok", true);
    }
    else
      response->setProperty("error", "Comando MCP desconocido.");

    const auto responseFile = dir.getChildFile("response_" + requestId + ".json");
    writeMcpJsonAtomically(responseFile, juce::var(response));
    requestFile.deleteFile();
  }
}

void MainComponent::timerCallback()
{
  // D-180: measurement only. A 60 Hz tick that arrives late means the message
  // thread was blocked, which is different from a late state change.
  {
    const double timerNowMs = nowMs();
    if (latencyTrace.lastTimerMs > 0.0)
      latencyTrace.maxTimerGapMs =
          std::max(latencyTrace.maxTimerGapMs,
                   timerNowMs - latencyTrace.lastTimerMs);
    latencyTrace.lastTimerMs = timerNowMs;
  }

  // D-180-R: measurement only. Detect a scroll that collapses to the start
  // between ticks, independently of who caused it.
  {
    const double observedScroll = pianoRoll.getScrollX();
    if (latencyTrace.lastObservedScrollX > 100.0 &&
        observedScroll < 1.0 &&
        std::abs(pianoRoll.getCursorTime()) > 10.0)
    {
      ++latencyTrace.scrollResetCount;
      latencyTrace.scrollResetFrom = latencyTrace.lastObservedScrollX;
      latencyTrace.scrollResetTo = observedScroll;
      latencyTrace.scrollResetAtMs = nowMs();
    }
    latencyTrace.lastObservedScrollX = observedScroll;
  }

  if (latencyTrace.layoutScrollGuardActive &&
      nowMs() > latencyTrace.layoutScrollGuardUntilMs)
    latencyTrace.layoutScrollGuardActive = false;

  pollMcpControl();
  dispatchPendingDragAudition();

  // Handle throttled cursor updates (30Hz max)
  if (hasPendingCursorUpdate.load())
  {
    double position = pendingCursorTime.load();
    hasPendingCursorUpdate.store(false);

    if (previewRegionActive)
    {
      if (isPluginMode())
      {
        // ARA editor preview is driven locally below; host stopped/position
        // updates should not interrupt or move that preview.
      }
      else if (position >= previewRegionEndTime)
      {
        finishPreviewRegion(true);
        return;
      }
      else
      {
        pianoRoll.setPreviewPlaybackPosition(position);
        return;
      }
    }

    if (!previewRegionActive || !isPluginMode())
    {
      pianoRoll.setCursorTime(position);
      toolbar.setCurrentTime(position);

      // D-180: follow a stopped host seek as well as playback. The old rule
      // only moved the view while the transport was running, so a cursor
      // outside the window left the notes at the project start until play.
      const bool stoppedSeekMoved = pendingStoppedHostSeek;
      const bool suppressPlaybackFollow =
          juce::Time::getMillisecondCounterHiRes() <
          suppressPlaybackFollowUntilMs;
      if (pitchnet::shouldFollowCursor(isPlaying, stoppedSeekMoved) &&
          toolbar.isFollowPlayback() &&
          (!isPlaying || !suppressPlaybackFollow))
      {
        const double newScrollX = pitchnet::scrollXToRevealCursor(
            position, pianoRoll.getScrollX(),
            static_cast<double>(pianoRoll.getPixelsPerSecond()),
            pianoRoll.getVisibleContentWidth(),
            pianoRoll.getTimelineStart(),
            pianoRoll.getTimelineDuration());
        if (std::abs(newScrollX - pianoRoll.getScrollX()) > 0.01)
          pianoRoll.setScrollX(newScrollX);
      }

      // D-180: measurement only. The stopped-seek event is applied here; the
      // first paint after this point is reported by the timer.
      if (pendingStoppedHostSeek)
      {
        if (latencyTrace.stoppedSeekAppliedMs == 0.0)
        {
          latencyTrace.stoppedSeekAppliedMs = nowMs();
          latencyTrace.stoppedSeekPaintBase = pianoRoll.getPaintCount();
          latencyTrace.stoppedSeekCursorVisible = cursorIsVisible(position);
        }
        pendingStoppedHostSeek = false;
      }
    }
  }

  if (previewRegionActive && isPluginMode())
  {
    const double elapsedSeconds =
        (juce::Time::getMillisecondCounterHiRes() -
         previewRegionWallClockStartMs) /
        1000.0;
    const double previewDuration =
        std::max(0.0, previewRegionEndTime - previewRegionStartTime);
    if (previewDuration > 0.0)
    {
      if (elapsedSeconds >= previewDuration)
      {
        pianoRoll.setPreviewPlaybackPosition(previewRegionEndTime);
        finishPreviewRegion(true);
      }
      else
      {
        pianoRoll.setPreviewPlaybackPosition(previewRegionStartTime +
                                            elapsedSeconds);
      }
    }
  }

  if (isLoadingAudio.load())
  {
    const auto progress = static_cast<float>(loadingProgress.load());

    juce::String msg;
    {
      const juce::ScopedLock sl(loadingMessageLock);
      msg = loadingMessage;
    }

    toolbar.hideProgress();
    showAnalysisProgress(progress);
    lastLoadingMessage = msg;

    return;
  }

  if (lastLoadingMessage.isNotEmpty())
  {
    toolbar.hideProgress();
    hideAnalysisProgress();
    lastLoadingMessage.clear();
  }

  updateUiLatencyPaintTrace();

  // D-180-R3/R4: one sample per observed paint. The 60 Hz timer is enough for
  // the ~500 ms intermediate frame this round is about.
  {
    const auto paintCount = pianoRoll.getPaintCount();
    if (paintCount != latencyTrace.lastSampledPaintCount)
    {
      auto &sample =
          latencyTrace.paintHistory[static_cast<size_t>(
              latencyTrace.paintHistoryNext)];
      sample.atMs = nowMs();
      sample.scrollX = pianoRoll.getScrollX();
      sample.cursorTime = pianoRoll.getCursorTime();
      sample.visibleWidth = pianoRoll.getVisibleContentWidth();
      sample.pixelsPerSecond =
          static_cast<double>(pianoRoll.getPixelsPerSecond());
      sample.cursorVisible =
          cursorIsVisible(sample.cursorTime);
      sample.playbackActive = isPlaying;
      sample.paintCount = paintCount;
      sample.projectNotes = getProject() != nullptr
                                ? static_cast<int>(getProject()->getNotes().size())
                                : 0;
      latencyTrace.paintHistoryNext =
          (latencyTrace.paintHistoryNext + 1) %
          UiLatencyTrace::kPaintHistorySize;
      latencyTrace.paintHistoryCount = std::min(
          latencyTrace.paintHistoryCount + 1,
          UiLatencyTrace::kPaintHistorySize);
      latencyTrace.lastSampledPaintCount = paintCount;
      if (pendingOpenCursorReveal && sample.cursorVisible)
        pendingOpenCursorReveal = false;
    }
  }

  // D-180-R3 fallback: if setup did not call focusTimelineRange, reveal the
  // seeded cursor once the canvas has a real width.
  if (pendingOpenCursorReveal && getProject() != nullptr && !isPlaying)
  {
    const double openCursor = pianoRoll.getCursorTime();
    const int openWidth = pianoRoll.getVisibleContentWidth();
    const double openPps =
        static_cast<double>(pianoRoll.getPixelsPerSecond());
    if (openCursor > 0.01 && openWidth > 0 && openPps > 0.0)
    {
      pianoRoll.setScrollX(pitchnet::scrollXToRevealCursor(
          openCursor, pianoRoll.getScrollX(), openPps, openWidth,
          pianoRoll.getTimelineStart(), pianoRoll.getTimelineDuration()));
      if (cursorIsVisible(openCursor))
        pendingOpenCursorReveal = false;
    }
  }
}

bool MainComponent::keyPressed(const juce::KeyPress &key,
                               juce::Component * /*originatingComponent*/)
{
  // All keyboard shortcuts are now handled by ApplicationCommandManager
  // This method is kept for potential future non-command keyboard handling
  juce::ignoreUnused(key);
  return false;
}

void MainComponent::saveProject(bool saveAs)
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, saveAs]()
                                    {
      if (safeThis != nullptr)
        safeThis->saveProject(saveAs); });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  // P1b fase 4: no se persiste ningun bloque sucio. El flush corre antes de
  // escribir el fichero; si sigue en vuelo, el guardado se reanuda cuando la
  // cadena de flush termina (resynthesizeIncremental -> resumeSaveAfterFlush).
  if (!resumeSaveAfterFlush &&
      (project->hasDirtyNotes() || project->hasF0DirtyRange()))
  {
    resumeSaveAfterFlush = true;
    saveAsAfterFlush = saveAs;
    deferredRenderPending = true;
    playAfterFlush = false;
    toolbar.showProgress(TR("progress.synthesizing"));
    toolbar.setProgress(-1.0f);
    flushDirtyBlocks();
    if (flushChainActive)
      return; // the save resumes when the last block is clean
    resumeSaveAfterFlush = false;
  }

  project->uiWidth = getWidth();
  project->uiHeight = getHeight();

  auto ensureAudioSha = [project]()
  {
    auto audioFile = project->getFilePath();
    if (audioFile.existsAsFile())
      project->setAudioSha256(SHA256Utils::fileSHA256(audioFile));
  };

  auto target = saveAs ? juce::File{} : project->getProjectFilePath();
  if (target == juce::File{})
  {
    // Prevent re-triggering while dialog is open
    if (fileChooser != nullptr)
      return;

    // Default next to audio if possible
    auto audio = project->getFilePath();
    if (audio.existsAsFile())
      target = audio.withFileExtension("pitchnet");
    else
      target =
          juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
              .getChildFile("Untitled.pitchnet");

#if JUCE_WINDOWS && JUCE_MODAL_LOOPS_PERMITTED
    juce::FileChooser chooser(TR("dialog.save_project"), target, "*.pitchnet",
                              true, false, this);
    if (!chooser.browseForFileToSave(true))
      return;

    auto file = chooser.getResult();
    if (file == juce::File{})
      return;

    if (file.getFileExtension().isEmpty())
      file = file.withFileExtension("pitchnet");

    toolbar.showProgress(TR("progress.saving"));
    toolbar.setProgress(-1.0f);

    ensureAudioSha();
    const bool ok = ProjectSerializer::saveToFile(*project, file);
    if (ok)
      project->setProjectFilePath(file);

    toolbar.hideProgress();
    return;
#else
    fileChooser = std::make_unique<juce::FileChooser>(
        TR("dialog.save_project"), target, "*.pitchnet");

    auto chooserFlags = juce::FileBrowserComponent::saveMode |
                        juce::FileBrowserComponent::canSelectFiles |
                        juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    fileChooser->launchAsync(chooserFlags, [safeThis](
                                               const juce::FileChooser &fc)
                             {
      if (safeThis == nullptr)
        return;

      auto file = fc.getResult();
      safeThis->fileChooser
          .reset(); // Allow next dialog to open (must be after getResult)

      if (file == juce::File{})
        return;

      if (file.getFileExtension().isEmpty())
        file = file.withFileExtension("pitchnet");

      safeThis->toolbar.showProgress(TR("progress.saving"));
      safeThis->toolbar.setProgress(-1.0f);

      auto *project = safeThis ? safeThis->getProject() : nullptr;
      if (!project) {
        safeThis->toolbar.hideProgress();
        return;
      }
      auto audioFile = project->getFilePath();
      if (audioFile.existsAsFile())
        project->setAudioSha256(SHA256Utils::fileSHA256(audioFile));
      const bool ok = ProjectSerializer::saveToFile(*project, file);
      if (ok)
        project->setProjectFilePath(file);

      safeThis->toolbar.hideProgress(); });

    return;
#endif
  }

  toolbar.showProgress(TR("progress.saving"));
  toolbar.setProgress(-1.0f);

  ensureAudioSha();
  const bool ok = ProjectSerializer::saveToFile(*project, target);
  juce::ignoreUnused(ok);

  toolbar.hideProgress();
}

void MainComponent::openFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->openFile(); });
    return;
  }

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.select_audio"), juce::File{},
      "*.pitchnet;*.wav;*.mp3;*.flac;*.aiff");

  auto chooserFlags = juce::FileBrowserComponent::openMode |
                      juce::FileBrowserComponent::canSelectFiles;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc)
      {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser.reset(); // Allow next dialog to open
        if (file.existsAsFile()) {
          safeThis->addRecentFile(file);
          if (file.hasFileExtension("pitchnet") || file.hasFileExtension(".pitchnet"))
            safeThis->openProjectFile(file);
          else
            safeThis->loadAudioFile(file);
        } });
}

void MainComponent::refreshRecentFilesMenu()
{
  if (menuHandler)
    menuHandler->setRecentFiles(recentFiles);
  if (menuHandler)
    menuHandler->menuItemsChanged();
}

void MainComponent::addRecentFile(const juce::File &file)
{
  if (!file.existsAsFile())
    return;

  const auto fullPath = file.getFullPathName();
  recentFiles.removeString(fullPath);
  recentFiles.insert(0, fullPath);

  constexpr int kMaxRecentFiles = 10;
  while (recentFiles.size() > kMaxRecentFiles)
    recentFiles.remove(recentFiles.size() - 1);

  if (settingsManager)
  {
    settingsManager->setLastFilePath(file);
    settingsManager->setRecentFiles(recentFiles);
    settingsManager->saveConfig();
  }
  refreshRecentFilesMenu();
}

void MainComponent::refreshLocalisedText()
{
  // Command names and categories are read back from the string table every
  // time JUCE asks for them, so the menus only need to be told to reread.
  if (commandManager)
    commandManager->commandStatusChanged();

  if (menuHandler)
  {
    menuHandler->menuItemsChanged();

#if JUCE_MAC
    if (!isPluginMode())
    {
      macExtraAppleMenuItems = menuHandler->getMacExtraAppleMenu();
      juce::MenuBarModel::setMacMainMenu(menuHandler.get(),
                                         &macExtraAppleMenuItems);
    }
#endif
  }

  resized();
  repaint();
}

void MainComponent::openRecentFile(const juce::File &file)
{
  if (!file.existsAsFile())
  {
    StyledMessageBox::show(this, TR("dialog.recent_missing"),
                           TR("dialog.file_not_found") + "\n" +
                               file.getFullPathName(),
                           StyledMessageBox::WarningIcon);
    recentFiles.removeString(file.getFullPathName());
    if (settingsManager)
    {
      settingsManager->setRecentFiles(recentFiles);
      settingsManager->saveConfig();
    }
    refreshRecentFilesMenu();
    return;
  }

  if (file.hasFileExtension("pitchnet") || file.hasFileExtension(".pitchnet"))
    openProjectFile(file);
  else
    loadAudioFile(file);
}

// openProjectFile and loadAudioFile implementations are in Main/MainComponent_ProjectIO.cpp

void MainComponent::analyzeAudio()
{
  auto *project = getProject();
  if (!project || !editorController)
    return;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->analyzeAudioAsync(
      [safeThis](Project &projectRef)
      {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.setProject(&projectRef);
        safeThis->pianoRollView.setProject(&projectRef);
        safeThis->fitAnalyzedPitchRangeToView(projectRef);
        safeThis->pianoRoll.repaint();

        // Analysis may have fallen back to CPU inference and taken the
        // synthesis engine with it. Keep the Synthesis card showing whatever
        // is actually rendering.
        if (safeThis->editorController != nullptr)
          safeThis->parameterPanel.setSynthesisEngine(
              safeThis->editorController->getSynthesisEngineType());
      },
      [safeThis]()
      {
        if (safeThis == nullptr)
          return;
        safeThis->notifyProjectDataChanged();
      });
}

void MainComponent::analyzeAudio(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress,
    std::function<void()> onComplete)
{
  if (editorController)
  {
    editorController->analyzeAudio(targetProject, onProgress, onComplete);
  }
}

void MainComponent::zoomToFull()
{
  auto *project = getProject();
  if (project == nullptr)
    return;
  // The piano roll's widest zoom shows the whole timeline (down to
  // MIN_PIXELS_PER_SECOND for a long one); start at its left edge.
  pianoRoll.setPixelsPerSecond(0.0f, false);
  pianoRoll.setScrollX(pianoRoll.getTimelineStart() * pianoRoll.getPixelsPerSecond());
  onZoomChanged(pianoRoll.getPixelsPerSecond());
  if (pianoRoll.onScrollChanged)
    pianoRoll.onScrollChanged(pianoRoll.getScrollX());
  fitAnalyzedPitchRangeToView(*project);
}

void MainComponent::fitAnalyzedPitchRangeToView(Project &project)
{
  if (pianoRoll.getVisibleContentHeight() <= 0)
  {
    // D-176: the initial ARA selection can attach the project before the
    // canvas is sized; retry after layout instead of leaving the previous
    // track's pitch range on screen.
    hasPendingPitchFit = true;
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
    ARA_DIAG("fitPitchRange deferred notes=" +
             juce::String(static_cast<int>(project.getNotes().size())) +
             " height=0");
#endif
    return;
  }
  hasPendingPitchFit = false;
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  ARA_DIAG("fitPitchRange run notes=" +
           juce::String(static_cast<int>(project.getNotes().size())) +
           " height=" + juce::String(pianoRoll.getVisibleContentHeight()));
#endif

  float minMidi = std::numeric_limits<float>::max();
  float maxMidi = std::numeric_limits<float>::lowest();

  for (const auto &note : project.getNotes())
  {
    if (note.isRest())
      continue;

    const float midi = note.getAdjustedMidiNote();
    if (!std::isfinite(midi))
      continue;

    minMidi = std::min(minMidi, midi);
    maxMidi = std::max(maxMidi, midi);
  }

  if (minMidi == std::numeric_limits<float>::max())
  {
    const auto &f0 = project.getAudioData().f0;
    for (float freq : f0)
    {
      if (freq <= 50.0f || !std::isfinite(freq))
        continue;

      const float midi = freqToMidi(freq);
      if (!std::isfinite(midi))
        continue;

      minMidi = std::min(minMidi, midi);
      maxMidi = std::max(maxMidi, midi);
    }
  }

  if (minMidi != std::numeric_limits<float>::max() && maxMidi >= minMidi)
  {
    maxMidi = std::min(maxMidi + 1.0f, static_cast<float>(MAX_MIDI_NOTE));
    pianoRoll.fitPitchRangeToView(minMidi, maxMidi);
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
    ARA_DIAG("fitPitchRange applied min=" + juce::String(minMidi, 2) +
             " max=" + juce::String(maxMidi, 2));
#endif
  }
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  else
    ARA_DIAG("fitPitchRange noData");
#endif
}

void MainComponent::exportFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->exportFile(); });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  const int inputSampleRate = project->getAudioData().sampleRate;
  juce::Component::SafePointer<MainComponent> safeThis(this);
  ExportHelper::showExportSettingsDialogAsync(
      this, inputSampleRate, settingsManager->getExportSettings(),
      [safeThis](std::optional<ExportHelper::ExportSettings> exportSettings)
      {
        if (safeThis == nullptr || !exportSettings.has_value())
          return;

        safeThis->settingsManager->setExportSettings(*exportSettings);
        safeThis->settingsManager->saveConfig();

        auto performExport = [safeThis, exportSettings](const juce::File &targetFile)
        {
          if (safeThis == nullptr)
            return;

          auto *activeProject = safeThis->getProject();
          if (!activeProject)
            return;

          auto file = targetFile;
          const auto extension = ExportHelper::getFormatExtension(exportSettings->format);
          if (file.getFileExtension().isEmpty())
            file = file.withFileExtension(extension);

          auto &audioData = activeProject->getAudioData();
          juce::AudioBuffer<float> sourceBuffer;
          sourceBuffer.makeCopyOf(audioData.waveform);
          const int sourceRate = audioData.sampleRate;

          safeThis->toolbar.showProgress(TR("progress.exporting_audio"));
          safeThis->toolbar.setProgress(-1.0f);
          safeThis->toolbar.setEnabled(false);

          std::thread([safeThis, settings = *exportSettings, file = std::move(file),
                       sourceBuffer = std::move(sourceBuffer), sourceRate]() mutable
                      {
            juce::String error;
            bool success = false;

            do {
              juce::AudioBuffer<float> exportBuffer =
                  ExportHelper::convertChannels(sourceBuffer, settings.channels);
              exportBuffer = ExportHelper::resampleAudio(exportBuffer, sourceRate, settings.sampleRate);

              if (file.existsAsFile() && !file.deleteFile()) {
                error = TR("dialog.failed_delete") + "\n" + file.getFullPathName();
                break;
              }

              auto fileStream = std::make_unique<juce::FileOutputStream>(file);
              if (!fileStream || !fileStream->openedOk()) {
                error = TR("dialog.failed_open") + "\n" + file.getFullPathName();
                break;
              }
              std::unique_ptr<juce::OutputStream> outputStream = std::move(fileStream);

              juce::AudioFormatManager formatManager;
              formatManager.registerBasicFormats();
              auto *format = ExportHelper::findFormatForExtension(
                  formatManager, ExportHelper::getFormatExtension(settings.format));
              if (!format) {
                error = TR("error.no_exporter") + " " +
                        ExportHelper::getFormatDisplayName(settings.format);
                break;
              }

              auto writerOptions = juce::AudioFormatWriterOptions{}
                                       .withSampleRate(settings.sampleRate)
                                       .withNumChannels(exportBuffer.getNumChannels())
                                       .withBitsPerSample(settings.bitsPerSample);
              constexpr int ogg192KbpsQualityOption = 6;
              if (settings.format == ExportHelper::ExportFormat::ogg)
                writerOptions = writerOptions.withQualityOptionIndex(
                    ogg192KbpsQualityOption);

              auto writer = format->createWriterFor(outputStream, writerOptions);
              if (!writer) {
                error = TR("dialog.failed_create_writer") + "\n" +
                        file.getFullPathName();
                break;
              }

              success = writer->writeFromAudioSampleBuffer(
                  exportBuffer, 0, exportBuffer.getNumSamples());
              writer->flush();
              writer.reset();
              if (!success) {
                error = TR("dialog.failed_write") + "\n" + file.getFullPathName();
                break;
              }
            } while (false);

            juce::MessageManager::callAsync([safeThis, success, error]() {
              if (safeThis == nullptr)
                return;
              safeThis->toolbar.setEnabled(true);
              safeThis->toolbar.hideProgress();
              if (!success) {
                StyledMessageBox::show(
                    safeThis.getComponent(), TR("dialog.export_failed"), error,
                    StyledMessageBox::WarningIcon);
              }
            }); })
              .detach();
        };

        if (safeThis->fileChooser != nullptr)
          return;

        juce::File defaultFile;
        if (!safeThis->isPluginMode())
        {
          if (auto *activeProject = safeThis->getProject())
          {
            if (activeProject->getFilePath().existsAsFile())
              defaultFile = activeProject->getFilePath();
            else if (activeProject->getProjectFilePath().existsAsFile())
              defaultFile = activeProject->getProjectFilePath();

            // Save dialogs need a full file path to initialise both the folder
            // and filename consistently across native platform backends.
            if (defaultFile != juce::File{})
              defaultFile = defaultFile.withFileExtension(
                  ExportHelper::getFormatExtension(exportSettings->format));
          }
        }

        safeThis->fileChooser = std::make_unique<juce::FileChooser>(
            TR("dialog.save_audio"), defaultFile,
            ExportHelper::getFormatWildcard(exportSettings->format));

        auto chooserFlags = juce::FileBrowserComponent::saveMode |
                            juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting;

        safeThis->fileChooser->launchAsync(
            chooserFlags, [safeThis, performExport](const juce::FileChooser &fc)
            {
              if (safeThis == nullptr)
                return;
              const auto file = fc.getResult();
              safeThis->fileChooser.reset();
              if (file == juce::File{})
                return;
              performExport(file); });
      });
}

void MainComponent::exportMidiFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->exportMidiFile(); });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  const auto &notes = project->getNotes();
  if (notes.empty())
  {
    StyledMessageBox::show(this, TR("dialog.export_failed"),
                           TR("dialog.no_notes_to_export"),
                           StyledMessageBox::WarningIcon);
    return;
  }

  // Suggest filename based on project or audio file
  juce::File defaultFile;
  if (project->getFilePath().existsAsFile())
  {
    defaultFile = project->getFilePath().withFileExtension("mid");
  }
  else if (project->getProjectFilePath().existsAsFile())
  {
    defaultFile = project->getProjectFilePath().withFileExtension("mid");
  }

#if JUCE_WINDOWS && JUCE_MODAL_LOOPS_PERMITTED
  juce::FileChooser chooser(TR("dialog.export_midi"), defaultFile,
                            "*.mid;*.midi", true, false, this);
  if (!chooser.browseForFileToSave(true))
    return;

  auto file = chooser.getResult();
  if (file == juce::File{})
    return;

  // Ensure .mid extension
  if (file.getFileExtension().isEmpty())
    file = file.withFileExtension("mid");

  // Show progress
  toolbar.showProgress(TR("progress.exporting_midi"));
  toolbar.setProgress(0.3f);

  // Configure export options
  MidiExporter::ExportOptions options;
  options.tempo = static_cast<float>(project->getTimelineTempoBpm());
  options.ticksPerQuarterNote = 480;
  options.velocity = 100;
  options.quantizePitch = true; // Round to nearest semitone

  toolbar.setProgress(0.6f);

  // Export using adjusted pitch data (includes user edits)
  bool success = MidiExporter::exportToFile(project->getNotes(), file, options);

  toolbar.setProgress(1.0f);
  toolbar.hideProgress();

  if (!success)
  {
    StyledMessageBox::show(
        this, TR("dialog.export_failed"),
        TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
        StyledMessageBox::WarningIcon);
  }
#else
  fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.export_midi"), defaultFile, "*.mid;*.midi");

  auto chooserFlags = juce::FileBrowserComponent::saveMode |
                      juce::FileBrowserComponent::canSelectFiles |
                      juce::FileBrowserComponent::warnAboutOverwriting;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc)
      {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser
            .reset(); // Allow next dialog to open (must be after getResult)

        if (file == juce::File{})
          return;

        // Ensure .mid extension
        if (file.getFileExtension().isEmpty())
          file = file.withFileExtension("mid");

        // Show progress
        safeThis->toolbar.showProgress(TR("progress.exporting_midi"));
        safeThis->toolbar.setProgress(0.3f);

        auto *project = safeThis ? safeThis->getProject() : nullptr;
        if (!project)
          return;

        // Configure export options
        MidiExporter::ExportOptions options;
        options.tempo = static_cast<float>(project->getTimelineTempoBpm());
        options.ticksPerQuarterNote = 480;
        options.velocity = 100;
        options.quantizePitch = true; // Round to nearest semitone

        safeThis->toolbar.setProgress(0.6f);

        // Export using adjusted pitch data (includes user edits)
        bool success =
            MidiExporter::exportToFile(project->getNotes(), file, options);

        safeThis->toolbar.setProgress(1.0f);
        safeThis->toolbar.hideProgress();

        if (!success) {
          StyledMessageBox::show(
              safeThis.getComponent(), TR("dialog.export_failed"),
              TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
              StyledMessageBox::WarningIcon);
        } });
#endif
}

void MainComponent::play()
{
  auditionOutput.yieldToTransport(); // the user's transport choice wins
  auto *project = getProject();

  // In plugin mode, playback is controlled by the host. Update immediately for
  // responsiveness; host/ARA callbacks will keep the state in sync.
  if (isPluginMode())
  {
    if (onRequestHostPlayState)
      onRequestHostPlayState(true);
    isPlaying = true;
    toolbar.setPlaying(true);
    pianoRoll.setPlaybackActive(true);
    return;
  }

  if (!project || project->getAudioData().waveform.getNumSamples() == 0)
  {
    isPlaying = false;
    toolbar.setPlaying(false);
    pianoRoll.setPlaybackActive(false);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;

  const auto &loopRange = project->getLoopRange();
  if (loopRange.isValid())
  {
    double position = audioEngine->getPosition();
    if (position < loopRange.startSeconds || position >= loopRange.endSeconds)
    {
      audioEngine->seek(loopRange.startSeconds);
      pianoRoll.setCursorTime(loopRange.startSeconds);
      toolbar.setCurrentTime(loopRange.startSeconds);
    }
  }

  isPlaying = true;
  toolbar.setPlaying(true);
  pianoRoll.setPlaybackActive(true);
  audioEngine->play();
}

void MainComponent::pause()
{
  auditionOutput.yieldToTransport(); // the user's transport choice wins
  if (previewRegionActive)
  {
    finishPreviewRegion(true);
    return;
  }

  // In plugin mode, playback is controlled by the host
  if (isPluginMode())
  {
    if (onRequestHostPlayState)
      onRequestHostPlayState(false);
    isPlaying = false;
    toolbar.setPlaying(false);
    pianoRoll.setPlaybackActive(false);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  isPlaying = false;
  toolbar.setPlaying(false);
  pianoRoll.setPlaybackActive(false);
  audioEngine->pause();
}

void MainComponent::stop()
{
  auditionOutput.yieldToTransport(); // the user's transport choice wins
  if (previewRegionActive)
  {
    finishPreviewRegion(false);
  }

  // In plugin mode, playback is controlled by the host
  if (isPluginMode())
  {
    if (onRequestHostStop)
      onRequestHostStop();
    isPlaying = false;
    toolbar.setPlaying(false);
    pianoRoll.setPlaybackActive(false);
    seek(0.0);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  isPlaying = false;
  toolbar.setPlaying(false);
  pianoRoll.setPlaybackActive(false);
  audioEngine->stop();
  // With Auto Scroll off, don't jump the view back to the start.
  seek(0.0, toolbar.isFollowPlayback());
}

void MainComponent::seek(double time, bool scrollToCursor)
{
  // In plugin mode, request a host seek when the host supports it and update
  // the UI cursor immediately for responsiveness.
  if (isPluginMode())
  {
    // 'time' is project time; the host expects its own timeline.
    if (onRequestHostSeek)
      onRequestHostSeek(pianoRoll.projectToTimeline(time));
    pendingCursorTime.store(time);
    pianoRoll.setCursorTime(time);
    toolbar.setCurrentTime(time);
    return;
  }

  // Standalone mode: use AudioEngine for seeking
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  audioEngine->seek(time);
  pendingCursorTime.store(time);
  pianoRoll.setCursorTime(time);
  toolbar.setCurrentTime(time);

  if (!scrollToCursor)
    return;

  // Scroll view to make cursor visible
  float cursorX = static_cast<float>(time * pianoRoll.getPixelsPerSecond());
  float viewWidth = static_cast<float>(pianoRoll.getVisibleContentWidth());
  float scrollX = static_cast<float>(pianoRoll.getScrollX());

  // If cursor is outside visible area, scroll to show it
  if (cursorX < scrollX || cursorX > scrollX + viewWidth)
  {
    // Center cursor in view, or scroll to start if time is 0
    double newScrollX;
    if (time < 0.001)
    {
      newScrollX = 0.0; // Go to start
    }
    else
    {
      newScrollX =
          std::max(0.0, static_cast<double>(cursorX - viewWidth * 0.3));
    }
    pianoRoll.setScrollX(newScrollX);
  }
}

void MainComponent::previewNoteRegion(int startFrame, int endFrame)
{
  if (endFrame <= startFrame)
    return;
  auditionOutput.claim(AuditionOutput::Source::RegionPreview);

  const double startTime = framesToSeconds(startFrame);
  const double endTime = framesToSeconds(endFrame);
  if (endTime <= startTime)
    return;

  if (isPluginMode())
  {
    previewRegionActive = true;
    previewRegionStartTime = startTime;
    previewRegionEndTime = endTime;
    previewRegionReturnTime = pianoRoll.getCursorTime();
    previewRegionWallClockStartMs = juce::Time::getMillisecondCounterHiRes();
    previewRegionWasProjectPlaying = isPlaying;
    pianoRoll.setPreviewPlaybackState(true, startFrame, endFrame);
    pianoRoll.setPreviewPlaybackPosition(startTime);

    if (auto *project = getProject())
      auditionOutput.startHostPreview(AuditionOutput::Source::RegionPreview,
                                      *project, startFrame, endFrame);

    return;
  }

  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;

  previewRegionActive = true;
  previewRegionStartTime = startTime;
  previewRegionEndTime = endTime;
  previewRegionReturnTime = pianoRoll.getCursorTime();
  previewRegionWallClockStartMs = juce::Time::getMillisecondCounterHiRes();
  previewRegionWasProjectPlaying = isPlaying;
  pianoRoll.setPreviewPlaybackState(true, startFrame, endFrame);
  pianoRoll.setPreviewPlaybackPosition(startTime);

  audioEngine->seek(startTime);
  audioEngine->play();
}

void MainComponent::finishPreviewRegion(bool restorePosition)
{
  if (!previewRegionActive)
    return;

  auditionOutput.release(AuditionOutput::Source::RegionPreview);
  const double returnTime = previewRegionReturnTime;
  const bool shouldResumeProjectPlayback =
      previewRegionWasProjectPlaying && restorePosition;
  previewRegionActive = false;
  previewRegionStartTime = 0.0;
  previewRegionWallClockStartMs = 0.0;
  previewRegionWasProjectPlaying = false;
  pianoRoll.setPreviewPlaybackState(false, 0, 0);

  if (isPluginMode())
  {
    auditionOutput.stopHostPreview();

    juce::ignoreUnused(shouldResumeProjectPlayback);
    hasPendingCursorUpdate.store(false);
    return;
  }

  if (auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr)
  {
    audioEngine->pause();
    if (restorePosition)
      audioEngine->seek(returnTime);
    if (shouldResumeProjectPlayback)
      audioEngine->play();
  }

  pendingCursorTime.store(returnTime);
  hasPendingCursorUpdate.store(false);
}

void MainComponent::auditionDraggedNote(const Note &note)
{
  if (!dragAuditionEnabled || noteDragAuditionSuppressed ||
      !resampledLoopAudition || note.isRest())
    return;

  // VariAudio acoustic feedback: while a segment is dragged its whole audio
  // repeats from the start; a new rendering is needed only when the pitch
  // (or the dragged segment) changes. The player keeps its position across
  // the swap, so the repetition does not restart on every pitch step.
  if (noteDragAuditionActive && &note == lastAuditionedNote &&
      std::abs(note.getAdjustedMidiNote() - lastAuditionedMidi) < 0.01f)
    return;
  if (noteDragAuditionActive && &note != lastAuditionedNote)
    noteDragAuditionBackendPreviewStarted = false; // new segment, new range
  lastAuditionedNote = &note;
  lastAuditionedMidi = note.getAdjustedMidiNote();

  std::vector<float> source;
  if (auto *project = getProject())
  {
    // Use the same timeline audio that the normal note-preview signal path
    // reads, rather than depending on optional/stale per-note clip caches.
    const auto &audioData = project->getAudioData();
    const auto &waveform = audioData.originalWaveform.getNumSamples() > 0
                               ? audioData.originalWaveform
                               : audioData.waveform;
    const int startFrame = note.getSrcStartFrame() >= 0
                               ? note.getSrcStartFrame()
                               : note.getStartFrame();
    const int endFrame = note.getSrcEndFrame() > startFrame
                             ? note.getSrcEndFrame()
                             : note.getEndFrame();
    const int startSample = juce::jlimit(0, waveform.getNumSamples(),
                                         startFrame * HOP_SIZE);
    const int endSample = juce::jlimit(startSample, waveform.getNumSamples(),
                                       endFrame * HOP_SIZE);
    if (endSample > startSample)
    {
      const auto *samples = waveform.getReadPointer(0);
      source.assign(samples + startSample, samples + endSample);
    }
  }

  if (source.empty())
    return;

  if (!noteDragAuditionActive)
  {
    noteDragAuditionActive = true;
    noteDragAuditionBackendPreviewStarted = false;
    auditionOutput.claim(AuditionOutput::Source::DraggedSegment);
    if (!isPluginMode())
    {
      auto *engine = editorController ? editorController->getAudioEngine() : nullptr;
      if (!engine)
      {
        noteDragAuditionActive = false;
        auditionOutput.release(AuditionOutput::Source::DraggedSegment);
        return;
      }
      noteDragAuditionWasPlaying = engine->isPlaying();
      noteDragAuditionReturnTime = engine->getPosition();
    }
  }

  const float shift = note.getAdjustedMidiNote() - note.getOriginalMidiNote();
  noteDragAuditionStartFrame = note.getStartFrame();
  noteDragAuditionEndFrame = note.getEndFrame();
  pendingNoteDragAuditionSource = std::move(source);
  pendingNoteDragAuditionShift = shift;
  pendingNoteDragAuditionMidiNote = note.getAdjustedMidiNote();
  noteDragAuditionLastInputMs = juce::Time::currentTimeMillis();
  noteDragAuditionPending = true;
}

void MainComponent::dispatchPendingDragAudition()
{
  if (!noteDragAuditionActive || !noteDragAuditionPending ||
      !resampledLoopAudition)
    return;

  const auto now = juce::Time::currentTimeMillis();
  if (now - noteDragAuditionLastInputMs < noteDragAuditionDebounceMs)
    return;

  noteDragAuditionPending = false;
  resampledLoopAudition->request(std::move(pendingNoteDragAuditionSource),
                                 pendingNoteDragAuditionShift,
                                 pendingNoteDragAuditionMidiNote,
                                 /*wholeSegment*/ true);
}

void MainComponent::abandonDragAudition(bool stopSound)
{
  if (!noteDragAuditionActive)
    return;
  // The feedback lives only while the mouse is down, so an active audition
  // means a drag is in progress: keep it silent for the rest of that drag
  // (otherwise the next mouse move would start it again over the transport).
  noteDragAuditionSuppressed = true;
  auditionOutput.release(AuditionOutput::Source::DraggedSegment);
  if (stopSound)
    auditionOutput.stopLoop();
  if (resampledLoopAudition)
    resampledLoopAudition->cancel();
  noteDragAuditionActive = false;
  noteDragAuditionPending = false;
  noteDragAuditionBackendPreviewStarted = false;
  noteDragAuditionWasPlaying = false;
  lastAuditionedNote = nullptr;
  pendingNoteDragAuditionSource.clear();
}

void MainComponent::finishDraggedNoteAudition()
{
  // Mouse up ends the feedback, as in VariAudio.
  noteDragAuditionSuppressed = false;
  cancelDraggedNoteAudition();
}

void MainComponent::cancelDraggedNoteAudition()
{
  if (!noteDragAuditionActive)
    return;
  lastAuditionedNote = nullptr;
  auditionOutput.release(AuditionOutput::Source::DraggedSegment);

  if (resampledLoopAudition)
    resampledLoopAudition->cancel();
  noteDragAuditionPending = false;
  pendingNoteDragAuditionSource.clear();

  auditionOutput.stopLoop();
  if (isPluginMode())
  {
    noteDragAuditionActive = false;
    noteDragAuditionBackendPreviewStarted = false;
    return;
  }

  if (auto *engine = editorController ? editorController->getAudioEngine() : nullptr)
  {
    if (noteDragAuditionWasPlaying)
      engine->play();
    else
      engine->pause();
  }

  noteDragAuditionActive = false;
  noteDragAuditionBackendPreviewStarted = false;
  noteDragAuditionWasPlaying = false;
}

void MainComponent::auditionPianoKey(int midiNote)
{
  constexpr double bufferDurationSeconds = 1.0;
  constexpr double fallbackSampleRate = 44100.0;
  constexpr float amplitude = 0.18f;
  constexpr int fadeSamples = 256;

  const double sampleRate =
      getProject() && getProject()->getAudioData().sampleRate > 0
          ? static_cast<double>(getProject()->getAudioData().sampleRate)
          : fallbackSampleRate;
  const int samples =
      juce::jmax(2, juce::roundToInt(bufferDurationSeconds * sampleRate));
  juce::AudioBuffer<float> tone(1, samples);
  const double frequency = 440.0 * std::pow(2.0, (midiNote - 69) / 12.0);

  for (int sample = 0; sample < samples; ++sample)
  {
    const float fadeIn =
        juce::jmin(1.0f, static_cast<float>(sample) / fadeSamples);
    const float fadeOut = juce::jmin(
        1.0f, static_cast<float>(samples - 1 - sample) / fadeSamples);
    const float value = amplitude * fadeIn * fadeOut *
                        std::sin(juce::MathConstants<double>::twoPi * frequency *
                                 static_cast<double>(sample) / sampleRate);
    tone.setSample(0, sample, value);
  }

  // The key's tone replaces a still-sounding segment audition; take over its
  // transport state rather than reading the audition's own "playing" flag.
  const bool takeOverDrag = noteDragAuditionActive;
  const bool dragWasPlaying = noteDragAuditionWasPlaying;
  constexpr auto source = AuditionOutput::Source::PianoKey;
  auditionOutput.claim(source);

  if (isPluginMode())
  {
    // ARA starts the host preview signal flow for a range; the temporary tone
    // buffer then replaces that source in the established audition path.
    if (auto *project = getProject())
      auditionOutput.startHostPreview(source, *project, 0, 1);
    auditionOutput.playLoop(source, tone, sampleRate, /*wholeSegment*/ false);
  }
  else if (auto *engine = editorController ? editorController->getAudioEngine()
                                            : nullptr)
  {
    pianoKeyAuditionWasPlaying = takeOverDrag ? dragWasPlaying : engine->isPlaying();
    pianoKeyAuditionReturnTime = engine->getPosition();
    auditionOutput.playLoop(source, tone, sampleRate, /*wholeSegment*/ false);
  }
  else
  {
    auditionOutput.release(source);
    return;
  }

  pianoKeyAuditionActive = true;
}

void MainComponent::finishPianoKeyAudition()
{
  if (!pianoKeyAuditionActive)
    return;
  auditionOutput.release(AuditionOutput::Source::PianoKey);

  auditionOutput.stopLoop();
  if (auto *engine = !isPluginMode() && editorController
                         ? editorController->getAudioEngine()
                         : nullptr)
  {
    if (pianoKeyAuditionWasPlaying)
      engine->play();
    else
    {
      engine->pause();
      engine->seek(pianoKeyAuditionReturnTime);
    }
  }

  pianoKeyAuditionActive = false;
  pianoKeyAuditionWasPlaying = false;
}

void MainComponent::jumpTransport(bool forward)
{
  auto *project = getProject();
  if (!project)
    return;

  double currentTime = pendingCursorTime.load();
  if (!isPluginMode())
  {
    if (auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr)
      currentTime = audioEngine->getPosition();
  }

  const double duration = project->getAudioData().getDuration();
  double targetTime = currentTime;

  if (project->getTimelineDisplayMode() == TimelineDisplayMode::Beats)
  {
    const double bpm = juce::jlimit(20.0, 300.0, project->getTimelineTempoBpm());
    const int numerator = juce::jmax(1, project->getTimelineBeatNumerator());
    const int denominator = juce::jmax(1, project->getTimelineBeatDenominator());
    const double beatSeconds = (60.0 / bpm) * (4.0 / static_cast<double>(denominator));
    const double barSeconds = beatSeconds * static_cast<double>(numerator);

    if (barSeconds > 1.0e-6)
    {
      constexpr double epsilon = 1.0e-6;
      const double barPosition = currentTime / barSeconds;
      targetTime = forward
                       ? (std::floor(barPosition + epsilon) + 1.0) * barSeconds
                       : (std::ceil(barPosition - epsilon) - 1.0) * barSeconds;
    }
  }
  else
  {
    constexpr double epsilon = 1.0e-6;
    targetTime = forward
                     ? (std::floor(currentTime / 2.0 + epsilon) + 1.0) * 2.0
                     : (std::ceil(currentTime / 2.0 - epsilon) - 1.0) * 2.0;
  }

  seek(juce::jlimit(0.0, duration, targetTime));
}

void MainComponent::resynthesizeIncremental()
{

  auto *project = getProject();
  if (!project || !editorController)
  {
    return;
  }

  mcpRenderPending = true;
  ++mcpRenderGeneration;

  toolbar.showProgress(TR("progress.synthesizing"));
  toolbar.setProgress(-1.0f);
  setToolGroupEnabled(false);
  showRenderStatus(ParameterPanel::RenderStatus::Rendering);

  if (isPluginMode() && onRequestBackendRender)
  {
    onRequestBackendRender(*project);
    return;
  }

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->resynthesizeIncrementalAsync(
      *project,
      [safeThis](const juce::String &message)
      {
        if (safeThis == nullptr)
          return;
        safeThis->toolbar.showProgress(message);
        // P1b fase 6: el barrido avanza con los repintados; el progreso del
        // render no toca el piano roll por su cuenta.
        safeThis->pianoRoll.repaint();
      },
      [safeThis](bool success)
      {
        if (safeThis == nullptr)
          return;

        safeThis->mcpRenderPending = false;

        safeThis->setToolGroupEnabled(true);
        safeThis->toolbar.hideProgress();
        safeThis->showRenderStatus(success ? ParameterPanel::RenderStatus::Done
                                           : ParameterPanel::RenderStatus::Failed);

        if (!success)
        {
          return;
        }

        // P1b: the flush continues block by block. The project renders one
        // silence-bounded block per pass (D-197), so the transport waits until
        // none is left dirty.
        if (safeThis->flushChainActive)
        {
          auto *chainProject = safeThis->getProject();
          if (chainProject != nullptr &&
              (chainProject->hasDirtyNotes() || chainProject->hasF0DirtyRange()))
          {
            safeThis->resynthesizeIncremental();
            return;
          }

          safeThis->deferredRenders.clear();
          safeThis->pushSweepState();
          safeThis->flushChainActive = false;
          if (safeThis->playAfterFlush)
          {
            safeThis->playAfterFlush = false;
            safeThis->play();
          }
          // P1b fase 4: el guardado que esperaba al flush se reanuda aqui,
          // con los bloques ya limpios.
          if (safeThis->resumeSaveAfterFlush)
          {
            safeThis->resumeSaveAfterFlush = false;
            safeThis->saveProject(safeThis->saveAsAfterFlush);
          }
        }

        safeThis->pianoRoll.invalidateWaveformCache();
        safeThis->pianoRoll.repaint();
        if (safeThis->isPluginMode())
          safeThis->notifyProjectDataChanged();
      },
      pendingIncrementalResynth,
      isPluginMode());
}

// P1b: an edit, an undo, an engine change, an MCP command or DAW automation
// only marks. The audio is not touched here; the blocks wait for Play.
void MainComponent::markRenderDirty()
{
  auto *project = getProject();
  if (project == nullptr || editorController == nullptr)
    return;

  const auto range = project->getDirtyFrameRange();
  if (range.first >= 0 && range.second > range.first)
    deferredRenders.markBlock(range.first, range.second);

  deferredRenderPending = true;
  pushSweepState();
  pianoRoll.repaint();
}

// P1b fase 6: el barrido se alimenta de la cola, no del proyecto.
void MainComponent::pushSweepState()
{
  const auto state = sweep::fromQueue(deferredRenders);
  pianoRoll.setSweepState(state.active, state.blockStart, state.blockEnd,
                          state.remaining);
}

// P1b: Play from the UI. Renders the dirty blocks first and only then starts
// the transport; the cursor does not advance until the last block is done.
void MainComponent::flushDirtyBlocks()
{
  if (!deferredRenderPending || flushChainActive)
    return;

  auto *project = getProject();
  if (project == nullptr || editorController == nullptr)
  {
    deferredRenderPending = false;
    return;
  }

  deferredRenderPending = false;
  flushChainActive = true;
  resynthesizeIncremental();
}

#define DBG_P1B(msg)                                                          \
  do {                                                                        \
    juce::File::getSpecialLocation(juce::File::tempDirectory)                 \
        .getChildFile("pitchcore_p1b_trace.log")                              \
        .appendText("[" +                                                      \
                    juce::Time::getCurrentTime().toString(true, true, true,    \
                                                           true) +            \
                    "][tid=" +                                                 \
                    juce::String::toHexString((juce::int64)(juce::pointer_sized_int) \
                                                  juce::Thread::getCurrentThreadId()) + \
                    "] " + juce::String(msg) + "\n");                          \
  } while (0)

// P1b fase 3: the host started playback. With dirty blocks the transport is
// held back: stop, render, restart. Without them this returns false and the
// play edge goes through untouched (no stop/start).
bool MainComponent::flushDirtyBlocksForHostPlay()
{
  auto *project = getProject();
  DBG_P1B("flushDirty entrance");
  if (project == nullptr || editorController == nullptr)
    return false;

  const bool hasDirtyBlocks =
      project->hasDirtyNotes() || project->hasF0DirtyRange();
  // Sin bloques en la cola ni rango F0 sucio no hay flush que hacer.
  if (deferredRenders.dirtyBlockCount() == 0 && !project->hasF0DirtyRange())
    return false;
  if (!hasDirtyBlocks || flushChainActive)
    return false;

  // Fallback, documented: the ARA transport requests are queued for the host
  // and it may run them late or ignore them (ARAInterface.h:2155-2195). If the
  // host keeps playing, what is heard until the flush ends is the audio
  // processed so far; the restart below is then a no-op. Nothing here promises
  // that the cursor stopped at zero movement.
  if (onRequestHostPlayState)
    onRequestHostPlayState(false);

  deferredRenderPending = true;
  playAfterFlush = true;
  DBG_P1B("flush launch id=" +
          juce::String::toHexString((juce::int64)(juce::pointer_sized_int)this) +
          " dirty=" + juce::String(deferredRenders.dirtyBlockCount()));
  flushDirtyBlocks();
  if (!flushChainActive)
  {
    playAfterFlush = false;
    if (onRequestHostPlayState)
      onRequestHostPlayState(true);
  }
  return true;
}

void MainComponent::rerenderEditedNotes()
{
  auto *project = getProject();
  if (!project || !editorController)
    return;

  // Marks exactly what an edit of those notes would mark, so the render
  // window and the splice points are the ones the edits themselves produce.
  if (project->markEditedNotesDirty() == 0)
    return;

  markRenderDirty();
}

void MainComponent::showRenderStatus(ParameterPanel::RenderStatus status)
{
  parameterPanel.setRenderStatus(
      status, editorController ? editorController->getSynthesisEngineType()
                               : defaultSynthesisEngineType());
}

void MainComponent::onNoteSelected(Note *note)
{
  parameterPanel.setSelectedNote(note);
}

void MainComponent::onPitchEdited()
{
  pianoRoll.repaint();
  pianoRollView.refreshOverview();
  parameterPanel.updateFromNote();
}

std::optional<NoteEngine> MainComponent::getSelectionNoteEngine() const
{
  auto *project = getProject();
  if (project == nullptr)
    return std::nullopt;
  const auto targets = pianoRoll.getInspectorTargets();
  if (targets.empty() || targets.front() == nullptr)
    return std::nullopt;
  return targets.front()->getEngine();
}

int MainComponent::applyNoteEngineToSelection(NoteEngine engine)
{
  std::vector<int> selection;
  const auto targets = pianoRoll.getInspectorTargets();
  selection.reserve(targets.size());
  auto *project = getProject();
  if (project == nullptr)
    return 0;
  auto &notes = project->getNotes();
  for (auto *note : targets)
  {
    // Identity, not pointer arithmetic: the targets must be notes of this
    // project, and anything else is simply not part of the edit.
    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
      if (&notes[static_cast<size_t>(i)] == note)
      {
        selection.push_back(i);
        break;
      }
  }
  if (selection.empty())
    return 0;

  return applyNoteEngineToIndices(selection, engine);
}

int MainComponent::applyNoteEngineToIndices(const std::vector<int> &indices,
                                            NoteEngine engine)
{
  auto *project = getProject();
  if (project == nullptr)
    return 0;
  auto &notes = project->getNotes();
  if (notes.empty() || indices.empty())
    return 0;

  std::vector<int> selection;
  selection.reserve(indices.size());
  std::unordered_set<int> seen;
  for (const int index : indices)
    if (index >= 0 && index < static_cast<int>(notes.size()) &&
        seen.insert(index).second && !notes[static_cast<size_t>(index)].isRest())
      selection.push_back(index);
  if (selection.empty())
    return 0;

  // D-185: a new gesture replaces the previous explanation.
  lastRefusedEngineRequests.clear();

  const auto &audio = project->getAudioData();
  const auto &source = audio.originalWaveform.getNumSamples() > 0
                           ? audio.originalWaveform
                           : audio.waveform;
  const auto globalEngine = editorController
                                ? editorController->getSynthesisEngineType()
                                : defaultSynthesisEngineType();

  // The rule lives in one place so the menu and the renderer cannot disagree:
  // a note only takes another engine when a usable silence separates it from
  // the previous one (D-129).
  const auto plan = engineBlocks::planEngineEdits(
      notes, selection, engine, globalEngine, source, audio.sampleRate,
      HOP_SIZE);

  if (plan.refused > 0)
    LOG("engine: refused " + juce::String(plan.refused) + " of " +
        juce::String(static_cast<int>(selection.size())) +
        " note(s): no silence between blocks");
  for (const auto &refusal : plan.refusals)
    lastRefusedEngineRequests[refusal.noteIndex] = refusal.requested;

  if (!plan.edits.empty())
  {
    std::vector<NoteEngineAction::Change> changes;
    changes.reserve(plan.edits.size());
    for (const auto &edit : plan.edits)
      changes.push_back(
          {&notes[static_cast<size_t>(edit.noteIndex)], edit.previous,
           edit.next});

    auto action = std::make_unique<NoteEngineAction>(
        std::move(changes), [this] { pianoRoll.repaint(); });
    action->redo();
    if (undoManager)
      undoManager->addAction(std::move(action));

    // The engine decides how the audio is produced, so the affected notes have
    // to be rendered again; the action left them dirty and synth-dirty.
    markRenderDirty();
    notifyProjectDataChanged();
  }

  variAudioInspector.refresh();
  return plan.refused;
}

void MainComponent::onZoomChanged(float pixelsPerSecond)
{
  if (isSyncingZoom)
    return;

  isSyncingZoom = true;

  // Update all components with zoom centered on cursor
  pianoRoll.setPixelsPerSecond(pixelsPerSecond, true);
  toolbar.setZoom(pixelsPerSecond);
  pianoRollView.repaintOverviewViewport();

  isSyncingZoom = false;
}

void MainComponent::notifyProjectDataChanged()
{
  if (onProjectDataChanged)
    onProjectDataChanged();
}

void MainComponent::applyProjectSynthesisEngine()
{
  auto *project = getProject();
  if (project == nullptr)
    return;
  const auto preference = settingsManager != nullptr
                              ? settingsManager->getSynthesisEngineType()
                              : defaultSynthesisEngineType();
  const auto engine = pitchnet::displayedSynthesisEngine(
      project->getSynthesisEngine(), preference);
  if (editorController)
    editorController->setSynthesisEngineType(engine);
  parameterPanel.setSynthesisEngine(engine);
}

void MainComponent::undo()
{
  // Restore any transient anchor preview before changing history.
  pianoRoll.cancelPitchDrawingPreview();
  // Cmd+Z during an inspector slider drag: commit the gesture first, so undo
  // takes it back instead of leaving it with pointers into a rebuilt list.
  pianoRoll.endInspectorEdit();

  if (undoManager && undoManager->canUndo())
  {
    LOG("history: undo " + undoManager->getUndoName());
    afterHistoryChange(undoManager->undo());
  }
}

void MainComponent::redo()
{
  // Restore any transient anchor preview before changing history.
  pianoRoll.cancelPitchDrawingPreview();
  pianoRoll.endInspectorEdit();

  if (undoManager && undoManager->canRedo())
  {
    LOG("history: redo " + undoManager->getRedoName());
    afterHistoryChange(undoManager->redo());
  }
}

void MainComponent::afterHistoryChange(bool requiresResynthesis)
{
  // Undo/redo can rebuild the note list (split, merge), so hover and smart
  // control pointers into it must not survive.
  pianoRoll.dropTransientNotePointers();
  parameterPanel.updateFromNote();
  pianoRoll.invalidateBasePitchCache(); // Refresh cache after note split etc.
  pianoRoll.repaint();
  pianoRollView.refreshOverview();

  if (requiresResynthesis && getProject())
  {
    // Don't mark all notes as dirty - the action marked the notes it changed
    // and set any F0 dirty range. This avoids synthesizing the entire project.
    markRenderDirty();
  }

  // Update command states (undo/redo availability changed)
  if (commandManager)
    commandManager->commandStatusChanged();
}

void MainComponent::setEditMode(EditMode mode)
{
  pianoRoll.setEditMode(mode);
  toolbar.setEditMode(mode);

  // Update command states (draw mode toggle state changed)
  if (commandManager)
    commandManager->commandStatusChanged();
}

void MainComponent::setToolGroupEnabled(bool enabled)
{
  toolGroupEnabled = enabled;
  toolbar.setToolGroupEnabled(enabled);

  if (commandManager)
    commandManager->commandStatusChanged();
}

void MainComponent::segmentIntoNotes()
{
  auto *project = getProject();
  if (!project || !editorController)
    return;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->segmentIntoNotesAsync(
      [safeThis](Project &projectRef)
      {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.setProject(&projectRef);
        safeThis->pianoRollView.setProject(&projectRef);
      },
      [safeThis]()
      {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.invalidateBasePitchCache();
        safeThis->pianoRoll.repaint();
      });
}

void MainComponent::segmentIntoNotes(Project &targetProject)
{
  if (editorController)
  {
    editorController->segmentIntoNotes(targetProject, [this]()
                                       {
      pianoRoll.invalidateBasePitchCache();
      pianoRoll.repaint(); },
                                       [this](double progress)
                                       { showAnalysisProgress(progress); });
    hideAnalysisProgress();
  }
}

void MainComponent::showAnalysisProgress(double progress)
{
  pianoRollView.dismissPitchCenterPopup();
  analysisProgressPopup.setProgress(progress);
  if (!analysisBackdrop.isVisible())
    analysisBackdrop.setVisible(true);
  analysisBackdrop.toFront(false);

  if (!analysisProgressPopup.isVisible())
    analysisProgressPopup.setVisible(true);
  analysisProgressPopup.toFront(false);
}

void MainComponent::hideAnalysisProgress()
{
  if (analysisProgressPopup.isVisible())
    analysisProgressPopup.setVisible(false);
  if (analysisBackdrop.isVisible())
    analysisBackdrop.setVisible(false);
}

void MainComponent::finishBackendRender(bool success)
{
  mcpRenderPending = false;
  setToolGroupEnabled(true);
  toolbar.hideProgress();
  showRenderStatus(success ? ParameterPanel::RenderStatus::Done
                           : ParameterPanel::RenderStatus::Failed);

  DBG_P1B("finishBackendRender success=" + juce::String(success ? 1 : 0) +
          " chain=" + juce::String(flushChainActive ? 1 : 0));
  // P1b: el flush de Play acabo; se baja la cadena antes de arrancar para que
  // el playState que genere nuestro start no reabra el flush.
  if (success && flushChainActive)
  {
    flushChainActive = false;
    play();
  }

  if (!success)
    return;

  pianoRoll.invalidateWaveformCache();
  pianoRoll.repaint();
}

void MainComponent::showSettings()
{
  if (!settingsOverlay)
  {
    // Pass AudioDeviceManager only in standalone mode
    juce::AudioDeviceManager *deviceMgr = nullptr;
    if (!isPluginMode() && editorController &&
        editorController->getAudioEngine())
      deviceMgr = &editorController->getAudioEngine()->getDeviceManager();

    settingsOverlay =
        std::make_unique<SettingsOverlay>(settingsManager.get(), deviceMgr);
    addAndMakeVisible(settingsOverlay.get());
    settingsOverlay->setVisible(false);
    settingsOverlay->onClose = [this]()
    {
      if (settingsOverlay)
        settingsOverlay->setVisible(false);
    };

    settingsOverlay->getSettingsComponent()->onSettingsChanged = [this]()
    {
      settingsManager->applySettings();
      reloadInferenceModels(true);
      // Execution provider or device may have changed; both decide what the
      // Rendering card's device row offers.
      refreshRenderDeviceOptions();
    };
    settingsOverlay->getSettingsComponent()->canChangeDevice = [this]()
    {
      return !isInferenceBusy();
    };
    settingsOverlay->getSettingsComponent()->onPitchDetectorChanged =
        [this](PitchDetectorType type)
    {
      if (editorController)
        editorController->setPitchDetectorType(type);
    };
    settingsOverlay->getSettingsComponent()->onShowSegmentsDebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowSegmentsDebug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowSegmentsDebug(show);
      pianoRollView.setShowSegmentsDebug(show);
      pianoRollView.refreshOverview();
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowGameValuesDebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowGameValuesDebug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowGameValuesDebug(show);
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowNoteFramesDebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowNoteFramesDebug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowNoteFramesDebug(show);
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowUvInterpolationDebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowUvInterpolationDebug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowUvInterpolationDebug(show);
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowActualF0DebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowActualF0Debug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowActualF0Debug(show);
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowCleanedF0DebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowCleanedF0Debug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowCleanedF0Debug(show);
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowVocoderF0DebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowVocoderF0Debug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowVocoderF0Debug(show);
      pianoRoll.repaint();
    };
  }

  settingsOverlay->setBounds(getLocalBounds());
  settingsOverlay->setVisible(true);
  settingsOverlay->toFront(true);
  settingsOverlay->grabKeyboardFocus();
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray &files)
{
  if (isPluginMode())
    return false;

  for (const auto &file : files)
  {
    if (file.endsWithIgnoreCase(".pitchnet") || file.endsWithIgnoreCase(".wav") ||
        file.endsWithIgnoreCase(".mp3") ||
        file.endsWithIgnoreCase(".flac") || file.endsWithIgnoreCase(".aiff") ||
        file.endsWithIgnoreCase(".ogg") || file.endsWithIgnoreCase(".m4a"))
      return true;
  }
  return false;
}

void MainComponent::filesDropped(const juce::StringArray &files, int /*x*/,
                                 int /*y*/)
{
  if (files.isEmpty())
    return;

  openFileFromPath(juce::File(files[0]));
}

void MainComponent::openFileFromPath(const juce::File &file)
{
  if (isPluginMode())
    return;

  if (!file.existsAsFile())
    return;

  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, file]()
                                    {
      if (safeThis != nullptr)
        safeThis->openFileFromPath(file); });
    return;
  }

  if (file.hasFileExtension("pitchnet") || file.hasFileExtension(".pitchnet"))
    openProjectFile(file);
  else
    loadAudioFile(file);
}

void MainComponent::setHostAudio(const juce::AudioBuffer<float> &buffer,
                                 double sampleRate,
                                 double timelineOffsetSeconds)
{
  if (!isPluginMode())
    return;

  if (!editorController)
    return;

  showAnalysisProgress(0.0);

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->setHostAudioAsync(
      buffer, sampleRate,
      [safeThis](double p, const juce::String &msg)
      {
        if (safeThis == nullptr)
          return;
        juce::MessageManager::callAsync([safeThis, p, msg]()
                                        {
          if (safeThis == nullptr)
            return;

          safeThis->toolbar.hideProgress();
          safeThis->showAnalysisProgress(p); });
      },
      [safeThis, timelineOffsetSeconds](const juce::AudioBuffer<float> &original)
      {
        if (safeThis == nullptr)
          return;

        if (safeThis->undoManager)
          safeThis->undoManager->clear();

        auto *project = safeThis->getProject();
        if (!project)
          return;

        project->getAudioData().timelineOffsetSeconds =
            std::max(0.0, timelineOffsetSeconds);

        safeThis->pianoRoll.setProject(project);
        safeThis->pianoRollView.setProject(project);
        safeThis->parameterPanel.setProject(project);
        safeThis->toolbar.setTotalTime(project->getAudioData().getDuration());
        safeThis->toolbar.setTransportEnabled(true);
        safeThis->applyCachedHostLoopRange();

        safeThis->fitAnalyzedPitchRangeToView(*project);

        auto *vocoder = safeThis->editorController
                            ? safeThis->editorController->getVocoder()
                            : nullptr;
        if (vocoder && !vocoder->isLoaded())
        {
          auto modelPath = PlatformPaths::getVocoderModelFile();
          if (modelPath.exists())
          {
            if (!vocoder->loadModel(modelPath))
            {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::AlertWindow::WarningIcon, TR("error.inference_failed"),
                  TR("error.vocoder_load_failed") + "\n" +
                      modelPath.getFullPathName() + "\n\n" +
                      TR("error.check_model_install"));
              safeThis->toolbar.hideProgress();
              return;
            }
          }
          else
          {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, TR("error.missing_model"),
                TR("error.vocoder_not_found") + "\n" +
                    modelPath.getFullPathName() + "\n\n" +
                    TR("error.install_models"));
            safeThis->toolbar.hideProgress();
            return;
          }
        }

        safeThis->repaint();

        safeThis->notifyProjectDataChanged();

        safeThis->hideAnalysisProgress();
        safeThis->toolbar.hideProgress();
      });
}

void MainComponent::beginLiveRecording(double sampleRate,
                                       double timelineOffsetSeconds)
{
  if (!isPluginMode())
    return;
  liveRecordingActive = true;
  pianoRoll.beginLiveRecordingWaveform(sampleRate, timelineOffsetSeconds);
}

void MainComponent::appendLiveRecordingAudio(
    const juce::AudioBuffer<float> &buffer)
{
  if (!isPluginMode())
    return;
  pianoRoll.appendLiveRecordingWaveform(buffer);
}

void MainComponent::setTimelineDisplayOffset(double seconds)
{
  // Switching active modifications changes project-time coordinates, not the
  // host transport. Preserve both the drawn cursor and any queued host update
  // in host time so the next timer tick cannot restore the old coordinates.
  const bool preserveTrackCursor = pianoRoll.isTrackViewMode();
  const double cursorHostTime =
      pianoRoll.projectToTimeline(pianoRoll.getCursorTime());
  const double pendingHostTime =
      pianoRoll.projectToTimeline(pendingCursorTime.load());

  pianoRoll.setTimelineDisplayOffset(seconds);

  if (preserveTrackCursor)
  {
    const double cursorTime = pianoRoll.timelineToProject(cursorHostTime);
    pianoRoll.setCursorTime(cursorTime);
    pendingCursorTime.store(pianoRoll.timelineToProject(pendingHostTime));
    toolbar.setCurrentTime(cursorTime);
  }

  // pianoRoll's own setter skips repainting when the offset is unchanged,
  // which a right-edge-only resize never changes (the offset is purely
  // start-derived). This is the only signal PluginProcessor sends on ANY
  // active-region property update though, so the highlighted region's
  // extent - which does change on a right-edge resize - would otherwise
  // stay stale until the region is deselected and reselected.
  pianoRoll.repaint();

  // The cached loop range is host timeline seconds, mapped to project time
  // through this same offset - a moved region invalidates that mapping until
  // it is recomputed here.
  applyCachedHostLoopRange();
  // Track mode places every other region through this offset.
  if (pianoRoll.isTrackViewMode())
    pianoRollView.refreshOverview();
}

void MainComponent::updateHostAudioTimelineOffset(double timelineOffsetSeconds)
{
  if (!isPluginMode())
    return;

  auto *project = getProject();
  if (!project)
    return;

  auto &audioData = project->getAudioData();
  if (audioData.waveform.getNumSamples() == 0 || audioData.sampleRate <= 0)
  {
    audioData.timelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
    return;
  }

  const double newOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
  const double oldOffsetSeconds = std::max(0.0, audioData.timelineOffsetSeconds);
  if (std::abs(newOffsetSeconds - oldOffsetSeconds) < 0.000001)
    return;

  const int sampleRate = audioData.sampleRate;
  const int oldOffsetSamples = std::max(
      0, static_cast<int>(std::llround(oldOffsetSeconds * sampleRate)));
  const int newOffsetSamples = std::max(
      0, static_cast<int>(std::llround(newOffsetSeconds * sampleRate)));
  const int oldOffsetFrames = std::max(
      0, static_cast<int>(std::llround(oldOffsetSeconds * sampleRate /
                                       static_cast<double>(HOP_SIZE))));
  const int newOffsetFrames = std::max(
      0, static_cast<int>(std::llround(newOffsetSeconds * sampleRate /
                                       static_cast<double>(HOP_SIZE))));
  const int frameDelta = newOffsetFrames - oldOffsetFrames;

  auto repadBuffer = [&](juce::AudioBuffer<float> &buffer)
  {
    const int oldSamples = buffer.getNumSamples();
    if (oldSamples <= 0)
      return;

    const int contentStart = std::min(oldOffsetSamples, oldSamples);
    const int contentSamples = oldSamples - contentStart;
    const int newSamples = newOffsetSamples + contentSamples;
    juce::AudioBuffer<float> shifted(buffer.getNumChannels(), newSamples);
    shifted.clear();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
      shifted.copyFrom(ch, newOffsetSamples, buffer, ch, contentStart,
                       contentSamples);
    buffer = std::move(shifted);
  };

  auto repadFloatVector = [&](std::vector<float> &values)
  {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<float> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)));
    std::copy(values.begin() + contentStart, values.end(),
              shifted.begin() + newOffsetFrames);
    values = std::move(shifted);
  };

  auto repadBoolVector = [&](std::vector<bool> &values)
  {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<bool> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)), false);
    for (int i = contentStart; i < oldSize; ++i)
      shifted[static_cast<size_t>(newOffsetFrames + i - contentStart)] =
          values[static_cast<size_t>(i)];
    values = std::move(shifted);
  };

  auto repadMel = [&](std::vector<std::vector<float>> &frames)
  {
    const int oldSize = static_cast<int>(frames.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    const size_t numMels = oldSize > 0 ? frames[static_cast<size_t>(
                                             std::min(contentStart, oldSize - 1))]
                                             .size()
                                       : 0;
    std::vector<std::vector<float>> shifted(
        static_cast<size_t>(newOffsetFrames + std::max(0, oldSize - contentStart)),
        std::vector<float>(numMels, 0.0f));
    std::move(frames.begin() + contentStart, frames.end(),
              shifted.begin() + newOffsetFrames);
    frames = std::move(shifted);
  };

  repadBuffer(audioData.waveform);
  repadBuffer(audioData.originalWaveform);
  repadMel(audioData.melSpectrogram);
  repadFloatVector(audioData.rawF0);
  repadFloatVector(audioData.cleanedF0);
  repadFloatVector(audioData.denseF0);
  repadFloatVector(audioData.f0);
  repadFloatVector(audioData.baseF0);
  repadFloatVector(audioData.basePitch);
  repadFloatVector(audioData.deltaPitch);
  repadBoolVector(audioData.voicedMask);
  repadBoolVector(audioData.vadMask);

  auto shiftFrame = [frameDelta](int frame)
  {
    return std::max(0, frame + frameDelta);
  };

  for (auto &note : project->getNotes())
  {
    note.setStartFrame(shiftFrame(note.getStartFrame()));
    note.setEndFrame(shiftFrame(note.getEndFrame()));
    note.setSrcStartFrame(shiftFrame(note.getSrcStartFrame()));
    note.setSrcEndFrame(shiftFrame(note.getSrcEndFrame()));
    note.setRenderedFrames(shiftFrame(note.getRenderedStartFrame()),
                           shiftFrame(note.getRenderedEndFrame()));
  }

  for (auto &range : audioData.segmentChunkRanges)
  {
    range.first = shiftFrame(range.first);
    range.second = shiftFrame(range.second);
  }

  for (auto &chunk : audioData.segmentDebugChunks)
  {
    chunk.startFrame = shiftFrame(chunk.startFrame);
    chunk.endFrame = shiftFrame(chunk.endFrame);
    for (auto &event : chunk.events)
    {
      event.startFrame = shiftFrame(event.startFrame);
      event.endFrame = shiftFrame(event.endFrame);
      event.attachedStartFrame = shiftFrame(event.attachedStartFrame);
    }
  }

  audioData.timelineOffsetSeconds = newOffsetSeconds;
  toolbar.setTotalTime(audioData.getDuration());
  pianoRoll.setProject(project);
  pianoRollView.setProject(project);
  parameterPanel.setProject(project);
  repaint();
  notifyProjectDataChanged();
}

void MainComponent::clearHostAudio()
{
  if (!isPluginMode())
    return;

  clearProjectForNewLoad();
  hideAnalysisProgress();
  toolbar.hideProgress();
  notifyProjectDataChanged();
}

void MainComponent::focusTimelineRange(double startSeconds, double endSeconds)
{
  const double pixelsPerSecond = pianoRoll.getPixelsPerSecond();
  const int visibleWidth = pianoRoll.getVisibleContentWidth();
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  ARA_DIAG("focusTimelineRange start=" + juce::String(startSeconds, 3) +
           " end=" + juce::String(endSeconds, 3) +
           " timelineStart=" + juce::String(pianoRoll.getTimelineStart(), 3) +
           " duration=" + juce::String(pianoRoll.getTimelineDuration(), 3) +
           " pps=" + juce::String(pixelsPerSecond, 3) +
           " width=" + juce::String(visibleWidth) +
           " scrollBefore=" + juce::String(pianoRoll.getScrollX(), 1));
#endif
  if (pixelsPerSecond <= 0.0 || visibleWidth <= 0)
  {
    // D-176: setupARAMode applies the initial host selection before the editor
    // is sized. Keep the span and retry after the layout has run.
    hasPendingTimelineFocus = true;
    pendingTimelineFocusStart = startSeconds;
    pendingTimelineFocusEnd = endSeconds;
    if (!pendingViewRetryScheduled && pendingViewRetries < 10)
    {
      pendingViewRetryScheduled = true;
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::Timer::callAfterDelay(200, [safeThis]() {
        if (safeThis == nullptr)
          return;
        safeThis->pendingViewRetryScheduled = false;
        ++safeThis->pendingViewRetries;
        if (safeThis->hasPendingPitchFit)
          if (auto *pendingProject = safeThis->getProject())
          {
            safeThis->hasPendingPitchFit = false;
            safeThis->fitAnalyzedPitchRangeToView(*pendingProject);
          }
        if (safeThis->hasPendingTimelineFocus)
        {
          const double pendingStart = safeThis->pendingTimelineFocusStart;
          const double pendingEnd = safeThis->pendingTimelineFocusEnd;
          safeThis->hasPendingTimelineFocus = false;
          safeThis->focusTimelineRange(pendingStart, pendingEnd);
        }
      });
    }
    return;
  }

  hasPendingTimelineFocus = false;
  pendingViewRetries = 0;
  const bool fromOpen = pendingOpenCursorReveal;

  const double timelineStart = pianoRoll.getTimelineStart();
  const double timelineDuration = pianoRoll.getTimelineDuration();
  const auto focus = pitchnet::computeAraViewFocus(
      startSeconds, endSeconds, timelineStart, timelineDuration,
      pixelsPerSecond, visibleWidth);

  // D-176: cover the selected region before clamping the scroll. Otherwise a
  // region beyond the current project window is unreachable until playback
  // extends the timeline.
  if (focus.timelineEnd > timelineStart + timelineDuration)
    pianoRoll.extendTimelineTo(focus.timelineEnd);

  // D-180-R: a selection focus must not hide an edit cursor that was already
  // on screen while the transport is stopped (R2).
  const double focusCursorTime = pianoRoll.getCursorTime();
  const bool focusCursorWasVisible = cursorIsVisible(focusCursorTime);

  bool focusCursorRestored = false;
  if (pitchnet::shouldApplyFocusScroll(isPlaying))
  {
    pianoRoll.setScrollX(focus.scrollX); // clamps to the timeline start/end
    if ((fromOpen && !isPlaying && focusCursorTime > 0.01) ||
        pitchnet::shouldRestoreCursorAfterFocus(
            isPlaying, focusCursorWasVisible,
            cursorIsVisible(focusCursorTime)))
    {
      pianoRoll.setScrollX(pitchnet::scrollXToRevealCursor(
          focusCursorTime, pianoRoll.getScrollX(),
          static_cast<double>(pianoRoll.getPixelsPerSecond()),
          pianoRoll.getVisibleContentWidth(), pianoRoll.getTimelineStart(),
          pianoRoll.getTimelineDuration()));
      focusCursorRestored = true;
      ++latencyTrace.focusRestoreCount;
    }
  }
#if PITCHNET_ARA_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED
  ARA_DIAG("focusTimelineRange applied timelineEnd=" +
           juce::String(focus.timelineEnd, 3) +
           " scrollAfter=" + juce::String(pianoRoll.getScrollX(), 1) +
           " cursorRestored=" + (focusCursorRestored ? "1" : "0"));
#endif
  pianoRollView.repaintOverviewViewport();

  // D-180-R: measurement only. What the selection focus actually left on screen.
  latencyTrace.lastFocusAtMs = nowMs();
  latencyTrace.lastFocusSpanStart = startSeconds;
  latencyTrace.lastFocusScrollX = pianoRoll.getScrollX();
  latencyTrace.lastFocusCursorTime = focusCursorTime;
  latencyTrace.lastFocusCursorWasVisible = focusCursorWasVisible;
  latencyTrace.lastFocusCursorVisibleAfter = cursorIsVisible(focusCursorTime);
  latencyTrace.lastFocusCursorRestored = focusCursorRestored;

  // D-180-R5: while playing, a track switch must not suppress follow; the
  // current view stays until the next position update keeps the playhead
  // visible. Stopped selections still get the 1.2 s selection priority.
  if (pitchnet::shouldApplyFocusScroll(isPlaying))
    suppressPlaybackFollowUntilMs =
        juce::Time::getMillisecondCounterHiRes() + 1200.0;
  else
    suppressPlaybackFollowUntilMs = 0.0;
}

void MainComponent::updatePlaybackPosition(double timeSeconds)
{
  if (!isPluginMode())
    return;

  // The host reports transport position on ITS timeline; everything below is
  // in project time. Convert here, at the boundary. Do not clamp before the
  // conversion - with a negative display offset a valid host position maps to
  // a valid project position that a zero clamp would destroy.
  // Track mode may show host time before the active take's own zero, i.e. a
  // negative project time; the canvas's timeline start is the true floor.
  double displayTime = std::max(pianoRoll.getTimelineStart(),
                                pianoRoll.timelineToProject(timeSeconds));

  // The host playhead can continue past the active ARA region. Retain that
  // furthest position as part of the timeline so follow-playback can scroll
  // there instead of stopping at the region's audio end.
  if (pianoRoll.extendTimelineTo(displayTime))
    pianoRollView.refreshOverview();

  // D-180: measurement only. Split a stopped seek from playback so the first
  // paint after each can be timed separately. pendingStoppedHostSeek is the
  // behaviour flag: the view follows a host seek even with the transport
  // stopped.
  if (!isPlaying)
  {
    const bool newSeek =
        latencyTrace.stoppedSeekMs == 0.0 ||
        std::abs(displayTime - latencyTrace.stoppedSeekTarget) > 0.05;
    if (newSeek)
    {
      if (latencyTrace.stoppedSeekAppliedMs != 0.0 ||
          latencyTrace.stoppedSeekMs == 0.0)
      {
        latencyTrace.stoppedSeekMs = nowMs();
        latencyTrace.stoppedSeekAppliedMs = 0.0;
        latencyTrace.stoppedSeekFirstPaintMs = 0.0;
        latencyTrace.stoppedSeekCursorVisible = false;
        latencyTrace.stoppedSeekPaintBase = pianoRoll.getPaintCount();
      }
      latencyTrace.stoppedSeekTarget = displayTime;
      pendingStoppedHostSeek = true;
    }
  }
  else if (latencyTrace.playStateMs > 0.0 &&
           latencyTrace.playCursorMs == 0.0)
  {
    latencyTrace.playCursorMs = nowMs();
    latencyTrace.playCursorPaintBase = pianoRoll.getPaintCount();
  }

  // Update cursor position using the same mechanism as AudioEngine
  pendingCursorTime.store(displayTime);
  hasPendingCursorUpdate.store(true);
}

void MainComponent::updateHostPlaybackState(bool hostIsPlaying)
{
  if (!isPluginMode())
    return;

  // D-180: measurement only. A stopped -> playing transition starts the clock
  // for the first frame with playback.
  if (!isPlaying && hostIsPlaying)
  {
    // D-180: a selection near the play start must not hide the playhead for
    // the 1.2 s suppression window. Starting the transport is an explicit
    // request to see where it is playing.
    if (pitchnet::shouldClearFollowSuppression(isPlaying, hostIsPlaying))
      suppressPlaybackFollowUntilMs = 0.0;
    latencyTrace.playStateMs = nowMs();
    latencyTrace.playFirstPaintMs = 0.0;
    latencyTrace.playPaintBase = pianoRoll.getPaintCount();
    latencyTrace.playCursorMs = 0.0;
    latencyTrace.playCursorFirstPaintMs = 0.0;
    latencyTrace.playCursorVisible = false;
  }

  isPlaying = hostIsPlaying;
  toolbar.setPlaying(hostIsPlaying);
  pianoRoll.setPlaybackActive(hostIsPlaying);
}

void MainComponent::updateHostTimelineState(double bpm, int numerator,
                                            int denominator)
{
  if (!isPluginMode())
    return;

  hasCachedHostTimelineState = true;
  cachedHostTempoBpm = bpm;
  cachedHostBeatNumerator = numerator;
  cachedHostBeatDenominator = denominator;

  // Analysis and project restoration can replace timeline settings without a
  // host tempo change. Reapply the host values whenever a project is bound.
  if (auto *project = getProject())
  {
    project->setTimelineTempoBpm(bpm);
    project->setTimelineBeatSignature(numerator, denominator);
  }
  parameterPanel.setHostTimelineState(bpm, numerator, denominator);
  pianoRoll.setTimelineTempoBpm(bpm);
  pianoRoll.setTimelineBeatSignature(numerator, denominator);
}

void MainComponent::updateHostLoopRange(double startSeconds, double endSeconds,
                                        bool enabled, bool hasRange)
{
  hasCachedHostLoopRange = true;
  cachedHostLoopStartSeconds = startSeconds;
  cachedHostLoopEndSeconds = endSeconds;
  cachedHostLoopEnabled = enabled;
  cachedHostLoopHasRange = hasRange;

  applyCachedHostLoopRange();
}

void MainComponent::applyCachedHostLoopRange()
{
  if (!hasCachedHostLoopRange)
    return;

  auto *project = getProject();
  if (!project)
    return;

  if (cachedHostLoopHasRange &&
      cachedHostLoopEndSeconds > cachedHostLoopStartSeconds)
  {
    // Cached values are host timeline seconds; the project's loop range is
    // stored in project (modification) time, same as everything else drawn.
    project->setLoopRange(
        pianoRoll.timelineToProject(cachedHostLoopStartSeconds),
        pianoRoll.timelineToProject(cachedHostLoopEndSeconds));
    project->setLoopEnabled(cachedHostLoopEnabled);
  }
  else
  {
    if (cachedHostLoopHasRange)
      project->clearLoopRange();
    else
      project->setLoopEnabled(false);
  }

  toolbar.setLoopEnabled(cachedHostLoopEnabled);
  pianoRoll.repaint();
}

bool MainComponent::hasAnalyzedProject() const
{
  if (auto *project = getProject())
  {
    auto &audioData = project->getAudioData();
    return audioData.waveform.getNumSamples() > 0 && !audioData.f0.empty();
  }
  return false;
}

void MainComponent::bindRealtimeProcessor(RealtimePitchProcessor &processor)
{
  processor.setProject(getProject());
}

juce::String MainComponent::serializeProjectJson() const
{
  if (auto *project = getProject())
  {
    auto json = ProjectSerializer::toJson(*project);
    return juce::JSON::toString(json, false);
  }
  return {};
}

bool MainComponent::restoreProjectJson(const juce::String &jsonString)
{
  if (jsonString.isEmpty())
    return false;
  if (auto *project = getProject())
  {
    auto json = juce::JSON::parse(jsonString);
    if (json.isObject())
    {
      if (!ProjectSerializer::fromJson(*project, json))
        return false;

      applyProjectSynthesisEngine();
      pianoRoll.setProject(project);
      pianoRollView.setProject(project);
      restoreProjectPresentation();
      parameterPanel.setProject(project);
      toolbar.setTotalTime(project->getAudioData().getDuration());
      toolbar.setTransportEnabled(true);
      toolbar.setLoopEnabled(project->getLoopRange().enabled);
      applyCachedHostLoopRange();
      if (hasAnalyzedProject())
        fitAnalyzedPitchRangeToView(*project);
      repaint();
      notifyProjectDataChanged();
      return true;
    }
  }
  return false;
}

bool MainComponent::restoreProjectSnapshot(const Project &snapshot)
{
  liveRecordingActive = false;
  if (auto *project = getProject())
  {
    const auto macroParameters =
        isPluginMode() ? project->getMacroParameters() : nullptr;
    *project = snapshot;
    if (macroParameters)
      project->setMacroParameters(macroParameters);
    applyProjectSynthesisEngine();
    pianoRoll.setProject(project);
    pianoRollView.setProject(project);
    restoreProjectPresentation();
    parameterPanel.setProject(project);
    toolbar.setTotalTime(project->getAudioData().getDuration());
    toolbar.setTransportEnabled(true);
    toolbar.setLoopEnabled(project->getLoopRange().enabled);
    applyCachedHostLoopRange();
    if (hasAnalyzedProject())
      fitAnalyzedPitchRangeToView(*project);
    repaint();
    notifyProjectDataChanged();
    return true;
  }
  return false;
}

void MainComponent::notifyHostStopped()
{
  if (!isPluginMode())
    return;

  if (previewRegionActive)
    return;

  updateHostPlaybackState(false);
}

double MainComponent::nowMs() const
{
  return juce::Time::getMillisecondCounterHiRes();
}

bool MainComponent::cursorIsVisible(double timeSeconds) const
{
  if (pianoRoll.getVisibleContentWidth() <= 0)
    return false;
  const double x = timeSeconds * pianoRoll.getPixelsPerSecond();
  const double left = pianoRoll.getScrollX();
  return x >= left &&
         x <= left + static_cast<double>(pianoRoll.getVisibleContentWidth());
}

void MainComponent::noteAraSelectionStarted()
{
  latencyTrace.selectionStartedMs = nowMs();
  latencyTrace.selectionBoundMs = 0.0;
  latencyTrace.selectionFirstPaintMs = 0.0;
  latencyTrace.selectionPaintBase = pianoRoll.getPaintCount();
}

void MainComponent::noteAraRegionRefreshDuration(double durationMs)
{
  latencyTrace.lastRegionRefreshMs = durationMs;
  latencyTrace.maxRegionRefreshMs =
      std::max(latencyTrace.maxRegionRefreshMs, durationMs);
}

void MainComponent::noteAraArchiveRestoreDuration(double durationMs)
{
  latencyTrace.lastArchiveRestoreMs = durationMs;
  latencyTrace.maxArchiveRestoreMs =
      std::max(latencyTrace.maxArchiveRestoreMs, durationMs);
}

void MainComponent::noteAraActivationDuration(double durationMs)
{
  latencyTrace.lastActivationMs = durationMs;
  latencyTrace.maxActivationMs =
      std::max(latencyTrace.maxActivationMs, durationMs);
}

void MainComponent::noteAraActivationBreakdown(
    double cancelMs, double copyMs, double parseMs, double installMs,
    double focusMs, double bindMs, double totalMs)
{
  latencyTrace.lastActivationCancelMs = cancelMs;
  latencyTrace.lastActivationCopyMs = copyMs;
  latencyTrace.lastActivationParseMs = parseMs;
  latencyTrace.lastActivationInstallMs = installMs;
  latencyTrace.lastActivationFocusMs = focusMs;
  latencyTrace.lastActivationBindMs = bindMs;
  latencyTrace.lastActivationTotalMs = totalMs;
}

void MainComponent::noteAraLayoutStarted()
{
  if (getProject() == nullptr)
    return;
  const double cursor = pianoRoll.getCursorTime();
  const double scroll = pianoRoll.getScrollX();
  if (scroll > 100.0 && cursorIsVisible(cursor))
  {
    latencyTrace.layoutScrollGuardActive = true;
    latencyTrace.layoutScrollGuardScroll = scroll;
    latencyTrace.layoutScrollGuardCursorVisible = true;
    latencyTrace.layoutScrollGuardUntilMs = nowMs() + 500.0;
  }
}

void MainComponent::noteAraLayoutFinished()
{
  if (!latencyTrace.layoutScrollGuardActive)
    return;
  if (pianoRoll.getScrollX() < 1.0 &&
      latencyTrace.layoutScrollGuardScroll > 100.0)
  {
    pianoRoll.setScrollX(latencyTrace.layoutScrollGuardScroll);
    ++latencyTrace.layoutScrollGuardRestoreCount;
  }
}

void MainComponent::noteAraEditorOpened(double hostCursorSeconds)
{
#if defined(PITCHNET_D180_R34_LEGACY)
  // D-180-R3/R4 measurement only: pre-fix behaviour (no open reveal).
  (void)hostCursorSeconds;
#else
  pendingOpenCursorReveal = true;
  if (pitchnet::shouldSeedCursorOnOpen(isPlaying, hostCursorSeconds > 0.01))
  {
    // The editor can open before the transport callback delivers a stopped
    // seek; seed the cursor now so the first bound paint is not at zero.
    updatePlaybackPosition(hostCursorSeconds);
    const double displayTime = pendingCursorTime.load();
    pianoRoll.setCursorTime(displayTime);
    toolbar.setCurrentTime(displayTime);
    hasPendingCursorUpdate.store(false);
  }
#endif
}

void MainComponent::updateUiLatencyPaintTrace()
{
  const auto paintCount = pianoRoll.getPaintCount();
  const double stamp = nowMs();

  if (latencyTrace.selectionBoundMs > 0.0 &&
      latencyTrace.selectionFirstPaintMs == 0.0 &&
      paintCount > latencyTrace.selectionPaintBase)
  {
    latencyTrace.selectionFirstPaintMs = stamp;
    latencyTrace.selectionScrollAtPaint = pianoRoll.getScrollX();
    latencyTrace.selectionCursorVisibleAtPaint =
        cursorIsVisible(pianoRoll.getCursorTime());
  }

  if (latencyTrace.playStateMs > 0.0 &&
      latencyTrace.playFirstPaintMs == 0.0 &&
      paintCount > latencyTrace.playPaintBase)
    latencyTrace.playFirstPaintMs = stamp;

  if (latencyTrace.playCursorMs > 0.0 &&
      latencyTrace.playCursorFirstPaintMs == 0.0 &&
      paintCount > latencyTrace.playCursorPaintBase)
  {
    latencyTrace.playCursorFirstPaintMs = stamp;
    latencyTrace.playCursorVisible =
        cursorIsVisible(pianoRoll.getCursorTime());
  }

  if (latencyTrace.stoppedSeekAppliedMs > 0.0 &&
      latencyTrace.stoppedSeekFirstPaintMs == 0.0 &&
      paintCount > latencyTrace.stoppedSeekPaintBase)
    latencyTrace.stoppedSeekFirstPaintMs = stamp;
}

void MainComponent::triggerResynthesis()
{
  if (!isPluginMode())
    return;

  // Triggered by DAW parameter automation (pitch offset, formant shift).
  // These parameters affect the complete render and do not dirty a particular
  // note, so explicitly request a full-range incremental pass.
  parameterPanel.updateGlobalSliders();
  if (auto *project = getProject())
  {
    const int totalFrames = static_cast<int>(
        project->getAudioData().melSpectrogram.size());
    if (totalFrames > 0)
      project->setF0DirtyRange(0, totalFrames);
  }
  markRenderDirty();
  notifyProjectDataChanged();
  if (onPitchEditFinished)
    onPitchEditFinished();
}

void MainComponent::setTrackViewModeAvailable(bool available) {
  toolbar.setTrackViewAvailable(available);
  // The left side panel is ARA only, like the Clip / Track toggle.
  toolbar.setLeftPanelAvailable(available);
  if (!available)
    workspace.showLeftPanel(false);
  pianoRollView.setTrackViewModeAvailable(available);
  const bool track = available && settingsManager != nullptr &&
                     settingsManager->getTrackViewMode();
  pianoRollView.setTrackViewMode(track);
  toolbar.setTrackViewMode(track);
}

void MainComponent::updateRegionPreviews(MainViewRegionPreviewList previews) {
  pianoRoll.setRegionPreviews(previews);
  pianoRollView.setRegionPreviews(std::move(previews));
}

void MainComponent::updateTrackList(
    const std::vector<MainViewTrackEntry> &tracks,
    const juce::String &activeKey) {
  trackListPanel.setTracks(tracks, activeKey);
}

void MainComponent::setHostTransportControlAvailable(bool available)
{
  hostTransportControlAvailable = available;

  // Play / stop / cycle only make sense when the host transport is reachable
  // (ARA). In non-ARA plugin mode the plugin cannot move the host playhead, so
  // those controls - and cycle-range editing on the timeline - stay inert.
  toolbar.setHostTransportAvailable(available);
  pianoRoll.setCycleEditEnabled(available);

  if (!available)
  {
    isPlaying = false;
    toolbar.setPlaying(false);
  }

  if (commandManager)
    commandManager->commandStatusChanged();
}

bool MainComponent::isARAModeActive() const
{
  // Check if we're in plugin mode and have project data from ARA
  // ARA mode is indicated by having project data but no manual capture
  // In ARA mode, audio comes from ARA document controller, not from
  // processBlock
  if (!isPluginMode())
    return false;

  // If we have project data and it wasn't captured manually, it's likely from
  // ARA This is a heuristic - in a real implementation, we'd track this
  // explicitly
  if (auto *project = getProject();
      project && project->getAudioData().waveform.getNumSamples() > 0)
  {
    // Check if we're not currently capturing (which would indicate non-ARA
    // mode) Note: This requires access to PluginProcessor, which we don't have
    // here A better approach would be to set a flag when ARA audio is received
    return true; // Assume ARA if we have project data in plugin mode
  }

  return false;
}

// ApplicationCommandTarget interface implementations
juce::ApplicationCommandTarget *MainComponent::getNextCommandTarget()
{
  return nullptr;
}

void MainComponent::getAllCommands(juce::Array<juce::CommandID> &commands)
{
  // Register all application commands that MainComponent handles
  const juce::CommandID commandArray[] = {
      // File commands
      CommandIDs::openFile,
      CommandIDs::saveProject,
      CommandIDs::saveProjectAs,
      CommandIDs::exportAudio,
      CommandIDs::exportMidi,
      CommandIDs::quit,

      // Edit commands
      CommandIDs::undo,
      CommandIDs::redo,
      CommandIDs::selectAll,

      // View commands
      CommandIDs::showSettings,
      CommandIDs::showAbout,
      CommandIDs::showDeltaPitch,
      CommandIDs::showBasePitch,
      CommandIDs::zoomFull,

      // Transport commands
      CommandIDs::playPause,
      CommandIDs::stop,
      CommandIDs::goToStart,
      CommandIDs::goToEnd,

      // Edit mode commands
      CommandIDs::activateMainTool,
      CommandIDs::activateSplitTool,
      CommandIDs::activatePitchDrawingTool,
      CommandIDs::activateTimingTool};

  commands.addArray(commandArray, sizeof(commandArray) / sizeof(commandArray[0]));
}

void MainComponent::getCommandInfo(juce::CommandID commandID,
                                   juce::ApplicationCommandInfo &result)
{
  const auto primaryModifier =
#if JUCE_MAC
      juce::ModifierKeys::commandModifier;
#else
      juce::ModifierKeys::ctrlModifier;
#endif
  auto *project = getProject();
  switch (commandID)
  {
  // File commands
  case CommandIDs::openFile:
    result.setInfo(TR("command.open_audio"), TR("command.open_audio.desp"), TR("menu.file"), 0);
    result.addDefaultKeypress('o', primaryModifier);
    break;

  case CommandIDs::saveProject:
    result.setInfo(TR("command.save_project"), TR("command.save_project.desp"), TR("menu.file"), 0);
    result.addDefaultKeypress('s', primaryModifier);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::saveProjectAs:
    result.setInfo(TR("command.save_project_as"), TR("command.save_project_as.desp"),
                   TR("menu.file"), 0);
    result.addDefaultKeypress('s', primaryModifier | juce::ModifierKeys::shiftModifier);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::exportAudio:
    result.setInfo(TR("command.export_audio"), TR("command.export_audio.desp"), TR("menu.file"), 0);
    result.addDefaultKeypress('e', primaryModifier);
    result.setActive(project != nullptr &&
                     project->getAudioData().waveform.getNumSamples() > 0);
    break;

  case CommandIDs::exportMidi:
    result.setInfo(TR("command.export_midi"), TR("command.export_midi.desp"), TR("menu.file"), 0);
    result.setActive(project != nullptr && !project->getNotes().empty());
    break;

  case CommandIDs::quit:
    result.setInfo(TR("command.quit"), TR("command.quit.desp"), TR("menu.file"), 0);
    result.addDefaultKeypress('q', primaryModifier);
    result.setActive(!isPluginMode());
    break;

  // Edit commands
  case CommandIDs::undo:
    result.setInfo(TR("command.undo"), TR("command.undo.desp"), TR("menu.edit"), 0);
    result.addDefaultKeypress('z', primaryModifier);
    result.setActive(isPluginMode() ||
                     (undoManager != nullptr && undoManager->canUndo()));
    break;

  case CommandIDs::redo:
    result.setInfo(TR("command.redo"), TR("command.redo.desp"), TR("menu.edit"), 0);
#if JUCE_MAC
    result.addDefaultKeypress('z', primaryModifier | juce::ModifierKeys::shiftModifier);
#else
    result.addDefaultKeypress('y', primaryModifier);
#endif
    result.setActive(isPluginMode() ||
                     (undoManager != nullptr && undoManager->canRedo()));
    break;

  case CommandIDs::selectAll:
    result.setInfo(TR("command.select_all"), TR("command.select_all.desp"), TR("menu.edit"), 0);
    result.addDefaultKeypress('a', primaryModifier);
    result.setActive(project != nullptr);
    break;

  // View commands
  case CommandIDs::showSettings:
    result.setInfo(TR("command.settings"), TR("command.settings.desp"), TR("menu.view"), 0);
    result.addDefaultKeypress(',', primaryModifier);
    break;

  case CommandIDs::showAbout:
    result.setInfo(TR("command.about"), TR("command.about.desp"), TR("menu.view"), 0);
    break;

  case CommandIDs::showDeltaPitch:
    result.setInfo(TR("command.show_delta_pitch"), TR("command.show_delta_pitch.desp"), TR("menu.view"), 0);
    result.addDefaultKeypress('d', primaryModifier | juce::ModifierKeys::shiftModifier);
    result.setTicked(settingsManager->getShowDeltaPitch());
    break;

  case CommandIDs::showBasePitch:
    result.setInfo(TR("command.show_base_pitch"), TR("command.show_base_pitch.desp"), TR("menu.view"), 0);
    result.addDefaultKeypress('b', primaryModifier | juce::ModifierKeys::shiftModifier);
    result.setTicked(settingsManager->getShowBasePitch());
    break;

  case CommandIDs::zoomFull:
    // Cubase's "Zoom Full" key.
    result.setInfo(TR("command.zoom_full"), TR("command.zoom_full.desp"), TR("menu.view"), 0);
    result.addDefaultKeypress('f', juce::ModifierKeys::shiftModifier);
    result.setActive(project != nullptr);
    break;

  // Transport commands
  case CommandIDs::playPause:
    result.setInfo(TR("command.play_pause"), TR("command.play_pause.desp"), TR("category.transport"), 0);
    // In plugin mode space is normally left to the host. JUCE's Windows peer
    // forwards unhandled keys to the host window, but the Linux X11 peer does
    // not, so a focused editor would swallow space. Bind it on Linux; the
    // setActive() guard below keeps it inert without ARA transport control.
   #if JUCE_LINUX || JUCE_BSD
    result.addDefaultKeypress(juce::KeyPress::spaceKey, juce::ModifierKeys::noModifiers);
   #else
    if (!isPluginMode())
      result.addDefaultKeypress(juce::KeyPress::spaceKey, juce::ModifierKeys::noModifiers);
   #endif
    // Non-ARA plugin mode has no host transport to drive.
    result.setActive(project != nullptr && hostTransportControlAvailable);
    break;

  case CommandIDs::stop:
    result.setInfo(TR("command.stop"), TR("command.stop.desp"), TR("category.transport"), 0);
    result.addDefaultKeypress(juce::KeyPress::escapeKey, juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr && isPlaying &&
                     hostTransportControlAvailable);
    break;

  case CommandIDs::goToStart:
    result.setInfo(TR("command.go_to_start"), TR("command.go_to_start.desp"), TR("category.transport"), 0);
    result.addDefaultKeypress(juce::KeyPress::homeKey, juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::goToEnd:
    result.setInfo(TR("command.go_to_end"), TR("command.go_to_end.desp"), TR("category.transport"), 0);
    result.addDefaultKeypress(juce::KeyPress::endKey, juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr);
    break;

  // Edit mode commands
  case CommandIDs::activateMainTool:
    result.setInfo(TR("command.main_tool"), TR("command.main_tool.desp"),
                   TR("category.edit_mode"), 0);
    result.addDefaultKeypress('1', juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr && toolGroupEnabled);
    result.setTicked(pianoRoll.getEditMode() == EditMode::Select);
    break;

  case CommandIDs::activateSplitTool:
    result.setInfo(TR("command.split_tool"), TR("command.split_tool.desp"),
                   TR("category.edit_mode"), 0);
    result.addDefaultKeypress('2', juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr && toolGroupEnabled);
    result.setTicked(pianoRoll.getEditMode() == EditMode::Split);
    break;

  case CommandIDs::activatePitchDrawingTool:
    result.setInfo(TR("command.pitch_drawing_tool"), TR("command.pitch_drawing_tool.desp"),
                   TR("category.edit_mode"), 0);
    result.addDefaultKeypress('3', juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr && toolGroupEnabled);
    result.setTicked(pianoRoll.getEditMode() == EditMode::PitchDrawing);
    break;

  case CommandIDs::activateTimingTool:
    result.setInfo(TR("command.timing_tool"), TR("command.timing_tool.desp"),
                   TR("category.edit_mode"), 0);
    result.addDefaultKeypress('4', juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr && toolGroupEnabled);
    result.setTicked(pianoRoll.getEditMode() == EditMode::Timing);
    break;

  default:
    break;
  }
}

bool MainComponent::perform(const ApplicationCommandTarget::InvocationInfo &info)
{
  switch (info.commandID)
  {
  // File commands
  case CommandIDs::openFile:
    this->openFile();
    return true;

  case CommandIDs::saveProject:
    this->saveProject();
    return true;

  case CommandIDs::saveProjectAs:
    this->saveProject(true);
    return true;

  case CommandIDs::exportAudio:
    exportFile();
    return true;

  case CommandIDs::exportMidi:
    exportMidiFile();
    return true;

  case CommandIDs::quit:
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
    return true;

  // Edit commands
  case CommandIDs::undo:
    if (isPluginMode())
    {
      if (undoManager != nullptr && undoManager->canUndo())
        this->undo();
      return true; // Consume in plugin mode to avoid host-level undo conflicts
    }
    this->undo();
    return true;

  case CommandIDs::redo:
    if (isPluginMode())
    {
      if (undoManager != nullptr && undoManager->canRedo())
        this->redo();
      return true; // Consume in plugin mode to avoid host-level redo conflicts
    }
    this->redo();
    return true;

  case CommandIDs::selectAll:
    if (auto *project = getProject())
    {
      project->selectAllNotes();
      pianoRoll.repaint();
    }
    return true;

  // View commands
  case CommandIDs::showSettings:
    showSettings();
    return true;

  case CommandIDs::showAbout:
    showAboutPopup();
    return true;

  case CommandIDs::showDeltaPitch:
  {
    bool newState = !settingsManager->getShowDeltaPitch();
    pianoRoll.setShowDeltaPitch(newState);
    settingsManager->setShowDeltaPitch(newState);
    settingsManager->saveConfig();
    commandManager->commandStatusChanged();
    return true;
  }

  case CommandIDs::zoomFull:
    zoomToFull();
    return true;

  case CommandIDs::showBasePitch:
  {
    bool newState = !settingsManager->getShowBasePitch();
    pianoRoll.setShowBasePitch(newState);
    settingsManager->setShowBasePitch(newState);
    settingsManager->saveConfig();
    commandManager->commandStatusChanged();
    return true;
  }

  // Transport commands
  case CommandIDs::playPause:
    if (isPlaying)
      pause();
    else
      play();
    return true;

  case CommandIDs::stop:
    this->stop();
    return true;

  case CommandIDs::goToStart:
    if (auto *project = getProject())
      seek(std::max(0.0, project->getAudioData().timelineOffsetSeconds));
    return true;

  case CommandIDs::goToEnd:
    if (auto *project = getProject())
    {
      seek(project->getAudioData().getDuration());
    }
    return true;

  // Edit mode commands
  case CommandIDs::activateMainTool:
    if (toolGroupEnabled)
      setEditMode(EditMode::Select);
    return true;

  case CommandIDs::activateSplitTool:
    if (toolGroupEnabled)
      setEditMode(EditMode::Split);
    return true;

  case CommandIDs::activatePitchDrawingTool:
    if (toolGroupEnabled)
      setEditMode(EditMode::PitchDrawing);
    return true;

  case CommandIDs::activateTimingTool:
    if (toolGroupEnabled)
      setEditMode(EditMode::Timing);
    return true;

  default:
    return false;
  }
}

void MainComponent::restoreProjectPresentation()
{
  auto *project = getProject();
  if (project == nullptr)
    return;
  const int width = project->uiWidth;
  const int height = project->uiHeight;
  pianoRollView.setOverviewVisible(project->thumbnailVisible);
  if (width <= 0 || height <= 0)
    return;
  auto size = juce::Point<int>(width, height);
  if (auto *display = WindowSizing::getDisplayForComponent(this))
    size = WindowSizing::getClampedSize(width, height, *display,
                                       WindowSizing::Constraints());
  if (auto *editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
    editor->setSize(size.x, size.y);
  else if (auto *window = findParentComponentOfClass<juce::DocumentWindow>())
    window->setContentComponentSize(size.x, size.y);
  else
    setSize(size.x, size.y);
}
