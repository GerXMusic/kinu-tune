// D-306 / D-308: inclinar no mueve el cuerpo de la nota.
//
// Criterio escrito ANTES de medir:
//
//   Arrastrar una manija de esquina solo escribe SU capa fina (D-308):
//   TiltLeft -> tiltFineLeft, TiltRight -> tiltFineRight. El tilt general, la
//   otra capa fina, midiNote y la referencia de Correct Pitch quedan
//   exactamente como estaban. La base que se reconstruye con
//   rebuildBaseFromNotes() sigue en el centro de la nota
//   (maxCurveNotesDisagreement <= 0,1 st).
//
//   Control: el mismo arrastre sin inclinacion no cambia nada.
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRoll/PitchToolController.h"
#include "../Source/UI/PianoRoll/PitchToolHandles.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kFrames = 600;
constexpr float kToleranceSemitones = 0.1f;

void prepareAudio(Project &project) {
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.melSpectrogram.assign(static_cast<size_t>(kFrames),
                              std::vector<float>(1, 0.0f));
  audio.f0.assign(static_cast<size_t>(kFrames), 261.6f);
  audio.basePitch.assign(static_cast<size_t>(kFrames), 60.0f);
  audio.deltaPitch.assign(static_cast<size_t>(kFrames), 0.0f);
}

Note makeNote(int start, int end, float midi) {
  Note note(start, end, midi);
  note.setSrcStartFrame(start);
  note.setSrcEndFrame(end);
  note.setOriginalMidiNote(midi);
  note.setDeltaPitch(std::vector<float>(static_cast<size_t>(end - start), 0.0f));
  return note;
}

bool near(float a, float b, float tolerance = 1.0e-4f) {
  return std::abs(a - b) <= tolerance;
}

juce::MouseEvent makeEvent(const juce::Component &c, juce::Point<float> at,
                           bool dragged) {
  static auto source = juce::Desktop::getInstance().getMainMouseSource();
  return juce::MouseEvent(
      source, at, juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, const_cast<juce::Component *>(&c),
      const_cast<juce::Component *>(&c), juce::Time::getCurrentTime(), at,
      juce::Time::getCurrentTime(), 1, dragged);
}

struct Fixture {
  Project project;
  CoordinateMapper mapper;
  PitchToolController controller;
  PitchToolHandles handles;
  juce::Component dummy;

  Fixture() {
    prepareAudio(project);
    project.addNote(makeNote(100, 200, 64.0f));
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    mapper.setPixelsPerSecond(200.0f);
    mapper.setPixelsPerSemitone(24.0f);
    handles.setOptions({/*showAll=*/true, /*canGlueLeft=*/false,
                        /*canGlueRight=*/false});
    handles.updateHandles({&note()}, mapper);
    controller.setProject(&project);
  }

  Note &note() { return project.getNotes().front(); }

  const PitchToolHandles::Handle *find(PitchToolHandles::HandleType type) const {
    for (const auto &handle : handles.getHandles())
      if (handle.type == type)
        return &handle;
    return nullptr;
  }
};

// Un arrastre vertical de `semitones` sobre el control `type`.
void drag(Fixture &f, PitchToolHandles::HandleType type, float semitones) {
  const auto *handle = f.find(type);
  CHECK(handle != nullptr);
  if (handle == nullptr)
    return;
  const auto start = handle->bounds.getCentre();
  const auto end =
      start + juce::Point<float>(0.0f, -semitones * f.mapper.getPixelsPerSemitone());
  std::vector<Note *> selection{&f.note()};
  f.controller.mouseDown(makeEvent(f.dummy, start, false), f.handles, selection,
                         f.mapper);
  f.controller.mouseDrag(makeEvent(f.dummy, end, true), selection, f.mapper);
  f.controller.mouseUp(makeEvent(f.dummy, end, false), nullptr, {});
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // --- La manija izquierda escribe solo su capa fina ----------------------
  {
    Fixture f;
    const float beforeMidi = f.note().getMidiNote();
    const float beforeSource = f.note().getLastNonMacroMidiNote();
    drag(f, PitchToolHandles::HandleType::TiltLeft, 1.0f);
    std::printf("D-308 manija izquierda +1,0 st: fino izq %.4f | general %.4f | "
                "fino der %.4f | cuerpo %.4f -> %.4f | fuente %.4f -> %.4f\n",
                f.note().getTiltFineLeft(), f.note().getTilt(),
                f.note().getTiltFineRight(), beforeMidi, f.note().getMidiNote(),
                beforeSource, f.note().getLastNonMacroMidiNote());
    CHECK(near(f.note().getTiltFineLeft(), 1.0f, 1.0e-3f));
    CHECK(near(f.note().getTilt(), 0.0f));
    CHECK(near(f.note().getTiltFineRight(), 0.0f));
    CHECK(near(f.note().getMidiNote(), beforeMidi));
    CHECK(near(f.note().getLastNonMacroMidiNote(), beforeSource));
  }

  // --- La manija derecha tampoco toca el cuerpo ni las otras capas --------
  {
    Fixture f;
    f.note().setTilt(0.75f);
    f.note().setTiltFineLeft(-0.5f);
    const float beforeMidi = f.note().getMidiNote();
    drag(f, PitchToolHandles::HandleType::TiltRight, -1.5f);
    std::printf("D-308 manija derecha -1,5 st: fino der %.4f | general %.4f | "
                "fino izq %.4f | cuerpo %.4f\n",
                f.note().getTiltFineRight(), f.note().getTilt(),
                f.note().getTiltFineLeft(), f.note().getMidiNote());
    CHECK(near(f.note().getTiltFineRight(), -1.5f, 1.0e-3f));
    CHECK(near(f.note().getTilt(), 0.75f));
    CHECK(near(f.note().getTiltFineLeft(), -0.5f));
    CHECK(near(f.note().getMidiNote(), beforeMidi));
  }

  // --- La base sigue al cuerpo: la curva no se desfasa --------------------
  {
    Fixture f;
    drag(f, PitchToolHandles::HandleType::TiltLeft, 1.5f);
    drag(f, PitchToolHandles::HandleType::TiltRight, -1.0f);
    f.note().setTilt(1.25f);
    PitchCurveProcessor::rebuildBaseFromNotes(f.project);
    const float desync = f.project.maxCurveNotesDisagreement();
    std::printf("D-308 nota inclinada (general %.2f, finos %.2f / %.2f): "
                "desfase curva/notas %.4f st (lim %.1f)\n",
                f.note().getTilt(), f.note().getTiltFineLeft(),
                f.note().getTiltFineRight(), desync,
                kToleranceSemitones);
    CHECK(desync <= kToleranceSemitones);
  }

  // --- Control: un gesto que no cambia la altura no mueve el cuerpo ------
  {
    Fixture f;
    const float beforeMidi = f.note().getMidiNote();
    drag(f, PitchToolHandles::HandleType::Formant, 1.0f);
    CHECK(near(f.note().getMidiNote(), beforeMidi));
    CHECK(near(f.note().getFormantShift(), 1.0f, 1.0e-3f));
    std::printf("D-308 control formante: cuerpo %.4f (sin cambio)\n",
                f.note().getMidiNote());
  }

  std::printf("TiltCenterTests: en verde\n");
  return 0;
}
