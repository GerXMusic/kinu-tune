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
#include "../Source/Models/ProjectSerializer.h"
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
  audio.melSpectrogram.assign(kFrames, std::vector<float>(1, 0.0f));
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
  float meanCents = 0.0f;       // getAdjustedF0 (final composed curve)
  float finalMeanCents = 0.0f;  // audioData.f0 (the curve synthesis consumes)
  float errorCents = 0.0f;
  Metrics metrics;
};

float plateauMeanCents(const std::vector<float> &f0, int start, int end) {
  const int first = start + kPlateauMargin;
  const int last = end - kPlateauMargin;
  double sum = 0.0;
  int count = 0;
  for (int i = first; i < last; ++i) {
    if (i < 0 || i >= static_cast<int>(f0.size()) ||
        f0[static_cast<size_t>(i)] <= 0.0f)
      continue;
    sum += cents(f0[static_cast<size_t>(i)]);
    ++count;
  }
  return count > 0 ? static_cast<float>(sum / count) : 0.0f;
}

IntervalRun runInterval(float factor) {
  Project project = makeProject(false);
  for (auto &note : project.getNotes())
    note.setVibrato(factor);
  auto &notes = project.getNotes();
  notes[1].setIntervalCents(200.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(project);

  IntervalRun run;
  run.vibrato = factor;
  run.meanCents = plateauMeanCents(project.getAdjustedF0(),
                                   notes[1].getStartFrame(),
                                   notes[1].getEndFrame());
  run.finalMeanCents = plateauMeanCents(project.getAudioData().f0,
                                        notes[1].getStartFrame(),
                                        notes[1].getEndFrame());
  run.errorCents = std::fabs(run.meanCents - cents(midiToFreq(62.0f)));
  run.metrics = measureJoin(project);
  return run;
}

void printRun(const char *name, const IntervalRun &run) {
  std::cout << name << ": mean=" << run.meanCents << " c, final="
            << run.finalMeanCents << " c, error=" << run.errorCents
            << " c, jump=" << run.metrics.jump << " c, slope="
            << run.metrics.slope << " c/sample\n";
}

// Roundtrip: save with intervalCents, reload, rebuild, and compare the final
// f0 against the original run. Criterion fixed before measuring: the interval
// must come back as 200 c; plateau error <= 0.5 c; max |f0 difference| <= 0.01 c.
struct RoundTrip {
  bool loaded = false;
  bool intervalKept = false;
  float errorCents = 0.0f;
  float maxF0DiffCents = 0.0f;
};

RoundTrip roundTripInterval(float factor) {
  Project project = makeProject(false);
  for (auto &note : project.getNotes())
    note.setVibrato(factor);
  project.getNotes()[1].setIntervalCents(200.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const auto reference = project.getAdjustedF0();

  auto json = ProjectSerializer::toJson(project, true, true);
  Project loaded;
  RoundTrip result;
  result.loaded = ProjectSerializer::fromJson(loaded, json);
  if (!result.loaded)
    return result;

  const auto &loadedNotes = loaded.getNotes();
  if (loadedNotes.size() < 2 ||
      !loadedNotes[1].getIntervalCents().has_value())
    return result;
  result.intervalKept =
      std::fabs(*loadedNotes[1].getIntervalCents() - 200.0f) <= 1.0e-3f;

  PitchCurveProcessor::rebuildBaseFromNotes(loaded);
  const auto rebuilt = loaded.getAdjustedF0();
  result.errorCents =
      std::fabs(plateauMeanCents(rebuilt, loadedNotes[1].getStartFrame(),
                                 loadedNotes[1].getEndFrame()) -
                cents(midiToFreq(62.0f)));
  for (size_t i = 0; i < reference.size() && i < rebuilt.size(); ++i)
    result.maxF0DiffCents =
        std::max(result.maxF0DiffCents,
                 std::fabs(cents(rebuilt[i]) - cents(reference[i])));
  return result;
}

void printRoundTrip(const char *name, const RoundTrip &rt) {
  std::cout << name << ": loaded=" << rt.loaded << ", intervalKept="
            << rt.intervalKept << ", error=" << rt.errorCents
            << " c, maxF0Diff=" << rt.maxF0DiffCents << " c\n";
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

  const RoundTrip rtZero = roundTripInterval(0.0f);
  const RoundTrip rtHalf = roundTripInterval(0.5f);
  const RoundTrip rtFull = roundTripInterval(1.0f);
  printRoundTrip("roundtrip 0%", rtZero);
  printRoundTrip("roundtrip 50%", rtHalf);
  printRoundTrip("roundtrip 100%", rtFull);

  // (a) plateau exactly at B + interval for every vibrato setting, on the
  // final composed curve (getAdjustedF0) and on audioData.f0 (synthesis).
  CHECK(zero.errorCents <= kMeanLimitCents);
  CHECK(half.errorCents <= kMeanLimitCents);
  CHECK(full.errorCents <= kMeanLimitCents);
  CHECK(std::fabs(zero.finalMeanCents - cents(midiToFreq(62.0f))) <=
        kMeanLimitCents);
  CHECK(std::fabs(half.finalMeanCents - cents(midiToFreq(62.0f))) <=
        kMeanLimitCents);
  CHECK(std::fabs(full.finalMeanCents - cents(midiToFreq(62.0f))) <=
        kMeanLimitCents);

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

  // Roundtrip: field survives and reproduces the same final result.
  CHECK(rtZero.loaded && rtZero.intervalKept);
  CHECK(rtHalf.loaded && rtHalf.intervalKept);
  CHECK(rtFull.loaded && rtFull.intervalKept);
  CHECK(rtZero.errorCents <= kMeanLimitCents);
  CHECK(rtHalf.errorCents <= kMeanLimitCents);
  CHECK(rtFull.errorCents <= kMeanLimitCents);
  CHECK(rtZero.maxF0DiffCents <= 0.01f);
  CHECK(rtHalf.maxF0DiffCents <= 0.01f);
  CHECK(rtFull.maxF0DiffCents <= 0.01f);

  std::cout << "D206IntervalTests: all checks passed\n";
  return 0;
}
