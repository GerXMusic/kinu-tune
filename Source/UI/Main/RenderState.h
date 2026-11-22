#pragma once

#include <cstdint>

/**
 * D-266 (Fase 3 / T6): el estado del render de la UI, en un solo sitio.
 *
 * D-261 dejo cinco flags sueltos (`deferredRenderPending`, `flushChainActive`,
 * `mcpRenderPending`, `editRenderScheduled`, `editRenderInFlight`) porque cada
 * uno cubre un borde distinto y unificarlos cambiaba el orden de tres caminos.
 * Este tipo conserva **exactamente** esos cinco significados y esa vida, pero
 * los agrupa y deriva de ellos un unico estado (`phase()`), que es lo que
 * consultan la UI, el canal de control y los tests.
 *
 * Prioridad de la derivacion:
 *   Rendering  gana a todo: hay un render en vuelo (edicion, MCP o Play).
 *   Dirty      hay una edicion esperando el debounce o bloques en la cola.
 *   Published  ya se publico al menos un render y no queda nada pendiente.
 *   Idle       no se ha renderizado nada todavia.
 *
 * `isQuiescent()` es el invariante de reposo de D-261: sin notas sucias, sin
 * rango F0 sucio, sin render pendiente y sin flush del backend. Las dos
 * primeras condiciones viven en `Project`, asi que las pasa el dueno.
 */
struct RenderState
{
    enum class Phase
    {
        Idle,
        Dirty,
        Rendering,
        Published
    };

    // D-249: hay una edicion esperando el debounce de 120 ms.
    bool editRenderScheduled = false;
    // D-249/D-254: el render lanzo la politica A (no Play ni MCP).
    bool editRenderInFlight = false;
    // Render pedido por el canal de control.
    bool mcpRenderPending = false;
    // P1b: cadena de Play/carga que renderiza bloque a bloque.
    bool flushChainActive = false;
    // P1b: la cola DeferredRenders tiene bloques pendientes.
    bool deferredRenderPending = false;

    // D-254/D-266: que cubre el render en vuelo y cuando se publico.
    std::uint64_t passRevision = 0;
    int passStartFrame = 0;
    int passEndFrame = 0;
    std::uint32_t generation = 0;

    bool renderPending() const
    {
        return editRenderInFlight || mcpRenderPending || flushChainActive;
    }

    bool hasDirtyWork() const
    {
        return editRenderScheduled || deferredRenderPending;
    }

    Phase phase() const
    {
        if (renderPending())
            return Phase::Rendering;
        if (hasDirtyWork())
            return Phase::Dirty;
        if (generation > 0)
            return Phase::Published;
        return Phase::Idle;
    }

    static const char* phaseName(Phase phase)
    {
        switch (phase)
        {
        case Phase::Dirty:     return "Dirty";
        case Phase::Rendering: return "Rendering";
        case Phase::Published: return "Published";
        case Phase::Idle:
        default:               return "Idle";
        }
    }

    // D-261: invariante de reposo. `backendFlushActive` es del backend
    // (EditorController) y no de este tipo, por eso entra como argumento.
    bool isQuiescent(bool hasDirtyNotes, bool hasF0DirtyRange,
                     bool backendFlushActive) const
    {
        return !hasDirtyNotes && !hasF0DirtyRange && !renderPending() &&
               !backendFlushActive;
    }
};
