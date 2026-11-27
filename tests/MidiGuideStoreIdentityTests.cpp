// R11b (guia MIDI, D-365) / R11d (D-367): identidad persistente del sidecar.
//
// R11 midio con la build 6EF011EA que tras guardar y reabrir el proyecto el
// store arrancaba con `guide_count=0` y `ghost_count=0` (esperado 1 y 111).
// Causa: la clave de pista llevaba la DIRECCION del ARADocument
// (`doc:75dd3060` -> `doc:75c709f0`) y el fingerprint del documento se
// construia con los `getPersistentID()` de las fuentes, que tambien cambian en
// cada sesion (`7fe16568...`, `b22f0ae4...`, `2616d898...` para el mismo
// material): cada sesion escribia un sidecar con una clave que no volvia.
//
// R11b lo arreglo con la huella de las fuentes de la pista + el nombre, pero
// R11c midio que esa huella ES la lista de items: mover, anadir o quitar audio
// renombraba el sidecar (G-B2 FAIL). R11d deja la clave de pista en el nombre
// solo; el contrato nuevo (items, undo del documento) se mide en
// MidiGuideStoreTrackKeyTests.
//
// Aqui se mide lo que NO cambia con R11d, y el contrato de R11f (D-370), sin
// ARA (no hay host en un test):
//  (a) dos sesiones con el mismo nombre de pista dan la MISMA clave y la MISMA
//      ruta, sin direccion ni documento dentro; el documento ya no vive en el
//      nombre del fichero;
//  (b) una reapertura lee el sidecar y ve las 111 notas;
//  (c) los sidecars legacy `doc:<direccion>` de la misma pista se importan una
//      vez, se consolidan en un solo fichero y quedan como `.bak` (no se borran
//      datos del usuario);
//  (d) una ruta por pista: dos pistas de nombre distinto no comparten fichero,
//      y dos documentos con una pista homonima SI (riesgo aceptado en D-370);
//  (e) renombrar la pista cambia la clave (riesgo documentado): el fichero
//      viejo no se borra, simplemente deja de encontrarse.
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
            .getChildFile("tunex-midi-guide-identity-"
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

// Copia el formato exacto que escribia el build de R11: mismo `format`, las
// guias en el mismo orden. Sirve para montar el sidecar legacy sin depender de
// que el codigo actual sepa escribirlo.
juce::var guideToVar(const GuideSource& guide)
{
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("id", juce::String(guide.id));
    object->setProperty("label", juce::String(guide.label));
    object->setProperty("kind", juce::String(guide.kind));
    object->setProperty("importedAt", juce::String(guide.importedAt));

    juce::Array<juce::var> notes;
    for (const auto& note : guide.notes)
    {
        juce::Array<juce::var> values;
        values.add(note.startQuarter);
        values.add(note.endQuarter);
        values.add(static_cast<double>(note.pitch));
        values.add(static_cast<double>(note.velocity));
        notes.add(juce::var(values));
    }
    object->setProperty("notes", juce::var(notes));
    return juce::var(object.get());
}

juce::File writeLegacySidecar(const juce::File& directory,
                              const juce::String& projectKey,
                              const juce::String& trackKey,
                              const std::vector<GuideSource>& guides)
{
    juce::Array<juce::var> array;
    for (const auto& guide : guides)
        array.add(guideToVar(guide));

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("format", midiGuideDisk::kFormatVersion);
    root->setProperty("projectKey", projectKey);
    root->setProperty("trackKey", trackKey);
    root->setProperty("guides", juce::var(array));

    const auto file = directory.getChildFile(
        "guide-" +
        juce::String::toHexString(
            juce::Random::getSystemRandom().nextInt64())
            .toLowerCase() +
        ".json");
    file.replaceWithText(juce::JSON::toString(juce::var(root.get()), false));
    return file;
}

int liveSidecars(const juce::File& directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles, "guide-*.json");
}

int backupSidecars(const juce::File& directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles, "*.bak");
}

