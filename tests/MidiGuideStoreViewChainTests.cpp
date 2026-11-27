// R11f (guia MIDI, D-370): una sola ruta de sidecar por pista, estable ante
// cualquier cambio de vista del documento.
//
// R12 midio con la build B2 (E2C56ECF) y desde proyecto limpio que la guia
// aguantaba el PRIMER cambio de vista del documento (1 guia / 111 fantasmas) y
// se perdia en el SEGUNDO (0/0), en dos cadenas independientes (4->1 y 1->2);
// `sync_midi_guides` la recuperaba, asi que el fichero bueno seguia en disco.
// Causa: `fileFor()` hasheaba el fingerprint de fuentes del documento ademas de
// la clave de pista, de modo que cada vista escribia y buscaba OTRO fichero: la
// guia "saltaba" y el segundo cambio caia en uno vacio. R11e habia pasado solo
// porque su directorio ya tenia un fichero por vista.
//
// Aqui se mide el contrato nuevo, sin ARA (no hay host en un test):
//  (a) desde estado limpio, un sync y CINCO cambios de vista seguidos: siempre
//      1 guia / 111 fantasmas, un solo fichero vivo y ningun `.bak`;
//  (b) las cadenas de R12 (4->1->2 y 1->2->1), con store nuevo en cada parada
//      como el editor que se reabre, siguen leyendo la guia;
//  (c) dos vistas simultaneas no crean dos ficheros;
//  (d) varios ficheros vivos de la misma pista (los de R11d/R11e, uno por
//      vista) se consolidan en uno: los demas a `.bak`, sin borrar datos;
//  (e) guardar en mitad de la cadena - y guardar sin guia en memoria - no
//      retira el sidecar bueno.
// Fixture = la guia Lead real medida en R5 (13,704374 s / 111 notas).
#include "TestAssert.h"

#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/Models/DocumentMidiGuides.h"
#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/Models/MidiGuidePersistence.h"
#include "../Source/Models/TrackIdentity.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr double kGuideHostStart = 13.704374;
constexpr int kLeadNotes = 111;
constexpr const char* kTrackName = "v2 - stem";

HostTempoMap measuredTempoMap()
{
    return HostTempoMap::fromEntries(
        {{13.704374, 32.0},
         {30.133561, 64.0},
         {48.319375, 100.0},
         {85.120996, 170.0},
         {100.499335, 200.0}},
        {{13.704374, 3, 4}});
}

GuideSource leadGuide(const char* id = "reaper:lead")
{
    GuideSource guide;
    guide.id = id;
    guide.label = "Lead";
    guide.kind = "companion";
    guide.importedAt = "2026-11-27T12:00:00Z";
    const double span = 151.0 - 32.0;
    guide.notes.reserve(kLeadNotes);
    for (int i = 0; i < kLeadNotes; ++i)
    {
        GuideNote note;
        note.startQuarter = 32.0 + span * static_cast<double>(i) / kLeadNotes;
        note.endQuarter = note.startQuarter + 0.75;
        note.pitch = 53.0f + static_cast<float>(i % 22);
        note.velocity = 0.8f;
        guide.notes.push_back(note);
    }
    return guide;
}

juce::File testDirectory()
{
    static const auto dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("tunex-midi-guide-viewchain-"
                          + juce::String::toHexString(
                                juce::Random::getSystemRandom().nextInt64()));
    dir.createDirectory();
    return dir;
}

juce::File freshDirectory(const juce::String& name)
{
    const auto dir = testDirectory().getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

int ghostCount(const DocumentMidiGuideStore& store)
{
    const auto* guide = store.at(0);
    if (guide == nullptr)
        return 0;
    return static_cast<int>(midiGuideGhostsFor(*guide, measuredTempoMap()).size());
}

int liveSidecars(const juce::File& directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles,
                                           "guide-*.json");
}

int backupSidecars(const juce::File& directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles, "*.bak");
}

// La vista del documento que produce un proyecto con n fuentes: es lo unico
// que cambia entre los reabrir del editor de R12 (el conjunto de nombres de las
// fuentes del documento). La clave de pista NO cambia.
juce::String viewKey(int sources)
{
    std::vector<juce::String> names;
    for (int i = 1; i <= sources; ++i)
        names.push_back("name:take-" + juce::String(i) + ".wav");
    return midiGuideDisk::projectKeyFromSources(names);
}

// Un fichero de una vista anterior: clave de pista canonica ("name:...") pero
// escrito en la ruta por vista de R11d/R11e. Se fabrica con el serializador
// real en otra carpeta y se copia con un nombre aleatorio, que es exactamente
// lo que dejo la build B2 en el directorio del usuario.
juce::File writePerViewFile(const juce::File& directory,
                            const juce::String& perViewKey,
                            const juce::String& trackKey,
                            const std::vector<GuideSource>& guides)
{
    const auto scratch = testDirectory().getChildFile("scratch");
    scratch.deleteRecursively();
    scratch.createDirectory();
    MidiGuidePersistence writer(scratch);
    CHECK(writer.save(perViewKey, trackKey, guides));
    const auto written =
        scratch.getNumberOfChildFiles(juce::File::findFiles, "guide-*.json") == 1;
    CHECK(written);
    const auto source = scratch.findChildFiles(juce::File::findFiles, false,
                                               "guide-*.json")
                            .getFirst();
    const auto copy = directory.getChildFile(
        "guide-" +
        juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())
            .toLowerCase() +
        ".json");
    CHECK(source.copyFileTo(copy));
    scratch.deleteRecursively();
    return copy;
}

