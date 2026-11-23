// Tarea A + B (2026-11-22): el reset borra la transicion de nota (D-225) y la
// cola de la ultima nota vuelve a la fuente.
//
// Criterio escrito ANTES de medir (no se ajusta despues del numero):
//
//   A · restablecer una nota por el camino de produccion
//       (PianoRollComponent::resetNoteToOriginal -> ResetNoteEditsAction).
//       Tras el reset: transition_out_ms == 0 y la nota es neutral
//       (isNeutralForOriginalWaveform); al renderizar, el tramo de la nota es la
//       fuente bit a bit: max|diff| <= 1e-7.
//
//   B · restablecer TODAS las notas y renderizar: la cola posterior al final de
//       la ultima nota (la region sigue teniendo audio de fuente ahi) tambien
//       es la fuente: max|diff| <= 1e-7 sobre [ultima.end, fin de la cola).
//
// Fixture: dos notas contiguas y una cola de decaimiento despues de la ultima
// (el caso real medido en REAPER: la cola quedaba re-sintetizada). La nota 1
// lleva transitionOutMs = 75, como las que el proyecto del campo trae del
// antiguo `slideMs` (medido: 0,259493969 de residuo en el reset).

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/UndoActions.h"
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
constexpr int kNote1End = 300;
constexpr int kTailEnd = 380;  // la fuente sigue sonando (decaimiento)
constexpr float kNote0Midi = 57.0f;
constexpr float kNote1Midi = 59.0f;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

// La fuente: dos notas y un decaimiento exponencial DESPUES del final de la
// ultima (donde no hay nota pero si material, como en una toma real).
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
    } else if (frame >= kNote1End && frame < kTailEnd) {
      // Cola de la ultima nota: mismo tono, amplitud que cae.
      const float t = static_cast<float>(frame - kNote1End) /
                      static_cast<float>(kTailEnd - kNote1End);
      const float gain = 0.30f * (1.0f - t);
      phase += 2.0 * 3.14159265358979323846 * midiHz(kNote1Midi) / kSampleRate;
      value = gain * static_cast<float>(std::sin(phase));
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
  for (int f = kNote0Start; f < kNote1End; ++f) {
    audio.voicedMask[static_cast<size_t>(f)] = true;
    audio.denseF0[static_cast<size_t>(f)] =
        midiHz(f < kNote0End ? kNote0Midi : kNote1Midi);
  }
  // La cola NO esta voz para el detector (asi llega del analisis real: la nota
  // termina antes que el material). Es lo que hace que el motor la re-sintetice
  // en vez de copiarla.
  Note n0(kNote0Start, kNote0End, kNote0Midi);
  n0.setOriginalMidiNote(kNote0Midi);
  n0.setOriginalDeltaPitch(std::vector<float>(kNote0End - kNote0Start, 0.0f));
  Note n1(kNote0End, kNote1End, kNote1Midi);
  n1.setOriginalMidiNote(kNote1Midi);
  n1.setOriginalDeltaPitch(std::vector<float>(kNote1End - kNote0End, 0.0f));
  // D-225 persistida (el antiguo slideMs): es la que impedia el reset exacto.
  n1.setTransitionOutMs(75);
  project.addNote(n0);
  project.addNote(n1);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

bool renderAndWait(Project &project, const char *label) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(SynthesisEngineType::Psola);
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

float maxAbsDiff(const juce::AudioBuffer<float> &a,
                 const juce::AudioBuffer<float> &b, int firstFrame,
                 int lastFrame) {
  float worst = 0.0f;
  const int n = std::min({a.getNumSamples(), b.getNumSamples(), lastFrame * kHop});
  for (int i = std::max(0, firstFrame * kHop); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return worst;
}

void editAndRender(Project &project) {
  auto &notes = project.getNotes();
  notes[1].setMidiNote(notes[1].getMidiNote() + 2.0f);
  for (auto &note : notes) {
    note.markDirty();
    note.markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kNote0Start, kNote1End);
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;

  // ---------------- A: reset de una nota borra la transicion ----------------
  {
    Project project = makeProject();
    const juce::AudioBuffer<float> source =
        project.getAudioData().originalWaveform;
    editAndRender(project);
    CHECK(renderAndWait(project, "A editado"));

    PianoRollComponent roll;
    PitchUndoManager history;
    roll.setProject(&project);
    roll.setUndoManager(&history);

    const int before = project.getNotes()[1].getTransitionOutMs();
    const bool applied = roll.resetNoteToOriginal(1);
    const auto &note = project.getNotes()[1];
    std::printf("A reset de nota: aplicado %d | transition antes %d ms, despues "
                "%d ms | neutral %d\n",
                applied ? 1 : 0, before, note.getTransitionOutMs(),
                note.isNeutralForOriginalWaveform() ? 1 : 0);
    CHECK(applied);
    CHECK(before == 75);
    CHECK(note.getTransitionOutMs() == 0);
    CHECK(note.isNeutralForOriginalWaveform());

    CHECK(renderAndWait(project, "A restablecido"));
    const float noteDiff =
        maxAbsDiff(project.getAudioData().waveform, source, kNote0End + 2,
                   kNote1End - 2);
    std::printf("A nota 1 vs fuente: max|diff| %.9f (lim 1e-7)\n", noteDiff);
    CHECK(noteDiff <= 1.0e-7f);
  }

  // ---------------- B: reset de todas y la cola de la ultima nota -----------
  {
    Project project = makeProject();
    const juce::AudioBuffer<float> source =
        project.getAudioData().originalWaveform;
    editAndRender(project);
    CHECK(renderAndWait(project, "B editado"));

    PianoRollComponent roll;
    PitchUndoManager history;
    roll.setProject(&project);
    roll.setUndoManager(&history);
    for (int i = 0; i < static_cast<int>(project.getNotes().size()); ++i)
      CHECK(roll.resetNoteToOriginal(i));
    CHECK(renderAndWait(project, "B restablecido"));

    const float tailDiff =
        maxAbsDiff(project.getAudioData().waveform, source, kNote1End + 1,
                   kTailEnd - 1);
    const float bodyDiff =
        maxAbsDiff(project.getAudioData().waveform, source, kNote0Start + 5,
                   kNote1End - 5);
    std::printf("B reset de todas: cuerpo vs fuente %.9f | COLA [%d,%d) vs "
                "fuente %.9f (lim 1e-7)\n",
                bodyDiff, kNote1End, kTailEnd, tailDiff);
    CHECK(bodyDiff <= 1.0e-7f);
    CHECK(tailDiff <= 1.0e-7f);
  }

  std::printf("NoteTransitionResetTests: en verde\n");
  return 0;
}
