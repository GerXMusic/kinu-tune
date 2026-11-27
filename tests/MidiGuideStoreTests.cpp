// R5b (guia MIDI): la guia es del DOCUMENTO, no del item activo.
//
// El fallo que esta fase cierra (R5, G-R2): la guia vivia en Project, que es
// por modificacion/region; al elegir otro item el Project era otro, no tenia
// guias y el canvas pasaba de 111 fantasmas a 0. Aqui se mide el store nuevo:
// una instantanea inmutable publicada que apunta a las guias reales (sin
// copiarlas), que sobrevive al cambio de item y que solo se libera cuando muere
// el ultimo Project del documento. Fixture = la guia Lead real medida en R5
// (13,704374 s / 111 notas, QN 32..151) y el mapa de tempo publicado en G-R3.
#include "TestAssert.h"

#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/Models/DocumentMidiGuides.h"
#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/Models/ProjectSerializer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
// Measured values (R5): host span of the Lead guide and the offset of item 4.
constexpr double kGuideHostStart = 13.704374;
constexpr double kGuideHostEnd = 73.962000;
constexpr double kItem4Offset = 5.818184;
constexpr int kLeadNotes = 111;

bool near(double a, double b, double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

// G-R3: the map the plug-in read in REAPER (qn -> host seconds), enough points
// to place the whole guide on the real timeline.
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
    guide.importedAt = "2026-11-27T00:00:00Z";

    // QN 32..151 with 111 notes, like the measured one.
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

GuideSource smallGuide(const std::string& id, int notes)
{
    GuideSource guide;
    guide.id = id;
    guide.label = id;
    guide.kind = "file";
    for (int i = 0; i < notes; ++i)
    {
        GuideNote note;
        note.startQuarter = 4.0 + i;
        note.endQuarter = note.startQuarter + 0.9;
        note.pitch = 60.0f + static_cast<float>(i % 5);
        guide.notes.push_back(note);
    }
    return guide;
}

// Un cambio de item (4 -> 1) no puede perder la guia ni copiarla: el snapshot
// publicado sigue apuntando a la misma GuideSource canonica.
void testSwitchingItemKeepsTheGuide()
{
    Project item4;
    Project item1;
    item4.getMidiGuides().push_back(leadGuide());

    DocumentMidiGuideStore store;
    store.bindTrack("track:v2-stem");
    store.observeProjects({&item4, &item1});

    CHECK(store.importFromProject(item4)); // migracion de la copia vieja
    const auto first = store.publish();
    CHECK(first->guideCount() == 1);
    CHECK(first->guides[0].notes == kLeadNotes);
    CHECK(first->totalNotes == kLeadNotes);

    // El store apunta a la guia real: misma direccion, mismo vector de notas.
    const GuideSource* canonical = store.at(0);
    CHECK(canonical != nullptr);
    CHECK(first->guides[0].guide.get() == canonical);
    CHECK(first->guides[0].guide->notes.data() == canonical->notes.data());

    // El item 1 no trae guias; la publicacion no cambia y la guia sigue ahi.
    CHECK(!store.importFromProject(item1));
    const auto afterSwitch = store.publish();
    CHECK(afterSwitch == first);
    CHECK(afterSwitch->guides[0].guide.get() == canonical);
    CHECK(afterSwitch->guides[0].notes == kLeadNotes);

    // Alternar otra vez tampoco toca la guia.
    store.observeProjects({&item1, &item4});
    CHECK(store.size() == 1);
    CHECK(store.publish()->guides[0].guide.get() == canonical);
}

// La copia por modificacion es solo cache: la verdad es del documento, asi que
// un Project puede morir sin dejar punteros colgando. Solo la muerte del ULTIMO
// Project del documento libera las guias.
void testGuideSurvivesItemDeathAndDiesWithTheDocument()
{
    Project item4;
    Project item1;
    DocumentMidiGuideStore store;
    store.bindTrack("track:v2-stem");
    store.observeProjects({&item4, &item1});
    item4.getMidiGuides().push_back(leadGuide());
    CHECK(store.importFromProject(item4));

    const GuideSource* canonical = store.at(0);
    CHECK(canonical != nullptr);

    CHECK(!store.releaseProject(&item4)); // el item 4 muere: la guia es del documento
    CHECK(store.size() == 1);
    CHECK(store.publish()->guideCount() == 1);
    CHECK(store.publish()->guides[0].guide.get() == canonical);
    CHECK(store.publish()->guides[0].guide->notes.size() ==
          static_cast<size_t>(kLeadNotes));

    // El ultimo Project del documento muere: ahora si, el documento se libera.
    CHECK(store.releaseProject(&item1));
    CHECK(store.empty());
    CHECK(store.publish()->empty());
}

// El snapshot es inmutable: una mutacion posterior no lo altera, y sin cambios
// la misma publicacion se devuelve tal cual (mismo shared_ptr).
void testSnapshotIsImmutable()
{
    DocumentMidiGuideStore store;
    store.bindTrack("track:v2-stem");
    CHECK(store.upsert(leadGuide(), "file") == 0);

    const auto snapshot = store.publish();
    const GuideSource* firstGuide = store.at(0);
    CHECK(snapshot->guideCount() == 1);

    CHECK(store.upsert(smallGuide("file:b", 5), "file") == 1);
    const auto second = store.publish();
    CHECK(second != snapshot);            // publicacion nueva...
    CHECK(snapshot->guideCount() == 1);   // ...sin tocar la anterior
    CHECK(snapshot->guides[0].guide.get() == firstGuide);
    CHECK(second->guideCount() == 2);

    // Sin cambios, la misma instantanea.
    CHECK(store.publish() == second);
    CHECK(store.publish() == store.snapshot());
}

// Migracion del JSON viejo: se importa una vez, no se duplica y no resucita lo
// que el usuario quito (el JSON antiguo sigue en el fichero guardado).
void testLegacyJsonMigrationIsOnce()
{
    Project legacy;
    legacy.getMidiGuides().push_back(leadGuide());

    const auto json = ProjectSerializer::toJson(legacy);
    Project restored;
    CHECK(ProjectSerializer::fromJson(restored, json));
    CHECK(restored.getMidiGuides().size() == 1);
    CHECK(restored.getMidiGuides().front().notes.size() ==
          static_cast<size_t>(kLeadNotes));

    DocumentMidiGuideStore store;
    store.bindTrack("track:v2-stem");
    CHECK(store.importFromProject(restored));
    CHECK(store.size() == 1);
    CHECK(!store.importFromProject(restored)); // una vez por Project
    CHECK(store.size() == 1);

    const auto snapshot = store.publish();
    CHECK(snapshot->guides[0].id == "reaper:lead");
    CHECK(snapshot->guides[0].origin == "legacy");
    CHECK(snapshot->totalNotes == kLeadNotes);

    // El live de la misma fuente no duplica: mismo id, mismas notas.
    CHECK(!store.syncLive({leadGuide()}));
    CHECK(store.size() == 1);

    // Y quitar la guia no la trae de vuelta desde la cache del Project viejo.
    CHECK(store.removeAt(0));
    CHECK(!store.importFromProject(restored));
    CHECK(store.empty());
}

// Dos fuentes: dos guias del documento, con sus notas y sin mezclarse. Un live
// con el mismo id reemplaza (no reordena) y uno nuevo se agrega.
void testTwoSources()
{
    DocumentMidiGuideStore store;
    store.bindTrack("track:v2-stem");

    const auto lead = leadGuide();
    const auto doubles = smallGuide("reaper:doubles", 5);
    CHECK(store.syncLive({lead, doubles}));
    CHECK(store.size() == 2);

    auto snapshot = store.publish();
    CHECK(snapshot->guideCount() == 2);
    CHECK(snapshot->totalNotes == kLeadNotes + 5);
    CHECK(snapshot->guides[0].id == "reaper:lead");
    CHECK(snapshot->guides[1].id == "reaper:doubles");
    CHECK(snapshot->guides[0].origin == "live");
    CHECK(store.findById("reaper:doubles") != nullptr);
    CHECK(store.findById("reaper:lead") != nullptr);
    CHECK(store.findById("reaper:none") == nullptr);

    auto grown = doubles;
    grown.notes.push_back({9.0, 9.9, 64.0f, 0.5f});
    CHECK(store.syncLive({grown}));
    CHECK(store.size() == 2);
    snapshot = store.publish();
    CHECK(snapshot->guides[0].id == "reaper:lead");
    CHECK(snapshot->guides[1].notes == 6);
    CHECK(snapshot->totalNotes == kLeadNotes + 6);

    // Un id nuevo se agrega; el orden de los existentes no se mueve.
    CHECK(store.syncLive({smallGuide("reaper:harmony", 3)}));
    CHECK(store.size() == 3);
    CHECK(store.at(0)->id == "reaper:lead");
    CHECK(store.at(1)->id == "reaper:doubles");
    CHECK(store.at(2)->id == "reaper:harmony");
}

// 111 fantasmas en la MISMA posicion host con cualquier item: el canvas solo
// cambia el offset (frameForRegion), la guia no se mueve.
void test111GhostsSameHostPositionAcrossItems()
{
    const auto guide = leadGuide();
    // El store sirve la guia aunque no haya Project: es del documento.
    DocumentMidiGuideStore store;
    store.bindTrack("track:v2-stem");
    store.upsert(guide, "file");

    const auto tempo = measuredTempoMap();
    CHECK(tempo.isValid());
    CHECK(near(tempo.timeForQuarter(32.0), kGuideHostStart, 1.0e-6));

    const auto ghosts = midiGuideGhostsFor(*store.at(0), tempo);
    CHECK(ghosts.size() == static_cast<size_t>(kLeadNotes));
    CHECK(near(ghosts.front().startHostSeconds, kGuideHostStart, 1.0e-6));
    CHECK(ghosts.back().endHostSeconds > ghosts.front().startHostSeconds);
    CHECK(ghosts.back().endHostSeconds < 76.0);

    // Item 4 (offset 5.818): la guia esta muy antes del item, pero la posicion
    // host no cambia. Con otro item el proyectado cambia exactamente el offset.
    MidiGuideGhostLayer layer;
    layer.set(ghosts);
    CHECK(near(layer.firstHostSeconds(), kGuideHostStart, 1.0e-6));
    CHECK(near(layer.startProjectSeconds(0, 0.0), kGuideHostStart, 1.0e-6));
    CHECK(near(layer.startProjectSeconds(0, kItem4Offset),
               kGuideHostStart - kItem4Offset, 1.0e-6));

    // Y sigue siendo la misma lista con dos publicaciones: no se recompone por
    // item ni se copia desde el Project.
    const auto snapshot = store.publish();
    CHECK(snapshot->guideAt(0) != nullptr);
    CHECK(snapshot->guideAt(0)->notes.data() == store.at(0)->notes.data());
    for (size_t i = 0; i < ghosts.size(); ++i)
        CHECK(near(ghosts[i].startHostSeconds,
                   tempo.timeForQuarter(guide.notes[i].startQuarter), 1.0e-6));
}

// Una pista nueva (otro trackKey) empieza sin guias; la misma pista las
// conserva. Una publicacion vacia (clip mode, sin indice) no borra nada.
void testTrackScopeAndEmptyPublication()
{
    DocumentMidiGuideStore store;
    CHECK(!store.bindTrack("track:v2-stem"));
    store.upsert(leadGuide(), "file");
    CHECK(store.size() == 1);

    CHECK(!store.bindTrack("track:v2-stem")); // misma pista
    CHECK(store.size() == 1);
    CHECK(!store.bindTrack({}));               // clip mode / sin documento
    CHECK(store.size() == 1);

    CHECK(store.bindTrack("track:other"));     // otra pista: otro documento
    CHECK(store.empty());
    CHECK(store.publish()->empty());

    store.observeProjects({});                 // publicacion sin regiones
    store.upsert(leadGuide(), "file");
    store.observeProjects({});
    CHECK(store.size() == 1);
}
} // namespace

int main()
{
    testSwitchingItemKeepsTheGuide();
    testGuideSurvivesItemDeathAndDiesWithTheDocument();
    testSnapshotIsImmutable();
    testLegacyJsonMigrationIsOnce();
    testTwoSources();
    test111GhostsSameHostPositionAcrossItems();
    testTrackScopeAndEmptyPublication();

    std::printf("MidiGuideStoreTests: all checks passed\n");
    return 0;
}
