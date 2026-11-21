#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

/**
 * M4 v1: cuantizacion de la posicion de las notas al grid (D-242).
 *
 * Logica pura, sin JUCE: el llamador decide que notas entran y en que rejilla
 * (en frames). Se separa para poder probarla en un test rojo->verde sin montar
 * el editor.
 *
 * Reglas aprobadas (D-242):
 *  - se mueven FRONTERAS COMPARTIDAS: una frontera se mapea UNA vez y las dos
 *    notas que la tocan reciben el mismo valor (sin huecos ni solapes nuevos);
 *  - fuerza 0..100: 0 es la identidad exacta (control negativo), 100 lleva la
 *    frontera a la rejilla;
 *  - si el resultado colapsa una nota o crea un solape que antes no existia,
 *    se rechaza el cambio entero (el llamador no toca el proyecto).
 */
namespace timingQuantize {

struct Span
{
    int startFrame = 0;
    int endFrame = 0;
    bool selected = false;
};

/** Frontera cuantizada. `gridFrames` <= 0 o fuerza <= 0 devuelven la identidad. */
inline int quantizeFrame (int frame, double gridFrames, int strengthPercent)
{
    if (strengthPercent <= 0 || gridFrames <= 0.0)
        return frame;

    const double snapped =
        std::round (static_cast<double> (frame) / gridFrames) * gridFrames;
    const double amount =
        static_cast<double> (std::clamp (strengthPercent, 0, 100)) / 100.0;
    const double blended =
        (1.0 - amount) * static_cast<double> (frame) + amount * snapped;
    return static_cast<int> (std::llround (blended));
}

/**
 * Calcula el estado cuantizado de TODAS las notas de `in` (las no
 * seleccionadas solo se mueven si comparten una frontera con una seleccionada).
 * Devuelve false sin modificar `out` utilizable si el resultado no es seguro.
 */
inline bool quantizeSpans (const std::vector<Span>& in, double gridFrames,
                           int strengthPercent, std::vector<Span>& out)
{
    out = in;
    if (strengthPercent <= 0 || gridFrames <= 0.0)
        return true;

    // Una frontera -> un destino. Las notas que comparten frontera comparten
    // destino por construccion, que es lo que conserva la contiguedad.
    std::map<int, int> boundaryTarget;
    for (const auto& span : in)
    {
        if (! span.selected)
            continue;
        boundaryTarget[span.startFrame] =
            quantizeFrame (span.startFrame, gridFrames, strengthPercent);
        boundaryTarget[span.endFrame] =
            quantizeFrame (span.endFrame, gridFrames, strengthPercent);
    }

    for (auto& span : out)
    {
        if (const auto it = boundaryTarget.find (span.startFrame);
            it != boundaryTarget.end())
            span.startFrame = it->second;
        if (const auto it = boundaryTarget.find (span.endFrame);
            it != boundaryTarget.end())
            span.endFrame = it->second;
    }

    // Ninguna nota puede colapsar ni invertirse.
    for (const auto& span : out)
        if (span.endFrame <= span.startFrame)
            return false;

    // No se admite un solape nuevo. El orden se toma del estado ORIGINAL:
    // si dos notas ya se solapaban, la relacion se conserva y no se juzga.
    std::vector<size_t> order (in.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort (order.begin(), order.end(),
                      [&in] (size_t a, size_t b)
                      { return in[a].startFrame < in[b].startFrame; });
    for (size_t k = 1; k < order.size(); ++k)
    {
        const auto& previousIn = in[order[k - 1]];
        const auto& currentIn = in[order[k]];
        if (previousIn.endFrame > currentIn.startFrame)
            continue; // ya se solapaban: no es un solape nuevo
        if (out[order[k - 1]].endFrame > out[order[k]].startFrame)
            return false;
    }
    return true;
}

} // namespace timingQuantize
