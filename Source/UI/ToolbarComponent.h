#pragma once

#include "../JuceHeader.h"
#include "../Utils/Constants.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include "Buttons.h"
#include "../Models/Project.h"
#include "GuiTypes.h"
#include "MelodyneIcons.h"
#include <optional>
#include <array>

// Forward declaration - EditMode is defined in PianoRollComponent.h
enum class EditMode;

// Tool button with hover and active states
class ToolButton : public juce::DrawableButton
{
public:
    ToolButton(const juce::String &name) : juce::DrawableButton(name, juce::DrawableButton::ImageFitted) {}

    void setActive(bool active)
    {
        isActive = active;
        repaint();
    }
    bool getActive() const { return isActive; }

    void paint(juce::Graphics &g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(2.0f);
        auto corner = 6.0f;

        if (isActive)
        {
            // Solid primary fill with subtle inner brightness
            g.setColour(APP_COLOR_PRIMARY.withAlpha(0.85f));
            g.fillRoundedRectangle(bounds, corner);

            // Soft inner highlight along top edge
            g.setColour(juce::Colour(0x18FFFFFFu));
            g.fillRoundedRectangle(bounds.removeFromTop(bounds.getHeight() * 0.45f), corner);
        }
        else if (isMouseOver())
        {
            g.setColour(APP_COLOR_SURFACE_RAISED.brighter(0.06f));
            g.fillRoundedRectangle(bounds, corner);
            g.setColour(APP_COLOR_BORDER.withAlpha(0.5f));
            g.drawRoundedRectangle(bounds, corner, 0.75f);
        }

        juce::DrawableButton::paint(g);
    }

private:
    bool isActive = false;
};

