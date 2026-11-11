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

} // namespace

int main() {
  std::cout << std::unitbuf;
  testMainViewHookExists();
  testPlayStateEdgeCallsTheHook();
  testStopFlushStartOrder();
  std::cout << "P1b fase 3 passed\n";
  return 0;
}
