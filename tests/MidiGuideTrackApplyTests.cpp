// R9 (guia MIDI, D-362): aplicar el plan por pista (SCOPE=A) por el camino real
// del editor y deshacerlo en UN solo paso.
//
// Se mide:
//  - (1) MainComponent + DocumentNoteIndex + store de guias, con las ventanas
//        medidas del item 2 y del item 4: la guia de 111 notas deja 55 cambios
//        en el item 2 (todas a 60), el item 4 no cambia, 53 quedan fuera de
//        todo item (no-item) y 3 dentro sin nota (no-match); el JSON dice lo
//        mismo y `scope=active` sigue dando los 55 de R1 para el item activo;
//  - un solo undo devuelve TODOS los items (y sus curvas) al estado anterior y
//    un solo redo los deja como el gesto; el nombre del paso es el de la accion
//    compuesta;
//  - (2) el aplicador puro: mismo estado -> mismo orden y mismos cambios,
//        ninguna nota se escribe dos veces aunque el plan la repita, y los items
//        que no reciben edicion no se tocan.
#include "TestAssert.h"

#include "../Source/Audio/EditorController.h"
#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/GuideApplicationPlan.h"
#include "../Source/Models/MidiGuideTrackApply.h"
#include "../Source/UI/MainComponent.h"
#include "../Source/Undo/DocumentBatchAction.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Localization.h"
#include "../Source/Utils/NotePitch.h"
#include "../Source/Utils/OnnxRuntime.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "../Source/Utils/TransformParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// Friend of MainComponent (see there): the editor without a host around it.
// It is at global scope on purpose: that is the name the class befriends.
struct MainComponentTestAccess
{
    MainComponent &mc;

    void wire(EditorController &controller, PitchUndoManager &history,
              Project &active)
    {
        mc.editorController = &controller;
        mc.bindUndoManager(&history);
        mc.pianoRoll.setProject(&active);
        mc.pianoRoll.setUndoManager(&history);
        mc.pianoRoll.setTimelineDisplayOffset(5.818184); // projectToTimeline(0)
    }

    void addGuide(const GuideSource &guide)
    {
        mc.midiGuideStore->upsert(guide);
        mc.refreshMidiGuideUi();
    }

    juce::var apply(const char *scope) { return mc.applyMidiGuideWithScope(scope); }

    juce::String statusText() { return mc.midiGuidePanel.getStatusText(); }
};

namespace
{
constexpr int kSampleRate = 44100;
constexpr int kHopSize = 512;
constexpr double kOffset = 5.818184; // projectToTimeline(0), medido

int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * kSampleRate / static_cast<double>(kHopSize)));
}

Note makeNote(double startSeconds, double endSeconds, float midi)
{
    return Note(frameFor(startSeconds), frameFor(endSeconds), midi);
}

void setLength(Project &project, double seconds)
{
    project.getAudioData().sampleRate = kSampleRate;
    const int frames = frameFor(seconds);
    project.getAudioData().melSpectrogram.assign(
        static_cast<size_t>(frames), std::vector<float>(1, 0.0f));
    project.getAudioData().f0.assign(static_cast<size_t>(frames), 0.0f);
}

// Guia calibrada a la medida R1/R8: 58 notas dentro de la ventana del item 2
// (55 con nota debajo, 3 sin) y 53 fuera, en el hueco hasta el item 4.
std::vector<GuideNote> measuredGuide()
{
    std::vector<GuideNote> notes;
    const int inside = 58;
    const double insideStart = 16.6;
    const double insideEnd = 40.9;
    const double inStep = (insideEnd - insideStart) / inside;
    for (int i = 0; i < inside; ++i)
    {
        GuideNote note;
        const double start = insideStart + inStep * i;
        note.startQuarter = start * 2.0; // 120 BPM
        note.endQuarter = (start + inStep * 0.9) * 2.0;
        note.pitch = 60.0f;
        notes.push_back(note);
    }
    const int outside = 53;
    const double outsideStart = 41.0;
    const double outsideEnd = 73.9;
    const double outStep = (outsideEnd - outsideStart) / outside;
    for (int i = 0; i < outside; ++i)
    {
        GuideNote note;
        const double start = outsideStart + outStep * i;
        note.startQuarter = start * 2.0;
        note.endQuarter = (start + outStep * 0.9) * 2.0;
        note.pitch = 60.0f;
        notes.push_back(note);
    }
    return notes;
}

DocumentNoteRegionSpec makeSpec(const char *key, const char *modification,
                                const char *source, double hostStart,
                                double hostEnd, double sourceStart,
                                double sourceEnd, Project *project)
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

