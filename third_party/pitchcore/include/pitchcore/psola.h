#pragma once
#include "engine.h"
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// Motor CLASSIC: TD-PSOLA en el dominio del tiempo.
//
// Por que el PSOLA y no otro metodo: nunca abandona el dominio del tiempo, asi
// que la forma de onda se conserva tal cual, los formantes vienen dentro del
// grano (no hay que estimarlos) y con ratio 1,0 la salida es una COPIA.
//
// Lo que lo hacia sonar mal (medido, y corregido aqui):
//   - la ganancia: la normalizacion fija g = Ts/Ta no compensa la envolvente de
//     cada grano -> modulos de amplitud (los "microcortes") y timbre opaco.
//     Ahora se iguala la ganancia LOCAL a la de la fuente (applyLocalGainMatch).
//   - las partes unvoiced se dejaban como el original -> eso es un empalme
//     entre dos señales distintas en mitad de una nota (dos fases distintas
//     sumandose = batido). Ahora se re-sintetizan con granos fijos de 6 ms.
//   - las marcas de altura: se caminaba un periodo a la vez y la correlacion
//     corregia la prediccion (igual que antes), pero la marca inicial de cada
//     tramo debe ser un PULSO GLOTAL (la muestra mas fuerte del primer
//     periodo), no donde resultara que empiece el tramo voiced: si no, el ataque
//     arranca con la fase equivocada y se oye un escalon.
// ---------------------------------------------------------------------------

// D-202: umbral del hibrido de formantes. El grano actual (remuestreado)
// mueve la envolvente ~0,96x el shift (D-164/D-194), asi que cruza el limite
// C7 (100 c) en ~105 c. Por debajo se queda el grano de siempre; por encima,
// el grano del Clasico del fork (copia sin remuestrear, conserva la
// envolvente de forma intrinseca). La decision es por TRAMO con voz: una
// region usa una sola tecnica y el cambio solo cae en su frontera.
constexpr double kFormantHybridShiftCents = 105.0;

// Media movil de dos pasadas por sumas prefijas (O(n) en vez de O(n*k)).
// Se expone para el test de equivalencia bit a bit con la version O(n*k).
std::vector<float> movingAverageTwoPass (const std::vector<float>& in, int halfWindow);

// Decide si el tramo con voz [firstMarkAbs, lastMarkAbs] (muestras ABSOLUTAS
// del original, como las que devuelve detectPitchMarks) usa el grano del fork.
// true si el |shift| maximo del tramo llega al umbral.
bool hybridRegionUsesForkGrain (const RenderRequest& req, long firstMarkAbs, long lastMarkAbs);

class ClassicEngine : public IEngine
{
public:
    EngineKind kind() const override { return EngineKind::Classic; }
    bool render (const RenderRequest& req, std::vector<float>& out, std::string& err) override;

    // Las marcas de altura (en samples del original, absolutos). Para test.
    static std::vector<int> detectPitchMarks (const RenderRequest& req);
};

} // namespace pitchcore
