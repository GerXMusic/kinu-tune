// D-183 R6: moving a contiguous group and putting it back exactly must give
// the same target curve. The drag path is the one the UI uses; the full path
// is the reference. Runs with the D-182 glide enabled and disabled so the
// attribution is explicit.

#include "../Source/Models/Project.h"
#include "../Source/Utils/BasePitchCurve.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr int kFrames = 900;
constexpr int kNoteFrames = 100;

struct Snapshot {
  std::vector<float> curve;
  std::vector<float> delta;
};

Snapshot capture(const Project &project) {
  const auto &audio = project.getAudioData();
  Snapshot s;
  s.curve.resize(audio.basePitch.size());
  for (size_t i = 0; i < s.curve.size() && i < audio.deltaPitch.size(); ++i)
    s.curve[i] = audio.basePitch[i] + audio.deltaPitch[i];
  s.delta = audio.deltaPitch;
  return s;
}

Project makeProject(std::vector<Note *> &group) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  audio.melSpectrogram.resize(kFrames);
  audio.denseF0.assign(kFrames, 0.0f);

  const float midi[7] = {60, 60, 62, 64, 62, 60, 60};
  for (int n = 0; n < 7; ++n) {
    Note note(n * kNoteFrames, (n + 1) * kNoteFrames, midi[n]);
    note.setOriginalMidiNote(midi[n]);
    std::vector<float> contour(kNoteFrames);
    for (int i = 0; i < kNoteFrames; ++i) {
      contour[static_cast<size_t>(i)] =
          0.08f * std::sin(0.15f * static_cast<float>(i) +
                           0.7f * static_cast<float>(n));
      const int frame = n * kNoteFrames + i;
      audio.denseF0[static_cast<size_t>(frame)] =
          midiToFreq(midi[n] + contour[static_cast<size_t>(i)]);
    }
    note.setOriginalDeltaPitch(contour);
    project.addNote(note);
  }

  auto &notes = project.getNotes();
  for (int n = 1; n <= 5; ++n)
    group.push_back(&notes[static_cast<size_t>(n)]);
  return project;
}

bool sameSnapshot(const Snapshot &a, const Snapshot &b) {
  if (a.curve.size() != b.curve.size() || a.delta.size() != b.delta.size())
    return false;
  for (size_t i = 0; i < a.curve.size(); ++i)
    if (a.curve[i] != b.curve[i])
      return false;
  for (size_t i = 0; i < a.delta.size(); ++i)
    if (a.delta[i] != b.delta[i])
      return false;
  return true;
}

bool runRoundTrip(bool glideEnabled, bool fullPath) {
  PitchCurveProcessor::setEditedBoundaryGlideEnabled(glideEnabled);
  std::vector<Note *> group;
  Project project = makeProject(group);

  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const Snapshot original = capture(project);

  for (auto *note : group)
    note->setMidiNote(note->getMidiNote() + 4.0f);
  if (fullPath)
    PitchCurveProcessor::rebuildBaseFromNotes(project);
  else
    PitchCurveProcessor::rebuildBaseFromNotesForDrag(project, group);

  for (auto *note : group)
    note->setMidiNote(note->getMidiNote() - 4.0f);
  if (fullPath)
    PitchCurveProcessor::rebuildBaseFromNotes(project);
  else
    PitchCurveProcessor::rebuildBaseFromNotesForDrag(project, group);

  return sameSnapshot(original, capture(project));
}

}  // namespace

int main() {
  const bool fullOn = runRoundTrip(true, true);
  const bool dragOn = runRoundTrip(true, false);
  const bool fullOff = runRoundTrip(false, true);
  const bool dragOff = runRoundTrip(false, false);

  std::cout << "glide ON : full=" << (fullOn ? "identical" : "DIFFERENT")
            << " drag=" << (dragOn ? "identical" : "DIFFERENT") << "\n";
  std::cout << "glide OFF: full=" << (fullOff ? "identical" : "DIFFERENT")
            << " drag=" << (dragOff ? "identical" : "DIFFERENT") << "\n";

  CHECK(fullOn);
  CHECK(dragOn);
  CHECK(fullOff);
  CHECK(dragOff);
  std::cout << "PitchCurveRoundTripTests: all checks passed\n";
  return 0;
}
