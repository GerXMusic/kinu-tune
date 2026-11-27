#include "ToolbarComponent.h"
#include "PianoRollComponent.h" // For EditMode enum
#include "StyledComponents.h"
#include "Components/AppFont.h"
#include "MelodyneIcons.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/TimecodeFont.h"
#include "BinaryData.h"

namespace
{
constexpr int kToolSlot = 34;
constexpr int kToolGap = 2;

void styleFlatTextButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    button.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);
    button.setColour(juce::TextButton::textColourOnId, APP_COLOR_TEXT_PRIMARY);
}

class ModeButtonLook final : public juce::LookAndFeel_V4
{
public:
    juce::Font getTextButtonFont(juce::TextButton&, int) override
    {
        return AppFont::getFont(15.5f);
    }
};

ModeButtonLook& modeButtonLook()
{
    static ModeButtonLook look;
    return look;
}
} // namespace

ToolbarComponent::ToolIconButton::ToolIconButton(EditorTool toolToUse, GuiIcon iconToUse)
    : juce::Button("tool"), tool(toolToUse), icon(iconToUse)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    refreshTooltip();
}

void ToolbarComponent::ToolIconButton::refreshTooltip()
{
    // D-279: Melodyne's Formant and Amplitude tools have no engine here yet.
    // They stay in the row in Advanced, visibly disabled, instead of pretending
    // to edit.
    if (!isEnabled())
        setTooltip(TR(toolLabelKey(tool)) + " - " +
                   TR("macro.not_implemented"));
    else
        setTooltip(TR(toolLabelKey(tool)));
}

