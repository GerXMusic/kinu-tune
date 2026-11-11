// P1b (render al Play) - fases 1 y 2.
//
// Grupo "deferred": la cola de bloques sucios. Cierra los cuatro criterios de
// P1b: editar -> 0 renders; Play con bloques sucios -> 1 render por bloque;
// Play sin ediciones -> 0; undo -> 0 hasta Play.
//
// Grupo "wiring": invariantes sobre el codigo real de MainComponent.cpp y del
// historial. Editar y undo no pueden volver a llamar a resynthesizeIncremental,
// y el Play de la UI tiene que vaciar los bloques sucios antes de arrancar.
#include "../Source/UI/Main/DeferredRenderQueue.h"
#include "TestAssert.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// Counts renders instead of producing audio: what the queue promises is the
// number of render calls, not their content.
struct RenderCounter {
  int renders = 0;
  std::vector<std::pair<int, int>> rendered;
  void operator()(int startFrame, int endFrame) {
    ++renders;
    rendered.emplace_back(startFrame, endFrame);
  }
};

std::string readFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

int countOccurrences(const std::string &haystack, const std::string &needle) {
  if (needle.empty())
    return 0;
  int count = 0;
  for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + needle.size()))
    ++count;
  return count;
}

// A single edit: mark the block it touched, render nothing.
void testEditRendersNothing() {
  DeferredRenderQueue queue;
  RenderCounter counter;
  queue.markBlock(100, 200); // pitch edit finished
  CHECK(queue.hasDirty());
  CHECK(counter.renders == 0);
  CHECK(queue.dirtyBlockCount() == 1);
}

// Play with dirty blocks: one render per block, no more, no fewer.
void testPlayRendersOnePerDirtyBlock() {
  DeferredRenderQueue queue;
  RenderCounter counter;
  queue.markBlock(0, 80);
  queue.markBlock(400, 480);
  queue.markBlock(900, 980);
  CHECK(counter.renders == 0);
  const int flushed = queue.flush(counter);
  CHECK(flushed == 3);
  CHECK(counter.renders == 3);
  CHECK(!queue.hasDirty());
}

// Two edits inside the same block cost one render, not two.
void testEditsInOneBlockCoalesce() {
  DeferredRenderQueue queue;
  RenderCounter counter;
  queue.markBlock(100, 160);
  queue.markBlock(120, 200);
  queue.markBlock(180, 220);
  CHECK(queue.dirtyBlockCount() == 1);
  queue.flush(counter);
  CHECK(counter.renders == 1);
  CHECK(counter.rendered[0].first == 100);
  CHECK(counter.rendered[0].second == 220);
}

// Play without edits: nothing queued, nothing rendered.
void testPlayWithoutEditsRendersNothing() {
  DeferredRenderQueue queue;
  RenderCounter counter;
  CHECK(!queue.hasDirty());
  const int flushed = queue.flush(counter);
  CHECK(flushed == 0);
  CHECK(counter.renders == 0);
}

// Undo also only marks: it stays at 0 renders until Play.
void testUndoRendersNothingUntilPlay() {
  DeferredRenderQueue queue;
  RenderCounter counter;
  queue.markBlock(100, 200); // edit
  queue.flush(counter);
  CHECK(counter.renders == 1);
  queue.markBlock(100, 200); // undo of that edit
  CHECK(counter.renders == 1);
  queue.flush(counter); // play
  CHECK(counter.renders == 2);
}

// An empty range is not a block.
void testEmptyRangeIgnored() {
  DeferredRenderQueue queue;
  RenderCounter counter;
  queue.markBlock(50, 50);
  queue.markBlock(80, 40);
  CHECK(!queue.hasDirty());
  CHECK(queue.flush(counter) == 0);
}

void testSourceWiring() {
  const std::string mainComponent =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/MainComponent.cpp");
  CHECK(!mainComponent.empty());

  // Phase 1: every editing trigger only marks. The flush primitive is the
  // only remaining caller of resynthesizeIncremental.
  const int marks = countOccurrences(mainComponent, "markRenderDirty();");
  const int renders = countOccurrences(mainComponent, "resynthesizeIncremental();");
  std::cout << "markRenderDirty()=" << marks
            << " resynthesizeIncremental()=" << renders << "\n";
  CHECK(marks >= 7);
  CHECK(renders <= 2);

  // Phase 2: the UI Play flushes the dirty blocks before starting playback.
  const auto playHook = mainComponent.find("toolbar.onPlay");
  CHECK(playHook != std::string::npos);
  const std::string playBody = mainComponent.substr(playHook, 200);
  std::cout << "onPlay body: " << playBody << "\n";
  CHECK(playBody.find("flushDirtyBlocks()") != std::string::npos);
  CHECK(playBody.find("flushDirtyBlocks()") < playBody.find("play();"));

  // History: 100 steps, without renders of previous states.
  const std::string undoManager =
      readFile(std::string(P1B_SOURCE_DIR) + "/Undo/PitchUndoManager.h");
  CHECK(!undoManager.empty());
  CHECK(undoManager.find("maxHistory = 100") != std::string::npos);
}

} // namespace

int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  const std::string group = argc > 1 ? argv[1] : "deferred";
  if (group == "wiring") {
    testSourceWiring();
    std::cout << "P1b wiring passed\n";
    return 0;
  }
  testEditRendersNothing();
  testPlayRendersOnePerDirtyBlock();
  testEditsInOneBlockCoalesce();
  testPlayWithoutEditsRendersNothing();
  testUndoRendersNothingUntilPlay();
  testEmptyRangeIgnored();
  std::cout << "P1b deferred passed\n";
  return 0;
}
