#pragma once
#include <string>
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// Generador de senal de voz sintetica con verdad exacta.
//
// La clase C del benchmark se construye aqui: en vez de renderizar con un
// sintetizador externo (cuyo F0 exacto solo se conoce por construccion y no al
// centavo), generamos la senal nosotros mismos a partir de una lista de notas,
// de modo que el F0 real de cada muestra es una funcion analitica conocida.
//
// Esa verdad permite medir el error del estimador al centavo y fijar casos
// limite de segmentacion (repeticiones, micro-errores, glides, vibrato).
// ---------------------------------------------------------------------------

struct SynthNote {
    double t0 = 0.0;            // segundos
    double t1 = 0.0;
    double midi = 60.0;          // altura base (MIDI, fraccionario)
    double amp = 1.0;

    // Expresion
    double vibCents = 0.0;       // profundidad del vibrato
    double vibHz = 5.5;
    double vibDelaySec = 0.0;    // retardo de entrada del vibrato
    double glideFromCents = 0.0; // slide desde la nota anterior (0 = sin glide)
    double glideSec = 0.0;       // duracion del slide (0 = 30% de la nota)
    double jitterCents = 0.0;    // deriva lenta aleatoria
    double driftCents = 0.0;     // deriva lineal (inicio -> final)

    // Envolvente y articulation
    double attackSec = 0.008;
    double releaseSec = 0.020;
    bool   legato = false;       // sin hueco entre notas contiguas
    bool   consonant = false;    // rafaga de ruido al inicio (previa al voicing)
    bool   breath = false;       // ruido de respiracion antes del ataque
    double consonantSec = 0.020;
    double breathSec = 0.040;
    double gapSec = 0.0;         // hueco (silencio) antes de la nota
};

struct SynthFormant {
    double hz = 700.0;
    double q = 8.0;
    double gain = 1.0;
};

struct SynthParams {
    double sampleRate = 48000.0;
    int    nHarmonics = 28;       // componentes de la fuente glotal
    double harmonicDecay = 1.0;   // amplitud ~ 1/k^decay
    std::vector<SynthFormant> formants;
    double noiseLevel = 0.0;      // ruido blanco added (0 = solo consonantes/breath)
    double gain = 0.22;           // ganancia de salida
    unsigned seed = 20261021u;
};

struct SynthTruth {
    std::vector<double> time;     // por muestra
    std::vector<double> f0;       // Hz exactos (0 si no voiced)
    std::vector<char>   voiced;    // 1 = voiced
    std::vector<SynthNote> notes;
};

// F0 exacto de una nota en el instante t (vibrato + glide + jitter + drift).
// `prev` puede ser nullptr (primera nota).
double synthF0At (const SynthNote& n, const SynthNote* prev, double t, const SynthParams& p);

// Genera la senal y su verdad exacta. `truth` se rellena con el F0 analitico
// muestra a muestra: es la referencia contra la que se mide el estimador.
std::vector<float> renderSynth (const std::vector<SynthNote>& notes,
                                const SynthParams& p,
                                SynthTruth& truth);

// helpers deConstruccion de casos
SynthNote makeNote (double t0, double t1, double midi);

// Escribe un MIDI de una sola pista con las notas dadas (formato 0, div 480).
// Permite que la clase C atraviese el MISMO pipeline de metricas que A y B.
bool writeMidi (const std::string& path,
                const std::vector<SynthNote>& notes,
                double bpm = 120.0);

} // namespace pitchcore
