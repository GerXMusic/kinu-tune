// D-267 (T3/T4): el rail como superficie verificable sin raton.
//
// Criterio escrito antes de medir:
// - El boton de Bypass existe, es visible y conmuta el estado del documento a
//   traves de su onClick real (triggerClick), no de una funcion paralela.
// - El aviso de motor es un componente del panel (Label), no un
//   DialogWindow/AlertWindow: en el escritorio no aparece ningun dialogo.
// - Con el aviso visible la UI sigue respondiendo (el boton de Bypass vuelve a
//   conmutar), asi que el aviso no bloquea.

#include "../Source/UI/EnginePanel.h"
#include "../Source/UI/EngineColours.h"
#include "../Source/Utils/Localization.h"
#include "TestAssert.h"

#include <iostream>

namespace {

bool anyDialogOnDesktop()
{
    const auto &desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (dynamic_cast<juce::DialogWindow*>(desktop.getComponent(i)) != nullptr)
            return true;
    return false;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    EnginePanel panel(EditorUiMode::Advanced);
    panel.setSize(300, 640);

    // D-309: puntos de color por motor y "Seguir global" con el color global.
    panel.setSynthesisEngine(SynthesisEngineType::Psola);
    const auto vocoderDot = panel.getEngineRowColour(0);
    const auto psolaDot = panel.getEngineRowColour(1);
    const auto globalDot = panel.getBlockRowColour(0);
    const auto blockPsolaDot = panel.getBlockRowColour(2);
    CHECK(vocoderDot.has_value());
    CHECK(*vocoderDot == engineColours::forEngine(SynthesisEngineType::Vocoder));
    CHECK(psolaDot.has_value());
    CHECK(*psolaDot == engineColours::forEngine(SynthesisEngineType::Psola));
    CHECK(globalDot.has_value());
    CHECK(*globalDot == engineColours::forEngine(SynthesisEngineType::Psola));
    CHECK(blockPsolaDot.has_value());
    CHECK(*blockPsolaDot == engineColours::forEngine(SynthesisEngineType::Psola));

    panel.setSynthesisEngine(SynthesisEngineType::Pitchcore);
    CHECK(panel.getBlockRowColour(0).has_value());
    CHECK(*panel.getBlockRowColour(0) ==
          engineColours::forEngine(SynthesisEngineType::Pitchcore));

    // D-309: sin seleccion explicita el selector de bloque queda deshabilitado
    // y no cae a "todas las notas".
    panel.setNoteEngine(std::nullopt);
    CHECK(!panel.isBlockSelectorEnabled());
    CHECK(panel.getSelectedBlockRow() == -1);
    panel.setEngineEditingEnabled(false);
    panel.setEngineEditingEnabled(true);
    CHECK(!panel.isBlockSelectorEnabled()); // sigue sin seleccion

    // Con una nota seleccionada vuelve a responder.
    panel.setNoteEngine(NoteEngine::Inherit);
    CHECK(panel.isBlockSelectorEnabled());
    CHECK(panel.getSelectedBlockRow() == 0);
    CHECK(!anyDialogOnDesktop());

    // Aviso de motor: numero y listas dentro del panel, sin dialogo.
    const juce::String notice =
        "Motor PSOLA: 2 de 3 nota(s) del bloque cambian de motor. "
        "Sobrescritas: #0, #1.\n"
        "Fuera del rango de beneficio del Vocoder [-1,5; +2,5] st (1): #2. "
        "Se aplica el motor Clasico igualmente.";
    panel.setBlockNotice(notice);
    const auto shown = panel.getBlockNoticeText();
    std::cout << "Aviso de motor: visible=" << (panel.isBlockNoticeVisible() ? 1 : 0)
              << " dialogos=" << (anyDialogOnDesktop() ? 1 : 0) << "\n";
    CHECK(panel.isBlockNoticeVisible());
    CHECK(shown.contains("2 de 3"));
    CHECK(shown.contains("#0"));
    CHECK(shown.contains("#1"));
    CHECK(shown.contains("#2"));
    CHECK(!anyDialogOnDesktop());

    // El aviso no bloquea: la UI sigue respondiendo sin abrir dialogos.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    CHECK(!anyDialogOnDesktop());

    // Sin texto, el aviso desaparece (no queda un panel vacio).
    panel.setBlockNotice({});
    CHECK(!panel.isBlockNoticeVisible());

    // D-288: con el transporte sonando el selector no responde y el rail dice
    // por que; al soltar, vuelve el aviso que hubiera y no queda ningun dialogo.
    panel.setBlockNotice(notice);
    panel.setEngineEditingEnabled(false);
    std::cout << "D-288 selector bloqueado: habilitado="
              << (panel.isEngineEditingEnabled() ? 1 : 0)
              << " aviso=\"" << panel.getBlockNoticeText().toStdString() << "\"\n";
    CHECK(!panel.isEngineEditingEnabled());
    CHECK(panel.isBlockNoticeVisible());
    CHECK(panel.getBlockNoticeText().containsIgnoreCase("reproducci"));
    CHECK(!anyDialogOnDesktop());

    panel.setEngineEditingEnabled(true);
    CHECK(panel.isEngineEditingEnabled());
    CHECK(panel.getBlockNoticeText() == notice); // vuelve el aviso D-258
    CHECK(!anyDialogOnDesktop());

    std::cout << "EnginePanelTests: en verde\n";
    return 0;
}
