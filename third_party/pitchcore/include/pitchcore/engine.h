#pragma once
#include "render_request.h"
#include <memory>
#include <string>
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// MOTORES (core v2, 2026-10-22).
//
// Todos los motores responden a la MISMA pregunta: "produce el audio editado de
// este tramo", y todos la responden LEYENDO la onda original (no regenerandola).
// Por eso un cambio de motor no puedemeter un empalme: los dos leen la misma
// señal y por tanto quedan alineados.
//
// CATALOGO:
//   Classic  TD-PSOLA en el dominio del tiempo: rapido, conserva los formantes
//            (vienen dentro del grano) y con ratio 1,0 es una copia.
//   Neural   vocoder PC-NSF-HiFiGAN: flexible, pero genera su propia fase (por eso
//            no se puede MEZCLAR con otro motor: batido) y deforma el timbre mas.
//   Natural  re-sintesis armonica con la envolvente preservada: el que no
//            deforma la voz (LSD 0,03 frente a 0,42 del neuronal).
// ---------------------------------------------------------------------------

enum class EngineKind : int { Classic = 0, Neural = 1, Spectral = 2 };

// Nombre para la interfaz y los CSV.
const char* engineName (EngineKind k);
bool        parseEngine (const std::string& s, EngineKind& out);

class IEngine
{
public:
    virtual ~IEngine() = default;
    virtual EngineKind kind() const = 0;
    // Renderiza `req.numSamples` samples a la tasa de la peticion.
    virtual bool render (const RenderRequest& req, std::vector<float>& out, std::string& err) = 0;
};

} // namespace pitchcore
