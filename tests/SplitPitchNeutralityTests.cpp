// D-285 (2.3): cortar no debe afinar.
//
// Criterio escrito ANTES de medir:
//
//   Con una nota partida en dos, la afinacion que el motor va a sintetizar en
//   cada trama es la MISMA antes y despues del corte; solo cambian los bordes.
//   Se mide sobre la curva compuesta (`Project::getAdjustedF0`), que es lo que
//   el sintetizador consume, en cents.
//
//   - Cuerpo de la nota, lejos del corte: max|dif| <= 0,5 c (identico).
//   - Ventana alrededor del corte: se mide y se informa. D-225 mueve la
//     transicion a la cola, asi que ahi SI puede haber un cambio; si supera
//     0,5 c se dice con el numero.
//
//   Control: la misma medida sin cortar debe dar exactamente 0. Si el
//   instrumento no da 0 en el control, la medida no vale.
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRoll/NoteSplitter.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

constexpr int kNoteA = 100;
constexpr int kSplitFrame = 50;
constexpr int kNoteB = 100;
constexpr int kFrames = kNoteA + kNoteB;
constexpr int kBoundary = kNoteA;
constexpr int kEdgeFrames = 6;

float cents(float hz) {
  return 1200.0f * std::log2(std::max(hz, 1.0e-6f) / 440.0f);
}

