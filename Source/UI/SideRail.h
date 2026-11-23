#pragma once

#include "../JuceHeader.h"
#include "GuiTypes.h"
#include "MelodyneIcons.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include <functional>
#include <vector>

/**
 * SynthV-style vertical rail on the far left of the editor.
 *
 * The rail is always visible. Clicking an icon opens its drawer next to the
 * rail, pushing the canvas; clicking the active icon (or the drawer's close
 * button) closes it. Only one drawer is open at a time. The Settings icon is
 * a command, not a drawer: it forwards to the existing settings overlay.
 *
 * The panel contents are owned by MainComponent and only borrowed here.
 */
class SideRail : public juce::Component
{
public:
    static constexpr int railWidth = 60;
    static constexpr int defaultDrawerWidth = 300;
    static constexpr int headerHeight = 34;

    SideRail();
    ~SideRail() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void addPanel(const juce::String& id, const juce::String& title,
                  GuiIcon icon, juce::Component* content,
                  int drawerWidth = defaultDrawerWidth);
    /** Show one drawer, or close the open one when visible == false. */
    void showPanel(const juce::String& id, bool visible);
    void togglePanel(const juce::String& id);
    bool isPanelVisible(const juce::String& id) const;
    bool isDrawerOpen() const { return openId.isNotEmpty(); }
    juce::String getOpenPanelId() const { return openId; }
    bool isPanelAvailable(const juce::String& id) const;

    /** Tracks is ARA-only; when unavailable its icon takes no space. */
    void setPanelAvailable(const juce::String& id, bool available);
    void setEditorMode(EditorUiMode mode);
    void refreshLocalisedText();

    int getPreferredWidth() const;
    /** The drawer area only, in local coordinates. */
    juce::Rectangle<int> getDrawerBounds() const;

    std::function<void(const juce::String&, bool)> onPanelVisibilityChanged;
    std::function<void()> onSettingsClicked;
    std::function<void()> onWidthChanged;

private:
    struct Item
    {
        juce::String id;
        juce::String title;
        GuiIcon icon = GuiIcon::Notes;
        juce::Component* content = nullptr;
        int width = defaultDrawerWidth;
        bool available = true;
        bool advancedOnly = false;
    };

    class RailButton final : public juce::Button
    {
    public:
        RailButton(const juce::String& name, GuiIcon iconToDraw);
        void setIcon(GuiIcon newIcon) { icon = newIcon; repaint(); }
        void setActive(bool shouldBeActive) { active = shouldBeActive; repaint(); }
        void paintButton(juce::Graphics& g, bool over, bool down) override;

    private:
        GuiIcon icon;
        bool active = false;
    };

    void rebuildButtons();
    void layoutTooltip(RailButton& button);
    void setOpenId(const juce::String& id);
    Item* findItem(const juce::String& id);
    const Item* findItem(const juce::String& id) const;

    std::vector<Item> items;
    std::vector<std::unique_ptr<RailButton>> buttons;
    std::unique_ptr<RailButton> settingsButton;
    std::unique_ptr<RailButton> closeButton;
    juce::String openId;
    juce::Rectangle<int> drawerBounds;
    EditorUiMode editorMode = EditorUiMode::Basic;
    bool layingOut = false;

    LocalisationWatcher languageWatcher{[this] {
        refreshLocalisedText();
        resized();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SideRail)
};
