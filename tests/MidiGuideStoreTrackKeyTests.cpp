// R11d (guia MIDI, D-367): la clave de pista no depende del conjunto de items.
//
// R11c midio con la build CA61F466 que guardar y reabrir ya funcionaba (G-E3
// PASS: 1 guia / 111 fantasmas), pero mover a otra pista los cuatro items que no
// eran el objetivo cambiaba la clave - `src:7221217c41993ac6|name:v2 - stem` ->
// `src:782bf3636937c799|name:v2 - stem` - y el store caia a
// `guide_count=0` / `ghost_count=0` (G-B2 FAIL). La clave de R11b llevaba el
// fingerprint de las fuentes DE LA PISTA, que es la lista de sus items vivos:
// cualquier alta, baja o movimiento de audio renombraba el sidecar, en contra de
// SCOPE=A (la guia es de toda la pista, todos sus items y fuentes).
//
// Aqui se mide el contrato nuevo, sin ARA (no hay host en un test):
//  (a) mover 4 de 5 items deja la MISMA clave y los mismos 111 fantasmas;
//  (b) anadir un item (el documento gana una fuente) tampoco la cambia;
//  (c) quitar items hasta dejar 1 tampoco;
//  (d) dos pistas con el mismo nombre en documentos distintos no se mezclan, y
//      con varios candidatos el persistidor NO adivina (devuelve `none`);
//  (e) renombrar la pista cambia la clave (riesgo documentado en D-367);
//  (f) la clave de R11b (`src:...|name:`) se migra una vez y queda en `.bak`;
//  (g) una pista sin nombre cae al ordinal, y ese ordinal puede cambiar.
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
            .getChildFile("tunex-midi-guide-trackkey-"
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

int ghostCount(const DocumentMidiGuideStore& store, int index = 0)
{
    const auto* guide = store.at(index);
    if (guide == nullptr)
        return 0;
    return static_cast<int>(midiGuideGhostsFor(*guide, measuredTempoMap()).size());
}

int liveSidecars(const juce::File& directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles, "guide-*.json");
}

int backupSidecars(const juce::File& directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles, "*.bak");
}

// Monta un store nuevo como lo haria una sesion: persistencia en el directorio,
// clave de documento y clave de pista, y una guia importada de disco.
void seedSidecar(const juce::File& directory, const juce::String& projectKey,
                 const juce::String& trackKey)
{
    MidiGuidePersistence persistence(directory);
    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(projectKey);
    store.bindTrack(trackKey);
    store.upsert(leadGuide(), "file");
    CHECK(store.size() == 1);
    CHECK(store.flushPersistence());
}

// (a) Mover 4 de 5 items: la clave se calcula solo con el nombre, asi que no
// cambia - ni por la lista de fuentes ni por la posicion de la pista - y la
// guia sigue viendose con sus 111 fantasmas, tambien en una sesion nueva.
void testMovingFourOfFiveItemsKeepsTheKeyAndTheGhosts()
{
    // Cinco items de la pista; la clave no recibe ni un nombre de fichero.
    const auto withFiveItems = trackKeyForTrack(kTrackName, 0);
    // Los cuatro items que no son el objetivo se van a otra pista: la pista
    // queda en otra posicion del documento y con otro conjunto de fuentes.
    const auto afterMovingFour = trackKeyForTrack(kTrackName, 3);
    CHECK(withFiveItems == afterMovingFour);
    CHECK(withFiveItems == juce::String("name:") + kTrackName);
    CHECK(!withFiveItems.contains("src:"));
    CHECK(!withFiveItems.contains("|"));
    // Ni el ordinal ni el numero de items forman parte de la clave con nombre.
    CHECK(!withFiveItems.contains("ordinal:"));

    // Extremo a extremo: el sidecar se escribe con 5 items y se lee igual.
    const auto directory = freshDirectory("move");
    seedSidecar(directory,
                midiGuideDisk::projectKeyFromSources(
                    {"name:03-Lead.wav", "name:v2 - stem.wav"}),
                withFiveItems);
    CHECK(liveSidecars(directory) == 1);

    MidiGuidePersistence persistence(directory);
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&persistence);
    reopened.setDocumentKey(midiGuideDisk::projectKeyFromSources(
        {"name:03-Lead.wav", "name:v2 - stem.wav"}));
    reopened.bindTrack(trackKeyForTrack(kTrackName, 3));
    CHECK(reopened.size() == 1);
    CHECK(reopened.isFromDisk());
    CHECK(ghostCount(reopened) == kLeadNotes);
    if (reopened.at(0) != nullptr)
        CHECK(std::abs(midiGuideGhostsFor(*reopened.at(0), measuredTempoMap())
                           .front()
                           .startHostSeconds -
                       kGuideHostStart) <= 1.0e-9);
}

