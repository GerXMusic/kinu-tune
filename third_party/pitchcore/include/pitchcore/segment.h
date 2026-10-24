#pragma once
#include "f0.h"
#include <vector>

namespace pitchcore {

struct SegmentParams {
    int    medianFrames    = 5;     // mediana sobre el contorno vozeado
    double splitSemitones  = 0.70;  // salto respecto a la nota en curso que abre nota nueva
    int    splitHoldFrames = 4;     // el salto debe sostenerse N frames
    double minNoteSec      = 0.060;
    double maxGapSec       = 0.040; // hueco unvoiced que aún mantiene la misma nota

    // Limpieza previa del contorno
    int    minVoicedRun    = 4;     // tramos vozeados de menos frames se descartan (blips)
    int    outlierWinFrames = 35;   // ventana +-N frames para juzgar un frame atípico
    double outlierSemitones = 7.0;  // distancia a la mediana vecina que lo marca como error de octava/ruido

    // Posproceso de notas
    double fragSec         = 0.10;  // notas más cortas que esto son candidatas a fragmento de transición
    double fragGapSec      = 0.06;  // y deben estar pegadas (hueco máx.) a una vecina
    double fragMaxSemitones = 1.6;  // se absorben solo si el vecino está a <= este intervalo
    double mergeSemitones  = 0.5;   // vecinas casi iguales y pegadas se unen

    // Anti-vibrato: antes de partir una nota por desviación de altura, se mira
    // hacia delante. Si el F0 vuelve al centro de la nota, la desviación era
    // vibrato (modulación periódica), no un cambio de altura: no se parte.
    // Sin esto, un vibrato de 200 cents a 8 Hz parte la nota en 11 fragmentos.
    double vibratoLookaheadSec = 0.20;

    // Anti-fusión: dos notas consecutivas a la misma altura NO se fusionan si
    // entre ellas hay un valle de energía (un ataque). Medido en el corpus real:
    // los valles en fronteras same-pitch son de 2,4-4,9 dB de mediana, frente a
    // 0,26-0,40 dB de mediana (p90 <= 2,4 dB) en el interior de las notas.
    double mergeMaxDipDb = 2.0;    // valle (dB) a partir del cual NO se fusionan
    double mergeValleyWinSec = 0.030;  // ventana del valle alrededor de la frontera
    double mergeValleyCtxSec = 0.120;  // contexto para la mediana de referencia

    // Detector de ataques por energía: dos notas a la MISMA altura no se
    // distinguen por el pitch, solo por el ataque. Un ataque tiene una firma
    // de dos partes: un VALLE local (la energia cae) y una SUBIDA posterior
    // (la nota nueva arranca). Exigir solo el valle no vale: en una caida
    // monotona de energia el minimo local esta siempre en el borde y la
    // "profundidad" mide solo la pendiente, lo que shreddeaba la nota en
    // trozos de 12 ms.
    //   ataque <=> (pre  - valle >= attackDipDb)  AND  (post - valle >= attackRiseDb)
    // donde pre/post son medianas de la energia antes y despues del valle.
    double attackDipDb = 1.0;      // caída (dB) respecto al nivel previo
    double attackRiseDb = 0.75;    // subida (dB) posterior minima
    double attackRiseSec = 0.080;   // ventana para confirmar que la energía vuelve

    // Diagnóstico: vuelca a stderr por qué se descarta o se parte cada nota.
    bool   debug = false;
};

struct Note {
    double t0 = 0, t1 = 0;      // segundos
    double midi = 0;            // mediana en semitonos MIDI (fraccionario)
    int    midiRounded = 0;
    double centsOffset = 0;     // desviación respecto a midiRounded
    int    firstFrame = 0, lastFrame = 0;
};

double hzToMidi(double hz);
double midiToHz(double midi);

// Segmenta SOLO tramos vozeados. Los unvoiced (sibilantes, respiraciones, silencio) no producen nota.
std::vector<Note> segmentNotes(const std::vector<F0Frame>& frames, const SegmentParams& p);

} // namespace pitchcore
