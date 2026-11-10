// D-205 (continuidad de la curva f0 en la union de notas contiguas).
//
// Dos notas contiguas con intervalo distinto de 0. Se mide la curva f0 final
// (base + delta, la que consumen los motores) en la union:
//   (a) salto discontinuo <= 0,5 c: residuo local
//       |f0[b] - (f0[b-1] + f0[b+1]) / 2|, que aisla el escalon de la rampa
//       suave de la base (una rampa tiene residuo ~0 en su punto de inflexion);
//   (b) discontinuidad de pendiente <= 1 c por muestra de control:
//       |(f0[b+1]-f0[b]) - (f0[b]-f0[b-1])| = 2 * residuo de (a).
// Umbrales fijados antes del arreglo; no se ajustan despues. Se imprime
// tambien el salto bruto f0[b]-f0[b-1], que incluye la pendiente legitima de la
// base y por eso no sirve como criterio de discontinuidad.
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

constexpr int kNoteFrames = 100;
constexpr int kBoundary = kNoteFrames;
constexpr int kFrames = 2 * kNoteFrames;
constexpr float kJumpLimitCents = 0.5f;
constexpr float kSlopeLimitCentsPerSample = 1.0f;

float cents(float hz) {
  return 1200.0f * std::log2(std::max(hz, 1.0e-6f) / 440.0f);
}

// The per-note stored delta is the editable curve relative to the base. A
// constant value per note with different values on each side is the case the
// hypothesis describes: the delta does not meet at the union.
Project makeProject(float detectedLeft, float detectedRight, bool continuousDelta) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  audio.melSpectrogram.resize(kFrames);
  audio.denseF0.assign(kFrames, 0.0f);

  Note left(0, kBoundary, detectedLeft);
  left.setOriginalMidiNote(detectedLeft);
  Note right(kBoundary, kFrames, detectedRight);
  right.setOriginalMidiNote(detectedRight);

  std::vector<float> leftDelta(static_cast<size_t>(kBoundary));
  std::vector<float> rightDelta(static_cast<size_t>(kFrames - kBoundary));
  for (int i = 0; i < kBoundary; ++i) {
    // 100 c step at the union, or a contour that crosses it smoothly.
    leftDelta[static_cast<size_t>(i)] =
        continuousDelta ? 0.10f * std::sin(0.05f * static_cast<float>(i))
                        : -0.50f;
    rightDelta[static_cast<size_t>(i)] =
        continuousDelta ? 0.10f * std::sin(0.05f * static_cast<float>(kBoundary + i))
                        : +0.50f;
    audio.denseF0[static_cast<size_t>(i)] =
        midiToFreq(detectedLeft + leftDelta[static_cast<size_t>(i)]);
    audio.denseF0[static_cast<size_t>(kBoundary + i)] =
        midiToFreq(detectedRight + rightDelta[static_cast<size_t>(i)]);
  }
  left.setOriginalDeltaPitch(leftDelta);
  right.setOriginalDeltaPitch(rightDelta);
  project.addNote(left);
  project.addNote(right);
  // Establish base/delta as detection does, before any edit or preview.
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

struct Metrics {
  float rawJump = 0.0f; // informational: includes the base ramp
  float jump = 0.0f;    // (a) local discontinuity residual
  float slope = 0.0f;   // (b) slope discontinuity
  float baseJump = 0.0f;
  float deltaJump = 0.0f;
};

Metrics measureCurve(const Project &project) {
  const auto f0 = project.getAdjustedF0();
  const auto &audio = project.getAudioData();
  Metrics m;
  const float c0 = cents(f0[static_cast<size_t>(kBoundary - 1)]);
  const float c1 = cents(f0[static_cast<size_t>(kBoundary)]);
  const float c2 = cents(f0[static_cast<size_t>(kBoundary + 1)]);
  m.rawJump = std::fabs(c1 - c0);
  m.jump = std::fabs(c1 - 0.5f * (c0 + c2));
  m.slope = std::fabs((c2 - c1) - (c1 - c0));
  m.baseJump =
      std::fabs(audio.basePitch[static_cast<size_t>(kBoundary)] -
                audio.basePitch[static_cast<size_t>(kBoundary - 1)]) *
      100.0f;
  m.deltaJump =
      std::fabs(audio.deltaPitch[static_cast<size_t>(kBoundary)] -
                audio.deltaPitch[static_cast<size_t>(kBoundary - 1)]) *
      100.0f;
  return m;
}

void print(const char *name, const Metrics &m) {
  std::cout << name << ": jump=" << m.jump << " c, slope=" << m.slope
            << " c/sample (raw=" << m.rawJump << " c, baseJump=" << m.baseJump
            << " c, deltaJump=" << m.deltaJump << " c)\n";
}

