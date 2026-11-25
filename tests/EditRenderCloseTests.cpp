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
  bool flushForHostPlay() { return mc.flushDirtyBlocksForHostPlay(); }
  // D-287
  void catchUpStaleAudio() { mc.catchUpStaleAudio(); }
  std::uint64_t publishedRevision() const { return mc.publishedRevision; }
  std::uint64_t dirtyRevision() const { return mc.dirtyRevision; }
  bool audioStale() const { return mc.publishedRevision < mc.dirtyRevision; }
  void simulateLostPass(std::uint64_t published, std::uint64_t dirty) {
    mc.publishedRevision = published;
    mc.dirtyRevision = dirty;
    mc.lastCatchUpRevision = 0;
  }
  // D-288
  int applyNoteEngineToIndices(const std::vector<int> &indices, NoteEngine engine) {
    return mc.applyNoteEngineToIndices(indices, engine);
  }
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
// D-288: con el transporte sonando no se cambia el motor. Un cambio de motor es
// un re-render de todo el proyecto y la publicacion es por bloque: medido en
// vivo, con Play el primer bloque entro en Pitchcore y el resto siguio en
// Vocoder durante un rato. La regla se comprueba en la puerta de atras (el
// mismo camino que el MCP y el menu), no solo deshabilitando el rail.
void testEngineChangeBlockedWhilePlaying() {
  EditorController controller(false);
  PitchUndoManager history(100);
  MainComponent mc(false);
  *controller.getProject() = makeProject(NoteEngine::Psola);
  MainComponentTestAccess::wire(mc, controller, history);
  MainComponentTestAccess access{mc};

  auto &notes = controller.getProject()->getNotes();
  CHECK(notes[0].getEngine() == NoteEngine::Psola);

  mc.updateHostPlaybackState(true);
  access.applyNoteEngineToIndices({0}, NoteEngine::Vocoder);
  const bool blocked = notes[0].getEngine() == NoteEngine::Psola;
  std::printf("D-288 motor con Play: sin cambio %d (esperado 1)\n",
              blocked ? 1 : 0);
  CHECK(blocked);

  mc.updateHostPlaybackState(false);
  access.applyNoteEngineToIndices({0}, NoteEngine::Vocoder);
  const bool applied = notes[0].getEngine() == NoteEngine::Vocoder;
  std::printf("D-288 motor parado: aplicado %d (esperado 1)\n",
              applied ? 1 : 0);
  CHECK(applied);
}

// D-289 (candidato): un cambio de motor GLOBAL tiene que re-renderizar TODOS los
// bloques, no solo los primeros. Medido en vivo: con «seguir global» en todas las
// notas, el usuario oia «las 10 primeras notas con el motor nuevo y el resto con
// el anterior». El recorrido D-197 pide un bloque por pase; si se corta, el
// cierre limpia la instantanea entera (todas las notas de la misma revision) y
// el resto se queda con el motor viejo para siempre.
namespace {
Project makeThreeBlockProject() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = kSampleRate;
  const int frames = 560;
  juce::AudioBuffer<float> buffer(1, frames * kHop);
  struct Span { int a, b; float midi; };
  const Span spans[] = {{40, 120, 57.0f}, {120, 200, 59.0f},   // bloque 1
                        {240, 320, 62.0f},                    // bloque 2
                        {360, 440, 64.0f}};                   // bloque 3
  audio.melSpectrogram.assign(static_cast<size_t>(frames),
                              std::vector<float>(NUM_MELS, 1.0f));
  audio.denseF0.assign(static_cast<size_t>(frames), 0.0f);
  audio.voicedMask.assign(static_cast<size_t>(frames), false);
  for (const auto &span : spans) {
    for (int f = span.a; f < span.b; ++f) {
      audio.voicedMask[static_cast<size_t>(f)] = true;
      audio.denseF0[static_cast<size_t>(f)] = midiHz(span.midi);
    }
    Note note(span.a, span.b, span.midi);
    note.setOriginalMidiNote(span.midi);
    note.setEngine(NoteEngine::Inherit);
    note.setPitchOffset(2.0f); // editada: entra en el plan de bloques
    note.setOriginalDeltaPitch(
        std::vector<float>(span.b - span.a, 0.0f));
    project.addNote(note);
  }
  double phase = 0.0;
  for (int i = 0; i < buffer.getNumSamples(); ++i) {
    const int frame = i / kHop;
    float midi = 0.0f;
    for (const auto &span : spans)
      if (frame >= span.a && frame < span.b)
        midi = span.midi;
    float value = 0.0f;
    if (midi > 0.0f) {
      phase += 2.0 * 3.14159265358979323846 * midiHz(midi) / kSampleRate;
      value = 0.30f * static_cast<float>(std::sin(phase));
    } else {
      phase = 0.0;
    }
    buffer.setSample(0, i, value);
  }
  audio.originalWaveform.makeCopyOf(buffer);
  audio.waveform.makeCopyOf(buffer);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  return project;
}

