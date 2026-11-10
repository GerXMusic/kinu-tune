// D-207 (per-frontier transition between two contiguous notes).
//
// Synthetic case fixed before implementing: two contiguous notes (gap = 0),
// left plateau 60, right plateau 62, stored deltas -50 / +50 c. W = 20 ms,
// C = -100 %. Criteria (a)-(f), also fixed before implementing:
//
// (a) the plateaus are untouched: max |f0 - baseline| outside the window
//     <= 0.5 c;
// (b) the join stays continuous: residual
//     |f0[b] - (f0[b-1] + f0[b+1]) / 2| <= 5 c and slope discontinuity
//     |(f0[b+1]-f0[b]) - (f0[b]-f0[b-1])| <= 10 c per frame. Looser than
//     D-205 (0.5 / 1): the Bezier is deliberately a different shape and at
//     W = 20 ms the window only holds three frames (hop 11.6 ms);
// (c) seam stitch: the first and last written frames stay within 5 c of the
//     original curve (the Bezier is anchored on the real curve at both edges);
// (d) the window follows the analytic cubic Bezier (asymmetric handles
//     0.35 / 0.25, tangents from the base) within 0.5 c, and it actually
//     changes the curve (>= 4 c somewhere inside);
// (e) C = 0 % and W = 0 are bit-identical to the D-205 result (maxDiff == 0);
// (f) W is clamped to 20 ms (200 ms gives the same curve), the note-duration
//     cap is honoured (a 2-frame note only touches the frames inside
//     min(dur)/2) and C = +100 changes the window without leaving it.
//
// The baseline is the same project rebuilt without any transition, i.e. the
// D-205 output. The window is [b*HOP - W_samples, b*HOP + W_samples].

#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr int kNoteFrames = 100;
constexpr int kB = kNoteFrames;
constexpr int kFrames = 2 * kNoteFrames;
constexpr int kHop = HOP_SIZE;
constexpr float kJumpLimitCents = 5.0f;
constexpr float kSlopeLimitCentsPerFrame = 10.0f;
constexpr float kSeamSlopeLimitCentsPerFrame = 5.0f;
constexpr float kPlateauLimitCents = 0.5f;
constexpr float kBezierLimitCents = 0.5f;
constexpr float kEffectiveMinCents = 4.0f;
constexpr float kCappedMinCents = 1.0f;
constexpr double kSampleRate = 44100.0;

float cents(float hz) {
  return 1200.0f * std::log2(std::max(hz, 1.0e-6f) / 440.0f);
}

Project makeProject(int leftFrames = kNoteFrames,
                    int rightFrames = kNoteFrames) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  const int total = leftFrames + rightFrames;
  audio.melSpectrogram.resize(static_cast<size_t>(total));
  audio.denseF0.assign(static_cast<size_t>(total), 0.0f);

  Note left(0, leftFrames, 60.0f);
  left.setOriginalMidiNote(60.0f);
  Note right(leftFrames, total, 62.0f);
  right.setOriginalMidiNote(62.0f);

  std::vector<float> leftDelta(static_cast<size_t>(leftFrames), -0.50f);
  std::vector<float> rightDelta(static_cast<size_t>(rightFrames), 0.50f);
  for (int i = 0; i < leftFrames; ++i)
    audio.denseF0[static_cast<size_t>(i)] =
        midiToFreq(60.0f + leftDelta[static_cast<size_t>(i)]);
  for (int i = 0; i < rightFrames; ++i)
    audio.denseF0[static_cast<size_t>(leftFrames + i)] =
        midiToFreq(62.0f + rightDelta[static_cast<size_t>(i)]);

  left.setOriginalDeltaPitch(leftDelta);
  right.setOriginalDeltaPitch(rightDelta);
  project.addNote(left);
  project.addNote(right);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

float sampleLinear(const std::vector<float> &values, double framePos,
                   int totalFrames) {
  if (values.empty() || totalFrames <= 0)
    return 0.0f;
  const double clamped =
      std::clamp(framePos, 0.0, static_cast<double>(totalFrames - 1));
  const int i0 = static_cast<int>(std::floor(clamped));
  const int i1 = std::min(i0 + 1, totalFrames - 1);
  const double f = clamped - i0;
  return static_cast<float>(values[static_cast<size_t>(i0)] * (1.0 - f) +
                            values[static_cast<size_t>(i1)] * f);
}

struct Window {
  int boundary = 0;
  int sFirst = 0;
  int sLast = 0;
  double s0 = 0.0;
  double s1 = 0.0;
  double y0 = 0.0;
  double y3 = 0.0;
  double m0 = 0.0;
  double m3 = 0.0;
};

