// N9 (D-349): una sola seleccion, un solo escritor.
//
// Antes de esta fase habia dos verdades: DocumentNoteSelection y Note::selected,
// y la bandera se escribia desde nueve ficheros. Aqui se pina que:
//   - el conjunto es el unico dueno y las banderas son vista derivada: tras
//     CADA operacion, para cada nota de cada project vivo,
//     note.isSelected() == (esta en el conjunto && no es rest && no es lapida);
//   - marquesina, shift-clic, seleccionar todo, lote, borrar, corte y undo/redo
//     pasan por el conjunto (nadie escribe la bandera a mano);
//   - la seleccion sobrevive al cambio de item y solo se suelta al borrar el
//     project o al recargarlo;
//   - dos ventanas hermanas de la misma toma no cuentan la nota dos veces;
//   - en modo clip (sin indice publicado) las banderas del item son su verdad y
//     se escriben por el mismo modulo, y al volver a pista se readoptan.
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteBatch.h"
#include "../Source/Models/DocumentNoteSelection.h"
#include "../Source/Models/Project.h"
#include "../Source/Undo/NoteDeleteAction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
constexpr int kSampleRate = 44100;
constexpr int kHopSize = 512;

int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * kSampleRate / static_cast<double>(kHopSize)));
}

double secondsFor(int frame)
{
    return static_cast<double>(frame) * kHopSize / kSampleRate;
}

void setLength(Project& project, double seconds)
{
    project.getAudioData().f0.assign(static_cast<size_t>(frameFor(seconds)),
                                     0.0f);
}

// `count` notes of 0.4 s each, spread over [start, end).
void addNotes(Project& project, double start, double end, int count, float midi)
{
    const double span = end - start;
    const double step = span / static_cast<double>(count);
    for (int i = 0; i < count; ++i)
        project.getNotes().push_back(
            Note(frameFor(start + step * i),
                 frameFor(start + step * i) + frameFor(0.4), midi));
}

DocumentNoteRegionSpec makeSpec(const juce::String& key,
                                const juce::String& modification,
                                const juce::String& source, double hostStart,
                                double hostEnd, double sourceStart,
                                double sourceEnd, Project* project)
{
    DocumentNoteRegionSpec spec;
    spec.key = key;
    spec.name = key;
    spec.modificationKey = modification;
    spec.sourceKey = source;
    spec.hostStartSeconds = hostStart;
    spec.hostEndSeconds = hostEnd;
    spec.sourceStartSeconds = sourceStart;
    spec.sourceEndSeconds = sourceEnd;
    spec.project = project;
    return spec;
}

// Three items, two sources: it0 and it1 are sibling windows of the same
// modification (they overlap on purpose, source 8..10 s), it2 is another take.
struct Fixture
{
    Project stems;
    Project other;
    std::vector<DocumentNoteRegionSpec> specs;

    Fixture()
    {
        setLength(stems, 20.0);
        addNotes(stems, 0.2, 19.8, 39, 60.0f);

        setLength(other, 6.0);
        addNotes(other, 0.2, 5.8, 28, 62.0f);

        specs.push_back(makeSpec("it0", "modA", "stems.wav", 0.0, 10.0, 0.0,
                                 10.0, &stems));
        specs.push_back(makeSpec("it1", "modA", "stems.wav", 10.0, 20.0, 8.0,
                                 18.0, &stems));
        specs.push_back(makeSpec("it2", "modB", "other.wav", 20.0, 26.0, 0.0,
                                 6.0, &other));
    }
};

std::vector<std::pair<Project*, Note*>>
pairsOfRegion(const DocumentNoteIndex& index, const juce::String& key)
{
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& region : index.regions)
        if (region.key == key)
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
    return pairs;
}

// THE invariant of the phase: the flags are exactly what the set says.
void checkFlagsMatchSet(DocumentNoteSelection& selection, Project& project)
{
    for (auto& note : project.getNotes())
    {
        const bool inSet = selection.contains(&project, &note);
        const bool expected = inSet && !note.isRest() && !note.isDeleted();
        CHECK(note.isSelected() == expected);
    }
}