// (b) Anadir un item: si trae un medio nuevo, el conjunto de fuentes del
// documento cambia y con el la clave de documento (el nombre del fichero), pero
// no la de pista. El unico candidato se adopta y la guia sigue ahi.
void testAddingAnItemKeepsTheKeyAndTheGuide()
{
    const auto directory = freshDirectory("add");
    const auto trackKey = trackKeyForTrack(kTrackName, 0);
    const auto before =
        midiGuideDisk::projectKeyFromSources({"name:v2 - stem.wav"});
    const auto after = midiGuideDisk::projectKeyFromSources(
        {"name:v2 - stem.wav", "name:v3 - take.wav"});
    CHECK(before != after); // otra vista del documento...
    CHECK(trackKeyForTrack(kTrackName, 0) == trackKey); // ...misma pista

    seedSidecar(directory, before, trackKey);

    MidiGuidePersistence persistence(directory);
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&persistence);
    reopened.setDocumentKey(after); // el item nuevo cambio el documento
    reopened.bindTrack(trackKey);
    CHECK(reopened.size() == 1);
    CHECK(ghostCount(reopened) == kLeadNotes);
    // Un solo sidecar vivo: el anterior quedo en `.bak`, no borrado.
    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 1);
}

// (c) Quitar items hasta dejar 1: la clave no cambia porque solo lleva el
// nombre; el store no se vacia al re-bindear, y una sesion nueva lee los mismos
// 111 fantasmas del mismo fichero.
void testRemovingItemsDownToOneKeepsTheKey()
{
    const auto trackKey = trackKeyForTrack(kTrackName, 0);
    const auto directory = freshDirectory("remove");
    const auto projectKey =
        midiGuideDisk::projectKeyFromSources({"name:03-Lead.wav",
                                              "name:v2 - stem.wav"});
    seedSidecar(directory, projectKey, trackKey);

    MidiGuidePersistence persistence(directory);
    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(projectKey);
    store.bindTrack(trackKey);
    CHECK(store.size() == 1);
    const auto canonical = store.at(0);

    // Quedan 1 de 5 items: misma pista, misma clave, nada que recargar.
    CHECK(!store.bindTrack(trackKeyForTrack(kTrackName, 0)));
    CHECK(store.size() == 1);
    CHECK(store.at(0) == canonical); // ni republicacion ni recarga
    CHECK(ghostCount(store) == kLeadNotes);

    // Sesion nueva con un solo item: el mismo fichero, los mismos fantasmas.
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&persistence);
    reopened.setDocumentKey(projectKey);
    reopened.bindTrack(trackKeyForTrack(kTrackName, 0));
    CHECK(reopened.size() == 1);
    CHECK(reopened.isFromDisk());
    CHECK(ghostCount(reopened) == kLeadNotes);
    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 0); // no hubo nada que migrar
}

// (d) Dos pistas con el mismo nombre en documentos distintos: la clave de pista
// es la misma (solo el nombre), pero el documento esta en el nombre del fichero
// y cada una lee la suya. Con varios candidatos y ninguno exacto, el
// persistidor NO adivina: devuelve `none` y el store migra de la cache del
// Project en vez de traer la guia de otro proyecto (riesgo de D-367).
void testSameNameInTwoDocumentsDoesNotMix()
{
    const auto directory = freshDirectory("documents");
    MidiGuidePersistence persistence(directory);
    const auto trackKey = trackKeyForTrack("Lead", 0);
    const auto documentA =
        midiGuideDisk::projectKeyFromSources({"name:03-Lead.wav"});
    const auto documentB =
        midiGuideDisk::projectKeyFromSources({"name:07-Bgv.wav"});
    CHECK(documentA != documentB);
    CHECK(persistence.fileFor(documentA, trackKey) !=
          persistence.fileFor(documentB, trackKey));

    CHECK(persistence.save(documentA, trackKey, {leadGuide("a")}));
    CHECK(persistence.save(documentB, trackKey, {leadGuide("b")}));

    const auto fromA = persistence.load(documentA, trackKey);
    const auto fromB = persistence.load(documentB, trackKey);
    CHECK(fromA.usable() && fromB.usable());
    CHECK(fromA.guides.size() == 1 && fromB.guides.size() == 1);
    CHECK(fromA.guides.front().id == "a");
    CHECK(fromB.guides.front().id == "b");

    // Tercer documento con la misma pista y sin fichero propio: hay dos
    // candidatos canonicos y no se adopta ninguno.
    const auto documentC =
        midiGuideDisk::projectKeyFromSources({"name:other.wav"});
    const auto unknown = persistence.load(documentC, trackKey);
    CHECK(unknown.status == MidiGuideLoadResult::Status::none);
    CHECK(liveSidecars(directory) == 2); // los dos de A y B, intactos

    // Dentro de UN documento, dos pistas homonimas son una sola clave: riesgo
    // aceptado y documentado (SCOPE=A: una pista por instancia del plugin).
    CHECK(trackKeyForTrack("Lead", 0) == trackKeyForTrack("Lead", 5));
}

