// D-189: editing the vibrato handle of ONE note must not move the curve of other
// notes, vibrato 1.0 must restore exactly, a shift edit must stay local, and the
// drag preview must match the committed curve inside the note.
//
// Runs with the D-182 glide ON and OFF. The first I1 run does not touch the flag,
// so it measures the build's default.

#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr int kNoteFrames = 100;
constexpr int kNotes = 7;
constexpr int kFrames = kNotes * kNoteFrames;

struct Snapshot {
  std::vector<float> base;
  std::vector<float> delta;
  std::vector<float> target;  // Hz
  std::vector<float> ratio;
};

Snapshot capture(const Project &project) {
  const auto &audio = project.getAudioData();
  const size_t n = audio.basePitch.size();
  Snapshot s;
  s.base = audio.basePitch;
  s.delta = audio.deltaPitch;
  s.target.resize(n);
  s.ratio.assign(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const float midi = audio.basePitch[i] + audio.deltaPitch[i];
    s.target[i] = midiToFreq(midi);
    const float src = i < audio.denseF0.size() ? audio.denseF0[i] : 0.0f;
    if (src > 0.0f && s.target[i] > 0.0f)
      s.ratio[i] = s.target[i] / src;
  }
  return s;
}

Project makeProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  audio.melSpectrogram.resize(kFrames);
  audio.denseF0.assign(kFrames, 0.0f);

  const float midi[kNotes] = {60, 60, 62, 64, 62, 60, 60};
  for (int n = 0; n < kNotes; ++n) {
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
  // Note 3 is moved +1 semitone: its two boundaries are edited by >= 60 cents,
  // which is what arms the D-182 glide.
  project.getNotes()[3].setMidiNote(midi[3] + 1.0f);
  return project;
}

// Max difference outside [start-margin, end+margin), per magnitude. Returns the
// worst relative/absolute value and records which frames differ.
struct Diff {
  float base = 0.0f;
  float delta = 0.0f;
  float targetRel = 0.0f;
  float ratioRel = 0.0f;
  int differingFrames = 0;
  float max() const {
    return std::max(std::max(base, delta), std::max(targetRel, ratioRel));
  }
};

Diff diffOutside(const Snapshot &a, const Snapshot &b, int start, int end,
                 int margin) {
  Diff d;
  const int n = static_cast<int>(std::min(a.target.size(), b.target.size()));
  for (int i = 0; i < n; ++i) {
    if (i >= start - margin && i < end + margin)
      continue;
    const float db = std::fabs(a.base[static_cast<size_t>(i)] -
                               b.base[static_cast<size_t>(i)]);
    const float dd = std::fabs(a.delta[static_cast<size_t>(i)] -
                               b.delta[static_cast<size_t>(i)]);
    const float at = a.target[static_cast<size_t>(i)];
    const float bt = b.target[static_cast<size_t>(i)];
    const float dt = at > 0.0f ? std::fabs(bt / at - 1.0f)
                               : std::fabs(bt - at);
    const float ar = a.ratio[static_cast<size_t>(i)];
    const float br = b.ratio[static_cast<size_t>(i)];
    const float dr = ar > 0.0f ? std::fabs(br / ar - 1.0f)
                               : std::fabs(br - ar);
    if (db > 0.0f || dd > 0.0f || dt > 0.0f || dr > 0.0f)
      ++d.differingFrames;
    d.base = std::max(d.base, db);
    d.delta = std::max(d.delta, dd);
    d.targetRel = std::max(d.targetRel, dt);
    d.ratioRel = std::max(d.ratioRel, dr);
  }
  return d;
}

