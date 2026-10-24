#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// CONTRATO DE RENDER (core v2, 2026-10-22).
//
// Idea (la de los motores de referencia, PitchNet/KinuTune): el motor NO
// "dibuja" notas, **recoloca la onda original**. Por eso una sola peticion
// describe cualquier edicion con dos curvas:
//
//   sourceFrame[]  por frame de SALIDA: que frame del ORIGINAL lee (fraccional).
//                  La identidad si no hay retime; un tramo estirado si la nota
//                  se re-timinea -> una sola pasada sigue cualquier mapa de tiempo.
//   pitchRatio[]   por frame de SALIDA: razon de frecuencias objetivo/fuente.
//
// Todo lo que el motor necesita del original son esas dos curvas y el F0 de la
// fuente (denso: los huecos unvoiced van interpolados).
//
// REGLA DE LOS BORDES: al muestrear las curvas MAS ALLA de sus extremos se
// CONTINUA LA IDENTIDAD, no se recorta a un extremo. Un recorte brusco plegaria
// la posicion de lectura sobre si misma y ensuciaria los bordes de la region
// (es exactamente el fallo de "el ultimo frame repite el anterior").
// ---------------------------------------------------------------------------

struct RenderRequest {
    // la señal original (completa: el motor puede leer antes y despues)
    const std::vector<float>* original = nullptr;
    int    sourceStartSample = 0;   // primer sample que se renderiza
    int    numSamples = 0;          // samples a producir

    // Las curvas van indexadas en la rejilla ABSOLUTA del proyecto. El motor
    // trabaja en frames LOCALES (0..numFrames-1) y el frame absoluto del local i
    // es frameOffset + i.
    int    frameOffset = 0;         // frame absoluto del frame local 0
    int    totalFrames = 0;         // total de frames absolutos (para extender
                                     // la identidad en los extremos)

    std::vector<double> sourceFrame;   // frame fraccional del original
    std::vector<float>  pitchRatio;    // objetivo / fuente
    std::vector<float>  sourceF0;      // F0 de la FUENTE (Hz, denso, 0 = sin altura)
    std::vector<uint8_t> sourceVoiced;  // 1 = la fuente tiene altura

    double curveHop = 512.0;   // hop de la rejilla de trabajo, en samples
    double sampleRate = 44100.0;
    bool   preserveFormants = true;
};

// Muestrea sourceFrame en un frame fraccional, extendiendo la identidad mas
// alla de los dos extremos.
inline double sampleSourceFrame (const std::vector<double>& curve, double frame,
                                 int numFrames)
{
    if (curve.empty() || numFrames <= 0) return frame;
    const int last = numFrames - 1;
    if (frame <= 0.0) return curve[0] + frame;
    if (frame >= (double) last) return curve[(size_t) last] + (frame - (double) last);
    const int left = (int) std::floor (frame);
    const int right = std::min (left + 1, last);
    const double amount = frame - (double) left;
    return curve[(size_t) left] * (1.0 - amount) + curve[(size_t) right] * amount;
}

// Muestrea una curva por frame fraccional, extendiendo la identidad.
inline double sampleCurve (const std::vector<float>& curve, double frame, int numFrames)
{
    if (curve.empty() || numFrames <= 0) return 1.0;
    const int last = numFrames - 1;
    if (frame <= 0.0) return curve[0] + (float) frame;
    if (frame >= (double) last) return curve[(size_t) last] + (float) (frame - (double) last);
    const int left = (int) std::floor (frame);
    const int right = std::min (left + 1, last);
    const double amount = frame - (double) left;
    return curve[(size_t) left] * (1.0 - amount) + curve[(size_t) right] * amount;
}

} // namespace pitchcore