// Mirror of the implementation's geometry, computed from the baseline (the
// D-205 curve) so the test can check the Hermite value independently.
Window makeWindow(const Project &baseline) {
  const auto &audio = baseline.getAudioData();
  const int total = static_cast<int>(audio.basePitch.size());
  const auto &notes = baseline.getNotes();
  Window w;
  w.boundary = notes[0].getEndFrame();
  const double hop = static_cast<double>(kHop);
  const double capSamples =
      0.5 * std::min(notes[0].getDurationFrames(),
                     notes[1].getDurationFrames()) *
      hop;
  const double wSamples =
      std::min(20.0 * kSampleRate / 1000.0, capSamples);
  w.s0 = std::max(0.0, w.boundary * hop - wSamples);
  w.s1 = std::min(static_cast<double>((total - 1) * kHop),
                  w.boundary * hop + wSamples);
  w.sFirst = static_cast<int>(std::ceil(w.s0));
  w.sLast = static_cast<int>(std::floor(w.s1));

  auto composedAt = [&](double s) {
    const double fp = s / hop;
    return static_cast<double>(sampleLinear(audio.basePitch, fp, total) +
                               sampleLinear(audio.deltaPitch, fp, total));
  };
  auto baseAt = [&](double s) {
    return static_cast<double>(
        sampleLinear(audio.basePitch, s / hop, total));
  };
  w.y0 = composedAt(w.s0);
  w.y3 = composedAt(w.s1);
  w.m0 = baseAt(w.s0 + 1.0) - baseAt(w.s0);
  w.m3 = baseAt(w.s1) - baseAt(w.s1 - 1.0);
  return w;
}

double hermiteAt(const Window &w, double s) {
  const double L = w.s1 - w.s0;
  if (L <= 0.0)
    return w.y0;
  const double x0 = w.s0;
  const double x3 = w.s1;
  const double x1 = w.s0 + 0.35 * L;
  const double x2 = w.s1 - 0.25 * L;
  const double y0 = w.y0;
  const double y1 = w.y0 + w.m0 * 0.35 * L;
  const double y2 = w.y3 - w.m3 * 0.25 * L;
  const double y3 = w.y3;
  auto xOf = [&](double t) {
    const double u = 1.0 - t;
    return u * u * u * x0 + 3.0 * u * u * t * x1 + 3.0 * u * t * t * x2 +
           t * t * t * x3;
  };
  auto yOf = [&](double t) {
    const double u = 1.0 - t;
    return u * u * u * y0 + 3.0 * u * u * t * y1 + 3.0 * u * t * t * y2 +
           t * t * t * y3;
  };
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 40; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (xOf(mid) < s)
      lo = mid;
    else
      hi = mid;
  }
  return yOf(0.5 * (lo + hi));
}

std::vector<float> f0Of(const Project &project) {
  return project.getAdjustedF0();
}

float maxDiff(const std::vector<float> &a, const std::vector<float> &b,
              int first, int last) {
  float diff = 0.0f;
  for (int i = first; i <= last; ++i)
    diff = std::max(diff, std::fabs(cents(a[static_cast<size_t>(i)]) -
                                    cents(b[static_cast<size_t>(i)])));
  return diff;
}

float diffAt(const std::vector<float> &a, const std::vector<float> &b,
             int frame) {
  return std::fabs(cents(a[static_cast<size_t>(frame)]) -
                   cents(b[static_cast<size_t>(frame)]));
}

} // namespace