Diff diffInside(const Snapshot &a, const Snapshot &b, int start, int end) {
  Diff d;
  for (int i = start; i < end; ++i) {
    const float db = std::fabs(a.base[static_cast<size_t>(i)] -
                               b.base[static_cast<size_t>(i)]);
    const float dd = std::fabs(a.delta[static_cast<size_t>(i)] -
                               b.delta[static_cast<size_t>(i)]);
    const float at = a.target[static_cast<size_t>(i)];
    const float bt = b.target[static_cast<size_t>(i)];
    const float dt = at > 0.0f ? std::fabs(bt / at - 1.0f)
                               : std::fabs(bt - at);
    const float ar = a.ratio[static_cast<size_t>(i)];
    const float br = b.ratio[static_cast<size_t>(i)];
    const float dr = ar > 0.0f ? std::fabs(br / ar - 1.0f)
                               : std::fabs(br - ar);
    if (db > 0.0f || dd > 0.0f || dt > 0.0f || dr > 0.0f)
      ++d.differingFrames;
    d.base = std::max(d.base, db);
    d.delta = std::max(d.delta, dd);
    d.targetRel = std::max(d.targetRel, dt);
    d.ratioRel = std::max(d.ratioRel, dr);
  }
  return d;
}

constexpr int kBaseMargin = 8;   // 80 ms cosine base window
constexpr int kGlideMargin = 16; // 0.18 s, half of the 0.36 s glide

int marginFor(bool glideEnabled) {
  return glideEnabled ? kGlideMargin : 0;
}

int cmpd(const Diff &d, int margin) {
  return d.differingFrames;
}

// I1 preview path: change vibrato of note 3 and rebuild only its delta.
Diff runI1Preview(bool setFlag, bool glideEnabled, int margin) {
  if (setFlag)
    PitchCurveProcessor::setEditedBoundaryGlideEnabled(glideEnabled);
  Project project = makeProject();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const Snapshot before = capture(project);
  auto &note = project.getNotes()[3];
  note.setVibrato(0.438f);
  PitchCurveProcessor::rebuildDeltaForNotes(project, {&note});
  const Snapshot after = capture(project);
  return diffOutside(before, after, note.getStartFrame(), note.getEndFrame(),
                     margin);
}

// I1 commit path.
Diff runI1Commit(bool setFlag, bool glideEnabled, int margin) {
  if (setFlag)
    PitchCurveProcessor::setEditedBoundaryGlideEnabled(glideEnabled);
  Project project = makeProject();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const Snapshot before = capture(project);
  auto &note = project.getNotes()[3];
  note.setVibrato(0.438f);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const Snapshot after = capture(project);
  return diffOutside(before, after, note.getStartFrame(), note.getEndFrame(),
                     margin);
}

// I2: vibrato 0.438 -> 1.0 restores exactly (whole curve).
Diff runI2(bool glideEnabled, bool previewPath) {
  PitchCurveProcessor::setEditedBoundaryGlideEnabled(glideEnabled);
  Project project = makeProject();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const Snapshot before = capture(project);
  auto &note = project.getNotes()[3];
  note.setVibrato(0.438f);
  if (previewPath)
    PitchCurveProcessor::rebuildDeltaForNotes(project, {&note});
  else
    PitchCurveProcessor::rebuildBaseFromNotes(project);
  note.setVibrato(1.0f);
  if (previewPath)
    PitchCurveProcessor::rebuildDeltaForNotes(project, {&note});
  else
    PitchCurveProcessor::rebuildBaseFromNotes(project);
  return diffInside(before, capture(project), 0, kFrames);
}

// I3: moving one note's pitch only touches its frames plus the declared margin.
Diff runI3(bool glideEnabled) {
  PitchCurveProcessor::setEditedBoundaryGlideEnabled(glideEnabled);
  Project project = makeProject();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const Snapshot before = capture(project);
  auto &note = project.getNotes()[3];
  note.setMidiNote(note.getMidiNote() + 1.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const int margin = glideEnabled ? kGlideMargin : kBaseMargin;
  return diffOutside(before, capture(project), note.getStartFrame(),
                     note.getEndFrame(), margin);
}

// I4: preview (drag) vs commit (release) inside the edited note.
Diff runI4(bool glideEnabled) {
  PitchCurveProcessor::setEditedBoundaryGlideEnabled(glideEnabled);
  Project project = makeProject();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  auto &note = project.getNotes()[3];
  note.setVibrato(0.438f);
  PitchCurveProcessor::rebuildDeltaForNotes(project, {&note});
  const Snapshot preview = capture(project);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  const Snapshot committed = capture(project);
  return diffInside(preview, committed, note.getStartFrame(), note.getEndFrame());
}

}  // namespace

