// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
// The scale name tables are taken from PitchNet's ScaleSelectionControl.cpp,
// Copyright (c) 2026 Session Loops and contributors.

#include "VariAudioInspector.h"
#include "../Utils/UI/Theme.h"
#include "Components/AppFont.h"
#include <array>

namespace
{
constexpr int kPadX = 5;
constexpr int kPadY = 6;
constexpr int kInnerPadX = 10;
constexpr int kInnerPadY = 8;
constexpr int kRowGap = 5;
constexpr int kHeaderHeight = 24;
constexpr int kMenuRowHeight = 28;
constexpr int kToggleHeight = 24;
constexpr int kTargetLabelHeight = 20;
constexpr int kEngineStatusHeight = 34;
constexpr int kValueRowHeight = 38;
constexpr int kNumRows = 6;
constexpr int kPlaceholderRowHeight = 34;
constexpr int kPlaceholderBoxHeight = 2 * kPlaceholderRowHeight + 4;
constexpr int kActionRowHeight = 26;
constexpr int kMacroRowHeight = 26;
constexpr int kMacrosCardHeight = 3 * kMacroRowHeight + 2 * 4;

constexpr int kRowsBoxHeight = kNumRows * kValueRowHeight + 4;
constexpr int kVariAudioCardHeight =
    kInnerPadY * 2 + kHeaderHeight + kRowGap + kMenuRowHeight * 3 + kRowGap +
    kEngineStatusHeight + kRowGap + kTargetLabelHeight + kActionRowHeight +
    kRowGap + kRowsBoxHeight + kRowGap + kPlaceholderBoxHeight + kRowGap +
    kMacrosCardHeight;
constexpr int kRootMenuWidth = 50;
constexpr int kMenuGap = 4;
constexpr int kScrollBarThickness = 8;

constexpr int kEditorScaleCardHeight = kInnerPadY * 2 + kHeaderHeight + kRowGap +
                                       kToggleHeight + kRowGap + kMenuRowHeight +
                                       kRowGap + kToggleHeight;

constexpr std::array<const char*, 12> kRootLabels{
    {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}};

struct ScaleModeOption
{
    ScaleMode mode;
    const char* labelKey;
};

constexpr std::array<ScaleModeOption, 14> kModeOptions{{
    {ScaleMode::Major, "scale.major"},
    {ScaleMode::Minor, "scale.minor"},
    {ScaleMode::Blues, "scale.blues"},
    {ScaleMode::Dorian, "scale.dorian"},
    {ScaleMode::HarmonicMinor, "scale.harmonic_minor"},
    {ScaleMode::Locrian, "scale.locrian"},
    {ScaleMode::Lydian, "scale.lydian"},
    {ScaleMode::MajorPentatonic, "scale.major_pentatonic"},
    {ScaleMode::MelodicMinor, "scale.melodic_minor"},
    {ScaleMode::MinorPentatonic, "scale.minor_pentatonic"},
    {ScaleMode::Mixolydian, "scale.mixolydian"},
    {ScaleMode::Phrygian, "scale.phrygian"},
    {ScaleMode::PhrygianDominant, "scale.phrygian_dominant"},
    {ScaleMode::WholeTone, "scale.whole_tone"},
}};

// Menu item id for a scale mode; 0 (nothing selected) for none/chromatic.
int scaleModeId(ScaleMode mode)
{
    for (size_t i = 0; i < kModeOptions.size(); ++i)
        if (kModeOptions[i].mode == mode)
            return static_cast<int>(i) + 1;
    return 0;
}

// Per-note engine menu: 1 Inherit, 2 Vocoder, 3 PSOLA, 4 Pitchcore.
int engineMenuId(NoteEngine engine)
{
    switch (engine)
    {
    case NoteEngine::Vocoder:   return 2;
    case NoteEngine::Psola:     return 3;
    case NoteEngine::Pitchcore: return 4;
    case NoteEngine::Inherit:
    default:                    return 1;
    }
}

NoteEngine engineFromMenuId(int id)
{
    switch (id)
    {
    case 2:  return NoteEngine::Vocoder;
    case 3:  return NoteEngine::Psola;
    case 4:  return NoteEngine::Pitchcore;
    default: return NoteEngine::Inherit;
    }
}

const auto kRowsBoxColour = juce::Colour(0xFF1D1F21u);
const auto kRowsBoxBorder = juce::Colour(0xFF3A3D40u);
const auto kBarTrack = juce::Colour(0xFF34373Au);
const auto kBarFill = juce::Colour(0xFFB9BDC2u);

void styleCombo(juce::ComboBox& box)
{
    box.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF101112u));
    box.setColour(juce::ComboBox::textColourId, APP_COLOR_TEXT_PRIMARY);
    box.setColour(juce::ComboBox::outlineColourId, kRowsBoxBorder);
    box.setColour(juce::ComboBox::arrowColourId, APP_COLOR_TEXT_PRIMARY);
}
} // namespace

