// M4 v1 / D-242: cuantizacion de tiempo al grid.
//
// Criterio (plan/m4/criterio-cuantizacion-tiempo.md):
//   S0 control negativo: fuerza 0 => identidad exacta.
//   S1 exactitud: fuerza 100 => cada frontera seleccionada cae en la rejilla.
//   S3 contiguedad: una frontera compartida sigue compartida.
//   Guardas: no colapsar una nota ni crear un solape nuevo.
//
// Las rejillas se expresan en frames (double) para no depender de SAMPLE_RATE.

#include "../Source/Utils/TimingQuantize.h"
#include "TestAssert.h"

#include <cstdio>
#include <vector>

namespace {

using timingQuantize::Span;

bool same (const std::vector<Span>& a, const std::vector<Span>& b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].startFrame != b[i].startFrame ||
        a[i].endFrame != b[i].endFrame || a[i].selected != b[i].selected)
      return false;
  return true;
}

} // namespace

int main()
{
  const std::vector<Span> one {{23, 38, true}};
  std::vector<Span> out;

  // S0: fuerza 0 es la identidad bit a bit.
  CHECK (timingQuantize::quantizeSpans (one, 10.0, 0, out));
  CHECK (same (one, out));
  const auto identity = out;

  // S1: fuerza 100 lleva inicio y fin a la rejilla de 10 frames.
  CHECK (timingQuantize::quantizeSpans (one, 10.0, 100, out));
  CHECK (out[0].startFrame == 20);
  CHECK (out[0].endFrame == 40);

  // Fuerza 50: punto medio redondeado (23 -> 21,5 -> 22; 38 -> 39).
  CHECK (timingQuantize::quantizeSpans (one, 10.0, 50, out));
  CHECK (out[0].startFrame == 22);
  CHECK (out[0].endFrame == 39);

  // S3: dos notas contiguas compartidas mantienen la frontera.
  {
    const std::vector<Span> pair {{0, 25, true}, {25, 47, true}};
    CHECK (timingQuantize::quantizeSpans (pair, 10.0, 100, out));
    CHECK (out[0].endFrame == 30);
    CHECK (out[1].startFrame == 30);
    CHECK (out[0].endFrame == out[1].startFrame);
  }

  // Una nota NO seleccionada pero que comparte frontera con una seleccionada
  // tambien se mueve (frontera compartida), y solo en esa frontera.
  {
    const std::vector<Span> pair {{0, 25, false}, {25, 47, true}};
    CHECK (timingQuantize::quantizeSpans (pair, 10.0, 100, out));
    CHECK (out[0].startFrame == 0);   // su otro borde no se toca
    CHECK (out[0].endFrame == 30);    // frontera compartida
    CHECK (out[1].startFrame == 30);
    CHECK (out[1].endFrame == 50);    // borde seleccionado
  }

  // Ya en la rejilla: sin cambios.
  {
    const std::vector<Span> onGrid {{20, 40, true}};
    CHECK (timingQuantize::quantizeSpans (onGrid, 10.0, 100, out));
    CHECK (same (onGrid, out));
  }

  // Guarda: la rejilla no puede colapsar una nota (0..4 con rejilla 10:
  // los dos bordes caen en 0).
  {
    const std::vector<Span> tiny {{0, 4, true}};
    CHECK (! timingQuantize::quantizeSpans (tiny, 10.0, 100, out));
  }

  // Guarda: no se admite un solape nuevo al cuantizar un borde suelto.
  // A [0,12] seleccionada, B [20,30] no seleccionada: el fin de A subiria a
  // 20 y tocaria a B, que empieza en 20; se acepta el toque (no es solape),
  // pero 12 -> 20 deja a A con 12 frames y no colapsa.
  {
    const std::vector<Span> pair {{0, 12, true}, {20, 30, false}};
    CHECK (timingQuantize::quantizeSpans (pair, 10.0, 100, out));
    CHECK (out[0].endFrame == 10);
    CHECK (out[1].startFrame == 20);
  }

  // Solape nuevo: A [0,18] seleccionada, B [20,30]; 18 -> 20 toca, 25 no;
  // con rejilla 7 el fin de A (18) sube a 21 y se solapa con B (20).
  {
    const std::vector<Span> pair {{0, 18, true}, {20, 30, false}};
    CHECK (! timingQuantize::quantizeSpans (pair, 7.0, 100, out));
  }

  // La rejilla <= 0 y la fuerza negativa no tocan nada.
  CHECK (timingQuantize::quantizeSpans (one, 0.0, 100, out));
  CHECK (same (one, out));
  CHECK (timingQuantize::quantizeSpans (one, 10.0, -50, out));
  CHECK (same (one, out));

  // Identidad explicita conservada tras todas las pruebas.
  CHECK (same (one, identity));

  std::printf ("M4 cuantizacion: S0 identidad, S1 -> rejilla, S3 contiguedad, "
               "guardas OK\n");
  return 0;
}
