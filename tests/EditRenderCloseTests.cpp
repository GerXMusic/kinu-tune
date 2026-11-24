// S4d (pendiente 1 de la fase 3): el cierre del render del plugin.
//
// Criterio escrito ANTES de medir (no se ajusta despues del numero):
//
//   Con un render en vuelo pedido por la politica A (render al soltar), una
//   SEGUNDA edicion de la MISMA nota llega antes de que ese render cierre.
//   Al cerrar (MainComponent::finishBackendRender, no el sintetizador suelto):
//     (a) la nota conserva dirty = 1 y su revision es la de la segunda edicion;
//     (b) el pase siguiente la renderiza;
//     (c) el bloque coincide con el render completo del estado final:
//         max|diff| = 0, tolerancia 1e-7.
//
// El render del backend se simula igual que en el plugin: se copia el proyecto
// en la peticion, se renderiza la copia y al terminar se devuelve el audio a la
// copia viva del editor antes de llamar a finishBackendRender.
#include "../Source/Audio/EditorController.h"
#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Models/Project.h"
#include "../Source/UI/MainComponent.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/NotePitch.h"
#include "../Source/Utils/OnnxRuntime.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kHop = HOP_SIZE;
constexpr int kTotalFrames = 520;
// Bloque 1
constexpr int kN0a = 50, kN0b = 150;
constexpr int kN1a = 150, kN1b = 250;
// Silencio D-129
constexpr int kGapA = 250, kGapB = 290;
// Bloque 2
constexpr int kN2a = 290, kN2b = 390;
constexpr int kN3a = 390, kN3b = 470;

float midiHz(float midi) {
  return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

juce::AudioBuffer<float> makePhrase() {
  juce::AudioBuffer<float> buffer(1, kTotalFrames * kHop);
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float value = 0.0f;
    float midi = 0.0f;
    if (frame >= kN0a && frame < kN0b)      midi = 57.0f;
    else if (frame >= kN1a && frame < kN1b) midi = 59.0f;
    else if (frame >= kN2a && frame < kN2b) midi = 62.0f;
    else if (frame >= kN3a && frame < kN3b) midi = 64.0f;
    if (midi > 0.0f) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(midi) / kSampleRate;
      value = 0.30f * static_cast<float>(std::sin(phase)) +
              0.12f * static_cast<float>(std::sin(2.0 * phase));
    } else {
      phase = 0.0;
    }
    buffer.setSample(0, i, value);
  }
  return buffer;
}

Project makeProject(NoteEngine engine = NoteEngine::Psola) {
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
  const std::pair<int, int> spans[] = {{kN0a, kN0b}, {kN1a, kN1b},
                                       {kN2a, kN2b}, {kN3a, kN3b}};
  const float midis[] = {57.0f, 59.0f, 62.0f, 64.0f};
  for (int i = 0; i < 4; ++i) {
    for (int f = spans[i].first; f < spans[i].second; ++f) {
      audio.voicedMask[static_cast<size_t>(f)] = true;
      audio.denseF0[static_cast<size_t>(f)] = midiHz(midis[i]);
    }
    Note note(spans[i].first, spans[i].second, midis[i]);
    note.setOriginalMidiNote(midis[i]);
    note.setEngine(engine);
    note.setOriginalDeltaPitch(
        std::vector<float>(spans[i].second - spans[i].first, 0.0f));
    project.addNote(note);
  }
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
  synthesizer.synthesizeRegion(nullptr, [&](bool ok) {
    success.store(ok);
    finished.store(true);
  });
  const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  while (!finished.load() &&
         juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  if (!finished.load()) {
    std::printf("%s: el render no termino en 30 s\n", label);
    return false;
  }
  return success.load();
}

Project fullRender(Project source, SynthesisEngineType engine, const char *label) {
  for (auto &note : source.getNotes())
    note.clearDirty();
  source.setF0DirtyRange(0, kTotalFrames);
  CHECK(renderAndWait(source, engine, label));
  return source;
}

float maxAbsDiff(const juce::AudioBuffer<float> &a,
                 const juce::AudioBuffer<float> &b, int firstFrame,
                 int lastFrame) {
  float worst = 0.0f;
  const int n =
      std::min({a.getNumSamples(), b.getNumSamples(), lastFrame * kHop});
  for (int i = std::max(0, firstFrame * kHop); i < n; ++i)
    worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
  return worst;
}

} // namespace