// (a) Desde estado limpio (sin sidecar): un sync y cinco cambios de vista
// seguidos. Cada parada tiene que ver la guia y un solo fichero vivo.
void testFiveViewChangesFromCleanKeepTheGuide()
{
    const auto directory = freshDirectory("chain");
    const auto trackKey = trackKeyForTrack(kTrackName, 0);
    CHECK(liveSidecars(directory) == 0); // estado limpio de verdad

    MidiGuidePersistence persistence(directory);
    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(viewKey(4));
    store.bindTrack(trackKey);
    CHECK(store.empty());

    // El sync de la guia viva (una vez) y el guardado del host.
    CHECK(store.syncLive({leadGuide()}));
    CHECK(store.size() == 1);
    CHECK(store.flushPersistence());
    CHECK(liveSidecars(directory) == 1);
    CHECK(ghostCount(store) == kLeadNotes);

    // Cinco cambios de vista del documento seguidos (el segundo era el que
    // dejaba 0/0 en R12). La pista es la misma: no hay nada que recargar.
    const int views[5] = {2, 4, 2, 4, 1};
    for (const int sources : views)
    {
        store.setDocumentKey(viewKey(sources));
        CHECK(store.size() == 1);
        CHECK(ghostCount(store) == kLeadNotes);
        CHECK(store.at(0) != nullptr);
        CHECK(store.at(0)->notes.size() ==
              static_cast<std::size_t>(kLeadNotes));
        CHECK(liveSidecars(directory) == 1);
        // La guia no se mueve ni se retira: sigue el mismo fichero.
        CHECK(backupSidecars(directory) == 0);
    }

    // Y una reapertura lee el mismo fichero con las 111 notas.
    MidiGuidePersistence reopenedPersistence(directory);
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&reopenedPersistence);
    reopened.setDocumentKey(viewKey(2));
    reopened.bindTrack(trackKey);
    CHECK(reopened.size() == 1);
    CHECK(reopened.isFromDisk());
    CHECK(ghostCount(reopened) == kLeadNotes);
    if (reopened.at(0) != nullptr)
        CHECK(std::abs(midiGuideGhostsFor(*reopened.at(0), measuredTempoMap())
                           .front()
                           .startHostSeconds -
                       kGuideHostStart) <= 1.0e-9);
}

// (b) Las dos cadenas medidas en R12, parada a parada y con un store nuevo en
// cada una (el editor se reabre): 4->1->2 y 1->2->1.
void testMeasuredChainsSurviveEveryStop()
{
    const auto directory = freshDirectory("chains-r12");
    const auto trackKey = trackKeyForTrack(kTrackName, 0);

    {
        MidiGuidePersistence persistence(directory);
        DocumentMidiGuideStore first;
        first.setPersistence(&persistence);
        first.setDocumentKey(viewKey(4));
        first.bindTrack(trackKey);
        CHECK(first.syncLive({leadGuide()}));
        CHECK(first.flushPersistence());
    }

    auto reopenAndCheck = [&directory, &trackKey](int sources) {
        MidiGuidePersistence persistence(directory);
        DocumentMidiGuideStore store;
        store.setPersistence(&persistence);
        store.setDocumentKey(viewKey(sources));
        store.bindTrack(trackKey);
        CHECK(store.size() == 1);
        CHECK(ghostCount(store) == kLeadNotes);
        CHECK(liveSidecars(directory) == 1);
    };

    // Cadena A de R12: 4 -> 1 -> 2.
    reopenAndCheck(1);
    reopenAndCheck(2);
    // Cadena B de R12: 1 -> 2 -> 1 (otra vez la vista de una fuente).
    reopenAndCheck(1);
    reopenAndCheck(2);
    reopenAndCheck(1);

    CHECK(backupSidecars(directory) == 0);
}

