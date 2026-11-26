// D-308: inclinacion en tres capas independientes.
//
// Criterio escrito ANTES de medir:
//
//   Tres capas que se SUMAN, todas ancladas en el centro de la nota (el
//   cuerpo no se mueve):
//     - tilt          (general): borde izq. = centro + t, centro = centro,
//                     der. = centro - t.
//     - tiltFineLeft  (fino): del borde izquierdo al centro; la mitad derecha
//                     no se toca.
//     - tiltFineRight (fino): del centro al borde derecho; la mitad izquierda
//                     no se toca.
//   Cada control escribe SOLO su capa. Con los finos a 0 se ve exactamente
//   como hoy (equivalencia con la formula vieja tiltLeft/tiltRight).
//
//   Medidas:
//   1) Tilt +1 -> +1 / 0 / -1 st en la curva compuesta (tol. 0,02 st).
//   2) Fino izq. +1 -> borde izq. +1 y la mitad derecha bit a bit igual.
//   3) Fino der. +1 -> simetrico.
//   4) Mover una capa no cambia las otras dos.
//   5) El cuerpo (midiNote) no se mueve en las tres.
//   6) Undo/redo de cada capa.
//   7) JSON: las tres sobreviven; un JSON viejo (solo tiltLeft/tiltRight)
//      da la misma curva.
//   8) Restablecer deja las tres a 0.
//   9) Corte de nota inclinada: costura <= 0,5 c; el caso con vibrato se mide
//      y se informa (limite conocido: (vibrato-1)*tilt, decenas de cents).
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/UI/PianoRoll/NoteSplitter.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/NoteActions.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kFrames = 401; // impar: el centro cae en una trama exacta
constexpr int kCenter = kFrames / 2;

bool near(float a, float b, float tolerance = 1.0e-4f) {
  return std::abs(a - b) <= tolerance;
}

float cents(float hz) {
  return 1200.0f * std::log2(std::max(hz, 1.0e-6f) / 440.0f);
}

float midiAt(const std::vector<float> &f0, int frame) {
  return freqToMidi(f0[static_cast<size_t>(frame)]);
}

