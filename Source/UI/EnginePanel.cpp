#include "EnginePanel.h"
#include "Components/AppFont.h"

namespace
{
constexpr int kPad = 12;
constexpr int kRowHeight = 30;
constexpr int kTitleHeight = 20;
constexpr int kHintHeight = 30;
constexpr int kCardPad = 10;

juce::String engineName(SynthesisEngineType engine)
{
    switch (engine)
    {
    case SynthesisEngineType::Vocoder:   return TR("inspector.engine_vocoder");
    case SynthesisEngineType::Pitchcore: return TR("inspector.engine_pitchcore");
    case SynthesisEngineType::Psola:
    default:                             return TR("inspector.engine_psola");
    }
}

juce::String noteEngineName(NoteEngine engine)
{
    switch (engine)
    {
    case NoteEngine::Vocoder:    return TR("inspector.engine_vocoder");
    case NoteEngine::Psola:      return TR("inspector.engine_psola");
    case NoteEngine::Pitchcore:  return TR("inspector.engine_pitchcore");
    case NoteEngine::Inherit:
    default:                     return TR("inspector.engine_inherit");
    }
}
} // namespace

void EnginePanel::ChoiceList::setOptions(std::vector<juce::String> newLabels)
{
    labels = std::move(newLabels);
    rowEnabled.assign(labels.size(), true);
    if (selected >= static_cast<int>(labels.size()))
        selected = -1;
    repaint();
}

void EnginePanel::ChoiceList::setSelected(int index, bool notify)
{
    if (selected == index)
        return;
    selected = index;
    repaint();
    if (notify && onChange && index >= 0)
        onChange(index);
}

void EnginePanel::ChoiceList::setRowEnabled(int index, bool enabled)
{
    if (index >= 0 && index < static_cast<int>(rowEnabled.size()) &&
        rowEnabled[static_cast<size_t>(index)] != enabled)
    {
        rowEnabled[static_cast<size_t>(index)] = enabled;
        repaint();
    }
}

int EnginePanel::ChoiceList::getPreferredHeight() const
{
    return static_cast<int>(labels.size()) * kRowHeight;
}

int EnginePanel::ChoiceList::rowAt(juce::Point<int> p) const
{
    if (labels.empty() || p.y < 0 || p.y >= static_cast<int>(labels.size()) * kRowHeight)
        return -1;
    return p.y / kRowHeight;
}

void EnginePanel::ChoiceList::mouseDown(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    if (row < 0 ||
        (row < static_cast<int>(rowEnabled.size()) && !rowEnabled[static_cast<size_t>(row)]))
        return;
    setSelected(row, true);
}

void EnginePanel::ChoiceList::mouseMove(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    if (row != hovered)
    {
        hovered = row;
        repaint();
    }
}

void EnginePanel::ChoiceList::mouseExit(const juce::MouseEvent&) 
{
    hovered = -1;
    repaint();
}

void EnginePanel::ChoiceList::paint(juce::Graphics& g)
{
    for (size_t i = 0; i < labels.size(); ++i)
    {
        const auto row = juce::Rectangle<int>(0, static_cast<int>(i) * kRowHeight,
                                              getWidth(), kRowHeight).reduced(2, 1);
        const bool isSelected = static_cast<int>(i) == selected;
        const bool enabled = i < rowEnabled.size() && rowEnabled[i];
        const bool isHovered = static_cast<int>(i) == hovered;

        if (isSelected || isHovered)
        {
            g.setColour(isSelected ? APP_COLOR_RAIL_ACTIVE.brighter(0.10f)
                                   : APP_COLOR_RAIL_ACTIVE.withAlpha(0.55f));
            g.fillRoundedRectangle(row.toFloat(), 5.0f);
        }

        const auto dot = juce::Rectangle<float>(
            static_cast<float>(row.getX() + 7), row.toFloat().getCentreY() - 5.0f, 10.0f,
            10.0f);
        g.setColour(APP_COLOR_BORDER_HIGHLIGHT);
        g.drawEllipse(dot, 1.2f);
        if (isSelected)
        {
            g.setColour(APP_COLOR_PRIMARY);
            g.fillEllipse(dot.reduced(2.2f));
        }

        g.setColour((enabled ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_TEXT_MUTED)
                        .withMultipliedAlpha(enabled ? 1.0f : 0.55f));
        g.setFont(AppFont::getFont(12.5f));
        g.drawText(labels[i], row.withTrimmedLeft(26), juce::Justification::centredLeft,
                   true);
    }
}