// Inspector menus: 14 px text, and a name too long for the menu (Japanese
// scale names in the narrow column) is drawn smaller instead of cut off.
class VariAudioInspector::ComboLookAndFeel : public juce::LookAndFeel_V4
{
public:
    juce::Font getComboBoxFont(juce::ComboBox&) override { return AppFont::getFont(15.0f); }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        // V4 draws the arrow in the last 30 px; the text may run up to it.
        label.setBounds(1, 1, box.getWidth() - 28, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
    }

    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        if (label.isBeingEdited())
        {
            juce::LookAndFeel_V4::drawLabel(g, label);
            return;
        }
        g.fillAll(label.findColour(juce::Label::backgroundColourId));
        const auto area = label.getBorderSize().subtractedFrom(label.getLocalBounds());
        auto font = getLabelFont(label);
        const float width = juce::GlyphArrangement::getStringWidth(font, label.getText());
        if (width > static_cast<float>(area.getWidth()) && width > 0.0f)
            font = font.withHeight(std::max(11.0f, font.getHeight() *
                                                       static_cast<float>(area.getWidth()) / width));
        g.setFont(font);
        g.setColour(label.findColour(juce::Label::textColourId)
                        .withMultipliedAlpha(label.isEnabled() ? 1.0f : 0.5f));
        g.drawFittedText(label.getText(), area, label.getJustificationType(), 1, 0.85f);
    }
};

VariAudioInspector::ValueRow::ValueRow(Param p, double minimum, double maximum,
                                       double defaultValue_, double step)
    : juce::Slider(juce::Slider::LinearBar, juce::Slider::NoTextBox),
      param(p),
      defaultValue(defaultValue_)
{
    setRange(minimum, maximum, step);
    setValue(defaultValue_, juce::dontSendNotification);
    setDoubleClickReturnValue(true, defaultValue_);
    // Drags are relative, like Cubase's inspector values.
    setSliderSnapsToMousePosition(false);
    setVelocityBasedMode(false);
    setMouseDragSensitivity(300);
    // A trackpad scroll would otherwise commit one edit (and one resynthesis)
    // per wheel tick across every target segment.
    setScrollWheelEnabled(false);
}

