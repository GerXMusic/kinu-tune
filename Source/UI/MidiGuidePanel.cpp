#include "MidiGuidePanel.h"
#include "Components/AppFont.h"

MidiGuidePanel::MidiGuidePanel()
{
    importButton.setButtonText(TR("midi.import"));
    importButton.setColour(juce::TextButton::buttonColourId,
                           APP_COLOR_SURFACE_RAISED);
    importButton.onClick = [this]
    {
        if (onImportRequested)
            onImportRequested();
    };
    addAndMakeVisible(importButton);

    applyButton.setButtonText(TR("midi.apply"));
    applyButton.setColour(juce::TextButton::buttonColourId,
                          APP_COLOR_SURFACE_RAISED);
    applyButton.onClick = [this]
    {
        if (onApplyRequested)
            onApplyRequested();
    };
    addAndMakeVisible(applyButton);

    removeButton.setButtonText(TR("midi.remove"));
    removeButton.setColour(juce::TextButton::buttonColourId,
                           APP_COLOR_SURFACE_RAISED);
    removeButton.onClick = [this]
    {
        if (onRemoveRequested)
            onRemoveRequested();
    };
    addAndMakeVisible(removeButton);

    revealButton.setButtonText(TR("midi.reveal"));
    revealButton.setColour(juce::TextButton::buttonColourId,
                           APP_COLOR_SURFACE_RAISED);
    revealButton.onClick = [this]
    {
        if (onRevealRequested)
            onRevealRequested();
    };
    revealButton.setEnabled(false);
    addAndMakeVisible(revealButton);

    ghostsButton.setButtonText(TR("midi.ghosts"));
    ghostsButton.setColour(juce::ToggleButton::textColourId,
                           APP_COLOR_TEXT_PRIMARY);
    ghostsButton.setToggleState(true, juce::dontSendNotification);
    ghostsButton.onClick = [this]
    {
        if (onGhostsToggled)
            onGhostsToggled(ghostsButton.getToggleState());
    };
    addAndMakeVisible(ghostsButton);

    hint.setFont(AppFont::getFont(12.5f));
    hint.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    hint.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(hint);

    status.setFont(AppFont::getFont(12.5f));
    status.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    status.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(status);

    details.setFont(AppFont::getFont(12.0f));
    details.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    details.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(details);

    refreshLocalisedText();
}

void MidiGuidePanel::refreshLocalisedText()
{
    importButton.setButtonText(TR("midi.import"));
    applyButton.setButtonText(TR("midi.apply"));
    removeButton.setButtonText(TR("midi.remove"));
    revealButton.setButtonText(TR("midi.reveal"));
    ghostsButton.setButtonText(TR("midi.ghosts"));
    hint.setText(TR("midi.hint"), juce::dontSendNotification);
}

void MidiGuidePanel::setGuides(const juce::StringArray& labels,
                               const juce::StringArray& ids, int selectedIndex)
{
    guides = labels;
    guideIds = ids;
    selected = juce::jlimit(-1, juce::jmax(-1, guides.size() - 1),
                            selectedIndex);
    applyButton.setEnabled(selected >= 0);
    removeButton.setEnabled(selected >= 0);
    revealButton.setEnabled(selected >= 0);
    repaint();
}

void MidiGuidePanel::setStatusText(const juce::String& text)
{
    status.setText(text, juce::dontSendNotification);
}

void MidiGuidePanel::setGuideDetails(const juce::String& text)
{
    details.setText(text, juce::dontSendNotification);
}

void MidiGuidePanel::setGhostsVisible(bool visible)
{
    ghostsButton.setToggleState(visible, juce::dontSendNotification);
}

void MidiGuidePanel::setApplyEnabled(bool enabled)
{
    applyButton.setEnabled(enabled && selected >= 0);
}

void MidiGuidePanel::paint(juce::Graphics& g)
{
    g.fillAll(APP_COLOR_DRAWER);

    g.setColour(APP_COLOR_SURFACE);
    g.fillRoundedRectangle(listBounds.toFloat(), 7.0f);
    g.setColour(APP_COLOR_BORDER_SUBTLE);
    g.drawRoundedRectangle(listBounds.toFloat().reduced(0.5f), 7.0f, 1.0f);

    if (guides.isEmpty())
    {
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.setFont(AppFont::getFont(13.0f));
        g.drawFittedText(TR("midi.empty"), listBounds.reduced(14),
                         juce::Justification::centred, 2);
        return;
    }

    const int rowHeight = 30;
    for (int i = 0; i < guides.size(); ++i)
    {
        auto row = juce::Rectangle<int>(listBounds.getX() + 4,
                                        listBounds.getY() + 4 + i * rowHeight,
                                        listBounds.getWidth() - 8, rowHeight - 2);
        if (row.getBottom() > listBounds.getBottom())
            break;
        if (i == selected)
        {
            g.setColour(APP_COLOR_RAIL_ACTIVE);
            g.fillRoundedRectangle(row.toFloat(), 5.0f);
        }
        g.setColour(i == selected ? APP_COLOR_PRIMARY_GLOW
                                  : APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getFont(13.5f));
        g.drawText(guides[i], row.withTrimmedLeft(10),
                   juce::Justification::centredLeft, true);
    }
}

void MidiGuidePanel::mouseDown(const juce::MouseEvent& e)
{
    if (!listBounds.contains(e.getPosition()))
        return;

    const int rowHeight = 30;
    const int index = (e.getPosition().y - listBounds.getY() - 4) / rowHeight;
    if (index < 0 || index >= guides.size())
        return;
    if (index == selected)
        return;

    selected = index;
    applyButton.setEnabled(true);
    removeButton.setEnabled(true);
    revealButton.setEnabled(true);
    repaint();
    if (onSelectionChanged)
        onSelectionChanged(selected);
}

void MidiGuidePanel::resized()
{
    auto area = getLocalBounds().reduced(12, 10);
    auto buttons = area.removeFromTop(30);
    importButton.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2 - 3));
    buttons.removeFromLeft(6);
    removeButton.setBounds(buttons.removeFromLeft(buttons.getWidth()));

    area.removeFromTop(6);
    auto secondRow = area.removeFromTop(30);
    applyButton.setBounds(secondRow.removeFromLeft(secondRow.getWidth() / 2 - 3));
    secondRow.removeFromLeft(6);
    ghostsButton.setBounds(secondRow);

    area.removeFromTop(6);
    revealButton.setBounds(area.removeFromTop(30));

    area.removeFromTop(10);
    hint.setBounds(area.removeFromBottom(52));
    status.setBounds(area.removeFromBottom(44));
    details.setBounds(area.removeFromBottom(42));
    area.removeFromBottom(4);
    listBounds = area;
}