int main() {
  std::cout << "D207BoundaryTransitionTests\n";

  const Project baseline = makeProject();
  const auto base = f0Of(baseline);
  const Window win = makeWindow(baseline);

  // C = -100 %, W = 20 ms.
  Project bezier = makeProject();
  bezier.getBoundaryTransitions().set(0, kB, 20.0f, -100.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(bezier);
  const auto curve = f0Of(bezier);

  // Derived frame ranges.
  const int fFirst = static_cast<int>(std::ceil(win.s0 / kHop));
  const int fLast = static_cast<int>(std::floor(win.s1 / kHop));
  std::cout << "window samples [" << win.s0 << ", " << win.s1 << "] frames ["
            << fFirst << ", " << fLast << "] y0=" << win.y0
            << " y3=" << win.y3 << "\n";

  // (a) plateaus untouched.
  const float plateau =
      std::max(maxDiff(curve, base, 0, fFirst - 1),
               maxDiff(curve, base, fLast + 1, kFrames - 1));
  std::cout << "(a) plateau maxDiff=" << plateau << " c\n";

  // (b) join continuity.
  const float c0 = cents(curve[static_cast<size_t>(kB - 1)]);
  const float c1 = cents(curve[static_cast<size_t>(kB)]);
  const float c2 = cents(curve[static_cast<size_t>(kB + 1)]);
  const float jump = std::fabs(c1 - 0.5f * (c0 + c2));
  const float slope = std::fabs((c2 - c1) - (c1 - c0));
  std::cout << "(b) jump=" << jump << " c slope=" << slope
            << " c/frame\n";

  // (c) seam stitch: the first and last written frames keep the original
  // curve value (the Bezier is anchored on the real curve at both edges).
  const float seamFirst = diffAt(curve, base, fFirst);
  const float seamLast = diffAt(curve, base, fLast);
  std::cout << "(c) seam values=" << seamFirst << " / " << seamLast
            << " c\n";

  // (d) analytic Hermite fidelity and effective change.
  float bezierDiff = 0.0f;
  for (int f = fFirst; f <= fLast; ++f) {
    const double expected = hermiteAt(win, f * static_cast<double>(kHop));
    const float expectedCents = static_cast<float>(100.0 * (expected - 69.0));
    bezierDiff =
        std::max(bezierDiff,
                 std::fabs(cents(curve[static_cast<size_t>(f)]) -
                           expectedCents));
  }
  const float effective =
      maxDiff(curve, base, fFirst, fLast);
  std::cout << "(d) hermiteDiff=" << bezierDiff
            << " c change=" << effective << " c\n";

  // (e) C = 0 and W = 0 are bit-identical to D-205.
  Project zero = makeProject();
  zero.getBoundaryTransitions().set(0, kB, 20.0f, 0.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(zero);
  const float zeroDiff = maxDiff(f0Of(zero), base, 0, kFrames - 1);

  Project noWidth = makeProject();
  noWidth.getBoundaryTransitions().set(0, kB, 0.0f, -100.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(noWidth);
  const float noWidthDiff =
      maxDiff(f0Of(noWidth), base, 0, kFrames - 1);
  std::cout << "(e) C=0 maxDiff=" << zeroDiff
            << " c, W=0 maxDiff=" << noWidthDiff << " c\n";

  // (f) clamping and +100 %.
  Project wide = makeProject();
  wide.getBoundaryTransitions().set(0, kB, 200.0f, -100.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(wide);
  const float wideDiff = maxDiff(f0Of(wide), curve, 0, kFrames - 1);

  Project shortLeft = makeProject(2, kNoteFrames);
  const auto shortBase = f0Of(shortLeft);
  const int shortB = 2;
  shortLeft.getBoundaryTransitions().set(0, shortB, 20.0f, -100.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(shortLeft);
  const auto shortCurve = f0Of(shortLeft);
  // cap = min(2, 100) / 2 = 1 frame: only frame 2 is strictly inside.
  float cappedOutside = 0.0f;
  for (int f = 0; f < static_cast<int>(shortCurve.size()); ++f)
    if (f != shortB)
      cappedOutside = std::max(cappedOutside,
                               diffAt(shortCurve, shortBase, f));
  const float cappedInside = diffAt(shortCurve, shortBase, shortB);

  Project plus = makeProject();
  plus.getBoundaryTransitions().set(0, kB, 20.0f, 100.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(plus);
  const auto plusCurve = f0Of(plus);
  const float plusEffective = maxDiff(plusCurve, base, fFirst, fLast);
  const float plusOutside =
      std::max(maxDiff(plusCurve, base, 0, fFirst - 1),
               maxDiff(plusCurve, base, fLast + 1, kFrames - 1));
  std::cout << "(f) W200-vs-W20 maxDiff=" << wideDiff
            << " c, cap outside=" << cappedOutside
            << " c inside=" << cappedInside
            << " c, +100 change=" << plusEffective
            << " c outside=" << plusOutside << " c\n";

  CHECK(plateau <= kPlateauLimitCents);
  CHECK(jump <= kJumpLimitCents);
  CHECK(slope <= kSlopeLimitCentsPerFrame);
  CHECK(seamFirst <= kSeamSlopeLimitCentsPerFrame);
  CHECK(seamLast <= kSeamSlopeLimitCentsPerFrame);
  CHECK(bezierDiff <= kBezierLimitCents);
  CHECK(effective >= kEffectiveMinCents);
  CHECK(zeroDiff == 0.0f);
  CHECK(noWidthDiff == 0.0f);
  CHECK(wideDiff == 0.0f);
  CHECK(cappedOutside == 0.0f);
  CHECK(cappedInside >= kCappedMinCents);
  CHECK(plusEffective >= kEffectiveMinCents);
  CHECK(plusOutside <= kPlateauLimitCents);

  std::cout << "D207BoundaryTransitionTests: all checks passed\n";
  return 0;
}