int selectedCount(Project& project)
{
    int count = 0;
    for (const auto& note : project.getNotes())
        if (note.isSelected() && !note.isRest() && !note.isDeleted())
            ++count;
    return count;
}

int visibleNotesOf(const DocumentNoteIndex& index, const Project* project)
{
    int count = 0;
    for (const auto& ref : index.notes)
        if (ref.project == project && ref.note != nullptr)
            ++count;
    return count;
}

// (a) Tres items con dos fuentes: la marquesina los alcanza a todos y el
// conjunto lo dice por region y por fuente.
void testThreeItemsTwoSources()
{
    Fixture fixture;
    const auto index = buildDocumentNoteIndex("track", fixture.specs);
    DocumentNoteSelection selection;
    selection.refresh(index);

    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*index, "it0");
    for (const auto& pair : pairsOfRegion(*index, "it1"))
        pairs.push_back(pair);
    for (const auto& pair : pairsOfRegion(*index, "it2"))
        pairs.push_back(pair);
    CHECK(selection.setFromPairs(pairs, *index));

    // The sibling windows share source 8..10 s: those notes count once.
    CHECK(selection.size() == static_cast<int>(index->notes.size()));
    CHECK(selection.countForSource("stems.wav") ==
          selection.countForRegion("it0") + selection.countForRegion("it1"));
    int sum = 0;
    for (const auto& region : index->regions)
        sum += selection.countForRegion(region.key);
    CHECK(sum == selection.size());
    CHECK(selection.countForSource("other.wav") == 28);

    checkFlagsMatchSet(selection, fixture.stems);
    checkFlagsMatchSet(selection, fixture.other);
    CHECK(selectedCount(fixture.other) == 28);
}

// (b) Dos ventanas hermanas de la misma toma: la nota de la costura entra una
// vez, y su region es la de menor indice.
void testSiblingWindowsDedupe()
{
    Fixture fixture;
    const auto index = buildDocumentNoteIndex("track", fixture.specs);
    DocumentNoteSelection selection;
    selection.refresh(index);

    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*index, "it0");
    for (const auto& pair : pairsOfRegion(*index, "it1"))
        pairs.push_back(pair);
    CHECK(selection.setFromPairs(pairs, *index));

    int sum = 0;
    for (const auto& region : index->regions)
        sum += selection.countForRegion(region.key);
    CHECK(sum == selection.size());

    // A note of the 8..10 s overlap is in the set once.
    const auto& overlap = index->regions[0].notes;
    int shared = 0;
    for (const auto& ref : overlap)
        if (ref.note != nullptr &&
            secondsFor(ref.note->getStartFrame()) >= 8.0)
        {
            ++shared;
            CHECK(selection.contains(&fixture.stems, ref.note));
        }
    CHECK(shared > 0);

    // The same note appears in both windows of the index, the set has it once:
    // walking the deduplicated index gives exactly size() hits.
    int occurrences = 0;
    for (const auto& ref : index->notes)
        if (ref.note != nullptr && selection.contains(ref.project, ref.note))
            ++occurrences;
    CHECK(occurrences == selection.size());
}

