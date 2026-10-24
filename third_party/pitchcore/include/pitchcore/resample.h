#pragma once
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// Remuestrador de banda limitada (polyphase windowed-sinc con ventana de Kaiser).
//
// Hace falta porque el vocoder PC-NSF-HiFiGAN es de 44 100 Hz y el host puede
// ir a otra tasa (el corpus esta a 48 000).
//
// PROPIEDAD IMPORTANTE: el remuestreo CONSERVA LA ALTURA. Un tono de 440 Hz a
// 48 kHz sigue a 440 Hz tras remuestrear a 44,1 kHz, porque el numero de
// muestras por periodo escala con la tasa de muestreo. (Escribir "reproducir
// mas rapido" es un error conceptual frecuente: el remuestreo no es un estirado
// temporal.)
// ---------------------------------------------------------------------------

class Resampler
{
public:
    Resampler() = default;

    // Prepara el remuestrador de `srcRate` a `dstRate`.
    void init (double srcRate, double dstRate);

    bool isReady() const { return ready_; }
    double ratio() const { return ratio_; }

    // Remuestrea `in` a la tasa destino. `out` se reemplaza.
    // OJO: `in` y `out` NO pueden ser el mismo vector (out.assign() borraria
    // la entrada antes de leerla).
    void process (const std::vector<float>& in, std::vector<float>& out) const;

private:
    bool   ready_ = false;
    double srcRate_ = 0.0, dstRate_ = 0.0, ratio_ = 1.0, cutoff_ = 0.5;
    int    phases_ = 1024, taps_ = 32, half_ = 16, center_ = 15;
    std::vector<float> table_;   // phases_ * taps_
};

} // namespace pitchcore
