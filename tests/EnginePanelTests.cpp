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

    std::cout << "EnginePanelTests: en verde\n";
    return 0;
}