struct NoteValues
{
    TransformParams params;
    bool muted = false;
    bool deleted = false;
    bool rest = false;
    bool selected = false;
    int startFrame = 0;
    int endFrame = 0;

    static NoteValues of(const Note &note)
    {
        NoteValues values;
        values.params = TransformParams::fromNote(note);
        values.muted = note.isMuted();
        values.deleted = note.isDeleted();
        values.rest = note.isRest();
        values.selected = note.isSelected();
        values.startFrame = note.getStartFrame();
        values.endFrame = note.getEndFrame();
        return values;
    }

    bool sameAs(const NoteValues &other) const
    {
        return params == other.params && muted == other.muted &&
               deleted == other.deleted && rest == other.rest &&
               selected == other.selected && startFrame == other.startFrame &&
               endFrame == other.endFrame;
    }
};

std::vector<NoteValues> valuesOf(const Project &project)
{
    std::vector<NoteValues> values;
    for (const auto &note : project.getNotes())
        values.push_back(NoteValues::of(note));
    return values;
}

bool valuesEqual(const std::vector<NoteValues> &a,
                 const std::vector<NoteValues> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!a[i].sameAs(b[i]))
            return false;
    return true;
}

std::vector<float> curveOf(const Project &project)
{
    std::vector<float> curve;
    const auto &audio = project.getAudioData();
    curve.insert(curve.end(), audio.basePitch.begin(), audio.basePitch.end());
    curve.insert(curve.end(), audio.deltaPitch.begin(), audio.deltaPitch.end());
    curve.insert(curve.end(), audio.f0.begin(), audio.f0.end());
    return curve;
}

bool sameCurve(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size())
        return false;
    return a.empty() ||
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

int countPitched(const Project &project, float midi)
{
    int count = 0;
    for (const auto &note : project.getNotes())
        if (std::abs(note.getMidiNote() - midi) <= 1.0e-4f)
            ++count;
    return count;
}

int intProperty(const juce::var &object, const char *name)
{
    return static_cast<int>(object.getProperty(name, 0));
}

