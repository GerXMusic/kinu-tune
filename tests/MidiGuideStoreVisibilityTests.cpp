// V3 (guia MIDI, D-374): carga y refresco deterministas.
//
// V1 midio que el recorte de tiempo era el bug de DIBUJO. V3 separa ese fallo
// del de CARGA: "no esta en vista" (tiempo/altura) no es "no esta cargada"
// (store vacio o vista sin refrescar). Se mide, sin host:
//  (a) sidecar cargado (origin document, kind reascript) + scan autoritativo
//      vacio -> sigue 1/111; no se escribe sidecar vacio ni se retira el bueno
//      a .bak;
//  (b) una guia que aporto syncLive se elimina cuando deja de aparecer (D-363);
//      una que solo entro por un scan NO autoritativo no la retira el primer
//      scan autoritativo que no la ve;
//  (c) setDocumentKey tardio: la vista se queda atras hasta que el documento
//      llama midiGuidesChanged() UNA vez; el refresco no escribe en el store
//      (generation no cambia), asi que no puede haber bucle;
//  (d) cadena 4->1->2->4 con store nuevo en cada parada y SIN sync: 111 en
//      todas (el sidecar es por pista, D-370).
#include "TestAssert.h"

#include "../Source/Models/DocumentMidiGuides.h"
#include "../Source/Models/MidiGuidePersistence.h"
#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/UI/MainComponent.h"

#include <cstdio>
#include <string>
#include <vector>

// Friend of MainComponent (see there): the editor without a host around it.
struct MainComponentTestAccess
{
    MainComponent &mc;

    DocumentMidiGuidesPtr snapshot() const { return mc.documentMidiGuides; }
    int selected() const { return mc.selectedMidiGuide; }
    void attach(DocumentMidiGuideStore *store) { mc.setMidiGuideStore(store); }
    void notify() { mc.midiGuidesChanged(); }
};