void VariAudioInspector::ValueRow::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(6.0f, 3.0f);
    const float alpha = isEnabled() ? 1.0f : 0.4f;

    auto top = bounds.removeFromTop(bounds.getHeight() - 8.0f);
    g.setFont(AppFont::getFont(14.5f));
    g.setColour(APP_COLOR_TEXT_MUTED.withMultipliedAlpha(alpha));
    g.drawText(caption, top, juce::Justification::centredLeft, true);
    g.setColour(APP_COLOR_TEXT_PRIMARY.withMultipliedAlpha(alpha));
    g.drawText(formatValue(param, getValue()), top, juce::Justification::centredRight,
               false);

    // Thin bar: bipolar values (formant) fill from the centre.
    auto bar = bounds.withSizeKeepingCentre(bounds.getWidth(), 4.0f);
    g.setColour(kBarTrack.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(bar, 2.0f);
    const double proportion = valueToProportionOfLength(getValue());
    const double origin =
        getMinimum() < 0.0 && getMaximum() > 0.0 ? valueToProportionOfLength(0.0) : 0.0;
    const float x0 = bar.getX() + bar.getWidth() * static_cast<float>(std::min(origin, proportion));
    const float x1 = bar.getX() + bar.getWidth() * static_cast<float>(std::max(origin, proportion));
    g.setColour(kBarFill.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(juce::Rectangle<float>(x0, bar.getY(),
                                                  std::max(3.0f, x1 - x0),
                                                  bar.getHeight()),
                           2.0f);
}

void VariAudioInspector::DisabledRow::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(6.0f, 3.0f);
    const float alpha = 0.42f;
    auto top = bounds.removeFromTop(bounds.getHeight() - 7.0f);
    g.setFont(AppFont::getFont(14.5f));
    g.setColour(APP_COLOR_TEXT_MUTED.withMultipliedAlpha(alpha));
    g.drawText(caption, top, juce::Justification::centredLeft, true);
    g.drawText(value, top, juce::Justification::centredRight, false);
    auto bar = bounds.withSizeKeepingCentre(bounds.getWidth(), 3.0f);
    g.setColour(kBarTrack.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(bar, 1.5f);
}

juce::String VariAudioInspector::formatValue(Param param, double value)
{
    switch (param)
    {
        case Param::CorrectPitch:
        case Param::FlattenCurve:
        case Param::PitchDrift:
            return juce::String(juce::roundToInt(value)) + " %";
        case Param::Formant:
            return (value > 0.0 ? "+" : "") + juce::String(value, 1) + " st";
        case Param::Volume:
            return value <= -59.9 ? juce::String::fromUTF8("-\xe2\x88\x9e dB")
                                  : (value > 0.0 ? "+" : "") + juce::String(value, 1) + " dB";
        case Param::Transition:
            return value <= 0.0 ? juce::String("off")
                                : juce::String(juce::roundToInt(value)) + " ms";
    }
    return {};
}

VariAudioInspector::VariAudioInspector()
    : comboLookAndFeel(std::make_unique<ComboLookAndFeel>())
{
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(kScrollBarThickness);
    addAndMakeVisible(viewport);

    variAudioHeader.setFont(AppFont::getBoldFont(18.0f));
    variAudioHeader.setColour(juce::Label::textColourId, juce::Colour(0xFF9B9B9Bu));
    content.addAndMakeVisible(variAudioHeader);

    for (auto* label : {&smartControlsLabel, &snapModeLabel, &engineLabel})
    {
        label->setFont(AppFont::getFont(15.0f));
        label->setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        content.addAndMakeVisible(*label);
    }

    for (auto* box : {&smartControlsBox, &snapModeBox, &engineBox, &scaleRootBox, &scaleModeBox})
        box->setLookAndFeel(comboLookAndFeel.get());

    styleCombo(smartControlsBox);
    smartControlsBox.onChange = [this]
    {
        if (onShowAllSmartControlsChanged)
            onShowAllSmartControlsChanged(smartControlsBox.getSelectedId() == 2);
    };
    content.addAndMakeVisible(smartControlsBox);

    styleCombo(snapModeBox);
    snapModeBox.onChange = [this]
    {
        const int id = snapModeBox.getSelectedId();
        shownSnapModeId = id;
        if (id >= 1 && id <= 3)
            setSnapModeInternal(static_cast<SnapMode>(id - 1), true);
    };
    content.addAndMakeVisible(snapModeBox);

    styleCombo(engineBox);
    engineBox.onChange = [this]
    {
        const int id = engineBox.getSelectedId();
        if (id >= 1 && id <= 4 && onNoteEngineChanged) {
            onNoteEngineChanged(engineFromMenuId(id));
            // The silence rule may refuse the request. Reflect the actual
            // engine after the edit instead of leaving the rejected choice
            // displayed as if it had taken effect.
            const auto actual = getNoteEngine ? getNoteEngine() : std::nullopt;
            if (actual) {
                shownEngineId = engineMenuId(*actual);
                engineBox.setSelectedId(shownEngineId,
                                        juce::dontSendNotification);
            }
        }
    };
    content.addAndMakeVisible(engineBox);

    // D-185: never leave a refused pick unexplained. Shows the block's actual
    // engine and, only when there was a refusal, the requested one and why.
    engineStatusLabel.setFont(AppFont::getFont(12.5f));
    engineStatusLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    engineStatusLabel.setJustificationType(juce::Justification::topLeft);
    engineStatusLabel.setMinimumHorizontalScale(0.85f);
    content.addAndMakeVisible(engineStatusLabel);

    targetLabel.setFont(AppFont::getFont(13.5f));
    targetLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    content.addAndMakeVisible(targetLabel);

    rows[0] = std::make_unique<ValueRow>(Param::CorrectPitch, 0.0, 100.0, 0.0, 1.0);
    rows[1] = std::make_unique<ValueRow>(Param::FlattenCurve, 0.0, 100.0, 0.0, 1.0);
    rows[2] = std::make_unique<ValueRow>(Param::PitchDrift, 0.0, 100.0, 0.0, 1.0);
    rows[3] = std::make_unique<ValueRow>(Param::Formant, -12.0, 12.0, 0.0, 0.1);
    rows[4] = std::make_unique<ValueRow>(Param::Volume, -60.0, 12.0, 0.0, 0.1);
    rows[5] = std::make_unique<ValueRow>(Param::Transition, 0.0, 250.0, 0.0, 1.0);
    for (auto& row : rows)
    {
        auto* r = row.get();
        r->onDragStart = [this, r]
        {
            editingRow = true;
            if (onParamEditBegin)
                onParamEditBegin(r->param);
        };
        r->onValueChange = [this, r]
        {
            if (editingRow)
            {
                if (onParamEditValue)
                    onParamEditValue(static_cast<float>(r->getValue()));
                return;
            }
            // Double-click reset or wheel: one self-contained edit.
            if (onParamEditBegin)
                onParamEditBegin(r->param);
            if (onParamEditValue)
                onParamEditValue(static_cast<float>(r->getValue()));
            if (onParamEditEnd)
                onParamEditEnd();
        };
        r->onDragEnd = [this]
        {
            editingRow = false;
            if (onParamEditEnd)
                onParamEditEnd();
            refreshFromProject();
        };
        content.addAndMakeVisible(*r);
    }

    for (auto& row : placeholderRows)
    {
        row = std::make_unique<DisabledRow>();
        content.addAndMakeVisible(*row);
    }

    for (auto* b : {&muteButton, &deleteButton})
    {
        b->setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_RAISED);
        b->setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_MUTED);
        b->setEnabled(false);
        content.addAndMakeVisible(*b);
    }

    auto setupMacro = [this](juce::TextButton& button, bool enabled)
    {
        button.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_RAISED);
        button.setColour(juce::TextButton::textColourOffId,
                         enabled ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_TEXT_MUTED);
        button.setEnabled(enabled);
        content.addAndMakeVisible(button);
    };
    setupMacro(correctPitchButton, true);
    setupMacro(quantizeButton, true);
    setupMacro(levelingButton, false);
    correctPitchButton.onClick = [this]
    {
        if (onCorrectPitchMacro)
            onCorrectPitchMacro();
    };
    // D-242: la fuerza es parte del criterio, asi que el clic abre la eleccion
    // en vez de aplicar 100 % a ciegas. 100 % va primero.
    quantizeButton.onClick = [this]
    {
        juce::PopupMenu menu;
        for (const int percent : {100, 75, 50, 25})
            menu.addItem(percent, juce::String(percent) + " %", true,
                         percent == quantizeStrengthPercent);
        juce::Component::SafePointer<VariAudioInspector> safe(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&quantizeButton),
            [safe](int result)
            {
                if (safe == nullptr || result <= 0)
                    return;
                safe->quantizeStrengthPercent = result;
                if (safe->onQuantizeTimeMacro)
                    safe->onQuantizeTimeMacro(result);
            });
    };

    useProjectKeyToggle.onClick = [this]
    {
        auto* project = getCurrentProject ? getCurrentProject() : nullptr;
        if (project == nullptr)
            return;
        project->setUseProjectKey(useProjectKeyToggle.getToggleState());
        if (onEditorScaleChanged)
            onEditorScaleChanged();
        refreshFromProject();
    };

    styleCombo(scaleRootBox);
    for (size_t i = 0; i < kRootLabels.size(); ++i)
        scaleRootBox.addItem(kRootLabels[i], static_cast<int>(i) + 1);
    scaleRootBox.setTextWhenNothingSelected("-");
    scaleRootBox.onChange = [this]
    {
        const int id = scaleRootBox.getSelectedId();
        shownRootId = id;
        if (id >= 1 && id <= static_cast<int>(kRootLabels.size()))
            applyScaleRoot(id - 1);
    };

    styleCombo(scaleModeBox);
    scaleModeBox.setTextWhenNothingSelected("-");
    scaleModeBox.onChange = [this]
    {
        const int id = scaleModeBox.getSelectedId();
        shownModeId = id;
        if (id >= 1 && id <= static_cast<int>(kModeOptions.size()))
            applyScaleMode(kModeOptions[static_cast<size_t>(id - 1)].mode);
    };

    snapScaleToggle.onClick = [this]
    {
        if (onDragSnapModeChanged)
            onDragSnapModeChanged(snapScaleToggle.getToggleState()
                                      ? DragSnapMode::Scale
                                      : DragSnapMode::Chromatic);
    };

    refreshLocalisedText();
    // Selection, snap state and values change from many places (canvas,
    // parameter panel, host); polling keeps the inspector honest.
    startTimerHz(8);
}

