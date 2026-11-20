#include "SideRail.h"
#include "Components/AppFont.h"

SideRail::RailButton::RailButton(const juce::String& name, GuiIcon iconToDraw)
    : juce::Button(name), icon(iconToDraw)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void SideRail::RailButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto iconBox = bounds.withSizeKeepingCentre(bounds.getHeight() * 0.46f,
                                                      bounds.getHeight() * 0.46f);

    if (active)
    {
        g.setColour(APP_COLOR_RAIL_ACTIVE);
        g.fillRoundedRectangle(bounds.reduced(5.0f, 3.0f), 7.0f);
        // Melodyne/SynthV active marker: a short blue bar on the outer edge.
        g.setColour(APP_COLOR_PRIMARY);
        g.fillRoundedRectangle(bounds.getX() + 1.0f, bounds.getY() + bounds.getHeight() * 0.28f,
                               2.5f, bounds.getHeight() * 0.44f, 1.25f);
    }
    else if (over || down)
    {
        g.setColour(APP_COLOR_RAIL_ACTIVE.withAlpha(down ? 0.95f : 0.65f));
        g.fillRoundedRectangle(bounds.reduced(5.0f, 3.0f), 7.0f);
    }

    drawGuiIcon(g, icon, iconBox,
                active ? APP_COLOR_PRIMARY_GLOW
                       : (over ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_RAIL_ICON),
                1.55f);
}

SideRail::SideRail()
{
    setOpaque(false);
    closeButton = std::make_unique<RailButton>("close", GuiIcon::Close);
    closeButton->setVisible(false);
    closeButton->onClick = [this] { setOpenId({}); };
    addChildComponent(*closeButton);

    settingsButton = std::make_unique<RailButton>("settings", GuiIcon::Settings);
    settingsButton->onClick = [this] {
        if (onSettingsClicked)
            onSettingsClicked();
    };
    addAndMakeVisible(*settingsButton);
}

void SideRail::addPanel(const juce::String& id, const juce::String& title,
                        GuiIcon icon, juce::Component* content, int drawerWidth)
{
    Item item;
    item.id = id;
    item.title = title;
    item.icon = icon;
    item.content = content;
    item.width = drawerWidth;
    item.available = content != nullptr;
    items.push_back(std::move(item));

    if (content != nullptr)
    {
        content->setVisible(false);
        addChildComponent(content);
    }
    rebuildButtons();
}

SideRail::Item* SideRail::findItem(const juce::String& id)
{
    for (auto& item : items)
        if (item.id == id)
            return &item;
    return nullptr;
}

const SideRail::Item* SideRail::findItem(const juce::String& id) const
{
    for (const auto& item : items)
        if (item.id == id)
            return &item;
    return nullptr;
}

bool SideRail::isPanelAvailable(const juce::String& id) const
{
    if (const auto* item = findItem(id))
        return item->available && (!item->advancedOnly || editorMode == EditorUiMode::Advanced);
    return false;
}

void SideRail::setPanelAvailable(const juce::String& id, bool available)
{
    if (auto* item = findItem(id))
    {
        if (item->available == available)
            return;
        item->available = available;
        if (!available && openId == id)
            setOpenId({});
        rebuildButtons();
    }
}

void SideRail::setEditorMode(EditorUiMode mode)
{
    if (editorMode == mode)
        return;
    editorMode = mode;
    if (const auto* item = findItem(openId))
        if (item->advancedOnly && mode != EditorUiMode::Advanced)
            setOpenId({});
    rebuildButtons();
}

void SideRail::rebuildButtons()
{
    for (auto& button : buttons)
        removeChildComponent(button.get());
    buttons.clear();

    int index = 0;
    for (auto& item : items)
    {
        auto button = std::make_unique<RailButton>(item.id, item.icon);
        button->onClick = [this, id = item.id] { togglePanel(id); };
        button->setTooltip(TR(item.title.toRawUTF8()));
        button->setActive(item.id == openId);
        button->setVisible(item.available &&
                           (!item.advancedOnly || editorMode == EditorUiMode::Advanced));
        addAndMakeVisible(*button);
        buttons.push_back(std::move(button));
        ++index;
    }
    juce::ignoreUnused(index);
    resized();
    repaint();
}

void SideRail::refreshLocalisedText()
{
    size_t index = 0;
    for (auto& item : items)
    {
        if (index < buttons.size() && buttons[index])
            buttons[index]->setTooltip(TR(item.title.toRawUTF8()));
        ++index;
    }
    if (settingsButton)
        settingsButton->setTooltip(TR("panel.settings"));
}

