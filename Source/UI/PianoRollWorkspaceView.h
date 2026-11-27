#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Utils/Localization.h"
#include "Buttons.h"
#include "PianoRollComponent.h"
#include "PianoRoll/OverviewPanel.h"
#include "Workspace/RoundedCard.h"
#include "ToolPill.h"
#include "GuiTypes.h"
#include <functional>

class PianoRollWorkspaceView : public juce::Component, private juce::Timer {
public:
  explicit PianoRollWorkspaceView(PianoRollComponent &pianoRoll);
  ~PianoRollWorkspaceView() override;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void timerCallback() override;

  void setProject(Project *project);
  void dismissPitchCenterPopup();
  void showPitchCenterPopup();
  std::function<std::vector<TrackPitchMacroTarget>()> getTrackPitchMacroTargets;
  void refreshOverview();
  void repaintOverviewViewport();
  void setOverviewVisible(bool visible);
  /** The overview strip is view state, not item state (N12/D-354). */
  bool isOverviewVisible() const { return overviewVisible; }
  std::function<void(bool)> onOverviewVisibilityChanged;
  void setRegionPreviews(MainViewRegionPreviewList previews);
  // N10 (D-350): the overview draws the inactive items from the document
  // index (the real Notes), so it is published here too.
  void setDocumentNoteIndex(DocumentNoteIndexPtr index);
  // Clip / Track mode (ARA only). The toggle itself lives in the toolbar.
  void setTrackViewModeAvailable(bool available);
  void setTrackViewMode(bool track);
  bool isTrackViewMode() const { return pianoRoll.isTrackViewMode(); }
  void setShowSegmentsDebug(bool show);
  PianoRollComponent &getPianoRoll() { return pianoRoll; }
  std::function<void()> onAutoZoomRequested;

  // Melodyne tool pill: shown on a right click over the canvas.
  void showToolPillAt(juce::Point<int> screenPoint);
  void hideToolPill();
  void setCurrentTool(EditorTool tool);
  void setEditorMode(EditorUiMode mode);
  std::function<void(EditorTool)> onToolSelected;

private:
  class FloatingZoomSliderLookAndFeel final : public juce::LookAndFeel_V4
  {
  public:
    void drawLinearSlider(juce::Graphics &g,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          const juce::Slider::SliderStyle style,
                          juce::Slider &slider) override;
  };

  class FloatingControlBackground final : public juce::Component
  {
  public:
    void paint(juce::Graphics &g) override;
  };

  void updateOverviewVisibility();
  void updateOverviewAnimation();
  void applyEditorTool(EditorTool tool);
  PianoRollComponent &pianoRoll;
  OverviewPanel overviewPanel;
  ToolPill toolPill;
  EditorUiMode editorMode = EditorUiMode::Basic;

  RoundedCard pianoCard;
  RoundedCard overviewCard;

  Button autoZoomButton;
  ToggleButton overviewToggleButton;
  bool trackViewModeAvailable = false;
  bool overviewVisible = false;
  bool pitchCenterSnapToScale = false;

  FloatingZoomSliderLookAndFeel floatingZoomSliderLookAndFeel;
  FloatingControlBackground zoomXBackground;
  FloatingControlBackground zoomYBackground;
  juce::Slider zoomXSlider;
  juce::Slider zoomYSlider;
  juce::Rectangle<float> zoomXBg;
  juce::Rectangle<float> zoomYBg;
  juce::Rectangle<float> toggleBg;
  double lastOverviewCursorTime = -1.0;
  float overviewAnimationProgress = 0.0f;
  float overviewAnimationStartProgress = 0.0f;
  juce::uint32 overviewAnimationStartMs = 0;
  bool overviewAnimationActive = false;

  static constexpr int overviewHeight = 84;
  static constexpr int overviewAnimationMs = 220;
  static constexpr int thumbnailOuterHorizontalPadding = 7;
  static constexpr int thumbnailOuterBottomPadding = 6;
  static constexpr int cardGap = 6;
  static constexpr int thumbnailButtonSize = 21;
  static constexpr int autoZoomButtonSize = 23;
  static constexpr int floatingControlInset = 15;
  static constexpr int floatingSliderThickness = 16;
  static constexpr int zoomSliderWidth = floatingSliderThickness;
  static constexpr int zoomSliderHeight = 88;
  static constexpr int zoomSliderLength = 110;
  static constexpr int overlayControlGap = 12;
  static constexpr int overlayVerticalGap = overlayControlGap - 4;

  void refreshLocalisedText();

  LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollWorkspaceView)
};