VariAudioInspector::~VariAudioInspector()
{
    stopTimer();
    for (auto* box : {&smartControlsBox, &snapModeBox, &engineBox, &scaleRootBox, &scaleModeBox})
        box->setLookAndFeel(nullptr);
    viewport.setViewedComponent(nullptr, false);
}

void VariAudioInspector::refreshLocalisedText()
{
    variAudioHeader.setText(TR("inspector.variaudio"), juce::dontSendNotification);    smartControlsLabel.setText(TR("inspector.smart_controls"), juce::dontSendNotification);
    snapModeLabel.setText(TR("inspector.snap_mode"), juce::dontSendNotification);
    engineLabel.setText(TR("inspector.engine"), juce::dontSendNotification);
    {
        const int engineId = engineBox.getSelectedId();
        engineBox.clear(juce::dontSendNotification);
        engineBox.addItem(TR("inspector.engine_inherit"), 1);
        engineBox.addItem(TR("inspector.engine_vocoder"), 2);
        engineBox.addItem(TR("inspector.engine_psola"), 3);
        engineBox.addItem(TR("inspector.engine_pitchcore"), 4);
        engineBox.setSelectedId(engineId > 0 ? engineId : 1,
                                juce::dontSendNotification);
        shownEngineId = engineBox.getSelectedId();
    }

    const int smartId = smartControlsBox.getSelectedId();
    smartControlsBox.clear(juce::dontSendNotification);
    smartControlsBox.addItem(TR("inspector.smart_default"), 1);
    smartControlsBox.addItem(TR("inspector.smart_all_short"), 2);
    smartControlsBox.setSelectedId(smartId > 0 ? smartId : 2, juce::dontSendNotification);

    const int snapId = snapModeBox.getSelectedId();
    snapModeBox.clear(juce::dontSendNotification);
    snapModeBox.addItem(TR("inspector.snap_absolute"), 1);
    snapModeBox.addItem(TR("inspector.snap_relative"), 2);
    snapModeBox.addItem(TR("inspector.snap_off"), 3);
    snapModeBox.setSelectedId(snapId > 0 ? snapId : 1, juce::dontSendNotification);
    shownSnapModeId = snapModeBox.getSelectedId();

    editorScaleHeader.setText(TR("inspector.editor_scale"), juce::dontSendNotification);
    useProjectKeyToggle.setButtonText(TR("inspector.use_project_key"));
    const int modeId = scaleModeBox.getSelectedId();
    scaleModeBox.clear(juce::dontSendNotification);
    for (size_t i = 0; i < kModeOptions.size(); ++i)
        scaleModeBox.addItem(TR(kModeOptions[i].labelKey), static_cast<int>(i) + 1);
    scaleModeBox.setSelectedId(modeId, juce::dontSendNotification);
    shownModeId = scaleModeBox.getSelectedId();

    snapScaleToggle.setButtonText(TR("inspector.snap_scale"));
    rows[0]->caption = TR("inspector.pitch");
    rows[1]->caption = TR("inspector.pitch_modulation");
    rows[2]->caption = TR("inspector.pitch_drift");
    rows[3]->caption = TR("inspector.formant");
    rows[4]->caption = TR("inspector.amplitude");
    rows[5]->caption = TR("inspector.transition");
    // D-225: the control belongs to the union with the next note.
    rows[5]->setTooltip(TR("inspector.transition_hint"));
    placeholderRows[0]->caption = TR("inspector.sibilant");
    placeholderRows[1]->caption = TR("inspector.attack_speed");
    muteButton.setButtonText(TR("inspector.mute_note"));
    deleteButton.setButtonText(TR("inspector.delete_note"));
    correctPitchButton.setButtonText(TR("macro.correct_pitch"));
    quantizeButton.setButtonText(TR("macro.quantize_time"));
    levelingButton.setButtonText(TR("macro.leveling"));
    correctPitchButton.setTooltip(TR("macro.correct_pitch_hint"));
    quantizeButton.setTooltip(TR("macro.quantize_time_hint"));
    levelingButton.setTooltip(TR("macro.not_implemented"));
    for (auto& row : rows)
        row->repaint();
    for (auto& row : placeholderRows)
        row->repaint();
    refreshFromProject();
}

