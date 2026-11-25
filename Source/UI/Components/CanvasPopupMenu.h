#pragma once

#include "PitchPopupMenu.h"
#include "../GuiTypes.h"
#include "../MelodyneIcons.h"
#include "../../Utils/Localization.h"
#include "BinaryData.h"
#include <array>

namespace canvasPopupMenu
{
class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
    }

    void drawPopupMenuBackground(juce::Graphics&, int, int) override {}
    void drawResizableFrame(juce::Graphics&, int, int,
                            const juce::BorderSize<int>&) override {}
    int getPopupMenuBorderSize() override { return 0; }
    // The custom item paints both cards; the menu window itself stays unframed.
    int getMenuWindowFlags() override { return 0; }
};

inline LookAndFeel& getLookAndFeel()
{
    static LookAndFeel instance;
    return instance;
}

class ActionButton final : public juce::Button
{
public:
    explicit ActionButton(const juce::String& label) : juce::Button(label) {}

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        if (isEnabled() && (over || down || hasKeyboardFocus(false)))
        {
            g.setColour(juce::Colour(0xFF171717u));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2, 1), 5);
        }
        g.setColour(APP_COLOR_TEXT_PRIMARY.withMultipliedAlpha(isEnabled() ? 1.0f : 0.35f));
        g.setFont(AppFont::getFont(14));
        g.drawText(getButtonText(), getLocalBounds().withTrimmedLeft(12),
                   juce::Justification::centredLeft, true);
    }
};

class ToolButton final : public juce::Button
{
public:
    explicit ToolButton(GuiIcon iconToUse) : juce::Button({}), icon(iconToUse) {}

    void setActive(bool shouldBeActive)
    {
        if (active == shouldBeActive)
            return;
        active = shouldBeActive;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        g.setOpacity(isEnabled() ? 1.0f : 0.35f);
        const auto slot = getLocalBounds().toFloat().reduced(2.0f);
        if (active)
        {
            g.setColour(APP_COLOR_PRIMARY.withAlpha(0.20f));
            g.fillEllipse(slot);
            g.setColour(APP_COLOR_PRIMARY);
            g.drawEllipse(slot, 1.2f);
        }
        else if (over || down)
        {
            g.setColour(APP_COLOR_RAIL_ACTIVE);
            g.fillEllipse(slot);
        }

        drawGuiIcon(g, icon, getLocalBounds().toFloat().reduced(9.0f),
                    active ? APP_COLOR_PRIMARY_GLOW
                           : (over || down ? APP_COLOR_TEXT_PRIMARY
                                           : APP_COLOR_RAIL_ICON),
                    1.7f);
    }

private:
    GuiIcon icon;
    bool active = false;
};

class Content final : public juce::PopupMenu::CustomComponent
{
public:
    struct State
    {
        bool hasNote = false;
        bool noteMuted = false;
        bool canRestore = false;
        bool canUndo = false;
        bool canRedo = false;
        bool showAdvancedTools = true;
        int activeToolIndex = -1; // EditorTool index, -1 = no tool highlighted
    };

    Content(std::shared_ptr<int> result, State newState)
        : juce::PopupMenu::CustomComponent(false), choice(std::move(result)),
          state(newState)
    {
        const GuiIcon icons[] = {GuiIcon::MainTool, GuiIcon::PitchTool,
                                 GuiIcon::FormantTool, GuiIcon::AmplitudeTool,
                                 GuiIcon::TimingTool, GuiIcon::SeparationTool};
        for (int i = 0; i < 6; ++i)
        {
            toolButtons[static_cast<size_t>(i)] =
                std::make_unique<ToolButton>(icons[i]);
            toolButtons[static_cast<size_t>(i)]->onClick = [this, i]
            {
                *choice = 20 + i;
                triggerMenuItem();
            };
            toolButtons[static_cast<size_t>(i)]->setActive(i == state.activeToolIndex);
            // D-279: Formant (2) and Amplitude (3) have no engine yet.
            toolButtons[static_cast<size_t>(i)]->setEnabled(i != 2 && i != 3);
            toolButtons[static_cast<size_t>(i)]->setVisible(
                i < 2 || i > 3 || state.showAdvancedTools);
            addAndMakeVisible(*toolButtons[static_cast<size_t>(i)]);
        }

        // Top card, Melodyne's order: the three macros first, then the note
        // commands and the history. Leveling stays visible but inactive
        // because its engine is not implemented yet.
        static const char* keys[] = {
            "macro.correct_pitch", "macro.quantize_time", "macro.leveling",
            "menu.mute_note", "menu.unmute_note", "menu.delete_note",
            "canvas.select_all", "command.undo", "command.redo", "menu.restore"};
        for (int i = 0; i < 10; ++i)
        {
            juce::String label = TR(keys[i]);
            if (i == 9)
                label += "   >";
            menuButtons[static_cast<size_t>(i)] =
                std::make_unique<ActionButton>(label);
            menuButtons[static_cast<size_t>(i)]->onClick = [this, i]
            {
                *choice = i + 1;
                triggerMenuItem();
            };
            // Without a note the menu keeps only select-all/undo/redo; the
            // macros and the note commands need something to act on.
            menuButtons[static_cast<size_t>(i)]->setVisible(
                state.hasNote || (i >= 6 && i <= 8));
            addAndMakeVisible(*menuButtons[static_cast<size_t>(i)]);
        }

        menuButtons[0]->setEnabled(state.hasNote && !state.noteMuted);
        menuButtons[1]->setEnabled(state.hasNote && !state.noteMuted);
        menuButtons[2]->setEnabled(false);
        menuButtons[3]->setEnabled(state.hasNote && !state.noteMuted);
        menuButtons[4]->setEnabled(state.hasNote && state.noteMuted);
        menuButtons[5]->setEnabled(state.hasNote);
        menuButtons[6]->setEnabled(true);
        menuButtons[7]->setEnabled(state.canUndo);
        menuButtons[8]->setEnabled(state.canRedo);
        menuButtons[9]->setEnabled(state.hasNote && state.canRestore);

        int widest = 0;
        for (int i = 0; i < 10; ++i)
        {
            if (!menuButtons[static_cast<size_t>(i)]->isVisible())
                continue;
            widest = juce::jmax(
                widest,
                juce::GlyphArrangement::getStringWidthInt(
                    AppFont::getFont(14), menuButtons[static_cast<size_t>(i)]->getButtonText()) +
                    2 * menuPadX);
        }
        menuWidth = juce::jmax(toolRowWidth() + 2 * toolPadX, widest);
    }