// (c) Marquesina, shift-clic y "seleccionar todo" pasan por el conjunto, y las
// banderas acompanan en cada paso.
void testMarqueeShiftSelectAll()
{
    Fixture fixture;
    const auto index = buildDocumentNoteIndex("track", fixture.specs);
    DocumentNoteSelection selection;
    selection.refresh(index);

    // Marquee over it2 only.
    CHECK(selection.setFromPairs(pairsOfRegion(*index, "it2"), *index));
    CHECK(selection.size() == 28);
    CHECK(selectedCount(fixture.stems) == 0);
    checkFlagsMatchSet(selection, fixture.stems);
    checkFlagsMatchSet(selection, fixture.other);

    // Shift-click on a note of it0 adds it.
    Note* noteA = index->regions[0].notes.front().note;
    CHECK(selection.toggle(&fixture.stems, noteA, *index));
    CHECK(selection.size() == 29);
    CHECK(noteA->isSelected());
    checkFlagsMatchSet(selection, fixture.stems);

    // Shift-click again removes only that note.
    CHECK(selection.toggle(&fixture.stems, noteA, *index));
    CHECK(selection.size() == 28);
    CHECK(!noteA->isSelected());
    checkFlagsMatchSet(selection, fixture.stems);

    // Track "select all": every visible note, once, nothing invisible.
    CHECK(selection.selectAll(*index));
    CHECK(selection.size() == index->totalNotes);
    checkFlagsMatchSet(selection, fixture.stems);
    checkFlagsMatchSet(selection, fixture.other);
    for (const auto& ref : index->outsideNotes)
        CHECK(ref.note != nullptr && !ref.note->isSelected());
}

// (d) La seleccion sobrevive al cambio de item: el host reordena las ventanas
// (el item 2 pasa a activo) y la republicacion no la toca.
void testSelectionSurvivesItemSwitch()
{
    Fixture fixture;
    const auto before = buildDocumentNoteIndex("track", fixture.specs);
    DocumentNoteSelection selection;
    selection.refresh(before);

    CHECK(selection.setFromPairs(pairsOfRegion(*before, "it2"), *before));
    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*before, "it0");
    CHECK(selection.setFromPairs(pairs, *before));
    const int size = selection.size();
    const auto revision = selection.getRevision();
    CHECK(size == static_cast<int>(before->regions[0].notesInWindow));

    auto reordered = fixture.specs;
    std::rotate(reordered.begin(), reordered.begin() + 2, reordered.end());
    const auto after = buildDocumentNoteIndex("track", reordered);
    selection.refresh(after);
    CHECK(selection.size() == size);
    CHECK(selection.getRevision() == revision); // same membership, no churn
    checkFlagsMatchSet(selection, fixture.stems);
}

// (e) Lote == nota a nota: borrar la seleccion en un gesto deja exactamente el
// mismo estado (y las mismas banderas) que borrarlas una por una.
void testBatchEqualsNoteByNote()
{
    Fixture batch;
    Fixture single;
    const auto batchIndex = buildDocumentNoteIndex("track", batch.specs);
    const auto singleIndex = buildDocumentNoteIndex("track", single.specs);

    DocumentNoteSelection batchSelection;
    batchSelection.refresh(batchIndex);
    DocumentNoteSelection singleSelection;
    singleSelection.refresh(singleIndex);

    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto* key : {"it0", "it2"})
        for (const auto& pair : pairsOfRegion(*batchIndex, key))
            pairs.push_back(pair);
    CHECK(batchSelection.setFromPairs(pairs, *batchIndex));

    std::vector<std::pair<Project*, Note*>> samePairs;
    for (const auto* key : {"it0", "it2"})
        for (const auto& pair : pairsOfRegion(*singleIndex, key))
            samePairs.push_back(pair);
    CHECK(singleSelection.setFromPairs(samePairs, *singleIndex));

    const auto targets = documentNoteBatch::targetsOf(batchSelection, *batchIndex);
    const auto result = documentNoteBatch::deleteNotes(targets);
    CHECK(result.applied());
    CHECK(result.notes == static_cast<int>(targets.size()));

    for (const auto& entry : singleSelection.entries())
    {
        NoteDeleteAction::Entry action{entry.note, false, false, true, true,
                                       true, false};
        NoteDeleteAction deleteAction(*entry.project, {action});
        deleteAction.redo();
    }

    CHECK(batch.stems.getNotes().size() == single.stems.getNotes().size());
    for (size_t i = 0; i < batch.stems.getNotes().size(); ++i)
    {
        const auto& a = batch.stems.getNotes()[i];
        const auto& b = single.stems.getNotes()[i];
        CHECK(a.isDeleted() == b.isDeleted());
        CHECK(a.isRest() == b.isRest());
        CHECK(a.isSelected() == b.isSelected());
    }
    for (size_t i = 0; i < batch.other.getNotes().size(); ++i)
    {
        const auto& a = batch.other.getNotes()[i];
        const auto& b = single.other.getNotes()[i];
        CHECK(a.isDeleted() == b.isDeleted());
        CHECK(a.isRest() == b.isRest());
        CHECK(a.isSelected() == b.isSelected());
    }

    batchSelection.refresh(batchIndex.get());
    singleSelection.refresh(singleIndex.get());
    checkFlagsMatchSet(batchSelection, batch.stems);
    checkFlagsMatchSet(batchSelection, batch.other);
    checkFlagsMatchSet(singleSelection, single.stems);
    checkFlagsMatchSet(singleSelection, single.other);
}

