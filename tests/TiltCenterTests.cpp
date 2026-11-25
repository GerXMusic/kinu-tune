// D-306: inclinar no mueve el cuerpo de la nota.
//
// Criterio escrito ANTES de medir:
//
//   Arrastrar el control de inclinacion (izquierda o derecha) solo escribe
//   tiltLeft/tiltRight. midiNote y la referencia de Correct Pitch quedan
//   exactamente como estaban, y la base que se reconstruye con
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

  // --- Inclinar izquierda no mueve el cuerpo ------------------------------
  {
    Fixture f;
    const float beforeMidi = f.note().getMidiNote();
    const float beforeSource = f.note().getLastNonMacroMidiNote();
    drag(f, PitchToolHandles::HandleType::TiltLeft, 1.0f);
    std::printf("D-306 tilt izquierda +1,0 st: tilt %.4f | cuerpo %.4f -> "
                "%.4f | fuente %.4f -> %.4f\n",
                f.note().getTiltLeft(), beforeMidi, f.note().getMidiNote(),
                beforeSource, f.note().getLastNonMacroMidiNote());
    CHECK(near(f.note().getTiltLeft(), 1.0f, 1.0e-3f));
    CHECK(near(f.note().getMidiNote(), beforeMidi));
    CHECK(near(f.note().getLastNonMacroMidiNote(), beforeSource));
  }

  // --- Inclinar derecha tampoco -------------------------------------------
  {
    Fixture f;
    const float beforeMidi = f.note().getMidiNote();
    drag(f, PitchToolHandles::HandleType::TiltRight, -1.5f);
    std::printf("D-306 tilt derecha -1,5 st: tilt %.4f | cuerpo %.4f\n",
                f.note().getTiltRight(), f.note().getMidiNote());
    CHECK(near(f.note().getTiltRight(), -1.5f, 1.0e-3f));
    CHECK(near(f.note().getMidiNote(), beforeMidi));
  }

  // --- La base sigue al cuerpo: la curva no se desfasa --------------------
  {
    Fixture f;
    drag(f, PitchToolHandles::HandleType::TiltLeft, 1.5f);
    drag(f, PitchToolHandles::HandleType::TiltRight, -1.0f);
    PitchCurveProcessor::rebuildBaseFromNotes(f.project);
    const float desync = f.project.maxCurveNotesDisagreement();
    std::printf("D-306 nota inclinada (%.2f / %.2f): desfase curva/notas "
                "%.4f st (lim %.1f)\n",
                f.note().getTiltLeft(), f.note().getTiltRight(), desync,
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
    std::printf("D-306 control formante: cuerpo %.4f (sin cambio)\n",
                f.note().getMidiNote());
  }

  std::printf("TiltCenterTests: en verde\n");
  return 0;
}
