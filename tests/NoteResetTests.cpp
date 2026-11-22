// D-246 (candidato): "restablecer original" tiene que devolver el audio de FUENTE.
//
// Escenario del campo (2026-11-21): un proyecto con correccion de altura y
// cuantizacion de tiempo guardado; al seleccionar todo y "restablecer original"
// el audio seguia siendo una resintesis (y con clics).
//
// Este test reproduce la secuencia con el camino de produccion:
//   1. proyecto neutral -> render (control = fuente)
//   2. altura corregida a semitonos + fronteras movidas -> render (editado)
//   3. restablecer (NoteEditState::defaultsFor + fronteras de fuente) -> render
//   4. el audio del tramo restablecido debe ser EXACTAMENTE el de la fuente.

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
  for (int f = kNote0Start; f < kNote1End; ++f) {
    audio.voicedMask[static_cast<size_t>(f)] = true;
    audio.denseF0[static_cast<size_t>(f)] =
        midiHz(f < kNote0End ? kNote0Midi : kNote1Midi);
  }
  Note n0(kNote0Start, kNote0End, kNote0Midi);
  n0.setOriginalMidiNote(kNote0Midi);
  n0.setOriginalDeltaPitch(std::vector<float>(kNote0End - kNote0Start, 0.0f));
  Note n1(kNote0End, kNote1End, kNote1Midi);
  n1.setOriginalMidiNote(kNote1Midi);
  n1.setOriginalDeltaPitch(std::vector<float>(kNote1End - kNote0End, 0.0f));
  project.addNote(n0);
  project.addNote(n1);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

bool renderAndWait(Project &project, const char *label) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(SynthesisEngineType::Psola);
  // NO se limpian los dirty: la ruta de "restablecer" depende de que la nota
  // siga marcada para que el render copie la fuente (reset spans).
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

// Lo que hace ResetNoteEditsAction: defaults por nota, fronteras de fuente y
// marcar el rango como editado.
void resetAll(Project &project) {
  for (auto &note : project.getNotes()) {
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
    note.setBlockEngineOverride(NoteEngine::Inherit);
    note.markDirty();
    note.markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kNote0Start, kNote1End);
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

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;

  // Control: la fuente tal cual (el proyecto empieza neutral y sin render).
  Project project = makeProject();
  const juce::AudioBuffer<float> control = project.getAudioData().originalWaveform;

  // 1. Editar como se guardo en el proyecto del campo: altura a semitonos y
  //    fronteras movidas +3 frames.
  auto &notes = project.getNotes();
  notes[0].setMidiNote(std::round(notes[0].getMidiNote()));
  notes[1].setMidiNote(std::round(notes[1].getMidiNote()));
  for (auto &note : notes) {
    note.setStartFrame(note.getStartFrame() + 3);
    note.setEndFrame(note.getEndFrame() + 3);
    note.markDirty();
    note.markSynthDirty();
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kNote0Start, kNote1End + 3);
  CHECK(renderAndWait(project, "editado"));

  const float editedDiff =
      maxAbsDiff(project.getAudioData().waveform, control, kNote0Start + 5,
                 kNote1End - 5);
  std::printf("editado vs control: max |diff| %.4f (tiene que ser grande)\n",
              editedDiff);
  CHECK(editedDiff > 1.0e-3f);

  // 2. Restablecer original: el tramo debe volver a ser la fuente, muestra a
  //    muestra.
  resetAll(project);
  CHECK(renderAndWait(project, "restablecido"));

  const float restoredDiff = maxAbsDiff(project.getAudioData().waveform,
                                        project.getAudioData().originalWaveform,
                                        kNote0Start + 5, kNote1End - 5);
  const float restoredVsControl =
      maxAbsDiff(project.getAudioData().waveform, control, kNote0Start + 5,
                 kNote1End - 5);
  std::printf("restablecido vs fuente: max |diff| %.8f\n", restoredDiff);
  std::printf("restablecido vs control: max |diff| %.8f\n", restoredVsControl);
  CHECK(restoredDiff <= 1.0e-6f);
  CHECK(restoredVsControl <= 1.0e-6f);

  // 3. D-247 (candidato): proyecto neutro + motor explicito por nota.
  //    En el campo esto dejo el proyecto EN SILENCIO. Debe sonar la fuente
  //    (o el render neutro), nunca silencio.
  for (auto &note : project.getNotes()) {
    note.setEngine(NoteEngine::Psola);
    note.markDirty();
    note.markSynthDirty();
  }
  project.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(project, "motor explicito"));

  float peak = 0.0f;
  for (int i = 0; i < project.getAudioData().waveform.getNumSamples(); ++i)
    peak = std::max(peak, std::abs(project.getAudioData().waveform.getSample(0, i)));
  const float engineDiff = maxAbsDiff(project.getAudioData().waveform,
                                      project.getAudioData().originalWaveform,
                                      kNote0Start + 5, kNote1End - 5);
  std::printf("motor explicito: pico %.6f, vs fuente max |diff| %.6f\n", peak,
              engineDiff);
  CHECK(peak > 0.01f);

  // 4. T6 / D-258: motor elegido YA renderizado + mute. Restablecer tiene que
  //    limpiar las dos cosas y devolver la fuente exacta (caso C). Antes el
  //    reset dejo motor y mute sin limpiar.
  Project resetCase = makeProject();
  resetCase.getNotes()[0].setEngine(NoteEngine::Psola);
  resetCase.getNotes()[0].setMuted(true);
  for (auto &note : resetCase.getNotes()) {
    note.markDirty();
    note.markSynthDirty();
  }
  resetCase.markNoteEditDirtyRange(kNote0Start, kNote1End);
  CHECK(renderAndWait(resetCase, "motor renderizado + mute"));
  CHECK(resetCase.getNotes()[0].hasRenderedEdit());

  resetAll(resetCase);
  CHECK(!resetCase.getNotes()[0].isMuted());
  CHECK(resetCase.getNotes()[0].getEngine() == NoteEngine::Inherit);
  CHECK(renderAndWait(resetCase, "reset tras motor + mute"));
  const float resetCaseDiff = maxAbsDiff(
      resetCase.getAudioData().waveform,
      resetCase.getAudioData().originalWaveform, kNote0Start + 5,
      kNote1End - 5);
  std::printf("T6 reset tras motor renderizado + mute: vs fuente %.8f\n",
              resetCaseDiff);
  CHECK(resetCaseDiff <= 1.0e-7f);

  std::printf("NoteResetTests: restablecer original devuelve la fuente\n");
  return 0;
}
