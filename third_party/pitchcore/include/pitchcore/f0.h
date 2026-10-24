#pragma once
#include <vector>

namespace pitchcore {

struct F0Params {
    double sampleRate  = 44100.0;
    double fMin        = 70.0;     // Hz
    double fMax        = 1000.0;   // Hz
    double windowSec   = 0.0232;   // ventana de integración de YIN
    double hopSec      = 0.0029;   // salto entre frames
    double yinThresh   = 0.15;     // umbral de aperiodicidad (cmnd) para voiced
    double silenceDb   = -55.0;    // por debajo: unvoiced (RMS dBFS)

    // Suavizado de la aperiodicidad antes de decidir voiced/unvoiced.
    // Sin esto, un frame con aper 0,153 en mitad de una nota cantada
    // (aper real 0,01) marca ese frame como unvoiced, parte el tramo voiced y
    // el filtro de blips descarta la nota entera. Medido en el corpus: el
    // 12,1 % de los frames tiene aper en [0,10, 0,25], justo en la zona donde
    // el umbral decide. 1 = sin suavizado.
    int    aperSmoothFrames = 5;
};

struct F0Frame {
    double timeSec = 0.0;          // centro aproximado del frame
    double f0Hz    = 0.0;          // 0 si unvoiced
    double aper    = 1.0;          // aperiodicidad (cmnd mínimo): 0 = periódico puro
    double rmsDb   = -200.0;
    bool   voiced  = false;
};

// Estimador F0 offline basado en YIN (de Cheveigné & Kawahara 2002) con interpolación parabólica.
// Línea base: se sustituirá/complementará por pYIN y modelos ONNX (CREPE/RMVPE) detrás de la misma interfaz.
std::vector<F0Frame> estimateF0(const std::vector<float>& mono, const F0Params& p);

} // namespace pitchcore