void testEngineChangeRendersEveryBlock() {
  EditorController controller(false);
  auto &project = *controller.getProject();
  project = makeThreeBlockProject();
  project.setSynthesisEngine(SynthesisEngineType::Psola);
  controller.setSynthesisEngineType(SynthesisEngineType::Psola);

  // Cambio de motor global: mismo camino que rerenderEditedNotes().
  const int marked = project.markEditedNotesDirty();
  CHECK(marked == 4);
  CHECK(project.hasEditedBlockPlan());

  std::atomic<bool> pendingRerun{false};
  bool done = false;
  bool ok = false;
  controller.resynthesizeIncrementalAsync(
      project, nullptr,
      [&](bool success) {
        done = true;
        ok = success;
      },
      pendingRerun, /*isPluginMode*/ false);
  const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  while (!done && juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  CHECK(done);
  CHECK(ok);

  // Referencia: el audio original (sin procesar). Cada bloque tiene notas con
  // +2 st, asi que un bloque renderizado TIENE que diferir del original; uno que
  // nunca se re-renderizo se queda igual (que es el fallo medido en vivo).
  const auto &rendered = project.getAudioData().waveform;
  const auto &original = project.getAudioData().originalWaveform;
  const float d1 = maxAbsDiff(rendered, original, 45, 195);
  const float d2 = maxAbsDiff(rendered, original, 245, 315);
  const float d3 = maxAbsDiff(rendered, original, 365, 435);
  std::printf("D-289 bloques 1/2/3 vs original: %.8f %.8f %.8f (lim > 1e-3)\n",
              d1, d2, d3);
  CHECK(d1 > 1.0e-3f);
  CHECK(d2 > 1.0e-3f);
  CHECK(d3 > 1.0e-3f);
}
} // namespace

