// D-257/D-258 - motor por bloque.
//
// Criterio escrito ANTES de medir:
//   - Elegir un motor en una nota del bloque A cambia TODAS las notas del
//     bloque A y ninguna de otro bloque.
//   - `overwritten` cuenta las notas cuyo motor EFECTIVO cambia.
//   - Al elegir Clasico (PSOLA), `outOfBenefitRange` lista las notas del bloque
//     cuyo desplazamiento (altura objetivo - altura original) queda fuera de
//     [-1,5; +2,5] st, con su numero; el cambio se aplica igual.
//   - Al elegir Vocoder, el motor anterior de cada nota queda en
//     `blockEngineOverride` para poder volver.
//   - T8: mover una frontera de timing dentro del silencio no cambia el motor
//     ni el numero de bloques (el silencio se mide en coordenadas de fuente).

#include "../Source/Audio/Synthesis/EngineBlocks.h"
#include "../Source/Models/Project.h"
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
constexpr int kA0 = 40, kA1 = 120, kA2 = 200;
constexpr int kAEnd = 280;
constexpr int kGapStart = 280, kGapEnd = 320;
constexpr int kB0 = 320, kBEnd = 400;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

juce::AudioBuffer<float> makeSignal() {
  juce::AudioBuffer<float> buffer(1, kFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float midi = 0.0f;
    if (frame >= kA0 && frame < kA1)      midi = 57.0f;
    else if (frame >= kA1 && frame < kA2) midi = 59.0f;
    else if (frame >= kA2 && frame < kAEnd) midi = 60.0f;
    else if (frame >= kB0 && frame < kBEnd) midi = 62.0f;
    float value = 0.0f;
    if (midi > 0.0f) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(midi) / kSampleRate;
      value = 0.3f * static_cast<float>(std::sin(phase)) +
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
  const auto signal = makeSignal();
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

const juce::AudioBuffer<float> &sourceOf(const Project &project) {
  return project.getAudioData().originalWaveform;
}

int blockIndexOf(const std::vector<engineBlocks::Block> &plan, int frame) {
  for (size_t i = 0; i < plan.size(); ++i)
    if (plan[i].startFrame <= frame && frame < plan[i].endFrame)
      return static_cast<int>(i);
  return -1;
}

void applyPlan(std::vector<Note> &notes,
               const engineBlocks::BlockEngineEdit &plan) {
  for (const auto &edit : plan.edits) {
    auto &note = notes[static_cast<size_t>(edit.noteIndex)];
    note.setEngine(edit.next);
    note.setBlockEngineOverride(edit.next == NoteEngine::Vocoder
                                    ? edit.previous
                                    : NoteEngine::Inherit);
  }
}

void testVocoderAppliesToWholeBlock() {
  Project project = makeProject();
  const auto &source = sourceOf(project);
  auto &notes = project.getNotes();

  const auto plan = engineBlocks::planBlockEngineEdit(
      notes, 1, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
      kSampleRate, kHop);
  std::printf("bloque del ancla 1: %d nota(s), sobrescritas %d\n",
              static_cast<int>(plan.blockNotes.size()), plan.overwritten);
  CHECK(plan.blockNotes.size() == 3);
  CHECK(plan.overwritten == 3);
  CHECK(plan.edits.size() == 3);
  CHECK(plan.outOfBenefitRange.empty());

  applyPlan(notes, plan);
  CHECK(notes[0].getEngine() == NoteEngine::Vocoder);
  CHECK(notes[1].getEngine() == NoteEngine::Vocoder);
  CHECK(notes[2].getEngine() == NoteEngine::Vocoder);
  // Fuera del bloque no se toca nada.
  CHECK(notes[3].getEngine() == NoteEngine::Inherit);

  // El motor anterior queda guardado para poder volver.
  CHECK(notes[0].getBlockEngineOverride() == NoteEngine::Inherit);
  CHECK(notes[1].getBlockEngineOverride() == NoteEngine::Inherit);
  CHECK(notes[2].getBlockEngineOverride() == NoteEngine::Inherit);

  // Con el bloque ya en Vocoder, el plan no cambia nada: 0 sobrescritas.
  const auto again = engineBlocks::planBlockEngineEdit(
      notes, 2, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
      kSampleRate, kHop);
  CHECK(again.overwritten == 0);
  CHECK(again.edits.empty());
  CHECK(again.changesSound == false);
}

void testClassicReportsOutOfBenefitRange() {
  Project project = makeProject();
  const auto &source = sourceOf(project);
  auto &notes = project.getNotes();

  // Nota 0: +3 st (fuera del rango). Nota 1: -2 st (fuera). Nota 2: +1 st
  // (dentro). El desplazamiento es altura objetivo - altura fuente.
  notes[0].setMidiNote(60.0f);
  notes[1].setMidiNote(57.0f);
  notes[2].setMidiNote(61.0f);

  const auto plan = engineBlocks::planBlockEngineEdit(
      notes, 0, NoteEngine::Psola, SynthesisEngineType::Psola, source,
      kSampleRate, kHop);
  std::printf("aviso clasico: fuera de rango %d nota(s):",
              static_cast<int>(plan.outOfBenefitRange.size()));
  for (const int index : plan.outOfBenefitRange)
    std::printf(" #%d", index);
  std::printf("\n");
  CHECK(plan.outOfBenefitRange.size() == 2);
  CHECK(std::find(plan.outOfBenefitRange.begin(),
                  plan.outOfBenefitRange.end(),
                  0) != plan.outOfBenefitRange.end());
  CHECK(std::find(plan.outOfBenefitRange.begin(),
                  plan.outOfBenefitRange.end(),
                  1) != plan.outOfBenefitRange.end());
  // El motor pedido ya era el efectivo: 0 sobrescritas, pero el aviso existe.
  CHECK(plan.overwritten == 0);
  CHECK(plan.outOfBenefitRange.size() == 2);
}

void testOtherBlockUntouched() {
  Project project = makeProject();
  const auto &source = sourceOf(project);
  auto &notes = project.getNotes();

  const auto planA = engineBlocks::planBlockEngineEdit(
      notes, 0, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
      kSampleRate, kHop);
  CHECK(planA.blockNotes.size() == 3);
  CHECK(std::find(planA.blockNotes.begin(), planA.blockNotes.end(), 3) ==
        planA.blockNotes.end());

  // El bloque B (tras el silencio) se edita por separado y solo tiene 1 nota.
  const auto planB = engineBlocks::planBlockEngineEdit(
      notes, 3, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
      kSampleRate, kHop);
  CHECK(planB.blockNotes.size() == 1);
  CHECK(planB.blockNotes.front() == 3);
  CHECK(planB.overwritten == 1);
}

void testTimingMoveKeepsEnginePlan() {
  Project project = makeProject();
  const auto &source = sourceOf(project);
  auto &notes = project.getNotes();

  for (int i = 0; i < 3; ++i)
    notes[static_cast<size_t>(i)].setEngine(NoteEngine::Vocoder);
  notes[3].setEngine(NoteEngine::Psola);

  auto plan = engineBlocks::buildPlan(notes, 0, kFrames,
                                      SynthesisEngineType::Psola, source,
                                      kSampleRate, kHop);
  CHECK(plan.size() == 2);
  CHECK(plan[0].engine == SynthesisEngineType::Vocoder);
  CHECK(plan[1].engine == SynthesisEngineType::Psola);

  // T8: mover la frontera de timing de la nota B dentro del silencio no
  // cambia el motor ni crea otro bloque (el silencio esta en la fuente).
  notes[3].setStartFrame(notes[3].getStartFrame() - 8);
  notes[3].setEndFrame(notes[3].getEndFrame() - 8);
  auto moved = engineBlocks::buildPlan(notes, 0, kFrames,
                                       SynthesisEngineType::Psola, source,
                                       kSampleRate, kHop);
  std::printf("T8 plan tras mover la frontera: %d bloque(s)\n",
              static_cast<int>(moved.size()));
  CHECK(moved.size() == 2);
  CHECK(moved[0].engine == SynthesisEngineType::Vocoder);
  CHECK(moved[1].engine == SynthesisEngineType::Psola);
  CHECK(blockIndexOf(moved, 60) == 0);
  CHECK(blockIndexOf(moved, 330) == 1);
}

void testVisualBlockMatchesTheGesture() {
  // D-309: the engine drawer's visual block and the block gesture are the same
  // phrase. Changing any note of it reaches exactly the same note set.
  Project project = makeProject();
  const auto &source = sourceOf(project);
  auto &notes = project.getNotes();
  const auto map = engineBlocks::buildVisualBlocks(
      notes, SynthesisEngineType::Psola, source, kSampleRate, kHop);
  CHECK(map.blocks.size() == 2);
  CHECK(map.blockForNote(0) == map.blockForNote(1));
  CHECK(map.blockForNote(1) == map.blockForNote(2));
  CHECK(map.blockForNote(3) != map.blockForNote(2));

  for (int anchor = 0; anchor < 4; ++anchor) {
    const auto plan = engineBlocks::planBlockEngineEdit(
        notes, anchor, NoteEngine::Vocoder, SynthesisEngineType::Psola, source,
        kSampleRate, kHop);
    const int blockIndex = map.blockForNote(anchor);
    CHECK(blockIndex >= 0);
    const auto &block = map.blocks[static_cast<size_t>(blockIndex)];
    CHECK(plan.blockNotes == block.noteIndices);
  }
  std::printf("D-309 bloque visual: %d bloque(s), gesto identico\n",
              static_cast<int>(map.blocks.size()));
}

}  // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  testVocoderAppliesToWholeBlock();
  testClassicReportsOutOfBenefitRange();
  testOtherBlockUntouched();
  testTimingMoveKeepsEnginePlan();
  testVisualBlockMatchesTheGesture();
  std::printf("BlockEngineTests: motor por bloque OK\n");
  return 0;
}
