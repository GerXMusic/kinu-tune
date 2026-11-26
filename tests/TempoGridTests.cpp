// R3 (D-316): la grilla y la regla siguen el mapa de tempo del host.
//
// Se mide lo que dibuja el renderer: TempoGridView construye las lineas con
// HostTempoMap::forEachBar/forEachGridLine, ancladas al inicio de compas. Antes
// del marcador de tempo/compas las lineas no se mueven; despues si.
#include "TestAssert.h"

#include "../Source/Audio/Engine/HostTempoMap.h"
#include "../Source/UI/PianoRoll/TempoGridView.h"

#include <cmath>
#include <vector>

namespace
{
bool near(double a, double b, double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

struct Found
{
    std::vector<double> barTimes;
    std::vector<double> beatTimes;
    std::vector<double> subdivisionTimes;
    std::vector<int> barNumbers;
};

Found scan(const std::vector<TempoGridLine>& lines)
{
    Found found;
    for (const auto& line : lines)
    {
        switch (line.kind)
        {
        case HostTempoMap::GridLineKind::Bar:
            found.barTimes.push_back(line.hostSeconds);
            found.barNumbers.push_back(line.barNumber);
            break;
        case HostTempoMap::GridLineKind::Beat:
            found.beatTimes.push_back(line.hostSeconds);
            break;
        case HostTempoMap::GridLineKind::Subdivision:
            found.subdivisionTimes.push_back(line.hostSeconds);
            break;
        }
    }
    return found;
}

// 4/4 a 120 BPM: compases cada 4 quarters = 2 s, negras cada 0,5 s.
void testConstantFourFour()
{
    const HostTempoMap map = HostTempoMap::constant(120.0, 4, 4);
    CHECK(map.isValid());

    // Dos compases enteros: quarters 0..8 = 0..4 s.
    const auto lines = tempoGridLines(map, 0.0, 4.0, 0.25,
                                      {TempoGridLevel::Subdivision, 1});
    const Found found = scan(lines);

    CHECK(found.barTimes.size() == 3); // 0, 2 y 4 s
    CHECK(near(found.barTimes[0], 0.0));
    CHECK(near(found.barTimes[1], 2.0));
    CHECK(near(found.barTimes[2], 4.0));
    CHECK(found.barNumbers.size() == 3);
    CHECK(found.barNumbers[0] == 1);
    CHECK(found.barNumbers[1] == 2);
    CHECK(found.barNumbers[2] == 3);

    // Negras de los dos compases: 1,2,3 y 5,6,7 quarters.
    CHECK(found.beatTimes.size() == 6);
    CHECK(near(found.beatTimes[0], 0.5));
    CHECK(near(found.beatTimes[1], 1.0));
    CHECK(near(found.beatTimes[2], 1.5));

    // Subdivisiones de 1/16 entre negras, ancladas al compas.
    CHECK(found.subdivisionTimes.size() == 24); // 2 compases x 12
    CHECK(near(found.subdivisionTimes[0], 0.125)); // 0,25 quarter
    // Primera subdivision del compas 2: no cae en un limite de compas.
    bool hasFourAndQuarter = false;
    for (double time : found.subdivisionTimes)
        if (near(time, 2.125))
            hasFourAndQuarter = true;
    CHECK(hasFourAndQuarter);

    // Mixto bar/beat/subdivision en orden estricto.
    for (size_t i = 1; i < lines.size(); ++i)
        CHECK(lines[i].hostSeconds > lines[i - 1].hostSeconds);
}

// 3/4 desde el marcador 13,7044 s: antes no se mueve, despues si.
void testThreeFourFromMarker()
{
    // 16 quarters = 13,7044 s -> el compas 5 empieza justo en el marcador.
    const double slope = 13.7044 / 16.0;
    std::vector<HostTempoMap::TempoEntry> entries{{0.0, 0.0}, {slope, 1.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{0.0, 4, 4}, {16.0, 3, 4}};
    const HostTempoMap map =
        HostTempoMap::fromEntries(std::move(entries), std::move(sigs));
    CHECK(map.isValid());

    const auto lines = tempoGridLines(map, 0.0, 25.0 * slope, 1.0,
                                      {TempoGridLevel::Subdivision, 1});
    const Found found = scan(lines);

    // Compases 1..4 (4/4) a 4 quarters; desde el 5, a 3 quarters.
    const std::vector<double> expectedQuarters{0.0,  4.0,  8.0,  12.0, 16.0,
                                               19.0, 22.0, 25.0};
    CHECK(found.barTimes.size() == expectedQuarters.size());
    for (size_t i = 0; i < expectedQuarters.size(); ++i)
    {
        CHECK(near(found.barTimes[i], expectedQuarters[i] * slope, 1.0e-9));
        CHECK(found.barNumbers[i] == static_cast<int>(i) + 1);
    }

    // Antes del marcador cada compas mide 4 quarters; despues, 3.
    for (size_t i = 1; i <= 4; ++i)
        CHECK(near((found.barTimes[i] - found.barTimes[i - 1]) / slope, 4.0));
    CHECK(near((found.barTimes[5] - found.barTimes[4]) / slope, 3.0));
    CHECK(near((found.barTimes[6] - found.barTimes[5]) / slope, 3.0));

    // La linea del marcador es exactamente 13,7044 s.
    CHECK(near(found.barTimes[4], 13.7044, 1.0e-9));

    // Las negras del 3/4: 3 por compas (quarters 16+1 y 16+2).
    bool hasSeventeen = false;
    for (double time : found.beatTimes)
        if (near(time, 17.0 * slope))
            hasSeventeen = true;
    CHECK(hasSeventeen);

    // Antes del marcador, las lineas coinciden con las de un mapa constante
    // del mismo tempo: nada se ha movido.
    std::vector<HostTempoMap::TempoEntry> constEntries{{0.0, 0.0}, {slope, 1.0}};
    std::vector<HostTempoMap::BarSignature> constSigs{{0.0, 4, 4}};
    const HostTempoMap constant =
        HostTempoMap::fromEntries(std::move(constEntries), std::move(constSigs));
    const Found plain = scan(tempoGridLines(constant, 0.0, 10.0, 1.0,
                                            {TempoGridLevel::Subdivision, 1}));
    CHECK(plain.barTimes.size() >= 3);
    for (size_t i = 0; i < 3; ++i) // compases 1..3: antes del marcador
        CHECK(near(plain.barTimes[i], found.barTimes[i], 1.0e-9));
}

// 7/8: compas de 3,5 quarters y 7 corcheas; la grilla se ancla al compas.
void testSevenEight()
{
    const HostTempoMap map = HostTempoMap::constant(120.0, 7, 8);
    CHECK(near(map.barQuartersFor(map.getBarSignatures().front()), 3.5));

    // Negras del mapa: un compas con sus 7 corcheas (0..3,5 quarters).
    const Found beats =
        scan(tempoGridLines(map, 0.0, 1.749, 0.5, {TempoGridLevel::Beat, 1}));
    CHECK(beats.barTimes.size() == 1); // solo el compas 1
    CHECK(beats.beatTimes.size() == 6); // 7 pulsos = compas + 6 corcheas
    CHECK(near(beats.barTimes[0], 0.0));
    CHECK(near(beats.beatTimes[0], 0.25)); // primera corchea tras el compas
    CHECK(near(beats.beatTimes[5], 1.5));  // septimo pulso del compas 1

    // Con grilla de negra, las lineas caen en 3,5 + k, no en multiplos de 1
    // desde cero: prueba de que el ancla es el inicio de compas.
    const Found quarter =
        scan(tempoGridLines(map, 0.0, 7.0, 1.0, {TempoGridLevel::Subdivision, 1}));
    CHECK(quarter.barTimes.size() >= 2);
    CHECK(near(quarter.barTimes[1], 1.75));
    // Compas 2: quarters 3,5 / 4,5 / 5,5 / 6,5 -> 1,75 / 2,25 / 2,75 / 3,25 s.
    bool hasFourAndHalf = false;
    for (double time : quarter.beatTimes)
        if (near(time, 2.25))
            hasFourAndHalf = true;
    CHECK(hasFourAndHalf);
    // Y no hay linea en 4 quarters (2 s): no es un multiplo de la grilla
    // anclada al compas.
    for (double time : quarter.beatTimes)
        CHECK(!near(time, 2.0));
    CHECK(quarter.subdivisionTimes.empty()); // la negra ya es un pulso del 7/8
}

// Dos tempos: 120 BPM hasta 10 s, 60 BPM despues.
void testTwoTempos()
{
    std::vector<HostTempoMap::TempoEntry> entries{
        {0.0, 0.0}, {10.0, 20.0}, {14.0, 24.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{0.0, 4, 4}};
    const HostTempoMap map =
        HostTempoMap::fromEntries(std::move(entries), std::move(sigs));

    const Found found = scan(tempoGridLines(map, 0.0, 20.0, 1.0,
                                            {TempoGridLevel::Subdivision, 1}));
    // Compases: 0, 10 y 14 s... el compas 4 arranca en quarter 12 = 12 s.
    CHECK(found.barTimes.size() >= 5);
    CHECK(near(found.barTimes[0], 0.0));
    CHECK(near(found.barTimes[1], 2.0));
    CHECK(near(found.barTimes[2], 4.0));
    CHECK(near(found.barTimes[3], 6.0));
    CHECK(near(found.barTimes[4], 8.0));
    // A partir del marcador cada negra dura 1 s.
    bool barAtTen = false;
    for (double time : found.barTimes)
        if (near(time, 10.0))
            barAtTen = true;
    CHECK(barAtTen);
    bool hasEleven = false;
    for (double time : found.beatTimes)
        if (near(time, 11.0))
            hasEleven = true;
    CHECK(hasEleven);
    // Con el tempo viejo, quarter 21 habria caido en 10,5 s: no existe.
    for (double time : found.beatTimes)
        CHECK(!near(time, 10.5));
}

// Rampa: muchos puntos, las lineas no derivan.
void testRamp()
{
    std::vector<HostTempoMap::TempoEntry> entries{
        {0.0, 0.0}, {0.5, 2.0}, {1.0, 4.0}, {1.6, 6.0}, {2.3, 8.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{0.0, 4, 4}};
    const HostTempoMap map =
        HostTempoMap::fromEntries(std::move(entries), std::move(sigs));
    CHECK(map.isValid());

    const auto lines = tempoGridLines(map, -1.0, 4.0, 0.5,
                                      {TempoGridLevel::Subdivision, 1});
    CHECK(!lines.empty());
    double previous = -1.0e9;
    for (const auto& line : lines)
    {
        CHECK(line.hostSeconds > previous);
        previous = line.hostSeconds;
        // Cada linea vuelve a su quarter exacto.
        CHECK(near(map.quarterForTime(line.hostSeconds), line.quarter, 1.0e-9));
    }
}

// Adelgazamiento por el minimo seconds-per-quarter y fallback constante.
void testThinningAndFallback()
{
    const HostTempoMap map = HostTempoMap::constant(120.0, 4, 4); // 0,5 s/quarter

    // 1/32 (0,125 quarters) a 100 px/s: 6,25 px -> subdivisiones.
    CHECK(planTempoGrid(map, 0.0, 100.0, 0.125, 100.0, 4.0).level ==
          TempoGridLevel::Subdivision);
    // A 20 px/s: 1,25 px no llega; la negra da 10 px -> beats.
    CHECK(planTempoGrid(map, 0.0, 100.0, 0.125, 20.0, 4.0).level ==
          TempoGridLevel::Beat);
    // A 1 px/s la negra da 0,5 px y el compas 2 px: compases cada 2.
    const TempoGridPlan coarse = planTempoGrid(map, 0.0, 100.0, 0.125, 1.0, 4.0);
    CHECK(coarse.level == TempoGridLevel::Bar);
    CHECK(coarse.barStride == 2);

    // La regla solo rotula compases.
    const TempoGridPlan ruler = planTempoGridBars(map, 0.0, 100.0, 10.0, 28.0);
    CHECK(ruler.level == TempoGridLevel::Bar);
    CHECK(ruler.barStride == 2);

    // Mapa invalido: sin lineas; el llamante conserva la constante.
    const HostTempoMap invalid;
    CHECK(!invalid.isValid());
    CHECK(tempoGridLines(invalid, 0.0, 10.0, 0.25,
                         {TempoGridLevel::Subdivision, 1})
              .empty());
    CHECK(planTempoGrid(invalid, 0.0, 10.0, 0.25, 100.0, 4.0).level ==
          TempoGridLevel::Beat);

    // Compases cada 2 con stride 2: numeros 1, 3, 5.
    const Found strided =
        scan(tempoGridLines(map, 0.0, 10.0, 1.0, {TempoGridLevel::Bar, 2}));
    CHECK(strided.barNumbers.size() == 3);
    CHECK(strided.barNumbers[0] == 1);
    CHECK(strided.barNumbers[1] == 3);
    CHECK(strided.barNumbers[2] == 5);
}

// forEachBar/forEachGridLine directos: rango negativo y clasificacion.
void testForEachPrimitives()
{
    const HostTempoMap map = HostTempoMap::constant(120.0, 3, 4); // 3 quarters

    int barsBefore = 0;
    map.forEachBar(-7.5, -1.0, [&](const HostTempoMap::BarInfo& bar) {
        ++barsBefore;
        CHECK(near(bar.endQuarter - bar.startQuarter, 3.0));
        CHECK(bar.numerator == 3);
    });
    CHECK(barsBefore == 3); // compases -3, -2 y -1

    int bars = 0;
    map.forEachBar(0.0, 9.0, [&](const HostTempoMap::BarInfo& bar) {
        ++bars;
        CHECK(bar.barNumber == bars);
        CHECK(near(bar.startQuarter, static_cast<double>(bars - 1) * 3.0));
    });
    CHECK(bars == 4); // 0-3, 3-6, 6-9 y el que empieza en 9

    // Clasificacion con grilla de negra en 3/4.
    int barLines = 0;
    int beatLines = 0;
    int subdivisions = 0;
    map.forEachGridLine(
        0.0, 3.0, 1.0, [&](const HostTempoMap::GridLineInfo& line) {
            if (line.kind == HostTempoMap::GridLineKind::Bar)
            {
                ++barLines;
                CHECK(line.beatInBar == 0);
            }
            else
            {
                ++beatLines;
                CHECK(line.kind == HostTempoMap::GridLineKind::Beat);
                CHECK(line.beatInBar == beatLines);
            }
        });
    CHECK(barLines == 2); // 0 y 3
    CHECK(beatLines == 2); // 1 y 2
    CHECK(subdivisions == 0);

    // Grilla de corchea: una subdivision entre cada negra (0,5 / 1,5 / 2,5).
    map.forEachGridLine(
        0.0, 3.0, 0.5, [&](const HostTempoMap::GridLineInfo& line) {
            if (line.kind == HostTempoMap::GridLineKind::Subdivision)
                ++subdivisions;
        });
    CHECK(subdivisions == 3);

    // Grilla mas gruesa que el compas: solo la linea de compas.
    int coarse = 0;
    map.forEachGridLine(0.0, 3.0, 8.0,
                        [&](const HostTempoMap::GridLineInfo&) { ++coarse; });
    CHECK(coarse == 2);
}
} // namespace

int main()
{
    testConstantFourFour();
    testThreeFourFromMarker();
    testSevenEight();
    testTwoTempos();
    testRamp();
    testThinningAndFallback();
    testForEachPrimitives();

    std::printf("TempoGridTests: all checks passed\n");
    return 0;
}