EnginePanel::EnginePanel(EditorUiMode mode) : editorMode(mode)
{
    auto setupLabel = [this](juce::Label& label, float size, bool bold,
                             juce::Colour colour)
    {
        label.setFont(bold ? AppFont::getBoldFont(size) : AppFont::getFont(size));
        label.setColour(juce::Label::textColourId, colour);
        addAndMakeVisible(label);
    };
    setupLabel(engineTitle, 11.5f, true, APP_COLOR_TEXT_MUTED);
    setupLabel(engineHint, 11.0f, false, APP_COLOR_TEXT_MUTED);
    setupLabel(blockTitle, 11.5f, true, APP_COLOR_TEXT_MUTED);
    setupLabel(blockHint, 11.0f, false, APP_COLOR_TEXT_MUTED);
    setupLabel(blockStatus, 11.0f, false, APP_COLOR_TEXT_MUTED);
    setupLabel(deviceTitle, 11.5f, true, APP_COLOR_TEXT_MUTED);
    deviceTitle.setVisible(false);

    addAndMakeVisible(engineChoices);
    addAndMakeVisible(blockChoices);

    engineChoices.onChange = [this](int index) {
        const SynthesisEngineType types[] = {SynthesisEngineType::Vocoder,
                                             SynthesisEngineType::Psola,
                                             SynthesisEngineType::Pitchcore};
        if (index >= 0 && index < 3 && onSynthesisEngineChanged)
            onSynthesisEngineChanged(types[index]);
    };
    blockChoices.onChange = [this](int index) {
        const NoteEngine engines[] = {NoteEngine::Inherit, NoteEngine::Vocoder,
                                      NoteEngine::Psola, NoteEngine::Pitchcore};
        if (index >= 0 && index < 4 && onNoteEngineChanged)
            onNoteEngineChanged(engines[index]);
    };

    deviceButton.setColour(juce::TextButton::buttonColourId,
                           APP_COLOR_SURFACE_RAISED);
    deviceButton.setColour(juce::TextButton::textColourOffId,
                           APP_COLOR_TEXT_PRIMARY);
    deviceButton.onClick = [this] {
        if (deviceNames.isEmpty())
            return;
        juce::PopupMenu menu;
        for (int i = 0; i < deviceNames.size(); ++i)
            menu.addItem(i + 1, deviceNames[i], true, i == deviceIndex);
        auto* panel = this;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&deviceButton),
                           [panel](int result) {
                               if (result > 0 && panel->onDeviceChanged)
                                   panel->onDeviceChanged(result - 1);
                           });
    };
    addChildComponent(deviceButton);

    setEditorMode(mode);
    refreshLocalisedText();
    startTimerHz(4);
}

void EnginePanel::setEditorMode(EditorUiMode mode)
{
    editorMode = mode;
    applyModeVisibility();
    resized();
    repaint();
}

void EnginePanel::applyModeVisibility()
{
    const bool advanced = editorMode == EditorUiMode::Advanced;
    blockTitle.setVisible(advanced);
    blockHint.setVisible(advanced);
    blockChoices.setVisible(advanced);
    blockStatus.setVisible(advanced);
    resized();
    repaint();
}

void EnginePanel::refreshLocalisedText()
{
    engineTitle.setText(TR("engine.global_title"), juce::dontSendNotification);
    engineHint.setText(TR("engine.global_hint"), juce::dontSendNotification);
    blockTitle.setText(TR("engine.block_title"), juce::dontSendNotification);
    blockHint.setText(TR("engine.block_hint"), juce::dontSendNotification);
    deviceTitle.setText(TR("engine.device"), juce::dontSendNotification);

    engineChoices.setOptions({TR("inspector.engine_vocoder"),
                              TR("inspector.engine_psola"),
                              TR("inspector.engine_pitchcore")});
    blockChoices.setOptions({TR("inspector.engine_inherit"),
                             TR("inspector.engine_vocoder"),
                             TR("inspector.engine_psola"),
                             TR("inspector.engine_pitchcore")});
    engineChoices.setRowEnabled(0, aiAvailable);
    resized();
    repaint();
}

void EnginePanel::setSynthesisEngine(SynthesisEngineType engine)
{
    const int index = engine == SynthesisEngineType::Vocoder   ? 0
                      : engine == SynthesisEngineType::Pitchcore ? 2
                                                                 : 1;
    engineChoices.setSelected(index, false);
}

