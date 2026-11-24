// D-285/3: la curva tiene que seguir a las notas SIEMPRE.
//
// Medido en vivo (sesión del usuario, 2026-11-24): transpuso las notas +7
// semitonos a propósito y la curva se quedó 7 semitonos por debajo, así que el
// dibujo y el audio dejaron de corresponder a lo que las notas dicen. La regla
// que se comprueba aquí: después de CUALQUIER gesto de altura, la curva
// compuesta (base + delta, lo que se dibuja y se sintetiza) tiene que quedar en
// el centro de las notas: `Project::maxCurveNotesDisagreement()` <= 0,1 st.
//
// Se recorre cada gesto con su camino real de UI (el mismo que usa el ratón):
// arrastre de una nota, arrastre de una selección, flechas del teclado, doble
// clic (afinar al 100 %), inspector y Cortar/Unir.
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr float kTolerance = 0.1f;
constexpr int kFrames = 600;

void prepareAudio(Project &project) {
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  // D-286: sin mel no hay `AudioData::getNumFrames()` (sale de su tamano), y
  // `rebuildBaseFromNotes` se quedaba en 0 frames: limpiaba base/delta y este
  // test medía sobre una curva vacía (0,0000 st por no haber frames, no por
  // estar alineada).
  audio.melSpectrogram.assign(static_cast<size_t>(kFrames),
                             std::vector<float>(1, 0.0f));
  audio.f0.assign(static_cast<size_t>(kFrames), 261.6f);
  audio.basePitch.assign(static_cast<size_t>(kFrames), 60.0f);
  audio.deltaPitch.assign(static_cast<size_t>(kFrames), 0.0f);
}

Note makeNote(int start, int end, float midi) {
  Note note(start, end, midi);
  note.setSrcStartFrame(start);
  note.setSrcEndFrame(end);
  note.setOriginalMidiNote(midi);
  note.setDeltaPitch(std::vector<float>(static_cast<size_t>(end - start), 0.0f));
  return note;
}

struct Fixture {
  Project project;
  PianoRollComponent roll;
  PitchUndoManager history;
  Fixture() {
    prepareAudio(project);
    project.addNote(makeNote(0, 100, 60.3f));
    project.addNote(makeNote(100, 200, 61.8f));
    project.addNote(makeNote(250, 300, 64.0f));
    // Punto de partida consistente: la base sale de las notas.
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    roll.setSize(900, 420);
    roll.setProject(&project);
    roll.setUndoManager(&history);
    roll.resized();
    roll.fitPitchRangeToView(56.0f, 70.0f);
  }
  Note &note(size_t i) { return project.getNotes()[i]; }
  float xAt(int frame) const {
    return PianoRollComponent::pianoKeysWidth +
           static_cast<float>(framesToSeconds(frame)) * roll.getPixelsPerSecond() -
           static_cast<float>(roll.getScrollX());
  }
  float yAt(float midi) const {
    return PianoRollComponent::headerHeight +
           (MAX_MIDI_NOTE - midi) * roll.getPixelsPerSemitone() +
           roll.getPixelsPerSemitone() * 0.5f -
           static_cast<float>(roll.getScrollY());
  }
};

juce::MouseEvent event(const juce::Component &c, juce::Point<float> at,
                       juce::ModifierKeys mods, int clicks, bool dragged = false) {
  static auto source = juce::Desktop::getInstance().getMainMouseSource();
  return juce::MouseEvent(source, at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                          const_cast<juce::Component *>(&c),
                          const_cast<juce::Component *>(&c),
                          juce::Time::getCurrentTime(), at,
                          juce::Time::getCurrentTime(), clicks, dragged);
}

void dragUp(Fixture &f, juce::Point<float> start, float semitones) {
  const auto left = juce::ModifierKeys::leftButtonModifier;
  const auto end =
      start + juce::Point<float>(0.0f, -semitones * f.roll.getPixelsPerSemitone());
  f.roll.mouseDown(event(f.roll, start, left, 1));
  f.roll.mouseDrag(event(f.roll, start + juce::Point<float>(0.0f, -6.0f), left, 1, true));
  f.roll.mouseDrag(event(f.roll, end, left, 1, true));
  f.roll.mouseUp(event(f.roll, end, left, 1));
}