void ToolbarComponent::ToolIconButton::paintButton(juce::Graphics& g, bool over,
                                                   bool down)
{
    g.setOpacity(isEnabled() ? 1.0f : 0.35f);
    const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    if (active)
    {
        g.setColour(APP_COLOR_PRIMARY.withAlpha(0.20f));
        g.fillRoundedRectangle(bounds, 7.0f);
        g.setColour(APP_COLOR_PRIMARY.withAlpha(0.85f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 7.0f, 1.0f);
    }
    else if (over || down)
    {
        g.setColour(APP_COLOR_RAIL_ACTIVE.withAlpha(down ? 0.95f : 0.7f));
        g.fillRoundedRectangle(bounds, 7.0f);
    }

    const auto iconBox = bounds.withSizeKeepingCentre(bounds.getHeight() * 0.58f,
                                                      bounds.getHeight() * 0.58f);
    drawGuiIcon(g, icon, iconBox,
                active ? APP_COLOR_PRIMARY_GLOW
                       : (over ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_RAIL_ICON),
                1.75f);
}

ToolbarComponent::ToolbarComponent()
{
    auto loadImage = [](const void* data, int size)
    { return juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size)); };

    recordButton.setImage(loadImage(BinaryData::record_png, BinaryData::record_pngSize));
    playButton.setImage(loadImage(BinaryData::play_png, BinaryData::play_pngSize));
    stopButton.setImage(loadImage(BinaryData::stop_png, BinaryData::stop_pngSize));
    loopButton.setImage(loadImage(BinaryData::cycle_png, BinaryData::cycle_pngSize));
    auditionButton.setImage(loadImage(BinaryData::audition_png, BinaryData::audition_pngSize));
    undoButton.setImage(loadImage(BinaryData::undo_png, BinaryData::undo_pngSize));
    redoButton.setImage(loadImage(BinaryData::redo_png, BinaryData::redo_pngSize));
    selectModeButton.setImage(loadImage(BinaryData::select_png, BinaryData::select_pngSize));
    splitModeButton.setImage(loadImage(BinaryData::split_png, BinaryData::split_pngSize));
    pitchDrawingModeButton.setImage(loadImage(BinaryData::drawing_png, BinaryData::drawing_pngSize));
    timingModeButton.setImage(loadImage(BinaryData::timing_png, BinaryData::timing_pngSize));
    logoImage = loadImage(BinaryData::logo_png, BinaryData::logo_pngSize);

    followButton.setImage(loadImage(BinaryData::scroll_png, BinaryData::scroll_pngSize));
    parametersButton.setImage(loadImage(BinaryData::side_png, BinaryData::side_pngSize));
    leftPanelButton.setImage(loadImage(BinaryData::tracks_png, BinaryData::tracks_pngSize));
    trackViewButton.setImage(loadImage(BinaryData::region_png, BinaryData::region_pngSize));

    // Playback is driven by the host (REAPER), so the editor does not show
    // its own transport. The buttons are kept as state holders for the
    // existing callbacks, but never added as visible children.
    addChildComponent(recordButton);
    addChildComponent(playButton);
    addChildComponent(stopButton);
    addChildComponent(loopButton);
    addChildComponent(followButton);
    addChildComponent(auditionButton);
    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    addChildComponent(parametersButton);
    addChildComponent(leftPanelButton);
    addChildComponent(trackViewButton);
    // The old four tool buttons are kept as state holders for existing
    // callbacks, but the Melodyne row below is what the user sees.
    addChildComponent(selectModeButton);
    addChildComponent(splitModeButton);
    addChildComponent(pitchDrawingModeButton);
    addChildComponent(timingModeButton);

    recordButton.addListener(this);
    playButton.addListener(this);
    stopButton.addListener(this);
    loopButton.addListener(this);
    followButton.addListener(this);
    auditionButton.addListener(this);
    undoButton.addListener(this);
    redoButton.addListener(this);
    parametersButton.addListener(this);
    leftPanelButton.addListener(this);
    trackViewButton.addListener(this);
    selectModeButton.addListener(this);
    splitModeButton.addListener(this);
    pitchDrawingModeButton.addListener(this);
    timingModeButton.addListener(this);

    // Melodyne tool row.
    const EditorTool tools[] = {EditorTool::Main, EditorTool::Pitch,
                                EditorTool::Formant, EditorTool::Amplitude,
                                EditorTool::Timing, EditorTool::Separation};
    for (size_t i = 0; i < toolButtons.size(); ++i)
    {
        toolButtons[i] = std::make_unique<ToolIconButton>(tools[i], iconForTool(tools[i]));
        toolButtons[i]->setEnabled(!isAdvancedOnlyTool(tools[i]));
        toolButtons[i]->onClick = [this, tool = tools[i]] {
            setActiveTool(tool);
            if (onToolSelected)
                onToolSelected(tool);
        };
        addAndMakeVisible(*toolButtons[i]);
    }

    styleFlatTextButton(basicModeButton);
    styleFlatTextButton(advancedModeButton);
    basicModeButton.setLookAndFeel(&modeButtonLook());
    advancedModeButton.setLookAndFeel(&modeButtonLook());
    styleFlatTextButton(scaleButton);
    styleFlatTextButton(tempoButton);
    styleFlatTextButton(gridButton);
    styleFlatTextButton(engineButton);
    bypassButton.setTooltip(TR("engine.bypass_hint"));
    bypassButton.onClick = [this]
    {
        if (onBypassClicked)
            onBypassClicked();
    };

    basicModeButton.onClick = [this] { setUiMode(EditorUiMode::Basic);
                                       if (onUiModeChanged) onUiModeChanged(EditorUiMode::Basic); };
    advancedModeButton.onClick = [this] { setUiMode(EditorUiMode::Advanced);
                                          if (onUiModeChanged) onUiModeChanged(EditorUiMode::Advanced); };
    scaleButton.onClick = [this] { showScaleMenu(); };
    gridButton.onClick = [this] { showGridMenu(); };
    engineButton.onClick = [this] { if (onEngineClicked) onEngineClicked(); };
    tempoButton.setMouseCursor(juce::MouseCursor::NormalCursor);

    addAndMakeVisible(basicModeButton);
    addAndMakeVisible(advancedModeButton);
    addAndMakeVisible(scaleButton);
    addAndMakeVisible(tempoButton);
    addAndMakeVisible(gridButton);
    addAndMakeVisible(bypassButton);
    // The engine summary chip was removed: the engine in use is chosen in the
    // Motor drawer, and the top bar no longer announces it.
    addChildComponent(engineButton);
    engineButton.setVisible(false);

    // Tooltips and captions come from the string table.
    refreshLocalisedText();

    selectModeButton.setToggleState(true, juce::dontSendNotification);
    followButton.setToggleState(followPlayback, juce::dontSendNotification);
    auditionButton.setToggleState(false, juce::dontSendNotification);
    undoButton.setEnabled(false);
    redoButton.setEnabled(false);

    // Time label (hidden in the compact Melodyne bar, kept for the API).
    addChildComponent(timeLabel);
    timeLabel.setText("00:00.000 / 00:00.000", juce::dontSendNotification);
    timeLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    timeLabel.setJustificationType(juce::Justification::centred);
    timeLabel.setFont(TimecodeFont::getBoldFont(21.0f).withHorizontalScale(0.92f));

    addChildComponent(zoomLabel);
    addChildComponent(zoomSlider);
    zoomLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    zoomSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 1.0);
    zoomSlider.setValue(100.0);
    zoomSlider.setSkewFactorFromMidPoint(200.0);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.addListener(this);
    zoomSlider.setColour(juce::Slider::backgroundColourId, APP_COLOR_SURFACE_ALT);
    zoomSlider.setColour(juce::Slider::trackColourId, APP_COLOR_PRIMARY.withAlpha(0.75f));
    zoomSlider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);

    addChildComponent(progressBar);
    addChildComponent(progressLabel);
    progressLabel.setVisible(false);
    progressBar.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(0xFFFF5600u));
    progressBar.setColour(juce::ProgressBar::backgroundColourId,
                          juce::Colours::transparentBlack);
    progressBar.setLookAndFeel(&DarkLookAndFeel::getInstance());

    updateModeButtonStates();
    updateToolButtonStates();
}

