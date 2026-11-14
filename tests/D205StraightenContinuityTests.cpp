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
#include <limits>
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

// D-221/D-223: one user control (`Transition`, in ms). The arriving note
// carries the length; the join smoother widens its window symmetrically around
// the boundary. The composed target must stay continuous and the correction
// must not reach past either note nor past the requested half-width.
struct SlideResult {
  Metrics metrics;
  int reachLeft = 0;   // frames with a visible correction, left of the union
  int reachRight = 0;  // frames with a visible correction, right of the union
};

SlideResult slidePreview(int slideMs) {
  Project project = makeProject(60.4f, 61.6f, false);
  auto &notes = project.getNotes();
  notes[0].setMidiNoteFromPitchCorrection(60.0f);
  notes[1].setMidiNoteFromPitchCorrection(62.0f);
  notes[0].setTransitionOutMs(slideMs);
  std::vector<Note *> affected{&notes[0], &notes[1]};
  PitchCurveProcessor::rebuildDeltaForNotes(project, affected);

  const auto f0 = project.getAdjustedF0();
  const auto &audio = project.getAudioData();
  SlideResult r;
  for (int i = kBoundary - 1; i >= 0; --i) {
    const float base = audio.basePitch[static_cast<size_t>(i)];
    const float corr = std::fabs(cents(f0[static_cast<size_t>(i)]) -
                                 cents(midiToFreq(base - 0.50f)));
    if (corr <= 5.0f) {
      r.reachLeft = kBoundary - 1 - i;
      break;
    }
  }
  for (int i = kBoundary; i < kFrames; ++i) {
    const float base = audio.basePitch[static_cast<size_t>(i)];
    const float corr = std::fabs(cents(f0[static_cast<size_t>(i)]) -
                                 cents(midiToFreq(base + 0.50f)));
    if (corr <= 5.0f) {
      r.reachRight = i - kBoundary;
      break;
    }
  }
  r.metrics = measureCurve(project);
  return r;
}

// D-223: the share of the ramp over the baseline is an engine constant
// (kTransitionBlendShare), so the check is the closed form frame by frame, in
// cents (base + delta is a semitone curve and cents are linear in it):
//   final = baseline + alpha * (ramp - baseline)
// with the ramp a smoothstep between the baseline values at the window edges.
// `continuousDelta` uses a contour that crosses the union smoothly, so the
// baseline has no audible step and only the ramp term moves.
struct MixResult {
  Metrics metrics;
  std::vector<float> cents;
};

