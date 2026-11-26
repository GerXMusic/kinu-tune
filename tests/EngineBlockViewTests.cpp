// D-309 - colores de motor por bloque visual y barrido de borde.
//
// Criterio escrito antes de medir:
// - El bloque visual es la frase delimitada por silencio de D-258, no el
//   bloque del renderer: dos frases con el mismo motor son dos bloques.
// - El motor de un bloque es el efectivo de su primera nota; "Seguir global"
//   sigue al motor global sin tocar la nota.
// - Un override por nota cambia solo su bloque.
// - Sin seleccion explicita no hay bloque activo ni notas de bloque.
// - La cache se reutiliza mientras la huella no cambie (tiempos, motor,
//   rest/deleted, motor global y fuente) y se reconstruye cuando cambia.
// - Undo/redo restauran el motor y, con el, el color derivado.
// - El barrido expande el rango sucio a la primera/ultima nota del bloque;
//   con varios bloques es neutro.
// - Rendimiento: paint_last <= 16,7 ms en regimen estable, primer paint con el
//   cajon abierto <= 50 ms y paint_max <= 2x la base con el cajon cerrado.

#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/EngineColours.h"
#include "../Source/Undo/NoteActions.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kHop = HOP_SIZE;
constexpr int kFrames = 460;
// Frase A: tres notas contiguas; silencio; frase B: una nota.
constexpr int kA0 = 40, kA1 = 120, kA2 = 200, kAEnd = 280;
constexpr int kB0 = 320, kBEnd = 400;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

juce::AudioBuffer<float> makeSignal(int frames, int noteFrames = 12,
                                    int gapFrames = 8) {
  juce::AudioBuffer<float> buffer(1, frames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    const int period = noteFrames + gapFrames;
    const int inPeriod = frame % period;
    const bool voiced = inPeriod < noteFrames;
    const float midi = 57.0f + static_cast<float>((frame / period) % 5);
    float value = 0.0f;
    if (voiced) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(midi) / kSampleRate;
      value = 0.3f * static_cast<float>(std::sin(phase));
    } else {
      phase = 0.0;
    }
    buffer.setSample(0, i, value);
  }
  return buffer;
}

Project makePhraseProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const auto signal = makeSignal(kFrames);
  audio.originalWaveform.makeCopyOf(signal);
  audio.waveform.makeCopyOf(signal);
  const std::pair<int, int> spans[] = {
      {kA0, kA1}, {kA1, kA2}, {kA2, kAEnd}, {kB0, kBEnd}};
  const float midis[] = {57.0f, 59.0f, 60.0f, 62.0f};
  for (int i = 0; i < 4; ++i) {
    Note note(spans[i].first, spans[i].second, midis[i]);
    note.setOriginalMidiNote(midis[i]);
    note.setDeltaPitch(
        std::vector<float>(spans[i].second - spans[i].first, 0.0f));
    note.setOriginalDeltaPitch(note.getDeltaPitch());
    project.addNote(note);
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

Project makePaintProject(int noteCount) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const int frames = noteCount * 20 + 40;
  const auto signal = makeSignal(frames);
  audio.originalWaveform.makeCopyOf(signal);
  audio.waveform.makeCopyOf(signal);
  int frame = 10;
  for (int i = 0; i < noteCount; ++i) {
    const int start = frame;
    const int end = start + 12;
    Note note(start, end, 57.0f + static_cast<float>(i % 12));
    note.setOriginalMidiNote(note.getMidiNote());
    note.setDeltaPitch(std::vector<float>(end - start, 0.0f));
    note.setOriginalDeltaPitch(note.getDeltaPitch());
    project.addNote(note);
    frame += 20; // 8 frames of digital silence between phrases
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

void selectOnly(Project &project, int index) {
  auto &notes = project.getNotes();
  for (size_t i = 0; i < notes.size(); ++i)
    notes[i].setSelected(static_cast<int>(i) == index);
}

void testGlobalAndOverride() {
  Project project = makePhraseProject();
  auto &notes = project.getNotes();
  PianoRollComponent pianoRoll;
  pianoRoll.setSize(1200, 700);
  pianoRoll.setProject(&project);
  pianoRoll.setEngineViewActive(true);
  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Psola);

  // Todas las notas son Inherit: el bloque sigue al motor global.
  const auto a0 = pianoRoll.getNoteBlockEngine(notes[0]);
  CHECK(a0.has_value());
  CHECK(*a0 == SynthesisEngineType::Psola);

  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Vocoder);
  const auto aVocoder = pianoRoll.getNoteBlockEngine(notes[1]);
  CHECK(aVocoder.has_value());
  CHECK(*aVocoder == SynthesisEngineType::Vocoder);

  // Override explicito en la frase B: cambia B y no A.
  notes[3].setEngine(NoteEngine::Psola);
  const auto bPsola = pianoRoll.getNoteBlockEngine(notes[3]);
  CHECK(bPsola.has_value());
  CHECK(*bPsola == SynthesisEngineType::Psola);
  const auto stillVocoder = pianoRoll.getNoteBlockEngine(notes[0]);
  CHECK(stillVocoder.has_value());
  CHECK(*stillVocoder == SynthesisEngineType::Vocoder);
  std::printf("D-309 motor: A=%s B=%s\n",
              synthesisEngineTypeToString(*stillVocoder),
              synthesisEngineTypeToString(*bPsola));
}

