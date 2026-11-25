// M5 v2 / D-302: el borde izquierdo del silencio de una nota eliminada.
//
// Criterio (C, decidido el 2026-11-25): la consonante que se oye al eliminar
// una nota es su ATAQUE PREVIO, que vive fuera de [start, end) y casi siempre
// dentro del tramo de la nota anterior. El borde no puede ser un tope fijo de
// milisegundos: se decide por energia con el mismo detector de silencio de
// D-129 (ventana 5 ms, hop 1 ms, minimo 20 ms) y un umbral relativo al nivel
// de cuerpo de la nota anterior (15 dB por debajo).
//
// Lo que se mide aqui:
//   A) silencio + consonante delante de la nota eliminada: la consonante queda
//      apagada y la nota anterior no se toca en sus ultimos 20 ms.
//   B) hueco largo: se apaga desde el fin del silencio, no desde prevEnd; el
//      silencio se conserva.
//   C) sin silencio (la consonante empieza en prevEnd): left = prevEnd.
//   D) primera nota del proyecto con consonante delante: left = 0 (clampado al
//      inicio del pase, que no llega al frame 0).

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
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
constexpr int kTwentyMs = kSampleRate / 50;   // 960 muestras
constexpr int kTinyMs = kSampleRate / 1000;   // 48 muestras

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

/// Un tramo de audio de la toma sintetica. `noise` es la consonante: ruido
/// determinista, sin voicedMask. `amplitude <= 0` deja silencio digital.
struct Segment {
  int startFrame = 0;
  int endFrame = 0;
  float midi = 0.0f;
  float amplitude = 0.0f;
  bool noise = false;
};

struct NoteSpec {
  int startFrame = 0;
  int endFrame = 0;
  float midi = 0.0f;
};

juce::AudioBuffer<float> makeAudio(int totalFrames,
                                   const std::vector<Segment> &segments) {
  juce::AudioBuffer<float> buffer(1, totalFrames * kHop);
  double phase = 0.0;
  unsigned int rng = 22222u;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float value = 0.0f;
    for (const auto &segment : segments) {
      if (frame < segment.startFrame || frame >= segment.endFrame)
        continue;
      if (segment.noise) {
        // Consonante: ruido determinista con energia (NoteBoundaryHoleTests).
        rng = rng * 1664525u + 1013904223u;
        value = segment.amplitude *
                (static_cast<float>((rng >> 8) & 0xFFFFu) / 32768.0f - 1.0f);
      } else if (segment.midi > 0.0f) {
        phase += 2.0 * 3.14159265358979323846 * midiHz(segment.midi) /
                 kSampleRate;
        value = segment.amplitude *
                (0.30f * static_cast<float>(std::sin(phase)) +
                 0.12f * static_cast<float>(std::sin(2.0 * phase)));
      }
      break;
    }
    buffer.setSample(0, i, value);
  }
  return buffer;
}

