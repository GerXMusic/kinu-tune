#pragma once

#include "../../JuceHeader.h"
#include "DspSynthesisRequest.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "pitchcore/render_request.h"

/**
 * Adaptador de M2 (D-089, D-092): nuestro motor CLASICO dentro del plugin.
 *
 * El plugin ya habla el contrato de la arquitectura de referencia
 * (DspSynthesisRequest: sourceFrame + pitchRatio + F0 de la fuente), asi que
 * esto no es un motor nuevo: es la traduccion de ese contrato al nucleo
 * vendorizado (third_party/pitchcore, D-095) y la entrega del buffer.
 *
 * Lo que NO hace, a proposito:
 *   - No mezcla, no recorta y no suaviza: devuelve exactamente
 *     numOutputFrames * hopSize muestras, como PsolaSynthesizer, y el commit
 *     del IncrementalSynthesizer sigue siendo el unico que escribe audio.
 *   - No cambia la fuente: el nucleo lee el tramo con originalStartSample
 *     (D-096), sin copiar la pista entera.
 *
 * El criterio de que el audio no cambia y de que C1/C2/C3/C4 no empeoran esta
 * escrito antes de medir en D-093.
 */
class PitchcoreSynthesizer
{
public:
    using Request = DspSynthesisRequest;

    PitchcoreSynthesizer();
    ~PitchcoreSynthesizer();

    /**
     * Render on a worker thread and deliver the result to @p callback.
     * The callback receives an empty vector on failure or cancellation.
     */
    void renderAsync(Request request,
                     std::function<void(std::vector<float>)> callback,
                     std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

    /// Synchronous render. Returns empty on invalid input or cancellation.
    static std::vector<float> render(const Request &request,
                                     const std::shared_ptr<std::atomic<bool>> &cancelFlag = nullptr);

    /**
     * Traduce la peticion del fork al contrato del nucleo. Expuesto para el
     * arnes bit a bit (D-093, nivel 1): si esto cambia, la comparacion con el
     * CLI deja de significar lo mismo.
     */
    static pitchcore::RenderRequest toCoreRequest(const Request &request);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchcoreSynthesizer)
};
