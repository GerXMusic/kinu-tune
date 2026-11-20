#pragma once

#include "../JuceHeader.h"
#include "GuiTypes.h"
#include "MelodyneIcons.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include <functional>

/**
 * The Melodyne tool pill.
 *
 * Shown on a right click over the canvas. Move while holding to hover a tool,
 * release to make it active; the three dots open the canvas menu. It is also
 * reachable from the top bar (same tools), so the editor does not depend on
 * the gesture.
 */
class ToolPill : public juce::Component
{
public:
    ToolPill();
    ~ToolPill() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    void setCurrentTool(EditorTool tool);
    EditorTool getCurrentTool() const { return currentTool; }
    void setEditorMode(EditorUiMode mode);
    void refreshLocalisedText();

    void showAt(juce::Point<int> screenPoint, juce::Rectangle<int> hostBounds);
    void hidePill();
    bool isOpen() const { return open; }
    // Right-button hold-drag: the canvas owns the mouse during the gesture,
    // so it forwards the pointer here instead of relying on pill hit events.
    void updateHoverFromScreen(juce::Point<int> screenPoint);
    void releaseAtScreen(juce::Point<int> screenPoint);

    std::function<void(EditorTool)> onToolSelected;
    std::function<void()> onMenuRequested;
    std::function<void()> onClosed;

private:
    struct Slot
    {
        EditorTool tool;
        GuiIcon icon;
        bool advancedOnly = false;
    };
    int slotAt(juce::Point<int> p) const;
    void setHoverAt(juce::Point<int> p);
    int visibleSlotCount() const;
    juce::Rectangle<int> slotBounds(int index) const;

    static constexpr int slotSize = 40;
    static constexpr int menuSlotWidth = 34;
    static constexpr int padX = 8;
    static constexpr int padY = 7;

    std::vector<Slot> slots;
    EditorTool currentTool = EditorTool::Main;
    EditorUiMode editorMode = EditorUiMode::Basic;
    int hovered = -1;
    int pressed = -1;
    bool open = false;
    juce::String hoverLabel;

    LocalisationWatcher languageWatcher{[this] {
        refreshLocalisedText();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToolPill)
};
