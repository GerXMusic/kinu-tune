// Fase 1 — reproducción de los seis síntomas (nivel audio).
//
// Criterio escrito ANTES de medir (no se ajusta después del número):
//
//   S1 · restablecer todo devuelve la fuente.
//        Tras editar altura + fronteras y renderizar, "restablecer todo" y renderizar debe
//        dejar el tramo **bit a bit** igual al original: max|diff| <= 1e-6 y ningún salto
//        muestra-a-muestra mayor que el del propio original (×1,05).
//
//   S2 · cambiar el motor de un bloque nunca deja la región sin audio.
//        Un cambio SOLO de motor por nota debe: (a) marcar >0 notas sucias
//        (markEditedNotesDirty), (b) renderizar, (c) dejar pico > 0,01 y el mismo audio que un
//        render completo del estado final (tol 2e-3 con Pitchcore).
//
//   S3 · editar una nota no toca a las vecinas ni a otro punto del bloque.
//        Render incremental de UNA nota vs render completo del estado final: dentro del bloque
//        max|diff| <= 2e-4 (PSOLA) y fuera del bloque max|diff| <= 1e-7.
//
//   S6 · restablecer un bloque carga la fuente de esa sección.
//        Dos bloques separados por silencio; se edita y luego se restablece SOLO el primero:
//        el primero vuelve a la fuente (<=1e-6) y el segundo queda intacto (<=1e-7).
//
// Todos los tests comparan contra un render completo del estado final, como pide la Fase 1.

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/NotePitch.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kHop = HOP_SIZE;
constexpr int kTotalFrames = 520;
// Bloque 1
constexpr int kN0a = 50, kN0b = 150;
constexpr int kN1a = 150, kN1b = 250;
// Silencio D-129 (8 frames = 42,7 ms)
constexpr int kGapA = 250, kGapB = 290;
// Bloque 2
constexpr int kN2a = 290, kN2b = 390;
constexpr int kN3a = 390, kN3b = 470;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

juce::AudioBuffer<float> makePhrase() {
  juce::AudioBuffer<float> buffer(1, kTotalFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float value = 0.0f;
    float midi = 0.0f;
    if (frame >= kN0a && frame < kN0b)      midi = 57.0f;
    else if (frame >= kN1a && frame < kN1b) midi = 59.0f;
    else if (frame >= kN2a && frame < kN2b) midi = 62.0f;
    else if (frame >= kN3a && frame < kN3b) midi = 64.0f;
    if (midi > 0.0f) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(midi) / kSampleRate;
      value = 0.30f * static_cast<float>(std::sin(phase)) +
              0.12f * static_cast<float>(std::sin(2.0 * phase));
    } else {
      phase = 0.0;
    }
    buffer.setSample(0, i, value);
  }
  return buffer;
}

