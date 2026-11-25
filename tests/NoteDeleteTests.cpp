// M5 v2 / D-299: eliminar una nota.
//
// Criterio (A): la nota desaparece y su tramo de destino queda en silencio, con
// un fade corto. No se revela el audio original y las vecinas no se mueven ni
// se estiran. La nota queda como lapida (deleted + rest) para que el bloque
// D-129 se re-renderice y el silencio sobreviva a guardar/reabrir.
//
// Lo que se mide:
//   - El tramo eliminado queda <= -120 dBFS.
//   - La nota vecina de una frase D-129 separada no cambia (diff 0).
//   - Undo devuelve el audio y la seleccion; redo vuelve a callar.
//   - El flag viaja en la serializacion y "Seleccionar todo" no la incluye.
//   - Un reset neutral vecino que intenta copiar el original sobre la lapida no
//     consigue reintroducir la nota: el silencio se aplica al final del pase.

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Models/RenderPlan.h"
#include "../Source/Undo/NoteDeleteAction.h"
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
constexpr int kNote0End = 150;
constexpr int kNote1Start = 160; // 10 frames of silence: > 20 ms, D-129.
constexpr int kNote1End = 300;
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
    if ((frame >= kNote0Start && frame < kNote0End) ||
        (frame >= kNote1Start && frame < kNote1End)) {
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

  for (int frame = kNote0Start; frame < kNote0End; ++frame) {
    audio.voicedMask[static_cast<size_t>(frame)] = true;
    audio.denseF0[static_cast<size_t>(frame)] = midiHz(kNote0Midi);
  }
  for (int frame = kNote1Start; frame < kNote1End; ++frame) {
    audio.voicedMask[static_cast<size_t>(frame)] = true;
    audio.denseF0[static_cast<size_t>(frame)] = midiHz(kNote1Midi);
  }

  Note note0(kNote0Start, kNote0End, kNote0Midi);
  note0.setOriginalMidiNote(kNote0Midi);
  note0.setOriginalDeltaPitch(
      std::vector<float>(kNote0End - kNote0Start, 0.0f));
  Note note1(kNote1Start, kNote1End, kNote1Midi);
  note1.setOriginalMidiNote(kNote1Midi);
  note1.setOriginalDeltaPitch(
      std::vector<float>(kNote1End - kNote1Start, 0.0f));
  project.addNote(note0);
  project.addNote(note1);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

// clearAllDirty: el render de control limpia y usa el rango F0 completo. Los
// renders de una edicion NO deben limpiar la marca de la nota: el camino real
// llega con la nota sucia y el F0 sucio.
bool renderAndWait(Project &project, const char *label, bool clearAllDirty) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(SynthesisEngineType::Psola);
  if (clearAllDirty) {
    for (auto &note : project.getNotes())
      note.clearDirty();
    project.setF0DirtyRange(0, kTotalFrames);
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

void markDeleted(Project &project, size_t index) {
  auto &note = project.getNotes()[index];
  note.setDeleted(true);
  note.setRest(true);
  note.setSelected(false);
  note.markDirty();
  note.markSynthDirty();
  project.setF0DirtyRange(std::max(0, note.getStartFrame() - 24),
                          std::min(kTotalFrames, note.getEndFrame() + 24));
  project.setModified(true);
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

float maxAbsDiff(const juce::AudioBuffer<float> &a,
                 const juce::AudioBuffer<float> &b, int firstFrame,
                 int lastFrame) {
  const int first = std::max(0, firstFrame * kHop);
  const int last =
      std::min({a.getNumSamples(), b.getNumSamples(), lastFrame * kHop});
  float diff = 0.0f;
  for (int i = first; i < last; ++i)
    diff = std::max(diff, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return diff;
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;

  // Control: las dos notas suenan.
  Project project = makeProject();
  CHECK(renderAndWait(project, "control", /*clearAllDirty=*/true));
  const float note0Before =
      rms(project.getAudioData().waveform, kNote0Start + 5, kNote0End - 5);
  const float note1Before =
      rms(project.getAudioData().waveform, kNote1Start + 5, kNote1End - 5);
  std::printf("control: nota 0 %.1f dBFS, nota 1 %.1f dBFS\n", db(note0Before),
              db(note1Before));
  CHECK(db(note0Before) > -60.0f);
  CHECK(db(note1Before) > -60.0f);
  const juce::AudioBuffer<float> controlWaveform =
      project.getAudioData().waveform;

  // S1: eliminar la primera nota calla su tramo y no toca la frase vecina.
  markDeleted(project, 0);
  CHECK(renderAndWait(project, "delete note 0", /*clearAllDirty=*/false));
  const float note0Deleted =
      rms(project.getAudioData().waveform, kNote0Start + 5, kNote0End - 5);
  const float note1After =
      rms(project.getAudioData().waveform, kNote1Start + 5, kNote1End - 5);
  const float neighbourDiff =
      maxAbsDiff(controlWaveform, project.getAudioData().waveform,
                 kNote1Start + 5, kNote1End - 5);
  std::printf("eliminada: nota 0 %.1f dBFS, vecina %.1f dBFS, diff %.9f\n",
              db(note0Deleted), db(note1After), neighbourDiff);
  CHECK(db(note0Deleted) <= -120.0f);
  CHECK(neighbourDiff == 0.0f);

  // S2: undo devuelve la nota, su audio y la seleccion; redo vuelve a callar.
  Project undoProject = project;
  auto &undoNote = undoProject.getNotes()[0];
  NoteDeleteAction action(
      undoProject,
      {{&undoNote, /*beforeDeleted=*/false, /*beforeRest=*/false,
        /*beforeSelected=*/true, /*afterDeleted=*/true, /*afterRest=*/true,
        /*afterSelected=*/false}});
  action.undo();
  CHECK(!undoNote.isDeleted());
  CHECK(!undoNote.isRest());
  CHECK(undoNote.isSelected());
  CHECK(renderAndWait(undoProject, "undo delete", /*clearAllDirty=*/false));
  const float note0Back =
      rms(undoProject.getAudioData().waveform, kNote0Start + 5, kNote0End - 5);
  std::printf("undo: nota 0 %.1f dBFS\n", db(note0Back));
  CHECK(std::abs(db(note0Back) - db(note0Before)) <= 1.0f);

  action.redo();
  CHECK(undoNote.isDeleted());
  CHECK(undoNote.isRest());
  CHECK(!undoNote.isSelected());
  CHECK(renderAndWait(undoProject, "redo delete", /*clearAllDirty=*/false));
  const float note0Redone =
      rms(undoProject.getAudioData().waveform, kNote0Start + 5, kNote0End - 5);
  std::printf("redo: nota 0 %.1f dBFS\n", db(note0Redone));
  CHECK(db(note0Redone) <= -120.0f);

  // S3: serializacion y seleccion.
  Project serial = makeProject();
  markDeleted(serial, 1);
  const juce::var json = ProjectSerializer::toJson(serial);
  Project restored = makeProject();
  CHECK(ProjectSerializer::fromJson(restored, json));
  CHECK(restored.getNotes().size() == serial.getNotes().size());
  CHECK(restored.getNotes()[1].isDeleted());
  CHECK(restored.getNotes()[1].isRest());
  CHECK(!restored.getNotes()[0].isDeleted());
  restored.selectAllNotes(/*includeRests=*/true);
  CHECK(!restored.getNotes()[1].isSelected());
  CHECK(restored.getNotes()[0].isSelected());

  // S3b: la lapida cuenta como edicion y entra en el plan inmutable D-292.
  Project planProject = makeProject();
  markDeleted(planProject, 0);
  planProject.getNotes()[0].clearDirty();
  CHECK(planProject.hasRegionEdits());
  CHECK(planProject.markEditedNotesDirty() == 1);
  CHECK(planProject.getNotes()[0].isDirty());
  const auto plan = renderPlan::build(planProject, SynthesisEngineType::Psola, 7);
  CHECK(plan.blocks.size() == 1);
  CHECK(plan.blocks[0].notes.size() == 1);
  CHECK(plan.blocks[0].notes[0].index == 0);

  // S4: el reset neutral de una nota anterior no puede reintroducir la lapida.
  // La nota 1 es la ultima, asi que el tramo neutral de la nota 0 se extiende
  // hasta el final del pase y atraviesa la nota eliminada si el reset gana.
  Project resetProject = makeProject();
  markDeleted(resetProject, 1);
  CHECK(renderAndWait(resetProject, "delete last note", /*clearAllDirty=*/false));
  const float deletedBeforeReset =
      rms(resetProject.getAudioData().waveform, kNote1Start + 5,
          kNote1End - 5);
  CHECK(db(deletedBeforeReset) <= -120.0f);
  auto &neutral = resetProject.getNotes()[0];
  neutral.markDirty();
  neutral.markSynthDirty();
  resetProject.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(resetProject, "neutral reset next to delete",
                      /*clearAllDirty=*/false));
  const float deletedAfterReset =
      rms(resetProject.getAudioData().waveform, kNote1Start + 5,
          kNote1End - 5);
  std::printf("reset neutral: nota eliminada %.1f dBFS\n",
              db(deletedAfterReset));
  CHECK(db(deletedAfterReset) <= -120.0f);

  std::printf("NoteDeleteTests: silencio, vecina, undo/redo, JSON y reset OK\n");
  return 0;
}