MixResult mixPreview(int slideMs, bool continuousDelta = false) {
  Project project = makeProject(60.4f, 61.6f, continuousDelta);
  auto &notes = project.getNotes();
  notes[0].setMidiNoteFromPitchCorrection(60.0f);
  notes[1].setMidiNoteFromPitchCorrection(62.0f);
  notes[0].setTransitionOutMs(slideMs);
  std::vector<Note *> affected{&notes[0], &notes[1]};
  PitchCurveProcessor::rebuildDeltaForNotes(project, affected);

  MixResult r;
  const auto f0 = project.getAdjustedF0();
  r.cents.resize(f0.size());
  for (size_t i = 0; i < f0.size(); ++i)
    r.cents[i] = cents(f0[i]);
  r.metrics = measureCurve(project);
  return r;
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

  // D-221/D-223: a 200 ms transition is continuous and its correction never
  // reaches past half the requested length (100 ms ~= 9 frames at 512/44100)
  // nor past either note. T4: it scales with the request and stays bounded.
  constexpr int kHalf200Ms = 9;
  const SlideResult slide200 = slidePreview(200);
  print("slide 200 ms", slide200.metrics);
  std::cout << "slide 200 ms: reach=" << slide200.reachLeft << "/"
            << slide200.reachRight << " frames\n";
  CHECK(slide200.metrics.jump <= kJumpLimitCents);
  CHECK(slide200.metrics.slope <= kSlopeLimitCentsPerSample);
  CHECK(slide200.reachLeft <= kHalf200Ms + 1);
  CHECK(slide200.reachRight <= kHalf200Ms + 1);

  // A short transition must not reach further than a long one.
  const SlideResult slide60 = slidePreview(60);
  std::cout << "slide 60 ms: reach=" << slide60.reachLeft << "/"
            << slide60.reachRight << " frames\n";
  CHECK(slide60.metrics.jump <= kJumpLimitCents);
  CHECK(slide60.reachLeft <= slide200.reachLeft);
  CHECK(slide60.reachRight <= slide200.reachRight);

  // T1: Transition = 0 is the pre-D-221 curve and leaves no residue. Set a
  // long transition on the same project, rebuild, reset to 0 and rebuild: the
  // result must equal a fresh Transition = 0 project.
  const MixResult baseline = mixPreview(0);
  CHECK(baseline.metrics.jump <= kJumpLimitCents);
  CHECK(baseline.metrics.slope <= kSlopeLimitCentsPerSample);
  {
    Project p = makeProject(60.4f, 61.6f, false);
    auto &notes = p.getNotes();
    notes[0].setMidiNoteFromPitchCorrection(60.0f);
    notes[1].setMidiNoteFromPitchCorrection(62.0f);
    std::vector<Note *> affected{&notes[0], &notes[1]};
    notes[0].setTransitionOutMs(200);
    PitchCurveProcessor::rebuildDeltaForNotes(p, affected);
    notes[0].setTransitionOutMs(0);
    PitchCurveProcessor::rebuildDeltaForNotes(p, affected);
    const auto f0 = p.getAdjustedF0();
    float worstReset = 0.0f;
    for (size_t i = 0; i < f0.size(); ++i)
      worstReset = std::max(worstReset,
                            std::fabs(cents(f0[i]) - baseline.cents[i]));
    std::cout << "transition 0 after 200: deviation=" << worstReset << " c\n";
    CHECK(worstReset <= 0.001f);
  }

  // T2/T3: 200 ms keeps the D-205 continuity and matches the closed form.
  const MixResult mix200 = mixPreview(200);
  print("transition 200 ms", mix200.metrics);
  CHECK(mix200.metrics.jump <= kJumpLimitCents);
  CHECK(mix200.metrics.slope <= kSlopeLimitCentsPerSample);

  constexpr float kAlpha = PitchCurveProcessor::kTransitionBlendShare;
  const int first = kBoundary - kHalf200Ms;
  const int last = kBoundary + kHalf200Ms;
  const float fromCents = baseline.cents[static_cast<size_t>(first)];
  const float toCents = baseline.cents[static_cast<size_t>(last)];
  const double span = static_cast<double>(last - first);
  float worstFormula = 0.0f;
  for (int frame = first; frame <= last; ++frame)
  {
    const double t = static_cast<double>(frame - first) / span;
    const double s = t * t * (3.0 - 2.0 * t);
    const float ramp =
        fromCents + static_cast<float>(toCents - fromCents) * static_cast<float>(s);
    const float base = baseline.cents[static_cast<size_t>(frame)];
    const float expected = base + kAlpha * (ramp - base);
    worstFormula = std::max(
        worstFormula, std::fabs(mix200.cents[static_cast<size_t>(frame)] - expected));
  }
  std::cout << "transition 200 ms: closed-form deviation=" << worstFormula
            << " c\n";
  CHECK(worstFormula <= 0.001f);

  // Outside the window nothing moves: the baseline is untouched.
  float worstOutside = 0.0f;
  for (int frame = 0; frame < kFrames; ++frame)
    if (frame < first || frame > last)
      worstOutside = std::max(
          worstOutside, std::fabs(mix200.cents[static_cast<size_t>(frame)] -
                                  baseline.cents[static_cast<size_t>(frame)]));
  std::cout << "transition 200 ms: outside-window deviation=" << worstOutside
            << " c\n";
  CHECK(worstOutside <= 0.001f);

  // With a continuous contour the same closed form holds: the share is the
  // ramp's, so the natural contour keeps (1 - alpha) of itself.
  const MixResult contourBase = mixPreview(0, true);
  const MixResult contour200 = mixPreview(200, true);
  const float fromContour = contourBase.cents[static_cast<size_t>(first)];
  const float toContour = contourBase.cents[static_cast<size_t>(last)];
  float worstContour = 0.0f;
  for (int frame = first; frame <= last; ++frame)
  {
    const double t = static_cast<double>(frame - first) / span;
    const double s = t * t * (3.0 - 2.0 * t);
    const float ramp = fromContour + static_cast<float>(toContour - fromContour) *
                                         static_cast<float>(s);
    const float base = contourBase.cents[static_cast<size_t>(frame)];
    const float expected = base + kAlpha * (ramp - base);
    worstContour = std::max(
        worstContour, std::fabs(contour200.cents[static_cast<size_t>(frame)] - expected));
  }
  std::cout << "transition 200 ms (contour): closed-form deviation="
            << worstContour << " c\n";
  CHECK(worstContour <= 0.001f);
  CHECK(contour200.metrics.jump <= kJumpLimitCents);

  std::cout << "D205StraightenContinuityTests: all checks passed\n";
  return 0;
}
