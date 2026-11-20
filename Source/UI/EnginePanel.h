#pragma once

#include "../JuceHeader.h"
#include "GuiTypes.h"
#include "../Audio/SynthesisEngineType.h"
#include "../Models/Note.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include <functional>
#include <optional>
#include <vector>

/**
 * Engine drawer (gui-decision.md §2): the project engine is always editable,
 * the per-block engine only in Advanced. The row shows what the block really
 * renders with, including a D-129 refusal, so the picker never lies.
 */
class EnginePanel : public juce::Component, private juce::Timer
{
public:
    explicit EnginePanel(EditorUiMode mode = EditorUiMode::Basic);
    ~EnginePanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void refreshLocalisedText();

    void setEditorMode(EditorUiMode mode);
    void setSynthesisEngine(SynthesisEngineType engine);
    void setNoteEngine(std::optional<NoteEngine> engine);
    void setEngineEffective(SynthesisEngineType engine,
                            std::optional<NoteEngine> refused);
    void setAiResynthesisAvailable(bool available);
    void setDeviceOptions(const juce::StringArray& names, int selectedIndex);
    void setRenderStatusText(const juce::String& text);

    std::function<void(SynthesisEngineType)> onSynthesisEngineChanged;
    std::function<void(NoteEngine)> onNoteEngineChanged;
    std::function<void(int)> onDeviceChanged;
    std::function<std::optional<NoteEngine>()> getNoteEngine;

private:
    class ChoiceList final : public juce::Component
    {
    public:
        void setOptions(std::vector<juce::String> labels);
        void setSelected(int index, bool notify);
        void setRowEnabled(int index, bool enabled);
        int getSelected() const { return selected; }
        int getPreferredHeight() const;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;
        std::function<void(int)> onChange;

    private:
        int rowAt(juce::Point<int> p) const;
        std::vector<juce::String> labels;
        std::vector<bool> rowEnabled;
        int selected = -1;
        int hovered = -1;
    };

    void timerCallback() override;
    void applyModeVisibility();

    EditorUiMode editorMode = EditorUiMode::Basic;
    bool aiAvailable = true;

    juce::Label engineTitle;
    juce::Label engineHint;
    ChoiceList engineChoices;

    juce::Label blockTitle;
    juce::Label blockHint;
    ChoiceList blockChoices;
    juce::Label blockStatus;

    juce::Label deviceTitle;
    juce::TextButton deviceButton;

    juce::Rectangle<int> engineCard;
    juce::Rectangle<int> blockCard;
    juce::Rectangle<int> deviceRow;
    juce::StringArray deviceNames;
    int deviceIndex = 0;
    juce::String renderStatus;
    bool hasDeviceRow = false;

    LocalisationWatcher languageWatcher{[this] {
        refreshLocalisedText();
        resized();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnginePanel)
};