namespace
{
constexpr double kGuideHostStart = 13.704374;
constexpr int kLeadNotes = 111;
constexpr const char* kTrackKey = "track:tunex-v2-2";
constexpr const char* kDocKey = "view:lead-stem";

GuideSource leadGuide()
{
    GuideSource guide;
    guide.id = "reaper:lead-live";
    guide.label = "Lead";
    guide.kind = "reascript"; // what the live ReaScript writes
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
            .getChildFile("tunex-midi-guide-store-visibility-" +
                          juce::String::toHexString(
                              juce::Random::getSystemRandom().nextInt64()));
    dir.createDirectory();
    return dir;
}

// Cada prueba escribe en su propia carpeta: un sidecar de una no puede tapar
// la carga de la siguiente.
juce::File freshDirectory(const juce::String &name)
{
    const auto dir = testDirectory().getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

int fileCount(const juce::File &directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles, "*");
}

// Escribe el sidecar por el camino real (syncLive autoritativo persiste).
void seedSidecar(MidiGuidePersistence &persistence)
{
    DocumentMidiGuideStore seed;
    seed.setPersistence(&persistence);
    seed.setDocumentKey(kDocKey);
    seed.bindTrack(kTrackKey);
    CHECK(seed.syncLive({leadGuide()}, true));
    CHECK(seed.size() == 1);
    CHECK(persistence.load(kDocKey, kTrackKey).guides.size() == 1);
}

// (a) Sidecar + scan autoritativo vacio: la guia NO se pierde.
void testSidecarSurvivesEmptyAuthoritativeScan()
{
    MidiGuidePersistence persistence(freshDirectory("a"));
    seedSidecar(persistence);

    const auto file = persistence.fileFor(kTrackKey);
    CHECK(file.existsAsFile());
    const auto bytesBefore = file.loadFileAsString();
    const auto filesBefore = fileCount(persistence.getDirectory());

    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(kDocKey);
    store.bindTrack(kTrackKey);
    CHECK(store.size() == 1);
    CHECK(store.isFromDisk());
    CHECK(store.isDiskAuthoritative());
    const auto loaded = store.publish();
    CHECK(loaded != nullptr);
    CHECK(loaded->guideCount() == 1);
    CHECK(loaded->totalNotes == kLeadNotes);
    CHECK(loaded->guides.front().origin == "document");

    // The folder exists but has no file: authoritative, yet no scan ever
    // contributed this id, so nothing is removed (V3/D-374).
    CHECK(!store.syncLive({}, true));
    CHECK(store.size() == 1);
    const auto after = store.publish();
    CHECK(after != nullptr);
    CHECK(after->totalNotes == kLeadNotes);
    CHECK(store.findById("reaper:lead-live") != nullptr);

    // No empty sidecar write and no .bak of the good file.
    CHECK(file.loadFileAsString() == bytesBefore);
    CHECK(fileCount(persistence.getDirectory()) == filesBefore);
    const auto reloaded = persistence.load(kDocKey, kTrackKey);
    CHECK(reloaded.usable());
    CHECK(reloaded.guides.size() == 1);
    CHECK(reloaded.guides.front().notes.size() ==
          static_cast<std::size_t>(kLeadNotes));
    std::printf(
        "sidecar: before=%d files, after=%d files, notes=%d origin=%s\n",
        filesBefore, fileCount(persistence.getDirectory()), kLeadNotes,
        loaded->guides.front().origin.toRawUTF8());
}

// (b) Lo que aporto un scan se va cuando su fichero desaparece; lo que no
// aporto ningun scan, no.
void testOnlyScanContributedGuidesAreRemoved()
{
    DocumentMidiGuideStore store;
    store.bindTrack("track:test");

    auto imported = leadGuide();
    imported.id = "file:abcdef";
    imported.kind = "file";
    store.upsert(imported, "file");

    auto live = leadGuide();
    live.id = "live:one";
    CHECK(store.syncLive({live}, true)); // the scan vouches for it
    CHECK(store.size() == 2);
    CHECK(store.getLastLiveIds().size() == 1);

    // The file disappeared: the guide goes with it (D-363).
    CHECK(store.syncLive({}, true));
    CHECK(store.size() == 1);
    CHECK(store.findById("live:one") == nullptr);
    CHECK(store.findById("file:abcdef") != nullptr);

    // A guide that only a non-authoritative scan added has not been vouched
    // for; the next authoritative scan cannot retire it (V3/D-374).
    auto late = leadGuide();
    late.id = "live:late";
    CHECK(store.syncLive({late}, false));
    CHECK(store.size() == 2);
    CHECK(store.getLastLiveIds().empty());
    CHECK(!store.syncLive({}, true));
    CHECK(store.size() == 2);
    CHECK(store.findById("live:late") != nullptr);

    // Once an authoritative scan sees it, its disappearance removes it (the
    // content did not change, so the scan itself reports no change; lastLiveIds
    // is what starts vouching for it).
    store.syncLive({late}, true);
    CHECK(store.getLastLiveIds().size() == 1);
    CHECK(store.syncLive({}, true));
    CHECK(store.findById("live:late") == nullptr);
    CHECK(store.size() == 1);
    std::printf("sync: scan-contributed removed, scan-less kept, file kept\n");
}

// (c) setDocumentKey tardio: el store carga y la vista no; una notificacion la
// pone al dia sin escribir en el store.
void testLateDocumentKeyNotifiesOnce()
{
    MidiGuidePersistence persistence(freshDirectory("c"));
    seedSidecar(persistence);

    // The editor opened before the document had a fingerprint: no load yet.
    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.bindTrack(kTrackKey);
    CHECK(store.size() == 0);

    {
        MainComponent mc(false);
        mc.setSize(1000, 400);
        MainComponentTestAccess access{mc};
        access.attach(&store);
        CHECK(access.snapshot() != nullptr);
        CHECK(access.snapshot()->guideCount() == 0); // "not loaded" in the view

        // The fingerprint arrives after the panel is already open.
        store.setDocumentKey(kDocKey);
        CHECK(store.size() == 1);
        const auto storeGeneration = store.generation();
        CHECK(access.snapshot()->guideCount() == 0); // view still behind

        // ONE notification from the document controller: the view republishes
        // exactly what the store holds.
        access.notify();
        CHECK(access.snapshot() != nullptr);
        CHECK(access.snapshot()->guideCount() == 1);
        CHECK(access.snapshot()->totalNotes == kLeadNotes);
        CHECK(access.selected() == 0);
        CHECK(store.generation() == storeGeneration); // no write-back, no loop

        // Idempotent: notifying again does not mutate the store either.
        access.notify();
        CHECK(access.snapshot()->guideCount() == 1);
        CHECK(store.size() == 1);
        CHECK(store.generation() == storeGeneration);
        std::printf(
            "late key: store=1 view_before=0 view_after=%d gen=%llu stable\n",
            access.snapshot()->guideCount(),
            static_cast<unsigned long long>(store.generation()));
    }
}

// (d) 4->1->2->4: store nuevo en cada parada, sin sync, 111 en todas.
void testViewChainWithoutSyncKeepsAllNotes()
{
    MidiGuidePersistence persistence(freshDirectory("d"));
    seedSidecar(persistence);

    const juce::String keys[] = {"view:4", "view:1", "view:2", "view:4"};
    for (const auto &key : keys)
    {
        DocumentMidiGuideStore store;
        store.setPersistence(&persistence);
        store.setDocumentKey(key);
        store.bindTrack(kTrackKey);
        const auto snapshot = store.publish();
        CHECK(store.size() == 1);
        CHECK(store.isFromDisk());
        CHECK(snapshot != nullptr);
        CHECK(snapshot->totalNotes == kLeadNotes);
        CHECK(snapshot->guides.front().origin == "document");
        CHECK(store.getLastLiveIds().empty()); // no scan in this chain
    }
    std::printf("chain 4->1->2->4: 4x%d notes, no sync\n", kLeadNotes);
}

// Extra: generation solo sube al publicar un cambio.
void testGenerationBumpsOnlyOnChange()
{
    DocumentMidiGuideStore store;
    store.bindTrack("track:gen");
    const auto initial = store.generation();
    CHECK(initial > 0);
    store.publish();
    CHECK(store.generation() == initial); // same publication

    auto guide = leadGuide();
    guide.id = "live:gen";
    CHECK(store.syncLive({guide}, true));
    const auto afterChange = store.generation();
    CHECK(afterChange == initial); // the mutation did not publish by itself
    store.publish();
    CHECK(store.generation() == afterChange + 1);

    CHECK(store.removeAt(0));
    store.publish();
    CHECK(store.generation() == afterChange + 2);
    std::printf("generation: initial=%llu after_publish=%llu final=%llu\n",
                static_cast<unsigned long long>(initial),
                static_cast<unsigned long long>(afterChange + 1),
                static_cast<unsigned long long>(store.generation()));
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    testSidecarSurvivesEmptyAuthoritativeScan();
    testOnlyScanContributedGuidesAreRemoved();
    testLateDocumentKeyNotifiesOnce();
    testViewChainWithoutSyncKeepsAllNotes();
    testGenerationBumpsOnlyOnChange();

    std::printf("MidiGuideStoreVisibilityTests: all checks passed\n");
    return 0;
}
