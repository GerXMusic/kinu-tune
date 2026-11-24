// D-266 (T6): el estado unico derivado del render.
//
// Criterio escrito antes de medir: la tabla de transiciones pedida por la Fase
// 3. Reposo = Idle; edicion esperando = Dirty(rev); render en vuelo =
// Rendering(rev, rango, generacion); tras el cierre sin nada sucio =
// Published(rev). isQuiescent() es falso con trabajo pendiente o contenido
// sucio y verdadero en Idle/Published.

#include "../Source/UI/Main/RenderState.h"
#include "TestAssert.h"

#include <iostream>
#include <string>

namespace {

const char *phaseName(RenderState::Phase phase)
{
    return RenderState::phaseName(phase);
}

} // namespace

int main()
{
    RenderState state;

    // Reposo inicial.
    CHECK(state.phase() == RenderState::Phase::Idle);
    CHECK(phaseName(state.phase()) == std::string("Idle"));
    CHECK(state.isQuiescent(false, false, false));

    // Una edicion pedida: Dirty(rev).
    state.editRenderScheduled = true;
    state.passRevision = 7;
    CHECK(state.phase() == RenderState::Phase::Dirty);
    CHECK(!state.isQuiescent(true, false, false));

    // Render en vuelo: Rendering(rev, rango, generacion).
    state.editRenderScheduled = false;
    state.editRenderInFlight = true;
    state.passStartFrame = 100;
    state.passEndFrame = 200;
    state.generation = 3;
    CHECK(state.phase() == RenderState::Phase::Rendering);
    CHECK(state.passRevision == 7);
    CHECK(state.passStartFrame == 100 && state.passEndFrame == 200);
    CHECK(!state.isQuiescent(false, false, false));
    CHECK(!state.isQuiescent(false, false, true)); // flush del backend

    // Cierre sin nada sucio: Published.
    state.editRenderInFlight = false;
    CHECK(state.phase() == RenderState::Phase::Published);
    CHECK(state.isQuiescent(false, false, false));
    CHECK(!state.isQuiescent(false, true, false)); // rango F0 sucio
    CHECK(phaseName(state.phase()) == std::string("Published"));

    // La cadena de Play y el render del canal tambien son Rendering.
    state.flushChainActive = true;
    CHECK(state.phase() == RenderState::Phase::Rendering);
    state.flushChainActive = false;
    state.mcpRenderPending = true;
    CHECK(state.phase() == RenderState::Phase::Rendering);
    CHECK(state.renderPending());
    state.mcpRenderPending = false;

    // Cola de bloques pendiente: Dirty hasta que se vacie.
    state.deferredRenderPending = true;
    CHECK(state.phase() == RenderState::Phase::Dirty);
    state.deferredRenderPending = false;
    CHECK(state.phase() == RenderState::Phase::Published);

    std::cout << "RenderStateTests: Idle -> Dirty(7) -> Rendering(7,100-200,3) "
                 "-> Published; quiescent en Idle/Published\n";

    // D-285: vigilante del cierre perdido. Reloj falso.
    {
        RenderState wd;
        wd.editRenderInFlight = true; // la UI cree que hay un pase en vuelo
        wd.noteRenderInFlight(1000.0);
        // Con una peticion viva (mcpRenderPending) nunca se rescata.
        wd.mcpRenderPending = true;
        CHECK(!wd.shouldRescueStall(9000.0, 42));
        wd.mcpRenderPending = false;
        // Antes del tope, tampoco.
        CHECK(!wd.shouldRescueStall(2500.0, 42));
        // Pasado el tope sin peticion viva: rescate.
        CHECK(wd.shouldRescueStall(1000.0 + RenderState::kStallTimeoutMs, 42));
        // Un rescate por revision: la misma revision no se repite.
        wd.lastRescueRevision = 42;
        CHECK(!wd.shouldRescueStall(9000.0, 42));
        // Una revision nueva vuelve a tener presupuesto.
        CHECK(wd.shouldRescueStall(9000.0, 43));
        // Sin render en vuelo no hay nada que rescatar.
        wd.noteRenderClosed();
        CHECK(!wd.shouldRescueStall(9000.0, 44));
        std::cout << "RenderStateTests: vigilante (3 s, una vez por revision) "
                     "en verde\n";
    }
    std::cout << "RenderStateTests: en verde\n";
    return 0;
}
