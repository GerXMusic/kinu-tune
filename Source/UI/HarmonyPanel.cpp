// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "HarmonyPanel.h"
#include "../Utils/UI/Theme.h"
#include "Components/AppFont.h"

namespace
{
constexpr int kCardPadX = 5;
constexpr int kCardPadY = 6;
constexpr int kInnerPadX = 10;
constexpr int kInnerPadY = 8;
constexpr int kRowGap = 5;
constexpr int kSectionLabelHeight = 20;
constexpr int kButtonRowHeight = 26;
constexpr int kHintRowHeight = 18;
constexpr int kStatusRowHeight = 18;
constexpr int kLabelColumnWidth = 74;
constexpr int kNumRows = 5;

constexpr int kPreferredHeight =
    kCardPadY * 2 + kInnerPadY * 2 + kSectionLabelHeight + kRowGap +
    kNumRows * kButtonRowHeight +
    (kNumRows - 1) * kRowGap + kRowGap + kHintRowHeight + kStatusRowHeight;

struct RowSpec
{
    HarmonyUtils::Interval interval;
    const char* labelKey;
};

constexpr std::array<RowSpec, kNumRows> kRowSpecs{{
    {HarmonyUtils::Interval::Third, "harmony.third"},
    {HarmonyUtils::Interval::Fourth, "harmony.fourth"},
    {HarmonyUtils::Interval::Fifth, "harmony.fifth"},
    {HarmonyUtils::Interval::Sixth, "harmony.sixth"},
    {HarmonyUtils::Interval::Octave, "harmony.octave"},
}};

void styleButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_ALT);
    button.setColour(juce::TextButton::buttonOnColourId, APP_COLOR_SURFACE_RAISED);
    button.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);
    button.setColour(juce::TextButton::textColourOnId, APP_COLOR_TEXT_PRIMARY);
}
} // namespace

HarmonyPanel::HarmonyPanel()
{
    sectionLabel.setFont(AppFont::getBoldFont(16.0f));
    sectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9B9B9Bu));
    addAndMakeVisible(sectionLabel);

    hintLabel.setFont(AppFont::getFont(12.0f));
    hintLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    addAndMakeVisible(hintLabel);

    statusLabel.setFont(AppFont::getFont(12.0f));
    statusLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    addAndMakeVisible(statusLabel);

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto& row = rows[i];
        row.interval = kRowSpecs[i].interval;
        row.labelKey = kRowSpecs[i].labelKey;

        row.label.setFont(AppFont::getFont(14.0f));
        row.label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        addAndMakeVisible(row.label);

        styleButton(row.upButton);
        styleButton(row.downButton);
        const auto interval = row.interval;
        row.upButton.onClick = [this, interval] { handlePress(interval, true); };
        row.downButton.onClick = [this, interval] { handlePress(interval, false); };
        addAndMakeVisible(row.upButton);
        addAndMakeVisible(row.downButton);
    }

    refreshLocalisedText();
    // The key can change from the inspector, the parameter panel or the host;
    // polling keeps this card in sync without wiring every source.
    startTimerHz(4);
}

HarmonyPanel::~HarmonyPanel()
{
    stopTimer();
}

void HarmonyPanel::refreshProject()
{
    shownRoot = -2;
    refreshKeyState();
}

void HarmonyPanel::refreshLocalisedText()
{
    sectionLabel.setText(TR("harmony.title"), juce::dontSendNotification);
    hintLabel.setText(TR("harmony.hint"), juce::dontSendNotification);
    for (auto& row : rows)
    {
        row.label.setText(TR(row.labelKey), juce::dontSendNotification);
        row.upButton.setButtonText(TR("harmony.up"));
        row.downButton.setButtonText(TR("harmony.down"));
    }
    statusLabel.setText({}, juce::dontSendNotification);
    shownRoot = -2;
    refreshKeyState();
}

void HarmonyPanel::timerCallback()
{
    refreshKeyState();
}

