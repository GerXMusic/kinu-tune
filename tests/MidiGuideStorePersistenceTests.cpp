// R7 (guia MIDI, D-360): la guia sobrevive a cerrar y reabrir el proyecto.
//
// R5b/R5d dejaron la guia en el documento (un store, una publicacion) y R5e
// midio que cambiar de item ya no la pierde. Lo que faltaba era el DISCO: el
// archivo ARA no es de fiar (D-346: `saved_notes = -1` en las cinco regiones y
// `copyProjectArchive` vacio), asi que la guia se escribe en un sidecar JSON
// atomico junto a los datos de la app.
//
// Aqui se mide, sin ARA (no hay host en un test):
//  (a) ida y vuelta exacta: 111 notas, mismas posiciones y mismos hosts;
//  (b) el store reabierto lee el disco y NO duplica con la cache del Project;
//  (c) proyecto viejo (solo `midiGuides`): migra y materializa el sidecar;
//  (d) proyecto sin guias: no se inventa fichero; quitar la ultima guia si se
//      escribe (vacio = quiero cero) y no resucita desde la cache;
//  (e) sidecar corrupto: se rechaza, se avisa y se sigue con la migracion;
//  (f) dos claves distintas no se mezclan (ni el fichero de la otra se adopta);
//  (g) escritura atomica: no queda fichero a medias ni temporal visible.
// Fixture = la guia Lead real medida en R5 (13,704374 s / 111 notas).
#include "TestAssert.h"

#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/Models/DocumentMidiGuides.h"
#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/Models/MidiGuidePersistence.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr double kGuideHostStart = 13.704374;
constexpr int kLeadNotes = 111;
constexpr const char* kProjectKey = "document:lead-stem-hash";
constexpr const char* kTrackKey = "track:tunex-v2-2";
constexpr const char* kOtherProjectKey = "document:other-song-hash";

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

GuideSource leadGuide()
{
    GuideSource guide;
    guide.id = "reaper:lead";
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
            .getChildFile("tunex-midi-guide-persistence-"
                          + juce::String::toHexString(
                                juce::Random::getSystemRandom().nextInt64()));
    dir.createDirectory();
    return dir;
}

// Cada prueba escribe en su propia carpeta: el disco de una no puede tapar la
// migracion de la siguiente.
juce::File freshDirectory(const juce::String& name)
{
    const auto dir = testDirectory().getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

bool sameGuide(const GuideSource& a, const GuideSource& b)
{
    if (a.id != b.id || a.label != b.label || a.kind != b.kind ||
        a.importedAt != b.importedAt || a.notes.size() != b.notes.size())
        return false;
    for (std::size_t i = 0; i < a.notes.size(); ++i)
    {
        // Exact, not near(): the round trip must not lose a bit or the render
        // would drift by a few samples on every reopen.
        if (a.notes[i].startQuarter != b.notes[i].startQuarter ||
            a.notes[i].endQuarter != b.notes[i].endQuarter ||
            a.notes[i].pitch != b.notes[i].pitch ||
            a.notes[i].velocity != b.notes[i].velocity)
            return false;
    }
    return true;
}

int fileCount(const juce::File& directory)
{
    return directory.getNumberOfChildFiles(juce::File::findFiles, "*");
}

// (a) Ida y vuelta exacta, hosts incluidos.
void testRoundTripIsExact()
{
    MidiGuidePersistence persistence(freshDirectory("a"));
    const auto guide = leadGuide();

    juce::String error;
    CHECK(persistence.save(kProjectKey, kTrackKey, {guide}, &error));
    CHECK(error.isEmpty());

    const auto loaded = persistence.load(kProjectKey, kTrackKey);
    CHECK(loaded.usable());
    CHECK(loaded.guides.size() == 1);
    CHECK(sameGuide(loaded.guides.front(), guide));

    // La misma guia en el eje del host: 111 fantasmas y el primero a 13,704374.
    const auto before =
        midiGuideGhostsFor(guide, measuredTempoMap());
    const auto after =
        midiGuideGhostsFor(loaded.guides.front(), measuredTempoMap());
    CHECK(before.size() == static_cast<std::size_t>(kLeadNotes));
    CHECK(after.size() == before.size());
    CHECK(after.front().startHostSeconds == before.front().startHostSeconds);
    CHECK(before.front().startHostSeconds == kGuideHostStart);
}

// (b) Reabrir es un store nuevo (otro documento del host, mismo disco).
void testStoreReadsDiskOnReopen()
{
    MidiGuidePersistence persistence(freshDirectory("b"));
    {
        DocumentMidiGuideStore first;
        first.setPersistence(&persistence);
        first.setDocumentKey(kProjectKey);
        first.bindTrack(kTrackKey);
        CHECK(first.syncLive({leadGuide()}));
        CHECK(first.size() == 1);
    }

    // El proyecto viejo sigue llevando su copia en la cache de compatibilidad:
    // con sidecar valido NO se vuelve a importar (ni se duplica).
    Project legacyCache;
    legacyCache.getMidiGuides().push_back(leadGuide());

    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&persistence);
    reopened.setDocumentKey(kProjectKey);
    reopened.observeProjects({&legacyCache});
    CHECK(!reopened.bindTrack(kTrackKey));
    CHECK(reopened.size() == 1);
    CHECK(reopened.isFromDisk());
    CHECK(reopened.at(0) != nullptr);
    CHECK(sameGuide(*reopened.at(0), leadGuide()));
    CHECK(reopened.takePersistenceWarning().isEmpty());

    // La cache no resucita nada: el store ya tiene la guia por id.
    CHECK(!reopened.importFromProject(legacyCache));
    CHECK(reopened.size() == 1);
    CHECK(midiGuideGhostsFor(*reopened.at(0), measuredTempoMap()).size() ==
          static_cast<std::size_t>(kLeadNotes));
}