void VariAudioInspector::setEditorMode(EditorUiMode mode)
{
    editorMode = mode;
    refreshFromProject();
    repaint();
}

void VariAudioInspector::timerCallback()
{
    refreshFromProject();
}

void VariAudioInspector::refreshFromProject()
{
    auto* project = getCurrentProject ? getCurrentProject() : nullptr;
    const bool hasProject = project != nullptr;
    smartControlsBox.setEnabled(hasProject);
    snapModeBox.setEnabled(hasProject);
    snapScaleToggle.setEnabled(hasProject && project->getSnapToSemitones());

    const auto engineValue = getNoteEngine ? getNoteEngine() : std::nullopt;
    engineBox.setEnabled(hasProject && engineValue.has_value());
    if (engineValue)
        showMenuValue(engineBox, engineMenuId(*engineValue), shownEngineId);

    // D-185: effective engine of the block, plus the refused request if the
    // last gesture could not change it (D-129).
    const auto engineStatus = getEngineStatus ? getEngineStatus() : std::nullopt;
    if (engineStatus)
    {
        const auto nameOf = [](SynthesisEngineType engine)
        {
            switch (engine)
            {
            case SynthesisEngineType::Vocoder:   return TR("inspector.engine_vocoder");
            case SynthesisEngineType::Pitchcore: return TR("inspector.engine_pitchcore");
            case SynthesisEngineType::Psola:
            default:                             return TR("inspector.engine_psola");
            }
        };
        juce::String text = TR("inspector.engine_effective")
                                .replace("{engine}", nameOf(engineStatus->effective));
        if (engineStatus->refusedRequest)
        {
            const auto requestedName =
                *engineStatus->refusedRequest == NoteEngine::Vocoder
                    ? TR("inspector.engine_vocoder")
                    : *engineStatus->refusedRequest == NoteEngine::Pitchcore
                          ? TR("inspector.engine_pitchcore")
                          : *engineStatus->refusedRequest == NoteEngine::Psola
                                ? TR("inspector.engine_psola")
                                : TR("inspector.engine_inherit");
            text += "\n" + TR("inspector.engine_refused")
                              .replace("{requested}", requestedName);
        }
        engineStatusLabel.setText(text, juce::dontSendNotification);
    }
    else
        engineStatusLabel.setText({}, juce::dontSendNotification);

    if (hasProject)
    {
        // Off is the project's snap switch; Absolute vs Relative is ours.
        const auto mode = !project->getSnapToSemitones() ? SnapMode::Off
                          : absoluteSelected             ? SnapMode::Absolute
                                                         : SnapMode::Relative;
        showMenuValue(snapModeBox, static_cast<int>(mode) + 1, shownSnapModeId);
        snapScaleToggle.setToggleState(project->getDragSnapMode() == DragSnapMode::Scale,
                                       juce::dontSendNotification);
    }
    refreshEditorScale(project);

    const auto [count, isSelection] =
        getTargetCount ? getTargetCount() : std::pair<int, bool>{0, false};
    targetLabel.setText(count <= 0 ? TR("inspector.target_none")
                        : isSelection
                            ? TR("inspector.target_selected").replace("{n}", juce::String(count))
                            : TR("inspector.target_all").replace("{n}", juce::String(count)),
                        juce::dontSendNotification);

    if (editingRow)
        return; // never fight the user's drag
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto value = getParamValue ? getParamValue(rows[i]->param) : std::nullopt;
        const bool advancedOnly = rows[i]->param == Param::Formant ||
                                  rows[i]->param == Param::Volume;
        const bool enabled = value.has_value() &&
                             (!advancedOnly || editorMode == EditorUiMode::Advanced);
        rows[i]->setEnabled(enabled);
        if (value && std::abs(rows[i]->getValue() - *value) > 1.0e-4)
        {
            rows[i]->setValue(*value, juce::dontSendNotification);
            rows[i]->repaint();
        }
    }
}

