#pragma once
#include "f0.h"
#include "segment.h"
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// LA CURVA DE ALTURA (core v2, 2026-10-22).
//
// Modelo (el de los motores de referencia): la curva base es la FUNCION ESCALON
// de las notas, con el cambio de nota en el PUNTO MEDIO entre notas, y luego se
// suaviza con una convolucion de un kernel coseno. Nada de rampas por nota: la
// suavidad sale de suavizar el escalon.
//
// Por que importa: una rampa por nota (lo que teniamos) mete una transicion
// lineal en cada nota, y al corregir el tono el oido nota la "correccion
// entrando"; el escalon suavizado es lo que hace que la curva parezca la que
// canto el cantante.
//
// La curva final = base (notas + correccion) + delta del usuario. El delta es
// lo que dibuja el usuario en el editor; aqui se deja a 0.
// ---------------------------------------------------------------------------

struct PitchCurveParams {
    double smoothSec     = 0.050;  // ancho del kernel coseno (la suavidad)
    double octaveGuardCents = 100.0;  // desviacion que se considera un error
    int    localWinFrames = 9;    // ventana para juzgar si un frame es atipico
    int    outlierMaxFrames = 15;  // tramo malo mas largo: no se repara
    double maxFillGapSec = 0.080; // hueco unvoiced relleno dentro de una nota
    double maxDevCents   = 120.0; // excursion maxima dentro de la nota
};

struct PitchCurve {
    std::vector<float> f0Hz;     // altura objetivo por frame (0 = sin altura)
    std::vector<char>  voiced;   // el frame tiene altura
    std::vector<float> sourceHz; // la altura ORIGINAL (reparada): la fuente
    std::vector<char>  sourceVoiced;
    std::vector<double> timeSec; // el tiempo de cada frame
};

// La curva de altura a partir de las notas y del desplazamiento por nota.
// `shiftCents` va en el MISMO orden que `notes` (0 = la nota se deja como esta).
PitchCurve buildPitchCurve (const std::vector<F0Frame>& frames,
                           const std::vector<Note>& notes,
                           const std::vector<double>& shiftCents,
                           const PitchCurveParams& p = PitchCurveParams());

// Repara los frames con error de estimacion (saltos de octava): se sustituyen
// por la interpolacion de sus vecinos. Se hace sobre la curva fuente.
void repairSource (std::vector<float>& f0, std::vector<char>& voiced,
                   const std::vector<double>& timeSec,
                   const PitchCurveParams& p);

} // namespace pitchcore