// (c) Dos vistas del documento a la vez (dos editores del mismo proyecto, o el
// documento publicado dos veces) no crean dos ficheros ni se pisan la guia.
void testTwoSimultaneousViewsUseOneFile()
{
    const auto directory = freshDirectory("simultaneous");
    const auto trackKey = trackKeyForTrack(kTrackName, 0);

    MidiGuidePersistence persistenceA(directory);
    MidiGuidePersistence persistenceB(directory);
    DocumentMidiGuideStore storeA;
    DocumentMidiGuideStore storeB;
    storeA.setPersistence(&persistenceA);
    storeB.setPersistence(&persistenceB);
    storeA.setDocumentKey(viewKey(4));
    storeB.setDocumentKey(viewKey(1));
    storeA.bindTrack(trackKey);
    storeB.bindTrack(trackKey);

    CHECK(storeA.syncLive({leadGuide()}));
    CHECK(storeA.size() == 1);
    CHECK(storeB.syncLive({leadGuide()})); // la otra vista ve la misma guia
    CHECK(storeB.size() == 1);
    CHECK(storeA.flushPersistence());
    CHECK(storeB.flushPersistence());

    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 0);
    CHECK(ghostCount(storeA) == kLeadNotes);
    CHECK(ghostCount(storeB) == kLeadNotes);
    CHECK(persistenceA.fileFor(trackKey) == persistenceB.fileFor(trackKey));
}

// (d) Varios ficheros vivos de la misma pista (los que R11d/R11e dejaron, uno
// por vista, con la clave canonica pero en rutas distintas): se consolidan en
// UNO y los demas quedan como `.bak`. Nada se borra.
void testSeveralLiveFilesAreConsolidated()
{
    const auto directory = freshDirectory("consolidate");
    const auto trackKey = trackKeyForTrack(kTrackName, 0);

    const auto one = writePerViewFile(directory, viewKey(4), trackKey,
                                      {leadGuide()});
    const auto two = writePerViewFile(directory, viewKey(1), trackKey,
                                      {leadGuide()});
    const auto three = writePerViewFile(directory, viewKey(2), trackKey,
                                        {leadGuide()});
    CHECK(one.existsAsFile() && two.existsAsFile() && three.existsAsFile());
    CHECK(liveSidecars(directory) == 3);

    MidiGuidePersistence persistence(directory);
    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(viewKey(2)); // la vista que reabre R12
    store.bindTrack(trackKey);

    // Las tres vistas tenian la MISMA guia (mismo id): una sola, no tres.
    CHECK(store.size() == 1);
    CHECK(ghostCount(store) == kLeadNotes);
    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 3);
    CHECK(!one.existsAsFile() && !two.existsAsFile() && !three.existsAsFile());
    CHECK(persistence.fileFor(trackKey).existsAsFile());

    // La consolidacion no se repite ni duplica en la siguiente apertura.
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&persistence);
    reopened.setDocumentKey(viewKey(4));
    reopened.bindTrack(trackKey);
    CHECK(reopened.size() == 1);
    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 3);
}

// (e) Guardar en mitad de la cadena - y guardar sin guia en memoria - no retira
// el sidecar bueno a `.bak`: primero se adopta, y solo se escribe si hay guia.
void testSavingMidChainKeepsTheGoodSidecar()
{
    const auto directory = freshDirectory("midchain");
    const auto trackKey = trackKeyForTrack(kTrackName, 0);

    MidiGuidePersistence persistence(directory);
    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(viewKey(4));
    store.bindTrack(trackKey);
    CHECK(store.syncLive({leadGuide()}));
    CHECK(store.flushPersistence());
    const auto goodFile = persistence.fileFor(trackKey);
    CHECK(goodFile.existsAsFile());

    // Un store que aun no conoce la vista (bindTrack sin clave de documento) no
    // tiene la guia en memoria: un guardado del host en ese estado NO puede
    // escribir un fichero vacio ni retirar el bueno.
    DocumentMidiGuideStore keyless;
    keyless.setPersistence(&persistence);
    keyless.bindTrack(trackKey);
    CHECK(keyless.empty());
    CHECK(!keyless.flushPersistence());
    CHECK(goodFile.existsAsFile());
    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 0);

    // La vista llega: adopta el fichero bueno (R12: era el fallo del segundo
    // cambio de vista).
    keyless.setDocumentKey(viewKey(1));
    CHECK(keyless.size() == 1);
    CHECK(ghostCount(keyless) == kLeadNotes);

    // Guardado del host en mitad de la cadena: mismo fichero, sin `.bak`.
    CHECK(keyless.flushPersistence());
    keyless.setDocumentKey(viewKey(2));
    CHECK(keyless.flushPersistence());
    CHECK(keyless.size() == 1);
    CHECK(ghostCount(keyless) == kLeadNotes);
    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 0);

    // Y el fichero sigue siendo legible con las 111 notas.
    const auto loaded = persistence.load(viewKey(2), trackKey);
    CHECK(loaded.usable());
    CHECK(loaded.guides.size() == 1);
    CHECK(loaded.guides.front().notes.size() ==
          static_cast<std::size_t>(kLeadNotes));
}
} // namespace

int main()
{
    testFiveViewChangesFromCleanKeepTheGuide();
    testMeasuredChainsSurviveEveryStop();
    testTwoSimultaneousViewsUseOneFile();
    testSeveralLiveFilesAreConsolidated();
    testSavingMidChainKeepsTheGoodSidecar();

    testDirectory().deleteRecursively();
    std::printf("MidiGuideStoreViewChainTests: all checks passed\n");
    return 0;
}