//============================================================================
// (1) La pista medida, por el camino real del editor
//============================================================================
void testMeasuredPairThroughEditor()
{
    EditorController controller(false);
    PitchUndoManager history(100);
    Project item4;
    MainComponent mc(false); // plug-in: no audio device

    auto *item2 = controller.getProject();
    CHECK(item2 != nullptr);
    if (item2 == nullptr)
        return;

    setLength(*item2, 35.157);
    setLength(item4, 115.787);
    item2->getAudioData().playbackRegionRanges = {{10.737, 35.157}};

    const auto guideNotes = measuredGuide();
    // Item 2: una nota bajo las 55 primeras guias (las 3 ultimas quedan sin).
    for (int i = 0; i < 55; ++i)
    {
        const double start = guideNotes[static_cast<size_t>(i)].startQuarter / 2.0;
        const double end = guideNotes[static_cast<size_t>(i)].endQuarter / 2.0;
        item2->getNotes().push_back(makeNote(start - kOffset, end - kOffset, 57.0f));
    }
    // Item 4: notas dentro de su ventana, pero la guia no llega hasta ella.
    for (int i = 0; i < 110; ++i)
    {
        const double start = 70.2 + i * 0.4;
        item4.getNotes().push_back(makeNote(start, start + 0.3, 57.0f));
    }

    PitchCurveProcessor::rebuildBaseFromNotes(*item2);
    PitchCurveProcessor::rebuildBaseFromNotes(item4);

    MainComponentTestAccess access{mc};
    access.wire(controller, history, *item2);

    // El indice es el del documento: dos items del mismo stem, item 4 aparte.
    std::vector<DocumentNoteRegionSpec> specs = {
        makeSpec("m2", "m2", "v2-stem", 16.555, 40.975, 10.737, 35.157, item2),
        makeSpec("m4", "m4", "v2-stem", 76.016413, 121.605056, 70.198229,
                 115.786872, &item4),
    };
    mc.updateDocumentNoteIndex(buildDocumentNoteIndex("track-r9", specs));

    GuideSource guide;
    guide.id = "measured-r9";
    guide.label = "Lead";
    guide.notes = guideNotes;
    access.addGuide(guide);

    const auto item2Before = valuesOf(*item2);
    const auto item4Before = valuesOf(item4);
    const auto item2Curve = curveOf(*item2);
    const auto item4Curve = curveOf(item4);

    // SCOPE=A (por defecto): los dos items se resuelven, la guia solo aplica al 2.
    auto report = access.apply("track");
    CHECK(report.getProperty("scope", juce::var()).toString() == "track");
    CHECK(intProperty(report, "matched") == 55);
    CHECK(intProperty(report, "changed") == 55);
    const auto skipped = report.getProperty("skipped", juce::var());
    CHECK(intProperty(skipped, "no_item") == 53);
    CHECK(intProperty(skipped, "no_match") == 3);
    CHECK(intProperty(skipped, "not_analyzed") == 0);
    CHECK(intProperty(skipped, "different_source") == 0);
    CHECK(!static_cast<bool>(
        report.getProperty("render_path_missing", juce::var(false))));

    // Los items tocados: solo el 2, con su clave, y el paso de undo anunciado.
    const auto items = report.getProperty("items", juce::var());
    CHECK(items.isArray());
    CHECK(items.size() == 1);
    CHECK(items.size() == 1 && items[0].toString() == "m2");
    const juce::String undoName = "Apply MIDI guide to track";
    CHECK(report.getProperty("undo_name", juce::var()).toString() == undoName);
    CHECK(access.statusText().contains("55"));
    CHECK(access.statusText().contains("53"));

    // El item 2 cambio lo suyo; el 4 quedo exactamente igual.
    CHECK(countPitched(*item2, 60.0f) == 55);
    CHECK(countPitched(*item2, 57.0f) == 0);
    CHECK(valuesEqual(valuesOf(item4), item4Before));
    CHECK(sameCurve(curveOf(item4), item4Curve));
    // Sin duplicados: 55 notas cambiadas, ninguna dos veces.
    const auto item2After = valuesOf(*item2);
    CHECK(item2After.size() == item2Before.size());

    // Un solo paso de undo para los dos items, con el nombre compuesto.
    CHECK(history.canUndo());
    CHECK(history.getUndoName() == undoName);
    CHECK(history.undo());
    CHECK(valuesEqual(valuesOf(*item2), item2Before));
    CHECK(valuesEqual(valuesOf(item4), item4Before));
    CHECK(sameCurve(curveOf(*item2), item2Curve));
    CHECK(!history.canUndo());

    // Un solo redo deja el gesto como estaba.
    CHECK(history.redo());
    CHECK(valuesEqual(valuesOf(*item2), item2After));
    CHECK(countPitched(*item2, 60.0f) == 55);
    CHECK(valuesEqual(valuesOf(item4), item4Before));

    // Mismo estado -> mismo resultado: se vuelve atras y se repite.
    CHECK(history.undo());
    auto again = access.apply("track");
    CHECK(intProperty(again, "matched") == intProperty(report, "matched"));
    CHECK(intProperty(again, "changed") == intProperty(report, "changed"));
    CHECK(valuesEqual(valuesOf(*item2), item2After));
    CHECK(valuesEqual(valuesOf(item4), item4Before));
    CHECK(history.undo());

    // scope=active sigue siendo R1: 55 cambios en el item activo, item 4 intacto.
    auto active = access.apply("active");
    CHECK(active.getProperty("scope", juce::var()).toString() == "active");
    CHECK(intProperty(active, "matched") == 55);
    CHECK(intProperty(active, "changed") == 55);
    const auto activeItems = active.getProperty("items", juce::var());
    CHECK(activeItems.size() == 1);
    CHECK(activeItems.size() == 1 && activeItems[0].toString() == "m2");
    CHECK(countPitched(*item2, 60.0f) == 55);
    CHECK(valuesEqual(valuesOf(item4), item4Before));
    CHECK(history.undo());
    CHECK(valuesEqual(valuesOf(*item2), item2Before));

    // El texto de R1 sigue siendo el suyo (las notas de la ventana activa), no
    // el de pista: no nombra los huecos de los demas items.
    CHECK(access.statusText().contains("55"));
}

//============================================================================
// (2) El aplicador puro: orden, sin duplicados y sin tocar lo ajeno
//============================================================================
GuideApplicationRegion makeRegion(const char *modification, double hostStart,
                                  double sourceStart, double offset,
                                  const Project *project)
{
    GuideApplicationRegion region;
    region.modificationKey = modification;
    region.sourceKey = "stem";
    region.hostStartSeconds = hostStart;
    region.hostEndSeconds = hostStart + 5.0;
    region.sourceStartSeconds = sourceStart;
    region.sourceEndSeconds = sourceStart + 5.0;
    region.offsetSeconds = offset;
    region.project = project;
    return region;
}