void VariAudioInspector::setSnapModeInternal(SnapMode mode, bool notify)
{
    if (mode != SnapMode::Off)
        absoluteSelected = mode == SnapMode::Absolute;
    if (notify && onSnapModeChanged)
        onSnapModeChanged(mode);
    refreshFromProject();
}

void VariAudioInspector::showMenuValue(juce::ComboBox& box, int id, int& shownId)
{
    // An open popup or a pick still waiting for its onChange wins.
    if (box.isPopupActive() || box.getSelectedId() != shownId)
        return;
    box.setSelectedId(id, juce::dontSendNotification);
    shownId = box.getSelectedId();
}

void VariAudioInspector::refreshEditorScale(Project* project)
{
    const bool hasProject = project != nullptr;
    scaleRootBox.setEnabled(hasProject);
    scaleModeBox.setEnabled(hasProject);
    useProjectKeyToggle.setEnabled(hasProject);
    if (!hasProject)
    {
        showMenuValue(scaleRootBox, 0, shownRootId);
        showMenuValue(scaleModeBox, 0, shownModeId);
        return;
    }

    // The key this region actually uses: harmony and the piano roll follow it.
    const int root = project->getScaleRootNote();
    const ScaleMode mode = project->getPreferredScaleMode();
    showMenuValue(scaleRootBox, root >= 0 && root < 12 ? root + 1 : 0, shownRootId);
    // A key that is not set shows "-" rather than a guessed major.
    showMenuValue(scaleModeBox, scaleModeId(mode), shownModeId);
    useProjectKeyToggle.setToggleState(project->getUseProjectKey(),
                                       juce::dontSendNotification);
}

