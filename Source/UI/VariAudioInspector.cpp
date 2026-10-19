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
constexpr int kHeaderHeight = 20;
constexpr int kMenuRowHeight = 26;
constexpr int kToggleHeight = 24;
constexpr int kTargetLabelHeight = 18;
constexpr int kValueRowHeight = 34;
constexpr int kNumRows = 5;

constexpr int kRowsBoxHeight = kNumRows * kValueRowHeight + 4;
constexpr int kVariAudioCardHeight =
    kInnerPadY * 2 + kHeaderHeight + kRowGap + kMenuRowHeight * 2 + kRowGap +
    kTargetLabelHeight + kRowsBoxHeight;
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
    juce::Font getComboBoxFont(juce::ComboBox&) override { return AppFont::getFont(14.0f); }

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
    g.setFont(AppFont::getFont(13.0f));
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

    for (auto* header : {&variAudioHeader, &editorScaleHeader})
    {
        header->setFont(AppFont::getBoldFont(16.0f));
        header->setColour(juce::Label::textColourId, juce::Colour(0xFF9B9B9Bu));
        content.addAndMakeVisible(*header);
    }

    for (auto* label : {&smartControlsLabel, &snapModeLabel})
    {
        label->setFont(AppFont::getFont(13.0f));
        label->setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        content.addAndMakeVisible(*label);
    }

    for (auto* box : {&smartControlsBox, &snapModeBox, &scaleRootBox, &scaleModeBox})
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

    targetLabel.setFont(AppFont::getFont(12.0f));
    targetLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    content.addAndMakeVisible(targetLabel);

    rows[0] = std::make_unique<ValueRow>(Param::CorrectPitch, 0.0, 100.0, 0.0, 1.0);
    rows[1] = std::make_unique<ValueRow>(Param::FlattenCurve, 0.0, 100.0, 0.0, 1.0);
    rows[2] = std::make_unique<ValueRow>(Param::PitchDrift, 0.0, 100.0, 0.0, 1.0);
    rows[3] = std::make_unique<ValueRow>(Param::Formant, -12.0, 12.0, 0.0, 0.1);
    rows[4] = std::make_unique<ValueRow>(Param::Volume, -60.0, 12.0, 0.0, 0.1);
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
    content.addAndMakeVisible(useProjectKeyToggle);

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
    content.addAndMakeVisible(scaleRootBox);

    styleCombo(scaleModeBox);
    scaleModeBox.setTextWhenNothingSelected("-");
    scaleModeBox.onChange = [this]
    {
        const int id = scaleModeBox.getSelectedId();
        shownModeId = id;
        if (id >= 1 && id <= static_cast<int>(kModeOptions.size()))
            applyScaleMode(kModeOptions[static_cast<size_t>(id - 1)].mode);
    };
    content.addAndMakeVisible(scaleModeBox);

    snapScaleToggle.onClick = [this]
    {
        if (onDragSnapModeChanged)
            onDragSnapModeChanged(snapScaleToggle.getToggleState()
                                      ? DragSnapMode::Scale
                                      : DragSnapMode::Chromatic);
    };
    content.addAndMakeVisible(snapScaleToggle);

    harmonyPanel.getCurrentProject = [this] { return getCurrentProject ? getCurrentProject() : nullptr; };
    content.addAndMakeVisible(harmonyPanel);

    refreshLocalisedText();
    // Selection, snap state and values change from many places (canvas,
    // parameter panel, host); polling keeps the inspector honest.
    startTimerHz(8);
}

VariAudioInspector::~VariAudioInspector()
{
    stopTimer();
    for (auto* box : {&smartControlsBox, &snapModeBox, &scaleRootBox, &scaleModeBox})
        box->setLookAndFeel(nullptr);
    viewport.setViewedComponent(nullptr, false);
}

void VariAudioInspector::refreshLocalisedText()
{
    variAudioHeader.setText(TR("inspector.variaudio"), juce::dontSendNotification);
    smartControlsLabel.setText(TR("inspector.smart_controls"), juce::dontSendNotification);
    snapModeLabel.setText(TR("inspector.snap_mode"), juce::dontSendNotification);

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
    rows[0]->caption = TR("inspector.correct_pitch");
    rows[1]->caption = TR("inspector.flatten_curve");
    rows[2]->caption = TR("inspector.pitch_drift");
    rows[3]->caption = TR("inspector.shift_formant");
    rows[4]->caption = TR("inspector.volume");
    for (auto& row : rows)
        row->repaint();
    refreshFromProject();
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
    for (auto& row : rows)
    {
        const auto value = getParamValue ? getParamValue(row->param) : std::nullopt;
        row->setEnabled(value.has_value());
        if (value && std::abs(row->getValue() - *value) > 1.0e-4)
        {
            row->setValue(*value, juce::dontSendNotification);
            row->repaint();
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
    for (const auto& cardBounds : {variAudioCardBounds, editorScaleCardBounds})
    {
        auto rect = cardBounds.toFloat();
        g.setColour(juce::Colour(0xFF171717u));
        g.fillRoundedRectangle(rect, radius);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
        g.drawRoundedRectangle(rect.reduced(0.5f), radius, 0.75f);
    }

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
}

int VariAudioInspector::getContentHeight() const
{
    return kPadY + kVariAudioCardHeight + kPadY + kEditorScaleCardHeight +
           harmonyPanel.getPreferredHeight() + kPadY;
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
    card.removeFromTop(kRowGap);
    targetLabel.setBounds(card.removeFromTop(kTargetLabelHeight));

    rowsBoxBounds = card.removeFromTop(kRowsBoxHeight);
    auto rowArea = rowsBoxBounds.reduced(0, 2);
    for (auto& row : rows)
        row->setBounds(rowArea.removeFromTop(kValueRowHeight));

    // Editor scale, as in Cubase: the scale the harmony buttons follow.
    area.removeFromTop(kPadY);
    editorScaleCardBounds = area.removeFromTop(kEditorScaleCardHeight);
    auto scaleCard = editorScaleCardBounds.reduced(kInnerPadX, kInnerPadY);
    editorScaleHeader.setBounds(scaleCard.removeFromTop(kHeaderHeight));
    scaleCard.removeFromTop(kRowGap);
    useProjectKeyToggle.setBounds(scaleCard.removeFromTop(kToggleHeight));
    scaleCard.removeFromTop(kRowGap);
    auto scaleRow = scaleCard.removeFromTop(kMenuRowHeight);
    scaleRootBox.setBounds(scaleRow.removeFromLeft(kRootMenuWidth).reduced(0, 1));
    scaleRow.removeFromLeft(kMenuGap);
    scaleModeBox.setBounds(scaleRow.reduced(0, 1));
    scaleCard.removeFromTop(kRowGap);
    snapScaleToggle.setBounds(scaleCard.removeFromTop(kToggleHeight));

    // HarmonyPanel pads itself like the side-panel cards; give it the column.
    area.setX(area.getX() - kPadX);
    area.setWidth(width);
    harmonyPanel.setBounds(area.removeFromTop(harmonyPanel.getPreferredHeight()));
    content.repaint();
}
