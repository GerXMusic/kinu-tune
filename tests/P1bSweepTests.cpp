// P1b fase 6 - indicador de barrido.
//
// El barrido se dibuja desde la cola de bloques pendientes: que bloque se esta
// renderizando y cuantos quedan. Sin cola no hay barrido. El estado vive en
// sweep::fromQueue (Source/UI/Main/DeferredRenderQueue.h) y el renderer del
// blob (NoteRenderer) lo recibe ya resuelto; el invariante comprueba ademas
// que MainComponent tiene la cola como miembro y que la alimenta al marcar.
#include "../Source/UI/Main/DeferredRenderQueue.h"
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

// Sin bloques pendientes: el indicador no se dibuja.
void testNoQueueNoSweep() {
  DeferredRenderQueue queue;
  const auto state = sweep::fromQueue(queue);
  CHECK(!state.active);
  CHECK(state.remaining == 0);
  CHECK(state.blockEnd == state.blockStart);
}

// Con un bloque pendiente: barrido activo, con ese bloque y 1 restante.
void testPendingBlockIsSwept() {
  DeferredRenderQueue queue;
  queue.markBlock(120, 260);
  const auto state = sweep::fromQueue(queue);
  CHECK(state.active);
  CHECK(state.remaining == 1);
  CHECK(state.blockStart == 120);
  CHECK(state.blockEnd == 260);
}

// Varios bloques: se barre el primero de la cola y se cuenta el resto.
void testRemainingCount() {
  DeferredRenderQueue queue;
  queue.markBlock(0, 80);
  queue.markBlock(400, 480);
  queue.markBlock(900, 980);
  auto state = sweep::fromQueue(queue);
  CHECK(state.active);
  CHECK(state.remaining == 3);
  CHECK(state.blockStart == 0);
  CHECK(state.blockEnd == 80);

  // Al vaciarse la cola el barrido desaparece.
  int rendered = 0;
  queue.flush([&rendered](int, int) { ++rendered; });
  CHECK(rendered == 3);
  state = sweep::fromQueue(queue);
  CHECK(!state.active);
  CHECK(state.remaining == 0);
}

// El borde brillante avanza en ciclos de 0 a 1 y vuelve a empezar.
void testPhaseCycles() {
  CHECK(sweep::phaseAt(0.0) == 0.0f);
  CHECK(sweep::phaseAt(0.45) > 0.4f);
  CHECK(sweep::phaseAt(0.45) < 0.6f);
  CHECK(sweep::phaseAt(0.9) < 0.01f);      // one full period
  CHECK(sweep::phaseAt(1.8) < 0.01f);      // two periods
  CHECK(sweep::phaseAt(-0.45) > 0.4f);     // negative time wraps forward
  CHECK(sweep::phaseAt(0.45, 0.0) == 0.0f); // degenerate period
}

std::string windowAfter(const std::string &text, const std::string &anchor,
                        std::size_t length) {
  const auto pos = text.find(anchor);
  if (pos == std::string::npos)
    return {};
  return text.substr(pos, length);
}

// Invariante sobre el codigo real: la cola es miembro de MainComponent, se
// alimenta al marcar, y el renderer del blob recibe el barrido.
void testWiring() {
  const std::string mainHeader =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/MainComponent.h");
  CHECK(windowAfter(mainHeader, "void markRenderDirty();", 2400)
            .find("DeferredRenderQueue deferredRenders;") != std::string::npos);

  const std::string mainSource =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/MainComponent.cpp");
  const std::string mark =
      windowAfter(mainSource, "void MainComponent::markRenderDirty()", 900);
  CHECK(!mark.empty());
  std::cout << "markRenderDirty: " << mark.substr(0, 260) << "\n";
  CHECK(mark.find("deferredRenders.markBlock(") != std::string::npos);
  CHECK(mark.find("setSweepState(") != std::string::npos);

  const std::string noteRenderer =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/PianoRoll/NoteRenderer.h");
  CHECK(noteRenderer.find("setSweepState(") != std::string::npos);

  const std::string noteRendererBody =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/PianoRoll/NoteRenderer.cpp");
  CHECK(windowAfter(noteRendererBody, "void NoteRenderer::draw(", 1600)
            .find("sweepActive") != std::string::npos);

  const std::string pianoRoll =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/PianoRollComponent.h");
  CHECK(pianoRoll.find("setSweepState(") != std::string::npos);
}

} // namespace

int main() {
  std::cout << std::unitbuf;
  testNoQueueNoSweep();
  testPendingBlockIsSwept();
  testRemainingCount();
  testPhaseCycles();
  testWiring();
  std::cout << "P1b fase 6 passed\n";
  return 0;
}