Project makeProject(int totalFrames, const std::vector<Segment> &segments,
                    const std::vector<NoteSpec> &notes) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const auto phrase = makeAudio(totalFrames, segments);
  audio.originalWaveform.makeCopyOf(phrase);
  audio.waveform.makeCopyOf(phrase);

  audio.melSpectrogram.assign(static_cast<size_t>(totalFrames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(totalFrames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(totalFrames), false);

  for (const auto &spec : notes) {
    for (int frame = spec.startFrame; frame < spec.endFrame; ++frame) {
      audio.voicedMask[static_cast<size_t>(frame)] = true;
      audio.denseF0[static_cast<size_t>(frame)] = midiHz(spec.midi);
    }
    Note note(spec.startFrame, spec.endFrame, spec.midi);
    note.setOriginalMidiNote(spec.midi);
    note.setOriginalDeltaPitch(
        std::vector<float>(spec.endFrame - spec.startFrame, 0.0f));
    project.addNote(note);
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

bool renderAndWait(Project &project, int totalFrames, const char *label,
                   bool clearAllDirty) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(SynthesisEngineType::Psola);
  if (clearAllDirty) {
    for (auto &note : project.getNotes())
      note.clearDirty();
    project.setF0DirtyRange(0, totalFrames);
  }

  std::atomic<bool> finished{false};
  std::atomic<bool> success{false};
  synthesizer.synthesizeRegion(nullptr, [&](bool ok) {
    success.store(ok);
    finished.store(true);
  });
  const double deadline = juce::Time::getMillisecondCounterHiRes() + 60000.0;
  while (!finished.load() &&
         juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  if (!finished.load()) {
    std::printf("%s: el render no termino\n", label);
    return false;
  }
  return success.load();
}

void markDeleted(Project &project, size_t index, int totalFrames) {
  auto &note = project.getNotes()[index];
  note.setDeleted(true);
  note.setRest(true);
  note.setSelected(false);
  note.markDirty();
  note.markSynthDirty();
  project.setF0DirtyRange(std::max(0, note.getStartFrame() - 24),
                          std::min(totalFrames, note.getEndFrame() + 24));
  project.setModified(true);
}

float rmsDb(const juce::AudioBuffer<float> &buffer, int firstSample,
            int lastSample) {
  const int first = std::max(0, firstSample);
  const int last = std::min(buffer.getNumSamples(), lastSample);
  if (last <= first)
    return -180.0f;
  double sum = 0.0;
  for (int i = first; i < last; ++i) {
    const double v = buffer.getSample(0, i);
    sum += v * v;
  }
  const double rms = std::sqrt(sum / static_cast<double>(last - first));
  return static_cast<float>(20.0 * std::log10(std::max(rms, 1.0e-9)));
}

float maxDiff(const juce::AudioBuffer<float> &a,
              const juce::AudioBuffer<float> &b, int firstSample,
              int lastSample) {
  const int first = std::max(0, firstSample);
  const int last =
      std::min({a.getNumSamples(), b.getNumSamples(), lastSample});
  float diff = 0.0f;
  for (int i = first; i < last; ++i)
    diff = std::max(diff, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return diff;
}

int frameSample(int frame) { return frame * kHop; }

// ---------------------------------------------------------------------------
// A) silencio 150..153, consonante 153..161, nota eliminada 161..300,
//    vecina siguiente 310..400. Se borra la nota de en medio.
// ---------------------------------------------------------------------------
void testSilenceThenConsonant() {
  constexpr int kTotal = 420;
  const std::vector<Segment> segments = {
      {50, 150, 57.0f, 1.0f, false},   // nota A
      {153, 161, 0.0f, 0.18f, true},   // consonante de la nota eliminada
      {161, 300, 59.0f, 1.0f, false},  // nota B (se elimina)
      {310, 400, 64.0f, 1.0f, false},  // nota C
  };
  const std::vector<NoteSpec> notes = {{50, 150, 57.0f},
                                       {161, 300, 59.0f},
                                       {310, 400, 64.0f}};

  Project project = makeProject(kTotal, segments, notes);
  CHECK(renderAndWait(project, kTotal, "A control", /*clearAllDirty=*/true));
  const juce::AudioBuffer<float> control = project.getAudioData().waveform;

  const float consonantBefore = rmsDb(control, frameSample(153),
                                      frameSample(161));
  CHECK(consonantBefore > -30.0f); // la fuente tiene la consonante audible

  markDeleted(project, 1, kTotal);
  CHECK(renderAndWait(project, kTotal, "A delete", /*clearAllDirty=*/false));
  const auto &waveform = project.getAudioData().waveform;

  const float consonantAfter = rmsDb(waveform, frameSample(153),
                                     frameSample(161));
  const float noteA = rmsDb(waveform, frameSample(60), frameSample(140));
  const float previousTailDiff =
      maxDiff(control, waveform, frameSample(150) - kTwentyMs,
              frameSample(150));
  const float nextAttackDiff =
      maxDiff(control, waveform, frameSample(310),
              frameSample(310) + kTwentyMs);
  std::printf("A: consonante %.1f dBFS (antes %.1f), nota A %.1f dBFS, "
              "diff cola %.9f, diff ataque siguiente %.9f\n",
              consonantAfter, consonantBefore, noteA, previousTailDiff,
              nextAttackDiff);
  CHECK(consonantAfter <= -120.0f);
  CHECK(noteA > -60.0f);
  CHECK(previousTailDiff == 0.0f);
  CHECK(nextAttackDiff == 0.0f);
}

// ---------------------------------------------------------------------------
// B) hueco largo 150..200 (suelo de ruido a -70 dBFS, no cero), consonante
//    200..210 y nota 210..300. Se apaga desde el fin del silencio: el silencio
//    no se toca (diff 0 contra el control).
// ---------------------------------------------------------------------------
void testLongGapKeepsSilence() {
  constexpr int kTotal = 320;
  const std::vector<Segment> segments = {
      {50, 150, 57.0f, 1.0f, false},       // nota A
      {150, 200, 0.0f, 0.0005f, true},     // suelo de ruido: no es cero
      {200, 210, 0.0f, 0.18f, true},       // consonante
      {210, 300, 59.0f, 1.0f, false},      // nota B (se elimina)
  };
  const std::vector<NoteSpec> notes = {{50, 150, 57.0f}, {210, 300, 59.0f}};

  Project project = makeProject(kTotal, segments, notes);
  CHECK(renderAndWait(project, kTotal, "B control", /*clearAllDirty=*/true));
  const juce::AudioBuffer<float> control = project.getAudioData().waveform;

  markDeleted(project, 1, kTotal);
  CHECK(renderAndWait(project, kTotal, "B delete", /*clearAllDirty=*/false));
  const auto &waveform = project.getAudioData().waveform;

  const float consonantAfter =
      rmsDb(waveform, frameSample(200), frameSample(210));
  const float silenceLevel =
      rmsDb(waveform, frameSample(152), frameSample(196));
  const float silenceDiff = maxDiff(control, waveform, frameSample(152),
                                    frameSample(196));
  std::printf("B: consonante %.1f dBFS, silencio %.1f dBFS, diff silencio "
              "%.9f\n",
              consonantAfter, silenceLevel, silenceDiff);
  CHECK(consonantAfter <= -120.0f);
  CHECK(silenceDiff == 0.0f); // el borde es el fin del silencio, no prevEnd
  CHECK(silenceLevel < -60.0f);
  CHECK(silenceLevel > -90.0f); // sigue siendo el suelo original, no cero
}

// ---------------------------------------------------------------------------
// C) sin silencio: la consonante empieza justo en prevEnd (150..160) y la nota
//    eliminada en 160. left = prevEnd = 150; la cola de A no se toca.
// ---------------------------------------------------------------------------
void testNoSilenceStartsAtPrevEnd() {
  constexpr int kTotal = 320;
  const std::vector<Segment> segments = {
      {50, 150, 57.0f, 1.0f, false},   // nota A
      {150, 160, 0.0f, 0.18f, true},   // consonante pegada a la nota anterior
      {160, 300, 59.0f, 1.0f, false},  // nota B (se elimina)
  };
  const std::vector<NoteSpec> notes = {{50, 150, 57.0f}, {160, 300, 59.0f}};

  Project project = makeProject(kTotal, segments, notes);
  CHECK(renderAndWait(project, kTotal, "C control", /*clearAllDirty=*/true));
  const juce::AudioBuffer<float> control = project.getAudioData().waveform;

  markDeleted(project, 1, kTotal);
  CHECK(renderAndWait(project, kTotal, "C delete", /*clearAllDirty=*/false));
  const auto &waveform = project.getAudioData().waveform;

  // El fade del borde izquierdo ocupa las primeras 256 muestras del tramo.
  const float consonantAfter =
      rmsDb(waveform, frameSample(150) + kTinyMs * 8, frameSample(160));
  const float previousTailDiff =
      maxDiff(control, waveform, frameSample(150) - kTwentyMs,
              frameSample(150));
  std::printf("C: consonante %.1f dBFS, diff cola %.9f\n", consonantAfter,
              previousTailDiff);
  CHECK(consonantAfter <= -120.0f);
  CHECK(previousTailDiff == 0.0f);
}

// ---------------------------------------------------------------------------
// D) primera nota del proyecto con consonante delante (0..100): no hay nota
//    anterior, left = 0. El pase empieza en el frame 36, asi que se comprueba
//    que el silencio llega hasta el inicio del pase.
// ---------------------------------------------------------------------------
void testFirstNoteStartsAtZero() {
  constexpr int kTotal = 240;
  const std::vector<Segment> segments = {
      {0, 100, 0.0f, 0.18f, true},    // consonante desde el sample 0
      {100, 200, 57.0f, 1.0f, false}, // primera nota (se elimina)
  };
  const std::vector<NoteSpec> notes = {{100, 200, 57.0f}};

  Project project = makeProject(kTotal, segments, notes);
  CHECK(renderAndWait(project, kTotal, "D control", /*clearAllDirty=*/true));
  const juce::AudioBuffer<float> control = project.getAudioData().waveform;

  markDeleted(project, 0, kTotal);
  CHECK(renderAndWait(project, kTotal, "D delete", /*clearAllDirty=*/false));
  const auto &waveform = project.getAudioData().waveform;

  // computeSynthesisRange arranca en el frame 36 (16 de puente + 24 de pade);
  // el tramo se clampa ahi: fuera del pase no se escribe nada.
  const float startOfPass = rmsDb(waveform, frameSample(37), frameSample(60));
  const float restOfConsonant =
      rmsDb(waveform, frameSample(60), frameSample(99));
  const float noteAfter = rmsDb(waveform, frameSample(101), frameSample(199));
  std::printf("D: inicio del pase %.1f dBFS, resto consonante %.1f dBFS, "
              "nota %.1f dBFS\n",
              startOfPass, restOfConsonant, noteAfter);
  CHECK(startOfPass <= -120.0f);
  CHECK(restOfConsonant <= -120.0f);
  CHECK(noteAfter <= -120.0f);
}

// ---------------------------------------------------------------------------
// E) el caso de la toma real (nota 5, frames 746-784): un suelo de soplo a
//    -33 dBFS que llega hasta el onset, con parpadeos de 10 ms a -25 dBFS cada
//    15 frames. Sin fusionar los tramos bajos, el ultimo empieza tras el ultimo
//    parpadeo y deja audible la cola del soplo.
// ---------------------------------------------------------------------------
void testWobblingFloorMergesRuns() {
  constexpr int kTotal = 320;
  const std::vector<Segment> segments = {
      {50, 150, 57.0f, 1.0f, false},    // nota A
      {155, 156, 0.0f, 0.0974f, true},  // parpadeo -25 dBFS
      {170, 171, 0.0f, 0.0974f, true},
      {185, 186, 0.0f, 0.0974f, true},
      {190, 191, 0.0f, 0.0974f, true},
      {150, 200, 0.0f, 0.0388f, true},  // soplo -33 dBFS hasta el onset
      {200, 300, 59.0f, 1.0f, false},   // nota B (se elimina)
  };
  const std::vector<NoteSpec> notes = {{50, 150, 57.0f}, {200, 300, 59.0f}};

  Project project = makeProject(kTotal, segments, notes);
  CHECK(renderAndWait(project, kTotal, "E control", /*clearAllDirty=*/true));
  const juce::AudioBuffer<float> control = project.getAudioData().waveform;

  markDeleted(project, 1, kTotal);
  CHECK(renderAndWait(project, kTotal, "E delete", /*clearAllDirty=*/false));
  const auto &waveform = project.getAudioData().waveform;

  const float floorAfter = rmsDb(waveform, frameSample(152), frameSample(188));
  const float noteAfter = rmsDb(waveform, frameSample(201), frameSample(299));
  const float previousTailDiff =
      maxDiff(control, waveform, frameSample(150) - kTwentyMs,
              frameSample(150));
  std::printf("E: soplo %.1f dBFS, nota %.1f dBFS, diff cola %.9f\n",
              floorAfter, noteAfter, previousTailDiff);
  CHECK(floorAfter <= -120.0f);
  CHECK(noteAfter <= -120.0f);
  CHECK(previousTailDiff == 0.0f);
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;

  testSilenceThenConsonant();
  testLongGapKeepsSilence();
  testNoSilenceStartsAtPrevEnd();
  testFirstNoteStartsAtZero();
  testWobblingFloorMergesRuns();

  std::printf("NoteDeleteAttackTests: borde izquierdo por energia OK\n");
  return 0;
}
