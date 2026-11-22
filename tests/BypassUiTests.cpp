// Tareas 1-3 (2026-11-22): cajon Motor alineado, bypass en la barra superior y
// vista de curva de fuente.
//
// Criterios escritos ANTES de medir:
// - T1: todos los controles del cajon comparten el margen izquierdo (kPad = 12)
//   y el ancho del area; ningun rotulo va centrado (justificacion a la
//   izquierda).
// - T2: el boton Bypass existe y es visible en la barra superior, a la derecha
//   del boton de rejilla (1/16); su onClick avisa por onBypassClicked y
//   setBypassState marca el estado activo.
// - T3: con la vista de bypass activa, la curva que se dibuja es la F0 de la
//   FUENTE: max|diff| <= 1 cent contra denseF0. Antes (vista editada) la curva
//   difiere de la fuente en el desplazamiento de la nota (+200 cents con +2 st),
//   asi que el mismo criterio falla sin el cambio.

#include "../Source/Models/Project.h"
#include "../Source/UI/EnginePanel.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/ToolbarComponent.h"
#include "../Source/Undo/UndoActions.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kHop = HOP_SIZE;
constexpr int kTotalFrames = 240;
constexpr int kNoteStart = 50;
constexpr int kNoteEnd = 150;
constexpr float kMidi = 57.0f;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

Project makeProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  juce::AudioBuffer<float> buffer(1, kTotalFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float value = 0.0f;
    if (frame >= kNoteStart && frame < kNoteEnd) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(kMidi) / kSampleRate;
      value = 0.3f * static_cast<float>(std::sin(phase));
    } else {
      phase = 0.0;
    }
    buffer.setSample(0, i, value);
  }
  audio.originalWaveform.makeCopyOf(buffer);
  audio.waveform.makeCopyOf(buffer);
  audio.melSpectrogram.assign(static_cast<size_t>(kTotalFrames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(kTotalFrames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(kTotalFrames), false);
  for (int f = kNoteStart; f < kNoteEnd; ++f) {
    audio.voicedMask[static_cast<size_t>(f)] = true;
    audio.denseF0[static_cast<size_t>(f)] = midiHz(kMidi);
  }
  Note note(kNoteStart, kNoteEnd, kMidi);
  note.setOriginalMidiNote(kMidi);
  note.setOriginalDeltaPitch(std::vector<float>(kNoteEnd - kNoteStart, 0.0f));
  project.addNote(note);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

std::vector<float> sourceCurveCents(const Project &project) {
  const auto &f0 = project.getAudioData().denseF0;
  std::vector<float> curve;
  for (float hz : f0)
    curve.push_back(hz > 0.0f ? (69.0f + 12.0f * std::log2(hz / 440.0f)) * 100.0f
                              : 0.0f);
  return curve;
}

float maxAbsDiff(const std::vector<float> &a, const std::vector<float> &b) {
  float worst = 0.0f;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;

  // ---------------- T1: cajon Motor alineado a la izquierda ----------------
  {
    EnginePanel panel(EditorUiMode::Advanced);
    panel.setSize(300, 640);
    CHECK(panel.areHeadersLeftAligned());
    int checked = 0;
    for (const auto &bounds : panel.getAlignedControlBounds()) {
      if (bounds.isEmpty())
        continue;
      ++checked;
      CHECK(bounds.getX() == 12);
      CHECK(bounds.getRight() == 288);
      CHECK(bounds.getWidth() == 276);
    }
    std::printf("T1 cajon: %d controles alineados en x=12, ancho 276, "
                "rotulos a la izquierda\n", checked);
    CHECK(checked >= 6);
  }

  // ---------------- T2: bypass en la barra superior ------------------------
  {
    ToolbarComponent bar;
    bar.setSize(1495, 60);
    int clicks = 0;
    bar.onBypassClicked = [&clicks] { ++clicks; };
    CHECK(bar.isBypassButtonVisible());
    bar.setBypassState(true);
    CHECK(bar.isBypassActive());
    const auto bypassBounds = bar.getBypassButtonBounds();
    const auto gridBounds = bar.getGridButtonBounds();
    std::printf("T2 barra: bypass [%d,%d) 72x%d | rejilla [%d,%d)\n",
                bypassBounds.getX(), bypassBounds.getRight(),
                bypassBounds.getHeight(), gridBounds.getX(),
                gridBounds.getRight());
    CHECK(!bypassBounds.isEmpty());
    CHECK(bypassBounds.getX() > gridBounds.getX());
    CHECK(bypassBounds.getRight() <= 1495);
    bar.clickBypassNow();
    CHECK(clicks == 1);
    bar.setBypassState(false);
    CHECK(!bar.isBypassActive());
  }

  // ---------------- T3: curva de fuente con bypass -------------------------
  {
    Project project = makeProject();
    project.getNotes()[0].setMidiNote(kMidi + 2.0f); // +200 cents
    PitchCurveProcessor::rebuildBaseFromNotes(project);

    PianoRollComponent roll;
    PitchUndoManager history;
    roll.setProject(&project);
    roll.setUndoManager(&history);

    const auto source = sourceCurveCents(project);
    const std::vector<float> sourceSpan(source.begin() + kNoteStart,
                                        source.begin() + kNoteEnd);
    // La curva que dibuja la vista editada es la F0 objetivo (getAdjustedF0ForRange).
    std::vector<float> editedSpan;
    for (float hz : project.getAdjustedF0ForRange(kNoteStart, kNoteEnd))
      editedSpan.push_back(hz > 0.0f ? (69.0f + 12.0f * std::log2(hz / 440.0f)) * 100.0f
                                     : 0.0f);
    const auto editedCurve = roll.getBypassCurveCents(kNoteStart, kNoteEnd);
    // La vista editada no es la fuente: el criterio de 1 cent falla antes del
    // cambio (rojo medido sobre la curva que se dibuja hoy).
    const float editedVsSource = maxAbsDiff(editedSpan, sourceSpan);
    std::printf("T3 ANTES (vista editada): curva vs fuente %.2f cents "
                "(lim 1 cent) -> %s\n",
                editedVsSource,
                editedVsSource <= 1.0f ? "verde" : "ROJO");
    CHECK(!roll.isBypassView());
    CHECK(editedVsSource > 1.0f);

    roll.setBypassView(true);
    CHECK(roll.isBypassView());
    const auto gray = roll.getBypassCurveCents(kNoteStart, kNoteEnd);
    const float grayVsSource = maxAbsDiff(gray, sourceSpan);
    std::printf("T3 DESPUES (bypass): curva vs fuente %.8f cents (lim 1 cent)\n",
                grayVsSource);
    CHECK(grayVsSource <= 1.0f);

    roll.setBypassView(false);
    CHECK(!roll.isBypassView());
  }

  std::printf("BypassUiTests: en verde\n");
  return 0;
}