// (e) Renombrar la pista cambia la clave (riesgo documentado en D-367). El
// fichero anterior NO se borra: simplemente deja de encontrarse.
void testRenamingTheTrackChangesTheKey()
{
    const auto directory = freshDirectory("rename");
    MidiGuidePersistence persistence(directory);
    const auto documentKey =
        midiGuideDisk::projectKeyFromSources({"name:03-Lead.wav"});
    const auto before = trackKeyForTrack("Lead", 0);
    const auto after = trackKeyForTrack("Lead Lead", 0);
    CHECK(before != after);

    CHECK(persistence.save(documentKey, before, {leadGuide()}));
    const auto oldFile = persistence.fileFor(documentKey, before);
    CHECK(oldFile.existsAsFile());

    const auto renamed = persistence.load(documentKey, after);
    CHECK(!renamed.found());
    CHECK(oldFile.existsAsFile()); // los datos siguen en disco
}

// (f) La clave de R11b/R11c (`src:<huella>|name:<pista>`) se adopta una vez,
// se consolida bajo la clave nueva y el fichero viejo queda en `.bak`.
void testSourceFingerprintKeyIsMigratedOnce()
{
    const auto directory = freshDirectory("legacy-src");
    MidiGuidePersistence persistence(directory);
    const auto documentKey =
        midiGuideDisk::projectKeyFromSources({"name:03-Lead.wav",
                                              "name:v2 - stem.wav"});
    const auto trackKey = trackKeyForTrack(kTrackName, 0);

    // El fichero vivo que dejo R11c en la maquina del usuario.
    const auto legacy = persistence.fileFor(
        documentKey, "src:7221217c41993ac6|name:" + juce::String(kTrackName));
    CHECK(persistence.save(documentKey,
                           "src:7221217c41993ac6|name:" +
                               juce::String(kTrackName),
                           {leadGuide()}));
    CHECK(legacy.existsAsFile());
    CHECK(liveSidecars(directory) == 1);

    const auto loaded = persistence.load(documentKey, trackKey);
    CHECK(loaded.usable());
    CHECK(loaded.guides.size() == 1);
    CHECK(loaded.guides.front().notes.size() ==
          static_cast<std::size_t>(kLeadNotes));
    CHECK(persistence.fileFor(documentKey, trackKey).existsAsFile());
    CHECK(!legacy.existsAsFile());
    CHECK(liveSidecars(directory) == 1);
    CHECK(backupSidecars(directory) == 1);

    // La migracion no se repite: la segunda lectura es el fichero nuevo.
    const auto again = persistence.load(documentKey, trackKey);
    CHECK(again.usable());
    CHECK(again.guides.size() == 1);
    CHECK(backupSidecars(directory) == 1);
}

// (g) Sin nombre, la clave cae al ordinal. Es el ultimo recurso y se documenta:
// reordenar pistas sin nombre renombra la clave.
void testNamelessTrackFallsBackToTheOrdinal()
{
    CHECK(trackKeyForTrack("", 2) == "ordinal:2");
    CHECK(trackKeyForTrack("   ", 2) == "ordinal:2");
    CHECK(trackKeyForTrack("", 2) != trackKeyForTrack("", 3));
    // El nombre se recorta, para que un espacio del host no cree otra pista.
    CHECK(trackKeyForTrack("  Lead  ", 0) == trackKeyForTrack("Lead", 0));
}
} // namespace

int main()
{
    testMovingFourOfFiveItemsKeepsTheKeyAndTheGhosts();
    testAddingAnItemKeepsTheKeyAndTheGuide();
    testRemovingItemsDownToOneKeepsTheKey();
    testSameNameInTwoDocumentsDoesNotMix();
    testRenamingTheTrackChangesTheKey();
    testSourceFingerprintKeyIsMigratedOnce();
    testNamelessTrackFallsBackToTheOrdinal();

    std::printf("MidiGuideStoreTrackKeyTests: all checks passed\n");
    return 0;
}