void testOrderAndNoDuplicates()
{
    // Dos items editables y uno que la guia no alcanza.
    Project a;
    Project b;
    Project untouched;
    setLength(a, 10.0);
    setLength(b, 10.0);
    setLength(untouched, 10.0);
    a.getNotes().push_back(makeNote(0.5, 1.0, 57.0f));
    a.getNotes().push_back(makeNote(2.5, 3.0, 57.0f));
    b.getNotes().push_back(makeNote(0.5, 1.0, 55.0f));
    untouched.getNotes().push_back(makeNote(0.5, 1.0, 50.0f));

    std::vector<GuideApplicationRegion> regions = {
        makeRegion("b1", 10.0, 0.0, 10.0, &b),
        makeRegion("a1", 0.0, 0.0, 0.0, &a),
        makeRegion("zz", 20.0, 0.0, 20.0, &untouched),
    };

    GuideSource guide;
    guide.id = "synthetic-r9";
    for (const auto &entry :
         {std::pair<double, double>{0.4, 1.1}, {2.4, 3.1}, {10.4, 11.1}})
    {
        GuideNote note;
        note.startQuarter = entry.first * 2.0;
        note.endQuarter = entry.second * 2.0;
        note.pitch = 60.0f;
        guide.notes.push_back(note);
    }

    MidiGuideSampling sampling{kSampleRate, kHopSize};
    const auto plan =
        planGuideApplication(guide, HostTempoMap::constant(120.0), regions,
                             sampling);
    CHECK(plan.modifications.size() == 2);
    CHECK(plan.appliesTo("a1"));
    CHECK(plan.appliesTo("b1"));
    CHECK(!plan.appliesTo("zz"));

    // El plan se pasa con una edicion repetida a proposito: el aplicador escribe
    // una sola vez, aunque el plan (defensivamente) la traiga dos veces.
    auto doubled = plan;
    auto &aPlan = doubled.modifications.front();
    CHECK(aPlan.modificationKey == "a1");
    aPlan.edits.push_back(aPlan.edits.front());

    const auto beforeA = valuesOf(a);
    const auto beforeB = valuesOf(b);
    auto result = applyMidiGuideTrackPlan(doubled, "Apply MIDI guide to track");
    CHECK(result.matchedNotes == 3);
    CHECK(result.changedNotes == 3);
    CHECK(result.duplicateEdits == 1);
    CHECK(result.changedNotes == static_cast<int>(result.changes.size()));
    CHECK(result.modificationKeys.size() == 2); // orden del plan: a1, b1
    CHECK(result.modificationKeys[0] == "a1");
    CHECK(result.modificationKeys[1] == "b1");
    CHECK(a.getNotes()[0].getMidiNote() == 60.0f);
    CHECK(a.getNotes()[1].getMidiNote() == 60.0f);
    CHECK(b.getNotes()[0].getMidiNote() == 60.0f);
    CHECK(untouched.getNotes()[0].getMidiNote() == 50.0f);

    // Mismo estado -> mismos cambios y mismo orden en una copia limpia.
    Project a2;
    Project b2;
    setLength(a2, 10.0);
    setLength(b2, 10.0);
    a2.getNotes().push_back(makeNote(0.5, 1.0, 57.0f));
    a2.getNotes().push_back(makeNote(2.5, 3.0, 57.0f));
    b2.getNotes().push_back(makeNote(0.5, 1.0, 55.0f));
    std::vector<GuideApplicationRegion> regions2 = {
        makeRegion("a1", 0.0, 0.0, 0.0, &a2),
        makeRegion("b1", 10.0, 0.0, 10.0, &b2),
    };
    const auto plan2 =
        planGuideApplication(guide, HostTempoMap::constant(120.0), regions2,
                             sampling);
    const auto result2 =
        applyMidiGuideTrackPlan(plan2, "Apply MIDI guide to track");
    CHECK(result2.changedNotes == 3);
    CHECK(result2.modificationKeys == result.modificationKeys);
    CHECK(valuesEqual(valuesOf(a2), valuesOf(a)));
    CHECK(valuesEqual(valuesOf(b2), valuesOf(b)));

    // Y el paso compuesto deshace los dos items a la vez.
    PitchUndoManager history;
    history.addAction(std::make_unique<documentNoteBatch::DocumentBatchAction>(
        std::move(result.changes), nullptr, result.undoName));
    CHECK(history.undo());
    CHECK(valuesEqual(valuesOf(a), beforeA));
    CHECK(valuesEqual(valuesOf(b), beforeB));
    CHECK(history.redo());
    CHECK(a.getNotes()[0].getMidiNote() == 60.0f);
    CHECK(b.getNotes()[0].getMidiNote() == 60.0f);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    // El EditorController crea detectores que enlazan ONNX Runtime; el editor
    // real lo carga de su bundle, un test de la libreria que enlaza.
    CHECK(OnnxRuntime::useLinkedRuntimeForTesting());
    Localization::getInstance().setLanguage("en");

    testMeasuredPairThroughEditor();
    testOrderAndNoDuplicates();

    std::printf("MidiGuideTrackApplyTests: all checks passed\n");
    return 0;
}
