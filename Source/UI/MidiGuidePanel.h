#pragma once

#include "../JuceHeader.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include <functional>

/**
 * MIDI-guide drawer (D-311).
 *
 * Lists the guides materialized in the project, imports .mid files and applies
 * the selected guide to the audio notes. The panel owns no state that matters:
 * MainComponent is the source of truth and calls setGuides()/setStatusText()
 * after every change, so the drawer can never disagree with the document.
 */
class MidiGuidePanel : public juce::Component
{
public:
    MidiGuidePanel();
    ~MidiGuidePanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void refreshLocalisedText();

    // Labels in row order; ids are kept for the caller's bookkeeping.
    void setGuides(const juce::StringArray& labels, const juce::StringArray& ids,
                   int selectedIndex);
    void setStatusText(const juce::String& text);
    void setGhostsVisible(bool visible);
    void setApplyEnabled(bool enabled);
    int getSelectedIndex() const { return selected; }

    std::function<void()> onImportRequested;
    std::function<void()> onApplyRequested;
    std::function<void()> onRemoveRequested;
    std::function<void(bool)> onGhostsToggled;
    std::function<void(int)> onSelectionChanged;

private:
    juce::TextButton importButton;
    juce::TextButton applyButton;
    juce::TextButton removeButton;
    juce::ToggleButton ghostsButton;
    juce::StringArray guides;
    juce::StringArray guideIds;
    int selected = -1;
    juce::Rectangle<int> listBounds;
    juce::Label hint;
    juce::Label status;

    LocalisationWatcher languageWatcher{[this] {
        refreshLocalisedText();
        resized();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiGuidePanel)
};