void SideRail::setOpenId(const juce::String& id)
{
    if (openId == id)
        return;

    if (auto* previous = findItem(openId))
        if (previous->content != nullptr)
            previous->content->setVisible(false);

    openId = id;
    // The open drawer's icon is highlighted, so the rail shows which panel is
    // selected instead of every icon looking the same.
    for (size_t i = 0; i < buttons.size() && i < items.size(); ++i)
        if (buttons[i] != nullptr)
            buttons[i]->setActive(items[i].id == openId);
    if (auto* item = findItem(openId))
        if (item->content != nullptr)
        {
            item->content->setVisible(true);
            item->content->toFront(false);
        }
    closeButton->setVisible(openId.isNotEmpty());
    if (openId.isNotEmpty())
        closeButton->toFront(false);

    resized();
    repaint();
    if (onPanelVisibilityChanged)
        for (const auto& item : items)
            onPanelVisibilityChanged(item.id, item.id == openId);
    if (onWidthChanged)
        onWidthChanged();
}

void SideRail::showPanel(const juce::String& id, bool visible)
{
    if (!visible)
    {
        if (openId == id)
            setOpenId({});
        return;
    }
    if (!isPanelAvailable(id))
        return;
    setOpenId(id);
}

void SideRail::togglePanel(const juce::String& id)
{
    if (!isPanelAvailable(id))
        return;
    setOpenId(openId == id ? juce::String() : id);
}

bool SideRail::isPanelVisible(const juce::String& id) const
{
    return openId == id;
}

int SideRail::getPreferredWidth() const
{
    if (const auto* item = findItem(openId))
        return railWidth + item->width;
    return railWidth;
}

juce::Rectangle<int> SideRail::getDrawerBounds() const
{
    return drawerBounds;
}

void SideRail::resized()
{
    if (layingOut)
        return;
    const juce::ScopedValueSetter<bool> guard(layingOut, true);

    const int iconSize = 72;
    const int iconGap = 6;
    int visibleCount = 0;
    for (const auto& button : buttons)
        if (button->isVisible())
            ++visibleCount;

    // Centre the stack between the top edge and the settings button. The old
    // layout pinned it to the top, which left a large empty column below and
    // read as an off-centre menu.
    const int groupHeight =
        visibleCount > 0 ? visibleCount * iconSize + (visibleCount - 1) * iconGap
                         : 0;
    const int settingsTop = getHeight() - iconSize - 10;
    const int topMargin = 10;
    const int freeHeight = juce::jmax(0, settingsTop - topMargin - 8);
    int y = topMargin + juce::jmax(0, (freeHeight - groupHeight) / 2);

    for (auto& button : buttons)
    {
        if (!button->isVisible())
            continue;
        button->setBounds((railWidth - iconSize) / 2, y, iconSize, iconSize);
        y += iconSize + iconGap;
    }

    if (settingsButton != nullptr)
        settingsButton->setBounds((railWidth - iconSize) / 2, settingsTop,
                                  iconSize, iconSize);

    drawerBounds = {};
    if (const auto* item = findItem(openId))
    {
        drawerBounds = { railWidth, 0, item->width, getHeight() };
        if (item->content != nullptr)
            item->content->setBounds(drawerBounds.withTrimmedTop(headerHeight));

        closeButton->setBounds(drawerBounds.getRight() - 30, 7, 22, 22);
    }
    else
    {
        closeButton->setBounds({});
        for (auto& item : items)
            if (item.content != nullptr)
                item.content->setBounds({});
    }
}

void SideRail::paint(juce::Graphics& g)
{
    // Rail background and a hairline against the canvas.
    g.setColour(APP_COLOR_RAIL);
    g.fillRect(0, 0, railWidth, getHeight());

    g.setColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.9f));
    g.drawVerticalLine(railWidth - 1, 0.0f, static_cast<float>(getHeight()));

    if (drawerBounds.isEmpty())
        return;

    g.setColour(APP_COLOR_DRAWER);
    g.fillRect(drawerBounds);

    // Drawer header: title left, close button right.
    const auto header = drawerBounds.withHeight(headerHeight);
    g.setColour(APP_COLOR_DRAWER_HEADER);
    g.fillRect(header);
    g.setColour(APP_COLOR_BORDER_SUBTLE);
    g.drawHorizontalLine(header.getBottom() - 1,
                         static_cast<float>(drawerBounds.getX()),
                         static_cast<float>(drawerBounds.getRight()));

    if (const auto* item = findItem(openId))
    {
        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getBoldFont(12.5f));
        g.drawText(TR(item->title.toRawUTF8()),
                   header.withTrimmedLeft(14).withTrimmedRight(36),
                   juce::Justification::centredLeft, true);
    }

    // Shadow toward the centre, so the drawer reads above the canvas.
    g.setGradientFill(juce::ColourGradient(
        juce::Colours::black.withAlpha(0.22f),
        static_cast<float>(drawerBounds.getRight()), 0.0f,
        juce::Colours::transparentBlack,
        static_cast<float>(drawerBounds.getRight() + 8), 0.0f, false));
    g.fillRect(drawerBounds.getRight(), 0, 8, getHeight());
}