// (c) Proyecto viejo: solo `midiGuides`, ningun sidecar.
void testLegacyProjectMigratesAndMaterialises()
{
    MidiGuidePersistence persistence(freshDirectory("c"));
    Project legacyCache;
    legacyCache.getMidiGuides().push_back(leadGuide());

    DocumentMidiGuideStore migrated;
    migrated.setPersistence(&persistence);
    migrated.setDocumentKey(kProjectKey);
    migrated.observeProjects({&legacyCache});
    migrated.bindTrack(kTrackKey);
    CHECK(migrated.size() == 1);
    CHECK(!migrated.isFromDisk());
    CHECK(persistence.fileFor(kProjectKey, kTrackKey).existsAsFile());

    // La migracion se materializo: la siguiente apertura ya lee del disco.
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&persistence);
    reopened.setDocumentKey(kProjectKey);
    reopened.bindTrack(kTrackKey);
    CHECK(reopened.size() == 1);
    CHECK(reopened.isFromDisk());
    CHECK(sameGuide(*reopened.at(0), leadGuide()));
}

// (d) Sin guias no se inventa fichero; quitar la ultima si se escribe.
void testNoGuidesWritesNothingAndRemovalSticks()
{
    MidiGuidePersistence persistence(freshDirectory("d"));
    Project emptyProject;

    DocumentMidiGuideStore empty;
    empty.setPersistence(&persistence);
    empty.setDocumentKey(kProjectKey);
    empty.observeProjects({&emptyProject});
    empty.bindTrack(kTrackKey);
    CHECK(empty.empty());
    CHECK(!persistence.fileFor(kProjectKey, kTrackKey).existsAsFile());

    // Un proyecto guardado sin guias (flush del host) tampoco puede crear un
    // sidecar vacio que luego tape la cache de un proyecto viejo.
    CHECK(!empty.flushPersistence());
    CHECK(!persistence.fileFor(kProjectKey, kTrackKey).existsAsFile());

    // Con una guia escrita, quitarla deja un sidecar vacio (autoritativo).
    CHECK(empty.syncLive({leadGuide()}));
    CHECK(persistence.fileFor(kProjectKey, kTrackKey).existsAsFile());
    CHECK(empty.removeAt(0));
    CHECK(empty.empty());

    Project staleCache;
    staleCache.getMidiGuides().push_back(leadGuide());
    DocumentMidiGuideStore reopened;
    reopened.setPersistence(&persistence);
    reopened.setDocumentKey(kProjectKey);
    reopened.observeProjects({&staleCache});
    reopened.bindTrack(kTrackKey);
    CHECK(reopened.empty()); // no resucita desde la cache vieja
    CHECK(reopened.isFromDisk());
}

// (e) Sidecar corrupto: rechazo + aviso + migracion, sin romper la carga.
void testCorruptSidecarIsRejectedAndWarns()
{
    MidiGuidePersistence persistence(freshDirectory("e"));
    const auto file = persistence.fileFor(kProjectKey, kTrackKey);
    file.getParentDirectory().createDirectory();
    file.replaceWithText("{ esto no es json");

    Project legacyCache;
    legacyCache.getMidiGuides().push_back(leadGuide());

    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(kProjectKey);
    store.observeProjects({&legacyCache});
    store.bindTrack(kTrackKey);
    CHECK(store.size() == 1); // la cache salva la carga
    CHECK(!store.isFromDisk());
    const auto warning = store.takePersistenceWarning();
    CHECK(warning.isNotEmpty());
    CHECK(warning.containsIgnoreCase("rejected"));
    CHECK(store.takePersistenceWarning().isEmpty()); // se avisa una vez

    // Un fichero de un formato futuro tampoco se adivina: se rechaza.
    file.replaceWithText(
        "{\"format\": 99, \"projectKey\": \"" + juce::String(kProjectKey) +
        "\", \"trackKey\": \"" + juce::String(kTrackKey) +
        "\", \"guides\": []}");
    const auto future = persistence.load(kProjectKey, kTrackKey);
    CHECK(future.status == MidiGuideLoadResult::Status::rejected);
}