// Friend of MainComponent (see there).
struct MainComponentTestAccess {
  MainComponent &mc;

  static void wire(MainComponent &mc, EditorController &controller,
                   PitchUndoManager &history) {
    mc.editorController = &controller;
    mc.undoManager = &history;
    mc.pianoRoll.setProject(controller.getProject());
    mc.pianoRoll.setUndoManager(&history);
  }

  void serviceEditRender() { mc.serviceEditRender(); }
  void markRenderDirty() { mc.markRenderDirty(); }
  void flushDirtyBlocks() { mc.flushDirtyBlocks(); }
  void bypassDebounce() {
    mc.editRenderRequestedAtMs = juce::Time::getMillisecondCounterHiRes() - 1000.0;
  }
  bool editRenderInFlight() const { return mc.renderState.editRenderInFlight; }
  bool flushChainActive() const { return mc.renderState.flushChainActive; }
  bool deferredRenderPending() const { return mc.renderState.deferredRenderPending; }
  RenderState::Phase phase() const { return mc.renderState.phase(); }
};

namespace {

// Simula el backend del plugin: copia el proyecto en la peticion, lo renderiza
// y al terminar devuelve el audio a la copia viva antes de cerrar el render.
struct BackendSim {
  std::vector<std::unique_ptr<Project>> renderedCopies;
  std::vector<std::unique_ptr<IncrementalSynthesizer>> synthesizers;
  std::function<void(bool)> finish; // plugin: finishBackendRender
  int requests = 0;
  bool done = false;
  bool success = true;

  void request(const Project &source, Project &live) {
    ++requests;
    done = false;
    renderedCopies.push_back(std::make_unique<Project>(source));
    auto *copy = renderedCopies.back().get();
    synthesizers.push_back(std::make_unique<IncrementalSynthesizer>());
    auto *synth = synthesizers.back().get();
    synth->setProject(copy);
    synth->setSynthesisEngine(SynthesisEngineType::Psola);
    synth->synthesizeRegion(
        nullptr,
        [this, copy, &live](bool ok) {
          juce::MessageManager::callAsync([this, copy, &live, ok]() {
            if (ok)
              live.getAudioData().waveform.makeCopyOf(
                  copy->getAudioData().waveform);
            success = ok;
            done = true;
            if (finish)
              finish(ok);
          });
        });
  }

  void pumpUntilDone() {
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
    while (!done && juce::Time::getMillisecondCounterHiRes() < deadline)
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    CHECK(done);
    CHECK(success);
  }
};

} // namespace

// D-285: backend que cierra con el resultado que se le diga. Sirve para medir
// que un cierre fallido suelta la cadena y deja pedir otro render.
struct FailingBackend {
  std::function<void(bool)> finish;
  int requests = 0;
  bool fail = true;

  void request(const Project &) {
    ++requests;
    const bool ok = !fail;
    juce::MessageManager::callAsync([this, ok]() {
      if (finish)
        finish(ok);
    });
  }

