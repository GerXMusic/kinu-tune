// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Utils/HarmonyUtils.h"
#include "../Utils/Localization.h"
#include "Workspace/PanelContent.h"
#include <array>
#include <functional>

// Inspector card with one-click harmony buttons. Each button moves the
// selected notes (or every note when nothing is selected) by a scale-degree
// interval in the editor scale, which is chosen in the card above it. The
// work itself happens in PianoRollComponent::applyHarmonyInterval via
// onHarmonyRequested.
class HarmonyPanel : public juce::Component,
                     public PanelContent,
                     private juce::Timer
{
public:
    HarmonyPanel();
    ~HarmonyPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    int getPreferredHeight() const override;

    /** Re-reads the key after the host or a file load rebinds the project. */
    void refreshProject();

    // Returns the project currently on screen. Asked on every refresh rather
    // than cached, so a replaced project is never read through a stale pointer.
    std::function<Project*()> getCurrentProject;

    /** Re-applies every caption after a language change. */
    void refreshLocalisedText();

    std::function<HarmonyUtils::Result(HarmonyUtils::Interval, bool)>
        onHarmonyRequested;

private:
    struct IntervalRow
    {
        HarmonyUtils::Interval interval;
        const char* labelKey;
        juce::Label label;
        juce::TextButton upButton;
        juce::TextButton downButton;
    };

    void timerCallback() override;
    void refreshKeyState();
    void handlePress(HarmonyUtils::Interval interval, bool up);
    void showStatus(const juce::String& text, bool isWarning);

    Project* currentProject() const
    {
        return getCurrentProject ? getCurrentProject() : nullptr;
    }

    bool hadProject = false;
    int shownRoot = -2;
    ScaleMode shownMode = ScaleMode::None;

    juce::Label sectionLabel;
    juce::Label hintLabel;
    juce::Label statusLabel;
    std::array<IntervalRow, 5> rows;
    juce::Rectangle<int> cardBounds;

    LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HarmonyPanel)
};