// (f) Undo/redo devuelven la seleccion por el dueno: tras cada paso las
// banderas siguen coincidiendo con el conjunto.
void testUndoRedoKeepsOneTruth()
{
    Fixture fixture;
    const auto index = buildDocumentNoteIndex("track", fixture.specs);
    DocumentNoteSelection selection;
    selection.refresh(index);

    std::vector<std::pair<Project*, Note*>> pairs =
        pairsOfRegion(*index, "it0");
    CHECK(selection.setFromPairs(pairs, *index));
    const int selectedBefore = selection.size();
    CHECK(selectedBefore > 0);

    std::vector<NoteDeleteAction::Entry> entries;
    for (auto& note : fixture.stems.getNotes())
        if (note.isSelected())
            entries.push_back({&note, false, false, true, true, true, false});
    CHECK(entries.size() == static_cast<size_t>(selectedBefore));

    NoteDeleteAction action(fixture.stems, entries);
    action.redo(); // delete
    CHECK(selectedCount(fixture.stems) == 0);
    selection.refresh(index.get());
    CHECK(selection.isEmpty());
    checkFlagsMatchSet(selection, fixture.stems);

    action.undo(); // give the notes back, selected again
    selection.refresh(index.get());
    CHECK(selection.size() == selectedBefore);
    checkFlagsMatchSet(selection, fixture.stems);
    CHECK(selectedCount(fixture.stems) == selectedBefore);

    action.redo();
    selection.refresh(index.get());
    CHECK(selection.isEmpty());
    checkFlagsMatchSet(selection, fixture.stems);
}

// (g) Un corte (edicion estructural) hereda la seleccion por el conjunto: el
// addNote mueve las notas, asi que los punteros se readoptan desde las banderas
// y la mitad nueva entra sin que nadie copie la bandera a mano.
void testSplitInheritsSelectionThroughSet()
{
    Fixture fixture;
    const auto index = buildDocumentNoteIndex("track", fixture.specs);
    DocumentNoteSelection selection;
    selection.refresh(index);

    Note* origin = index->regions[0].notes.at(1).note;
    CHECK(selection.setFromPairs({{&fixture.stems, origin}}, *index));
    CHECK(origin->isSelected());
    auto& notes = fixture.stems.getNotes();
    const int position = static_cast<int>(std::distance(
        notes.begin(), std::find_if(notes.begin(), notes.end(),
                                    [origin](const Note& note)
                                    { return &note == origin; })));
    const int startFrame = origin->getStartFrame();
    const int endFrame = origin->getEndFrame();
    const int splitFrame = (startFrame + endFrame) / 2;
    const float midi = origin->getMidiNote();

    // What NoteSplitter::splitNoteAtFrame does around project->addNote().
    const bool inherit = origin->isSelected();
    notes[static_cast<size_t>(position)].setEndFrame(splitFrame);
    Note* secondHalf = fixture.stems.addNote(Note(splitFrame, endFrame, midi));
    CHECK(secondHalf != nullptr);
    DocumentNoteSelection::inheritSelectionToNewNote(fixture.stems,
                                                     *secondHalf, inherit);

    // The index is republished with the new notes and the set re-derives.
    const auto after = buildDocumentNoteIndex("track", fixture.specs);
    selection.refresh(after);
    CHECK(fixture.stems.getNotes()[static_cast<size_t>(position)].isSelected());
    CHECK(secondHalf->isSelected());
    CHECK(selection.size() == 2);
    checkFlagsMatchSet(selection, fixture.stems);
}

