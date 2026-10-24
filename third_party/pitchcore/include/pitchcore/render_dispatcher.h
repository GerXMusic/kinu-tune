#pragma once
#include "render_request.h"
#include <memory>
#include <vector>

namespace pitchcore {

// ---------------------------------------------------------------------------
// DESPACHADOR DE RENDER (core v2, 2026-10-22).
//
// Hace tres cosas, y las tres son de los motores de referencia (PitchNet /
// KinuTune) porque son las que evitan los clicks y el "doble voz":
//
// 1) AMPLIAR EL RANGO A RENDERIZAR
//    - con CONTEXTO (contextFrames): el vocoder necesita frames de contexto
//      para estabilizar la fase local cuando re-sintetiza a trozos;
//    - PUENTEANDO los huecos unvoiced cortos (gapBridgeFrames): asi las notas
//      vecinas alrededor de una consonante se sintetizan JUNTAS y no hay un
//      reset de fase en la union.
//
// 2) LA MEZCLA "STABILITY-FIRST"
//    Por defecto manda el audio SINTETIZADO en toda la region. El original
//    solo se devuelve en los tramos unvoiced LARGOS (respiraciones, silencios),
//    NO en los huecos cortos entre notas. Mezclar el original con lo sintetizado
//    en los ataques produce batido (dos fases distintas sumandose) y es
//    exactamente el "doble voz" que se oye.
//
// 3) EL CRUCE de las transiciones: rampas lineales centradas en el limite,
//    de una duracion minima (rampSamples).
// ---------------------------------------------------------------------------

struct RenderOptions {
    int contextFrames = 24;              // pad de contexto (frames)
    int gapBridgeFrames = 16;            // puenteo de huecos unvoiced cortos
    int keepOriginalUnvoicedFrames = 24;  // tramo unvoiced "largo" -> original
    int rampSamples = 512;               // duracion minima de la rampa
    bool returnOriginalOnUnvoiced = true;   // devolver el original en los tramos largos
};

// Calcula el rango a renderizar para un tramo "sucio" (editado).
// Devuelve [start, end) en frames de la rejilla de trabajo.
std::pair<int, int> computeSynthesisRange (const RenderRequest& req,
                                          int dirtyStart, int dirtyEnd,
                                          const RenderOptions& opt);

// Renderiza el tramo [dirtyStart, dirtyEnd) con el motor dado y devuelve el
// audio (los samples del rango, ya mezclados con el original donde toca).
std::vector<float> renderRange (const RenderRequest& req,
                                 int dirtyStart, int dirtyEnd,
                                 class IEngine& engine,
                                 const RenderOptions& opt = RenderOptions());

} // namespace pitchcore