void testActiveBlockAndNoSelection() {
  Project project = makePhraseProject();
  PianoRollComponent pianoRoll;
  pianoRoll.setSize(1200, 700);
  pianoRoll.setProject(&project);
  pianoRoll.setEngineViewActive(true);
  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Psola);

  CHECK(!pianoRoll.hasExplicitEngineSelection());
  CHECK(pianoRoll.getActiveEngineBlockNotes().empty());
  CHECK(pianoRoll.getActiveEngineBlockIndices().empty());

  selectOnly(project, 1);
  CHECK(pianoRoll.hasExplicitEngineSelection());
  const auto activeNotes = pianoRoll.getActiveEngineBlockNotes();
  const auto activeBlocks = pianoRoll.getActiveEngineBlockIndices();
  CHECK(activeNotes.size() == 3); // la frase A entera
  CHECK(activeBlocks.size() == 1);

  // Una nota del otro bloque abre solo ese bloque.
  selectOnly(project, 3);
  const auto bNotes = pianoRoll.getActiveEngineBlockNotes();
  const auto bBlocks = pianoRoll.getActiveEngineBlockIndices();
  CHECK(bNotes.size() == 1);
  CHECK(bBlocks.size() == 1);
  CHECK(bBlocks.front() != activeBlocks.front());
  std::printf("D-309 bloque activo: A=%d notas, B=%d notas, bloques A=%d B=%d\n",
              static_cast<int>(activeNotes.size()),
              static_cast<int>(bNotes.size()),
              static_cast<int>(activeBlocks.size()),
              static_cast<int>(bBlocks.size()));
}

void testCacheFingerprint() {
  Project project = makePhraseProject();
  PianoRollComponent pianoRoll;
  pianoRoll.setProject(&project);
  pianoRoll.setEngineViewActive(true);
  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Psola);

  const int builds = pianoRoll.getEngineBlockBuildCount();
  (void)pianoRoll.getNoteBlockEngine(project.getNotes()[0]);
  (void)pianoRoll.getNoteBlockEngine(project.getNotes()[0]);
  CHECK(pianoRoll.getEngineBlockBuildCount() == builds);

  // Cambiar el motor guardado invalida la huella.
  project.getNotes()[0].setEngine(NoteEngine::Vocoder);
  (void)pianoRoll.getNoteBlockEngine(project.getNotes()[0]);
  CHECK(pianoRoll.getEngineBlockBuildCount() == builds + 1);

  // Mover una nota tambien.
  const int afterEngine = pianoRoll.getEngineBlockBuildCount();
  project.getNotes()[0].setStartFrame(project.getNotes()[0].getStartFrame() + 1);
  (void)pianoRoll.getNoteBlockEngine(project.getNotes()[0]);
  CHECK(pianoRoll.getEngineBlockBuildCount() == afterEngine + 1);

  // Cambiar el motor global tambien.
  const int afterMove = pianoRoll.getEngineBlockBuildCount();
  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Pitchcore);
  (void)pianoRoll.getNoteBlockEngine(project.getNotes()[1]);
  CHECK(pianoRoll.getEngineBlockBuildCount() == afterMove + 1);
  std::printf("D-309 cache: %d -> %d -> %d -> %d reconstrucciones\n", builds,
              builds + 1, afterEngine + 1, afterMove + 1);
}