// (f) Dos claves distintas (dos proyectos, o dos pistas) no se mezclan.
void testTwoKeysDoNotMix()
{
    MidiGuidePersistence persistence(freshDirectory("f"));
    const auto mine = persistence.fileFor(kProjectKey, kTrackKey);
    const auto other = persistence.fileFor(kOtherProjectKey, kTrackKey);
    CHECK(mine != other);

    GuideSource otherGuide;
    otherGuide.id = "reaper:other";
    otherGuide.label = "Other";
    otherGuide.notes.push_back({0.0, 1.0, 60.0f, 0.5f});

    CHECK(persistence.save(kProjectKey, kTrackKey, {leadGuide()}));
    CHECK(persistence.save(kOtherProjectKey, kTrackKey, {otherGuide}));

    const auto mineLoaded = persistence.load(kProjectKey, kTrackKey);
    const auto otherLoaded = persistence.load(kOtherProjectKey, kTrackKey);
    CHECK(mineLoaded.guides.size() == 1);
    CHECK(otherLoaded.guides.size() == 1);
    CHECK(mineLoaded.guides.front().id == "reaper:lead");
    CHECK(otherLoaded.guides.front().id == "reaper:other");

    // El fichero del otro proyecto, copiado con el nombre de este, se rechaza:
    // las claves del contenido no son las que se preguntan.
    CHECK(other.copyFileTo(mine));
    const auto borrowed = persistence.load(kProjectKey, kTrackKey);
    CHECK(borrowed.status == MidiGuideLoadResult::Status::rejected);
    CHECK(borrowed.guides.empty());

    DocumentMidiGuideStore store;
    store.setPersistence(&persistence);
    store.setDocumentKey(kProjectKey);
    store.bindTrack(kTrackKey);
    CHECK(store.empty()); // no adopta la guia del otro proyecto
    CHECK(store.takePersistenceWarning().containsIgnoreCase("another project"));
}

// (g) Escritura atomica: nada a medias, y un temporal huerfano se ignora.
void testAtomicWriteLeavesNoHalfFile()
{
    MidiGuidePersistence persistence(freshDirectory("g"));
    const auto file = persistence.fileFor(kProjectKey, kTrackKey);
    const auto directory = file.getParentDirectory();

    CHECK(persistence.save(kProjectKey, kTrackKey, {leadGuide()}));
    CHECK(file.existsAsFile());
    CHECK(fileCount(directory) == 1);

    // Segundo guardado completo: reemplaza, no mezcla.
    GuideSource shorter;
    shorter.id = "reaper:short";
    shorter.label = "Short";
    shorter.notes.push_back({1.0, 2.0, 61.0f, 0.25f});
    CHECK(persistence.save(kProjectKey, kTrackKey, {shorter}));
    CHECK(fileCount(directory) == 1);
    const auto replaced = persistence.load(kProjectKey, kTrackKey);
    CHECK(replaced.usable());
    CHECK(replaced.guides.size() == 1);
    CHECK(replaced.guides.front().notes.size() == 1);
    CHECK(sameGuide(replaced.guides.front(), shorter));

    // Un temporal que dejo una caida no se lee ni se adopta (el nombre del
    // temporal de juce::TemporaryFile es <destino>_temp<hex>.<ext>).
    const auto orphan =
        directory.getChildFile("guide_temp1234" + juce::String(".json"));
    orphan.replaceWithText("guia a medias");
    const auto loaded = persistence.load(kProjectKey, kTrackKey);
    CHECK(loaded.usable());
    CHECK(sameGuide(loaded.guides.front(), shorter));
    orphan.deleteFile();
}
} // namespace

int main()
{
    testRoundTripIsExact();
    testStoreReadsDiskOnReopen();
    testLegacyProjectMigratesAndMaterialises();
    testNoGuidesWritesNothingAndRemovalSticks();
    testCorruptSidecarIsRejectedAndWarns();
    testTwoKeysDoNotMix();
    testAtomicWriteLeavesNoHalfFile();

    testDirectory().deleteRecursively();
    std::printf("MidiGuideStorePersistenceTests: all checks passed\n");
    return 0;
}