void HarmonyPanel::refreshKeyState()
{
    auto* project = currentProject();
    const int root = project != nullptr ? project->getScaleRootNote() : -1;
    const ScaleMode mode =
        project != nullptr ? project->getPreferredScaleMode() : ScaleMode::None;
    const bool hasProject = project != nullptr;
    if (root == shownRoot && mode == shownMode && hasProject == hadProject)
        return;

    shownRoot = root;
    shownMode = mode;
    hadProject = hasProject;

    const bool degreesAvailable =
        HarmonyUtils::supportsDiatonicIntervals(mode, root);
    for (auto& row : rows)
    {
        const bool enabled = project != nullptr &&
                             (degreesAvailable ||
                              row.interval == HarmonyUtils::Interval::Octave);
        row.upButton.setEnabled(enabled);
        row.downButton.setEnabled(enabled);
    }

    if (project != nullptr && !degreesAvailable)
        showStatus(TR("harmony.scale_unsupported"), true);
    else
        showStatus({}, false);
}

void HarmonyPanel::handlePress(HarmonyUtils::Interval interval, bool up)
{
    if (!onHarmonyRequested)
        return;

    switch (onHarmonyRequested(interval, up))
    {
    case HarmonyUtils::Result::Applied:
        showStatus({}, false);
        break;
    case HarmonyUtils::Result::NoNotes:
        showStatus(TR("harmony.no_notes"), true);
        break;
    case HarmonyUtils::Result::ScaleUnsupported:
        showStatus(TR("harmony.scale_unsupported"), true);
        break;
    case HarmonyUtils::Result::PartlyOutOfRange:
        showStatus(TR("harmony.partly_out_of_range"), true);
        break;
    case HarmonyUtils::Result::OutOfRange:
        showStatus(TR("harmony.out_of_range"), true);
        break;
    }
}

void HarmonyPanel::showStatus(const juce::String& text, bool isWarning)
{
    statusLabel.setColour(juce::Label::textColourId,
                          isWarning ? APP_COLOR_ALERT_WARNING : APP_COLOR_TEXT_MUTED);
    statusLabel.setText(text, juce::dontSendNotification);
}

void HarmonyPanel::paint(juce::Graphics& g)
{
    if (cardBounds.isEmpty())
        return;

    constexpr float radius = 8.0f;
    auto rect = cardBounds.toFloat();
    g.setColour(juce::Colour(0xFF171717u));
    g.fillRoundedRectangle(rect, radius);
    g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
    g.drawRoundedRectangle(rect.reduced(0.5f), radius, 0.75f);
}

void HarmonyPanel::resized()
{
    auto card = getLocalBounds().reduced(kCardPadX, kCardPadY);
    card.setHeight(juce::jmin(card.getHeight(),
                              kPreferredHeight - kCardPadY * 2));
    cardBounds = card;

    auto bounds = card.reduced(kInnerPadX, kInnerPadY);
    sectionLabel.setBounds(bounds.removeFromTop(kSectionLabelHeight));
    bounds.removeFromTop(kRowGap);

    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (i > 0)
            bounds.removeFromTop(kRowGap);
        auto rowArea = bounds.removeFromTop(kButtonRowHeight);
        auto& row = rows[i];
        row.label.setBounds(rowArea.removeFromLeft(kLabelColumnWidth));
        const int buttonWidth = (rowArea.getWidth() - kRowGap) / 2;
        row.upButton.setBounds(rowArea.removeFromLeft(buttonWidth));
        rowArea.removeFromLeft(kRowGap);
        row.downButton.setBounds(rowArea);
    }

    bounds.removeFromTop(kRowGap);
    hintLabel.setBounds(bounds.removeFromTop(kHintRowHeight));
    statusLabel.setBounds(bounds.removeFromTop(kStatusRowHeight));
}

int HarmonyPanel::getPreferredHeight() const
{
    return kPreferredHeight;
}
