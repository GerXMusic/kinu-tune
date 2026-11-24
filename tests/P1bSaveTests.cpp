// P1b fase 4 - guardar y cerrar.
//
// Con bloques sucios, saveProject tiene que vaciarlos antes de escribir el
// fichero; al terminar, no queda ningun bloque sucio que persistir. Si la
// espera no cabe en la misma llamada (el render es asincrono), el guardado se
// reanuda al terminar la cadena de flush en vez de escribir a medias.
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

void testSaveFlushesBeforeWriting() {
  const std::string mainComponent =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/MainComponent.cpp");
  CHECK(!mainComponent.empty());

  const std::string save = windowAfter(
      mainComponent, "void MainComponent::saveProject", 4200);
  CHECK(!save.empty());
  const auto flush = save.find("flushDirtyBlocks()");
  const auto write = save.find("ProjectSerializer::saveToFile");
  std::cout << "flush@" << (flush == std::string::npos ? -1 : (long)flush)
            << " write@" << (write == std::string::npos ? -1 : (long)write)
            << "\n";
  CHECK(flush != std::string::npos);
  CHECK(write != std::string::npos);
  CHECK(flush < write); // the render happens first, the file second
  // Nothing dirty reaches the file: the save only resumes once the flush ends.
  CHECK(save.find("resumeSaveAfterFlush") != std::string::npos);
}

void testFlushChainResumesTheSave() {
  const std::string mainComponent =
      readFile(std::string(P1B_SOURCE_DIR) + "/UI/MainComponent.cpp");
  // D-285: el ancla es el cierre del render del plugin, no la primera aparicion
  // de `flushChainActive = false` (que ahora esta tambien en el camino
  // standalone de un pase fallido).
  // D-290: la ventana crece con el tope de pases de la cadena (el cierre ahora
  // lleva la contabilidad D-290 antes del guardado diferido).
  const auto finish = windowAfter(
      mainComponent, "void MainComponent::finishBackendRender", 6200);
  CHECK(!finish.empty());
  CHECK(finish.find("saveProject(saveAsAfterFlush)") != std::string::npos);
  CHECK(mainComponent.find("resumeSaveAfterFlush = false") !=
        std::string::npos);
}

} // namespace

int main() {
  std::cout << std::unitbuf;
  testSaveFlushesBeforeWriting();
  testFlushChainResumesTheSave();
  std::cout << "P1b fase 4 passed\n";
  return 0;
}
