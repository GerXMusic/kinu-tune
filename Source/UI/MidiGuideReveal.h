// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include <algorithm>
#include <cmath>

/**
 * V4c (guia MIDI): calculo puro del boton "Ver guia".
 *
 * No toca el proyecto ni el canvas: solo dice que habria que hacer. El boton
 * aplica el resultado con los setters publicos del piano roll.
 *
 * Reglas (D-378):
 *   * el rango de pitch devuelto SIEMPRE incluye la guia;
 *   * las notas activas se suman solo si no ensanchan el rango mas de una
 *     octava sobre el de la guia ("si caben"); si no, manda la guia;
 *   * si la guia no solapa el tiempo visible, se pide el scroll horizontal
 *     minimo que lleva su INICIO al borde izquierdo, recortado al timeline;
 *   * no se cambia el zoom horizontal: el plan solo expresa scrollX;
 *   * sin guia, o con la guia fuera del timeline, no hay plan.
 *
 * Ejes: todo en segundos de PROYECTO (el eje del canvas). El llamador mapea
 * los fantasmas, que viven en tiempo host, con el offset que muestra el canvas.
 * scrollX va en pixeles, como PianoRollComponent::setScrollX.
 */
struct MidiGuideRevealInput
{
    bool hasGuide = false;
    double guideStartSeconds = 0.0;
    double guideEndSeconds = 0.0;
    float guidePitchMin = 0.0f;
    float guidePitchMax = 0.0f;

    bool hasActiveNotes = false;
    float activePitchMin = 0.0f;
    float activePitchMax = 0.0f;

    double visibleStartSeconds = 0.0;
    double visibleEndSeconds = 0.0;
    double pixelsPerSecond = 1.0;
    double timelineStartSeconds = 0.0;
    double timelineEndSeconds = 0.0;
};

struct MidiGuideRevealPlan
{
    bool hasGuide = false;
    float pitchMin = 0.0f;
    float pitchMax = -1.0f; // invertido = rango vacio
    bool scrollRequested = false;
    double scrollX = 0.0;
};

/** V4c: cuanta mas anchura que la guia se tolera para conservar las activas. */
inline constexpr float kMidiGuideRevealActiveSlackSemitones = 12.0f;

inline MidiGuideRevealPlan
computeMidiGuideRevealPlan(const MidiGuideRevealInput &input)
{
    MidiGuideRevealPlan plan;
    if (!input.hasGuide)
        return plan;
    if (!std::isfinite(input.guideStartSeconds) ||
        !std::isfinite(input.guideEndSeconds) ||
        !std::isfinite(input.guidePitchMin) ||
        !std::isfinite(input.guidePitchMax))
        return plan;
    if (input.guideEndSeconds < input.guideStartSeconds ||
        input.guidePitchMin > input.guidePitchMax)
        return plan;

    // Sin solape con el timeline no hay nada que traer a la vista.
    if (input.guideEndSeconds < input.timelineStartSeconds ||
        input.guideStartSeconds > input.timelineEndSeconds)
        return plan;

    plan.hasGuide = true;
    plan.pitchMin = input.guidePitchMin;
    plan.pitchMax = input.guidePitchMax;

    const float guideSpan = input.guidePitchMax - input.guidePitchMin;
    if (input.hasActiveNotes && std::isfinite(input.activePitchMin) &&
        std::isfinite(input.activePitchMax) &&
        input.activePitchMax >= input.activePitchMin)
    {
        const float unionMin = std::min(plan.pitchMin, input.activePitchMin);
        const float unionMax = std::max(plan.pitchMax, input.activePitchMax);
        // "Activas dentro si caben": se conservan mientras el rango total no
        // crezca mas de una octava sobre el de la guia.
        if (unionMax - unionMin <= guideSpan + kMidiGuideRevealActiveSlackSemitones)
        {
            plan.pitchMin = unionMin;
            plan.pitchMax = unionMax;
        }
    }

    const double windowSeconds =
        input.visibleEndSeconds - input.visibleStartSeconds;
    if (input.pixelsPerSecond > 0.0 && windowSeconds > 0.0)
    {
        const bool guideInTime =
            input.guideEndSeconds >= input.visibleStartSeconds &&
            input.guideStartSeconds <= input.visibleEndSeconds;
        if (!guideInTime)
        {
            const double maxTarget = std::max(
                input.timelineStartSeconds,
                input.timelineEndSeconds - windowSeconds);
            const double target =
                std::clamp(input.guideStartSeconds, input.timelineStartSeconds,
                           maxTarget);
            plan.scrollRequested = true;
            plan.scrollX = target * input.pixelsPerSecond;
        }
    }

    return plan;
}