// (h) Modo clip: sin indice publicado el item es su propia verdad y se escribe
// por el mismo modulo; al publicar el documento el conjunto manda, y al
// volver a clip la seleccion no se pierde.
void testItemModeAndBackToTrack()
{
    Fixture fixture;
    Project lonely;
    setLength(lonely, 6.0);
    addNotes(lonely, 0.2, 5.8, 10, 60.0f);

    // Item mode: no owner, the flags are the truth.
    lonely.selectAllNotes();
    CHECK(selectedCount(lonely) == 10);
    lonely.deselectAllNotes();
    CHECK(selectedCount(lonely) == 0);
    Note& one = lonely.getNotes()[3];
    DocumentNoteSelection::setNoteSelected(lonely, one, true);
    CHECK(one.isSelected());
    DocumentNoteSelection::setNoteSelected(lonely, one, false);
    CHECK(!one.isSelected());

    // The document is published: the same calls become set changes.
    Fixture track;
    const auto index = buildDocumentNoteIndex("track", track.specs);
    DocumentNoteSelection selection;
    selection.refresh(index);
    CHECK(track.stems.noteSelection == &selection);
    CHECK(track.other.noteSelection == &selection);

    track.stems.selectAllNotes();
    CHECK(selection.size() == visibleNotesOf(*index, &track.stems));
    checkFlagsMatchSet(selection, track.stems);
    checkFlagsMatchSet(selection, track.other);

    track.stems.deselectAllNotes();
    CHECK(selection.countForRegion("it0") == 0);
    CHECK(selection.countForRegion("it1") == 0);
    checkFlagsMatchSet(selection, track.stems);

    // Back to clip: no index, nothing derived, and the flags keep the selection.
    CHECK(selection.setFromPairs(pairsOfRegion(*index, "it2"), *index));
    CHECK(selectedCount(track.other) == 28);
    selection.refresh(nullptr);
    CHECK(track.stems.noteSelection == nullptr);
    CHECK(track.other.noteSelection == nullptr);
    CHECK(selection.isEmpty());
    // Nothing is derived while no document is published, so the item keeps what
    // the user selected: the selection survives the mode switch.
    CHECK(selectedCount(track.other) == 28);
    CHECK(track.other.getNotes()[0].isSelected());
}

// (i) Un elemento que se recarga (project vacio) no puede seguir seleccionado.
void testReloadDropsStaleNotes()
{
    Fixture fixture;
    const auto index = buildDocumentNoteIndex("track", fixture.specs);
    DocumentNoteSelection selection;
    selection.refresh(index);
    CHECK(selection.selectAll(*index));
    CHECK(!selection.isEmpty());

    // The item is reloaded: its notes are gone.
    fixture.stems.getNotes().clear();
    const auto reloaded = buildDocumentNoteIndex("track", fixture.specs);
    selection.refresh(reloaded);
    CHECK(selection.size() == 28);
    CHECK(selection.countForSource("stems.wav") == 0);
    checkFlagsMatchSet(selection, fixture.other);
}
} // namespace

int main()
{
    testThreeItemsTwoSources();
    testSiblingWindowsDedupe();
    testMarqueeShiftSelectAll();
    testSelectionSurvivesItemSwitch();
    testBatchEqualsNoteByNote();
    testUndoRedoKeepsOneTruth();
    testSplitInheritsSelectionThroughSet();
    testItemModeAndBackToTrack();
    testReloadDropsStaleNotes();
    std::printf("DocumentSelectionSingleSourceTests: OK\n");
    return 0;
}
