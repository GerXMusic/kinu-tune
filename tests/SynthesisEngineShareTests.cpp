// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
//
// The rendering engine (AI Resynthesis / Classic) in the plug-in.
//
// The plug-in keeps several EditorControllers and binds the editor to one of
// them. Switching the engine in the editor used to reach that one controller
// only; the controllers that actually render the region on the canvas kept the
// default engine. Now they all share one SynthesisEnginePreference, and a
// switch re-renders the notes that carry edits (Project::markEditedNotesDirty),
// since the audio on hand was made by the previous engine.

#include "../Source/Audio/EditorController.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/OnnxRuntime.h"
#include "TestAssert.h"

#include <iostream>
#include <memory>

namespace {

void controllersSharingAPreferenceSeeTheSameEngine() {
  auto shared = std::make_shared<SynthesisEnginePreference>();
  shared->value.store(SynthesisEngineType::Vocoder);

  EditorController boundToEditor(false);
  EditorController rendersTheCanvas(false);
  boundToEditor.shareSynthesisEngine(shared);
  rendersTheCanvas.shareSynthesisEngine(shared);
  CHECK(boundToEditor.getSynthesisEngineType() == SynthesisEngineType::Vocoder);
  CHECK(rendersTheCanvas.getSynthesisEngineType() == SynthesisEngineType::Vocoder);

  // The editor writes through its bound controller; the renderer reads it.
  boundToEditor.setSynthesisEngineType(SynthesisEngineType::Psola);
  CHECK(rendersTheCanvas.getSynthesisEngineType() == SynthesisEngineType::Psola);
  CHECK(shared->value.load() == SynthesisEngineType::Psola);

  // A controller created after the switch (a modification rendered in the
  // background) starts on the chosen engine, not on the default.
  EditorController createdLater(false);
  createdLater.shareSynthesisEngine(shared);
  CHECK(createdLater.getSynthesisEngineType() == SynthesisEngineType::Psola);

  // And back again, from the renderer's side this time.
  rendersTheCanvas.setSynthesisEngineType(SynthesisEngineType::Vocoder);
  CHECK(boundToEditor.getSynthesisEngineType() == SynthesisEngineType::Vocoder);
  CHECK(createdLater.getSynthesisEngineType() == SynthesisEngineType::Vocoder);
}

void unsharedControllersKeepTheirOwnSetting() {
  // The standalone app owns its single controller; nothing is shared there.
  EditorController own(false);
  EditorController stranger(false);
  own.setSynthesisEngineType(SynthesisEngineType::Psola);
  stranger.setSynthesisEngineType(SynthesisEngineType::Vocoder);
  CHECK(own.getSynthesisEngineType() == SynthesisEngineType::Psola);
  CHECK(stranger.getSynthesisEngineType() == SynthesisEngineType::Vocoder);
}

Project projectWithF0(int frames) {
  Project project;
  project.getAudioData().f0.assign(static_cast<size_t>(frames), 110.0f);
  return project;
}

void markEditedNotesDirtyMarksOnlyNotesWhoseEditsReachTheAudio() {
  auto project = projectWithF0(2000);

  Note untouched(100, 200, 60.0f);
  Note formantShifted(300, 400, 62.0f);
  formantShifted.setFormantShift(2.0f);
  Note offsetPitch(900, 1000, 64.0f);
  offsetPitch.setPitchOffset(0.5f);
  Note restWithEdits(1500, 1600, 60.0f);
  restWithEdits.setRest(true);
  restWithEdits.setFormantShift(3.0f);
  for (auto *note : {&untouched, &formantShifted, &offsetPitch, &restWithEdits})
    note->clearDirty();
  project.addNote(untouched);
  project.addNote(formantShifted);
  project.addNote(offsetPitch);
  project.addNote(restWithEdits);
  project.clearAllDirty();
  CHECK(!project.hasDirtyNotes());
  CHECK(!project.hasF0DirtyRange());

  CHECK(project.markEditedNotesDirty() == 2);

  const auto &notes = project.getNotes();
  CHECK(notes.size() == 4);
  CHECK(!notes[0].isDirty());
  CHECK(notes[1].isDirty());
  CHECK(notes[2].isDirty());
  CHECK(!notes[3].isDirty());

  // The F0 dirty range spans the edited notes with the same 60-frame padding
  // an edit of those notes would apply (Project::markNoteEditDirtyRange).
  CHECK(project.hasF0DirtyRange());
  const auto range = project.getF0DirtyRange();
  CHECK(range.first == 300 - 60);
  CHECK(range.second == 1000 + 60);
}

void markEditedNotesDirtyLeavesAnUneditedProjectAlone() {
  auto project = projectWithF0(2000);
  Note a(100, 200, 60.0f);
  Note b(300, 400, 62.0f);
  project.addNote(a);
  project.addNote(b);
  project.clearAllDirty();

  CHECK(project.markEditedNotesDirty() == 0);
  CHECK(!project.hasDirtyNotes());
  CHECK(!project.hasF0DirtyRange());
}

void projectWideEditsReachEveryNote() {
  auto project = projectWithF0(2000);
  Note a(100, 200, 60.0f);
  Note b(300, 400, 62.0f);
  Note rest(500, 600, 60.0f);
  rest.setRest(true);
  project.addNote(a);
  project.addNote(b);
  project.addNote(rest);
  project.clearAllDirty();
  project.setFormantShift(1.0f);

  CHECK(project.markEditedNotesDirty() == 2);
  const auto &notes = project.getNotes();
  CHECK(notes[0].isDirty());
  CHECK(notes[1].isDirty());
  CHECK(!notes[2].isDirty());
  const auto range = project.getF0DirtyRange();
  CHECK(range.first == 100 - 60);
  CHECK(range.second == 400 + 60);
}

} // namespace

int main() {
  // EditorController needs the JUCE runtime (message manager, fonts) like the
  // other tests that construct one (AuditionHandoffTests).
  juce::ScopedJuceInitialiser_GUI gui;
  // The detectors the controller owns need the ONNX Runtime API; the plug-in
  // loads it from its bundle, a test from the library it links.
  Ort::InitApi(OrtGetApiBase()->GetApi(ORT_API_VERSION));

  controllersSharingAPreferenceSeeTheSameEngine();
  unsharedControllersKeepTheirOwnSetting();
  markEditedNotesDirtyMarksOnlyNotesWhoseEditsReachTheAudio();
  markEditedNotesDirtyLeavesAnUneditedProjectAlone();
  projectWideEditsReachEveryNote();
  std::cout << "SynthesisEngineShareTests passed" << std::endl;
  return 0;
}
