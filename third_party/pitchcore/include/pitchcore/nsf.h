#pragma once
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// Pre-proceso del vocoder neuronal NSF-HiFiGAN (PC-NSF-HiFiGAN).
//
// Implementa EXACTAMENTE la referencia de DiffSinger (`modules/nsf_hifigan/nvSTFT.py`),
// que es la que usa el paquete OpenUtau para este mismo ONNX. Los detalles que
// hay que respetar y que no son evidentes:
//
//  - el filtro mel es el de librosa con los valores por defecto: escala Slaney
//    (htk=False) y normalizacion de area Slaney (norm='slaney');
//  - el STFT usa reflect-pad explicito de (win-hop)/2 y (win-hop+1)/2 muestras
//    y center=FALSE, de modo que el frame t tiene su centro en la muestra
//    t*hop + (nFft/2 - (win-hop)/2);
//  - se usa la MAGNITUD |STFT|, no la potencia: pasar de magnitud a potencia
//    multiplica la mel lineal por 2 y el log por 0.693;
//  - la mel es log NATURAL con clip en 1e-5.
//
// Verificado: con esta implementacion la reconstruccion del vocoder tiene una
// distancia mel L1 de 0.29 y la F0 de salida sigue al f0 inyectado con +-0.9 cents.
// ---------------------------------------------------------------------------

struct NsfMelParams {
    int    sampleRate = 44100;
    int    nFft       = 2048;
    int    winSize    = 2048;
    int    hopSize    = 512;
    int    nMels      = 128;
    double fmin       = 40.0;
    double fmax       = 16000.0;
    double clipVal    = 1e-5;
};

// Filtro mel estilo Slaney, identico a librosa.filters.mel(htk=False, norm='slaney').
// Devuelve nMels filas de (nFft/2+1) coeficientes.
std::vector<std::vector<float>> nsfMelFilterbank (const NsfMelParams& p);

// STFT con reflect-pad y center=false. Devuelve la MAGNITUD en [frames][nFft/2+1]
// (row-major) y el numero de frames en `nFrames`.
std::vector<float> nsfStftMagnitude (const std::vector<float>& x, const NsfMelParams& p,
                                     int& nFrames);

// Mel en log natural, [frames][nMels] (row-major).
std::vector<float> nsfComputeMel (const std::vector<float>& x, const NsfMelParams& p,
                                  int& nFrames);

// Muestra original en la que se centra cada frame.
std::vector<int> nsfFrameCenters (int nSamples, const NsfMelParams& p);

// Rejilla un f0 por muestra (Hz, 0 si unvoiced) a los centros de frame.
std::vector<float> nsfResampleF0 (const std::vector<float>& f0PerSample, int nSamples,
                                  const NsfMelParams& p, int& nFrames);

// Rellena los frames unvoiced con el F0 interpolado de sus vecinos voiced.
//
// Es IMPRESCINDIBLE para el vocoder NSF: si se le inyecta f0=0 en un frame
// unvoiced, el modelo genera silencio ahi y aparecen huecos en la salida
// (medido: la retencion de voicing cae del 92,3 % al 87,2 %). Los vocoders
// NSF esperan una curva de F0 CONTINUA. Solo los huecos mas largos que
// `maxGapFrames` se dejan a 0 (son silencios reales).
std::vector<float> nsfContinuousF0 (const std::vector<float>& f0, const std::vector<char>& voiced,
                                   int maxGapFrames = 8);

} // namespace pitchcore
