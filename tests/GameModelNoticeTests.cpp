// V6: la falta de modelos GAME es un aviso del editor, no un dialogo modal.
//
// Criterio escrito antes de medir:
// - El estado que publica segmentIntoNotes dice "hay un problema" y lleva el
//   detalle (rutas y fichero por fichero) para el log y el tooltip.
// - Despues de drenar el message loop no hay ningun componente modal ni
//   DialogWindow en el escritorio: este camino ya no abre ventanas.
// - El estado se entrega una sola vez por intento (el timer no repite el aviso).

#include "../Source/Audio/EditorController.h"
#include "../Source/Audio/GameModelsNotice.h"
#include "../Source/Models/Project.h"
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

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("GameModelNoticeTests");
    root.deleteRecursively();
    root.createDirectory();

    // 1. La comprobacion de ficheros nombra los cinco requeridos.
    {
        const auto empty = root.getChildFile("empty");
        empty.createDirectory();
        const auto files = gameModels::checkRequiredFiles(empty);
        CHECK(!files.allPresent);
        CHECK(files.presentCount == 0);
        CHECK(files.missingCount == 5);

        const auto full = root.getChildFile("full");
        full.createDirectory();
        for (const auto *name : gameModels::requiredFiles)
            CHECK(full.getChildFile(name).replaceWithText("x"));
        const auto complete = gameModels::checkRequiredFiles(full);
        CHECK(complete.allPresent);
        CHECK(complete.missingCount == 0);

        // El detalle repite lo que decia el modal, sin abrir nada.
        const auto detail = gameModels::describeUnavailable(
            empty, full, true, false, "GAME models were not found.");
        CHECK(detail.contains("GAME models were not found."));
        CHECK(detail.contains(empty.getFullPathName()));
        CHECK(detail.contains(full.getFullPathName()));
        CHECK(detail.contains("encoder.onnx: MISSING"));
        CHECK(detail.contains("config.json: MISSING"));
        CHECK(detail.contains("isLoaded: false"));
    }

    // 2. El camino real de segmentacion publica el aviso y no abre dialogos.
    {
        EditorController controller(false);

        Project project;
        auto &audio = project.getAudioData();
        audio.sampleRate = 44100.0;
        audio.waveform.setSize(1, 4410);
        audio.waveform.clear();
        audio.f0.assign(400, 220.0f);

        controller.segmentIntoNotes(project);
        // Drena cualquier callAsync que el camino viejo hubiera encolado.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(60);

        CHECK(juce::Component::getNumCurrentlyModalComponents() == 0);
        CHECK(!anyDialogOnDesktop());

        EditorController::GameModelsNotice notice;
        CHECK(controller.takeGameModelsNotice(notice));
        CHECK(notice.missing);
        CHECK(notice.summary.isNotEmpty());
        CHECK(notice.detail.contains("encoder.onnx"));

        // Un intento = un aviso. El timer no lo pinta una y otra vez.
        EditorController::GameModelsNotice again;
        CHECK(!controller.takeGameModelsNotice(again));
    }

    root.deleteRecursively();
    std::cout << "GameModelNoticeTests passed\n";
    return 0;
}
