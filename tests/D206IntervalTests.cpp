// D-206 (intervalo por nota respecto a la vecina izquierda).
//
// Dos notas contiguas: la izquierda tiene base corregida B; la derecha lleva
// intervalCents = +200 c. El criterio se fija aqui, antes de medir:
//   (a) |media(f0 de la meseta) - (B + 200 c)| <= 0,5 c para el tirador
//       "Straighten Pitch Curve" al 0 %, 50 % y 100 %;
//   (b) continuidad en la union: salto residual <= 0,5 c y discontinuidad de
//       pendiente <= 1 c/muestra (el mismo criterio de D-205);
//   (c) sin intervalo, la curva f0 es exactamente la de antes (bit a bit).
//
// La meseta excluye 30 frames en cada borde de la nota, fuera de la ventana
// coseno de 80 ms de la base (~7 frames).
//
// El glide D-182 sigue apagado (D-189) y no se activa aqui.

#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kNoteFrames = 200;
constexpr int kBoundary = kNoteFrames;
constexpr int kFrames = 2 * kNoteFrames;
constexpr int kPlateauMargin = 30;
constexpr float kMeanLimitCents = 0.5f;
constexpr float kJumpLimitCents = 0.5f;
constexpr float kSlopeLimitCentsPerSample = 1.0f;

float cents(float hz) {
  return 1200.0f * std::log2(std::max(hz, 1.0e-6f) / 440.0f);
}

float sineDelta(int globalFrame) {
  // Exactly two periods over each 200-frame note, so the note mean is 0.
  return 0.20f * std::sin(2.0f * static_cast<float>(M_PI) *
                          static_cast<float>(globalFrame % kNoteFrames) /
                          static_cast<float>(kNoteFrames));
}

Project makeProject(bool continuousDelta) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  audio.melSpectrogram.resize(kFrames);
  audio.denseF0.assign(kFrames, 0.0f);

  const float leftBase = 60.0f;
  const float rightBase = 61.0f; // B + 200 c = 62.0, so the interval moves it.

  Note left(0, kBoundary, leftBase);
  left.setOriginalMidiNote(leftBase);
  Note right(kBoundary, kFrames, rightBase);
  right.setOriginalMidiNote(rightBase);

  std::vector<float> leftDelta(static_cast<size_t>(kBoundary));
  std::vector<float> rightDelta(static_cast<size_t>(kFrames - kBoundary));
  for (int i = 0; i < kBoundary; ++i) {
    leftDelta[static_cast<size_t>(i)] =
        continuousDelta ? sineDelta(i) : -0.50f;
    rightDelta[static_cast<size_t>(i)] =
        continuousDelta ? sineDelta(kBoundary + i) : +0.50f;
    audio.denseF0[static_cast<size_t>(i)] =
        midiToFreq(leftBase + leftDelta[static_cast<size_t>(i)]);
    audio.denseF0[static_cast<size_t>(kBoundary + i)] =
        midiToFreq(rightBase + rightDelta[static_cast<size_t>(i)]);
  }
  left.setOriginalDeltaPitch(leftDelta);
  right.setOriginalDeltaPitch(rightDelta);
  project.addNote(left);
  project.addNote(right);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

struct Metrics {
  float jump = 0.0f;
  float slope = 0.0f;
};

Metrics measureJoin(const Project &project) {
  const auto f0 = project.getAdjustedF0();
  Metrics m;
  const float c0 = cents(f0[static_cast<size_t>(kBoundary - 1)]);
  const float c1 = cents(f0[static_cast<size_t>(kBoundary)]);
  const float c2 = cents(f0[static_cast<size_t>(kBoundary + 1)]);
  m.jump = std::fabs(c1 - 0.5f * (c0 + c2));
  m.slope = std::fabs((c2 - c1) - (c1 - c0));
  return m;
}

float plateauMeanCents(const Project &project, int start, int end) {
  const auto f0 = project.getAdjustedF0();
  const int first = start + kPlateauMargin;
  const int last = end - kPlateauMargin;
  double sum = 0.0;
  int count = 0;
  for (int i = first; i < last; ++i) {
    if (f0[static_cast<size_t>(i)] <= 0.0f)
      continue;
    sum += cents(f0[static_cast<size_t>(i)]);
    ++count;
  }
  return count > 0 ? static_cast<float>(sum / count) : 0.0f;
}