void EnginePanel::setNoteEngine(std::optional<NoteEngine> engine)
{
    if (!engine)
    {
        blockChoices.setSelected(-1, false);
        blockChoices.setEnabled(false);
        return;
    }
    blockChoices.setEnabled(true);
    const int index = *engine == NoteEngine::Vocoder    ? 1
                      : *engine == NoteEngine::Psola     ? 2
                      : *engine == NoteEngine::Pitchcore ? 3
                                                         : 0;
    blockChoices.setSelected(index, false);
}

void EnginePanel::setEngineEffective(SynthesisEngineType engine,
                                     std::optional<NoteEngine> refused)
{
    juce::String text = TR("inspector.engine_effective")
                            .replace("{engine}", engineName(engine));
    if (refused)
        text += "\n" + TR("inspector.engine_refused")
                            .replace("{requested}", noteEngineName(*refused));
    blockStatus.setText(text, juce::dontSendNotification);
}

void EnginePanel::setAiResynthesisAvailable(bool available)
{
    aiAvailable = available;
    engineChoices.setRowEnabled(0, available);
}

void EnginePanel::setDeviceOptions(const juce::StringArray& names, int selectedIndex)
{
    deviceNames = names;
    deviceIndex = juce::jlimit(0, juce::jmax(0, names.size() - 1), selectedIndex);
    hasDeviceRow = names.size() > 1;
    deviceButton.setVisible(hasDeviceRow);
    deviceTitle.setVisible(hasDeviceRow);
    if (hasDeviceRow)
        deviceButton.setButtonText(names[deviceIndex]);
    resized();
    repaint();
}

void EnginePanel::setRenderStatusText(const juce::String& text)
{
    renderStatus = text;
    repaint();
}

void EnginePanel::timerCallback()
{
    if (getNoteEngine)
        setNoteEngine(getNoteEngine());
}

void EnginePanel::paint(juce::Graphics& g)
{
    g.fillAll(APP_COLOR_DRAWER);

    auto card = [&](juce::Rectangle<int> bounds) {
        g.setColour(APP_COLOR_SURFACE);
        g.fillRoundedRectangle(bounds.toFloat(), 7.0f);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 7.0f, 1.0f);
    };
    card(engineCard);
    if (editorMode == EditorUiMode::Advanced)
        card(blockCard);

    if (hasDeviceRow)
    {
        g.setColour(APP_COLOR_SURFACE);
        g.fillRoundedRectangle(deviceRow.toFloat(), 6.0f);
    }

    if (renderStatus.isNotEmpty())
    {
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.setFont(AppFont::getFont(11.0f));
        g.drawText(renderStatus,
                   getLocalBounds().withTrimmedLeft(kPad).withTrimmedRight(kPad)
                       .removeFromBottom(30),
                   juce::Justification::topLeft, true);
    }
}

void EnginePanel::resized()
{
    auto area = getLocalBounds().reduced(kPad, 8);

    // Project engine first, compactly: the drawer must read top-down as
    // "what renders" before any secondary option.
    engineTitle.setBounds(area.removeFromTop(kTitleHeight));
    engineHint.setBounds(area.removeFromTop(24));
    const int engineRows = 3;
    engineCard = area.removeFromTop(kCardPad * 2 + engineRows * kRowHeight);
    engineChoices.setBounds(engineCard.reduced(kCardPad, kCardPad));

    // Inference device directly under the project engine, not stranded at
    // the bottom of the drawer.
    if (hasDeviceRow)
    {
        area.removeFromTop(12);
        deviceTitle.setBounds(area.removeFromTop(kTitleHeight));
        deviceRow = area.removeFromTop(32);
        deviceButton.setBounds(deviceRow);
    }
    else
    {
        deviceTitle.setBounds({});
        deviceRow = {};
        deviceButton.setBounds({});
    }

    if (editorMode == EditorUiMode::Advanced)
    {
        area.removeFromTop(16);
        blockTitle.setBounds(area.removeFromTop(kTitleHeight));
        blockHint.setBounds(area.removeFromTop(kHintHeight));
        blockCard = area.removeFromTop(kCardPad * 2 + 4 * kRowHeight);
        blockChoices.setBounds(blockCard.reduced(kCardPad, kCardPad));
        area.removeFromTop(6);
        blockStatus.setBounds(area.removeFromTop(40));
    }
}
