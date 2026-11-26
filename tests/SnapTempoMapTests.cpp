// R4 (D-317): el snap de la grilla pasa por el mapa de tempo del host.
//
// Se mide lo que hace el editor al soltar el ciclo o un borde con snap activo:
// project -> host -> quarter -> round a gridQuarterNotes -> host -> project.
// Con un tempo constante coincide con el snap en segundos; con dos tempos o un
// 3/4 solo coincide si la cuenta se hace en quarters.
#include "TestAssert.h"

#include "../Source/Audio/Engine/HostTempoMap.h"
#include "../Source/UI/PianoRoll/TimelineGridSnap.h"

#include <cmath>

namespace
{
bool near(double a, double b, double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

// 120 BPM: la negra son 0,5 s.
void testConstantQuarter()
{
    const HostTempoMap map = HostTempoMap::constant(120.0);

    CHECK(near(map.snapTimeToGrid(0.6, 1.0), 0.5));
    CHECK(near(map.snapTimeToGrid(0.9, 1.0), 1.0));
    CHECK(near(map.snapTimeToGrid(1.0, 1.0), 1.0));

    // Corchea (0,5 quarters = 0,25 s).
    CHECK(near(map.snapTimeToGrid(1.31, 0.5), 1.25));
    CHECK(near(map.snapTimeToGrid(1.39, 0.5), 1.5));

    // Sin grilla no toca el valor.
    CHECK(near(map.snapTimeToGrid(0.6, 0.0), 0.6));

    // Decimales por delante de cero: quarter negativo, sin sorpresa.
    CHECK(near(map.snapTimeToGrid(-0.6, 1.0), -0.5));
}

// Dos tempos: 240 BPM en 0..2 s (0..8 quarters), 120 BPM en 2..4 s (8..12).
void testTwoTempos()
{
    const HostTempoMap map = HostTempoMap::fromEntries(
        {{0.0, 0.0}, {2.0, 8.0}, {4.0, 12.0}}, {{0.0, 4, 4}});
    CHECK(map.isValid());
    CHECK(!map.isConstant());

    // 3,2 s = quarter 10,4 -> 10 -> 2,0 + 2 x 0,5 = 3,0 s.
    CHECK(near(map.snapTimeToGrid(3.2, 1.0), 3.0));
    // 4,4 s = quarter 12,8 -> 13 -> 4,0 + 0,5 = 4,5 s.
    CHECK(near(map.snapTimeToGrid(4.4, 1.0), 4.5));
    // 1,3 s = quarter 5,2 -> 5 -> 1,25 s (segmento rapido, 0,25 s/quarter).
    CHECK(near(map.snapTimeToGrid(1.3, 1.0), 1.25));
}

// 3/4 a 120 BPM: negra 0,5 s, compas 1,5 s.
void testThreeFour()
{
    const HostTempoMap map = HostTempoMap::constant(120.0, 3, 4);

    // Corchea dentro del compas: 0,6 s = 1,2 quarters -> 1 -> 0,5 s.
    CHECK(near(map.snapTimeToGrid(0.6, 0.5), 0.5));
    // La linea del compas 2 esta en 1,5 s.
    CHECK(near(map.snapTimeToGrid(1.55, 0.5), 1.5));

    // Grilla de compas entero: 1,4 s -> 1,5 s; 2,6 s = 5,2 quarters -> 6 -> 3,0 s.
    CHECK(near(map.snapTimeToGrid(1.4, 3.0), 1.5));
    CHECK(near(map.snapTimeToGrid(2.6, 3.0), 3.0));
}

// Un proyecto que empieza antes del cero del host (offset negativo): el snap
// sigue siendo el de la linea dibujada, sin recortar el offset a cero.
void testProjectOffsetWithMap()
{
    const HostTempoMap map = HostTempoMap::constant(120.0);

    // offset -6: el proyecto 0,6 s es host -5,4 s -> -5,5 s -> proyecto 0,5 s.
    CHECK(near(snapProjectSecondsToHostGrid(map, -6.0, 0.6, 1.0), 0.5));
    CHECK(near(snapProjectSecondsToHostGrid(map, -6.0, -0.4, 1.0), -0.5));

    // Con dos tempos y offset, la cuenta sigue siendo en quarters.
    const HostTempoMap two = HostTempoMap::fromEntries(
        {{0.0, 0.0}, {2.0, 8.0}, {4.0, 12.0}}, {{0.0, 4, 4}});
    // proyecto 0,2 s = host 2,2 s = quarter 8,4 -> 8 -> host 2,0 -> proyecto 0,0.
    CHECK(near(snapProjectSecondsToHostGrid(two, 2.0, 0.2, 1.0), 0.0));
}
} // namespace

int main()
{
    testConstantQuarter();
    testTwoTempos();
    testThreeFour();
    testProjectOffsetWithMap();
    return 0;
}