int main() {
  // Default of the build: no flag is touched, product margin (0 frames).
  const Diff i1Default = runI1Preview(false, false, 0);
  const bool defaultPass = i1Default.max() == 0.0f;

  const Diff i1OnPreview = runI1Preview(true, true, kGlideMargin);
  const Diff i1OnCommit = runI1Commit(true, true, kGlideMargin);
  const Diff i1OffPreview = runI1Preview(true, false, 0);
  const Diff i1OffCommit = runI1Commit(true, false, 0);

  const Diff i2OnCommit = runI2(true, false);
  const Diff i2OnPreview = runI2(true, true);
  const Diff i2OffCommit = runI2(false, false);
  const Diff i2OffPreview = runI2(false, true);

  const Diff i3On = runI3(true);
  const Diff i3Off = runI3(false);

  const Diff i4On = runI4(true);
  const Diff i4Off = runI4(false);

  auto report = [](const char *name, const Diff &d) {
    std::cout << name << ": frames=" << d.differingFrames << " max=" << d.max()
              << " (base=" << d.base << " delta=" << d.delta
              << " targetRel=" << d.targetRel << " ratioRel=" << d.ratioRel
              << ")\n";
  };

  std::cout << "I1 default (no flag): "
            << (defaultPass ? "PASA" : "FALLA") << "\n";
  report("I1 glide ON  preview", i1OnPreview);
  report("I1 glide ON  commit ", i1OnCommit);
  report("I1 glide OFF preview", i1OffPreview);
  report("I1 glide OFF commit ", i1OffCommit);
  report("I2 glide ON  commit ", i2OnCommit);
  report("I2 glide ON  preview", i2OnPreview);
  report("I2 glide OFF commit ", i2OffCommit);
  report("I2 glide OFF preview", i2OffPreview);
  report("I3 glide ON ", i3On);
  report("I3 glide OFF", i3Off);
  report("I4 glide ON ", i4On);
  report("I4 glide OFF", i4Off);

  const bool i1PassOn = i1OnPreview.max() == 0.0f && i1OnCommit.max() == 0.0f;
  const bool i1PassOff = i1OffPreview.max() == 0.0f && i1OffCommit.max() == 0.0f;
  const bool i2PassOn = i2OnCommit.max() == 0.0f && i2OnPreview.max() == 0.0f;
  const bool i2PassOff = i2OffCommit.max() == 0.0f && i2OffPreview.max() == 0.0f;
  const bool i3PassOn = i3On.targetRel == 0.0f && i3On.ratioRel == 0.0f &&
                        i3On.base == 0.0f && i3On.delta == 0.0f;
  const bool i3PassOff = i3Off.targetRel == 0.0f && i3Off.ratioRel == 0.0f &&
                         i3Off.base == 0.0f && i3Off.delta == 0.0f;
  const bool i4PassOn = i4On.max() <= 1.0e-4f && i4On.base <= 1.0e-4f &&
                        i4On.delta <= 1.0e-4f;
  const bool i4PassOff = i4Off.max() <= 1.0e-4f && i4Off.base <= 1.0e-4f &&
                         i4Off.delta <= 1.0e-4f;

  CHECK(defaultPass);
  CHECK(i1PassOn);
  CHECK(i1PassOff);
  CHECK(i2PassOn);
  CHECK(i2PassOff);
  CHECK(i3PassOn);
  CHECK(i3PassOff);
  CHECK(i4PassOn);
  CHECK(i4PassOff);

  std::cout << "PitchCurveHandleTests: all checks passed\n";
  return 0;
}