ToolbarComponent::~ToolbarComponent()
{
    basicModeButton.setLookAndFeel(nullptr);
    advancedModeButton.setLookAndFeel(nullptr);
    progressBar.setLookAndFeel(nullptr);
}

void ToolbarComponent::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // Flat graphite top bar with a single hairline under it.
    g.setColour(juce::Colour(0xFF15171Bu));
    g.fillRect(bounds);
    g.setColour(APP_COLOR_BORDER_SUBTLE);
    g.fillRect(0.0f, bounds.getBottom() - 1.0f, bounds.getWidth(), 1.0f);

    if (logoImage.isValid())
    {
        const int logoW = (logoImage.getWidth() + 1) / 2;
        const int logoH = (logoImage.getHeight() + 1) / 2;
        g.drawImage(logoImage, 15, (getHeight() - logoH) / 2, logoW, logoH,
                    0, 0, logoImage.getWidth(), logoImage.getHeight());
    }

    // Basic / Advanced: two independent buttons, each with its own frame.
    auto drawModeButton = [&](juce::Rectangle<int> bounds, bool active)
    {
        g.setColour(juce::Colour(0xFF101215u));
        g.fillRoundedRectangle(bounds.toFloat(), 7.0f);
        if (active)
        {
            g.setColour(APP_COLOR_PRIMARY.withAlpha(0.22f));
            g.fillRoundedRectangle(bounds.toFloat().reduced(2.0f), 5.5f);
            g.setColour(APP_COLOR_PRIMARY);
            g.fillRoundedRectangle(bounds.toFloat().reduced(2.0f).removeFromBottom(2.0f),
                                   1.0f);
        }
        g.setColour(active ? APP_COLOR_PRIMARY.withAlpha(0.55f)
                           : APP_COLOR_BORDER_SUBTLE);
        g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 7.0f, 1.0f);
    };
    drawModeButton(basicModeBounds, uiMode == EditorUiMode::Basic);
    drawModeButton(advancedModeBounds, uiMode == EditorUiMode::Advanced);

    // Tool strip background.
    if (!toolContainerBounds.isEmpty())
    {
        g.setColour(juce::Colour(0xFF101215u));
        g.fillRoundedRectangle(toolContainerBounds.toFloat(), 9.0f);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawRoundedRectangle(toolContainerBounds.toFloat().reduced(0.5f), 9.0f, 1.0f);
    }

    // Right-side utility group.
    if (!rightGroupBounds.isEmpty())
    {
        g.setColour(juce::Colour(0xFF101215u));
        g.fillRoundedRectangle(rightGroupBounds.toFloat(), 9.0f);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawRoundedRectangle(rightGroupBounds.toFloat().reduced(0.5f), 9.0f, 1.0f);
    }

    // Engine chip: removed from the bar. The engine in use is shown in the
    // Motor drawer; the top bar keeps only the scale, tempo and grid.
}

void ToolbarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10, 0);
    const int centreY = bounds.getCentreY();
    const int contentH = 34;

    // Logo occupies the same spot as before.
    const int logoRight = 15 + (logoImage.isValid() ? (logoImage.getWidth() + 1) / 2 : 0);
    auto left = bounds.withTrimmedLeft(logoRight + 10);

    // Basic / Advanced: two separate buttons with a visible gap, top-left as
    // decided in gui-decision.md.
    const int modeButtonWidth = (getWidth() < 1100) ? 76 : 82;
    constexpr int modeGap = 8;
    basicModeBounds = {left.getX(), centreY - 13, modeButtonWidth, 26};
    advancedModeBounds = {basicModeBounds.getRight() + modeGap, centreY - 13,
                          modeButtonWidth, 26};
    basicModeButton.setBounds(basicModeBounds);
    advancedModeButton.setBounds(advancedModeBounds);
    left.removeFromLeft(advancedModeBounds.getRight() - left.getX() + 12);

    // No transport strip: REAPER owns playback. The tools take the space
    // that the old play/stop/loop group used to reserve.
    playButton.setBounds({});
    stopButton.setBounds({});
    loopButton.setBounds({});
    followButton.setBounds({});
    auditionButton.setBounds({});
    recordButton.setBounds({});
    left.removeFromLeft(10);

    // Tools sit in the remaining centre.
    int visibleTools = 0;
    for (auto& button : toolButtons)
    {
        const bool advanced = button->getTool() == EditorTool::Formant ||
                              button->getTool() == EditorTool::Amplitude;
        const bool visible = !advanced || uiMode == EditorUiMode::Advanced;
        button->setVisible(visible);
        if (visible)
            ++visibleTools;
    }
    const int toolGroupWidth = visibleTools * kToolSlot +
                               juce::jmax(0, visibleTools - 1) * kToolGap + 16;
    const int rightLimit = bounds.getRight() - 360;
    int toolX = left.getX();
    const int centreCandidate = bounds.getCentreX() - toolGroupWidth / 2;
    if (centreCandidate > left.getX() && centreCandidate + toolGroupWidth < rightLimit)
        toolX = centreCandidate;
    toolX = juce::jmax(toolX, left.getX());
    toolContainerBounds = {toolX, centreY - 19, toolGroupWidth, 38};
    int x = toolContainerBounds.getX() + 8;
    for (auto& button : toolButtons)
    {
        if (!button->isVisible())
            continue;
        button->setBounds(x, centreY - kToolSlot / 2, kToolSlot, kToolSlot);
        x += kToolSlot + kToolGap;
    }

    // Right group: scale, tempo, grid, undo/redo.
    const bool compact = getWidth() < 1000;
    bool showTempo = getWidth() >= 1080;
    const int rightW = getWidth() < 900 ? 250 : (compact ? 300 : 380);
    auto right = bounds.removeFromRight(rightW);
    rightGroupBounds = right;
    const int utilityH = 26;

    int rx = right.getRight() - 8;
    auto placeRight = [&](juce::Component& c, int width) {
        rx -= width;
        c.setBounds(rx, centreY - utilityH / 2, width, utilityH);
        rx -= 6;
    };

    placeRight(redoButton, 26);
    placeRight(undoButton, 26);
    // N8 (D-348): the Clip/Track switch sits next to undo/redo when the host
    // offers track view; the rest of the group does not change.
    if (trackViewAvailable)
        placeRight(trackViewButton, 36);
    // Tarea 2: Bypass a la derecha del grupo escala/tiempo/rejilla. Icono
    // cuadrado estilo Melodyne.
    placeRight(bypassButton, 36);
    placeRight(gridButton, 48);

    tempoButton.setVisible(showTempo);
    if (showTempo)
        placeRight(tempoButton, 58);

    placeRight(scaleButton, compact ? 100 : 124);

    engineButton.setVisible(false);
    engineButton.setBounds({});

    // Old hidden controls and overlays.
    timeLabel.setVisible(false);
    zoomLabel.setVisible(false);
    zoomSlider.setVisible(false);
    timeCapsuleBounds = {};
    transportCapsuleBounds = {};
    leftPanelCapsuleBounds = {};
    parametersButton.setVisible(false);
    leftPanelButton.setVisible(false);
    trackViewButton.setVisible(trackViewAvailable);
    selectModeButton.setVisible(false);
    splitModeButton.setVisible(false);
    pitchDrawingModeButton.setVisible(false);
    timingModeButton.setVisible(false);

    if (showingProgress)
        progressBar.setBounds(toolContainerBounds.getX(),
                              toolContainerBounds.getBottom() - 2,
                              toolContainerBounds.getWidth(), 2);
}