void VariAudioInspector::applyScaleRoot(int root)
{
    auto* project = getCurrentProject ? getCurrentProject() : nullptr;
    if (project == nullptr)
        return;
    const int normalized = juce::jlimit(0, 11, root);
    if (projectKeyScope() && project->getUseProjectKey())
        project->setProjectKey(normalized, project->getProjectKeyMode());
    else
        project->setScaleRootNote(normalized); // also stops following the project key
    if (onEditorScaleChanged)
        onEditorScaleChanged();
    refreshFromProject();
}

void VariAudioInspector::applyScaleMode(ScaleMode mode)
{
    auto* project = getCurrentProject ? getCurrentProject() : nullptr;
    if (project == nullptr)
        return;
    if (projectKeyScope() && project->getUseProjectKey())
        project->setProjectKey(project->getProjectKeyRoot(), mode);
    else
        project->setPreferredScaleMode(mode); // also stops following the project key
    if (onEditorScaleChanged)
        onEditorScaleChanged();
    refreshFromProject();
}

void VariAudioInspector::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF121212u));
}

void VariAudioInspector::paintContent(juce::Graphics& g)
{
    constexpr float radius = 8.0f;
    // Card backgrounds: the note inspector is the only card left; the editor
    // scale moved to the top scale dropdown and harmony has its own rail
    // drawer, so neither is repeated here.
    auto rect = variAudioCardBounds.toFloat();
    g.setColour(juce::Colour(0xFF171717u));
    g.fillRoundedRectangle(rect, radius);
    g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
    g.drawRoundedRectangle(rect.reduced(0.5f), radius, 0.75f);

    // Rows sit in one framed box, separated by hairlines, as in Cubase.
    auto box = rowsBoxBounds.toFloat();
    g.setColour(kRowsBoxColour);
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(kRowsBoxBorder);
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
    for (size_t i = 1; i < rows.size(); ++i)
    {
        const float y = static_cast<float>(rows[i]->getY());
        g.drawHorizontalLine(static_cast<int>(y), box.getX() + 1.0f, box.getRight() - 1.0f);
    }

    auto placeholders = placeholdersBox.toFloat();
    g.setColour(kRowsBoxColour.withAlpha(0.55f));
    g.fillRoundedRectangle(placeholders, 4.0f);
    g.setColour(kRowsBoxBorder.withAlpha(0.65f));
    g.drawRoundedRectangle(placeholders.reduced(0.5f), 4.0f, 1.0f);
    for (size_t i = 1; i < placeholderRows.size(); ++i)
        g.drawHorizontalLine(placeholderRows[i]->getY(), placeholders.getX() + 1.0f,
                             placeholders.getRight() - 1.0f);

    auto macros = macrosCardBounds.toFloat();
    g.setColour(kRowsBoxColour.withAlpha(0.55f));
    g.fillRoundedRectangle(macros, 4.0f);
    g.setColour(kRowsBoxBorder.withAlpha(0.65f));
    g.drawRoundedRectangle(macros.reduced(0.5f), 4.0f, 1.0f);
}

