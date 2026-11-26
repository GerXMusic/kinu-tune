// R4 (D-317): con mapa de tempo real, el BPM del cursor no se guarda.
//
// Antes, cada vez que el host publicaba su tempo (o se reabria el proyecto) se
// escribia el BPM del cursor en Project::timelineTempoBpm. Con un mapa real ese
// numero es solo el punto donde estaba el cursor: al reabrir, el proyecto
// mostraba como tempo propio el de la ultima posicion. Ahora solo se guarda
// cuando NO hay mapa usable (es el fallback constante).
#include "TestAssert.h"

#include "../Source/Audio/Engine/HostTempoMap.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/UI/HostTimelineStatePolicy.h"

#include <cmath>
#include <memory>

namespace
{
bool near(double a, double b, double tolerance = 1.0e-9)
{
    return std::abs(a - b) <= tolerance;
}

void testCursorWithMapDoesNotChangeStoredTempo()
{
    Project project;
    project.setTimelineTempoBpm(100.0);
    project.setTimelineBeatSignature(3, 4);

    const auto map =
        std::make_shared<HostTempoMap>(HostTempoMap::constant(150.0, 4, 4));
    CHECK(map->isValid());
    CHECK(!shouldPersistHostCursorTempo(map));

    CHECK(!applyHostTimelineStateToProject(project, map, 150.0, 6, 8));
    CHECK(near(project.getTimelineTempoBpm(), 100.0));
    CHECK(project.getTimelineBeatNumerator() == 3);
    CHECK(project.getTimelineBeatDenominator() == 4);

    // Mover el cursor por el mapa tampoco cambia lo guardado.
    CHECK(!applyHostTimelineStateToProject(project, map, 75.0, 4, 4));
    CHECK(near(project.getTimelineTempoBpm(), 100.0));
    CHECK(project.getTimelineBeatNumerator() == 3);
}

void testCursorWithoutMapIsStored()
{
    Project project;
    project.setTimelineTempoBpm(100.0);

    // Un mapa vacio no es usable: el cursor es el fallback y se guarda.
    const auto empty = std::make_shared<HostTempoMap>();
    CHECK(!empty->isValid());
    CHECK(shouldPersistHostCursorTempo(empty));
    CHECK(applyHostTimelineStateToProject(project, empty, 75.0, 6, 8));
    CHECK(near(project.getTimelineTempoBpm(), 75.0));
    CHECK(project.getTimelineBeatNumerator() == 6);
    CHECK(project.getTimelineBeatDenominator() == 8);

    // Sin mapa ninguno, igual que siempre.
    CHECK(applyHostTimelineStateToProject(project, nullptr, 90.0, 4, 4));
    CHECK(near(project.getTimelineTempoBpm(), 90.0));
    CHECK(project.getTimelineBeatNumerator() == 4);
}

// Un proyecto viejo sin el campo carga con el fallback documentado (120) y no
// se rompe: FORMAT_VERSION no sube por esto.
void testOldProjectWithoutTempoFieldLoads()
{
    Project project;
    project.setTimelineTempoBpm(123.0);

    auto json = ProjectSerializer::toJson(project);
    auto* object = json.getDynamicObject();
    CHECK(object != nullptr);
    object->removeProperty("timelineTempoBpm");

    Project reopened;
    CHECK(ProjectSerializer::fromJson(reopened, json));
    CHECK(near(reopened.getTimelineTempoBpm(), 120.0));
}

void testTempoSurvivesSaveAndLoad()
{
    Project project;
    project.setTimelineTempoBpm(123.0);

    Project reopened;
    CHECK(ProjectSerializer::fromJson(reopened,
                                      ProjectSerializer::toJson(project)));
    CHECK(near(reopened.getTimelineTempoBpm(), 123.0));

    // Con mapa real el cursor no toca lo guardado, y lo que se reabre sigue
    // siendo el fallback del proyecto.
    const auto map =
        std::make_shared<HostTempoMap>(HostTempoMap::constant(150.0));
    CHECK(!applyHostTimelineStateToProject(project, map, 60.0, 4, 4));

    Project third;
    CHECK(ProjectSerializer::fromJson(third, ProjectSerializer::toJson(project)));
    CHECK(near(third.getTimelineTempoBpm(), 123.0));
}
} // namespace

int main()
{
    testCursorWithMapDoesNotChangeStoredTempo();
    testCursorWithoutMapIsStored();
    testOldProjectWithoutTempoFieldLoads();
    testTempoSurvivesSaveAndLoad();
    return 0;
}