void ToolbarComponent::refreshLocalisedText()
{
    selectModeButton.setTooltip(TR("tooltip.select_tool"));
    splitModeButton.setTooltip(TR("tooltip.split_tool"));
    pitchDrawingModeButton.setTooltip(TR("tooltip.pitch_drawing_tool"));
    timingModeButton.setTooltip(TR("tooltip.timing_tool"));
    followButton.setTooltip(TR("tooltip.auto_scroll"));
    auditionButton.setTooltip(TR("tooltip.live_audition"));
    recordButton.setTooltip(TR("tooltip.capture"));
    playButton.setTooltip(TR("toolbar.play"));
    stopButton.setTooltip(TR("toolbar.stop"));
    loopButton.setTooltip(TR("toolbar.loop"));
    parametersButton.setTooltip(TR("panel.parameters"));
    leftPanelButton.setTooltip(TR("panel.tracks"));
    refreshTrackViewTooltip();
#ifdef JUCE_MAC
    undoButton.setTooltip(TR("command.undo") + " (\u2318Z)");
    redoButton.setTooltip(TR("command.redo") + " (\u21e7\u2318Z)");
#else
    undoButton.setTooltip(TR("command.undo") + " (Ctrl+Z)");
    redoButton.setTooltip(TR("command.redo") + " (Ctrl+Y)");
#endif
    for (auto& button : toolButtons)
        if (button)
            button->refreshTooltip();

    basicModeButton.setButtonText(TR("mode.basic"));
    advancedModeButton.setButtonText(TR("mode.advanced"));
    scaleButton.setButtonText(scaleRootLabel + "  " + scaleModeLabel);
    tempoButton.setButtonText(tempoLabelText);
    gridButton.setButtonText(gridLabelText);
    engineButton.setButtonText(engineLabelText);
    scaleButton.setTooltip(TR("editor.scale"));
    gridButton.setTooltip(TR("editor.grid"));
    tempoButton.setTooltip(TR("editor.tempo"));
    engineButton.setTooltip(TR("panel.rendering"));
    refreshTrackViewTooltip();
    zoomLabel.setText(TR("toolbar.zoom"), juce::dontSendNotification);
    resized();
}

void ToolbarComponent::updateModeButtonStates()
{
    basicModeButton.setToggleState(uiMode == EditorUiMode::Basic,
                                   juce::dontSendNotification);
    advancedModeButton.setToggleState(uiMode == EditorUiMode::Advanced,
                                      juce::dontSendNotification);
    repaint();
}

void ToolbarComponent::updateToolButtonStates()
{
    for (auto& button : toolButtons)
        if (button)
            button->setActiveState(button->getTool() == activeTool);
}

void ToolbarComponent::setBypassState(bool active)
{
    bypassActive = active;
    bypassButton.setToggleState(active, juce::dontSendNotification);
    bypassButton.repaint();
}

// Icono de bypass: el glifo de la referencia (barra gruesa, tallo superior,
// desvio por la derecha y flecha abajo). Sin fondo cuando esta apagado; azul
// de la casa cuando esta activo.
void ToolbarComponent::BypassIconButton::paintButton(juce::Graphics& g, bool over,
                                                     bool down)
{
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    const bool active = getToggleState();
    if (active)
    {
        g.setColour(APP_COLOR_PRIMARY);
        g.fillRoundedRectangle(bounds, 4.0f);
    }
    else if (over || down)
    {
        g.setColour(juce::Colours::white.withAlpha(down ? 0.12f : 0.07f));
        g.fillRoundedRectangle(bounds, 4.0f);
    }

    // Glifo centrado: el eje vertical del tallo, la barra y la flecha es el
    // mismo (x = 0.5) y los tres tramos del corchete (arriba, derecha, abajo)
    // llevan el mismo grosor. El conjunto queda centrado en el boton.
    const auto inner = bounds.reduced(bounds.getWidth() * 0.05f,
                                       bounds.getHeight() * 0.05f);
    const float x = inner.getX();
    const float y = inner.getY();
    const float w = inner.getWidth();
    const float h = inner.getHeight();
    const auto at = [&](float u, float v) {
        return juce::Point<float>(x + u * w, y + v * h);
    };

    g.setColour(APP_COLOR_TEXT_PRIMARY);
    const float stroke = std::max(1.4f, w * 0.08f);

    // Barra (algo mas gruesa que las lineas), centrada con el resto.
    const auto bar = juce::Rectangle<float>(at(0.24f, 0.40f), at(0.62f, 0.58f));
    g.fillRect(bar);

    // Corchete: tallo superior, tramo de arriba, vertical derecha y tramo de
    // abajo, los tres con el mismo grosor.
    juce::Path bracket;
    bracket.startNewSubPath(at(0.50f, 0.08f));
    bracket.lineTo(at(0.50f, 0.30f));
    bracket.lineTo(at(0.74f, 0.30f));
    bracket.lineTo(at(0.74f, 0.70f));
    bracket.lineTo(at(0.50f, 0.70f));
    g.strokePath(bracket, juce::PathStrokeType(stroke,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::butt));

    // Flecha en el mismo eje, separada de la barra.
    const auto arrowStem = juce::Line<float>(at(0.50f, 0.70f), at(0.50f, 0.78f));
    g.drawLine(arrowStem, stroke);
    juce::Path arrow;
    arrow.startNewSubPath(at(0.37f, 0.78f));
    arrow.lineTo(at(0.63f, 0.78f));
    arrow.lineTo(at(0.50f, 0.95f));
    arrow.closeSubPath();
    g.fillPath(arrow);
}