void checkCurveFollows(Fixture &f, const char *label) {
  const float desync = f.project.maxCurveNotesDisagreement();
  std::printf("%-34s desfase curva/notas: %.4f st (lim %.1f)\n", label, desync,
              kTolerance);
  CHECK(desync <= kTolerance);
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  // D-286: sin esto, un fallo que aborta pierde lo ya impreso al canalizar la
  // salida y el test parece "no haber medido nada".
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  {
    Fixture f;
    checkCurveFollows(f, "reposo");
  }

  // --- Arrastre de UNA nota ------------------------------------------------
  {
    Fixture f;
    const float midiBefore = f.note(0).getMidiNote();
    dragUp(f, {f.xAt(50), f.yAt(midiBefore)}, 7.0f);
    std::printf("arrastre 1 nota: %.2f -> %.2f\n", midiBefore,
                f.note(0).getMidiNote());
    checkCurveFollows(f, "arrastre 1 nota +7 st");
  }

  // --- Arrastre de una SELECCION ------------------------------------------
  {
    Fixture f;
    for (auto &note : f.project.getNotes())
      note.setSelected(true);
    const float midiBefore = f.note(1).getMidiNote();
    dragUp(f, {f.xAt(150), f.yAt(midiBefore)}, 7.0f);
    std::printf("arrastre seleccion: %.2f -> %.2f (nota 1)\n", midiBefore,
                f.note(1).getMidiNote());
    checkCurveFollows(f, "arrastre seleccion +7 st");
  }

  // --- Flechas (nudge) -----------------------------------------------------
  {
    Fixture f;
    f.note(0).setSelected(true);
    for (int i = 0; i < 7; ++i)
      f.roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    std::printf("flechas: nota 0 = %.2f\n", f.note(0).getMidiNote());
    checkCurveFollows(f, "flechas +7 st");
  }

  // --- Doble clic (afinar al 100 %) ---------------------------------------
  {
    Fixture f;
    const auto left = juce::ModifierKeys::leftButtonModifier;
    f.roll.mouseDoubleClick(event(f.roll, {f.xAt(50), f.yAt(60.3f)}, left, 2));
    std::printf("doble clic: nota 0 = %.2f\n", f.note(0).getMidiNote());
    checkCurveFollows(f, "doble clic (afinar)");
  }

  // --- Inspector (Correct Pitch al 100 %) ---------------------------------
  {
    Fixture f;
    f.note(0).setSelected(true);
    f.roll.beginInspectorEdit(PianoRollComponent::InspectorParam::CorrectPitch);
    f.roll.setInspectorEditValue(100.0f);
    f.roll.endInspectorEdit();
    checkCurveFollows(f, "inspector Correct Pitch 100 %");
  }

  // --- Cortar y unir -------------------------------------------------------
  {
    Fixture f;
    const auto left = juce::ModifierKeys::leftButtonModifier;
    const float yTop = f.yAt(60.3f) - f.roll.getPixelsPerSemitone() * 0.35f;
    f.roll.mouseDown(event(f.roll, {f.xAt(50), yTop}, left, 1));
    f.roll.mouseUp(event(f.roll, {f.xAt(50), yTop}, left, 1));
    f.roll.mouseDown(event(f.roll, {f.xAt(50), yTop}, left, 2));
    f.roll.mouseDoubleClick(event(f.roll, {f.xAt(50), yTop}, left, 2));
    f.roll.mouseUp(event(f.roll, {f.xAt(50), yTop}, left, 2));
    checkCurveFollows(f, "cortar (doble clic arriba)");
    f.roll.mouseDown(event(f.roll, {f.xAt(50), yTop}, left, 1));
    f.roll.mouseUp(event(f.roll, {f.xAt(50), yTop}, left, 1));
    f.roll.mouseDown(event(f.roll, {f.xAt(50), yTop}, left, 2));
    f.roll.mouseDoubleClick(event(f.roll, {f.xAt(50), yTop}, left, 2));
    f.roll.mouseUp(event(f.roll, {f.xAt(50), yTop}, left, 2));
    checkCurveFollows(f, "unir (doble clic en el corte)");
  }

  // --- D-286: la vibracion de la fuente NO es un desfase --------------------
  // Medido en vivo (sesion del usuario, 24-11): la curva compuesta se separaba
  // 0,7489 st del centro de una nota sana y la base 0,0000 st en las 42 notas.
  // Con la compuesta, el arreglo al cargar se disparaba en un proyecto sano
  // (umbral 0,5 st), reconstruia la base sin cambiar nada y lo re-renderizaba.
  {
    Project project;
    prepareAudio(project);
    Note note = makeNote(200, 300, 60.0f);
    std::vector<float> vibrato(100);
    for (int i = 0; i < 100; ++i)
      vibrato[static_cast<size_t>(i)] =
          0.9f * std::sin(juce::MathConstants<float>::twoPi *
                          static_cast<float>(i) / 200.0f); // pico +0,9 en i=50
    note.setDeltaPitch(vibrato);
    project.addNote(note);
    PitchCurveProcessor::rebuildBaseFromNotes(project);

    const float composedPeak =
        freqToMidi(project.getAudioData().f0[250]) - 60.0f;
    const float desync = project.maxCurveNotesDisagreement();
    std::printf(
        "D-286 vibracion: curva compuesta %.3f st en el centro, base vs notas "
        "%.4f st\n",
        composedPeak, desync);
    CHECK(composedPeak > 0.8f); // la vibracion esta de verdad en la curva
    CHECK(desync <= kTolerance); // y no se cuenta como desfase
  }

  // --- D-286: el caso que SI hay que arreglar (notas movidas, base vieja) ---
  {
    Fixture f;
    for (auto &note : f.project.getNotes())
      note.setMidiNote(note.getMidiNote() + 7.0f);
    const float desynced = f.project.maxCurveNotesDisagreement();
    std::printf("D-286 base vieja: %.3f st (esperado ~7)\n", desynced);
    CHECK(desynced > 6.0f);
    PitchCurveProcessor::rebuildBaseFromNotes(f.project);
    const float repaired = f.project.maxCurveNotesDisagreement();
    std::printf("D-286 tras reconstruir: %.4f st (lim %.1f)\n", repaired,
                kTolerance);
    CHECK(repaired <= kTolerance);
  }

  // --- D-286: una nota corta entre dos saltos no falsea la medida ----------
  // La base es una escalera suavizada: en una nota corta pegada a un salto de
  // 7 st el suavizado podria arrastrarla y parecer un desfase.
  {
    Project project;
    prepareAudio(project);
    project.addNote(makeNote(0, 100, 60.0f));
    project.addNote(makeNote(100, 110, 67.0f));
    project.addNote(makeNote(110, 210, 60.0f));
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    const float desync = project.maxCurveNotesDisagreement();
    std::printf("D-286 nota corta (10 frames) entre saltos: %.4f st\n", desync);
    CHECK(desync <= kTolerance);
  }

  std::printf("CurveFollowsNotesTests: en verde\n");
  return 0;
}