// D-290: un cierre que vuelve a pedir pase sin que nada avance no puede
// re-lanzarse sin fin. Medido en vivo: con el Vocoder y «seguir global», Play
// quedaba retenido para siempre (phase=Rendering, generation subiendo) y el
// transporte solo arrancaba con un segundo Play. El test usa un backend que
// cierra bien pero no limpia nada: la cadena tiene que soltarse en el tope y el
// transporte volver a sonar.
void testChainPassesAreBounded() {
  EditorController controller(false);
  PitchUndoManager history(100);
  MainComponent mc(false);
  *controller.getProject() = makeProject(NoteEngine::Psola);
  MainComponentTestAccess::wire(mc, controller, history);
  MainComponentTestAccess access{mc};

  struct StuckBackend {
    std::function<void(bool)> finish;
    std::function<void()> beforeFinish; // re-marca sucio, como un pase que no cubre
    int requests = 0;
    void request(const Project &) {
      ++requests;
      juce::MessageManager::callAsync([this]() {
        if (beforeFinish) beforeFinish();
        if (finish) finish(true);
      });
    }
  } backend;
  backend.finish = [&mc](bool ok) { mc.finishBackendRender(ok); };

  auto &project = *controller.getProject();
  // Peor caso: cada cierre re-marca la nota (la revision sube en cada pase, asi
  // que el tope por revision no bastaria).
  backend.beforeFinish = [&project]() {
    project.getNotes()[0].markDirty();
    project.getNotes()[0].markSynthDirty();
  };
  mc.setOnRequestBackendRender(
      [&backend](const Project &source) { backend.request(source); });

  std::vector<bool> transport;
  mc.setOnRequestHostPlayState([&transport](bool play) { transport.push_back(play); });

  project.getNotes()[0].markDirty();
  project.getNotes()[0].markSynthDirty();
  access.markRenderDirty();

  const bool held = access.flushForHostPlay();
  CHECK(held);
  CHECK(access.flushChainActive());
  const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  while (access.flushChainActive() &&
         juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  const bool endsPlaying = !transport.empty() && transport.back();
  std::printf("D-290 cadena atascada: peticiones %d (<= 32) | cadena %d "
              "(esperado 0) | termina sonando %d (esperado 1)\n",
              backend.requests, access.flushChainActive() ? 1 : 0,
              endsPlaying ? 1 : 0);
  CHECK(backend.requests <= 32);
  CHECK(!access.flushChainActive());
  CHECK(endsPlaying);
}

// D-291: un pase con notas sucias no puede fallar porque el override del
// recorrido apunte a un rango que ya no cubre ninguna nota (override obsoleto).
// Antes: commit vacio -> onComplete(false) -> 3 reintentos agotados y el audio
// viejo; cada Play reintentaba. Ahora se commitea el tramo de cada nota sucia.
void testStaleOverrideStillCommits() {
  Project project = makeProject(NoteEngine::Psola);
  for (auto &note : project.getNotes()) {
    note.setPitchOffset(2.0f);
  }
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  for (auto &note : project.getNotes()) {
    note.markDirty();
    note.markSynthDirty();
  }
  project.markNoteEditDirtyRange(kN0a, kN3b);
  const auto before = project.getAudioData().waveform;

  IncrementalSynthesizer::resetFallbackCommitCount();
  IncrementalSynthesizer synth;
  synth.setProject(&project);
  synth.setSynthesisEngine(SynthesisEngineType::Psola);
  synth.setRenderRangeOverride(500, 520); // no cubre ninguna nota sucia

  std::atomic<bool> done{false};
  std::atomic<bool> ok{false};
  synth.synthesizeRegion(nullptr, [&](bool success) {
    ok.store(success);
    done.store(true);
  });
  const double deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  while (!done.load() &&
         juce::Time::getMillisecondCounterHiRes() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  CHECK(done.load());
  const float diff = maxAbsDiff(project.getAudioData().waveform, before, 55, 145);
  std::printf("D-291 override obsoleto: exito %d | fallback %d | bloque 1 vs "
              "antes %.6f (lim > 1e-3)\n",
              ok.load() ? 1 : 0,
              IncrementalSynthesizer::getFallbackCommitCount(), diff);
  CHECK(ok.load());
  CHECK(IncrementalSynthesizer::getFallbackCommitCount() > 0);
  CHECK(diff > 1.0e-3f);
}

// modelo. Medido en vivo: pass=44 < dirty=47, nada agendado ni en vuelo, notas
// limpias y el buffer aun con el motor anterior mientras el estado decia
// `Published`/`quiescent`. El test simula ese estado exacto y exige:
//   (a) el catch-up pide un render de lo editado (marca las notas sucias);
//   (b) no pide un segundo pase de la misma revision (presupuesto);
//   (c) tras un pase bueno, la revision publicada alcanza a la sucia;
//   (d) un cierre fallido no adelanta la revision publicada.
void testStaleAudioIsCaughtUpOnce() {
  EditorController controller(false);
  PitchUndoManager history(100);
  MainComponent mc(false);
  *controller.getProject() = makeProject(NoteEngine::Psola);
  MainComponentTestAccess::wire(mc, controller, history);
  MainComponentTestAccess access{mc};

  auto &project = *controller.getProject();
  project.clearAllDirty();

  // Estado del fallo en vivo: dos revisiones por detras y nada pendiente.
  access.simulateLostPass(44, 47);
  CHECK(access.audioStale());

  access.catchUpStaleAudio();
  const bool marked = project.hasDirtyNotes();
  std::printf("D-287 audio atrasado: notas marcadas %d (esperado 1) | "
              "publicada %llu | sucia %llu\n",
              marked ? 1 : 0,
              static_cast<unsigned long long>(access.publishedRevision()),
              static_cast<unsigned long long>(access.dirtyRevision()));
  CHECK(marked);
  CHECK(access.dirtyRevision() > 47); // el catch-up pidio su propio pase

  // Segundo intento sin que cambie nada: no vuelve a pedir.
  const auto dirtyBeforeSecond = access.dirtyRevision();
  access.catchUpStaleAudio();
  std::printf("D-287 segundo catch-up sin cambios: revision %llu (esperado %llu)\n",
              static_cast<unsigned long long>(access.dirtyRevision()),
              static_cast<unsigned long long>(dirtyBeforeSecond));
  CHECK(access.dirtyRevision() == dirtyBeforeSecond);

  // Cierre fallido: el audio no puede dar por buena la revision nueva.
  access.simulateLostPass(44, 48);
  mc.finishBackendRender(false);
  CHECK(access.audioStale());
  std::printf("D-287 cierre fallido: publicada %llu < sucia %llu (esperado si)\n",
              static_cast<unsigned long long>(access.publishedRevision()),
              static_cast<unsigned long long>(access.dirtyRevision()));
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

// D-285/2: un cierre sin instantanea (el relanzamiento con el mismo
// `onComplete`) no puede juzgar el trabajo pendiente. Antes dejaba
// `deferred_pending` clavado, la fase en Dirty para siempre y el Play se paraba
// a los ~100 ms en cada intento (medido en vivo).
void testCloseWithoutSnapshotDecidesNothing() {
  EditorController controller(false);
  PitchUndoManager history(100);
  MainComponent mc(false);
  *controller.getProject() = makeProject(NoteEngine::Psola);
  MainComponentTestAccess::wire(mc, controller, history);
  MainComponentTestAccess access{mc};

  FailingBackend backend;
  backend.fail = false;
  backend.finish = [&mc](bool ok) { mc.finishBackendRender(ok); };
  mc.setOnRequestBackendRender(
      [&backend](const Project &source) { backend.request(source); });

  auto &project = *controller.getProject();
  project.getNotes()[0].markDirty();
  project.getNotes()[0].markSynthDirty();
  access.markRenderDirty();
  access.bypassDebounce();
  access.serviceEditRender();
  backend.pumpUntil(1);
  CHECK(!access.editRenderInFlight());

  // El relanzamiento del EditorController entrega un segundo cierre que no
  // tiene instantanea: no limpia, no reintenta y no deja cola pendiente.
  mc.finishBackendRender(true);
  std::printf("D-285/2 cierre sin instantanea: fase %s | cola %d (esperado 0)\n",
              RenderState::phaseName(access.phase()),
              access.deferredRenderPending() ? 1 : 0);
  CHECK(!access.deferredRenderPending());
  CHECK(access.phase() != RenderState::Phase::Rendering);
  CHECK(access.phase() != RenderState::Phase::Dirty);
}

// D-285/2: el contrato del borde de Play no es «no parar nunca» sino «terminar
// sonando». Con un render que falla siempre, el Play para, intenta el flush y
// el cierre vuelve a arrancar el transporte con el ultimo audio publicado.
void testExhaustedRetryDoesNotStopPlay() {
  EditorController controller(false);
  PitchUndoManager history(100);
  MainComponent mc(false);
  *controller.getProject() = makeProject(NoteEngine::Psola);
  MainComponentTestAccess::wire(mc, controller, history);
  MainComponentTestAccess access{mc};

  FailingBackend backend; // falla siempre
  backend.finish = [&mc](bool ok) { mc.finishBackendRender(ok); };
  mc.setOnRequestBackendRender(
      [&backend](const Project &source) { backend.request(source); });

  std::vector<bool> transport;
  mc.setOnRequestHostPlayState([&transport](bool play) { transport.push_back(play); });

  auto &project = *controller.getProject();
  project.getNotes()[0].markDirty();
  project.getNotes()[0].markSynthDirty();
  access.markRenderDirty();

  access.flushDirtyBlocks();
  backend.pumpUntil(2); // pase + reintento, los dos fallidos
  CHECK(access.deferredRenderPending());

  // Un Play: se para para el flush y el cierre lo devuelve a sonar.
  const bool held = access.flushForHostPlay();
  backend.pumpUntil(backend.requests + 1);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(150);

  const bool endsPlaying = !transport.empty() && transport.back();
  std::printf("D-285/2 Play con render que falla: retenido %d | llamadas al "
              "transporte %d | termina sonando %d (esperado 1)\n",
              held ? 1 : 0, static_cast<int>(transport.size()),
              endsPlaying ? 1 : 0);
  CHECK(held);              // si hay trabajo, el Play espera al flush
  CHECK(endsPlaying);       // pero nunca se queda parado
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
  testEngineChangeBlockedWhilePlaying();
  testEngineChangeRendersEveryBlock();
  testChainPassesAreBounded();
  testStaleOverrideStillCommits();
  testStaleAudioIsCaughtUpOnce();
  testFailedCloseReleasesChainAndRecovers();
  testCloseWithoutSnapshotDecidesNothing();
  testExhaustedRetryDoesNotStopPlay();
  testCleanProjectIsNoOpSuccess();
  std::printf("EditRenderCloseTests: en verde\n");
  return 0;
}
