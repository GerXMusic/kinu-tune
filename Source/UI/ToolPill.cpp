#include "ToolPill.h"
#include "Components/AppFont.h"

ToolPill::ToolPill()
{
    slots = {{EditorTool::Main, GuiIcon::MainTool, false},
             {EditorTool::Pitch, GuiIcon::PitchTool, false},
             {EditorTool::Formant, GuiIcon::FormantTool, true},
             {EditorTool::Amplitude, GuiIcon::AmplitudeTool, true},
             {EditorTool::Timing, GuiIcon::TimingTool, false},
             {EditorTool::Separation, GuiIcon::SeparationTool, false}};
    setVisible(false);
    setInterceptsMouseClicks(true, false);
}

int ToolPill::visibleSlotCount() const
{
    int count = 0;
    for (const auto& slot : slots)
        if (!slot.advancedOnly || editorMode == EditorUiMode::Advanced)
            ++count;
    return count;
}

juce::Rectangle<int> ToolPill::slotBounds(int index) const
{
    int visibleIndex = 0;
    for (const auto& slot : slots)
    {
        if (slot.advancedOnly && editorMode != EditorUiMode::Advanced)
            continue;
        if (visibleIndex == index)
            return { padX + visibleIndex * slotSize, padY, slotSize, slotSize };
        ++visibleIndex;
    }
    return {};
}

int ToolPill::slotAt(juce::Point<int> p) const
{
    const int count = visibleSlotCount();
    for (int i = 0; i < count; ++i)
        if (slotBounds(i).contains(p))
            return i;
    return -1;
}

void ToolPill::setCurrentTool(EditorTool tool)
{
    currentTool = tool;
    repaint();
}

void ToolPill::setEditorMode(EditorUiMode mode)
{
    if (editorMode == mode)
        return;
    editorMode = mode;
    if (isAdvancedOnlyTool(currentTool) && mode != EditorUiMode::Advanced)
        currentTool = EditorTool::Main;
    setSize(getWidth(), getHeight());
    resized();
    repaint();
}

void ToolPill::refreshLocalisedText()
{
    repaint();
}

void ToolPill::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    // Shadow.
    g.setColour(juce::Colours::black.withAlpha(0.38f));
    g.fillRoundedRectangle(bounds.translated(0.0f, 3.0f).reduced(1.0f), 16.0f);
    g.setColour(APP_COLOR_TOOL_PILL);
    g.fillRoundedRectangle(bounds.reduced(1.0f), 15.0f);
    g.setColour(APP_COLOR_BORDER_HIGHLIGHT.withAlpha(0.9f));
    g.drawRoundedRectangle(bounds.reduced(1.0f), 15.0f, 1.0f);

    const int count = visibleSlotCount();
    for (int i = 0; i < count; ++i)
    {
        const auto slot = slotBounds(i).toFloat().reduced(5.0f);
        int slotIndex = 0;
        for (const auto& candidate : slots)
        {
            if (candidate.advancedOnly && editorMode != EditorUiMode::Advanced)
                continue;
            if (slotIndex == i)
                break;
            ++slotIndex;
        }
        const auto& definition = slots[static_cast<size_t>(slotIndex)];
        const bool active = definition.tool == currentTool;
        const bool hover = i == hovered;

        if (active)
        {
            g.setColour(APP_COLOR_PRIMARY.withAlpha(0.20f));
            g.fillEllipse(slot.expanded(1.5f));
            g.setColour(APP_COLOR_PRIMARY);
            g.drawEllipse(slot.expanded(1.5f), 1.2f);
        }
        else if (hover)
        {
            g.setColour(APP_COLOR_RAIL_ACTIVE);
            g.fillEllipse(slot.expanded(1.5f));
        }

        drawGuiIcon(g, definition.icon, slot.reduced(7.0f),
                    active ? APP_COLOR_PRIMARY_GLOW
                           : (hover ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_RAIL_ICON),
                    1.7f);
    }

    // Separator and the "more" slot that opens the canvas menu.
    const float sepX = static_cast<float>(padX + count * slotSize + 2);
    g.setColour(APP_COLOR_BORDER);
    g.drawVerticalLine(static_cast<int>(sepX), 12.0f, bounds.getHeight() - 12.0f);
    const auto menuBox = juce::Rectangle<float>(
        sepX + 5.0f, static_cast<float>(padY), static_cast<float>(menuSlotWidth - 8),
        static_cast<float>(slotSize));
    drawGuiIcon(g, GuiIcon::Menu, menuBox.reduced(9.0f),
                hovered == count ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_RAIL_ICON, 1.6f);

    if (hovered >= 0 && hovered < count && hoverLabel.isNotEmpty())
    {
        const auto labelFont = AppFont::getFont(12.5f);
        const float width =
            static_cast<float>(juce::GlyphArrangement::getStringWidthInt(
                labelFont, hoverLabel)) + 14.0f;
        const auto tag = juce::Rectangle<float>(
            bounds.getCentreX() - width * 0.5f, -25.0f, width, 21.0f);
        g.setColour(juce::Colour(0xF0202429u));
        g.fillRoundedRectangle(tag, 5.0f);
        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.setFont(labelFont);
        g.drawText(hoverLabel, tag, juce::Justification::centred);
    }
}