void testUndoRedoRestoresEngineAndColour() {
  Project project = makePhraseProject();
  auto &notes = project.getNotes();
  PianoRollComponent pianoRoll;
  pianoRoll.setProject(&project);
  pianoRoll.setEngineViewActive(true);
  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Vocoder);

  const auto colourBefore = pianoRoll.getNoteBlockEngine(notes[0]);
  CHECK(colourBefore.has_value());
  CHECK(*colourBefore == SynthesisEngineType::Vocoder);

  std::vector<NoteEngineAction::Change> changes;
  for (int i = 0; i < 3; ++i)
    changes.push_back({&notes[static_cast<size_t>(i)], notes[static_cast<size_t>(i)].getEngine(),
                       NoteEngine::Psola});
  NoteEngineAction action(changes);
  action.redo();
  const auto after = pianoRoll.getNoteBlockEngine(notes[0]);
  CHECK(after.has_value());
  CHECK(*after == SynthesisEngineType::Psola);

  action.undo();
  const auto undone = pianoRoll.getNoteBlockEngine(notes[0]);
  CHECK(undone.has_value());
  CHECK(*undone == SynthesisEngineType::Vocoder);
  std::printf("D-309 undo/redo: %s -> %s -> %s\n",
              synthesisEngineTypeToString(*colourBefore),
              synthesisEngineTypeToString(*after),
              synthesisEngineTypeToString(*undone));
}

void testSweepRangeAndNeutral() {
  Project project = makePhraseProject();
  PianoRollComponent pianoRoll;
  pianoRoll.setSize(1200, 700);
  pianoRoll.setProject(&project);
  pianoRoll.setEngineViewActive(true);
  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Psola);

  // Un punto dentro de la frase A se expande a su primera/ultima nota.
  pianoRoll.setSweepState(true, kA1 + 3, kA1 + 4, 1);
  CHECK(pianoRoll.getSweepStartFrame() == kA0);
  CHECK(pianoRoll.getSweepEndFrame() == kAEnd);
  CHECK(!pianoRoll.isSweepNeutral());

  // Un rango que toca A y B no elige color de motor: barrido neutro.
  pianoRoll.setSweepState(true, kAEnd - 1, kB0 + 1, 2);
  CHECK(pianoRoll.getSweepStartFrame() == kA0);
  CHECK(pianoRoll.getSweepEndFrame() == kBEnd);
  CHECK(pianoRoll.isSweepNeutral());
  std::printf("D-309 barrido: A [%d,%d) neutro=%d\n",
              pianoRoll.getSweepStartFrame(), pianoRoll.getSweepEndFrame(),
              pianoRoll.isSweepNeutral() ? 1 : 0);
}

void testPaintBudget() {
  Project project = makePaintProject(160);
  PianoRollComponent pianoRoll;
  pianoRoll.setSize(1400, 800);
  pianoRoll.setProject(&project);
  pianoRoll.setGlobalSynthesisEngine(SynthesisEngineType::Psola);

  auto paintOnce = [&pianoRoll] {
    juce::Image image(juce::Image::ARGB, pianoRoll.getWidth(),
                      pianoRoll.getHeight(), true);
    juce::Graphics g(image);
    pianoRoll.paint(g);
  };

  paintOnce(); // calentamiento (fuentes, curvas, waveform)
  pianoRoll.resetPaintStats();
  paintOnce();
  paintOnce();
  const double baselineMax = pianoRoll.getMaxPaintDurationMs();

  pianoRoll.setEngineViewActive(true);
  pianoRoll.resetPaintStats();
  paintOnce();
  const double firstPaintMs = pianoRoll.getLastPaintDurationMs();

  pianoRoll.resetPaintStats();
  for (int i = 0; i < 5; ++i)
    paintOnce();
  const double steadyLast = pianoRoll.getLastPaintDurationMs();
  const double steadyMax = pianoRoll.getMaxPaintDurationMs();

  std::printf("D-309 paint: base_max=%.2f first=%.2f last=%.2f max=%.2f\n",
              baselineMax, firstPaintMs, steadyLast, steadyMax);
  CHECK(firstPaintMs <= 50.0);
  CHECK(steadyLast <= 16.7);
  CHECK(steadyMax <= baselineMax * 2.0);
}

} // namespace

int main() {
  testGlobalAndOverride();
  testActiveBlockAndNoSelection();
  testCacheFingerprint();
  testUndoRedoRestoresEngineAndColour();
  testSweepRangeAndNeutral();
  testPaintBudget();
  std::printf("EngineBlockViewTests: en verde\n");
  return 0;
}