void ToolbarComponent::setUiMode(EditorUiMode mode){
    uiMode = mode;
    updateModeButtonStates();
    resized();
    repaint();
}

void ToolbarComponent::setActiveTool(EditorTool tool)
{
    activeTool = tool;
    updateToolButtonStates();
}

void ToolbarComponent::setScaleSummary(const juce::String& root,
                                       const juce::String& mode)
{
    scaleRootLabel = root;
    scaleModeLabel = mode;
    scaleButton.setButtonText(scaleRootLabel + "  " + scaleModeLabel);
    resized();
}

void ToolbarComponent::setTempoSummary(const juce::String& text)
{
    tempoLabelText = text;
    tempoButton.setButtonText(tempoLabelText);
}

void ToolbarComponent::setGridSummary(const juce::String& text)
{
    gridLabelText = text;
    gridButton.setButtonText(gridLabelText);
}

void ToolbarComponent::setEngineSummary(const juce::String& text)
{
    engineLabelText = text;
    engineButton.setButtonText(engineLabelText);
    resized();
}

void ToolbarComponent::showScaleMenu()
{
    juce::PopupMenu menu;
    juce::PopupMenu roots;
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B"};
    for (int i = 0; i < 12; ++i)
        roots.addItem(100 + i, names[i], true, scaleRootLabel == names[i]);
    menu.addSubMenu(TR("scale.root"), roots);

    juce::PopupMenu modes;
    struct ModeEntry { const char* key; int value; };
    static const ModeEntry modeEntries[] = {
        {"scale.major", 0}, {"scale.minor", 1}, {"scale.blues", 2},
        {"scale.dorian", 3}, {"scale.harmonic_minor", 4},
        {"scale.locrian", 5}, {"scale.lydian", 6},
        {"scale.major_pentatonic", 7}, {"scale.melodic_minor", 8},
        {"scale.minor_pentatonic", 9}, {"scale.mixolydian", 10},
        {"scale.phrygian", 11}, {"scale.phrygian_dominant", 12},
        {"scale.whole_tone", 13}};
    for (const auto& entry : modeEntries)
    {
        const auto label = TR(entry.key);
        modes.addItem(200 + entry.value, label, true, label == scaleModeLabel);
    }
    menu.addSubMenu(TR("scale.mode"), modes);

    // The two switches that used to live in the inspector's Editor Scale card
    // now sit here, with the root and the mode, so the top dropdown is the
    // single place for the editor scale.
    menu.addSeparator();
    menu.addItem(300, TR("inspector.use_project_key"), true,
                 isProjectKeyUsed && isProjectKeyUsed());
    menu.addItem(301, TR("inspector.snap_scale"), true,
                 isSnapToScale && isSnapToScale());

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&scaleButton),
                       [this](int result) {
                           if (result >= 100 && result < 112)
                           {
                               if (onScaleRootSelected)
                                   onScaleRootSelected(result - 100);
                           }
                           else if (result >= 200 && result < 214)
                           {
                               if (onScaleModeSelected)
                                   onScaleModeSelected(result - 200);
                           }
                           else if (result == 300 && onProjectKeyUsedChanged &&
                                    isProjectKeyUsed)
                               onProjectKeyUsedChanged(!isProjectKeyUsed());
                           else if (result == 301 && onSnapToScaleChanged &&
                                    isSnapToScale)
                               onSnapToScaleChanged(!isSnapToScale());
                       });
}

void ToolbarComponent::showGridMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "1/4");
    menu.addItem(2, "1/8");
    menu.addItem(3, "1/16");
    menu.addItem(4, "1/32");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&gridButton),
                       [this](int result) {
                           if (result > 0 && onGridDivisionSelected)
                               onGridDivisionSelected(result - 1);
                       });
}

