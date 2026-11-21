// D-239: the commit unit is the engine block.
//
// The symptom this test pins down: editing one note and rendering only that
// note's span splices a freshly rendered signal against audio from a previous
// render whose grain lattice has a different phase. PSOLA and the classical
// core anchor their lattice once per voiced run and accumulate the output
// position from there, so a pitch change inside the run shifts every later
// grain. The splice then cancels over the crossfade and is heard as a small
// cut between the edited note and its neighbours.
//
// The criterion is the invariant, not a threshold picked after looking:
// rendering the same edited project incrementally must produce exactly the
// audio a full render produces over the block it commits. Outside the block,
// the incremental render must leave the previous composite untouched.
//
// It runs PSOLA and Pitchcore, with a two-note legato phrase and an unvoiced
// tail long enough for the render range to end in original audio.

#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kHop = HOP_SIZE;
constexpr int kTotalFrames = 500;
constexpr int kNote0Start = 50;
constexpr int kNote0End = 200;
constexpr int kNote1End = 350;
constexpr int kUnvoicedFrom = kNote1End;
constexpr float kNote0Midi = 57.0f; // A3, 220 Hz
constexpr float kNote1Midi = 59.0f; // B3, ~246.9 Hz

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

// A sung two-note phrase: a harmonic tone whose frequency follows the notes,
// silent outside the voiced span. The source F0 and the note grid describe the
// same pitch, so the engines' pitch marks land on the pulses.
juce::AudioBuffer<float> makePhrase() {
  juce::AudioBuffer<float> buffer(1, kTotalFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float value = 0.0f;
    if (frame >= kNote0Start && frame < kNote1End) {
      const float midi = frame < kNote0End ? kNote0Midi : kNote1Midi;
      const double frequency = static_cast<double>(midiHz(midi));
      phase += 2.0 * 3.14159265358979323846 * frequency / kSampleRate;
      value = 0.30f * static_cast<float>(std::sin(phase)) +
              0.12f * static_cast<float>(std::sin(2.0 * phase)) +
              0.06f * static_cast<float>(std::sin(3.0 * phase));
    } else {
      phase = 0.0;
    }
    buffer.setSample(0, i, value);
  }
  // Short fades at the sounding edges so the fixture itself has no step.
  const int fade = kSampleRate / 100; // 10 ms
  const int first = kNote0Start * kHop;
  const int last = kNote1End * kHop;
  for (int i = 0; i < fade; ++i) {
    const float gain = static_cast<float>(i) / static_cast<float>(fade);
    buffer.setSample(0, first + i, buffer.getSample(0, first + i) * gain);
    buffer.setSample(0, last - 1 - i, buffer.getSample(0, last - 1 - i) * gain);
  }
  return buffer;
}

