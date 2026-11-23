// M5 v1 / D-245: silenciar y reactivar una nota.
//
// Criterio (mismo formato que el resto de M5): una feature se mide.
//   - Silenciar la nota 0 la deja inaudible en el audio renderizado (RMS del
//     tramo por debajo de -60 dBFS) y NO toca el nivel de la nota 1.
//   - Reactivar (undo / setMuted(false)) devuelve el audio a su nivel anterior.
//   - El flag viaja en la serializacion del proyecto.

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Undo/NoteMuteAction.h"
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
constexpr int kTotalFrames = 420;
constexpr int kNote0Start = 50;
constexpr int kNote0End = 200;
constexpr int kNote1End = 350;
constexpr float kNote0Midi = 57.0f;
constexpr float kNote1Midi = 59.0f;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

juce::AudioBuffer<float> makePhrase() {
  juce::AudioBuffer<float> buffer(1, kTotalFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float value = 0.0f;
    if (frame >= kNote0Start && frame < kNote1End) {
      const float midi = frame < kNote0End ? kNote0Midi : kNote1Midi;
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

  for (int frame = kNote0Start; frame < kNote1End; ++frame) {
    audio.voicedMask[static_cast<size_t>(frame)] = true;
    audio.denseF0[static_cast<size_t>(frame)] =
        midiHz(frame < kNote0End ? kNote0Midi : kNote1Midi);
  }

  Note note0(kNote0Start, kNote0End, kNote0Midi);
  note0.setOriginalMidiNote(kNote0Midi);
  note0.setOriginalDeltaPitch(std::vector<float>(kNote0End - kNote0Start, 0.0f));
  Note note1(kNote0End, kNote1End, kNote1Midi);
  note1.setOriginalMidiNote(kNote1Midi);
  note1.setOriginalDeltaPitch(std::vector<float>(kNote1End - kNote0End, 0.0f));
  project.addNote(note0);
  project.addNote(note1);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

bool renderAndWait(Project &project, const char *label) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(SynthesisEngineType::Psola);
  for (auto &note : project.getNotes())
    note.clearDirty();
  project.setF0DirtyRange(0, kTotalFrames);

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

float rms(const juce::AudioBuffer<float> &buffer, int firstFrame,
          int lastFrame) {
  double sum = 0.0;
  int count = 0;
  const int first = std::max(0, firstFrame * kHop);
  const int last = std::min(buffer.getNumSamples(), lastFrame * kHop);
  for (int i = first; i < last; ++i) {
    const double v = buffer.getSample(0, i);
    sum += v * v;
    ++count;
  }
  if (count == 0)
    return 0.0f;
  return static_cast<float>(std::sqrt(sum / count));
}

float db(float amplitude) {
  return 20.0f * std::log10(std::max(amplitude, 1.0e-9f));
}

// La edicion que hace el boton: marcar y re-renderizar.
void markMute(Project &project, size_t index, bool muted) {
  auto &note = project.getNotes()[index];
  note.setMuted(muted);
  note.markDirty();
  note.markSynthDirty();
  project.setF0DirtyRange(std::max(0, note.getStartFrame() - 24),
                          std::min(kTotalFrames, note.getEndFrame() + 24));
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;

  Project project = makeProject();
  CHECK(renderAndWait(project, "control"));

  const float note0Before =
      rms(project.getAudioData().waveform, kNote0Start + 5, kNote0End - 5);
  const float note1Before =
      rms(project.getAudioData().waveform, kNote0End + 5, kNote1End - 5);
  std::printf("control: nota 0 %.1f dBFS, nota 1 %.1f dBFS\n", db(note0Before),
              db(note1Before));
  CHECK(db(note0Before) > -60.0f);

  // S1: silenciar deja la nota inaudible.
  markMute(project, 0, true);
  CHECK(renderAndWait(project, "mute"));

  const float note0Muted =
      rms(project.getAudioData().waveform, kNote0Start + 5, kNote0End - 5);
  const float note1After =
      rms(project.getAudioData().waveform, kNote0End + 5, kNote1End - 5);
  std::printf("silenciada: nota 0 %.1f dBFS, nota 1 %.1f dBFS\n",
              db(note0Muted), db(note1After));
  CHECK(db(note0Muted) <= -60.0f);
  // La vecina no se ensordece: sigue a su nivel (tolerancia de 1 dB).
  CHECK(std::abs(db(note1After) - db(note1Before)) <= 1.0f);

  // S2: reactivar devuelve el audio (undo del flag por la accion real).
  Project undoProject = project;
  NoteMuteAction undoAction(
      undoProject,
      {{&undoProject.getNotes()[0], /*before=*/false, /*after=*/true}});
  undoAction.undo();
  CHECK(!undoProject.getNotes()[0].isMuted());
  CHECK(renderAndWait(undoProject, "unmute"));
  const float note0Back = rms(undoProject.getAudioData().waveform,
                              kNote0Start + 5, kNote0End - 5);
  std::printf("reactivada: nota 0 %.1f dBFS\n", db(note0Back));
  CHECK(std::abs(db(note0Back) - db(note0Before)) <= 1.0f);

  // S3: el flag viaja en la serializacion.
  Project muted = makeProject();
  muted.getNotes()[0].setMuted(true);
  const juce::var json = ProjectSerializer::toJson(muted);
  Project restored = makeProject();
  CHECK(ProjectSerializer::fromJson(restored, json));
  CHECK(restored.getNotes().size() == muted.getNotes().size());
  CHECK(restored.getNotes()[0].isMuted());
  CHECK(!restored.getNotes()[1].isMuted());

  std::printf("NoteMuteTests: silenciar, reactivar y serializar OK\n");
  return 0;
}