// (a) La clave es el nombre de pista. No hay direccion, ni documento, ni
// huella de fuentes, ni ordinal que pueda cambiarla entre sesiones; el
// documento se separa en el NOMBRE DEL FICHERO.
void testSameTrackNameGivesTheSameKey()
{
    const auto sessionOne = trackKeyForTrack(kTrackName, 0);
    const auto sessionTwo = trackKeyForTrack(kTrackName, 2);

    // El ordinal no entra cuando hay nombre: la posicion en la lista del host
    // puede cambiar (reordenar items) sin renombrar el sidecar.
    CHECK(sessionOne == sessionTwo);
    CHECK(sessionOne == juce::String("name:") + kTrackName);
    CHECK(!sessionOne.contains("doc:"));
    CHECK(!sessionOne.contains("0x"));

    // Otro nombre es otra pista.
    CHECK(trackKeyForTrack("Lead", 0) != sessionOne);

    // El documento (la huella de sus fuentes) ni entra en la clave de pista ni
    // en el nombre del fichero (D-370): la misma pista es UNA ruta en todas las
    // vistas del documento. Antes de R11f cada vista hasheaba otro fichero y la
    // guia "saltaba" (R12: segundo cambio -> 0/0).
    const auto documentA =
        midiGuideDisk::projectKeyFromSources({"name:03-Lead.wav"});
    const auto documentB =
        midiGuideDisk::projectKeyFromSources({"name:07-Bgv.wav"});
    CHECK(documentA != documentB);
    const auto keysDirectory = freshDirectory("keys");
    CHECK(midiGuideDisk::fileFor(keysDirectory, sessionOne) ==
          midiGuideDisk::fileFor(keysDirectory, sessionTwo));
    CHECK(midiGuideDisk::fileFor(keysDirectory, sessionOne) !=
          midiGuideDisk::fileFor(keysDirectory, trackKeyForTrack("Lead", 0)));

    // Sin nombre el fingerprint no existe: la clave cae al ordinal.
    const auto nameless = trackKeyForTrack({}, 2);
    CHECK(nameless == "ordinal:2");
    CHECK(nameless != trackKeyForTrack({}, 3));
}

// (b) La reapertura es un store y un MidiGuidePersistence nuevos sobre el
// mismo disco: la clave derivada de los mismos datos encuentra el sidecar y
// las 111 notas vuelven, sin sync.
void testReopenReadsTheSidecarWithTheNewKey()
{
    const auto directory = freshDirectory("reopen");
    const std::vector<juce::String> sources{"name:03-Lead.wav",
                                            "name:v2 - stem.wav"};
    const auto projectKey = midiGuideDisk::projectKeyFromSources(sources);
    const auto trackKey = trackKeyForTrack(kTrackName, 0);
    CHECK(projectKey.isNotEmpty());

    {
        MidiGuidePersistence persistence(directory);
        DocumentMidiGuideStore first;
        first.setPersistence(&persistence);
        first.setDocumentKey(projectKey);
        first.bindTrack(trackKey);
        first.upsert(leadGuide(), "file");
        CHECK(first.size() == 1);
        CHECK(first.flushPersistence());
    }

    // Sesion nueva: otro documento, otra RegionSequence, mismos datos.
    MidiGuidePersistence reopenedPersistence(directory);
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&reopenedPersistence);
    reopened.setDocumentKey(
        midiGuideDisk::projectKeyFromSources({"name:v2 - stem.wav",
                                              "name:03-Lead.wav"}));
    reopened.bindTrack(trackKeyForTrack(kTrackName, 0));

    CHECK(reopened.size() == 1);
    CHECK(reopened.isFromDisk());
    CHECK(ghostCount(reopened) == kLeadNotes);
    if (reopened.at(0) != nullptr)
        CHECK(std::abs(
                  midiGuideGhostsFor(*reopened.at(0), measuredTempoMap())
                      .front()
                      .startHostSeconds -
                  kGuideHostStart) <= 1.0e-9);
}

