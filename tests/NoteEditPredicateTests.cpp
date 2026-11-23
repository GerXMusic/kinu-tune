// T7 / D-248 - predicado unico de edicion audible.
//
// Criterio escrito ANTES de medir:
//   - `noteHasAudioRelevantEdit` es la unica definicion de "esta nota tiene una
//     edicion que toca el audio".
//   - `markEditedNotesDirty` marca EXACTAMENTE las notas para las que el
//     predicado es true (ni una mas, ni una menos).
//   - `hasRegionEdits` es true si y solo si alguna nota cumple el predicado (o
//     hay edicion global / bypass).
//   - El reset (`shouldResetNeutralNoteToOriginal`) no devuelve a la fuente una
//     nota con motor explicito sin renderizar ni una nota silenciada; si
//     devuelve una nota neutral sucia.

#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <cstdio>
#include <vector>

namespace {

constexpr int kHop = HOP_SIZE;
constexpr int kFrames = 200;

Project makeProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 48000;
  audio.waveform.setSize(1, kFrames * kHop);
  audio.originalWaveform.setSize(1, kFrames * kHop);
  audio.f0.assign(static_cast<size_t>(kFrames), 220.0f);
  audio.denseF0 = audio.f0;
  audio.voicedMask.assign(static_cast<size_t>(kFrames), true);
  for (int i = 0; i < 5; ++i) {
    Note note(10 + i * 30, 35 + i * 30, 60.0f);
    note.setOriginalMidiNote(60.0f);
    note.setDeltaPitch(std::vector<float>(25, 0.0f));
    note.setOriginalDeltaPitch(note.getDeltaPitch());
    project.addNote(note);
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

}  // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  Project project = makeProject();
  auto &notes = project.getNotes();

  notes[1].setMidiNote(60.5f);   // edicion de altura
  notes[2].setMuted(true);       // mute
  notes[3].setEngine(NoteEngine::Psola);  // motor explicito

  const bool expected[] = {false, true, true, true, false};
  for (int i = 0; i < 5; ++i)
    CHECK(Project::noteHasAudioRelevantEdit(notes[static_cast<size_t>(i)]) ==
          expected[i]);

  CHECK(project.hasRegionEdits());

  const int marked = project.markEditedNotesDirty();
  std::printf("T7 markEditedNotesDirty: %d nota(s)\n", marked);
  CHECK(marked == 3);
  for (int i = 0; i < 5; ++i) {
    const bool dirty = notes[static_cast<size_t>(i)].isDirty();
    const bool predicted =
        Project::noteHasAudioRelevantEdit(notes[static_cast<size_t>(i)]);
    CHECK(dirty == predicted);
  }

  // Reset: la nota neutral sucia vuelve a la fuente; mute y motor explicito
  // sin renderizar NO (conservan su sonido hasta que el reset los limpie).
  notes[0].markDirty();
  CHECK(project.shouldResetNeutralNoteToOriginal(notes[0]));
  notes[2].markDirty();
  CHECK(!project.shouldResetNeutralNoteToOriginal(notes[2]));
  notes[3].markDirty();
  CHECK(!project.shouldResetNeutralNoteToOriginal(notes[3]));

  std::printf("NoteEditPredicateTests: predicado unico OK\n");
  return 0;
}