class ToolbarComponent : public juce::Component,
                         public juce::Button::Listener,
                         public juce::Slider::Listener
{
public:
    ToolbarComponent();
    ~ToolbarComponent() override;

    // Melodyne-style tool button: vector icon, label on hover, active pill.
    class ToolIconButton final : public juce::Button
    {
    public:
        ToolIconButton(EditorTool tool, GuiIcon icon);
        void paintButton(juce::Graphics& g, bool over, bool down) override;
        void setActiveState(bool shouldBeActive) { active = shouldBeActive; repaint(); }
        EditorTool getTool() const { return tool; }
        void refreshTooltip();

    private:
        EditorTool tool;
        GuiIcon icon;
        bool active = false;
    };

    void paint(juce::Graphics &g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &e) override;
    void mouseDrag(const juce::MouseEvent &e) override;
    void mouseDoubleClick(const juce::MouseEvent &e) override;

    void buttonClicked(juce::Button *button) override;

    /** Re-applies every tooltip and caption after a language change. */
    void refreshLocalisedText();
    void sliderValueChanged(juce::Slider *slider) override;

    void setPlaying(bool playing);
    void setTransportEnabled(bool enabled);
    // False when the plugin cannot drive the host transport (non-ARA plugin
    // mode): play / stop / cycle stay disabled, only capture remains usable.
    void setHostTransportAvailable(bool available);
    bool isHostTransportAvailable() const { return hostTransportAvailable; }
    void setToolGroupEnabled(bool enabled);
    void setCurrentTime(double time);
    void setTotalTime(double time);
    void setEditMode(EditMode mode);
    void setZoom(float pixelsPerSecond); // Update zoom slider without triggering callback
    void setLoopEnabled(bool enabled);
    void setAuditionEnabled(bool enabled);
    void setParametersVisible(bool visible);
    void setLeftPanelVisible(bool visible);
    // Left-panel group is ARA only; hidden (and takes no space) otherwise.
    void setLeftPanelAvailable(bool available);
    void setUndoRedoEnabled(bool undoEnabled, bool redoEnabled);
    bool isFollowPlayback() const { return followPlayback; }
    bool isLoopEnabled() const { return loopEnabled; }

    // ── Melodyne/SynthV editor chrome ──────────────────────────────
    void setUiMode(EditorUiMode mode);
    EditorUiMode getUiMode() const { return uiMode; }
    void setScaleSummary(const juce::String& root, const juce::String& mode);
    void setTempoSummary(const juce::String& text);
    void setGridSummary(const juce::String& text);
    void setEngineSummary(const juce::String& text);
    void setActiveTool(EditorTool tool);
    EditorTool getActiveTool() const { return activeTool; }

    // Plugin mode
    void setPluginMode(bool isPlugin);
    void setRecordControlVisible(bool visible);
    void setRecordArmed(bool armed);

    // Progress bar control
    void showProgress(const juce::String &message);
    void hideProgress();
    void setProgress(float progress); // 0.0 to 1.0, or -1 for indeterminate

    void setStatusMessage(const juce::String &message);

    std::function<void()> onPlay;
    std::function<void()> onPause;
    std::function<void()> onStop;
    std::function<void(float)> onZoomChanged;
    std::function<void(EditMode)> onEditModeChanged;
    std::function<void(bool)> onToggleLoop;
    std::function<void()> onUndo;
    std::function<void()> onRedo;
    std::function<void(bool)> onToggleRecord;
    std::function<void(bool)> onToggleAudition;

    std::function<void(bool)> onToggleParameters; // Called with new visibility state
    std::function<void(bool)> onToggleLeftPanel;  // Called with new left-panel visibility
    std::function<void(EditorUiMode)> onUiModeChanged;
    std::function<void(EditorTool)> onToolSelected;
    std::function<void(int)> onScaleRootSelected;
    std::function<void(int)> onScaleModeSelected;
    std::function<void(int)> onGridDivisionSelected;
    std::function<void()> onEngineClicked;
    // Clip / Track view toggle (ARA only). Hidden until made available.
    void setTrackViewAvailable(bool available);
    void setTrackViewMode(bool track);
    std::function<void(bool)> onTrackViewModeChanged; // true = Track mode
    // Note: Removed onRender - Melodyne-style: edits automatically trigger real-time processing

private:
    void updateTimeDisplay();
    juce::String formatTime(double seconds);

    ToggleButton recordButton;
    ToggleButton playButton;
    Button stopButton;
    ToggleButton loopButton;
    juce::Image logoImage;

    bool pluginMode = false;
    bool hostTransportAvailable = true;
    bool transportEnabled = false;
    // Note: Removed renderButton - Melodyne-style: automatic real-time processing

    // Edit mode buttons
    ToggleButton selectModeButton;
    ToggleButton splitModeButton;
    ToggleButton pitchDrawingModeButton;
    ToggleButton timingModeButton;
    ToggleButton followButton;
    ToggleButton auditionButton;
    Button undoButton;
    Button redoButton;
    ToggleButton parametersButton;
    ToggleButton leftPanelButton; // sits in its own group between logo and transport
    bool leftPanelAvailable = false;
    ToggleButton trackViewButton;
    bool trackViewAvailable = false;
    void refreshTrackViewTooltip();

    // Melodyne/SynthV chrome.
    std::array<std::unique_ptr<ToolIconButton>, 6> toolButtons;
    juce::TextButton basicModeButton { "Basic" };
    juce::TextButton advancedModeButton { "Advanced" };
    juce::TextButton scaleButton;
    juce::TextButton tempoButton;
    juce::TextButton gridButton;
    juce::TextButton engineButton;
    EditorUiMode uiMode = EditorUiMode::Basic;
    EditorTool activeTool = EditorTool::Main;
    juce::String scaleRootLabel { "C" };
    juce::String scaleModeLabel { "Major" };
    juce::String tempoLabelText { "120.00" };
    juce::String gridLabelText { "1/4" };
    juce::String engineLabelText { "Classic" };
    juce::Rectangle<int> modeSwitchBounds;
    juce::Rectangle<int> rightGroupBounds;
    void showScaleMenu();
    void showGridMenu();
    void updateModeButtonStates();
    void updateToolButtonStates();
    juce::Rectangle<int> toolContainerBounds;    // For drawing container background
    juce::Rectangle<int> transportCapsuleBounds; // For drawing transport capsule background
    juce::Rectangle<int> leftPanelCapsuleBounds; // For drawing the left-panel group background
    juce::Rectangle<int> timeCapsuleBounds;      // For drawing centered time capsule

    juce::Label timeLabel;

    juce::Slider zoomSlider;
    juce::Label zoomLabel;

    // Progress components
    double progressValue = 0.0; // Must be declared before progressBar
    juce::ProgressBar progressBar{progressValue};
    juce::Label progressLabel;
    bool showingProgress = false;

    bool parametersVisible = false;

    double currentTime = 0.0;
    double totalTime = 0.0;
    bool isPlaying = false;
    bool followPlayback = true;
    bool loopEnabled = false;
    int currentEditModeInt = 0;

#if JUCE_MAC
    juce::ComponentDragger dragger;
#endif

    // Declared last so it is torn down before the widgets it refreshes.
    LocalisationWatcher languageWatcher{[this]
                                        {
                                            refreshLocalisedText();
                                            resized();
                                            repaint();
                                        }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToolbarComponent)
};
