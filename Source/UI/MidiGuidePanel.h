#pragma once

#include "../JuceHeader.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include <functional>

/**
 * MIDI-guide drawer (D-311; document-level since R5b, D-356).
 *
 * Lists the guides the DOCUMENT owns (not the active item's Project) and
 * applies the selected guide to the audio notes. The panel owns no state that
 * matters: MainComponent is the source of truth and calls setGuides()/
 * setStatusText() after every change, so the drawer can never disagree with the
 * document.
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
    // R10/D-363: coverage, last update and the "changed since applied" warning.
    // It is a separate line from the transient status, so an apply message
    // does not hide it (and it does not hide the apply message).
    void setGuideDetails(const juce::String& text);
    juce::String getStatusText() const { return status.getText(); }
    void setGhostsVisible(bool visible);
    void setApplyEnabled(bool enabled);
    int getSelectedIndex() const { return selected; }

    std::function<void()> onImportRequested;
    std::function<void()> onApplyRequested;
    std::function<void()> onRemoveRequested;
    std::function<void()> onRevealRequested;
    std::function<void(bool)> onGhostsToggled;
    std::function<void(int)> onSelectionChanged;

private:
    juce::TextButton importButton;
    juce::TextButton applyButton;
    juce::TextButton removeButton;
    // V4c: only a user action; no auto-fit on open/reopen (D-378).
    juce::TextButton revealButton;
    juce::ToggleButton ghostsButton;
    juce::StringArray guides;
    juce::StringArray guideIds;
    int selected = -1;
    juce::Rectangle<int> listBounds;
    juce::Label hint;
    juce::Label status;
    juce::Label details;

    LocalisationWatcher languageWatcher{[this] {
        refreshLocalisedText();
        resized();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiGuidePanel)
};