Project makeProject(NoteEngine engine = NoteEngine::Psola) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const auto phrase = makePhrase();
  audio.originalWaveform.makeCopyOf(phrase);
  audio.waveform.makeCopyOf(phrase);
  audio.melSpectrogram.assign(static_cast<size_t>(kTotalFrames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(kTotalFrames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(kTotalFrames), false);
  const std::pair<int, int> spans[] = {{kN0a, kN0b}, {kN1a, kN1b},
                                       {kN2a, kN2b}, {kN3a, kN3b}};
  const float midis[] = {57.0f, 59.0f, 62.0f, 64.0f};
  for (int i = 0; i < 4; ++i) {
    for (int f = spans[i].first; f < spans[i].second; ++f) {
      audio.voicedMask[static_cast<size_t>(f)] = true;
      audio.denseF0[static_cast<size_t>(f)] = midiHz(midis[i]);
    }
    Note note(spans[i].first, spans[i].second, midis[i]);
    note.setOriginalMidiNote(midis[i]);
    note.setEngine(engine);
    note.setOriginalDeltaPitch(
        std::vector<float>(spans[i].second - spans[i].first, 0.0f));
    project.addNote(note);
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

bool renderAndWait(Project &project, SynthesisEngineType engine,
                   const char *label) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(engine);
  std::atomic<bool> finished{false};
  std::atomic<bool> success{false};
  synthesizer.synthesizeRegion(nullptr, [&](bool ok) {
    success.store(ok);
    finished.store(true);
  });
  const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  while (!finished.load() &&
         juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  if (!finished.load()) {
    std::printf("%s: el render no termino en 30 s\n", label);
    return false;
  }
  return success.load();
}

// Referencia: el estado final renderizado completo (notas limpias, rango F0 total).
Project fullRender(Project source, SynthesisEngineType engine, const char *label) {
  for (auto &note : source.getNotes())
    note.clearDirty();
  source.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(source, engine, label));
  return source;
}

float maxAbsDiff(const juce::AudioBuffer<float> &a,
                 const juce::AudioBuffer<float> &b, int firstFrame,
                 int lastFrame) {
  float worst = 0.0f;
  const int n = std::min({a.getNumSamples(), b.getNumSamples(), lastFrame * kHop});
  for (int i = std::max(0, firstFrame * kHop); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return worst;
}

// Salto máximo muestra-a-muestra: un clic es un salto que el original no tiene.
float maxStep(const juce::AudioBuffer<float> &a, int firstFrame, int lastFrame) {
  float worst = 0.0f;
  const int n = std::min(a.getNumSamples(), lastFrame * kHop);
  for (int i = std::max(1, firstFrame * kHop); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - a.getSample(0, i - 1)));
  return worst;
}

float peak(const juce::AudioBuffer<float> &a) {
  float p = 0.0f;
  for (int i = 0; i < a.getNumSamples(); ++i)
    p = std::max(p, std::abs(a.getSample(0, i)));
  return p;
}

// ---------------- reset de fabrica (politica 2) ----------------
// Limpia pitch, formant, amplitud, timing, motor y mute: la nota vuelve a la
// fuente exacta. Lo comparten S1 (todo) y S6 (un bloque).
void resetNoteToFactory(Note &note) {
  NotePitch::restore(note, note.getOriginalMidiNote(),
                     note.getOriginalMidiNote());
  note.setPitchOffset(0.0f);
  note.setVolumeDb(0.0f);
  note.setFormantShift(0.0f);
  note.setTiltLeft(0.0f);
  note.setTiltRight(0.0f);
  note.setVibrato(1.0f);
  note.setPitchDrift(1.0f);
  note.setSmoothLeftFrames(0);
  note.setSmoothRightFrames(0);
  note.setDeltaScale(1.0f);
  note.setDeltaOffset(0.0f);
  note.setBakedDeltaPitch({});
  note.setDeltaPitch(note.getOriginalDeltaPitch());
  note.setStartFrame(note.getSrcStartFrame());
  note.setEndFrame(note.getSrcEndFrame());
  note.setMuted(false);
  note.setEngine(NoteEngine::Inherit);
  note.markDirty();
  note.markSynthDirty();
}

// ---------------- S1 ----------------
void testS1ResetAll() {
  Project project = makeProject();
  const juce::AudioBuffer<float> control =
      project.getAudioData().originalWaveform;

  auto &notes = project.getNotes();
  notes[0].setPitchOffset(1.0f);
  notes[1].setStartFrame(notes[1].getStartFrame() + 3);
  notes[1].setEndFrame(notes[1].getEndFrame() + 3);
  for (auto &note : notes) {
    note.markDirty();
    note.markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN0a, kN1b + 3);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S1 editado"));

  // Restablecer todo (D-246 candidato): estado por defecto + fronteras de fuente.
  for (auto &note : project.getNotes())
    resetNoteToFactory(note);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(0, kTotalFrames);
  project.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S1 restablecido"));

  const auto &out = project.getAudioData().waveform;
  const float diff = maxAbsDiff(out, control, 0, kTotalFrames);
  const float stepOut = maxStep(out, 0, kTotalFrames);
  const float stepControl = maxStep(control, 0, kTotalFrames);
  std::printf("S1 restablecer: vs fuente max|diff| %.8f (lim 1e-6) | paso %.6f vs %.6f\n",
              diff, stepOut, stepControl);
  CHECK(diff <= 1.0e-6f);
  CHECK(stepOut <= stepControl * 1.05f);
}

// ---------------- S2 ----------------
void testS2EngineChange() {
  // Se parte con motor explicito PSOLA (lo normal en un proyecto real).
  Project project = makeProject(NoteEngine::Psola);
  // Estado de partida: el audio es la fuente, sin render pendiente.
  CHECK(peak(project.getAudioData().waveform) > 0.01f);

  // Cambio SOLO de motor: ninguna otra edicion.
  for (auto &note : project.getNotes())
    note.setEngine(NoteEngine::Pitchcore);

  const int marked = project.markEditedNotesDirty();
  std::printf("S2 cambio de motor: markEditedNotesDirty = %d (esperado 4) | "
              "hasRegionEdits = %d\n",
              marked, project.hasRegionEdits() ? 1 : 0);
  CHECK(marked == 4);

  CHECK(renderAndWait(project, SynthesisEngineType::Pitchcore, "S2 pitchcore"));

  Project referencePc = fullRender(makeProject(NoteEngine::Pitchcore),
                                   SynthesisEngineType::Pitchcore, "S2 control pc");
  const auto &out = project.getAudioData().waveform;
  const float p = peak(out);
  const float diff = maxAbsDiff(out, referencePc.getAudioData().waveform, 0,
                                kTotalFrames);
  std::printf("S2 tras el cambio: pico %.6f (min 0,01) | vs render completo %.6f "
              "(lim 2e-3)\n",
              p, diff);
  CHECK(p > 0.01f);
  CHECK(diff <= 2.0e-3f);
}

// ---------------- S3 ----------------
void testS3NeighbourUntouched() {
  Project incremental = makeProject(NoteEngine::Psola);

  // Editar solo la nota 0 y marcar solo ella (como el arrastre de altura).
  {
    auto &note = incremental.getNotes()[0];
    note.setPitchOffset(1.0f);
    note.markDirty();
    note.markSynthDirty();
    PitchCurveProcessor::rebuildBaseFromNotes(incremental);
    incremental.markNoteEditDirtyRange(kN0a, kN0b);
  }
  CHECK(renderAndWait(incremental, SynthesisEngineType::Psola, "S3 editado"));

  Project reference = makeProject(NoteEngine::Psola);
  reference.getNotes()[0].setPitchOffset(1.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(reference);
  Project full = fullRender(reference, SynthesisEngineType::Psola, "S3 completo");

  const auto &out = incremental.getAudioData().waveform;
  const auto &fullOut = full.getAudioData().waveform;
  const auto &original = incremental.getAudioData().originalWaveform;

  // Interior del bloque 1 (hasta justo antes del gap).
  const float insideBlock1 = maxAbsDiff(out, fullOut, 0, kGapA - 1);
  // Bloque 2: no se edito, debe quedar como estaba antes del render.
  const float block2 = maxAbsDiff(out, original, kN2a + 1, kN3b - 1);
  // Vecina inmediata al borde del bloque (donde estaba el empalme por nota).
  const float seam = maxAbsDiff(out, fullOut, kN0b - 2, kN0b + 8);
  std::printf("S3 bloque1 %.6f (lim 2e-4) | costura %.6f | bloque2 %.8f (lim 1e-7)\n",
              insideBlock1, seam, block2);
  CHECK(insideBlock1 <= 2.0e-4f);
  CHECK(seam <= 2.0e-4f);
  CHECK(block2 <= 1.0e-7f);
}

// ---------------- S6 ----------------
void testS6ResetOneBlock() {
  Project project = makeProject(NoteEngine::Psola);
  const juce::AudioBuffer<float> originalBlock2 = [&] {
    juce::AudioBuffer<float> b;
    b.makeCopyOf(project.getAudioData().waveform);
    return b;
  }();

  auto &notes = project.getNotes();
  notes[0].setPitchOffset(1.0f);
  notes[1].setPitchOffset(-1.0f);
  for (int i = 0; i < 2; ++i) {
    notes[static_cast<size_t>(i)].markDirty();
    notes[static_cast<size_t>(i)].markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN0a, kN1b);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S6 editado"));

  // Estado JUSTO ANTES de restablecer: es contra el que se juzga el bloque B.
  juce::AudioBuffer<float> beforeReset;
  beforeReset.makeCopyOf(project.getAudioData().waveform);

  // Restablecer SOLO el bloque 1.
  for (int i = 0; i < 2; ++i)
    resetNoteToFactory(notes[static_cast<size_t>(i)]);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN0a, kN1b);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S6 restablecido"));

  const auto &out = project.getAudioData().waveform;
  const auto &source = project.getAudioData().originalWaveform;
  const float block1 = maxAbsDiff(out, source, kN0a + 5, kN1b - 5);
  const float block2 = maxAbsDiff(out, originalBlock2, kN2a + 5, kN3b - 5);
  const float block2BeforeReset =
      maxAbsDiff(out, beforeReset, kN2a + 5, kN3b - 5);
  std::printf("S6 bloque1 vs fuente %.8f (lim 1e-6) | bloque2 vs previo al "
              "restablecer %.8f (lim 1e-7) | bloque2 vs original %.8f\n",
              block1, block2BeforeReset, block2);
  CHECK(block1 <= 1.0e-6f);
  CHECK(block2BeforeReset <= 1.0e-7f);
  CHECK(block2 <= 1.0e-7f);
}

// ---------------- S6 (dos anclas) ----------------
// Criterio escrito antes de medir (T2): con anclas sucias en los DOS bloques,
// restablecer solo A debe devolver A a la fuente (<=1e-6) y dejar B intacto
// fuera de su propio commit (<=1e-7): el cuerpo de B conserva lo que ya sonaba
// y el silencio entre frases no se toca.
void testS6ResetOneBlockWithTwoDirtyAnchors() {
  Project project = makeProject(NoteEngine::Psola);
  auto &notes = project.getNotes();
  for (int i = 0; i < 4; ++i) {
    notes[static_cast<size_t>(i)].setPitchOffset(i % 2 == 0 ? 1.0f : -1.0f);
    notes[static_cast<size_t>(i)].markDirty();
    notes[static_cast<size_t>(i)].markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN0a, kN3b);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S6b editado"));

  juce::AudioBuffer<float> editedState;
  editedState.makeCopyOf(project.getAudioData().waveform);

  // Restablecer SOLO A; las notas de B siguen sucias (anclas en A y B).
  for (int i = 0; i < 2; ++i)
    resetNoteToFactory(notes[static_cast<size_t>(i)]);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  for (int i = 0; i < 4; ++i)
    notes[static_cast<size_t>(i)].markDirty();
  project.markNoteEditDirtyRange(kN0a, kN3b);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S6b anclas"));

  const auto &out = project.getAudioData().waveform;
  const auto &source = project.getAudioData().originalWaveform;
  const float blockA = maxAbsDiff(out, source, kN0a + 5, kN1b - 5);
  // Fuera del commit de B: la mitad derecha del silencio (A commitea hasta el
  // punto medio; el fade del commit no llega más allá de un frame). Es la
  // comprobación de que dos anclas no hacen que un commit cruce al vecino.
  const float between = maxAbsDiff(out, editedState, kN1b + 22, kN2a - 2);

  // B se commitearon con su estado final: debe coincidir con el render completo
  // de ese estado (no con el de la pasada anterior, que tenía A editado).
  Project reference = makeProject(NoteEngine::Psola);
  auto &refNotes = reference.getNotes();
  for (int i = 0; i < 4; ++i)
    if (i >= 2)
      refNotes[static_cast<size_t>(i)].setPitchOffset(i % 2 == 0 ? 1.0f : -1.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(reference);
  Project full = fullRender(reference, SynthesisEngineType::Psola, "S6b completo");
  const float blockB = maxAbsDiff(out, full.getAudioData().waveform, kN2a + 5,
                                  kN3b - 5);

  std::printf("S6b A vs fuente %.8f (lim 1e-6) | B vs completo %.6f (lim "
              "2e-3) | silencio %.8f (lim 1e-7)\n",
              blockA, blockB, between);
  CHECK(blockA <= 1.0e-6f);
  CHECK(blockB <= 2.0e-3f);
  CHECK(between <= 1.0e-7f);
}

// ---------------- S4: edicion durante el render ----------------
// Criterio escrito antes de medir (T3): una edición que llega mientras hay un
// render en vuelo conserva su dirty tras el merge, se renderiza en el pase
// siguiente y el audio final coincide con el render completo del estado final
// (<=2e-3 en PSOLA).
void testS4EditDuringRender() {
  Project project = makeProject(NoteEngine::Psola);
  auto &notes = project.getNotes();
  for (int i = 0; i < 4; ++i) {
    notes[static_cast<size_t>(i)].setPitchOffset(i % 2 == 0 ? 1.0f : -1.0f);
    notes[static_cast<size_t>(i)].markDirty();
    notes[static_cast<size_t>(i)].markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN0a, kN3b);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S4 inicial"));

  // Edición A: nota 0. Se marca y se lanza el render.
  notes[0].setPitchOffset(1.5f);
  notes[0].markDirty();
  notes[0].markSynthDirty();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN0a, kN0b);

  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(SynthesisEngineType::Psola);
  std::atomic<bool> finished{false};
  std::atomic<bool> success{false};
  synthesizer.synthesizeRegion(nullptr, [&](bool ok) {
    success.store(ok);
    finished.store(true);
  });

  // Edición B mientras el render de A está en vuelo (después de capturar A).
  notes[2].setPitchOffset(-1.5f);
  notes[2].markDirty();
  notes[2].markSynthDirty();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN2a, kN2b);

  const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  while (!finished.load() &&
         juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  CHECK(finished.load());
  CHECK(success.load());

  const bool keptDirty = project.getNotes()[2].isDirty();
  const bool f0Kept = project.hasF0DirtyRange();
  std::printf("S4 edicion en vuelo: nota 2 dirty %d (esperado 1) | f0 dirty "
              "%d (esperado 1)\n",
              keptDirty ? 1 : 0, f0Kept ? 1 : 0);
  CHECK(keptDirty);
  CHECK(f0Kept);

  // Pase siguiente: la edición B se renderiza.
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "S4 segundo pase"));

  Project reference = makeProject(NoteEngine::Psola);
  auto &refNotes = reference.getNotes();
  refNotes[0].setPitchOffset(1.5f);
  refNotes[1].setPitchOffset(-1.0f);
  refNotes[2].setPitchOffset(-1.5f);
  refNotes[3].setPitchOffset(-1.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(reference);
  Project full = fullRender(reference, SynthesisEngineType::Psola, "S4 completo");

  const float diff = maxAbsDiff(project.getAudioData().waveform,
                                full.getAudioData().waveform, kN2a + 5,
                                kN3b - 5);
  std::printf("S4 final vs render completo: bloque B %.6f (lim 2e-3)\n", diff);
  CHECK(diff <= 2.0e-3f);
}

} // namespace

int main(int argc, char **argv) {
  juce::ScopedJuceInitialiser_GUI gui;
  const std::string group = argc > 1 ? argv[1] : "all";
  if (group == "s1") {
    testS1ResetAll();
  } else if (group == "s2") {
    testS2EngineChange();
  } else if (group == "s3") {
    testS3NeighbourUntouched();
  } else if (group == "s6") {
    testS6ResetOneBlock();
  } else if (group == "s6b") {
    testS6ResetOneBlockWithTwoDirtyAnchors();
  } else if (group == "s4") {
    testS4EditDuringRender();
  } else {
    testS1ResetAll();
    testS2EngineChange();
    testS3NeighbourUntouched();
    testS6ResetOneBlock();
    testS6ResetOneBlockWithTwoDirtyAnchors();
    testS4EditDuringRender();
  }
  std::printf("EditSymptomAudioTests [%s]: en verde\n", group.c_str());
  return 0;
}
