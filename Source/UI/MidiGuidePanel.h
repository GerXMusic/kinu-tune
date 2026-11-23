#pragma once

#include "../JuceHeader.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include <functional>

/**
 * MIDI-guide drawer (gui-decision.md §7, still PENDIENTE in the spec).
 * It lists imported guide files visually; the engine does not consume them
 * yet, and the hint says so instead of pretending they are applied.
 */
class MidiGuidePanel : public juce::Component
{
public:
    MidiGuidePanel();
    ~MidiGuidePanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void refreshLocalisedText();

    std::function<void()> onImportRequested;

private:
    juce::TextButton importButton;
    juce::TextButton removeButton;
    juce::StringArray guides;
    int selected = -1;
    juce::Rectangle<int> listBounds;
    juce::Label hint;

    LocalisationWatcher languageWatcher{[this] {
        refreshLocalisedText();
        resized();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiGuidePanel)
};