void ToolbarComponent::buttonClicked(juce::Button* button)
{
    if (button == &recordButton)
    {
        if (onToggleRecord)
            onToggleRecord(recordButton.getToggleState());
    }
    else if (button == &playButton)
    {
        if (isPlaying)
        {
            if (onPause)
                onPause();
        }
        else if (onPlay)
            onPlay();
    }
    else if (button == &stopButton && onStop)
        onStop();
    else if (button == &loopButton)
    {
        loopEnabled = !loopEnabled;
        loopButton.setToggleState(loopEnabled, juce::dontSendNotification);
        if (onToggleLoop)
            onToggleLoop(loopEnabled);
    }
    else if (button == &selectModeButton)
        setEditMode(EditMode::Select);
    else if (button == &splitModeButton)
        setEditMode(EditMode::Split);
    else if (button == &pitchDrawingModeButton)
        setEditMode(EditMode::PitchDrawing);
    else if (button == &timingModeButton)
        setEditMode(EditMode::Timing);
    else if (button == &followButton)
        followPlayback = followButton.getToggleState();
    else if (button == &auditionButton)
    {
        if (onToggleAudition)
            onToggleAudition(auditionButton.getToggleState());
    }
    else if (button == &undoButton && onUndo)
        onUndo();
    else if (button == &redoButton && onRedo)
        onRedo();
    else if (button == &parametersButton)
    {
        parametersVisible = parametersButton.getToggleState();
        if (onToggleParameters)
            onToggleParameters(parametersVisible);
    }
    else if (button == &leftPanelButton)
    {
        if (onToggleLeftPanel)
            onToggleLeftPanel(leftPanelButton.getToggleState());
    }
    else if (button == &trackViewButton)
    {
        refreshTrackViewTooltip();
        if (onTrackViewModeChanged)
            onTrackViewModeChanged(trackViewButton.getToggleState());
    }
}

void ToolbarComponent::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &zoomSlider && onZoomChanged)
        onZoomChanged(static_cast<float>(slider->getValue()));
}

void ToolbarComponent::setPlaying(bool playing)
{
    isPlaying = playing;
    playButton.setToggleState(playing, juce::dontSendNotification);
}

void ToolbarComponent::setTransportEnabled(bool enabled)
{
    transportEnabled = enabled;
    recordButton.setEnabled(enabled);
    const bool hostControls = enabled && hostTransportAvailable;
    stopButton.setEnabled(hostControls);
    playButton.setEnabled(hostControls);
    loopButton.setEnabled(hostControls);
}

void ToolbarComponent::setHostTransportAvailable(bool available)
{
    if (hostTransportAvailable == available)
        return;
    hostTransportAvailable = available;
    if (!available)
    {
        isPlaying = false;
        playButton.setToggleState(false, juce::dontSendNotification);
    }
    setTransportEnabled(transportEnabled);
    repaint();
}

void ToolbarComponent::setToolGroupEnabled(bool enabled)
{
    selectModeButton.setEnabled(enabled);
    splitModeButton.setEnabled(enabled);
    pitchDrawingModeButton.setEnabled(enabled);
    timingModeButton.setEnabled(enabled);
    for (auto& button : toolButtons)
        if (button)
            button->setEnabled(enabled &&
                              !isAdvancedOnlyTool(button->getTool()));
    scaleButton.setEnabled(enabled);
    gridButton.setEnabled(enabled);
}

