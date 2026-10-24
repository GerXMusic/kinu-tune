#pragma once
#include <complex>
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// Shelf de agudos (compensacion de "aire").
//
// POR QUE EXISTE: el vocoder neural pierde la energia de 8-16 kHz (medido: la
// pasada nula deja ~0,57 de la original) y el oido lo percibe como una voz
// matada. Un shelf devuelve ese aire.
//
// OJO CON LA RECETA: el biquad "clasico" con A = 10^(dB/40) que usabamos NO
// era un shelf. Medido: -6,6 dB a 20 Hz, -6,5 a 200 Hz, -5,8 a 1 kHz y +4,6 a
// 8 kHz, o sea que apagaba el CUERPO de la voz y no era un realce de agudos.
// Por eso el shelf se construye aqui por principio, no por formula:
//
//     lp = paso bajo Butterworth de 2º orden a f0
//     hp = x - lp            (complementario exacto)
//     y  = lp + A*hp
//
// En graves lp=x y hp=0 -> 0 dB (el cuerpo intacto). En agudos lp->0 y hp=x ->
// +gainDb. No hay sorpresas algebraicas.
//
// Se aplica SOLO a la zona resintetizada: el original que se devuelve en las
// zonas sin altura no pasa por el filtro (si no, se alteraria dos veces).
// ---------------------------------------------------------------------------

// `gainDb` en dB (0 = apagado). `f0` es el centro del shelf (8 kHz por defecto).
void airShelf (std::vector<float>& x, double sampleRate, double gainDb, double f0 = 8000.0);

// ---------------------------------------------------------------------------
// FFT radix-2 en sitio (compleja) + STFT/ISTFT.
//
// Hace falta para el motor ESPECTRAL: la resintesis armonica necesita la
// magnitud Y la fase del espectro, no solo la magnitud.
// ---------------------------------------------------------------------------

class Fft
{
public:
    explicit Fft (int n);            // n debe ser potencia de 2
    int size() const { return n_; }

    // Transformada directa (signo -1), in situ.
    void forward (std::vector<std::complex<float>>& a) const;
    // Transformada inversa (signo +1, con 1/n), in situ.
    void inverse (std::vector<std::complex<float>>& a) const;

private:
    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<float>> roots_;   // raices de la unidad
};

// STFT con ventana de Hann simetrica y solape del 75 % (hop = nFft/4).
// Devuelve solo los bins 0..nFft/2 (conjugados simetricos) en [frames][bins].
// `nFrames` se calcula sobre la senal completa (sin padding).
void stftComplex (const std::vector<float>& x, int nFft, int hop, int& nFrames,
                  std::vector<std::complex<float>>& out);

// ISTFT: ventana de Hann en la sintesis y normalizacion por la suma de w^2
// (OLA ponderada: reconstruye la senal original si no se toca nada).
void istftComplex (const std::vector<std::complex<float>>& spec, int nFft, int hop,
                   int nFrames, std::vector<float>& out);

} // namespace pitchcore
