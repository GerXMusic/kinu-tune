#pragma once

#include "DspSynthesisRequest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * D-106: compuerta de `voiced` del camino Pitchcore.
 *
 * El nucleo da por buena la mascara de voz que recibe y su marcha de marcas
 * necesita un arranque periodico (psola.cpp:186-213). La mascara del fork marca
 * voz un frame antes de que el audio tenga periodo (D-105: frames 1837-1839,
 * corr -0,391 en el lag predicho), y con ese arranque la marcha hace `break` y
 * no se reinicia (I-12): el tramo entero se queda sin marcas.
 *
 * Esta compuerta quita SOLO los frames que el propio test del nucleo
 * rechazaria: mismo umbral (`kMinCorr` = 0,55, psola.cpp:25) y misma geometria
 * (+-half, retardo T). No hay nota, frame ni indice especial en el codigo.
 *
 * Vive aqui y no en IncrementalSynthesizer para no cambiar la peticion del
 * PSOLA del fork (premisa de 2a) ni el nucleo vendorizado (I-11/I-12 siguen
 * abiertas y sin arreglar).
 */
namespace pitchcore_voiced_gate {

/// Aplica la compuerta in situ sobre `sourceVoiced`. Devuelve cuantos frames
/// apago. Idempotente: un frame ya apagado no se vuelve a contar.
inline int apply(DspSynthesisRequest &request, double threshold = 0.55)
{
    const int hop = request.hopSize;
    const double sampleRate = static_cast<double>(request.sampleRate);
    auto &voiced = request.sourceVoiced;
    const auto &f0 = request.sourceF0;

    if (hop <= 0 || !(sampleRate > 0.0) || voiced.empty() || f0.empty() ||
        request.source.empty())
        return 0;

    const long total = static_cast<long>(request.source.size());
    int cleared = 0;

    for (size_t frame = 0; frame < voiced.size(); ++frame)
    {
        if (voiced[frame] == 0 || frame >= f0.size())
            continue;

        const double value = static_cast<double>(f0[frame]);
        if (!(value > 0.0))
            continue;

        const long period = std::lround(sampleRate / value);
        if (period < 16 || period > 1200)
            continue;

        const long half = std::max<long>(4, period / 2);
        const long centre =
            std::lround(static_cast<double>(frame) * hop) -
            static_cast<long>(request.sourceStartSample);
        if (centre - half < 0 || centre + period + half > total)
            continue; // sin ventana completa: no hay evidencia, no se toca

        double dot = 0.0, energyA = 0.0, energyB = 0.0;
        for (long k = -half; k < half; ++k)
        {
            const double a =
                static_cast<double>(request.source[static_cast<size_t>(centre + k)]);
            const double b = static_cast<double>(
                request.source[static_cast<size_t>(centre + period + k)]);
            dot += a * b;
            energyA += a * a;
            energyB += b * b;
        }

        const double den = std::sqrt(energyA * energyB);
        if (den <= 1e-12)
            continue;

        if (dot / den < threshold)
        {
            voiced[frame] = 0;
            ++cleared;
        }
    }

    return cleared;
}

} // namespace pitchcore_voiced_gate
