#include "MidiGuidePanel.h"
#include "Components/AppFont.h"

MidiGuidePanel::MidiGuidePanel()
{
    importButton.setButtonText(TR("midi.import"));
    importButton.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_RAISED);
    importButton.onClick = [this] {
        if (onImportRequested)
            return onImportRequested();
        auto chooser = std::make_shared<juce::FileChooser>(
            TR("midi.import"), juce::File(), "*.mid;*.midi");
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                 juce::FileBrowserComponent::canSelectFiles,
                             [this, chooser](const juce::FileChooser& fc) {
                                 const auto file = fc.getResult();
                                 if (file.existsAsFile())
                                 {
                                     guides.addIfNotAlreadyThere(file.getFileName());
                                     selected = guides.size() - 1;
                                     repaint();
                                 }
                             });
    };
    addAndMakeVisible(importButton);

    removeButton.setButtonText(TR("midi.remove"));
    removeButton.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_RAISED);
    removeButton.onClick = [this] {
        if (selected >= 0 && selected < guides.size())
        {
            guides.remove(selected);
            selected = juce::jmin(selected, guides.size() - 1);
            repaint();
        }
    };
    addAndMakeVisible(removeButton);

    hint.setFont(AppFont::getFont(11.0f));
    hint.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    hint.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(hint);

    refreshLocalisedText();
}

void MidiGuidePanel::refreshLocalisedText()
{
    importButton.setButtonText(TR("midi.import"));
    removeButton.setButtonText(TR("midi.remove"));
    hint.setText(TR("midi.hint"), juce::dontSendNotification);
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
        g.setFont(AppFont::getFont(11.5f));
        g.drawFittedText(TR("midi.empty"), listBounds.reduced(14),
                         juce::Justification::centred, 2);
        return;
    }

    for (int i = 0; i < guides.size(); ++i)
    {
        auto row = juce::Rectangle<int>(listBounds.getX() + 4,
                                        listBounds.getY() + 4 + i * 30,
                                        listBounds.getWidth() - 8, 28);
        if (i == selected)
        {
            g.setColour(APP_COLOR_RAIL_ACTIVE);
            g.fillRoundedRectangle(row.toFloat(), 5.0f);
        }
        g.setColour(i == selected ? APP_COLOR_PRIMARY_GLOW
                                  : APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getFont(12.0f));
        g.drawText(guides[i], row.withTrimmedLeft(10),
                   juce::Justification::centredLeft, true);
    }
}

void MidiGuidePanel::resized()
{
    auto area = getLocalBounds().reduced(12, 10);
    auto buttons = area.removeFromTop(30);
    importButton.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2 - 3));
    buttons.removeFromLeft(6);
    removeButton.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2));

    area.removeFromTop(10);
    hint.setBounds(area.removeFromBottom(56));
    listBounds = area;
}