  void pumpUntil(int wantedRequests) {
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
    while (requests < wantedRequests &&
           juce::Time::getMillisecondCounterHiRes() < deadline)
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    // Deja correr el cierre pendiente de la ultima peticion.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  }
};

// Test
namespace {
void testS4dCloseKeepsSecondEdit() {
  CHECK(OnnxRuntime::useLinkedRuntimeForTesting());

  EditorController controller(false);
  PitchUndoManager history(100);
  MainComponent mc(false); // plugin: sin dispositivo de audio
  *controller.getProject() = makeProject(NoteEngine::Psola);
  MainComponentTestAccess::wire(mc, controller, history);
  MainComponentTestAccess access{mc};

  BackendSim backend;
  backend.finish = [&mc](bool ok) { mc.finishBackendRender(ok); };
  mc.setOnRequestBackendRender([&backend, &live = *controller.getProject()](
                                   const Project &source) {
    backend.request(source, live);
  });

  auto &project = *controller.getProject();
  auto &notes = project.getNotes();

  // Primera edicion del bloque B: notas 2 y 3.
  notes[2].setPitchOffset(-1.5f);
  notes[2].markDirty();
  notes[2].markSynthDirty();
  notes[3].setPitchOffset(-1.0f);
  notes[3].markDirty();
  notes[3].markSynthDirty();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN2a, kN3b);
  access.markRenderDirty();

  // Politica A: se sirve el render (debounce ya vencido).
  access.bypassDebounce();
  access.serviceEditRender();
  CHECK(backend.requests == 1);
  CHECK(access.editRenderInFlight());

  // Segunda edicion de la MISMA nota 2 mientras ese render esta en vuelo.
  notes[2].setPitchOffset(-1.7f);
  notes[2].markDirty();
  notes[2].markSynthDirty();
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(kN2a, kN3b);
  access.markRenderDirty();
  const auto secondRevision = notes[2].getDirtyRevision();

  // El backend termina y el plugin cierra el render. La segunda edicion tiene
  // que seguir sucia y con su revision justo despues de ese cierre.
  int closes = 0;
  bool dirtyAfterClose = false;
  bool revisionAfterClose = false;
  backend.finish = [&](bool ok) {
    ++closes;
    mc.finishBackendRender(ok);
    if (closes == 1) {
      dirtyAfterClose = project.getNotes()[2].isDirty();
      revisionAfterClose = project.getNotes()[2].getDirtyRevision() ==
                           secondRevision;
    }
  };
  backend.pumpUntilDone();

  std::printf("S4d cierre del plugin: nota 2 dirty %d (esperado 1) | "
              "revision %s (esperado la de la segunda edicion)\n",
              dirtyAfterClose ? 1 : 0,
              revisionAfterClose ? "coincide" : "NO coincide");
  CHECK(dirtyAfterClose);
  CHECK(revisionAfterClose);

  // Pase siguiente: la segunda edicion se renderiza y el cierre la limpia.
  std::printf("S4d pases de render: %d (>= 2)\n", backend.requests);
  CHECK(backend.requests >= 2);
  CHECK(closes >= 2);

  const bool stillDirty = project.getNotes()[2].isDirty();
  std::printf("S4d tras el segundo cierre: nota 2 dirty %d (esperado 0)\n",
              stillDirty ? 1 : 0);
  CHECK(!stillDirty);

  // Referencia: render completo del estado final.
  Project reference = makeProject(NoteEngine::Psola);
  auto &refNotes = reference.getNotes();
  refNotes[2].setPitchOffset(-1.7f);
  refNotes[3].setPitchOffset(-1.0f);
  PitchCurveProcessor::rebuildBaseFromNotes(reference);
  Project full = fullRender(reference, SynthesisEngineType::Psola, "S4d completo");

  const float diff = maxAbsDiff(project.getAudioData().waveform,
                                full.getAudioData().waveform, kN2a + 5,
                                kN3b - 5);
  std::printf("S4d bloque B vs render completo: %.8f (lim 1e-7) | nota 2 dirty "
              "%d\n",
              diff, project.getNotes()[2].isDirty() ? 1 : 0);
  CHECK(diff <= 1.0e-7f);
  CHECK(!project.getNotes()[2].isDirty());
}
} // namespace

