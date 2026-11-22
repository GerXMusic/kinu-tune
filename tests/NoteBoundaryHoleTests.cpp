// Clic en reposo en la union de dos notas (R4, 2026-11-21).
//
// Criterio escrito ANTES de medir (no se ajusta despues del numero):
//
//   Una consonante no vocalizada entre dos notas de la misma frase debe
//   conservarse. En la ventana central del tramo no vocalizado (la mitad
//   interior, sin las rampas de 10 ms de los bordes), la energia RMS de la
//   salida debe ser al menos el 50 % de la de la fuente (-6 dB), y el salto
//   muestra-a-muestra no debe superar el de la fuente en esa ventana x1,05.
//
//   Caso medido en REAPER (captura en tiempo real, 2026-11-21): 18 huecos de
//   8-38 ms en uniones de nota donde la fuente suena (p. ej. 10,495-10,525 s,
//   17,857-17,898 s); la salida queda ~20 dB por debajo y el retorno es un
//   escalon. Este test fija ese comportamiento a nivel de render.

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
constexpr int kTotalFrames = 200;
// Dos notas contiguas; la union (70..78) es una consonante no vocalizada.
constexpr int kA0 = 20, kA1 = 78;
constexpr int kB0 = 78, kB1 = 140;
constexpr int kUv0 = 70, kUv1 = 78;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

juce::AudioBuffer<float> makePhrase() {
  juce::AudioBuffer<float> buffer(1, kTotalFrames * kHop);
  double phase = 0.0;
  unsigned int rng = 22222u;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float value = 0.0f;
    float midi = 0.0f;
    if (frame >= kA0 && frame < kUv0)
      midi = 57.0f;
    else if (frame >= kB0 && frame < kB1)
      midi = 59.0f;
    if (midi > 0.0f) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(midi) / kSampleRate;
      value = 0.30f * static_cast<float>(std::sin(phase)) +
              0.10f * static_cast<float>(std::sin(2.0 * phase));
    } else {
      phase = 0.0;
      if (frame >= kUv0 && frame < kUv1) {
        // Consonante: ruido determinista, no vocalizado, con energia.
        rng = rng * 1664525u + 1013904223u;
        value = 0.18f * (static_cast<float>((rng >> 8) & 0xFFFFu) / 32768.0f -
                         1.0f);
      }
    }
    buffer.setSample(0, i, value);
  }
  return buffer;
}

Project makeProject() {
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

  const std::pair<int, int> spans[] = {{kA0, kA1}, {kB0, kB1}};
  const float midis[] = {57.0f, 59.0f};
  for (int i = 0; i < 2; ++i) {
    for (int f = spans[i].first; f < spans[i].second; ++f) {
      if (f >= kUv0 && f < kUv1)
        continue;
      audio.voicedMask[static_cast<size_t>(f)] = true;
      audio.denseF0[static_cast<size_t>(f)] = midiHz(midis[i]);
    }
    Note note(spans[i].first, spans[i].second, midis[i]);
    note.setOriginalMidiNote(midis[i]);
    note.setEngine(NoteEngine::Inherit);
    note.setOriginalDeltaPitch(
        std::vector<float>(spans[i].second - spans[i].first, 0.0f));
    project.addNote(note);
  }
  for (int f = kUv0; f < kUv1; ++f) {
    audio.voicedMask[static_cast<size_t>(f)] = false;
    audio.denseF0[static_cast<size_t>(f)] = 0.0f;
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

float rms(const juce::AudioBuffer<float> &a, int first, int last) {
  double sum = 0.0;
  int n = 0;
  for (int i = std::max(0, first); i < std::min(a.getNumSamples(), last); ++i) {
    const double v = a.getSample(0, i);
    sum += v * v;
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(sum / n)) : 0.0f;
}

float maxStep(const juce::AudioBuffer<float> &a, int first, int last) {
  float worst = 0.0f;
  for (int i = std::max(1, first); i < std::min(a.getNumSamples(), last); ++i)
    worst = std::max(worst,
                     std::abs(a.getSample(0, i) - a.getSample(0, i - 1)));
  return worst;
}

void testUnionNoVocalizada() {
  Project project = makeProject();
  const juce::AudioBuffer<float> source =
      project.getAudioData().originalWaveform;

  // Edicion real de las dos notas (sin mover fronteras): +1 st.
  for (auto &note : project.getNotes()) {
    note.setPitchOffset(1.0f);
    note.markDirty();
    note.markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kA0, kB1);
  project.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "union PSOLA"));

  const auto &out = project.getAudioData().waveform;
  // Mitad interior del tramo no vocalizado (sin las rampas de borde).
  const int first = (kUv0 * kHop) + (kUv1 - kUv0) * kHop / 4;
  const int last = (kUv1 * kHop) - (kUv1 - kUv0) * kHop / 4;
  const float outRms = rms(out, first, last);
  const float srcRms = rms(source, first, last);
  const float ratio = srcRms > 0.0f ? outRms / srcRms : 0.0f;
  const float outStep = maxStep(out, kUv0 * kHop, kUv1 * kHop);
  const float srcStep = maxStep(source, kUv0 * kHop, kUv1 * kHop);

  std::printf("union no vocalizada: rms salida %.5f vs fuente %.5f (ratio %.3f, "
              "lim 0.500) | paso %.5f vs %.5f\n",
              outRms, srcRms, ratio, outStep, srcStep);
  CHECK(ratio >= 0.5f);
  CHECK(outStep <= srcStep * 1.05f);
}

void testUnionVocalizada() {
  Project project = makeProject();
  // Sin consonante: las dos notas contiguas, todo vocalizado.
  auto &audio = project.getAudioData();
  for (int f = kUv0; f < kUv1; ++f) {
    audio.voicedMask[static_cast<size_t>(f)] = true;
    audio.denseF0[static_cast<size_t>(f)] =
        (f < kB0) ? midiHz(57.0f) : midiHz(59.0f);
  }
  // Fuente vocalizada tambien (sin el ruido): la union es una senal continua.
  juce::AudioBuffer<float> &src = audio.originalWaveform;
  for (int i = kUv0 * kHop; i < kUv1 * kHop; ++i) {
    const float a = src.getSample(0, kUv0 * kHop - 1);
    const float b = src.getSample(0, kUv1 * kHop);
    const float t = static_cast<float>(i - kUv0 * kHop) /
                    static_cast<float>(kUv1 * kHop - kUv0 * kHop);
    src.setSample(0, i, a * (1.0f - t) + b * t);
  }
  audio.waveform.makeCopyOf(audio.originalWaveform);

  for (auto &note : project.getNotes()) {
    note.setPitchOffset(2.0f);
    note.markDirty();
    note.markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kA0, kB1);
  project.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "union vocalizada"));

  const auto &out = project.getAudioData().waveform;
  const int first = kB0 * kHop - 4 * kHop;
  const int last = kB0 * kHop + 4 * kHop;
  const float outRms = rms(out, first, last);
  const float srcRms = rms(audio.originalWaveform, first, last);
  const float ratio = srcRms > 0.0f ? outRms / srcRms : 0.0f;
  std::printf("union vocalizada (union de nota): rms salida %.5f vs fuente "
              "%.5f (ratio %.3f, lim 0.500)\n",
              outRms, srcRms, ratio);
  CHECK(ratio >= 0.5f);
}

} // namespace

int main() {
  testUnionNoVocalizada();
  testUnionVocalizada();
  std::printf("NoteBoundaryHoleTests: OK\n");
  return 0;
}
