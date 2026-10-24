#pragma once
#include "engine.h"
#include <string>

namespace pitchcore {

// ---------------------------------------------------------------------------
// Motor ESPECTRAL: resintesis armonica con la ENVOLVENTE preservada.
//
// POR QUE ESTE MOTOR
// ------------------
// Medido sobre las notas del corpus (distancia espectral mel contra el
// ORIGINAL, la misma nota):
//
//     espectral   0,018 - 0,053     <- praticamente transparente
//     PSOLA       0,24  - 0,36      <- cambia el timbre ("opaco")
//     neuronal    0,49  - 0,53      <- el "artefacto de autotune" es el vocoder
//
// El motor funciona asi:
//   1) STFT de la region;
//   2) ENVOLVENTE espectral E: el percentil 25 de la magnitud en 24 bandas
//      log-espaciadas (el percentil, no la media: la media la siguen las
//      armonicas y entonces la envolvente no seria la de los formantes);
//   3) ESTRUCTURA ARMONICA de la fuente: la amplitud de cada armonico m y su
//      peso relativo S_m = A_m / E(m*F0) (el "espectro de la fuente", sin
//      formantes);
//   4) las armonicas se COLOCAN en m*F0_objetivo con la envolvente EVALUADA
//      en la posicion nueva: los formantes NO se mueven con la altura (esto es
//      lo que hace Melodyne);
//   5) el residuo (ruido, consonantes, soplo) se conserva tal cual;
//   6) la fase es la del original (phase vocoder) y la reconstruccion es un
//      OLA ponderado.
//
// En resumen: mueve la altura de las armonicas y deja la envolvente y el
// ruido donde estaban. Por eso es el motor que "suena como el original".
// ---------------------------------------------------------------------------

struct SpectralEngineParams {
    int    nFft      = 1024;   // una rejilla MAS GRUESA difumina menos el
                                // leakage de las armonicas al desplazarlas
    int    hop       = 256;
    double fmin      = 60.0;
    double fmax      = 8000.0;
    int    nBands    = 24;
    double percentile = 25.0;   // percentil de la magnitud dentro de cada banda
    bool   keepUnvoiced = true;  // devolver el original donde no hay altura
};

class SpectralEngine : public IEngine
{
public:
    SpectralEngine() = default;
    explicit SpectralEngine (const SpectralEngineParams& p) : params_ (p) {}

    EngineKind kind() const override { return EngineKind::Spectral; }
    bool render (const RenderRequest& req, std::vector<float>& out, std::string& err) override;

    const SpectralEngineParams& params() const { return params_; }

private:
    SpectralEngineParams params_;
};

} // namespace pitchcore