void ToolPill::resized()
{
    const int count = visibleSlotCount();
    setSize(padX * 2 + count * slotSize + 8 + menuSlotWidth,
            slotSize + padY * 2);
}

void ToolPill::mouseMove(const juce::MouseEvent& e)
{
    setHoverAt(e.getPosition());
}

void ToolPill::setHoverAt(juce::Point<int> position)
{
    const int count = visibleSlotCount();
    const int previous = hovered;
    hovered = slotAt(position);
    if (hovered < 0)
    {
        const auto menuBox = juce::Rectangle<int>(
            padX + count * slotSize + 8, padY, menuSlotWidth, slotSize);
        if (menuBox.contains(position))
            hovered = count;
    }
    hoverLabel.clear();
    if (hovered >= 0 && hovered < count)
    {
        int visibleIndex = 0;
        for (const auto& slot : slots)
        {
            if (slot.advancedOnly && editorMode != EditorUiMode::Advanced)
                continue;
            if (visibleIndex == hovered)
            {
                hoverLabel = TR(toolLabelKey(slot.tool));
                break;
            }
            ++visibleIndex;
        }
    }
    if (hovered != previous)
        repaint();
}

void ToolPill::mouseExit(const juce::MouseEvent&)
{
    if (hovered != -1)
    {
        hovered = -1;
        hoverLabel.clear();
        repaint();
    }
}

void ToolPill::mouseDown(const juce::MouseEvent& e)
{
    pressed = slotAt(e.getPosition());
}

void ToolPill::mouseUp(const juce::MouseEvent& e)
{
    const int count = visibleSlotCount();
    const int slot = slotAt(e.getPosition());
    if (slot >= 0 && slot == pressed && slot < count)
    {
        int visibleIndex = 0;
        for (const auto& definition : slots)
        {
            if (definition.advancedOnly && editorMode != EditorUiMode::Advanced)
                continue;
            if (visibleIndex == slot)
            {
                currentTool = definition.tool;
                if (onToolSelected)
                    onToolSelected(currentTool);
                break;
            }
            ++visibleIndex;
        }
        hidePill();
        return;
    }

    const auto menuBox = juce::Rectangle<int>(
        padX + count * slotSize + 8, padY, menuSlotWidth, slotSize);
    if (menuBox.contains(e.getPosition()))
    {
        hidePill();
        if (onMenuRequested)
            onMenuRequested();
        return;
    }

    if (!getLocalBounds().contains(e.getPosition()))
        hidePill();
}

void ToolPill::updateHoverFromScreen(juce::Point<int> screenPoint)
{
    if (getParentComponent() == nullptr)
        return;
    setHoverAt(getParentComponent()->getLocalPoint(nullptr, screenPoint));
}

void ToolPill::releaseAtScreen(juce::Point<int> screenPoint)
{
    if (!open || getParentComponent() == nullptr)
        return;
    const auto local = getParentComponent()->getLocalPoint(nullptr, screenPoint);
    const int count = visibleSlotCount();
    const int slot = slotAt(local);
    if (slot >= 0 && slot < count)
    {
        int visibleIndex = 0;
        for (const auto &definition : slots)
        {
            if (definition.advancedOnly && editorMode != EditorUiMode::Advanced)
                continue;
            if (visibleIndex == slot)
            {
                currentTool = definition.tool;
                if (onToolSelected)
                    onToolSelected(currentTool);
                break;
            }
            ++visibleIndex;
        }
        hidePill();
        return;
    }

    const auto menuBox = juce::Rectangle<int>(
        padX + count * slotSize + 8, padY, menuSlotWidth, slotSize);
    if (menuBox.contains(local))
    {
        hidePill();
        if (onMenuRequested)
            onMenuRequested();
        return;
    }
    // Released outside: a plain right click (no drag) keeps the pill open so
    // it can be clicked normally; a drag off the pill cancels it.
    if (!getLocalBounds().contains(local) && hovered >= 0)
        hidePill();
}

void ToolPill::showAt(juce::Point<int> screenPoint, juce::Rectangle<int> hostBounds)
{
    // hostBounds is in the parent's coordinates; screenPoint is on screen.
    const auto local = getParentComponent() != nullptr
                           ? getParentComponent()->getLocalPoint(nullptr, screenPoint)
                           : screenPoint;
    resized();
    int x = local.x - getWidth() / 2;
    int y = local.y - getHeight() - 10;
    x = juce::jlimit(hostBounds.getX() + 6, hostBounds.getRight() - getWidth() - 6, x);
    y = juce::jlimit(hostBounds.getY() + 6, hostBounds.getBottom() - getHeight() - 6, y);
    setBounds(x, y, getWidth(), getHeight());
    open = true;
    hovered = -1;
    setVisible(true);
    toFront(false);
    repaint();
}

void ToolPill::hidePill()
{
    if (!open)
        return;
    open = false;
    pressed = -1;
    hovered = -1;
    setVisible(false);
    if (onClosed)
        onClosed();
}