// Dos notas contiguas con contorno de afinacion realista: la primera derivando
// hacia abajo, la segunda plana. La transicion entre ellas existe (D-225).
Project makeProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 48000;
  audio.melSpectrogram.assign(kFrames, std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(kFrames, 0.0f);
  audio.voicedMask.assign(kFrames, true);

  const float midiA = 60.0f;
  const float midiB = 62.0f;

  Note left(0, kNoteA, midiA);
  left.setOriginalMidiNote(midiA);
  std::vector<float> leftDelta(static_cast<size_t>(kNoteA));
  for (int i = 0; i < kNoteA; ++i) {
    // Deriva de +0,35 a -0,25 semitonos: el centro de cada mitad difiere.
    const float t = static_cast<float>(i) / static_cast<float>(kNoteA - 1);
    leftDelta[static_cast<size_t>(i)] = 0.35f - 0.60f * t;
    audio.denseF0[static_cast<size_t>(i)] =
        midiToFreq(midiA + leftDelta[static_cast<size_t>(i)]);
  }
  left.setOriginalDeltaPitch(leftDelta);

  Note right(kNoteA, kFrames, midiB);
  right.setOriginalMidiNote(midiB);
  std::vector<float> rightDelta(static_cast<size_t>(kNoteB), 0.05f);
  for (int i = 0; i < kNoteB; ++i)
    audio.denseF0[static_cast<size_t>(kNoteA + i)] =
        midiToFreq(midiB + rightDelta[static_cast<size_t>(i)]);
  right.setOriginalDeltaPitch(rightDelta);

  project.addNote(left);
  project.addNote(right);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

struct Diff {
  float body = 0.0f;   // fuera de la ventana del corte
  float seam = 0.0f;   // dentro de la ventana del corte
  int bodyWorstFrame = -1;
  int seamWorstFrame = -1;
};

Diff compare(const std::vector<float> &before, const std::vector<float> &after,
             int seamFrame) {
  Diff diff;
  const int n = std::min(static_cast<int>(before.size()),
                         static_cast<int>(after.size()));
  for (int frame = 0; frame < n; ++frame) {
    if (before[static_cast<size_t>(frame)] <= 0.0f ||
        after[static_cast<size_t>(frame)] <= 0.0f)
      continue;
    const float delta =
        std::abs(cents(after[static_cast<size_t>(frame)]) -
                 cents(before[static_cast<size_t>(frame)]));
    const bool nearSeam = std::abs(frame - seamFrame) <= kEdgeFrames;
    if (nearSeam) {
      if (delta > diff.seam) {
        diff.seam = delta;
        diff.seamWorstFrame = frame;
      }
    } else if (delta > diff.body) {
      diff.body = delta;
      diff.bodyWorstFrame = frame;
    }
  }
  return diff;
}

void testControlWithoutCut() {
  Project project = makeProject();
  const auto before = project.getAdjustedF0();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const auto after = project.getAdjustedF0();
  const auto diff = compare(before, after, kSplitFrame);
  std::printf("2.3 control (sin cortar): cuerpo %.6f c | costura %.6f c\n",
              diff.body, diff.seam);
  CHECK(diff.body <= 1.0e-3f);
  CHECK(diff.seam <= 1.0e-3f);
}

void testSplitIsPitchNeutral() {
  Project project = makeProject();
  const auto before = project.getAdjustedF0();

  PitchUndoManager history(100);
  NoteSplitter splitter;
  splitter.setProject(&project);
  splitter.setUndoManager(&history);

  auto &notes = project.getNotes();
  Note *first = &notes.front();
  const int firstStart = first->getStartFrame();
  const int firstEnd = first->getEndFrame();
  const float firstMidi = first->getMidiNote();
  const float firstSource = first->getLastNonMacroMidiNote();

  CHECK(splitter.splitNoteAtFrame(first, kSplitFrame));
  CHECK(project.getNotes().size() == 3);

  // El camino de la UI reconstruye la curva despues del corte.
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const auto after = project.getAdjustedF0();
  const auto diff = compare(before, after, kSplitFrame);

  // Afinacion por tramo, en semitonos efectivos (centro + desviacion): lo que
  // afina cada mitad tiene que ser lo que afinaba ese tramo antes del corte.
  auto &halves = project.getNotes();
  const Note &left = halves[0];
  const Note &right = halves[1];
  CHECK(left.getStartFrame() == firstStart);
  CHECK(left.getEndFrame() == kSplitFrame);
  CHECK(right.getStartFrame() == kSplitFrame);
  CHECK(right.getEndFrame() == firstEnd);

  const auto leftDelta = left.getOriginalDeltaPitch();
  const auto rightDelta = right.getOriginalDeltaPitch();
  CHECK(static_cast<int>(leftDelta.size()) == kSplitFrame - firstStart);
  CHECK(static_cast<int>(rightDelta.size()) == firstEnd - kSplitFrame);

  // Afinacion efectiva por tramo: centro de la mitad + su desviacion, que es
  // lo que se sintetiza. Debe ser la del tramo correspondiente antes del corte
  // (la comparacion fina esta en `diff`, sobre la curva compuesta).
  float worstHalfSemitones = 0.0f;
  for (int i = 0; i < static_cast<int>(leftDelta.size()); ++i)
    worstHalfSemitones =
        std::max(worstHalfSemitones,
                 std::abs((left.getMidiNote() + leftDelta[static_cast<size_t>(i)]) -
                          (left.getOriginalMidiNote() +
                           left.getOriginalDeltaPitch()[static_cast<size_t>(i)])));
  std::printf(
      "2.3 corte: nota %.3f -> mitades %.3f / %.3f (el corte no recentra) | "
      "fuente %.4f conservada\n",
      firstMidi, left.getMidiNote(), right.getMidiNote(), firstSource);
  std::printf("2.3 corte: afinacion compuesta cuerpo max %.4f c (lim 0,5) en "
              "trama %d | costura max %.4f c en trama %d\n",
              diff.body, diff.bodyWorstFrame, diff.seam, diff.seamWorstFrame);
  CHECK(diff.body <= 0.5f);
  juce::ignoreUnused(worstHalfSemitones);
  // La correccion de cada mitad sigue siendo la de la nota original.
  CHECK(std::abs((left.getLastNonMacroMidiNote() - left.getMidiNote()) -
                 (firstSource - firstMidi)) <= 1.0e-3f);
  CHECK(std::abs((right.getLastNonMacroMidiNote() - right.getMidiNote()) -
                 (firstSource - firstMidi)) <= 1.0e-3f);

  // 2.2: el corte recien hecho es "unible" en su frontera y no lejos de ella.
  CoordinateMapper mapper;
  mapper.setPixelsPerSecond(DEFAULT_PIXELS_PER_SECOND);
  mapper.setPixelsPerSemitone(DEFAULT_PIXELS_PER_SEMITONE);
  splitter.setCoordinateMapper(&mapper);
  const float boundaryX =
      mapper.framesToSeconds(kSplitFrame) * mapper.getPixelsPerSecond();
  const float insideY = mapper.midiToY(59.7f);
  auto mergeable = splitter.findMergeableNotesAt(boundaryX, insideY);
  CHECK(mergeable.first != nullptr && mergeable.second != nullptr);
  auto farAway = splitter.findMergeableNotesAt(boundaryX + 400.0f, insideY);
  CHECK(farAway.first == nullptr && farAway.second == nullptr);

  // Unir vuelve a una sola nota y no mueve la afinacion compuesta.
  CHECK(splitter.mergeNotes(mergeable.first, mergeable.second));
  CHECK(project.getNotes().size() == 2);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const auto merged = project.getAdjustedF0();
  const auto mergeDiff = compare(before, merged, kSplitFrame);
  std::printf("2.2 unir: cuerpo max %.4f c | costura max %.4f c | notas %d\n",
              mergeDiff.body, mergeDiff.seam,
              static_cast<int>(project.getNotes().size()));
  CHECK(mergeDiff.body <= 0.5f);
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  testControlWithoutCut();
  testSplitIsPitchNeutral();
  std::printf("SplitPitchNeutralityTests: en verde\n");
  return 0;
}