// (c) Los sidecars legacy `doc:<direccion>` (6 en la medida de R11, tres
// documentos distintos) de la misma pista se importan, se consolidan en un
// unico fichero vivo y los originales quedan como `.bak`. Nada se borra.
void testLegacyDocumentKeyIsImportedAndConsolidated()
{
    const auto directory = freshDirectory("legacy");
    MidiGuidePersistence persistence(directory);

    const std::vector<juce::String> sources{"name:03-Lead.wav",
                                            "name:v2 - stem.wav"};
    const auto projectKey = midiGuideDisk::projectKeyFromSources(sources);
    const auto trackKey = trackKeyForTrack(kTrackName, 0);

    // Tres sesiones de R11: mismo material, tres projectKey y tres direcciones.
    const auto legacyOne =
        writeLegacySidecar(directory, "7fe16568fb444250",
                           "doc:75e02210|name:" + juce::String(kTrackName),
                           {leadGuide()});
    const auto legacyTwo =
        writeLegacySidecar(directory, "b22f0ae49148631a",
                           "doc:75dd3060|name:" + juce::String(kTrackName),
                           {leadGuide()});
    // Una pista distinta en el mismo directorio: no se puede adoptar.
    const auto otherTrack =
        writeLegacySidecar(directory, "b22f0ae49148631a",
                           "doc:75dd3060|name:Otra pista", {leadGuide("other")});
    CHECK(liveSidecars(directory) == 3);

    const auto loaded = persistence.load(projectKey, trackKey);
    CHECK(loaded.usable());
    CHECK(loaded.guides.size() == 1); // consolidado por id, sin duplicar
    CHECK(loaded.guides.front().notes.size() ==
          static_cast<std::size_t>(kLeadNotes));
    CHECK(persistence.fileFor(trackKey).existsAsFile());

    // Solo un sidecar vivo de la pista; los dos legacy, retirados a `.bak`.
    CHECK(liveSidecars(directory) == 2); // el nuevo + el de la otra pista
    CHECK(backupSidecars(directory) == 2);
    CHECK(!legacyOne.existsAsFile());
    CHECK(!legacyTwo.existsAsFile());
    CHECK(otherTrack.existsAsFile()); // no se toca lo que no es de la pista

    // La migracion no se repite ni duplica: la segunda lectura es la nueva.
    const auto again = persistence.load(projectKey, trackKey);
    CHECK(again.usable());
    CHECK(again.guides.size() == 1);
    CHECK(backupSidecars(directory) == 2);
}

// (d) R11f (D-370): una ruta por pista. Dos pistas de nombre distinto siguen
// teniendo ficheros distintos; dos documentos con una pista homonima comparten
// el fichero de esa pista (riesgo aceptado y documentado en D-370: gana el
// ultimo que escribe y el otro adopta la misma guia, que es justo lo que evita
// el salto de fichero de R12).
void testOneFilePerTrackName()
{
    const auto directory = freshDirectory("tracks");
    MidiGuidePersistence persistence(directory);
    const auto key = trackKeyForTrack(kTrackName, 0);

    const auto projectKey = midiGuideDisk::projectKeyFromSources(
        {"name:03-Lead.wav", "name:07-Bgv.wav"});
    const auto leadKey = trackKeyForTrack(kTrackName, 0);
    const auto bgvKey = trackKeyForTrack("v2 - bgv", 1);
    CHECK(leadKey != bgvKey); // otro nombre, otra pista
    CHECK(persistence.fileFor(leadKey) != persistence.fileFor(bgvKey));

    CHECK(persistence.save(projectKey, leadKey, {leadGuide("lead")}));
    CHECK(persistence.save(projectKey, bgvKey, {leadGuide("bgv")}));

    const auto lead = persistence.load(projectKey, leadKey);
    const auto bgv = persistence.load(projectKey, bgvKey);
    CHECK(lead.usable() && bgv.usable());
    CHECK(lead.guides.size() == 1 && bgv.guides.size() == 1);
    CHECK(lead.guides.front().id == "lead");
    CHECK(bgv.guides.front().id == "bgv");

    // Dos documentos distintos, la misma pista: la MISMA ruta, y el segundo
    // documento lee lo que escribio el primero en vez de crear otra.
    const auto otherDocument =
        midiGuideDisk::projectKeyFromSources({"name:other-take.wav"});
    const auto leadFile = persistence.fileFor(key);
    CHECK(leadFile.existsAsFile());
    const auto fromOtherDocument = persistence.load(otherDocument, key);
    CHECK(fromOtherDocument.usable());
    CHECK(fromOtherDocument.guides.size() == 1);
    CHECK(fromOtherDocument.guides.front().id == "lead");
    CHECK(persistence.fileFor(key) == leadFile);
    CHECK(liveSidecars(directory) == 2); // el de lead y el de bgv, sin duplicar
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
    const auto oldFile = persistence.fileFor(before);
    CHECK(oldFile.existsAsFile());

    const auto renamed = persistence.load(documentKey, after);
    CHECK(!renamed.found());
    CHECK(oldFile.existsAsFile()); // los datos siguen en disco
}
} // namespace

int main()
{
    testSameTrackNameGivesTheSameKey();
    testReopenReadsTheSidecarWithTheNewKey();
    testLegacyDocumentKeyIsImportedAndConsolidated();
    testOneFilePerTrackName();
    testRenamingTheTrackChangesTheKey();

    std::printf("MidiGuideStoreIdentityTests: all checks passed\n");
    return 0;
}
