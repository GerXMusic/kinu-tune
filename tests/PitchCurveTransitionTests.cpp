// D-182: note-boundary transitions when the melody is edited.
//
// The target curve is base + delta. The stored delta is relative to the
// original base, so moving one side of a boundary leaves the old 80 ms step in
// the subtraction. The fix smooths the shift (target - source) around edited
// boundaries and writes it back into the global delta; the source contour is
// not touched. An unedited boundary (or a phrase moved as a whole) must keep
// the default curve.

#include "../Source/Models/Project.h"
#include "../Source/Utils/BasePitchCurve.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr int kFrames = 300;
constexpr int kBoundary = 100;

float maxStepCents(const std::vector<float> &curve, int center, int radius) {
  float best = 0.0f;
  const int first = std::max(1, center - radius);
  const int last = std::min(static_cast<int>(curve.size()) - 1, center + radius);
  for (int i = first; i <= last; ++i)
    best = std::max(
        best, std::abs(curve[static_cast<size_t>(i)] -
                       curve[static_cast<size_t>(i - 1)]) *
                  100.0f);
  return best;
}

Project makeProject(float targetLeft, float targetRight, float originalLeft,
                    float originalRight) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  audio.melSpectrogram.resize(kFrames);
  audio.denseF0.assign(static_cast<size_t>(kFrames), 0.0f);
  for (int frame = 0; frame < kBoundary; ++frame)
    audio.denseF0[static_cast<size_t>(frame)] = midiToFreq(originalLeft);
  for (int frame = kBoundary; frame < kFrames; ++frame)
    audio.denseF0[static_cast<size_t>(frame)] = midiToFreq(originalRight);

  Note left(0, kBoundary, targetLeft);
  left.setOriginalMidiNote(originalLeft);
  Note right(kBoundary, 200, targetRight);
  right.setOriginalMidiNote(originalRight);
  project.addNote(left);
  project.addNote(right);
  return project;
}

std::vector<float> targetCurve(Project &project) {
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const auto &audio = project.getAudioData();
  std::vector<float> target(audio.basePitch.size(), 0.0f);
  for (size_t i = 0; i < target.size() && i < audio.deltaPitch.size(); ++i)
    target[i] = audio.basePitch[i] + audio.deltaPitch[i];
  return target;
}

}  // namespace

int main() {
  // Unedited: source and target agree; the target is the plain base curve.
  auto uneditedProject = makeProject(60.0f, 66.0f, 60.0f, 66.0f);
  const auto unedited = targetCurve(uneditedProject);
  const auto defaultBase = BasePitchCurve::generateForNotes(
      {{0, kBoundary, 60.0f, 0.0f}, {kBoundary, 200, 66.0f, 0.0f}}, kFrames);
  CHECK(unedited.size() == defaultBase.size());
  for (size_t i = 0; i < unedited.size(); ++i)
    CHECK(unedited[i] == defaultBase[i]);
  const float uneditedStep = maxStepCents(unedited, kBoundary, 3);
  CHECK(uneditedStep > 0.0f);

  // One side moved +600 cents: the boundary must get less steep.
  auto editedProject = makeProject(60.0f, 66.0f, 60.0f, 60.0f);
  const auto edited = targetCurve(editedProject);
  const float editedStep = maxStepCents(edited, kBoundary, 3);
  CHECK(editedStep < uneditedStep);
  std::cout << "boundary step cents/frame: default " << uneditedStep
            << " -> edited " << editedStep << "\n";

  // Whole phrase moved +300 cents: the interval is unchanged, so the default
  // transition must be preserved exactly.
  auto wholePhraseProject = makeProject(63.0f, 69.0f, 60.0f, 66.0f);
  const auto wholePhrase = targetCurve(wholePhraseProject);
  const auto expectedWhole = BasePitchCurve::generateForNotes(
      {{0, kBoundary, 63.0f, 0.0f}, {kBoundary, 200, 69.0f, 0.0f}}, kFrames);
  CHECK(wholePhrase.size() == expectedWhole.size());
  for (size_t i = 0; i < wholePhrase.size(); ++i)
    CHECK(wholePhrase[i] == expectedWhole[i]);

  std::cout << "PitchCurveTransitionTests: all checks passed\n";
  return 0;
}