Metrics flatten100(float detectedLeft, float detectedRight) {
  Project project = makeProject(detectedLeft, detectedRight, false);
  for (auto &note : project.getNotes())
    note.setVibrato(0.0f); // Straighten Pitch Curve / Flatten 100 %.
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return measureCurve(project);
}

Metrics correct100(float detectedLeft, float detectedRight) {
  Project project = makeProject(detectedLeft, detectedRight, false);
  // Correct Pitch 100 %: each note goes to its nearest semitone (60 and 62).
  auto &notes = project.getNotes();
  notes[0].setMidiNoteFromPitchCorrection(60.0f);
  notes[1].setMidiNoteFromPitchCorrection(62.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return measureCurve(project);
}

// Correct Pitch 100 % on the preview path (rebuildDeltaForNotes), the path the
// slider and the handle use while dragging.
Metrics correct100Preview(float detectedLeft, float detectedRight) {
  Project project = makeProject(detectedLeft, detectedRight, false);
  auto &notes = project.getNotes();
  notes[0].setMidiNoteFromPitchCorrection(60.0f);
  notes[1].setMidiNoteFromPitchCorrection(62.0f);
  std::vector<Note *> affected{&notes[0], &notes[1]};
  PitchCurveProcessor::rebuildDeltaForNotes(project, affected);
  return measureCurve(project);
}

// 0 % / 50 % straighten on a continuous delta: no join step, so the smoother
// must not change the curve. maxDiff compares the composed f0 with
// base + factor * stored delta, the curve before this change.
struct Untouched {
  float maxDiff = 0.0f;
  Metrics metrics;
};

Untouched untouched(float factor) {
  Project project = makeProject(60.4f, 61.6f, true);
  for (auto &note : project.getNotes())
    note.setVibrato(factor);
  PitchCurveProcessor::rebuildBaseFromNotes(project);

  const auto f0 = project.getAdjustedF0();
  const auto &audio = project.getAudioData();
  const auto &notes = project.getNotes();
  Untouched u;
  for (int n = 0; n < 2; ++n) {
    const int start = notes[static_cast<size_t>(n)].getStartFrame();
    const int end = notes[static_cast<size_t>(n)].getEndFrame();
    const auto &original = notes[static_cast<size_t>(n)].getOriginalDeltaPitch();
    for (int i = start; i < end; ++i) {
      const int local = i - start;
      if (local < 0 || local >= static_cast<int>(original.size()))
        continue;
      const float expected = cents(
          midiToFreq(audio.basePitch[static_cast<size_t>(i)] +
                     factor * original[static_cast<size_t>(local)]));
      const float actual = cents(f0[static_cast<size_t>(i)]);
      u.maxDiff = std::max(u.maxDiff, std::fabs(actual - expected));
    }
  }
  u.metrics = measureCurve(project);
  return u;
}

} // namespace

int main() {
  const Metrics flat = flatten100(60.0f, 62.0f);
  print("flatten 100%", flat);

  // Correct Pitch 100 %: sung one semitone apart (60,4 and 61,6), corrected to
  // 60 and 62 -> a 200 c interval at the union. The stored delta steps 100 c.
  const Metrics corrected = correct100(60.4f, 61.6f);
  print("correct 100% commit ", corrected);
  const Metrics correctedPreview = correct100Preview(60.4f, 61.6f);
  print("correct 100% preview", correctedPreview);

  const Untouched zero = untouched(1.0f);
  print("straighten 0%", zero.metrics);
  std::cout << "straighten 0%: maxDiff=" << zero.maxDiff << " c\n";
  const Untouched half = untouched(0.5f);
  print("straighten 50%", half.metrics);
  std::cout << "straighten 50%: maxDiff=" << half.maxDiff << " c\n";

  // (a) and (b) on every 100 % case.
  CHECK(flat.jump <= kJumpLimitCents);
  CHECK(flat.slope <= kSlopeLimitCentsPerSample);
  CHECK(corrected.jump <= kJumpLimitCents);
  CHECK(corrected.slope <= kSlopeLimitCentsPerSample);
  CHECK(correctedPreview.jump <= kJumpLimitCents);
  CHECK(correctedPreview.slope <= kSlopeLimitCentsPerSample);

  // 0 % / 50 %: unchanged (<= 0.5 c) and still continuous.
  CHECK(zero.maxDiff <= 0.5f);
  CHECK(zero.metrics.jump <= kJumpLimitCents);
  CHECK(zero.metrics.slope <= kSlopeLimitCentsPerSample);
  CHECK(half.maxDiff <= 0.5f);
  CHECK(half.metrics.jump <= kJumpLimitCents);
  CHECK(half.metrics.slope <= kSlopeLimitCentsPerSample);

  std::cout << "D205StraightenContinuityTests: all checks passed\n";
  return 0;
}
