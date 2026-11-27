// R5d (guia MIDI, D-358): vida y clave del store de guias.
//
// R5c midio con la build de R5b que el store estaba bien colocado pero moria con
// el editor (MainComponent) y que bindTrack se vaciaba porque la clave de pista
// era la DIRECCION del ARA::PlugIn::RegionSequence, que el host recrea al cerrar
// y abrir el editor. Resultado medido: 111 fantasmas solo en la primera parada
// del montaje 4 -> 1 -> 2 -> 4 y 0 en las otras tres.
//
// Aqui se mide el contrato de esta fase, sin ARA (no hay host en un test):
//  (a) dos RegionSequence distintos del mismo documento dan la MISMA clave, y
//      el store no se vacia al recrear la secuencia;
//  (b) destruir y recrear la vista/editor no pierde la guia ni pide sync;
//  (c) una clave desconocida importa de la cache del Project, no deja la lista
//      vacia;
//  (d) una clave de otra pista no se mezcla con la anterior.
// Fixture = la guia Lead real medida en R5 (13,704374 s / 111 notas).
#include "TestAssert.h"

#include "../Source/Models/Project.h" // pulls JuceHeader first
#include "../Source/Models/DocumentMidiGuides.h"
#include "../Source/Models/MidiGuideGhosts.h"
#include "../Source/Models/TrackIdentity.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr double kGuideHostStart = 13.704374;
constexpr int kLeadNotes = 111;

bool near(double a, double b, double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

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

int ghostCount(const DocumentMidiGuideStore& store, int index = 0)
{
    const auto* guide = store.at(index);
    if (guide == nullptr)
        return 0;
    return static_cast<int>(midiGuideGhostsFor(*guide, measuredTempoMap()).size());
}

// (a) La clave de pista no depende de ningun objeto ni de su direccion: el
// host recrea la RegionSequence al cerrar y abrir el editor (R5c) y el
// ARADocument al cerrar y abrir el proyecto (R11). R11d (D-367) quita tambien
// la huella de las fuentes, que es la lista de items: la clave es el nombre de
// la pista. Y el store no se vacia al re-bindear.
void testRecreatedRegionSequenceKeepsTheKeyAndTheGuide()
{
    // Dos "sesiones" distintas con el mismo nombre: no hay direccion, ni
    // documento, ni fuentes, ni ordinal que pueda cambiarla.
    const auto sequenceBefore = trackKeyForTrack("TuneX", 2);
    const auto sequenceAfterRecreation = trackKeyForTrack("TuneX", 0);
    CHECK(sequenceBefore == sequenceAfterRecreation);
    CHECK(sequenceBefore.isNotEmpty());
    CHECK(sequenceBefore == "name:TuneX");

    // Mover, anadir o quitar items de la pista (R11c G-B2) no cambia la clave:
    // el conjunto de fuentes ya no entra en ella.
    CHECK(trackKeyForTrack("TuneX", 5) == sequenceBefore);
    // Otro nombre es otra pista.
    CHECK(trackKeyForTrack("Lead", 2) != sequenceBefore);

    // El ordinal solo entra cuando el host no manda nombre.
    CHECK(trackKeyForTrack("", 2) != trackKeyForTrack("", 3));
    CHECK(trackKeyForTrack("", 2) == trackKeyForTrack("", 2));

    DocumentMidiGuideStore store;
    CHECK(!store.bindTrack(sequenceBefore));
    store.upsert(leadGuide(), "file");
    CHECK(store.size() == 1);

    // La secuencia se recrea: misma clave, no se toca nada y no hay sync.
    const auto snapshot = store.publish();
    const GuideSource* canonical = store.at(0);
    CHECK(!store.bindTrack(sequenceAfterRecreation));
    CHECK(store.size() == 1);
    CHECK(store.publish() == snapshot);          // misma publicacion
    CHECK(store.at(0) == canonical);             // misma guia canonica
    CHECK(ghostCount(store) == kLeadNotes);
}

// (b) La vista/editor se destruye y se recrea; el store es del documento, asi
// que sobrevive. La vista nueva ve los 111 fantasmas sin pedir un sync.
struct FakeEditorView
{
    DocumentMidiGuideStore* store = nullptr;

    void open(DocumentMidiGuideStore* documentStore) { store = documentStore; }
    void close() { store = nullptr; }
    int ghosts() const { return store != nullptr ? ghostCount(*store) : 0; }
};

void testGuideSurvivesEditorCloseAndReopen()
{
    // El "documento" (PitchNetDocumentController) posee el store.
    DocumentMidiGuideStore documentStore;
    documentStore.bindTrack(trackKeyForTrack("TuneX", 2));
    documentStore.upsert(leadGuide(), "file");
    const auto published = documentStore.publish();

    FakeEditorView first;
    first.open(&documentStore);
    CHECK(first.ghosts() == kLeadNotes);

    first.close(); // el editor se cierra: solo suelta el prestamo
    CHECK(documentStore.size() == 1); // el documento sigue vivo

    FakeEditorView second;
    second.open(&documentStore);
    CHECK(second.ghosts() == kLeadNotes);       // sin sync
    CHECK(documentStore.publish() == published); // ni republicacion
    CHECK(near(midiGuideGhostsFor(*documentStore.at(0), measuredTempoMap())
                   .front()
                   .startHostSeconds,
               kGuideHostStart));
}

// (c) Una clave desconocida (p.ej. la primera publicacion tras recrear la
// secuencia, o una pista renombrada) NO deja la lista vacia si el documento
// conserva la cache en algun Project: se importa.
void testUnknownKeyImportsInsteadOfEmptying()
{
    Project item;
    item.getMidiGuides().push_back(leadGuide());

    DocumentMidiGuideStore store;
    CHECK(!store.bindTrack("track:old"));
    store.observeProjects({&item});
    CHECK(store.importFromProject(item));
    CHECK(store.size() == 1);

    // Clave nueva/desconocida: no hereda la lista anterior, pero importa la
    // cache del documento (el mismo Project sigue observado).
    CHECK(store.bindTrack("track:new"));
    CHECK(store.size() == 1);
    CHECK(store.at(0) != nullptr);
    CHECK(store.at(0)->id == "reaper:lead");
    CHECK(ghostCount(store) == kLeadNotes);
}

// (d) Clave de otra pista: no se mezcla. Sin cache en ningun Project, la lista
// queda vacia, que es el unico caso en que vaciar es valido.
void testAnotherTrackKeyDoesNotMix()
{
    Project emptyItem;
    DocumentMidiGuideStore store;
    store.bindTrack("track:first");
    store.observeProjects({&emptyItem});
    store.upsert(leadGuide(), "file");
    CHECK(store.size() == 1);

    CHECK(store.bindTrack("track:second"));
    CHECK(store.empty());
    CHECK(store.publish()->empty());
    CHECK(ghostCount(store) == 0);
}
} // namespace

int main()
{
    testRecreatedRegionSequenceKeepsTheKeyAndTheGuide();
    testGuideSurvivesEditorCloseAndReopen();
    testUnknownKeyImportsInsteadOfEmptying();
    testAnotherTrackKeyDoesNotMix();

    std::printf("MidiGuideStoreLifetimeTests: all checks passed\n");
    return 0;
}
