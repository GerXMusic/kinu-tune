// P1b fase 3 - Play del host.
//
// En ARA no hay aviso de arranque: el borde se detecta en el callback de estado
// de PluginTransportController. Cuando ese borde llega con bloques sucios, el
// transporte tiene que pararse, vaciarse los bloques y reanudarse; sin sucios
// no puede haber ciclo stop/start. Eso no se puede ejecutar sin el host, asi
// que el test comprueba el invariante sobre el codigo real: el puente existe,
// se llama en el callback y su cuerpo hace stop -> flush -> start en ese orden.
#include "TestAssert.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

std::string windowAfter(const std::string &text, const std::string &anchor,
                        std::size_t length) {
  const auto pos = text.find(anchor);
  if (pos == std::string::npos)
    return {};
  return text.substr(pos, length);
}

void testMainViewHookExists() {
  const std::string view =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/IMainView.h");
  CHECK(!view.empty());
  CHECK(view.find("flushDirtyBlocksForHostPlay") != std::string::npos);
}

void testPlayStateEdgeCallsTheHook() {
  const std::string editor =
      readFile(std::string(P1B_SOURCE_DIR) + "/Plugin/PluginEditor.cpp");
  CHECK(!editor.empty());

  const auto callback = editor.find("setPlayStateCallback");
  CHECK(callback != std::string::npos);
  const std::string body =
      windowAfter(editor, "setPlayStateCallback", 900);
  std::cout << "play-state callback: " << body.substr(0, 400) << "\n";
  const auto hook = body.find("flushDirtyBlocksForHostPlay");
  const auto sync = body.find("updateHostPlaybackState(isPlaying)");
  CHECK(hook != std::string::npos);
  CHECK(sync != std::string::npos);
  CHECK(hook < sync); // the flush is asked for before the UI says "playing"
}

void testStopFlushStartOrder() {
  const std::string mainComponent =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/MainComponent.cpp");
  const std::string body = windowAfter(
      mainComponent, "MainComponent::flushDirtyBlocksForHostPlay", 1400);
  CHECK(!body.empty());
  const auto stop = body.find("onRequestHostPlayState(false)");
  const auto flush = body.find("flushDirtyBlocks()");
  const auto start = body.find("onRequestHostPlayState(true)");
  CHECK(stop != std::string::npos);
  CHECK(flush != std::string::npos);
  CHECK(start != std::string::npos);
  CHECK(stop < flush);
  CHECK(flush < start);
  // The dirty check must run before the stop: no dirty, no stop/start.
  const auto dirty = body.find("hasDirtyNotes()");
  CHECK(dirty != std::string::npos);
  CHECK(dirty < stop);
}

// D-285: los pestillos ya no son puertas de audio. Estas guardas de fuente no
// sustituyen a la escucha; fijan que el mute no vuelva por descuido.
void testNoRenderPendingMuteInPlaybackRenderer() {
  const std::string controller =
      readFile(std::string(P1B_SOURCE_DIR) + "/Plugin/ARADocumentController.cpp");
  CHECK(!controller.empty());
  // `renderPending` se queda como estado (lo escribe el procesador), pero el
  // playback renderer no lo consulta para silenciar.
  CHECK(controller.find("renderPending.load") == std::string::npos);
}

void testNoBackendFlushMuteInAudioProcessor() {
  const std::string processor =
      readFile(std::string(P1B_SOURCE_DIR) + "/Plugin/PluginProcessor.cpp");
  CHECK(!processor.empty());
  // `backendFlushActive` se queda como estado de reposo (D-266), no como puerta.
  CHECK(processor.find("backendFlushActive.load") == std::string::npos);
}

void testFinishBackendRenderAlwaysUnmountsChain() {
  const std::string mainComponent =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/MainComponent.cpp");
  const std::string body =
      windowAfter(mainComponent, "MainComponent::finishBackendRender", 3000);
  CHECK(!body.empty());
  // El desmontaje de la cadena no puede vivir dentro de `if (success ...)`.
  CHECK(body.find("if (success && flushSnapshotValid)") == std::string::npos);
  CHECK(body.find("flushChainActive = false") != std::string::npos);
}

// D-285 F2: el vocoder consulta la cancelacion entre trozos de 1024 frames, y el
// sintetizador le pasa la bandera del pase (antes solo la miraba antes de
// empezar: un cambio de motor esperaba al archivo entero).
void testVocoderCancelsBetweenChunks() {
  const std::string vocoder =
      readFile(std::string(P1B_SOURCE_DIR) + "/Audio/Vocoder.cpp");
  CHECK(!vocoder.empty());
  CHECK(vocoder.find("cancelled()") != std::string::npos);
  const std::string loop = windowAfter(vocoder, "Long input: chunked inference", 2200);
  CHECK(!loop.empty());
  CHECK(loop.find("isShuttingDown.load()") != std::string::npos);
  CHECK(loop.find("cancelled()") != std::string::npos);

  const std::string synth =
      readFile(std::string(P1B_SOURCE_DIR) + "/Audio/Synthesis/IncrementalSynthesizer.cpp");
  CHECK(!synth.empty());
  // El camino de un solo bloque es el que antes no pasaba la bandera; el de
  // varios bloques ya la pasaba con otro nombre, asi que se ancla en la llamada
  // de un bloque (`std::move(melRange)`).
  const std::string call =
      windowAfter(synth, "vocoder->inferAsync(std::move(melRange)", 300);
  CHECK(!call.empty());
  CHECK(call.find("capturedCancelFlag") != std::string::npos);
}

} // namespace

int main() {
  std::cout << std::unitbuf;
  testMainViewHookExists();
  testPlayStateEdgeCallsTheHook();
  testStopFlushStartOrder();
  testNoRenderPendingMuteInPlaybackRenderer();
  testNoBackendFlushMuteInAudioProcessor();
  testFinishBackendRenderAlwaysUnmountsChain();
  testVocoderCancelsBetweenChunks();
  std::cout << "P1b fase 3 passed\n";
  return 0;
}