void ToolbarComponent::setCurrentTime(double time)
{
    currentTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setTotalTime(double time)
{
    totalTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setEditMode(EditMode mode)
{
    currentEditModeInt = static_cast<int>(mode);
    selectModeButton.setToggleState(mode == EditMode::Select,
                                    juce::dontSendNotification);
    splitModeButton.setToggleState(mode == EditMode::Split,
                                   juce::dontSendNotification);
    pitchDrawingModeButton.setToggleState(mode == EditMode::PitchDrawing,
                                          juce::dontSendNotification);
    timingModeButton.setToggleState(mode == EditMode::Timing,
                                    juce::dontSendNotification);
    // Keep the visible Melodyne row in step with shortcut / menu changes.
    const EditorTool mapped = mode == EditMode::Split          ? EditorTool::Separation
                              : mode == EditMode::PitchDrawing ? EditorTool::Pitch
                              : mode == EditMode::Timing       ? EditorTool::Timing
                                                               : EditorTool::Main;
    if (!isAdvancedOnlyTool(activeTool) || uiMode != EditorUiMode::Advanced)
        activeTool = mapped;
    updateToolButtonStates();
}

void ToolbarComponent::setZoom(float pixelsPerSecond)
{
    zoomSlider.setValue(pixelsPerSecond, juce::dontSendNotification);
}

void ToolbarComponent::setLoopEnabled(bool enabled)
{
    loopEnabled = enabled;
    loopButton.setToggleState(loopEnabled, juce::dontSendNotification);
}

void ToolbarComponent::setAuditionEnabled(bool enabled)
{
    auditionButton.setToggleState(enabled, juce::dontSendNotification);
}

void ToolbarComponent::setParametersVisible(bool visible)
{
    parametersVisible = visible;
    parametersButton.setToggleState(parametersVisible, juce::dontSendNotification);
}

void ToolbarComponent::setLeftPanelAvailable(bool available)
{
    if (leftPanelAvailable == available)
        return;
    leftPanelAvailable = available;
    leftPanelButton.setVisible(false);
    resized();
    repaint();
}

void ToolbarComponent::setLeftPanelVisible(bool visible)
{
    leftPanelButton.setToggleState(visible, juce::dontSendNotification);
}

void ToolbarComponent::setTrackViewAvailable(bool available)
{
    if (trackViewAvailable == available)
        return;
    trackViewAvailable = available;
    // N8 (D-348): the Clip/Track switch was unreachable (D-347). Available
    // means visible; resized() places it in the right group when it is.
    trackViewButton.setVisible(available);
    resized();
}

void ToolbarComponent::setTrackViewMode(bool track)
{
    trackViewButton.setToggleState(track, juce::dontSendNotification);
    refreshTrackViewTooltip();
}

void ToolbarComponent::refreshTrackViewTooltip()
{
    trackViewButton.setTooltip(TR(trackViewButton.getToggleState()
                                      ? "tooltip.region_view_track"
                                      : "tooltip.region_view_clip"));
}

void ToolbarComponent::setUndoRedoEnabled(bool undoEnabled, bool redoEnabled)
{
    undoButton.setEnabled(undoEnabled);
    redoButton.setEnabled(redoEnabled);
}

void ToolbarComponent::showProgress(const juce::String& message)
{
    juce::ignoreUnused(message);
    showingProgress = true;
    progressLabel.setVisible(false);
    progressBar.setVisible(true);
    progressValue = -1.0;
    resized();
    repaint();
}

void ToolbarComponent::hideProgress()
{
    showingProgress = false;
    progressLabel.setVisible(false);
    progressBar.setVisible(false);
    resized();
    repaint();
}

void ToolbarComponent::setProgress(float progress)
{
    progressValue = progress < 0 ? -1.0
                                 : static_cast<double>(juce::jlimit(0.0f, 1.0f, progress));
}

void ToolbarComponent::setStatusMessage(const juce::String& message)
{
    juce::ignoreUnused(message);
}

void ToolbarComponent::updateTimeDisplay()
{
    timeLabel.setText(formatTime(currentTime) + " / " + formatTime(totalTime),
                      juce::dontSendNotification);
}

juce::String ToolbarComponent::formatTime(double seconds)
{
    const int mins = static_cast<int>(seconds) / 60;
    const int secs = static_cast<int>(seconds) % 60;
    const int ms = static_cast<int>((seconds - std::floor(seconds)) * 1000);
    return juce::String::formatted("%02d:%02d.%03d", mins, secs, ms);
}

void ToolbarComponent::mouseDown(const juce::MouseEvent& e)
{
#if JUCE_MAC
    if (!pluginMode)
        if (auto* window = getTopLevelComponent())
            dragger.startDraggingComponent(window, e.getEventRelativeTo(window));
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDrag(const juce::MouseEvent& e)
{
#if JUCE_MAC
    if (!pluginMode)
        if (auto* window = getTopLevelComponent())
            dragger.dragComponent(window, e.getEventRelativeTo(window), nullptr);
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
}

void ToolbarComponent::setPluginMode(bool isPlugin)
{
    // The host owns playback in every mode, so the transport stays hidden.
    pluginMode = isPlugin;
    playButton.setVisible(false);
    stopButton.setVisible(false);
    loopButton.setVisible(false);
    followButton.setVisible(false);
    auditionButton.setVisible(false);
    resized();
}

void ToolbarComponent::setRecordControlVisible(bool visible)
{
    recordButton.setVisible(visible);
    if (!visible)
        recordButton.setToggleState(false, juce::dontSendNotification);
    resized();
    repaint();
}

void ToolbarComponent::setRecordArmed(bool armed)
{
    recordButton.setToggleState(armed, juce::dontSendNotification);
}