// Una nota plana (delta original 0) entre 0 y `frames`, con su base.
Project makeProject(int frames = kFrames, float midi = 60.0f,
                    float vibrato = 1.0f) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.melSpectrogram.assign(static_cast<size_t>(frames),
                              std::vector<float>(1, 0.0f));
  audio.f0.assign(static_cast<size_t>(frames), midiToFreq(midi));
  audio.basePitch.assign(static_cast<size_t>(frames), midi);
  audio.deltaPitch.assign(static_cast<size_t>(frames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(frames), true);

  Note note(0, frames, midi);
  note.setSrcStartFrame(0);
  note.setSrcEndFrame(frames);
  note.setOriginalMidiNote(midi);
  note.setOriginalDeltaPitch(std::vector<float>(static_cast<size_t>(frames), 0.0f));
  note.setVibrato(vibrato);
  project.addNote(note);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

Note &onlyNote(Project &project) { return project.getNotes().front(); }

void rebuild(Project &project) {
  PitchCurveProcessor::rebuildBaseFromNotes(project);
}

// --- 1: las tres capas suman sobre el centro ------------------------------
void testGeneralTilt() {
  Project project = makeProject();
  Note &note = onlyNote(project);
  const float midi0 = note.getMidiNote();

  note.setTilt(1.0f);
  rebuild(project);
  const auto f0 = project.getAdjustedF0();
  const float left = midiAt(f0, 0);
  const float center = midiAt(f0, kCenter);
  const float right = midiAt(f0, kFrames - 1);
  std::printf("D-308 tilt +1: borde izq %.4f (esperado +1) | centro %.4f "
              "(esperado 0) | borde der %.4f (esperado -1)\n",
              left - midi0, center - midi0, right - midi0);
  CHECK(near(left - midi0, 1.0f, 0.02f));
  CHECK(near(center - midi0, 0.0f, 0.02f));
  CHECK(near(right - midi0, -1.0f, 0.02f));
  CHECK(near(note.getMidiNote(), midi0));
}

// --- 2/3: cada fino toca solo su mitad ------------------------------------
void testFineLeftAndRight() {
  {
    Project project = makeProject();
    Note &note = onlyNote(project);
    const auto before = project.getAdjustedF0();
    const float midi0 = note.getMidiNote();
    note.setTiltFineLeft(1.0f);
    rebuild(project);
    const auto after = project.getAdjustedF0();

    float rightDiff = 0.0f;
    int differing = 0;
    for (int i = kCenter; i < kFrames; ++i) {
      if (before[static_cast<size_t>(i)] != after[static_cast<size_t>(i)]) {
        ++differing;
        rightDiff = std::max(rightDiff,
                             std::abs(cents(after[static_cast<size_t>(i)]) -
                                      cents(before[static_cast<size_t>(i)])));
      }
    }
    const float leftDiff = midiAt(after, 0) - midi0;
    std::printf("D-308 fino izq +1: borde izq %+.4f st | mitad derecha: %d "
                "tramas distintas, max %.6f c (exigido 0)\n",
                leftDiff, differing, rightDiff);
    CHECK(near(leftDiff, 1.0f, 0.02f));
    CHECK(differing == 0);
    CHECK(rightDiff == 0.0f);
    CHECK(near(midiAt(after, kCenter) - midi0, 0.0f, 1.0e-4f));
    CHECK(near(note.getMidiNote(), midi0));
  }
  {
    Project project = makeProject();
    Note &note = onlyNote(project);
    const auto before = project.getAdjustedF0();
    const float midi0 = note.getMidiNote();
    note.setTiltFineRight(1.0f);
    rebuild(project);
    const auto after = project.getAdjustedF0();

    float leftDiffCents = 0.0f;
    int differing = 0;
    for (int i = 0; i <= kCenter; ++i) {
      if (before[static_cast<size_t>(i)] != after[static_cast<size_t>(i)]) {
        ++differing;
        leftDiffCents = std::max(leftDiffCents,
                                 std::abs(cents(after[static_cast<size_t>(i)]) -
                                          cents(before[static_cast<size_t>(i)])));
      }
    }
    const float rightDiff = midiAt(after, kFrames - 1) - midi0;
    std::printf("D-308 fino der +1: borde der %+.4f st | mitad izquierda: %d "
                "tramas distintas, max %.6f c (exigido 0)\n",
                rightDiff, differing, leftDiffCents);
    CHECK(near(rightDiff, 1.0f, 0.02f));
    CHECK(differing == 0);
    CHECK(leftDiffCents == 0.0f);
    CHECK(near(midiAt(after, kCenter) - midi0, 0.0f, 1.0e-4f));
    CHECK(near(note.getMidiNote(), midi0));
  }
}

// --- 4/5: independencia y cuerpo inmóvil ---------------------------------
void testIndependenceAndBody() {
  Project project = makeProject();
  Note &note = onlyNote(project);
  const float body = note.getMidiNote();
  const float source = note.getLastNonMacroMidiNote();

  note.setTiltFineLeft(0.6f);
  note.setTiltFineRight(-0.2f);
  const float fineLeft = note.getTiltFineLeft();
  const float fineRight = note.getTiltFineRight();
  note.setTilt(0.9f);
  CHECK(near(note.getTiltFineLeft(), fineLeft));
  CHECK(near(note.getTiltFineRight(), fineRight));

  note.setTiltFineLeft(1.1f);
  CHECK(near(note.getTilt(), 0.9f));
  CHECK(near(note.getTiltFineRight(), fineRight));

  note.setTiltFineRight(0.4f);
  CHECK(near(note.getTilt(), 0.9f));
  CHECK(near(note.getTiltFineLeft(), 1.1f));

  rebuild(project);
  CHECK(near(note.getMidiNote(), body));
  CHECK(near(note.getLastNonMacroMidiNote(), source));
  std::printf("D-308 independencia: general %.2f | finos %.2f / %.2f | cuerpo "
              "%.4f (sin cambio)\n",
              note.getTilt(), note.getTiltFineLeft(), note.getTiltFineRight(),
              note.getMidiNote());
}

// --- 6: undo/redo de cada capa -------------------------------------------
void testUndoRedo() {
  Project project = makeProject();
  Note &note = onlyNote(project);

  auto checkLayer = [&](const char *label, float (Note::*getter)() const,
                        void (Note::*setter)(float), float value) {
    const float old = (note.*getter)();
    (note.*setter)(value);
    NoteFloatPropertyAction action(&note, old, value, setter, label);
    action.undo();
    CHECK(near((note.*getter)(), old));
    action.redo();
    CHECK(near((note.*getter)(), value));
    (note.*setter)(old);
    std::printf("D-308 undo/redo %s: %.2f -> %.2f -> %.2f\n", label, old,
                value, (note.*getter)());
  };

  checkLayer("Tilt", &Note::getTilt, &Note::setTilt, 0.8f);
  checkLayer("Tilt Fine Left", &Note::getTiltFineLeft, &Note::setTiltFineLeft,
             0.5f);
  checkLayer("Tilt Fine Right", &Note::getTiltFineRight,
             &Note::setTiltFineRight, -0.75f);
}

// --- 7: JSON nuevo y migracion del viejo ---------------------------------
void testOldFormulaEquivalence() {
  // La formula vieja con un comun distinto de 0: base = centro - (L+R)/2 y
  // delta = L(1-p) + R p. La migracion tilt=(L-R)/2 debe dar la MISMA curva.
  const float oldLeft = 1.5f;
  const float oldRight = -0.5f;
  const float migratedTilt = (oldLeft - oldRight) * 0.5f; // 1.0
  Project project = makeProject();
  Note &note = onlyNote(project);
  note.setTilt(migratedTilt);
  rebuild(project);
  const auto composed = project.getAdjustedF0();

  const float midi = 60.0f;
  const float oldBase = midi - (oldLeft + oldRight) * 0.5f; // 59.5
  float worst = 0.0f;
  for (int i = 0; i < kFrames; ++i) {
    const float p = static_cast<float>(i) / static_cast<float>(kFrames - 1);
    const float oldMidi = oldBase + oldLeft * (1.0f - p) + oldRight * p;
    worst = std::max(worst, std::abs(cents(composed[static_cast<size_t>(i)]) -
                                     cents(midiToFreq(oldMidi))));
  }
  std::printf("D-308 equivalencia formula vieja (L %.2f / R %.2f -> tilt %.2f): "
              "max diff %.6f c\n",
              oldLeft, oldRight, migratedTilt, worst);
  CHECK(worst <= 1.0e-3f);
}

void testJson() {
  {
    Project project = makeProject();
    Note &note = onlyNote(project);
    note.setTilt(1.0f);
    note.setTiltFineLeft(0.3f);
    note.setTiltFineRight(-0.4f);
    rebuild(project);
    const auto before = project.getAdjustedF0();

    const juce::var json = ProjectSerializer::toJson(project);
    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    const Note &round = loaded.getNotes().front();
    std::printf("D-308 JSON nuevo: %.2f / %.2f / %.2f (guardado %.2f / %.2f "
                "/ %.2f)\n",
                round.getTilt(), round.getTiltFineLeft(),
                round.getTiltFineRight(), note.getTilt(),
                note.getTiltFineLeft(), note.getTiltFineRight());
    CHECK(near(round.getTilt(), 1.0f));
    CHECK(near(round.getTiltFineLeft(), 0.3f));
    CHECK(near(round.getTiltFineRight(), -0.4f));
  }

  // Un JSON viejo (sin `tilt`) migra a tilt=(L-R)/2, finos=0 y la curva queda
  // identica a la del modelo nuevo con esos mismos valores.
  {
    Project project = makeProject();
    Note &note = onlyNote(project);
    // La formula vieja con comun distinto de 0 se migra a (L-R)/2 = 1.0.
    note.setTilt(1.0f);
    rebuild(project);
    const auto expected = project.getAdjustedF0();

    juce::var json = ProjectSerializer::toJson(project);
    juce::var notesVar = json.getProperty("notes", juce::var());
    auto *array = notesVar.getArray();
    CHECK(array != nullptr && array->size() == 1);
    if (array != nullptr && array->size() == 1) {
      auto *obj = array->getReference(0).getDynamicObject();
      CHECK(obj != nullptr);
      if (obj != nullptr) {
        obj->removeProperty("tilt");
        obj->removeProperty("tiltFineLeft");
        obj->removeProperty("tiltFineRight");
        obj->setProperty("tiltLeft", 1.5);
        obj->setProperty("tiltRight", -0.5);
      }
    }

    // Devuelve el array mutado al objeto raiz (por si la copia no comparte
    // almacenamiento con la propiedad original).
    if (auto *root = json.getDynamicObject())
      root->setProperty("notes", notesVar);

    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    const Note &old = loaded.getNotes().front();
    std::printf("D-308 JSON viejo (tiltLeft/tiltRight): migrado a %.4f / "
                "%.4f / %.4f\n",
                old.getTilt(), old.getTiltFineLeft(), old.getTiltFineRight());
    CHECK(near(old.getTilt(), 1.0f));
    CHECK(near(old.getTiltFineLeft(), 0.0f));
    CHECK(near(old.getTiltFineRight(), 0.0f));

    rebuild(loaded);
    const auto migrated = loaded.getAdjustedF0();
    float worst = 0.0f;
    for (size_t i = 0; i < expected.size() && i < migrated.size(); ++i)
      worst = std::max(worst, std::abs(cents(migrated[i]) - cents(expected[i])));
    std::printf("D-308 JSON viejo: curva max diff %.6f c (lim 0,001)\n", worst);
    CHECK(worst <= 1.0e-3f);
  }
}

// --- 8: restablecer deja las tres capas a 0 ------------------------------
void testReset() {
  Project project = makeProject();
  PianoRollComponent roll;
  roll.setSize(600, 300);
  roll.setProject(&project);
  Note &note = onlyNote(project);
  note.setTilt(1.0f);
  note.setTiltFineLeft(0.5f);
  note.setTiltFineRight(-0.75f);
  note.setSelected(true);
  const float body = note.getMidiNote();
  roll.resetSelection(PianoRollComponent::NoteRestoreMode::Pitch);
  std::printf("D-308 restablecer: %.2f / %.2f / %.2f | cuerpo %.4f\n",
              note.getTilt(), note.getTiltFineLeft(), note.getTiltFineRight(),
              note.getMidiNote());
  CHECK(near(note.getTilt(), 0.0f));
  CHECK(near(note.getTiltFineLeft(), 0.0f));
  CHECK(near(note.getTiltFineRight(), 0.0f));
  CHECK(near(note.getMidiNote(), body));
}

// --- 9: corte de una nota inclinada --------------------------------------
struct SplitDiff {
  float body = 0.0f;
  float seam = 0.0f;
};

SplitDiff compareCents(const std::vector<float> &before,
                       const std::vector<float> &after, int seamFrame,
                       int edgeFrames = 6) {
  SplitDiff diff;
  const int n = std::min(static_cast<int>(before.size()),
                         static_cast<int>(after.size()));
  for (int i = 0; i < n; ++i) {
    if (before[static_cast<size_t>(i)] <= 0.0f ||
        after[static_cast<size_t>(i)] <= 0.0f)
      continue;
    const float d = std::abs(cents(after[static_cast<size_t>(i)]) -
                             cents(before[static_cast<size_t>(i)]));
    if (std::abs(i - seamFrame) <= edgeFrames)
      diff.seam = std::max(diff.seam, d);
    else
      diff.body = std::max(diff.body, d);
  }
  return diff;
}

void testSplitInclined(float vibrato, const char *label) {
  Project project = makeProject(kFrames, 60.0f, vibrato);
  Note &note = onlyNote(project);
  note.setTilt(1.0f);
  note.setTiltFineLeft(0.25f);
  note.setTiltFineRight(-0.25f);
  rebuild(project);
  const auto before = project.getAdjustedF0();

  PitchUndoManager history(100);
  NoteSplitter splitter;
  splitter.setProject(&project);
  splitter.setUndoManager(&history);
  CHECK(splitter.splitNoteAtFrame(&note, kCenter));
  CHECK(project.getNotes().size() == 2);
  rebuild(project);
  const auto after = project.getAdjustedF0();
  const auto diff = compareCents(before, after, kCenter);
  std::printf("D-308 corte inclinado (%s, vibrato %.2f): cuerpo %.4f c | "
              "costura %.4f c\n",
              label, vibrato, diff.body, diff.seam);
  if (vibrato >= 0.999f) {
    CHECK(diff.body <= 0.5f);
    CHECK(diff.seam <= 0.5f);
  } else {
    // Limite conocido: el padre se parte con el delta ya horneado (activo +
    // rampas) y luego cada mitad vuelve a aplicar su vibrato; la diferencia es
    // ~(vibrato-1)*tilt (decenas de cents), nunca el salto de 2*tilt.
    CHECK(diff.body < 100.0f);
    CHECK(diff.seam < 100.0f);
  }
  for (auto &half : project.getNotes()) {
    CHECK(near(half.getTilt(), 0.0f));
    CHECK(near(half.getTiltFineLeft(), 0.0f));
    CHECK(near(half.getTiltFineRight(), 0.0f));
  }
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  testGeneralTilt();
  testFineLeftAndRight();
  testIndependenceAndBody();
  testUndoRedo();
  testOldFormulaEquivalence();
  testJson();
  testReset();
  testSplitInclined(1.0f, "sin vibrato");
  // Limite conocido y reportado: el padre se parte con el delta ya horneado
  // (activo + rampas) y luego cada mitad vuelve a aplicar su vibrato, asi que
  // la diferencia es ~(vibrato-1)*tilt (aqui ~20 c), no el salto de 2*tilt.
  testSplitInclined(0.8f, "con vibrato");

  std::printf("TiltIndependenceTests: en verde\n");
  return 0;
}
