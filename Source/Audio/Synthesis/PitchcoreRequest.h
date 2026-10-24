#pragma once

#include "DspSynthesisRequest.h"
#include "pitchcore/render_request.h"

#include <algorithm>
#include <cmath>

/**
 * Traduccion de la peticion del fork al contrato del nucleo (M2 paso 1).
 *
 * Vive aparte de PitchcoreSynthesizer y sin JUCE a proposito: el arnes
 * tools/pitchcore_adapter_check.cpp tiene que poder ejercitar exactamente esta
 * traduccion sin enlazar el plugin entero (D-093, nivel 1).
 *
 * Lo unico que se toca son DOS cosas, y ninguna es DSP:
 *
 *   1. El tramo. El fork manda `source` con `sourceStartSample` (muestra
 *      absoluta del proyecto donde empieza el tramo) y el nucleo lee por
 *      posiciones absolutas: se copia a `originalStartSample` (D-096).
 *
 *   2. El indice de las curvas. El fork indexa `sourceFrame` y `pitchRatio` por
 *      frame LOCAL de la region (indice 0 = primer frame renderizado); el
 *      nucleo las muestrea en frame = muestra_absoluta / hop. Se rellena el
 *      tramo anterior con la MISMA continuacion que el nucleo aplica fuera de
 *      rango (identidad para sourceFrame, plano para pitchRatio), de forma que
 *      la interpolacion en el borde da el mismo valor que daria el nucleo.
 *
 * `sourceF0` y `sourceVoiced` ya vienen en indexacion absoluta: se copian tal
 * cual.
 */
namespace pitchcore_adapter
{

inline pitchcore::RenderRequest toCoreRequest(const DspSynthesisRequest &request)
{
    pitchcore::RenderRequest core;

    core.original = &request.source;
    // DOS origenes distintos, y confundirlos desplaza el audio:
    //   originalStartSample = donde cae source[0] en el proyecto (el tramo con
    //                         contexto que manda el fork), solo para LEER;
    //   sourceStartSample   = donde empieza la SALIDA, que en el fork es el
    //                         frame 0 del mapa (sourceFrame.front()), no el
    //                         inicio del tramo. El fork posiciona con
    //                         sourceFrame*hop - sourceStartSample
    //                         (PsolaSynthesizer.cpp:284-286).
    core.originalStartSample = request.sourceStartSample;

    const int hop = request.hopSize;
    const double firstSource =
        request.sourceFrame.empty() ? 0.0 : request.sourceFrame.front();
    const long outputOrigin =
        (hop > 0) ? std::lround(firstSource * static_cast<double>(hop)) : 0L;
    core.sourceStartSample = static_cast<int>(outputOrigin);
    core.numSamples = request.numOutputFrames * hop;
    core.curveHop = static_cast<double>(hop);
    core.sampleRate = static_cast<double>(request.sampleRate);
    core.preserveFormants = request.preserveFormants;

    const int base = hop > 0
                         ? static_cast<int>(std::floor(
                               static_cast<double>(outputOrigin) / hop))
                         : 0;
    const int frames = std::max(0, request.numOutputFrames);
    core.totalFrames = base + frames;

    core.sourceFrame.assign(static_cast<size_t>(core.totalFrames), 0.0);
    core.pitchRatio.assign(static_cast<size_t>(core.totalFrames), 1.0f);

    const float firstRatio =
        request.pitchRatio.empty() ? 1.0f : request.pitchRatio.front();
    for (int j = 0; j < base; ++j)
    {
        core.sourceFrame[static_cast<size_t>(j)] =
            firstSource - static_cast<double>(base - j);
        core.pitchRatio[static_cast<size_t>(j)] = firstRatio;
    }

    const int available = static_cast<int>(
        std::min(request.sourceFrame.size(), request.pitchRatio.size()));
    const int copyFrames = std::min(frames, available);
    for (int i = 0; i < copyFrames; ++i)
    {
        core.sourceFrame[static_cast<size_t>(base + i)] =
            request.sourceFrame[static_cast<size_t>(i)];
        core.pitchRatio[static_cast<size_t>(base + i)] =
            request.pitchRatio[static_cast<size_t>(i)];
    }

    core.sourceF0 = request.sourceF0;
    core.sourceVoiced = request.sourceVoiced;

    return core;
}

} // namespace pitchcore_adapter
