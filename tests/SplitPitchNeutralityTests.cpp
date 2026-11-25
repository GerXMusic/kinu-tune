// D-285 (2.3) / D-303: cortar no debe afinar.
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
//
// D-303 anade dos medidas mas (el audio sigue igual, lo visible se recentra):
//   - mitad.getMidiNote() == media local de su tramo (0,02 st)
//   - mitad.getMidiNote() + mitad.getRenderCenterOffset() == centro original
//   - al unir: merged.getMidiNote() == media de la union (0,02 st)
//   - renderCenterOffset sobrevive a toJson/fromJson
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
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

// D-303: la media local que usa el corte, calculada igual que el splitter
// (solo tramas sonoras de la curva compuesta).
float meanMidi(const std::vector<float> &f0, int start, int end) {
  double sum = 0.0;
  int count = 0;
  for (int frame = std::max(0, start);
       frame < std::min(end, static_cast<int>(f0.size())); ++frame) {
    const float hz = f0[static_cast<size_t>(frame)];
    if (hz > 0.0f && std::isfinite(hz)) {
      sum += freqToMidi(hz);
      ++count;
    }
  }
  return count > 0 ? static_cast<float>(sum / count) : 0.0f;
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

  // D-285/2: el instrumento que dice si la curva quedo desfasada de las notas.
  // Consistente: ~0. Si una nota se mueve sin reconstruir la base: ~el salto.
  CHECK(project.maxCurveNotesDisagreement() <= 0.1f);
  auto &notes = project.getNotes();
  for (auto &note : notes)
    if (!note.isRest())
      note.setMidiNote(note.getMidiNote() + 7.0f);
  const float desynced = project.maxCurveNotesDisagreement();
  std::printf("2.3 desfase a proposito: %.3f st (esperado ~7)\n", desynced);
  CHECK(desynced > 6.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const float repaired = project.maxCurveNotesDisagreement();
  std::printf("2.3 tras reconstruir la base: %.4f st (esperado < 0,1)\n", repaired);
  // La base se suaviza con un coseno: al centro de una nota quedan ~5 cents,
  // muy por debajo del 0,5 st con el que se distingue un desfase de verdad.
  CHECK(repaired <= 0.1f);
}

void testSplitIsPitchNeutral() {
  Project project = makeProject();
  const auto before = project.getAdjustedF0();
  // D-303: la media local de cada mitad, medida sobre la MISMA curva que lee
  // el splitter antes de tocar la nota.
  const float expectLeftMean =
      meanMidi(project.getAudioData().f0, 0, kSplitFrame);
  const float expectRightMean =
      meanMidi(project.getAudioData().f0, kSplitFrame, kNoteA);

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
      "2.3 corte: nota %.3f -> mitades visibles %.3f / %.3f (D-303) | "
      "fuente %.4f conservada\n",
      firstMidi, left.getMidiNote(), right.getMidiNote(), firstSource);
  std::printf("2.3 corte: afinacion compuesta cuerpo max %.4f c (lim 0,5) en "
              "trama %d | costura max %.4f c en trama %d\n",
              diff.body, diff.bodyWorstFrame, diff.seam, diff.seamWorstFrame);
  CHECK(diff.body <= 0.5f);
  juce::ignoreUnused(worstHalfSemitones);
  // D-303: cada mitad se recentra en la media local de la curva blanca...
  CHECK(std::abs(left.getMidiNote() - expectLeftMean) <= 0.02f);
  CHECK(std::abs(right.getMidiNote() - expectRightMean) <= 0.02f);
  // ...y conserva el centro de AUDIO de la nota original, que es el que
  // construye la base: por eso el corte no mueve la afinacion.
  CHECK(std::abs(left.getMidiNote() + left.getRenderCenterOffset() - firstMidi) <=
        1.0e-3f);
  CHECK(std::abs(right.getMidiNote() + right.getRenderCenterOffset() - firstMidi) <=
        1.0e-3f);
  std::printf("2.3 D-303 mitades visibles %.3f / %.3f | audio %.3f / %.3f "
              "(original %.3f)\n",
              left.getMidiNote(), right.getMidiNote(),
              left.getMidiNote() + left.getRenderCenterOffset(),
              right.getMidiNote() + right.getRenderCenterOffset(), firstMidi);
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
  const float expectUnionMean =
      meanMidi(project.getAudioData().f0, firstStart, firstEnd);
  CHECK(splitter.mergeNotes(mergeable.first, mergeable.second));
  CHECK(project.getNotes().size() == 2);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const auto merged = project.getAdjustedF0();
  const auto mergeDiff = compare(before, merged, kSplitFrame);
  std::printf("2.2 unir: cuerpo max %.4f c | costura max %.4f c | notas %d\n",
              mergeDiff.body, mergeDiff.seam,
              static_cast<int>(project.getNotes().size()));
  CHECK(mergeDiff.body <= 0.5f);
  // D-303: la nota unida muestra la media local de la union y conserva el
  // centro de audio que tenian las dos mitades.
  const Note &mergedNote = project.getNotes().front();
  CHECK(std::abs(mergedNote.getMidiNote() - expectUnionMean) <= 0.02f);
  CHECK(std::abs(mergedNote.getMidiNote() + mergedNote.getRenderCenterOffset() -
                 firstMidi) <= 1.0e-3f);
  std::printf("2.2 D-303 union: visible %.3f (media %.3f) | audio %.3f\n",
              mergedNote.getMidiNote(), expectUnionMean,
              mergedNote.getMidiNote() + mergedNote.getRenderCenterOffset());

  // D-303: el centro de render sobrevive a guardar y cargar.
  {
    const juce::var json = ProjectSerializer::toJson(project);
    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    CHECK(loaded.getNotes().size() == project.getNotes().size());
    for (size_t i = 0; i < loaded.getNotes().size() &&
                       i < project.getNotes().size();
         ++i) {
      CHECK(std::abs(loaded.getNotes()[i].getMidiNote() -
                     project.getNotes()[i].getMidiNote()) <= 1.0e-3f);
      CHECK(std::abs(loaded.getNotes()[i].getRenderCenterOffset() -
                     project.getNotes()[i].getRenderCenterOffset()) <= 1.0e-4f);
    }
    std::printf("2.2 D-303 json: renderCenterOffset cargado %.4f (guardado "
                "%.4f)\n",
                loaded.getNotes().front().getRenderCenterOffset(),
                project.getNotes().front().getRenderCenterOffset());
  }
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  testControlWithoutCut();
  testSplitIsPitchNeutral();
  std::printf("SplitPitchNeutralityTests: en verde\n");
  return 0;
}