int VariAudioInspector::getContentHeight() const
{
    return kPadY + kVariAudioCardHeight + kPadY;
}

void VariAudioInspector::resized()
{
    viewport.setBounds(getLocalBounds());
    const int contentHeight = getContentHeight();
    const bool scrolls = contentHeight > getHeight();
    const int width = std::max(0, getWidth() - (scrolls ? kScrollBarThickness : 0));
    content.setSize(width, contentHeight);
    layoutContent(width);
}

void VariAudioInspector::layoutContent(int width)
{
    auto area = juce::Rectangle<int>(0, 0, width, content.getHeight()).reduced(kPadX, kPadY);

    variAudioCardBounds = area.removeFromTop(kVariAudioCardHeight);
    auto card = variAudioCardBounds.reduced(kInnerPadX, kInnerPadY);
    variAudioHeader.setBounds(card.removeFromTop(kHeaderHeight));
    card.removeFromTop(kRowGap);

    const int menuWidth = 92;
    auto smartRow = card.removeFromTop(kMenuRowHeight);
    smartControlsBox.setBounds(smartRow.removeFromRight(menuWidth).reduced(0, 1));
    smartControlsLabel.setBounds(smartRow);
    auto snapRow = card.removeFromTop(kMenuRowHeight);
    snapModeBox.setBounds(snapRow.removeFromRight(menuWidth).reduced(0, 1));
    snapModeLabel.setBounds(snapRow);
    auto engineRow = card.removeFromTop(kMenuRowHeight);
    engineBox.setBounds(engineRow.removeFromRight(menuWidth).reduced(0, 1));
    engineLabel.setBounds(engineRow);
    card.removeFromTop(kRowGap);
    engineStatusLabel.setBounds(card.removeFromTop(kEngineStatusHeight));
    card.removeFromTop(kRowGap);
    targetLabel.setBounds(card.removeFromTop(kTargetLabelHeight));

    auto actionRow = card.removeFromTop(kActionRowHeight);
    muteButton.setBounds(actionRow.removeFromLeft(actionRow.getWidth() / 2 - 3));
    actionRow.removeFromLeft(6);
    deleteButton.setBounds(actionRow);
    card.removeFromTop(kRowGap);

    rowsBoxBounds = card.removeFromTop(kRowsBoxHeight);
    auto rowArea = rowsBoxBounds.reduced(0, 2);
    for (auto& row : rows)
        row->setBounds(rowArea.removeFromTop(kValueRowHeight));

    card.removeFromTop(kRowGap);
    placeholdersBox = card.removeFromTop(kPlaceholderBoxHeight);
    auto placeholderArea = placeholdersBox.reduced(0, 2);
    for (auto& row : placeholderRows)
        row->setBounds(placeholderArea.removeFromTop(kPlaceholderRowHeight));

    card.removeFromTop(kRowGap);
    macrosCardBounds = card.removeFromTop(kMacrosCardHeight);
    auto macroArea = macrosCardBounds.reduced(0, 2);
    correctPitchButton.setBounds(macroArea.removeFromTop(kMacroRowHeight));
    macroArea.removeFromTop(4);
    quantizeButton.setBounds(macroArea.removeFromTop(kMacroRowHeight));
    macroArea.removeFromTop(4);
    levelingButton.setBounds(macroArea.removeFromTop(kMacroRowHeight));

    content.repaint();
}