    void getIdealSize(int& width, int& height) override
    {
        width = menuWidth;
        height = menuPanelHeight() + panelGap + toolPanelHeight;
    }

    void paint(juce::Graphics& g) override
    {
        // Top: options card.
        pitchPopupMenu::getLookAndFeel().drawPopupMenuBackground(
            g, getWidth(), menuPanelHeight());

        // Bottom: tool card, same width.
        const auto toolPanel = juce::Rectangle<float>(
            0.0f, static_cast<float>(menuPanelHeight() + panelGap),
            static_cast<float>(getWidth()), static_cast<float>(toolPanelHeight));
        g.setColour(juce::Colours::black.withAlpha(0.38f));
        g.fillRoundedRectangle(toolPanel.translated(0.0f, 3.0f).reduced(1.0f), 16.0f);
        g.setColour(APP_COLOR_TOOL_PILL);
        g.fillRoundedRectangle(toolPanel.reduced(1.0f), 15.0f);
        g.setColour(APP_COLOR_BORDER_HIGHLIGHT.withAlpha(0.9f));
        g.drawRoundedRectangle(toolPanel.reduced(1.0f), 15.0f, 1.0f);
    }

    void resized() override
    {
        const int width = getWidth();
        int y = menuPadY;
        const auto place = [this, width, &y](int index)
        {
            menuButtons[static_cast<size_t>(index)]->setBounds(
                menuPadX, y, width - 2 * menuPadX, rowHeight);
            y += rowHeight;
        };
        if (state.hasNote)
        {
            for (int i = 0; i < 10; ++i)
                place(i);
        }
        else
        {
            place(6);
            place(7);
            place(8);
        }

        const int toolX = juce::jmax(0, (width - toolRowWidth()) / 2);
        const int toolY = menuPanelHeight() + panelGap +
                          (toolPanelHeight - toolButtonSize) / 2;
        int visible = 0;
        for (int i = 0; i < 6; ++i)
        {
            if (!toolButtons[static_cast<size_t>(i)]->isVisible())
                continue;
            toolButtons[static_cast<size_t>(i)]->setBounds(
                toolX + visible * (toolButtonSize + toolGap),
                toolY, toolButtonSize, toolButtonSize);
            ++visible;
        }
    }

private:
    static constexpr int toolButtonSize = 36, toolGap = 2, toolPadX = 8;
    static constexpr int toolPanelHeight = 46, panelGap = 4;
    static constexpr int rowHeight = 26, menuPadX = 6, menuPadY = 4;

    int toolRowWidth() const
    {
        const int count = state.showAdvancedTools ? 6 : 4;
        return count * toolButtonSize + (count - 1) * toolGap;
    }
    int menuRowCount() const { return state.hasNote ? 10 : 3; }
    int menuPanelHeight() const
    {
        return menuPadY * 2 + menuRowCount() * rowHeight;
    }

    State state;
    int menuWidth = 0;
    std::shared_ptr<int> choice;
    std::array<std::unique_ptr<ToolButton>, 6> toolButtons;
    std::array<std::unique_ptr<ActionButton>, 10> menuButtons;
};
}