struct IntervalRun {
  float vibrato = 0.0f;
  float meanCents = 0.0f;
  float errorCents = 0.0f;
  Metrics metrics;
};

IntervalRun runInterval(float factor) {
  Project project = makeProject(false);
  for (auto &note : project.getNotes())
    note.setVibrato(factor);
  auto &notes = project.getNotes();
  notes[1].setIntervalCents(200.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(project);

  IntervalRun run;
  run.vibrato = factor;
  run.meanCents = plateauMeanCents(project, notes[1].getStartFrame(),
                                   notes[1].getEndFrame());
  run.errorCents = std::fabs(run.meanCents - cents(midiToFreq(62.0f)));
  run.metrics = measureJoin(project);
  return run;
}

void printRun(const char *name, const IntervalRun &run) {
  std::cout << name << ": mean=" << run.meanCents << " c, error="
            << run.errorCents << " c, jump=" << run.metrics.jump
            << " c, slope=" << run.metrics.slope << " c/sample\n";
}

// (c) No interval: composed f0 must be exactly base + factor * original delta.
float noIntervalMaxDiff(float factor) {
  Project project = makeProject(true); // continuous delta: D-205 does nothing
  for (auto &note : project.getNotes())
    note.setVibrato(factor);
  PitchCurveProcessor::rebuildBaseFromNotes(project);

  const auto f0 = project.getAdjustedF0();
  const auto &audio = project.getAudioData();
  const auto &notes = project.getNotes();
  float maxDiff = 0.0f;
  for (int n = 0; n < 2; ++n) {
    const int start = notes[static_cast<size_t>(n)].getStartFrame();
    const int end = notes[static_cast<size_t>(n)].getEndFrame();
    const auto &original =
        notes[static_cast<size_t>(n)].getOriginalDeltaPitch();
    for (int i = start; i < end; ++i) {
      const int local = i - start;
      if (local < 0 || local >= static_cast<int>(original.size()))
        continue;
      const float expected =
          midiToFreq(audio.basePitch[static_cast<size_t>(i)] +
                     factor * original[static_cast<size_t>(local)]);
      maxDiff = std::max(maxDiff,
                         std::fabs(f0[static_cast<size_t>(i)] - expected));
    }
  }
  return maxDiff;
}

} // namespace

int main() {
  const IntervalRun zero = runInterval(0.0f);
  const IntervalRun half = runInterval(0.5f);
  const IntervalRun full = runInterval(1.0f);
  printRun("interval +200 c, straighten 0%", zero);
  printRun("interval +200 c, straighten 50%", half);
  printRun("interval +200 c, straighten 100%", full);

  const float diffOne = noIntervalMaxDiff(1.0f);
  const float diffHalf = noIntervalMaxDiff(0.5f);
  std::cout << "no interval: maxDiff(100%)=" << diffOne
            << ", maxDiff(50%)=" << diffHalf << "\n";

  // (a) plateau exactly at B + interval for every vibrato setting.
  CHECK(zero.errorCents <= kMeanLimitCents);
  CHECK(half.errorCents <= kMeanLimitCents);
  CHECK(full.errorCents <= kMeanLimitCents);

  // (b) union continuity, same criteria as D-205.
  CHECK(zero.metrics.jump <= kJumpLimitCents);
  CHECK(zero.metrics.slope <= kSlopeLimitCentsPerSample);
  CHECK(half.metrics.jump <= kJumpLimitCents);
  CHECK(half.metrics.slope <= kSlopeLimitCentsPerSample);
  CHECK(full.metrics.jump <= kJumpLimitCents);
  CHECK(full.metrics.slope <= kSlopeLimitCentsPerSample);

  // (c) absent interval: bit-identical to the pre-D-206 composition.
  CHECK(diffOne == 0.0f);
  CHECK(diffHalf == 0.0f);

  std::cout << "D206IntervalTests: all checks passed\n";
  return 0;
}