namespace {
// D-285: un cierre fallido no puede clavar la cadena. Antes `finishBackendRender`
// se saltaba entero el desmontaje con `success == false` y el siguiente cambio
// de motor ya no tenia quien renderizase: la region quedaba muda hasta quitar y
// reinsertar el plugin.
void testFailedCloseReleasesChainAndRecovers() {
  EditorController controller(false);
  PitchUndoManager history(100);
  MainComponent mc(false); // plugin: sin dispositivo de audio
  *controller.getProject() = makeProject(NoteEngine::Psola);
  MainComponentTestAccess::wire(mc, controller, history);
  MainComponentTestAccess access{mc};

  FailingBackend backend;
  backend.finish = [&mc](bool ok) { mc.finishBackendRender(ok); };
  mc.setOnRequestBackendRender(
      [&backend](const Project &source) { backend.request(source); });

  auto &project = *controller.getProject();
  project.getNotes()[0].markDirty();
  project.getNotes()[0].markSynthDirty();
  access.markRenderDirty();

  // Cadena de Play: un solo bloque sucio.
  access.flushDirtyBlocks();
  CHECK(access.flushChainActive());
  CHECK(backend.requests == 1);

  // La primera peticion cierra con `false`: la cadena se suelta y se re-arma un
  // intento. El segundo `false` agota el presupuesto (uno por revision).
  backend.pumpUntil(2);

  std::printf("D-285 cierre fallido: peticiones %d (esperado 2) | cadena %d "
              "(esperado 0) | en vuelo %d (esperado 0) | fase %s\n",
              backend.requests, access.flushChainActive() ? 1 : 0,
              access.editRenderInFlight() ? 1 : 0,
              RenderState::phaseName(access.phase()));
  CHECK(backend.requests == 2);
  CHECK(!access.flushChainActive());
  CHECK(!access.editRenderInFlight());
  CHECK(access.phase() != RenderState::Phase::Rendering);
  // No se limpio nada: el trabajo sigue pendiente para la siguiente edicion.
  CHECK(project.getNotes()[0].isDirty());
  CHECK(access.deferredRenderPending());

  // Recuperacion sin reinsertar el plugin: un cambio de motor posterior (aqui,
  // otra edicion con el mismo camino) pide y completa un render.
  backend.fail = false;
  access.markRenderDirty();
  access.bypassDebounce();
  access.serviceEditRender();
  CHECK(access.editRenderInFlight());
  CHECK(backend.requests == 3);
  backend.pumpUntil(4);
  std::printf("D-285 recuperacion: nota 0 dirty %d (esperado 0) | en vuelo %d "
              "(esperado 0)\n",
              project.getNotes()[0].isDirty() ? 1 : 0,
              access.editRenderInFlight() ? 1 : 0);
  CHECK(!access.editRenderInFlight());
  CHECK(!project.getNotes()[0].isDirty());
}

// D-285: «no hay nada que renderizar» es exito, no fallo. Antes ese `false`
// envenenaba la cadena de cierre y mostraba «Render fallo» sin fallo alguno.
void testCleanProjectIsNoOpSuccess() {
  EditorController controller(false);
  auto &project = *controller.getProject();
  project = makeProject(NoteEngine::Psola);
  project.clearAllDirty();
  CHECK(!project.hasDirtyNotes());
  CHECK(!project.hasF0DirtyRange());

  std::atomic<bool> pendingRerun{false};
  bool called = false;
  bool reported = false;
  controller.resynthesizeIncrementalAsync(
      project, nullptr,
      [&](bool ok) {
        called = true;
        reported = ok;
      },
      pendingRerun, /*isPluginMode*/ false);

  std::printf("D-285 proyecto limpio: onComplete %d (esperado 1) | valor %d "
              "(esperado 1)\n",
              called ? 1 : 0, reported ? 1 : 0);
  CHECK(called);
  CHECK(reported);
}
} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  testS4dCloseKeepsSecondEdit();
  testFailedCloseReleasesChainAndRecovers();
  testCleanProjectIsNoOpSuccess();
  std::printf("EditRenderCloseTests: en verde\n");
  return 0;
}