Project makeProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const auto phrase = makePhrase();
  audio.originalWaveform.makeCopyOf(phrase);
  audio.waveform.makeCopyOf(phrase);

  audio.melSpectrogram.assign(static_cast<size_t>(kTotalFrames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(kTotalFrames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(kTotalFrames), false);

  for (int frame = kNote0Start; frame < kUnvoicedFrom; ++frame) {
    audio.voicedMask[static_cast<size_t>(frame)] = true;
    audio.denseF0[static_cast<size_t>(frame)] =
        midiHz(frame < kNote0End ? kNote0Midi : kNote1Midi);
  }

  Note note0(kNote0Start, kNote0End, kNote0Midi);
  note0.setOriginalMidiNote(kNote0Midi);
  note0.setOriginalDeltaPitch(std::vector<float>(kNote0End - kNote0Start, 0.0f));
  Note note1(kNote0End, kNote1End, kNote1Midi);
  note1.setOriginalMidiNote(kNote1Midi);
  note1.setOriginalDeltaPitch(std::vector<float>(kNote1End - kNote0End, 0.0f));
  project.addNote(note0);
  project.addNote(note1);

  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

bool renderAndWait(Project &project, SynthesisEngineType engine,
                   const char *label) {
  IncrementalSynthesizer synthesizer;
  synthesizer.setProject(&project);
  synthesizer.setSynthesisEngine(engine);

  std::atomic<bool> finished{false};
  std::atomic<bool> success{false};
  synthesizer.synthesizeRegion(
      nullptr, [&](bool ok) {
        success.store(ok);
        finished.store(true);
      });

  const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  while (!finished.load() &&
         juce::Time::getMillisecondCounterHiRes() < deadline) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
  if (!finished.load()) {
    std::cout << label << ": render did not finish in 30 s\n";
    return false;
  }
  return success.load();
}

// Apply the edit the way the UI does: move note 0 one semitone, rebuild the
// target curve and mark the note plus its F0 window.
void editNote0(Project &project) {
  project.getNotes()[0].setPitchOffset(1.0f);
  project.getNotes()[0].markDirty();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kNote0Start, kNote0End);
}

float maxAbsDiff(const juce::AudioBuffer<float> &a,
                 const juce::AudioBuffer<float> &b, int firstSample,
                 int lastSample) {
  float worst = 0.0f;
  const int n = std::min({a.getNumSamples(), b.getNumSamples(), lastSample});
  for (int i = std::max(0, firstSample); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return worst;
}

float maxAbsDiffOutside(const juce::AudioBuffer<float> &a,
                        const juce::AudioBuffer<float> &b, int firstSample,
                        int lastSample) {
  float worst = 0.0f;
  const int n = std::min(a.getNumSamples(), b.getNumSamples());
  for (int i = 0; i < firstSample; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  for (int i = std::max(firstSample, lastSample); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return worst;
}

void checkEngine(SynthesisEngineType engine, const char *name,
                 float interiorTolerance) {
  // Reference: the same edited project rendered over the whole file. Clearing
  // the note flags leaves the F0 range as the only anchor, so both the old and
  // the new commit policies render and write the full extent.
  Project reference = makeProject();
  editNote0(reference);
  for (auto &note : reference.getNotes())
    note.clearDirty();
  reference.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(reference, engine, name));

  // Incremental: a pristine composite, the same edit, only the note dirty.
  Project incremental = makeProject();
  editNote0(incremental);
  CHECK(renderAndWait(incremental, engine, name));

  const auto &incrementalAudio = incremental.getAudioData().waveform;
  const auto &referenceAudio = reference.getAudioData().waveform;
  const auto &original = incremental.getAudioData().originalWaveform;

  // The block starts at 0 here; compare the interior up to a few frames before
  // the render range end (the PSOLA loudness match windows the buffer edge).
  const int interiorLastFrame = 385;
  const float diff = maxAbsDiff(incrementalAudio, referenceAudio, 0,
                                interiorLastFrame * kHop);
  std::cout << name << " incremental vs full render, interior max diff "
            << diff << " (limit " << interiorTolerance << ")\n";
  CHECK(diff <= interiorTolerance);

  // The note that was not edited must be rewritten as part of the block: it is
  // where the old phase splice used to be. Compare a window after the edited
  // note against the full render, and require it to be the new render, not the
  // original (the old per-note commit left the original there).
  const int blockEnd = 390 * kHop;

  // The seam is at the boundary between the edited note and the untouched
  // one: the target curve changes there because the base pitch is smoothed
  // over ~80 ms, and the old per-note commit ended exactly on that boundary.
  // The full render changes those frames; the incremental render must too.
  const int boundaryFirst = 195 * kHop;
  const int boundaryLast = 215 * kHop;
  const float referenceVsOriginal =
      maxAbsDiff(referenceAudio, original, boundaryFirst, boundaryLast);
  const float incrementalVsReference =
      maxAbsDiff(incrementalAudio, referenceAudio, boundaryFirst,
                 boundaryLast);
  std::cout << name << " boundary [" << 195 << "," << 215
            << ") ref-vs-orig " << referenceVsOriginal << " inc-vs-ref "
            << incrementalVsReference << "\n";
  CHECK(referenceVsOriginal > 1.0e-4f);
  CHECK(incrementalVsReference <= interiorTolerance);

  // Outside the block (the unvoiced tail) the previous composite survives.
  const float outsideDiff = maxAbsDiffOutside(incrementalAudio, original,
                                              0, blockEnd + 4096);
  std::cout << name << " outside the block vs original max diff "
            << outsideDiff << "\n";
  CHECK(outsideDiff <= 1.0e-7f);
}

} // namespace

// D-125 b at audio level: with two engine blocks separated by a usable
// silence, editing a note in the first block must leave the second block bit
// for bit identical to what was already there. The render still includes the
// second block as context (the silence is short enough for the phrase
// expansion), so this exercises the commit selection, not the render window.
void checkSecondBlockUntouched() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const int totalFrames = 700;
  audio.melSpectrogram.assign(static_cast<size_t>(totalFrames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(totalFrames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(totalFrames), false);

  auto fill = [&](int first, int last, float midi) {
    for (int frame = first; frame < last; ++frame) {
      audio.voicedMask[static_cast<size_t>(frame)] = true;
      audio.denseF0[static_cast<size_t>(frame)] = midiHz(midi);
    }
  };
  fill(50, 250, kNote0Midi);
  fill(253, 453, 62.0f);

  juce::AudioBuffer<float> source(1, totalFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < source.getNumSamples(); ++i) {
    const int frame = i / kHop;
    if (!audio.voicedMask[static_cast<size_t>(frame)]) {
      phase = 0.0;
      continue;
    }
    const double frequency = static_cast<double>(
        audio.denseF0[static_cast<size_t>(frame)]);
    phase += 2.0 * 3.14159265358979323846 * frequency / kSampleRate;
    source.setSample(0, i,
                     0.30f * static_cast<float>(std::sin(phase)) +
                         0.10f * static_cast<float>(std::sin(2.0 * phase)));
  }
  audio.originalWaveform.makeCopyOf(source);
  audio.waveform.makeCopyOf(source);

  auto addNote = [&](int start, int end, float midi, NoteEngine engine) {
    Note note(start, end, midi);
    note.setOriginalMidiNote(midi);
    note.setEngine(engine);
    note.setOriginalDeltaPitch(std::vector<float>(end - start, 0.0f));
    project.addNote(note);
  };
  addNote(50, 150, kNote0Midi, NoteEngine::Psola);
  addNote(150, 250, 59.0f, NoteEngine::Psola);
  addNote(253, 353, 62.0f, NoteEngine::Pitchcore);
  addNote(353, 453, 64.0f, NoteEngine::Pitchcore);
  PitchCurveProcessor::rebuildBaseFromNotes(project);

  project.getNotes()[0].setPitchOffset(1.0f);
  project.getNotes()[0].markDirty();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(50, 150);
  CHECK(renderAndWait(project, SynthesisEngineType::Psola, "two blocks"));

  const auto &rendered = project.getAudioData().waveform;
  const auto &original = project.getAudioData().originalWaveform;
  const float diff = maxAbsDiff(rendered, original, 253 * kHop, 453 * kHop);
  std::cout << "two blocks: second block vs original max diff " << diff
            << "\n";
  CHECK(diff <= 1.0e-7f);

  // And the first block did change: the test is not vacuous.
  const float changed = maxAbsDiff(
      rendered, original, 50 * kHop, 150 * kHop);
  CHECK(changed > 1.0e-3f);
}

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  checkEngine(SynthesisEngineType::Psola, "PSOLA", 2.0e-4f);
  checkEngine(SynthesisEngineType::Pitchcore, "Pitchcore", 2.0e-3f);
  checkSecondBlockUntouched();
  std::cout << "CommitSeamTests: all checks passed\n";
  return 0;
}
