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

    // D-267 (aviso de motor, sin dialogo modal): panel de texto del rail con
    // el numero y la lista de D-258. Vacio = no se pinta.
    void setBlockNotice(const juce::String& text);
    juce::String getBlockNoticeText() const { return blockNotice.getText(); }
    bool isBlockNoticeVisible() const { return blockNotice.isVisible(); }

    // D-288: bloqueo del selector de motor mientras el transporte suena. El
    // aviso del rail dice por que, sin dialogo (D-263: un AlertWindow cerro
    // REAPER). Al desbloquear se restaura el aviso D-258 que hubiera.
    void setEngineEditingEnabled(bool enabled);
    bool isEngineEditingEnabled() const { return engineEditingEnabled; }

    // Tarea 1 (2026-11-22): comprobacion del alineado del cajon. Devuelve los
    // rectangulos de todos los controles y si los rotulos van a la izquierda.
    juce::Array<juce::Rectangle<int>> getAlignedControlBounds() const;
    bool areHeadersLeftAligned() const;

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
        // D-288: con el transporte sonando no se puede cambiar de motor (un
        // cambio es un re-render de todo el proyecto y publicaria una mezcla de
        // dos motores; medido en vivo: 10 notas en Pitchcore y el resto en
        // Vocoder). La fila se ve igual; solo deja de responder.
        void setInteractionEnabled(bool enabled) { interactionEnabled = enabled; }
        bool isInteractionEnabled() const { return interactionEnabled; }
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
        bool interactionEnabled = true;
    };

    void timerCallback() override;
    void applyModeVisibility();
    void applyNotice();

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

    juce::Label blockNotice;

    juce::Rectangle<int> engineCard;
    juce::Rectangle<int> blockCard;
    juce::Rectangle<int> deviceRow;
    juce::Rectangle<int> noticeCard;
    juce::StringArray deviceNames;
    int deviceIndex = 0;
    juce::String renderStatus;
    bool hasDeviceRow = false;
    // D-288
    bool engineEditingEnabled = true;
    juce::String pendingBlockNotice;

    LocalisationWatcher languageWatcher{[this] {
        refreshLocalisedText();
        resized();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnginePanel)
};
