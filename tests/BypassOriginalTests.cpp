// D-260 (S5) - bypass del editor con el original como estado del documento.
//
// Criterio escrito ANTES de medir:
//   - Con el bypass activo, el interior publicado es la fuente exacta
//     (max|diff| = 0, limite 1e-7).
//   - El primer y el ultimo sample publicados son los del procesado
//     (el crossfade de 5 ms arranca/termina en el audio procesado).
//   - El paso muestra-a-muestra en cada borde no supera el del procesado ni el
//     de la fuente (x 1,05).
//   - Apagar el bypass devuelve el procesado bit a bit.
//   - El estado sobrevive a ProjectSerializer::toJson/fromJson.

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
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
constexpr int kFrames = 300;
constexpr int kN0a = 40, kN0b = 140, kN1b = 240;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

juce::AudioBuffer<float> makeSignal() {
  juce::AudioBuffer<float> buffer(1, kFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float midi = 0.0f;
    if (frame >= kN0a && frame < kN0b) midi = 57.0f;
    else if (frame >= kN0b && frame < kN1b) midi = 59.0f;
    float value = 0.0f;
    if (midi > 0.0f) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(midi) / kSampleRate;
      value = 0.3f * static_cast<float>(std::sin(phase));
    } else {
      phase = 0.0;
    }
    buffer.setSample(0, i, value);
  }
  return buffer;
}

Project makeProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const auto signal = makeSignal();
  audio.originalWaveform.makeCopyOf(signal);
  audio.waveform.makeCopyOf(signal);
  audio.melSpectrogram.assign(static_cast<size_t>(kFrames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(kFrames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(kFrames), false);
  const std::pair<int, int> spans[] = {{kN0a, kN0b}, {kN0b, kN1b}};
  const float midis[] = {57.0f, 59.0f};
  for (int i = 0; i < 2; ++i) {
    for (int f = spans[i].first; f < spans[i].second; ++f) {
      audio.voicedMask[static_cast<size_t>(f)] = true;
      audio.denseF0[static_cast<size_t>(f)] = midiHz(midis[i]);
    }
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

bool renderAndWait(Project &project) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(SynthesisEngineType::Psola);
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
  return finished.load() && success.load();
}

float maxAbsDiff(const juce::AudioBuffer<float> &a,
                 const juce::AudioBuffer<float> &b, int first, int last) {
  float worst = 0.0f;
  const int n = std::min({a.getNumSamples(), b.getNumSamples(), last});
  for (int i = std::max(0, first); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return worst;
}

float maxStep(const juce::AudioBuffer<float> &a, int first, int last) {
  float worst = 0.0f;
  const int n = std::min(a.getNumSamples(), last);
  for (int i = std::max(1, first); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - a.getSample(0, i - 1)));
  return worst;
}

}  // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  Project project = makeProject();
  const auto &original = project.getAudioData().originalWaveform;

  project.getNotes()[0].setMidiNote(58.0f);
  project.getNotes()[1].setMidiNote(60.0f);
  for (auto &note : project.getNotes()) {
    note.markDirty();
    note.markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN0a, kN1b);
  CHECK(renderAndWait(project));

  const juce::AudioBuffer<float> processed =
      project.getAudioData().waveform;
  CHECK(maxAbsDiff(processed, original, 0, original.getNumSamples()) > 1.0e-3f);

  project.setBypassProcessing(true);
  const auto published = project.buildPublishAudio();
  constexpr int kFade = 240;
  const int samples = published.getNumSamples();
  const float interior =
      maxAbsDiff(published, original, kFade, samples - kFade);
  const float edgeStart =
      std::abs(published.getSample(0, 0) - processed.getSample(0, 0));
  const float edgeEnd = std::abs(published.getSample(0, samples - 1) -
                                 processed.getSample(0, samples - 1));
  const float stepPublished = maxStep(published, 0, samples);
  const float stepProcessed = maxStep(processed, 0, samples);
  const float stepOriginal = maxStep(original, 0, samples);
  std::printf("bypass: interior vs fuente %.8f | borde ini %.8f fin %.8f | "
              "paso %.6f (proc %.6f, fuente %.6f)\n",
              interior, edgeStart, edgeEnd, stepPublished, stepProcessed,
              stepOriginal);
  CHECK(interior <= 1.0e-7f);
  CHECK(edgeStart <= 1.0e-7f);
  CHECK(edgeEnd <= 1.0e-7f);
  CHECK(stepPublished <= std::max(stepProcessed, stepOriginal) * 1.05f + 1.0e-6f);

  project.setBypassProcessing(false);
  const auto restored = project.buildPublishAudio();
  CHECK(maxAbsDiff(restored, processed, 0, processed.getNumSamples()) == 0.0f);

  // El estado es del documento.
  project.setBypassProcessing(true);
  Project roundTrip;
  CHECK(ProjectSerializer::fromJson(roundTrip, ProjectSerializer::toJson(project)));
  CHECK(roundTrip.isBypassProcessing());
  project.setBypassProcessing(false);

  std::printf("BypassOriginalTests: bypass con original OK\n");
  return 0;
}
